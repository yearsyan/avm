// SPDX-License-Identifier: MIT

#include "machine_manager.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace {

namespace fs = std::filesystem;

std::string path_join(const std::string& lhs, const std::string& rhs) {
    if (lhs.empty()) {
        return rhs;
    }
    if (lhs.back() == '/') {
        return lhs + rhs;
    }
    return lhs + "/" + rhs;
}

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

std::string config_ini_for(const ShellOptions& options) {
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
        << "hw.gpu.enabled=no\n"
        << "hw.gpu.mode=auto\n"
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
        << "image.sysdir.1=images/android-35-arm64/\n"
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
        << "target=android-35\n"
        << "test.delayAdbTillBootComplete=0\n"
        << "test.monitorAdb=0\n"
        << "test.quitAfterBootTimeOut=-1\n"
        << "userdata.useQcow2=no\n"
        << "vm.heapSize=228M\n";
    return out.str();
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
    return file_exists(path_join(options.systemPath, "kernel-ranchu")) &&
           file_exists(path_join(options.systemPath, "ramdisk.img")) &&
           file_exists(path_join(options.systemPath, "system.img")) &&
           file_exists(path_join(options.systemPath, "vendor.img"));
}

bool macmu_machine_exists(const ShellOptions& options) {
    return file_exists(macmu_machine_ini_path(options)) &&
           file_exists(path_join(macmu_machine_path(options), "config.ini"));
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
        "target=android-35\n";
    if (!write_text_file(macmu_machine_ini_path(options), rootIni, error)) {
        return false;
    }
    return write_text_file(path_join(machinePath, "config.ini"), config_ini_for(options), error);
}
