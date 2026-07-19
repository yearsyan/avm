// SPDX-License-Identifier: MIT

#import <Foundation/Foundation.h>

#include "image_importer.h"

#include <CommonCrypto/CommonDigest.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <signal.h>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr char kManifestFormat[] = "macmu-system-image";
constexpr uint64_t kManifestVersion = 2;
constexpr size_t kDownloadWorkers = 8;

std::atomic<bool> gImportCancelled{false};
std::mutex gTaskPidsMutex;
std::set<pid_t> gTaskPids;

struct TaskResult {
  bool launched = false;
  int status = -1;
  std::string standardError;
};

struct ObjectSpec {
  std::string relativePath;
  uint64_t size = 0;
  std::string sha256;
  std::string source;
  std::string localPath;
};

struct ChunkSpec {
  uint64_t offset = 0;
  uint64_t size = 0;
  std::string sha256;
  size_t objectIndex = 0;
  std::string entry;
};

struct FileSpec {
  std::string relativePath;
  uint64_t size = 0;
  std::string sha256;
  std::vector<ChunkSpec> chunks;
};

struct ChunkManifest {
  std::string root;
  std::string baseUrl;
  size_t baseObjectIndex = 0;
  std::vector<ObjectSpec> objects;
  std::vector<FileSpec> files;
};

std::string ns_string_value(NSString *value) {
  return value ? std::string(value.UTF8String ?: "") : std::string();
}

NSString *ns_string(const std::string &value) {
  return [NSString stringWithUTF8String:value.c_str()];
}

std::string trim_task_error(NSData *data) {
  if (!data || data.length == 0) {
    return {};
  }
  NSString *text = [[NSString alloc] initWithData:data
                                         encoding:NSUTF8StringEncoding];
  if (!text) {
    return {};
  }
  return ns_string_value([text
      stringByTrimmingCharactersInSet:[NSCharacterSet
                                          whitespaceAndNewlineCharacterSet]]);
}

TaskResult run_task(const std::string &executable,
                    const std::vector<std::string> &arguments,
                    int standard_output_fd = -1) {
  @autoreleasepool {
    TaskResult result;
    if (gImportCancelled.load(std::memory_order_acquire)) {
      result.standardError = "image import cancelled";
      return result;
    }
    NSTask *task = [[NSTask alloc] init];
    task.executableURL = [NSURL fileURLWithPath:ns_string(executable)];
    NSMutableArray<NSString *> *taskArguments =
        [NSMutableArray arrayWithCapacity:arguments.size()];
    for (const std::string &argument : arguments) {
      [taskArguments addObject:ns_string(argument)];
    }
    task.arguments = taskArguments;

    NSPipe *errorPipe = [NSPipe pipe];
    task.standardError = errorPipe;
    NSFileHandle *outputHandle = nil;
    if (standard_output_fd >= 0) {
      const int duplicate = dup(standard_output_fd);
      if (duplicate < 0) {
        result.standardError = "failed to duplicate output descriptor: " +
                               std::string(std::strerror(errno));
        return result;
      }
      outputHandle = [[NSFileHandle alloc] initWithFileDescriptor:duplicate
                                                   closeOnDealloc:YES];
      task.standardOutput = outputHandle;
    }

    NSError *launchError = nil;
    if (![task launchAndReturnError:&launchError]) {
      result.standardError = "failed to launch " + executable + ": " +
                             ns_string_value(launchError.localizedDescription);
      return result;
    }
    result.launched = true;
    const pid_t taskPid = task.processIdentifier;
    {
      std::lock_guard<std::mutex> lock(gTaskPidsMutex);
      if (gImportCancelled.load(std::memory_order_acquire)) {
        kill(taskPid, SIGTERM);
      } else {
        gTaskPids.insert(taskPid);
      }
    }
    [task waitUntilExit];
    {
      std::lock_guard<std::mutex> lock(gTaskPidsMutex);
      gTaskPids.erase(taskPid);
    }
    result.status = task.terminationStatus;
    result.standardError =
        trim_task_error([errorPipe.fileHandleForReading readDataToEndOfFile]);
    return result;
  }
}

