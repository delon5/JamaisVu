cmake_minimum_required(VERSION 2.8)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(JamaisVu_payload C ASM)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu17 -Wl,-q -Wall -O0 -mcpu=cortex-a9 -mthumb-interwork -mno-unaligned-access -fno-builtin -Wno-main")
# -mno-unaligned-access: the payload runs with the MMU off, so all memory is
# strongly-ordered and every misaligned access faults (ARMv7). Without it GCC
# reads packed fields (MBR partition table) and merged byte loads (FatFs)
# with LDR at odd addresses: data abort seen on 3.60 hardware (75f1aaf,
# fault address 0x1C00ED21 in trace_locate).
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -fno-exceptions")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -T ${CMAKE_CURRENT_SOURCE_DIR}/linker.x -nostartfiles -nodefaultlibs -nostdlib")

include_directories(
  ./
  msif
  FatFs
  ../plugin/gui
)

set(JamaisVu_payload_sources
  start.S
  main.c
  state.c
  guard.c
  crc32.c
  gpio.c
  pervasive.c
  spi.c
  syscon.c
  libc/qsort.c
  libc/string.c
  msif/ms.c
  msif/msif.c
  msif/msif_auth.c
  FatFs/diskio.c
  FatFs/ff.c
  FatFs/ffsystem.c
  FatFs/ffunicode.c
  # Diagnostics "Display test" only; from xerpi's vita-libbaremetal (no
  # licence: private builds only, see xerpi/NOTICE.md)
  xerpi/display_test.c
  xerpi/pervasive_display.c
  xerpi/dsi.c
  xerpi/iftu.c
  xerpi/lcd.c
  xerpi/i2c.c
  xerpi/utils.c
  xerpi/font_data.c
  # the menu's renderer, font and logo, for the progress screen
  ../plugin/gui/render.c
  ../plugin/gui/menu_view.c
  ../plugin/gui/font_data.c
  ../plugin/gui/jpfont.c
)

# Not -mgeneral-regs-only: on 3.60 hardware that build never reached the
# LED (A/B test, build ee965f0), while the default build does, and the FPU
# is usable at that point (syscon_common_write uses NEON before the LED).
add_executable(JamaisVu_payload ${JamaisVu_payload_sources})
target_link_libraries(JamaisVu_payload gcc)
add_custom_command(TARGET JamaisVu_payload POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -S -O binary JamaisVu_payload JamaisVu_payload.bin
)

add_custom_target(JamaisVu_payload_binary DEPENDS JamaisVu_payload)
