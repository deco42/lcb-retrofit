cmake_minimum_required(VERSION 3.28)
project(lcbd VERSION 0.1.0 LANGUAGES C)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(LCBD_SANITIZE "build with -fsanitize=address,undefined" OFF)

add_compile_options(-Wall -Wextra -Wpedantic -ffp-contract=off)
add_compile_definitions(_GNU_SOURCE)
if(LCBD_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

find_package(Threads REQUIRED)

add_library(lcbcore STATIC
    src/config.c
    src/nlr.c
    src/sensorproto.c
    src/station.c
)

target_include_directories(lcbcore PUBLIC src)
target_link_libraries(lcbcore PUBLIC m)

# tests
enable_testing()
set(TESTDATA ${CMAKE_SOURCE_DIR}/testdata CACHE PATH "folder with the reference test data")

foreach(t config nlr sensorproto station)
    add_executable(test_${t} tests/test_${t}.c)
    target_link_libraries(test_${t} PRIVATE lcbcore)
    add_test(NAME unit_${t} COMMAND test_${t} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endforeach()
set_tests_properties(unit_nlr PROPERTIES ENVIRONMENT "LCBD_VECTORS=${TESTDATA}/cybelec_link.vectors.txt")
