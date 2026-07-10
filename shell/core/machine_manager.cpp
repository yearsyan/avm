// SPDX-License-Identifier: MIT

#include "machine_manager.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include "posix_util.h"

namespace {

namespace fs = std::filesystem;
using macmu::shell::path_join;

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

bool system_image_exists_at(const std::string& path) {
    return file_exists(path_join(path, "kernel-ranchu")) &&
           file_exists(path_join(path, "ramdisk.img")) &&
           file_exists(path_join(path, "system.img")) &&
           file_exists(path_join(path, "vendor.img")) &&
           file_exists(path_join(path, "vendor_boot.img"));
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
           file_exists(path_join(macmu_machine_path(options), "encryptionkey.img"));
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
    if (!ensure_sized_file(path_join(machinePath, "encryptionkey.img"), 64ull * 1024ull * 1024ull,
                           error)) {
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
