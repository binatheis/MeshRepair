# SPDX-License-Identifier: GPL-3.0-or-later
# Conan 2 build definition for MeshRepair (native + Python binding wheels).
#
# Local usage:
#   conan install . -of build/conan -b missing -s build_type=Release
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=build/conan/conan_toolchain.cmake \
#         -DCMAKE_PREFIX_PATH=build/conan -DBUILD_MESHREPAIR_GUI=OFF
#   cmake --build build --config Release

from conan import ConanFile


class MeshRepairConan(ConanFile):
    name = "meshrepair"
    version = "2.8.0"
    settings = "os", "arch", "compiler", "build_type"
    generators = "CMakeDeps", "CMakeToolchain"

    # Notes:
    # - boost header-only: CGAL only consumes Boost headers on this code path.
    # - onetbb static: avoids bundling tbb12.dll/.so/.dylib inside the wheels.
    # - spdlog header-only: avoids ABI mismatches (repo already uses SPDLOG_USE_STD_FORMAT).
    # - gtest static: links into the test executable only.
    # - gmp/mpfr static: avoids a runtime DLL dependency (gmp-10.dll) that would
    #   otherwise need to be bundled inside the wheel. Static linking makes the
    #   .pyd self-contained on Windows, Linux, and macOS.
    default_options = {
        "boost/*:header_only": True,
        "onetbb/*:shared": False,
        "spdlog/*:header_only": True,
        "gtest/*:shared": False,
        "gmp/*:shared": False,
        "mpfr/*:shared": False,
    }

    def requirements(self):
        # "Latest safe" per plan (2026-08-17): cgal 6.2 with conservative
        # overrides for boost and eigen; gmp/mpfr respect the cgal recipe pins.
        self.requires("cgal/6.2")
        self.requires("boost/1.91.0", force=True)
        self.requires("eigen/3.4.1", force=True)
        self.requires("nlohmann_json/3.12.0")
        self.requires("onetbb/2023.1.0")
        self.requires("gtest/1.18.0")

    def configure(self):
        # Windows/MSVC: runtime fixed by the conan toolchain (dynamic /MD),
        # which is required for CPython extension compatibility.
        if self.settings.os == "Windows":
            self.options["gtest/*"].shared = False