bool run_checked_task(const std::string &executable,
                      const std::vector<std::string> &arguments,
                      int standard_output_fd, std::string *error) {
  const TaskResult result = run_task(executable, arguments, standard_output_fd);
  if (result.launched && result.status == 0) {
    return true;
  }
  if (error) {
    *error = result.standardError.empty()
                 ? executable + " failed with status " +
                       std::to_string(result.status)
                 : result.standardError;
  }
  return false;
}

bool starts_with(const std::string &value, const char *prefix) {
  const size_t length = std::strlen(prefix);
  return value.size() >= length && value.compare(0, length, prefix) == 0;
}

bool ends_with_case_insensitive(const std::string &value, const char *suffix) {
  const size_t suffixLength = std::strlen(suffix);
  if (value.size() < suffixLength) {
    return false;
  }
  return std::equal(value.end() - static_cast<std::ptrdiff_t>(suffixLength),
                    value.end(), suffix, suffix + suffixLength,
                    [](char left, char right) {
                      return std::tolower(static_cast<unsigned char>(left)) ==
                             std::tolower(static_cast<unsigned char>(right));
                    });
}

bool is_sha256(const std::string &value) {
  return value.size() == CC_SHA256_DIGEST_LENGTH * 2 &&
         std::all_of(value.begin(), value.end(), [](unsigned char byte) {
           return std::isxdigit(byte) != 0;
         });
}

bool safe_relative_path(const std::string &value,
                        bool singleComponent = false) {
  if (value.empty()) {
    return false;
  }
  const fs::path path(value);
  if (path.is_absolute() || (singleComponent && path.has_parent_path())) {
    return false;
  }
  for (const fs::path &component : path) {
    if (component.empty() || component == "." || component == "..") {
      return false;
    }
  }
  return true;
}

bool file_size_matches(const std::string &path, uint64_t expected) {
  std::error_code ec;
  return fs::is_regular_file(path, ec) && fs::file_size(path, ec) == expected;
}

bool sha256_file(const std::string &path, std::string *digest,
                 std::string *error) {
  std::ifstream input(path, std::ios::in | std::ios::binary);
  if (!input) {
    if (error) {
      *error = "failed to open for SHA-256: " + path;
    }
    return false;
  }

  CC_SHA256_CTX context;
  CC_SHA256_Init(&context);
  std::vector<char> buffer(1024 * 1024);
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize count = input.gcount();
    if (count > 0) {
      CC_SHA256_Update(&context, buffer.data(), static_cast<CC_LONG>(count));
    }
  }
  if (!input.eof()) {
    if (error) {
      *error = "failed while hashing: " + path;
    }
    return false;
  }

  unsigned char bytes[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256_Final(bytes, &context);
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (unsigned char byte : bytes) {
    output << std::setw(2) << static_cast<unsigned>(byte);
  }
  if (digest) {
    *digest = output.str();
  }
  return true;
}

bool verify_file(const std::string &path, uint64_t expectedSize,
                 const std::string &expectedSha256, std::string *error) {
  if (!file_size_matches(path, expectedSize)) {
    if (error) {
      *error = "size mismatch for " + path;
    }
    return false;
  }
  std::string actualSha256;
  if (!sha256_file(path, &actualSha256, error)) {
    return false;
  }
  if (actualSha256 != expectedSha256) {
    if (error) {
      *error = "SHA-256 mismatch for " + path;
    }
    return false;
  }
  return true;
}

bool json_string(NSDictionary *dictionary, NSString *key, std::string *value,
                 std::string *error, bool required = true) {
  id object = dictionary[key];
  if (!object && !required) {
    value->clear();
    return true;
  }
  if (![object isKindOfClass:[NSString class]]) {
    if (error) {
      *error = "manifest field is not a string: " + ns_string_value(key);
    }
    return false;
  }
  *value = ns_string_value((NSString *)object);
  return true;
}

bool json_u64(NSDictionary *dictionary, NSString *key, uint64_t *value,
              std::string *error) {
  id object = dictionary[key];
  if (![object isKindOfClass:[NSNumber class]]) {
    if (error) {
      *error = "manifest field is not an integer: " + ns_string_value(key);
    }
    return false;
  }
  NSNumber *number = (NSNumber *)object;
  const long long signedValue = number.longLongValue;
  if (signedValue < 0 ||
      number.doubleValue != static_cast<double>(signedValue)) {
    if (error) {
      *error = "manifest field is not a non-negative integer: " +
               ns_string_value(key);
    }
    return false;
  }
  *value = static_cast<uint64_t>(signedValue);
  return true;
}

