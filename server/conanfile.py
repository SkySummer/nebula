from conan import ConanFile


class NebulaConan(ConanFile):
    name = "nebula"
    settings = "os", "arch", "compiler", "build_type"
    requires = (
        "grpc/1.78.1",
        "libpqxx/8.0.1",
        "openssl/3.6.2",
    )
    generators = "CMakeToolchain", "CMakeDeps"
