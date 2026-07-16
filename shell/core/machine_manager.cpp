// SPDX-License-Identifier: MIT

#include "machine_manager.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include "posix_util.h"

namespace {

namespace fs = std::filesystem;
using macmu::shell::path_join;

constexpr uint64_t kDiskSectorSize = 512;
constexpr uint64_t kGptHeaderOffset = kDiskSectorSize;
constexpr uint64_t kGptFirstEntryOffset = 2 * kDiskSectorSize;
constexpr size_t kGptEntrySize = 128;
constexpr size_t kGptFirstLbaOffset = 32;
constexpr size_t kGptLastLbaOffset = 40;
constexpr size_t kGptPartitionNameOffset = 56;
constexpr uint64_t kExt4SuperblockOffset = 1024;
constexpr size_t kExt4MagicOffset = 56;
constexpr size_t kExt4VolumeNameOffset = 120;

bool ensure_directory(const std::string& path, std::string* error) {
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        return true;
    }
    if (fs::create_directories(path, ec) || fs::is_directory(path, ec)) {
        return true;
    }
    if (error) {
        *error = "failed to create directory: " + path + " (" + ec.message() + ")";
    }
    return false;
}

bool file_exists(const std::string& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

uint64_t read_little_endian_u64(const unsigned char* bytes) {
    uint64_t value = 0;
    for (size_t i = 0; i < sizeof(value); ++i) {
        value |= static_cast<uint64_t>(bytes[i]) << (8 * i);
    }
    return value;
}

bool read_exact_at(std::ifstream* input, uint64_t offset, void* data, size_t size) {
    input->clear();
    input->seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!*input) {
        return false;
    }
    input->read(static_cast<char*>(data), static_cast<std::streamsize>(size));
    return input->gcount() == static_cast<std::streamsize>(size);
}

bool has_utf16le_name(const unsigned char* bytes, size_t size, const char* expected) {
    const size_t length = std::strlen(expected);
    if (size < (length + 1) * 2) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        if (bytes[i * 2] != static_cast<unsigned char>(expected[i]) || bytes[i * 2 + 1] != 0) {
            return false;
        }
    }
    return bytes[length * 2] == 0 && bytes[length * 2 + 1] == 0;
}

bool metadata_disk_exists(const std::string& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        return false;
    }
    const uintmax_t file_size = fs::file_size(path, ec);
    if (ec || file_size < 2 * 1024 * 1024) {
        return false;
    }

    std::ifstream input(path, std::ios::in | std::ios::binary);
    if (!input) {
        return false;
    }

    std::array<unsigned char, 8> gpt_magic{};
    if (!read_exact_at(&input, kGptHeaderOffset, gpt_magic.data(), gpt_magic.size()) ||
        std::memcmp(gpt_magic.data(), "EFI PART", gpt_magic.size()) != 0) {
        return false;
    }

    std::array<unsigned char, kGptEntrySize> entry{};
    if (!read_exact_at(&input, kGptFirstEntryOffset, entry.data(), entry.size())) {
        return false;
    }
    const uint64_t first_lba = read_little_endian_u64(entry.data() + kGptFirstLbaOffset);
    const uint64_t last_lba = read_little_endian_u64(entry.data() + kGptLastLbaOffset);
    if (first_lba == 0 || last_lba < first_lba ||
        first_lba >= file_size / kDiskSectorSize ||
        last_lba >= file_size / kDiskSectorSize ||
        !has_utf16le_name(entry.data() + kGptPartitionNameOffset,
                          entry.size() - kGptPartitionNameOffset, "metadata")) {
        return false;
    }

    const uint64_t partition_offset = first_lba * kDiskSectorSize;
    const uint64_t superblock_offset = partition_offset + kExt4SuperblockOffset;
    if (superblock_offset + kExt4VolumeNameOffset + 16 > file_size) {
        return false;
    }

    std::array<unsigned char, 2> ext4_magic{};
    if (!read_exact_at(&input, superblock_offset + kExt4MagicOffset, ext4_magic.data(),
                       ext4_magic.size()) ||
        ext4_magic[0] != 0x53 || ext4_magic[1] != 0xef) {
        return false;
    }

    std::array<char, 16> volume_name{};
    if (!read_exact_at(&input, superblock_offset + kExt4VolumeNameOffset, volume_name.data(),
                       volume_name.size())) {
        return false;
    }
    return std::memcmp(volume_name.data(), "metadata", 8) == 0 && volume_name[8] == '\0';
}