bool add_object(NSDictionary *dictionary, ChunkManifest *manifest,
                std::map<std::string, size_t> *objectIndexes,
                size_t *objectIndex, std::string *error) {
  ObjectSpec object;
  if (!json_string(dictionary, @"object", &object.relativePath, error) ||
      !json_u64(dictionary, @"object_size", &object.size, error) ||
      !json_string(dictionary, @"object_sha256", &object.sha256, error)) {
    return false;
  }
  if (!safe_relative_path(object.relativePath) || object.size == 0 ||
      !is_sha256(object.sha256)) {
    if (error) {
      *error = "manifest contains an invalid object descriptor";
    }
    return false;
  }

  auto existing = objectIndexes->find(object.sha256);
  if (existing != objectIndexes->end()) {
    const ObjectSpec &previous = manifest->objects[existing->second];
    if (previous.relativePath != object.relativePath ||
        previous.size != object.size) {
      if (error) {
        *error = "manifest reuses an object hash with conflicting metadata";
      }
      return false;
    }
    *objectIndex = existing->second;
    return true;
  }

  *objectIndex = manifest->objects.size();
  (*objectIndexes)[object.sha256] = *objectIndex;
  manifest->objects.push_back(std::move(object));
  return true;
}

bool parse_manifest(const std::string &path, ChunkManifest *manifest,
                    std::string *error) {
  @autoreleasepool {
    NSData *data = [NSData dataWithContentsOfFile:ns_string(path)];
    if (!data) {
      if (error) {
        *error = "failed to read image manifest: " + path;
      }
      return false;
    }
    NSError *jsonError = nil;
    id root = [NSJSONSerialization JSONObjectWithData:data
                                              options:0
                                                error:&jsonError];
    if (![root isKindOfClass:[NSDictionary class]]) {
      if (error) {
        *error = jsonError ? ns_string_value(jsonError.localizedDescription)
                           : "image manifest root is not an object";
      }
      return false;
    }
    NSDictionary *dictionary = (NSDictionary *)root;

    std::string format;
    std::string product;
    std::string device;
    std::string variant;
    uint64_t version = 0;
    uint64_t api = 0;
    uint64_t chunkSize = 0;
    if (!json_string(dictionary, @"format", &format, error) ||
        !json_u64(dictionary, @"version", &version, error) ||
        !json_string(dictionary, @"product", &product, error) ||
        !json_string(dictionary, @"device", &device, error) ||
        !json_string(dictionary, @"variant", &variant, error) ||
        !json_u64(dictionary, @"api", &api, error) ||
        !json_u64(dictionary, @"chunk_size", &chunkSize, error) ||
        !json_string(dictionary, @"root", &manifest->root, error) ||
        !json_string(dictionary, @"base_url", &manifest->baseUrl, error,
                     false)) {
      return false;
    }
    if (format != kManifestFormat || version != kManifestVersion ||
        product != "macmu_sdk_phone64_arm64" || device != "emu64a" ||
        variant != "user" || api != 36 || chunkSize == 0) {
      if (error) {
        *error =
            "unsupported MacMu image manifest identity, format, or version";
      }
      return false;
    }
    if (!safe_relative_path(manifest->root, true)) {
      if (error) {
        *error = "manifest root must be one safe directory name";
      }
      return false;
    }

    id baseValue = dictionary[@"base"];
    id filesValue = dictionary[@"files"];
    if (![baseValue isKindOfClass:[NSDictionary class]] ||
        ![filesValue isKindOfClass:[NSArray class]]) {
      if (error) {
        *error = "manifest base or files field has the wrong type";
      }
      return false;
    }

    std::map<std::string, size_t> objectIndexes;
    if (!add_object((NSDictionary *)baseValue, manifest, &objectIndexes,
                    &manifest->baseObjectIndex, error)) {
      return false;
    }

    std::set<std::string> filePaths;
    for (id fileValue in (NSArray *)filesValue) {
      if (![fileValue isKindOfClass:[NSDictionary class]]) {
        if (error) {
          *error = "manifest file record is not an object";
        }
        return false;
      }
      NSDictionary *fileDictionary = (NSDictionary *)fileValue;
      FileSpec file;
      if (!json_string(fileDictionary, @"path", &file.relativePath, error) ||
          !json_u64(fileDictionary, @"size", &file.size, error) ||
          !json_string(fileDictionary, @"sha256", &file.sha256, error)) {
        return false;
      }
      if (!safe_relative_path(file.relativePath) || file.size == 0 ||
          !is_sha256(file.sha256) ||
          !filePaths.insert(file.relativePath).second) {
        if (error) {
          *error = "manifest contains an invalid or duplicate file record";
        }
        return false;
      }
      id chunksValue = fileDictionary[@"chunks"];
      if (![chunksValue isKindOfClass:[NSArray class]] ||
          ((NSArray *)chunksValue).count == 0) {
        if (error) {
          *error = "manifest file has no chunks: " + file.relativePath;
        }
        return false;
      }

      uint64_t expectedOffset = 0;
      for (id chunkValue in (NSArray *)chunksValue) {
        if (![chunkValue isKindOfClass:[NSDictionary class]]) {
          if (error) {
            *error = "manifest chunk record is not an object";
          }
          return false;
        }
        NSDictionary *chunkDictionary = (NSDictionary *)chunkValue;
        ChunkSpec chunk;
        if (!json_u64(chunkDictionary, @"offset", &chunk.offset, error) ||
            !json_u64(chunkDictionary, @"size", &chunk.size, error) ||
            !json_string(chunkDictionary, @"sha256", &chunk.sha256, error) ||
            !json_string(chunkDictionary, @"entry", &chunk.entry, error) ||
            !add_object(chunkDictionary, manifest, &objectIndexes,
                        &chunk.objectIndex, error)) {
          return false;
        }
        if (chunk.offset != expectedOffset || chunk.size == 0 ||
            chunk.size > chunkSize || chunk.size > file.size - expectedOffset ||
            !is_sha256(chunk.sha256) || chunk.entry != "payload") {
          if (error) {
            *error = "manifest contains invalid chunk coverage for " +
                     file.relativePath;
          }
          return false;
        }
        expectedOffset += chunk.size;
        file.chunks.push_back(std::move(chunk));
      }
      if (expectedOffset != file.size) {
        if (error) {
          *error = "manifest chunks do not cover the complete file: " +
                   file.relativePath;
        }
        return false;
      }
      manifest->files.push_back(std::move(file));
    }
    if (manifest->files.empty()) {
      if (error) {
        *error = "manifest has no chunked files";
      }
      return false;
    }
    return true;
  }
}

