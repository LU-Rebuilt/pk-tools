cmake_minimum_required(VERSION 3.25)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
project(pk-tools VERSION 0.1.0 LANGUAGES CXX DESCRIPTION "LEGO Universe PK archive tools")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(lu_assets
    GIT_REPOSITORY https://github.com/LU-Rebuilt/lu-assets.git
    GIT_TAG main
)
FetchContent_MakeAvailable(lu_assets)

FetchContent_Declare(tool_common
    GIT_REPOSITORY https://github.com/LU-Rebuilt/tool-common.git
    GIT_TAG main
)
FetchContent_MakeAvailable(tool_common)

# CLI extractor
add_executable(pk_extractor src/pk_extractor.cpp)
target_link_libraries(pk_extractor PRIVATE lu_assets)

# GUI viewer (optional, requires Qt6)
find_package(Qt6 COMPONENTS Widgets Concurrent QUIET)
if(Qt6_FOUND)
    set(CMAKE_AUTOMOC ON)
    add_executable(pk_viewer
        src/pk_viewer/main.cpp
        src/pk_viewer/main_window.cpp src/pk_viewer/main_window.h
    )
    target_link_libraries(pk_viewer PRIVATE qt_common lu_assets Qt6::Widgets Qt6::Concurrent)
    target_include_directories(pk_viewer PRIVATE src/pk_viewer)
else()
    message(STATUS "Skipping pk_viewer (Qt6 not found)")
endif()
