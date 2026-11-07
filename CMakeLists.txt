cmake_minimum_required(VERSION 2.8)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

add_subdirectory(payload)

project(JamaisVu C ASM)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu17 -Wl,-q -Wall -O3 -nostdlib")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -fno-exceptions")

set(JamaisVu_payload_object "${CMAKE_CURRENT_BINARY_DIR}/payload/JamaisVu_payload.bin.o")
add_custom_command(OUTPUT ${JamaisVu_payload_object}
  COMMAND ${CMAKE_LINKER} -r -b binary -o ${JamaisVu_payload_object} payload/JamaisVu_payload.bin
  DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/payload/JamaisVu_payload.bin
  COMMENT "Building payload object"
)

# No NEON vector code in the kernel plugin: in kernel context the Vita runs
# it far slower than plain instructions (6ecd5fa "row cost" line, PS TV at
# 1080p: the same loop 64 us a row vectorized, 19 us without; the menu's
# compose was 29 ms a frame). The payload (payload/CMakeLists.txt) keeps its
# own flags.
add_compile_options(-fno-tree-vectorize -fno-tree-slp-vectorize)

add_executable(JamaisVu
  plugin/main.c
  plugin/fs.c
  plugin/fs_policy.c
  plugin/offsets.c
  plugin/slots.c
  plugin/md5.c
  plugin/remap.c
  plugin/lockskip.c
  plugin/appdb.c
  plugin/slot_policy.c
  plugin/menu.c
  plugin/storage.c
  plugin/trace.c
  plugin/hwdump.c
  plugin/gui/render.c
  plugin/gui/rle16.c
  plugin/gui/menu_view.c
  plugin/gui/logo.c
  plugin/gui/font_data.c
  plugin/gui/jpfont.c
  payload/crc32.c
  plugin/resume.S
  ${JamaisVu_payload_object}
)

# Set -DJAMAISVU_ALLOW_UNVERIFIED_FW=ON to arm on firmware whose offsets
# have not been verified on hardware (see plugin/offsets.c). Testing only.
option(JAMAISVU_ALLOW_UNVERIFIED_FW "Arm on unverified firmware" OFF)
if(JAMAISVU_ALLOW_UNVERIFIED_FW)
  target_compile_definitions(JamaisVu PRIVATE JAMAISVU_ALLOW_UNVERIFIED_FW)
endif()

# Set -DJAMAISVU_TESTING=ON to also arm on firmware whose offsets were
# checked against a module dump but not yet tested on hardware (3.60).
option(JAMAISVU_TESTING "Arm on dump-verified firmware (testing build)" OFF)

# Short commit id, shown in status.txt so a report names the exact build
execute_process(COMMAND git rev-parse --short=7 HEAD
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                OUTPUT_VARIABLE JV_BUILD_ID
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT JV_BUILD_ID AND DEFINED ENV{GITHUB_SHA})
  string(SUBSTRING "$ENV{GITHUB_SHA}" 0 7 JV_BUILD_ID)
endif()
if(NOT JV_BUILD_ID)
  set(JV_BUILD_ID "unknown")
endif()
target_compile_definitions(JamaisVu PRIVATE JV_BUILD_ID="${JV_BUILD_ID}")
if(JAMAISVU_TESTING)
  target_compile_definitions(JamaisVu PRIVATE JAMAISVU_TESTING)
endif()

add_dependencies(JamaisVu JamaisVu_payload_binary)

# room for vita-elf-create's SCE data between the segments (see the file)
target_link_options(JamaisVu PRIVATE
  "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/plugin/sce_gap.ld")

target_link_libraries(JamaisVu
  SceCtrlForDriver_stub
  SceCpuForDriver_stub
  SceDisplayForDriver_stub
  SceKernelSuspendForDriver_stub
  SceIofilemgrForDriver_stub
  ScePowerForDriver_stub
  SceRegMgrForDriver_stub
  SceRtcForDriver_stub
  SceSblAIMgrForDriver_stub
  SceSblACMgrForDriver_stub
  SceSysclibForDriver_stub
  SceSysconForDriver_stub
  SceSysmemForDriver_stub
  SceSysrootForKernel_stub
  SceSysrootForDriver_stub
  SceAppMgrForDriver_stub
  SceThreadmgrForDriver_stub
  taihenForKernel_stub
  taihenModuleUtils_stub
)

vita_create_self(JamaisVu.skprx JamaisVu CONFIG plugin/exports.yml UNSAFE)

add_custom_target(copy
  COMMAND cp JamaisVu.skprx F:/tai/JamaisVu.skprx
  DEPENDS JamaisVu.skprx
)