bool path_from_file_url(const std::string &source, std::string *path,
                        std::string *error) {
  @autoreleasepool {
    NSURL *url = [NSURL URLWithString:ns_string(source)];
    if (!url || !url.isFileURL || url.path.length == 0) {
      if (error) {
        *error = "invalid file URL: " + source;
      }
      return false;
    }
    *path = ns_string_value(url.path);
    return true;
  }
}

bool download_url(const std::string &url, const std::string &destination,
                  bool resume, TaskResult *result) {
  std::vector<std::string> arguments = {
      "--fail",
      "--location",
      "--silent",
      "--show-error",
      "--retry",
      "20",
      "--retry-all-errors",
      "--retry-delay",
      "2",
      "--retry-max-time",
      "180",
      "--retry-connrefused",
      "--connect-timeout",
      "20",
      "--proto",
      "=https",
  };
  if (resume) {
    arguments.insert(arguments.end(), {"--continue-at", "-"});
  }
  arguments.insert(arguments.end(), {"--output", destination, url});
  *result = run_task("/usr/bin/curl", arguments);
  return result->launched && result->status == 0;
}

bool resolve_object_sources(const std::string &manifestSource,
                            ChunkManifest *manifest, std::string *error) {
  @autoreleasepool {
    const bool remoteManifest = starts_with(manifestSource, "https://");
    NSURL *manifestUrl = nil;
    if (remoteManifest || starts_with(manifestSource, "file://")) {
      manifestUrl = [NSURL URLWithString:ns_string(manifestSource)];
    } else {
      std::error_code pathError;
      const fs::path absoluteManifest = fs::absolute(manifestSource, pathError);
      if (pathError) {
        if (error) {
          *error = "failed to resolve manifest path: " + pathError.message();
        }
        return false;
      }
      manifestUrl =
          [NSURL fileURLWithPath:ns_string(absoluteManifest.string())];
    }
    if (!manifestUrl) {
      if (error) {
        *error = "invalid manifest source: " + manifestSource;
      }
      return false;
    }

    NSURL *directoryUrl = [manifestUrl URLByDeletingLastPathComponent];
    NSURL *baseUrl = directoryUrl;
    if (!manifest->baseUrl.empty()) {
      baseUrl = [NSURL URLWithString:ns_string(manifest->baseUrl)
                       relativeToURL:directoryUrl];
    }
    baseUrl = baseUrl.absoluteURL;
    if (!baseUrl) {
      if (error) {
        *error = "manifest contains an invalid base_url";
      }
      return false;
    }

    for (ObjectSpec &object : manifest->objects) {
      NSURL *objectUrl = [NSURL URLWithString:ns_string(object.relativePath)
                                relativeToURL:baseUrl];
      objectUrl = objectUrl.absoluteURL;
      if (!objectUrl) {
        if (error) {
          *error = "failed to resolve image object: " + object.relativePath;
        }
        return false;
      }
      if (objectUrl.isFileURL) {
        if (remoteManifest) {
          if (error) {
            *error = "an HTTPS manifest cannot reference local image objects";
          }
          return false;
        }
        object.source = ns_string_value(objectUrl.path);
      } else if ([objectUrl.scheme.lowercaseString isEqualToString:@"https"]) {
        object.source = ns_string_value(objectUrl.absoluteString);
      } else {
        if (error) {
          *error = "image objects must use local files or HTTPS";
        }
        return false;
      }
    }
    return true;
  }
}

