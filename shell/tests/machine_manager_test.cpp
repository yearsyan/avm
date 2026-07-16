// SPDX-License-Identifier: MIT

#include "machine_manager.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

class TemporaryDirectory {
   public:
    TemporaryDirectory() {
        const fs::path base = fs::temp_directory_path();
        for (unsigned attempt = 0; attempt < 1000; ++attempt) {
            path_ = base / ("macmu-machine-manager-test-" + std::to_string(::getpid()) + "-" +
                            std::to_string(attempt));
            std::error_code ec;
            if (fs::create_directory(path_, ec)) {
                return;
            }
        }
        throw std::runtime_error("failed to create temporary directory");
    }

    ~TemporaryDirectory() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const { return path_; }

   private:
    fs::path path_;
};

void write_file(const fs::path& path, const std::string& contents = "test") {
    std::ofstream output(path, std::ios::out | std::ios::binary | std::ios::trunc);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!output) {
        throw std::runtime_error("failed to write " + path.string());
    }
}

void write_little_endian_u64(unsigned char* bytes, uint64_t value) {
    for (size_t i = 0; i < sizeof(value); ++i) {
        bytes[i] = static_cast<unsigned char>((value >> (8 * i)) & 0xff);
    }
}

void write_metadata_disk(const fs::path& path) {
    constexpr uint64_t kDiskSize = 4 * 1024 * 1024;
    constexpr uint64_t kFirstLba = 2048;
    constexpr uint64_t kLastLba = 4095;
    constexpr uint64_t kPartitionOffset = kFirstLba * 512;
    constexpr uint64_t kSuperblockOffset = kPartitionOffset + 1024;

    std::fstream output(path, std::ios::out | std::ios::in | std::ios::binary | std::ios::trunc);
    output.seekp(static_cast<std::streamoff>(kDiskSize - 1));
    output.put('\0');

    output.seekp(512);
    output.write("EFI PART", 8);

    std::array<unsigned char, 128> entry{};
    write_little_endian_u64(entry.data() + 32, kFirstLba);
    write_little_endian_u64(entry.data() + 40, kLastLba);
    const std::string name = "metadata";
    for (size_t i = 0; i < name.size(); ++i) {
        entry[56 + i * 2] = static_cast<unsigned char>(name[i]);
    }
    output.seekp(1024);
    output.write(reinterpret_cast<const char*>(entry.data()),
                 static_cast<std::streamsize>(entry.size()));

    const std::array<unsigned char, 2> magic = {0x53, 0xef};
    output.seekp(static_cast<std::streamoff>(kSuperblockOffset + 56));
    output.write(reinterpret_cast<const char*>(magic.data()),
                 static_cast<std::streamsize>(magic.size()));
    output.seekp(static_cast<std::streamoff>(kSuperblockOffset + 120));
    output.write(name.data(), static_cast<std::streamsize>(name.size()));
    if (!output) {
        throw std::runtime_error("failed to write metadata disk");
    }
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main() {
    try {
        TemporaryDirectory temporary;
        const fs::path app_data = temporary.path() / "data";
        const fs::path system = temporary.path() / "system";
        const fs::path avd_home = app_data / "avd";
        fs::create_directories(system);

        constexpr std::array<const char*, 9> kRequiredFiles = {
            "advancedFeatures.ini",         "android-info.txt",
            "VerifiedBootParams.textproto", "kernel-ranchu",
            "ramdisk.img",                  "system-qemu.img",
            "userdata.img",                 "vendor-qemu.img",
            "vendor_boot.img",
        };
        for (const char* file : kRequiredFiles) {
            write_file(system / file);
        }
        write_metadata_disk(system / "encryptionkey.img");

        ShellOptions options;
        options.appDataDir = app_data.string();
        options.avdHome = avd_home.string();
        options.systemPath = system.string();
        options.avdName = "machine_manager_test";

        fs::rename(system / "advancedFeatures.ini", system / "advancedFeatures.ini.missing");
        require(!macmu_system_image_exists(options),
                "image without advancedFeatures.ini was accepted");
        fs::rename(system / "advancedFeatures.ini.missing", system / "advancedFeatures.ini");
        require(macmu_system_image_exists(options), "valid release image was rejected");

        std::string error;
        require(macmu_create_default_machine(options, &error), "machine creation failed: " + error);
        require(macmu_machine_exists(options), "created machine was not recognized");

        const fs::path machine = macmu_machine_path(options);
        require(fs::is_symlink(machine / "system-qemu.img"), "system-qemu.img is not a symlink");
        require(fs::is_symlink(machine / "vendor-qemu.img"), "vendor-qemu.img is not a symlink");
        require(fs::equivalent(machine / "system-qemu.img", system / "system-qemu.img"),
                "system-qemu.img symlink points to the wrong file");
        require(fs::equivalent(machine / "vendor-qemu.img", system / "vendor-qemu.img"),
                "vendor-qemu.img symlink points to the wrong file");
        require(fs::file_size(machine / "encryptionkey.img") ==
                    fs::file_size(system / "encryptionkey.img"),
                "metadata disk was not copied");
        require(fs::file_size(machine / "cache.img") == 66ull * 1024ull * 1024ull,
                "cache disk has the wrong size");

        write_file(machine / "encryptionkey.img", std::string(64, '\0'));
        write_file(machine / "encryptionkey.img.qcow2", "stale");
        require(!macmu_machine_exists(options), "invalid metadata disk was accepted");
        error.clear();
        require(macmu_create_default_machine(options, &error),
                "legacy metadata repair failed: " + error);
        require(macmu_machine_exists(options), "repaired machine was not recognized");
        require(!fs::exists(machine / "encryptionkey.img.qcow2"),
                "stale metadata qcow2 overlay was not removed");

        std::cout << "machine_manager_test: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "machine_manager_test: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
