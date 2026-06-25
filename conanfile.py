from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.cmake import CMakeDeps


class AemuConan(ConanFile):
    name = "aemu-core"
    version = "0.1.0"
    package_type = "application"

    settings = "os", "arch", "compiler", "build_type"

    # macOS arm64 core build external dependency boundary. Source-level
    # third_party checkouts should move here as they are deleted from external/.
    generators = "VirtualBuildEnv", "VirtualRunEnv"

    default_options = {
        "abseil/*:shared": False,
        "flatbuffers/*:shared": False,
        "libusb/*:shared": False,
        "libxml2/*:shared": False,
        "libxml2/*:programs": False,
        "libxml2/*:zlib": False,
        "libxml2/*:lzma": False,
        "lz4/*:shared": False,
        "protobuf/*:shared": False,
        "protobuf/*:lite": False,
        "protobuf/*:upb": False,
        "protobuf/*:with_rtti": False,
        "protobuf/*:with_zlib": True,
        "zlib/*:shared": False,
    }

    def validate(self):
        if str(self.settings.os) != "Macos" or str(self.settings.arch) != "armv8":
            raise ConanInvalidConfiguration("aemu-core first cut supports only macOS arm64")

    def requirements(self):
        self.requires("abseil/20240116.2")
        self.requires("flatbuffers/25.9.23")
        self.requires("libusb/1.0.29")
        self.requires("libxml2/2.9.14")
        self.requires("lz4/1.10.0")
        self.requires("protobuf/5.28.3")
        self.requires("zlib/1.3.1")

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