bool materialize_object(const ShellOptions &options, const ObjectSpec &object,
                        std::string *localPath, std::string *error) {
  if (!starts_with(object.source, "https://")) {
    if (!verify_file(object.source, object.size, object.sha256, error)) {
      return false;
    }
    *localPath = object.source;
    return true;
  }

  const fs::path cacheDir =
      fs::path(options.appDataDir) / "cache" / "image-objects";
  std::error_code ec;
  fs::create_directories(cacheDir, ec);
  if (ec) {
    if (error) {
      *error = "failed to create image object cache: " + ec.message();
    }
    return false;
  }

  const fs::path cached = cacheDir / (object.sha256 + ".zip");
  const fs::path partial = cacheDir / (object.sha256 + ".zip.partial");
  if (fs::is_regular_file(cached, ec)) {
    std::string validationError;
    if (verify_file(cached.string(), object.size, object.sha256,
                    &validationError)) {
      *localPath = cached.string();
      return true;
    }
    fs::remove(cached, ec);
  }

  bool resume =
      fs::is_regular_file(partial, ec) && fs::file_size(partial, ec) > 0;
  TaskResult result;
  if (!download_url(object.source, partial.string(), resume, &result)) {
    if (resume && result.launched && result.status == 33) {
      fs::remove(partial, ec);
      resume = false;
      if (!download_url(object.source, partial.string(), false, &result)) {
        if (error) {
          *error = result.standardError.empty()
                       ? "failed to download image object"
                       : result.standardError;
        }
        return false;
      }
    } else {
      if (error) {
        *error = result.standardError.empty()
                     ? "failed to download image object"
                     : result.standardError;
      }
      return false;
    }
  }

  std::string validationError;
  if (!verify_file(partial.string(), object.size, object.sha256,
                   &validationError)) {
    fs::remove(partial, ec);
    if (error) {
      *error = validationError;
    }
    return false;
  }
  fs::rename(partial, cached, ec);
  if (ec) {
    if (fs::is_regular_file(cached, ec) &&
        verify_file(cached.string(), object.size, object.sha256,
                    &validationError)) {
      fs::remove(partial, ec);
    } else {
      if (error) {
        *error = "failed to commit image object to cache: " + ec.message();
      }
      return false;
    }
  }
  *localPath = cached.string();
  return true;
}

