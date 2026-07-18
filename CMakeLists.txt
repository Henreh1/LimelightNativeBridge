cmake_minimum_required(VERSION 3.22)

project(
    LimelightNativeBridge
    VERSION 0.1.6
    LANGUAGES CXX
)

set(
    UE4SS_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../RE-UE4SS"
    CACHE PATH
    "Path to the pinned RE-UE4SS source checkout"
)

if(NOT EXISTS "${UE4SS_ROOT}/CMakeLists.txt")
    message(
        FATAL_ERROR
        "RE-UE4SS was not found. Set UE4SS_ROOT to the pinned UE4SS source directory."
    )
endif()

# I build against the same pinned UE4SS revision that Limelight installs.
add_subdirectory(
    "${UE4SS_ROOT}"
    "${CMAKE_BINARY_DIR}/RE-UE4SS"
)

add_library(
    LimelightNativeBridge
    SHARED
    dllmain.cpp
    MountResolver.cpp
    LivePakMount.cpp
    CharliePackageReload.cpp
)

target_include_directories(
    LimelightNativeBridge
    PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(
    LimelightNativeBridge
    PRIVATE
    UE4SS
)

set_target_properties(
    LimelightNativeBridge
    PROPERTIES
    OUTPUT_NAME "LimelightNativeBridge"
)
