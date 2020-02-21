cmake_minimum_required(VERSION 3.4.3)
project(llvm-ir-test-01)

find_package(LLVM REQUIRED CONFIG)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17")

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

set(SOURCES main.cpp)

add_executable(llvm-ir-test-01 ${SOURCES})

llvm_map_components_to_libnames(llvm_libs
    support
    core
    irreader
    x86codegen
    orcjit
    all
)
target_link_libraries(llvm-ir-test-01 ${llvm_libs})