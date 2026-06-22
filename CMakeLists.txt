cmake_minimum_required(VERSION 3.10)
project(MCPT)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(mcpt main.cpp mcpt.cpp)

# Test executables
add_executable(test_regime tests/test_regime.cpp)
target_include_directories(test_regime PRIVATE ${CMAKE_SOURCE_DIR}/shared)

add_executable(test_kelly_sizing tests/test_kelly_sizing.cpp)

add_executable(test_mcpt_example mcpt_example.cpp mcpt.cpp)
