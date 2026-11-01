/* Added to the VitaSDK default linker script (INSERT), not a replacement.
 *
 * vita-elf-create puts its SCE data (module info, import/export tables,
 * ~1.8 KB) right after segment 0 (.text/.rodata) and needs that room
 * before segment 1 (.data). The default script starts .data at the next
 * 4 KB page, so the room left depends on the code size: at build 7a9d18f
 * .rodata ended at 0x8102FA61, leaving 1439 bytes, and the build failed
 * ("Cannot allocate 1780 bytes for SCE data at end of segment 0").
 * Moving .data one page further always leaves at least 4 KB. */
SECTIONS
{
  . = . + 0x1000;
}
INSERT BEFORE .data;