bool system_image_exists_at(const std::string& path) {
    constexpr std::array<const char*, 9> kRequiredFiles = {
        "advancedFeatures.ini",         "android-info.txt",
        "VerifiedBootParams.textproto", "kernel-ranchu",
        "ramdisk.img",                  "system-qemu.img",
        "userdata.img",                 "vendor-qemu.img",
        "vendor_boot.img",
    };
    for (const char* file : kRequiredFiles) {
        if (!file_exists(path_join(path, file))) {
            return false;
        }
    }
    return metadata_disk_exists(path_join(path, "encryptionkey.img"));
}

bool write_text_file(const std::string& path, const std::string& text, std::string* error) {
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out) {
        if (error) {
            *error = "failed to write: " + path;
        }
        return false;
    }
    out << text;
    if (!out.good()) {
        if (error) {
            *error = "failed to finish writing: " + path;
        }
        return false;
    }
    return true;
}

bool ensure_sized_file(const std::string& path, uintmax_t size, std::string* error) {
    std::error_code ec;
    if (fs::is_regular_file(path, ec) && fs::file_size(path, ec) == size) {
        return true;
    }
    if (!fs::exists(path, ec)) {
        std::ofstream out(path, std::ios::out | std::ios::binary);
        if (!out) {
            if (error) {
                *error = "failed to create disk image: " + path;
            }
            return false;
        }
    }
    fs::resize_file(path, size, ec);
    if (!ec && fs::is_regular_file(path, ec) && fs::file_size(path, ec) == size) {
        return true;
    }
    if (error) {
        *error = "failed to create disk image: " + path + " (" + ec.message() + ")";
    }
    return false;
}

bool replace_with_symlink(const std::string& source, const std::string& destination,
                          std::string* error) {
    std::error_code ec;
    fs::remove(destination, ec);
    ec.clear();
    fs::create_symlink(fs::absolute(source, ec), destination, ec);
    if (!ec && file_exists(destination)) {
        return true;
    }
    if (error) {
        *error = "failed to link system image: " + destination + " (" + ec.message() + ")";
    }
    return false;
}

bool ensure_metadata_disk(const std::string& source, const std::string& destination,
                          std::string* error) {
    if (metadata_disk_exists(destination)) {
        return true;
    }

    std::error_code ec;
    fs::remove(destination, ec);
    fs::remove(destination + ".qcow2", ec);
    ec.clear();
    fs::copy_file(source, destination, fs::copy_options::overwrite_existing, ec);
    if (!ec && metadata_disk_exists(destination)) {
        return true;
    }
    if (error) {
        *error = "failed to install metadata disk: " + destination + " (" + ec.message() + ")";
    }
    return false;
}

