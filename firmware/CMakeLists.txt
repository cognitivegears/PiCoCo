cmake_minimum_required(VERSION 3.20)
option(PICOCO_HOST "Build host library, tests and picoco-host instead of Pico firmware" OFF)
set(PICOCO_VERSION "1.1")
# PiCoCo board: "pico2" (Pico 2 / RP2350A, default) or "plusw" (Waveshare
# RP2350B-Plus-W). Picks both the PiCoCo pin header (boards/<x>.h) and, for
# plusw, the SDK's PICO_BOARD (boards/picoco_plusw.h, below).
set(PICOCO_BOARD "pico2" CACHE STRING "PiCoCo board: pico2 or plusw")

set(CORE_SRC
  src/dw/dw_util.c src/dw/dw_disk.c src/dw/dw_server.c src/dw/dw_vser.c
  src/bus/bus.c src/dev/device.c src/dev/rom.c src/dev/becker.c
  src/log.c src/console/console.c src/console/mode.c)
# dw_store_posix.c is the host filesystem backend (fopen/unistd); it also
# collides with newlib's own posix_close() declaration under arm-none-eabi.
# Host-only: added to picoco_core below, never to the picoco (Pico) target.
set(HOST_ONLY_SRC src/dw/dw_store_posix.c)

if(NOT PICOCO_HOST)
  if(PICOCO_BOARD STREQUAL "pico2_breadboard")
    # ponytail: legacy alias — PICOCO_BOARD used to hold the pin-header stem
    # directly (pre-Plus-W refactor); normalize an old cached build dir
    # instead of silently misreading it or forcing a clean reconfigure.
    set(PICOCO_BOARD "pico2" CACHE STRING "PiCoCo board: pico2 or plusw" FORCE)
  endif()
  if(PICOCO_BOARD STREQUAL "plusw")
    list(APPEND PICO_BOARD_HEADER_DIRS "${CMAKE_CURRENT_LIST_DIR}/boards")
    set(PICO_BOARD picoco_plusw CACHE STRING "" FORCE)
  elseif(PICOCO_BOARD STREQUAL "pico2")
    set(PICO_BOARD pico2 CACHE STRING "" FORCE)
  else()
    message(FATAL_ERROR "PICOCO_BOARD must be 'pico2' or 'plusw' (got '${PICOCO_BOARD}')")
  endif()
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
    src/plat_pico.c src/usb/usb_descriptors.c src/main.c
    src/crash.c src/bus/bus_core1.c)
  if(PICOCO_BOARD STREQUAL "plusw")
    set(PICOCO_PIN_HEADER plusw)
  else()
    set(PICOCO_PIN_HEADER pico2_breadboard)
  endif()
  target_include_directories(picoco PRIVATE src src/usb src/dw src/bus src/dev src/console boards third_party/fatfs)
  target_compile_options(picoco PRIVATE -Wall -Wextra)
  set_source_files_properties(${FATFS_SRC} PROPERTIES COMPILE_OPTIONS -w)
  target_compile_definitions(picoco PRIVATE
    PICOCO_BOARD_H="${PICOCO_PIN_HEADER}.h"
    PICOCO_LOG_LEVEL=3
    PICOCO_VERSION="${PICOCO_VERSION}"
    PICO_FLASH_ASSUME_CORE1_SAFE=1
    PICO_USE_STACK_GUARDS=1
    PICO_STACK_SIZE=0x1000
    PICO_PANIC_FUNCTION=picoco_panic
    PICO_PANIC_FUNCTION_DOES_NOT_RETURN=1
    CFG_TUSB_CONFIG_FILE="tusb_config.h")
  target_link_libraries(picoco
    pico_stdlib pico_unique_id pico_multicore
    hardware_flash hardware_sync hardware_watchdog
    tinyusb_device tinyusb_board)
  pico_add_extra_outputs(picoco)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    add_custom_command(TARGET picoco POST_BUILD
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_core1_flash_free.py
              $<TARGET_FILE:picoco> ${CMAKE_NM} ${CMAKE_OBJDUMP} ${CMAKE_CURRENT_SOURCE_DIR}/..
      COMMENT "Checking core1 (BUS_HOT) code stays flash-free"
      VERBATIM)
  else()
    message(WARNING "python3 not found; skipping the core1 flash-free check")
  endif()
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
