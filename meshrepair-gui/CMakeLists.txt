find_package(glfw3 REQUIRED CONFIG)
find_package(OpenGL REQUIRED)
find_package(nfd REQUIRED CONFIG)

set(IMGUI_DIR "" CACHE PATH "Path to Dear ImGui source tree")
if(NOT IMGUI_DIR)
    set(_imgui_candidates
        "$ENV{IMGUI_DIR}"
        "${CMAKE_SOURCE_DIR}/../imgui"
        "${CMAKE_SOURCE_DIR}/../../imgui"
        "E:/GH/imgui"
        "/mnt/e/GH/imgui"
    )
    foreach(_candidate ${_imgui_candidates})
        if(NOT IMGUI_DIR AND EXISTS "${_candidate}/imgui.h")
            set(IMGUI_DIR "${_candidate}" CACHE PATH "Path to Dear ImGui source tree" FORCE)
        endif()
    endforeach()
endif()

if(NOT IMGUI_DIR OR NOT EXISTS "${IMGUI_DIR}/imgui.h")
    message(FATAL_ERROR "IMGUI_DIR must point to Dear ImGui sources (imgui.h).")
endif()

if(NOT TARGET nfd::nfd)
    message(FATAL_ERROR "nfd::nfd target not found (nativefiledialog-extended).")
endif()

set(GLFW_TARGET "")
if(TARGET glfw)
    set(GLFW_TARGET glfw)
elseif(TARGET glfw::glfw)
    set(GLFW_TARGET glfw::glfw)
else()
    message(FATAL_ERROR "glfw target not found after find_package(glfw3 CONFIG).")
endif()

if(UNIX AND NOT APPLE)
    # Validate that the imported nfd target is built for this platform (avoid Windows import libs)
    get_target_property(_nfd_location nfd::nfd IMPORTED_LOCATION)
    if(NOT _nfd_location)
        get_target_property(_nfd_location nfd::nfd IMPORTED_LOCATION_RELEASE)
    endif()
    if(_nfd_location AND _nfd_location MATCHES "\\.lib$")
        message(FATAL_ERROR "nfd::nfd appears to point to a Windows import library (${_nfd_location}). "
            "Install a Linux build of nativefiledialog-extended or adjust CMAKE_PREFIX_PATH.")
    endif()
endif()

set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    ${IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
)

add_library(imgui STATIC ${IMGUI_SOURCES})
target_include_directories(imgui
    PUBLIC
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
        ${IMGUI_DIR}/misc/cpp
)
target_link_libraries(imgui PUBLIC ${GLFW_TARGET})

set(MESHREPAIR_GUI_SOURCES
    main.cpp
    mr_gui.cpp
    include/mr_gui.h
)

if(WIN32)
    list(APPEND MESHREPAIR_GUI_SOURCES resources/meshrepair.rc)
endif()

add_executable(meshrepair_gui
    ${MESHREPAIR_GUI_SOURCES}
)

set(MESHREPAIR_GUI_FONT "${CMAKE_SOURCE_DIR}/external/Fira/FiraSans-Medium.otf")
if(NOT EXISTS "${MESHREPAIR_GUI_FONT}")
    message(FATAL_ERROR "Expected font not found at ${MESHREPAIR_GUI_FONT}")
endif()

add_executable(meshrepair_imgui_binary_to_compressed EXCLUDE_FROM_ALL
    ${IMGUI_DIR}/misc/fonts/binary_to_compressed_c.cpp
)

set(MESHREPAIR_GUI_FONT_HEADER "${CMAKE_CURRENT_BINARY_DIR}/meshrepair_gui_font.h")
add_custom_command(
    OUTPUT "${MESHREPAIR_GUI_FONT_HEADER}"
    COMMAND ${CMAKE_COMMAND}
        -DMESHREPAIR_FONT_TOOL=$<TARGET_FILE:meshrepair_imgui_binary_to_compressed>
        -DMESHREPAIR_FONT_INPUT=${MESHREPAIR_GUI_FONT}
        -DMESHREPAIR_FONT_SYMBOL=meshrepair_gui_fira_sans
        -DMESHREPAIR_FONT_OUTPUT=${MESHREPAIR_GUI_FONT_HEADER}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_font.cmake"
    DEPENDS
        meshrepair_imgui_binary_to_compressed
        "${MESHREPAIR_GUI_FONT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/embed_font.cmake"
    COMMENT "Embed FiraSans font for ImGui"
    VERBATIM
)

add_custom_target(meshrepair_gui_font_header
    DEPENDS "${MESHREPAIR_GUI_FONT_HEADER}"
)
add_dependencies(meshrepair_gui meshrepair_gui_font_header)

target_link_libraries(meshrepair_gui
    PRIVATE
        meshrepair_core
        imgui
        ${GLFW_TARGET}
        dnd_glfw
        OpenGL::GL
        nfd::nfd
)

target_include_directories(meshrepair_gui
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(native_flag $<$<AND:$<CONFIG:Release>,$<BOOL:${MESHREPAIR_ENABLE_NATIVE_ARCH}>>:-march=native>)
    set(lto_flag $<$<AND:$<CONFIG:Release>,$<BOOL:${MESHREPAIR_ENABLE_LTO}>>:-flto>)
    target_compile_options(meshrepair_gui PRIVATE
        -Wall
        -Wextra
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:Debug>:-g>
        ${native_flag}
        ${lto_flag}
    )
    if(MESHREPAIR_ENABLE_LTO)
        target_link_options(meshrepair_gui PRIVATE ${lto_flag})
    endif()
elseif(MSVC)
    target_compile_options(meshrepair_gui PRIVATE /W4 /permissive- /EHsc)
endif()

if(WIN32)
    target_link_libraries(meshrepair_gui PRIVATE ole32)
endif()

install(TARGETS meshrepair_gui
    RUNTIME DESTINATION bin
)

if(UNIX AND NOT APPLE)
    install(FILES ${CMAKE_SOURCE_DIR}/packaging/linux/meshrepair.desktop
        DESTINATION share/applications
    )
    foreach(_icon_size 16 24 32 48 64 128 256 512 1024)
        install(FILES ${CMAKE_SOURCE_DIR}/packaging/linux/icons/hicolor/${_icon_size}x${_icon_size}/apps/meshrepair.png
            DESTINATION share/icons/hicolor/${_icon_size}x${_icon_size}/apps
        )
    endforeach()
endif()
