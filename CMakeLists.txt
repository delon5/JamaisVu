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

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O3 -nostdlib")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -fno-exceptions")

set(JamaisVu_payload_object "${CMAKE_CURRENT_BINARY_DIR}/payload/JamaisVu_payload.bin.o")
add_custom_command(OUTPUT ${JamaisVu_payload_object}
  COMMAND ${CMAKE_LINKER} -r -b binary -o ${JamaisVu_payload_object} payload/JamaisVu_payload.bin
  DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/payload/JamaisVu_payload.bin
  COMMENT "Building payload object"
)

add_executable(JamaisVu
  plugin/main.c
  plugin/fs.c
  plugin/resume.S
  ${JamaisVu_payload_object}
)

add_dependencies(JamaisVu JamaisVu_payload_binary)

target_link_libraries(JamaisVu
  SceCtrlForDriver_stub
  SceCpuForDriver_stub
  SceKernelSuspendForDriver_stub
  SceIofilemgrForDriver_stub
  ScePowerForDriver_stub
  SceRtcForDriver_stub
  SceSysclibForDriver_stub
  SceSysconForDriver_stub
  SceSysmemForDriver_stub
  SceSysrootForKernel_stub
  SceThreadmgrForDriver_stub
  taihenForKernel_stub
  taihenModuleUtils_stub
)

vita_create_self(JamaisVu.skprx JamaisVu CONFIG plugin/exports.yml UNSAFE)

add_custom_target(copy
  COMMAND cp JamaisVu.skprx F:/tai/JamaisVu.skprx
  DEPENDS JamaisVu.skprx
)