uint64_t regular_file_size_at_most(const fs::path &path, uint64_t maximum) {
  std::error_code ec;
  if (!fs::is_regular_file(path, ec)) {
    return 0;
  }
  const uintmax_t size = fs::file_size(path, ec);
  return ec ? 0 : std::min<uint64_t>(size, maximum);
}

uint64_t
acquired_object_bytes(const ShellOptions &options,
                      const ChunkManifest &manifest,
                      const std::vector<std::atomic_bool> &objectComplete) {
  const fs::path cacheDir =
      fs::path(options.appDataDir) / "cache" / "image-objects";
  uint64_t acquiredBytes = 0;
  for (size_t index = 0; index < manifest.objects.size(); ++index) {
    const ObjectSpec &object = manifest.objects[index];
    if (objectComplete[index].load(std::memory_order_acquire)) {
      acquiredBytes += object.size;
      continue;
    }
    if (!starts_with(object.source, "https://")) {
      continue;
    }

    const fs::path cached = cacheDir / (object.sha256 + ".zip");
    const fs::path partial = cacheDir / (object.sha256 + ".zip.partial");
    const uint64_t availableBytes =
        std::max(regular_file_size_at_most(cached, object.size),
                 regular_file_size_at_most(partial, object.size));
    acquiredBytes += availableBytes;
  }
  return acquiredBytes;
}