std::string config_ini_for(const ShellOptions& options) {
    std::string sysdir = options.systemPath;
    if (!sysdir.empty() && sysdir.back() != '/') {
        sysdir += "/";
    }

    std::ostringstream out;
    out
        << "PlayStore.enabled=no\n"
        << "abi.type=arm64-v8a\n"
        << "avd.id=" << options.avdName << "\n"
        << "avd.ini.encoding=UTF-8\n"
        << "avd.name=" << options.avdName << "\n"
        << "disk.cachePartition=yes\n"
        << "disk.cachePartition.size=66MB\n"
        << "disk.dataPartition.path=<temp>\n"
        << "disk.dataPartition.size=6G\n"
        << "disk.systemPartition.size=0\n"
        << "disk.vendorPartition.size=0\n"
        << "fastboot.forceChosenSnapshotBoot=no\n"
        << "fastboot.forceColdBoot=yes\n"
        << "fastboot.forceFastBoot=no\n"
        << "firstboot.bootFromDownloadableSnapshot=no\n"
        << "firstboot.bootFromLocalSnapshot=no\n"
        << "firstboot.saveToLocalSnapshot=no\n"
        << "hw.accelerometer=yes\n"
        << "hw.arc=false\n"
        << "hw.audioInput=yes\n"
        << "hw.audioOutput=yes\n"
        << "hw.battery=yes\n"
        << "hw.camera.back=emulated\n"
        << "hw.camera.back.orientation=90\n"
        << "hw.camera.front=none\n"
        << "hw.cpu.arch=arm64\n"
        << "hw.cpu.ncore=4\n"
        << "hw.dPad=no\n"
        << "hw.device.manufacturer=Google\n"
        << "hw.device.name=pixel_6\n"
        << "hw.gltransport=pipe\n"
        << "hw.gpu.enabled=yes\n"
        << "hw.gpu.mode=host\n"
        << "hw.gsmModem=yes\n"
        << "hw.gyroscope=yes\n"
        << "hw.keyboard=no\n"
        << "hw.keyboard.charmap=qwerty2\n"
        << "hw.keyboard.lid=yes\n"
        << "hw.lcd.backlight=yes\n"
        << "hw.lcd.circular=false\n"
        << "hw.lcd.density=420\n"
        << "hw.lcd.depth=32\n"
        << "hw.lcd.height=2400\n"
        << "hw.lcd.transparent=false\n"
        << "hw.lcd.vsync=60\n"
        << "hw.lcd.width=1080\n"
        << "hw.mainKeys=no\n"
        << "hw.ramSize=2G\n"
        << "hw.screen=multi-touch\n"
        << "hw.sdCard=yes\n"
        << "hw.sensors.light=yes\n"
        << "hw.sensors.magnetic_field=yes\n"
        << "hw.sensors.orientation=yes\n"
        << "hw.sensors.pressure=yes\n"
        << "hw.sensors.proximity=yes\n"
        << "hw.trackBall=no\n"
        << "hw.useext4=yes\n"
        << "image.sysdir.1=" << sysdir << "\n"
        << "kernel.newDeviceNaming=autodetect\n"
        << "kernel.supportsYaffs2=autodetect\n"
        << "runtime.network.latency=none\n"
        << "runtime.network.speed=full\n"
        << "sdcard.size=512 MB\n"
        << "showDeviceFrame=yes\n"
        << "tag.display=Default Android System Image\n"
        << "tag.displaynames=Default Android System Image\n"
        << "tag.id=default\n"
        << "tag.ids=default\n"
        << "target=android-36\n"
        << "test.delayAdbTillBootComplete=0\n"
        << "test.monitorAdb=0\n"
        << "test.quitAfterBootTimeOut=-1\n"
        << "userdata.useQcow2=no\n"
        << "vm.heapSize=228M\n";
    return out.str();
}

bool stage_system_image_directory(const fs::path& source,
                                  const fs::path& importing,
                                  std::string* error) {
    std::error_code rename_ec;
    fs::rename(source, importing, rename_ec);
    if (!rename_ec) {
        return true;
    }

    if (rename_ec != std::errc::cross_device_link) {
        if (error) {
            *error = "failed to move system image into place: " + rename_ec.message();
        }
        return false;
    }

    std::error_code copy_ec;
    fs::copy(source, importing,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing |
                 fs::copy_options::copy_symlinks,
             copy_ec);
    if (!copy_ec) {
        return true;
    }
    std::error_code cleanup_ec;
    fs::remove_all(importing, cleanup_ec);
    if (error) {
        *error = "failed to copy system image after cross-volume move failed: " +
                 copy_ec.message();
    }
    return false;
}

}  // namespace

std::string macmu_machine_path(const ShellOptions& options) {
    return path_join(options.avdHome, options.avdName + ".avd");
}

std::string macmu_machine_ini_path(const ShellOptions& options) {
    return path_join(options.avdHome, options.avdName + ".ini");
}

bool macmu_ensure_runtime_directories(const ShellOptions& options, std::string* error) {
    return ensure_directory(options.appDataDir, error) &&
           ensure_directory(options.avdHome, error) &&
           ensure_directory(path_join(options.appDataDir, "images"), error);
}

bool macmu_system_image_exists(const ShellOptions& options) {
    return system_image_exists_at(options.systemPath);
}

bool macmu_machine_exists(const ShellOptions& options) {
    return file_exists(macmu_machine_ini_path(options)) &&
           file_exists(path_join(macmu_machine_path(options), "config.ini")) &&
           file_exists(path_join(macmu_machine_path(options), "system-qemu.img")) &&
           file_exists(path_join(macmu_machine_path(options), "vendor-qemu.img")) &&
           metadata_disk_exists(path_join(macmu_machine_path(options), "encryptionkey.img"));
}

