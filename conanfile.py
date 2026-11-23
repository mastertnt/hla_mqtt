# Compatible Conan 1.66 (new generators) and Conan 2.x
from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout


class HlaMqttRtiConan(ConanFile):
    name = "hla-mqtt-rti"
    version = "0.1.0"
    description = "IEEE 1516e (HLA Evolved) RTI library binary compatible, transported over MQTT"
    settings = "os", "arch", "compiler", "build_type"
    options = {
        "with_ssl": [True, False],
        "use_unique_ptr": [True, False],
        "library_name": ["ANY"],
    }
    default_options = {
        "with_ssl": False,
        "use_unique_ptr": False,
        "library_name": "rti1516e",
    }
    exports_sources = "CMakeLists.txt", "include/*", "src/*", "tests/*", "tools/*"

    def configure(self):
        # Paho linked statically into the RTI DLL: federates only need rti1516e.dll
        # Only `shared` is set on paho-mqtt-cpp: depending on the recipe revision
        # (Conan 1 ConanCenter 1.4.0), it has no `ssl` option.
        self.options["paho-mqtt-cpp"].shared = False
        self.options["paho-mqtt-c"].shared = False
        # paho-mqtt-c has `ssl` (default True in ConanCenter, which pulls OpenSSL).
        self.options["paho-mqtt-c"].ssl = bool(self.options.with_ssl)

    def requirements(self):
        self.requires("paho-mqtt-cpp/1.4.0")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
        tc = CMakeToolchain(self)
        tc.cache_variables["HLA_MQTT_RTI_LIBRARY_NAME"] = str(self.options.library_name)
        tc.cache_variables["HLA_MQTT_RTI_USE_UNIQUE_PTR"] = bool(self.options.use_unique_ptr)
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        CMake(self).install()

    def package_info(self):
        self.cpp_info.libs = [str(self.options.library_name)]