bool materialize_objects(const ShellOptions &options, ChunkManifest *manifest,
                         const ImageImportProgressCallback &progress,
                         std::string *error) {
  const size_t total = manifest->objects.size();
  uint64_t totalBytes = 0;
  bool network = false;
  for (const ObjectSpec &object : manifest->objects) {
    totalBytes += object.size;
    network = network || starts_with(object.source, "https://");
  }

  std::atomic<size_t> next{0};
  std::atomic<size_t> completed{0};
  std::atomic<bool> failed{false};
  std::atomic<bool> progressStopped{false};
  std::vector<std::atomic_bool> objectComplete(total);
  for (std::atomic_bool &complete : objectComplete) {
    complete.store(false, std::memory_order_relaxed);
  }
  std::mutex errorMutex;
  std::string firstError;

  std::thread progressThread;
  if (progress) {
    progressThread = std::thread([&] {
      uint64_t monotonicBytes = 0;
      uint64_t lastEmittedBytes = std::numeric_limits<uint64_t>::max();
      size_t lastEmittedItems = std::numeric_limits<size_t>::max();
      while (!progressStopped.load(std::memory_order_acquire)) {
        const uint64_t currentBytes =
            acquired_object_bytes(options, *manifest, objectComplete);
        monotonicBytes = std::max(monotonicBytes, currentBytes);
        const size_t currentItems = completed.load(std::memory_order_acquire);
        if (monotonicBytes != lastEmittedBytes ||
            currentItems != lastEmittedItems) {
          lastEmittedBytes = monotonicBytes;
          lastEmittedItems = currentItems;
          @autoreleasepool {
            progress({ImageImportPhase::kAcquiringObjects, monotonicBytes,
                      totalBytes, currentItems, total, network});
          }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
      }
    });
  }

  const size_t workerCount = std::min(kDownloadWorkers, total);
  std::vector<std::thread> workers;
  workers.reserve(workerCount);
  for (size_t worker = 0; worker < workerCount; ++worker) {
    workers.emplace_back([&] {
      @autoreleasepool {
        while (!failed.load(std::memory_order_acquire)) {
          const size_t index = next.fetch_add(1, std::memory_order_relaxed);
          if (index >= total) {
            break;
          }
          std::string localPath;
          std::string objectError;
          if (!materialize_object(options, manifest->objects[index], &localPath,
                                  &objectError)) {
            {
              std::lock_guard<std::mutex> lock(errorMutex);
              if (firstError.empty()) {
                firstError = objectError;
              }
            }
            failed.store(true, std::memory_order_release);
            break;
          }
          manifest->objects[index].localPath = std::move(localPath);
          objectComplete[index].store(true, std::memory_order_release);
          completed.fetch_add(1, std::memory_order_acq_rel);
        }
      }
    });
  }
  for (std::thread &worker : workers) {
    worker.join();
  }
  progressStopped.store(true, std::memory_order_release);
  if (progressThread.joinable()) {
    progressThread.join();
  }
  if (failed.load(std::memory_order_acquire)) {
    if (error) {
      *error =
          firstError.empty() ? "failed to acquire image objects" : firstError;
    }
    return false;
  }
  if (progress) {
    progress({ImageImportPhase::kAcquiringObjects, totalBytes, totalBytes,
              total, total, network});
  }
  return true;
}

bool extract_base_object(const ChunkManifest &manifest,
                         const std::string &destinationRoot,
                         std::string *error) {
  const ObjectSpec &base = manifest.objects[manifest.baseObjectIndex];
  if (!run_checked_task("/usr/bin/ditto",
                        {"-x", "-k", base.localPath, destinationRoot}, -1,
                        error)) {
    if (error && error->empty()) {
      *error = "failed to extract base image object";
    }
    return false;
  }
  const fs::path imageRoot = fs::path(destinationRoot) / manifest.root;
  std::error_code ec;
  if (!fs::is_directory(imageRoot, ec) || fs::is_symlink(imageRoot, ec)) {
    if (error) {
      *error = "base image object did not create the declared root directory";
    }
    return false;
  }
  return true;
}

bool reconstruct_file(const ChunkManifest &manifest, const FileSpec &file,
                      const std::string &destinationRoot,
                      const ImageImportProgressCallback &progress,
                      uint64_t totalBytes, size_t totalChunks,
                      uint64_t *completedBytes, size_t *completedChunks,
                      std::string *error) {
  const fs::path imageRoot = fs::path(destinationRoot) / manifest.root;
  const fs::path destination = imageRoot / file.relativePath;
  const fs::path partial = destination.string() + ".partial";
  std::error_code ec;
  fs::create_directories(destination.parent_path(), ec);
  if (ec) {
    if (error) {
      *error = "failed to create image directory: " + ec.message();
    }
    return false;
  }
  fs::remove(partial, ec);

  const int outputFd =
      open(partial.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (outputFd < 0) {
    if (error) {
      *error = "failed to create reconstructed image file: " +
               std::string(std::strerror(errno));
    }
    return false;
  }

  bool ok = true;
  for (const ChunkSpec &chunk : file.chunks) {
    const ObjectSpec &object = manifest.objects[chunk.objectIndex];
    if (!run_checked_task("/usr/bin/unzip",
                          {"-p", object.localPath, chunk.entry}, outputFd,
                          error)) {
      ok = false;
      break;
    }
    const off_t currentOffset = lseek(outputFd, 0, SEEK_CUR);
    const uint64_t expectedOffset = chunk.offset + chunk.size;
    if (currentOffset < 0 ||
        static_cast<uint64_t>(currentOffset) != expectedOffset) {
      if (error) {
        *error =
            "chunk size mismatch while reconstructing " + file.relativePath;
      }
      ok = false;
      break;
    }
    *completedBytes += chunk.size;
    ++*completedChunks;
    if (progress) {
      progress({ImageImportPhase::kReconstructingImage, *completedBytes,
                totalBytes, *completedChunks, totalChunks, false});
    }
  }
  if (close(outputFd) != 0 && ok) {
    if (error) {
      *error = "failed to close reconstructed image file: " +
               std::string(std::strerror(errno));
    }
    ok = false;
  }
  if (!ok) {
    fs::remove(partial, ec);
    return false;
  }

  std::string validationError;
  if (!verify_file(partial.string(), file.size, file.sha256,
                   &validationError)) {
    fs::remove(partial, ec);
    if (error) {
      *error = validationError;
    }
    return false;
  }
  fs::permissions(partial,
                  fs::perms::owner_read | fs::perms::owner_write |
                      fs::perms::group_read | fs::perms::others_read,
                  fs::perm_options::replace, ec);
  if (ec) {
    fs::remove(partial, ec);
    if (error) {
      *error = "failed to set image file permissions: " + ec.message();
    }
    return false;
  }
  fs::remove(destination, ec);
  ec.clear();
  fs::rename(partial, destination, ec);
  if (ec) {
    fs::remove(partial, ec);
    if (error) {
      *error = "failed to finish reconstructed image file: " + ec.message();
    }
    return false;
  }
  return true;
}

bool extract_chunk_manifest(const ShellOptions &options,
                            const std::string &manifestPath,
                            const std::string &manifestSource,
                            const std::string &destinationRoot,
                            const ImageImportProgressCallback &progress,
                            std::string *error) {
  ChunkManifest manifest;
  if (!parse_manifest(manifestPath, &manifest, error) ||
      !resolve_object_sources(manifestSource, &manifest, error) ||
      !materialize_objects(options, &manifest, progress, error)) {
    return false;
  }

  uint64_t totalBytes = 0;
  size_t totalChunks = 0;
  for (const FileSpec &file : manifest.files) {
    totalBytes += file.size;
    totalChunks += file.chunks.size();
  }
  uint64_t completedBytes = 0;
  size_t completedChunks = 0;
  if (progress) {
    progress({ImageImportPhase::kReconstructingImage, 0, totalBytes, 0,
              totalChunks, false});
  }
  if (!extract_base_object(manifest, destinationRoot, error)) {
    return false;
  }
  for (const FileSpec &file : manifest.files) {
    if (!reconstruct_file(manifest, file, destinationRoot, progress, totalBytes,
                          totalChunks, &completedBytes, &completedChunks,
                          error)) {
      return false;
    }
  }
  return true;
}

bool download_remote_manifest(const std::string &source,
                              const std::string &destination,
                              std::string *error) {
  TaskResult result;
  if (download_url(source, destination, false, &result)) {
    return true;
  }
  if (error) {
    *error = result.standardError.empty() ? "failed to download image manifest"
                                          : result.standardError;
  }
  return false;
}

} // namespace

bool macmu_extract_system_image_source(
    const ShellOptions &options, const std::string &source,
    const std::string &destination_root,
    const ImageImportProgressCallback &progress, std::string *error) {
  gImportCancelled.store(false, std::memory_order_release);
  if (source.empty()) {
    if (error) {
      *error = "empty image source";
    }
    return false;
  }

  std::error_code ec;
  fs::create_directories(destination_root, ec);
  if (ec) {
    if (error) {
      *error = "failed to create image extraction directory: " + ec.message();
    }
    return false;
  }

  if (starts_with(source, "https://")) {
    if (ends_with_case_insensitive(source, ".zip")) {
      if (error) {
        *error = "remote complete ZIP imports are not supported; use a chunk "
                 "manifest";
      }
      return false;
    }
    const std::string manifestPath =
        (fs::path(destination_root) / ".macmu-image-manifest.json").string();
    if (!download_remote_manifest(source, manifestPath, error)) {
      return false;
    }
    return extract_chunk_manifest(options, manifestPath, source,
                                  destination_root, progress, error);
  }

  std::string localPath = source;
  if (starts_with(source, "file://") &&
      !path_from_file_url(source, &localPath, error)) {
    return false;
  }
  localPath = fs::absolute(localPath, ec).string();
  if (!ec && fs::is_directory(localPath, ec)) {
    const std::string directoryPath = localPath;
    localPath = (fs::path(localPath) / "manifest.json").string();
    ec.clear();
    if (!fs::is_regular_file(localPath, ec)) {
      if (error) {
        *error = "image source directory does not contain manifest.json: " +
                 directoryPath;
      }
      return false;
    }
  }
  if (ec || !fs::is_regular_file(localPath, ec)) {
    if (error) {
      *error = "image source does not exist: " + localPath;
    }
    return false;
  }
  if (ends_with_case_insensitive(localPath, ".zip")) {
    return run_checked_task(
        "/usr/bin/ditto", {"-x", "-k", localPath, destination_root}, -1, error);
  }
  return extract_chunk_manifest(options, localPath, localPath, destination_root,
                                progress, error);
}

void macmu_cancel_system_image_import() {
  gImportCancelled.store(true, std::memory_order_release);
  std::vector<pid_t> taskPids;
  {
    std::lock_guard<std::mutex> lock(gTaskPidsMutex);
    taskPids.assign(gTaskPids.begin(), gTaskPids.end());
  }
  for (pid_t taskPid : taskPids) {
    if (taskPid > 0) {
      kill(taskPid, SIGTERM);
    }
  }
}
