# Writes payload_id.h (size, MD5 and file name of the payload the plugin
# was built with) and copies the payload next to it under that name:
#   cmake -DIN=<JamaisVu_payload.bin> -DOUTDIR=<dir> -P payload_id.cmake
# The name carries the first 8 digits of the MD5, so payloads of different
# builds can lie side by side on the Memory Card and each plugin build
# finds its own (see plugin/menu.c, payload_get).
file(MD5 "${IN}" H)
file(READ "${IN}" X HEX)
string(LENGTH "${X}" L)
math(EXPR N "${L} / 2")
string(SUBSTRING "${H}" 0 8 P)
string(REGEX REPLACE "(..)" "0x\\1," BYTES "${H}")
set(NAME "JamaisVu_payload_${P}.bin")
file(WRITE "${OUTDIR}/payload_id.h"
  "#define JV_PAYLOAD_SIZE ${N}u\n#define JV_PAYLOAD_MD5 { ${BYTES} }\n#define JV_PAYLOAD_FILE \"${NAME}\"\n")
execute_process(COMMAND ${CMAKE_COMMAND} -E copy "${IN}" "${OUTDIR}/${NAME}")
message(STATUS "payload: ${NAME}, ${N} bytes, MD5 ${H}")
