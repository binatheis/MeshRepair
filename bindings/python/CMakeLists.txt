# Python binding CMake: nanobind module over meshrepair_core.
cmake_minimum_required(VERSION 3.20...3.28)

# vcpkg integration is disabled in the root CMakeLists.txt (CMAKE_VS_GLOBALS).

# Python + nanobind discovery. scikit-build-core provides Python; nanobind is
# a pip build requirement, so locate its CMake helpers via the interpreter.
find_package(Python 3.11 REQUIRED COMPONENTS Interpreter Development.Module Development.SABIModule)

execute_process(
    COMMAND "${Python_EXECUTABLE}" -c "import nanobind; print(nanobind.cmake_dir())"
    OUTPUT_VARIABLE _nanobind_cmake_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _nanobind_result
)
if(NOT _nanobind_result EQUAL 0 OR NOT _nanobind_cmake_dir)
    message(FATAL_ERROR "nanobind CMake helpers not found (is nanobind pip-installed?)")
endif()
list(APPEND CMAKE_PREFIX_PATH "${_nanobind_cmake_dir}")

find_package(nanobind CONFIG REQUIRED)

nanobind_add_module(
    meshrepair_core_py
    STABLE_ABI
    NB_DOMAIN MeshRepair
    src/meshrepair_py.cpp
)

# Link against the core library (brings CGAL, Eigen, Boost, etc. transitively).
target_link_libraries(meshrepair_core_py PRIVATE meshrepair_core)

# The module lands inside the meshrepair package dir.
install(TARGETS meshrepair_core_py DESTINATION meshrepair)
