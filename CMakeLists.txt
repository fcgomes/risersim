cmake_minimum_required(VERSION 3.18)
project(riserSim VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC Static Runtime Library (/MT)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Disable Eigen tests/docs
set(EIGEN_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(EIGEN_BUILD_DOC OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

# 1. Fetch Eigen 3.4.0 (Header-only matrix library)
include(FetchContent)
message(STATUS "Fetching Eigen 3.4.0...")
FetchContent_Declare(
    Eigen3
    URL https://gitlab.com/libeigen/eigen/-/archive/3.4.0/eigen-3.4.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(Eigen3)

# Core C++ Library
add_library(risersim_core STATIC
    src/node.cpp
    src/element_beam.cpp
    src/solver.cpp
    src/hydrodynamics.cpp
)

target_include_directories(risersim_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# 2. Check or Fetch HDF5 C/C++ library
find_package(HDF5 COMPONENTS C CXX QUIET)

if(HDF5_FOUND)
    message(STATUS "✅ HDF5 C/C++ Library found on system!")
    add_compile_definitions(RISERSIM_HAS_HDF5)
else()
    message(STATUS "ℹ️ HDF5 library not found locally. Fetching HDF5 source via CMake...")
    set(HDF5_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(HDF5_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(HDF5_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(HDF5_BUILD_CPP_LIB ON CACHE BOOL "" FORCE)
    set(HDF5_BUILD_HL_LIB ON CACHE BOOL "" FORCE)

    FetchContent_Declare(
        hdf5
        URL https://github.com/HDFGroup/hdf5/archive/refs/tags/hdf5-1_14_3.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(hdf5)
    if(TARGET hdf5_cpp)
        add_compile_definitions(RISERSIM_HAS_HDF5)
    endif()
endif()

# Executable test
add_executable(risersim_test_main src/main_test.cpp src/node.cpp src/element_beam.cpp src/solver.cpp src/hydrodynamics.cpp)
target_include_directories(risersim_test_main PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(risersim_test_main PUBLIC Eigen3::Eigen)

if(TARGET hdf5_cpp-static)
    target_link_libraries(risersim_test_main PRIVATE hdf5_cpp-static hdf5-static)
    add_compile_definitions(RISERSIM_HAS_HDF5)
elseif(TARGET hdf5_cpp)
    target_link_libraries(risersim_test_main PRIVATE hdf5_cpp hdf5)
    add_compile_definitions(RISERSIM_HAS_HDF5)
elseif(HDF5_FOUND)
    target_include_directories(risersim_test_main PRIVATE ${HDF5_INCLUDE_DIRS})
    target_link_libraries(risersim_test_main PRIVATE ${HDF5_LIBRARIES})
    add_compile_definitions(RISERSIM_HAS_HDF5)
endif()

# 2. Optional pybind11 for Python API Bindings (import risersim)
find_package(Python3 COMPONENTS Interpreter Development)
if(Python3_FOUND)
    message(STATUS "Python3 found: ${Python3_EXECUTABLE}. Fetching pybind11...")
    FetchContent_Declare(
        pybind11
        URL https://github.com/pybind/pybind11/archive/refs/tags/v2.11.1.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(pybind11)

    pybind11_add_module(risersim src/bindings.cpp)
    target_link_libraries(risersim PRIVATE risersim_core)
else()
    message(STATUS "Python3 not found. Skipping pybind11 module build.")
endif()
