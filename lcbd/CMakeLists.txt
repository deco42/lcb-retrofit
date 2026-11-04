cmake_minimum_required(VERSION 3.28)
project(lcbd VERSION 0.1.0 LANGUAGES C)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

include(FetchContent)
if(NOT SOEM_URL)
    set(SOEM_URL https://github.com/OpenEtherCATsociety/SOEM/archive/refs/tags/v2.0.1.tar.gz)
endif()
FetchContent_Declare(soem
    URL ${SOEM_URL}
    URL_HASH SHA256=933208261f1737ae530d25b915c1ce459413dcdba13d92e95af3726380601648
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(soem)

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
    src/evaluator.c
    src/io.c
    src/control.c
    src/sequences.c
    src/sim.c
    src/httputil.c
)

target_include_directories(lcbcore PUBLIC src)
target_link_libraries(lcbcore PUBLIC m)

add_library(lcbecat STATIC src/ecat.c)
target_include_directories(lcbecat PUBLIC src)
target_link_libraries(lcbecat PRIVATE soem PUBLIC Threads::Threads)

file(GLOB WEB_FILES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/web/*)
string(JOIN "|" WEB_FILE_LIST ${WEB_FILES})
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/web_assets.c
    COMMAND ${CMAKE_COMMAND} -DOUT=${CMAKE_BINARY_DIR}/web_assets.c -DFILES=${WEB_FILE_LIST}
            -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS ${WEB_FILES} ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
    COMMENT "embedding web/ into the binary"
    VERBATIM)

add_executable(lcbd
    src/main.c
    src/shared.c
    src/log.c
    src/persist.c
    src/hal.c
    src/link.c
    src/sensor_client.c
    src/iothread.c
    src/web.c
    ${CMAKE_BINARY_DIR}/web_assets.c
)

add_executable(lcb_test src/lcb_test.c)
target_link_libraries(lcb_test PRIVATE lcbecat)

add_executable(lcb_replay tools/replay.c)
target_link_libraries(lcb_replay PRIVATE lcbcore)

target_link_libraries(lcbd PRIVATE lcbcore lcbecat Threads::Threads)

install(TARGETS lcbd lcb_test RUNTIME DESTINATION bin)

# tests
enable_testing()
set(TESTDATA ${CMAKE_SOURCE_DIR}/../testdata CACHE PATH "folder with the reference test data")

foreach(t config nlr sensorproto station evaluator io sequences httputil frames)
    add_executable(test_${t} tests/test_${t}.c)
    target_link_libraries(test_${t} PRIVATE lcbcore)
    add_test(NAME unit_${t} COMMAND test_${t} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endforeach()
set_tests_properties(unit_nlr PROPERTIES ENVIRONMENT "LCBD_VECTORS=${TESTDATA}/cybelec_link.vectors.txt")

set(HAVE_RECORDINGS FALSE)
if(EXISTS ${TESTDATA}/bend_090.frames)
    set(HAVE_RECORDINGS TRUE)
else()
    message(STATUS "sensor recordings not found in ${TESTDATA}: frame and replay tests skipped")
endif()

if(HAVE_RECORDINGS)
    add_test(NAME vectors_bend_090 COMMAND test_frames ${TESTDATA}/bend_090.frames ${TESTDATA}/bend_090.expected.csv --time)
    add_test(NAME vectors_bend_135 COMMAND test_frames ${TESTDATA}/bend_135.frames ${TESTDATA}/bend_135.expected.csv --time)
    add_test(NAME vectors_synthetic COMMAND test_frames ${TESTDATA}/synthetic.frames ${TESTDATA}/synthetic.expected.csv --min 99.9 --time)
endif()
