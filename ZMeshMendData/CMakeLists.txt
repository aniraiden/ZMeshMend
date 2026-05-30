cmake_minimum_required(VERSION 3.16)
project(ZMeshMend_CGAL_Core VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# CGAL can be provided via vcpkg (recommended) or system install.
# When using vcpkg, pass -DCMAKE_TOOLCHAIN_FILE=C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
find_package(CGAL REQUIRED)
find_package(Eigen3 REQUIRED)
find_package(OpenMP)

add_executable(zmeshmend_core
    zmeshmend_core.cpp
    GoZ_Mesh.cpp
    GoZ_Utils.cpp
)

target_link_libraries(zmeshmend_core PRIVATE CGAL::CGAL Eigen3::Eigen)
if(OpenMP_CXX_FOUND)
    target_link_libraries(zmeshmend_core PRIVATE OpenMP::OpenMP_CXX)
endif()

if(MSVC)
    target_link_libraries(zmeshmend_core PRIVATE psapi)
    target_compile_options(zmeshmend_core PRIVATE /W3 /O2 /utf-8)
    set_target_properties(zmeshmend_core PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
else()
    target_compile_options(zmeshmend_core PRIVATE -Wall -O3 -DNDEBUG)
endif()

install(TARGETS zmeshmend_core RUNTIME DESTINATION .)
