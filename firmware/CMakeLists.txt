cmake_minimum_required(VERSION 3.20)
option(PICOCO_HOST "Build host library, tests and picoco-host instead of Pico firmware" OFF)
set(PICOCO_VERSION "0.2-usb")

set(CORE_SRC
  src/dw/dw_util.c src/dw/dw_disk.c src/dw/dw_server.c
  src/bus/bus.c src/dev/device.c src/dev/rom.c src/dev/becker.c
  src/log.c src/console/console.c src/console/mode.c)
# dw_store_posix.c is the host filesystem backend (fopen/unistd); it also
# collides with newlib's own posix_close() declaration under arm-none-eabi.
# Host-only: added to picoco_core below, never to the picoco (Pico) target.
set(HOST_ONLY_SRC src/dw/dw_store_posix.c)

if(NOT PICOCO_HOST)
  set(PICO_BOARD pico2 CACHE STRING "")
  set(PICO_PLATFORM rp2350-arm-s CACHE STRING "")
  include(pico_sdk_import.cmake)
  project(picoco C CXX ASM)
  set(CMAKE_C_STANDARD 11)
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)     # -O2: keeps ring.h inlines inline
  endif()
  pico_sdk_init()
  set(FATFS_SRC third_party/fatfs/ff.c third_party/fatfs/ffunicode.c)
  add_executable(picoco ${CORE_SRC} ${FATFS_SRC}
    src/fs_flash.c src/dw/dw_store_fatfs.c
    src/plat_pico.c src/usb/usb_descriptors.c src/main.c)
  set(PICOCO_BOARD pico2_breadboard CACHE STRING "")
  target_include_directories(picoco PRIVATE src src/usb src/dw src/bus src/dev src/console boards third_party/fatfs)
  target_compile_options(picoco PRIVATE -Wall -Wextra)
  set_source_files_properties(${FATFS_SRC} PROPERTIES COMPILE_OPTIONS -w)
  target_compile_definitions(picoco PRIVATE
    PICOCO_BOARD_H="${PICOCO_BOARD}.h"
    PICOCO_LOG_LEVEL=3
    PICOCO_VERSION="${PICOCO_VERSION}"
    PICO_FLASH_ASSUME_CORE1_SAFE=1
    CFG_TUSB_CONFIG_FILE="tusb_config.h")
  target_link_libraries(picoco
    pico_stdlib pico_unique_id pico_multicore
    hardware_flash hardware_sync hardware_watchdog
    tinyusb_device tinyusb_board)
  pico_add_extra_outputs(picoco)
  return()
endif()

project(picoco_host C)
set(CMAKE_C_STANDARD 11)
add_compile_options(-Wall -Wextra -g -fsanitize=address,undefined -fno-omit-frame-pointer)
add_link_options(-fsanitize=address,undefined)
add_compile_definitions(PICOCO_HOST=1 PICOCO_LOG_LEVEL=3 PICOCO_VERSION="${PICOCO_VERSION}")

add_library(picoco_core STATIC ${CORE_SRC} ${HOST_ONLY_SRC} host/plat_host.c)
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
