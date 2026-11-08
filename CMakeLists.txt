cmake_minimum_required(VERSION 3.16)
project(Q2Translate VERSION 2.8.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS
    Core Gui Widgets Network Qml Concurrent Svg Xml)

set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
set(YAML_CPP_FORMAT_SOURCE OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/yaml-cpp EXCLUDE_FROM_ALL)

file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS "src/*.cpp" "src/*.h")

qt_add_executable(Q2Translate
    ${APP_SOURCES}
    resources/resources.qrc
)

# The version lives in project(...) above; only main.cpp bakes it in, so a
# version bump does not rebuild the whole project.
set_source_files_properties(src/main.cpp PROPERTIES
    COMPILE_DEFINITIONS "APP_VERSION=\"${PROJECT_VERSION}\"")

if(WIN32)
    enable_language(RC)
    target_sources(Q2Translate PRIVATE resources/app.rc)
endif()

set_target_properties(Q2Translate PROPERTIES
    OUTPUT_NAME "Q2Translate"
    WIN32_EXECUTABLE TRUE
)

target_include_directories(Q2Translate PRIVATE src
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pdfium/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nuspell/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/icu/include")
target_link_libraries(Q2Translate PRIVATE
    Qt6::Core
    Qt6::CorePrivate
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::Qml
    Qt6::Concurrent
    Qt6::Svg
    Qt6::Xml
    yaml-cpp::yaml-cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nuspell/lib/libnuspell.dll.a"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/icu/lib/libicuuc.dll.a"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/icu/lib/libicudata.dll.a"
)

if(WIN32)
    target_link_libraries(Q2Translate PRIVATE sapi ole32 uuid bcrypt winmm
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pdfium/lib/pdfium.dll.lib")
    add_custom_command(TARGET Q2Translate POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pdfium/bin/pdfium.dll"
            "$<TARGET_FILE_DIR:Q2Translate>/pdfium.dll")
endif()

if(MSVC)
    target_compile_options(Q2Translate PRIVATE /utf-8 /W4 /permissive-)
else()
    target_compile_options(Q2Translate PRIVATE -Wall -Wextra)
endif()

add_custom_command(TARGET Q2Translate POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/settings-default.json"
        "$<TARGET_FILE_DIR:Q2Translate>/settings-default.json"
    COMMAND ${CMAKE_COMMAND} -E touch
        "$<TARGET_FILE_DIR:Q2Translate>/portable.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/data"
        "$<TARGET_FILE_DIR:Q2Translate>"
)

# Locales and themes are data-only files: a POST_BUILD copy would be skipped
# when only these files change.  Keep the runnable build directory in sync on
# every build so newly added locales/themes appear without a relink.
add_custom_target(sync_runtime_data ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:Q2Translate>/Locales"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/data/Locales"
        "$<TARGET_FILE_DIR:Q2Translate>/Locales"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:Q2Translate>/Themes"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/data/Themes"
        "$<TARGET_FILE_DIR:Q2Translate>/Themes"
    COMMENT "Syncing Locales and Themes"
)
add_dependencies(Q2Translate sync_runtime_data)

if(WIN32)
    # Keep the build output runnable on its own (Qt libraries, plugins and the
    # MinGW runtime next to Q2Translate.exe) instead of relying on PATH.
    add_custom_command(TARGET Q2Translate POST_BUILD
        COMMAND $<TARGET_FILE:Qt6::windeployqt>
            --release
            --compiler-runtime
            --no-translations
            "$<TARGET_FILE:Q2Translate>"
        COMMENT "Deploying Qt runtime into the build directory"
    )
    add_custom_command(TARGET Q2Translate POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nuspell/bin/libnuspell.dll"
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/icu/bin/cygicuuc76.dll"
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/icu/bin/cygicudata76.dll"
            "$<TARGET_FILE_DIR:Q2Translate>"
        COMMENT "Deploying Nuspell and ICU"
    )
endif()

