# MeshRepair test suite (GoogleTest, synthetic meshes generated in code).

find_package(GTest CONFIG REQUIRED)

add_executable(meshrepair_tests
    src/test_mesh_binary.cpp
    src/test_protocol.cpp
    src/test_hole_ops.cpp
    src/test_preprocessor.cpp
    src/test_pipeline.cpp
)

target_link_libraries(meshrepair_tests
    PRIVATE
        meshrepair_core
        GTest::gtest_main
)

if(MSVC)
    # Innocuous for tests; core already carries /W4 /permissive- etc.
    target_compile_options(meshrepair_tests PRIVATE /EHsc /MP)
endif()

include(GoogleTest)
gtest_discover_tests(meshrepair_tests DISCOVERY_TIMEOUT 120)
