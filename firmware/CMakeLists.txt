cmake_minimum_required(VERSION 3.20)
option(PICOCO_HOST "Build host library, tests and picoco-host instead of Pico firmware" OFF)

if(NOT PICOCO_HOST)
  # Pico branch is completed and verified in Plan B. Kept minimal here.
  set(PICO_BOARD pico2 CACHE STRING "")
  include(pico_sdk_import.cmake)
  project(picoco C CXX ASM)
  pico_sdk_init()
  add_executable(picoco src/main.c)
  target_link_libraries(picoco pico_stdlib)
  pico_add_extra_outputs(picoco)
  return()
endif()

project(picoco_host C)
set(CMAKE_C_STANDARD 11)
add_compile_options(-Wall -Wextra -g -fsanitize=address,undefined -fno-omit-frame-pointer)
add_link_options(-fsanitize=address,undefined)
add_compile_definitions(PICOCO_HOST=1 PICOCO_LOG_LEVEL=3)

set(CORE_SRC
  src/dw/dw_util.c src/dw/dw_disk.c src/dw/dw_store_posix.c src/dw/dw_server.c
  src/bus/bus.c src/dev/device.c src/dev/rom.c src/dev/becker.c
  src/log.c src/console/console.c src/console/mode.c)
# Only files that exist are compiled, so early tasks build.
set(CORE_EXISTING)
foreach(f ${CORE_SRC})
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${f})
    list(APPEND CORE_EXISTING ${f})
  endif()
endforeach()
add_library(picoco_core STATIC ${CORE_EXISTING} host/plat_host.c)
target_include_directories(picoco_core PUBLIC src src/dw src/bus src/dev src/console host)

enable_testing()
file(GLOB TEST_SRCS tests/test_*.c)
foreach(t ${TEST_SRCS})
  get_filename_component(name ${t} NAME_WE)
  add_executable(${name} ${t})
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/host/sim_bus.c)
    target_sources(${name} PRIVATE host/sim_bus.c)
  endif()
  target_link_libraries(${name} picoco_core)
  target_include_directories(${name} PRIVATE tests)
  target_compile_definitions(${name} PRIVATE FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  add_test(NAME ${name} COMMAND ${name})
endforeach()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/host/picoco_host.c)
  add_executable(picoco-host host/picoco_host.c)
  target_link_libraries(picoco-host picoco_core)
endif()