bool macmu_create_default_machine(const ShellOptions& options, std::string* error) {
    if (!macmu_ensure_runtime_directories(options, error)) {
        return false;
    }
    if (!macmu_system_image_exists(options)) {
        if (error) {
            *error = "missing MacMu system image at: " + options.systemPath;
        }
        return false;
    }

    const std::string machinePath = macmu_machine_path(options);
    if (!ensure_directory(machinePath, error)) {
        return false;
    }

    const std::string rootIni =
        "avd.ini.encoding=UTF-8\n"
        "path=" + machinePath + "\n"
        "path.rel=avd/" + options.avdName + ".avd\n"
        "target=android-36\n";
    if (!write_text_file(macmu_machine_ini_path(options), rootIni, error)) {
        return false;
    }
    if (!write_text_file(path_join(machinePath, "config.ini"), config_ini_for(options), error)) {
        return false;
    }
    if (!replace_with_symlink(path_join(options.systemPath, "system-qemu.img"),
                              path_join(machinePath, "system-qemu.img"), error) ||
        !replace_with_symlink(path_join(options.systemPath, "vendor-qemu.img"),
                              path_join(machinePath, "vendor-qemu.img"), error)) {
        return false;
    }
    if (!ensure_metadata_disk(path_join(options.systemPath, "encryptionkey.img"),
                              path_join(machinePath, "encryptionkey.img"), error)) {
        return false;
    }
    return ensure_sized_file(path_join(machinePath, "cache.img"), 66ull * 1024ull * 1024ull,
                             error);
}

bool macmu_find_system_image_directory(const std::string& root,
                                       std::string* system_image_dir,
                                       std::string* error) {
    if (system_image_exists_at(root)) {
        if (system_image_dir) {
            *system_image_dir = root;
        }
        return true;
    }

    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        if (error) {
            *error = "not a directory: " + root;
        }
        return false;
    }

    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied,
                                             ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_directory(ec)) {
            continue;
        }
        const std::string candidate = it->path().string();
        if (system_image_exists_at(candidate)) {
            if (system_image_dir) {
                *system_image_dir = candidate;
            }
            return true;
        }
    }

    if (error) {
        *error = "archive does not contain a MacMu AOSP16 arm64 system image";
    }
    return false;
}

bool macmu_replace_system_image_from_directory(const ShellOptions& options,
                                               const std::string& source_dir,
                                               std::string* error) {
    if (!system_image_exists_at(source_dir)) {
        if (error) {
            *error = "missing required image files in: " + source_dir;
        }
        return false;
    }
    if (!macmu_ensure_runtime_directories(options, error)) {
        return false;
    }

    const fs::path destination(options.systemPath);
    const fs::path parent = destination.parent_path();
    const fs::path importing(destination.string() + ".importing");
    const fs::path previous(destination.string() + ".previous");

    std::error_code ec;
    fs::remove_all(importing, ec);
    fs::remove_all(previous, ec);

    fs::create_directories(parent, ec);
    if (ec) {
        if (error) {
            *error = "failed to create image parent directory: " + parent.string() + " (" +
                     ec.message() + ")";
        }
        return false;
    }

    if (!stage_system_image_directory(source_dir, importing, error)) {
        fs::remove_all(importing, ec);
        return false;
    }
    if (!system_image_exists_at(importing.string())) {
        fs::remove_all(importing, ec);
        if (error) {
            *error = "copied image did not pass validation";
        }
        return false;
    }

    if (fs::exists(destination, ec)) {
        fs::rename(destination, previous, ec);
        if (ec) {
            fs::remove_all(importing, ec);
            if (error) {
                *error = "failed to replace existing system image: " + ec.message();
            }
            return false;
        }
    }

    fs::rename(importing, destination, ec);
    if (ec) {
        std::error_code restore_ec;
        if (fs::exists(previous, restore_ec)) {
            fs::rename(previous, destination, restore_ec);
        }
        fs::remove_all(importing, restore_ec);
        if (error) {
            *error = "failed to activate imported system image: " + ec.message();
        }
        return false;
    }

    fs::remove_all(previous, ec);
    return true;
}
