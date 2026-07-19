// SPDX-License-Identifier: MIT

#include "shell_options.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main() {
    try {
        unsetenv("MACMU_AUTO_IMPORT_IMAGE");
        unsetenv("AEMU_SHELL_AUTO_IMPORT_IMAGE");

        char executable[] = "macmu";
        char* defaultArgs[] = {executable};
        require(!parse_options(1, defaultArgs).autoImportDefaultImage,
                "first launch should wait for an image source choice");

        char autoFlag[] = "--auto-image-import";
        char* autoArgs[] = {executable, autoFlag};
        require(parse_options(2, autoArgs).autoImportDefaultImage,
                "--auto-image-import did not enable unattended official import");

        setenv("MACMU_AUTO_IMPORT_IMAGE", "1", 1);
        require(parse_options(1, defaultArgs).autoImportDefaultImage,
                "MACMU_AUTO_IMPORT_IMAGE=1 did not enable unattended official import");

        char noAutoFlag[] = "--no-auto-image-import";
        char* noAutoArgs[] = {executable, noAutoFlag};
        require(!parse_options(2, noAutoArgs).autoImportDefaultImage,
                "--no-auto-image-import did not override the environment");

        std::cout << "shell_options_test: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "shell_options_test: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
