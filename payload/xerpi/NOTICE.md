# Code from xerpi's vita-libbaremetal

The files in this folder (except `display_test.c`, `sysroot.h` and the
parts marked "JamaisVu") come from
[xerpi/vita-libbaremetal](https://github.com/xerpi/vita-libbaremetal):
display link (DSI), display controller (IFTU), PCH-2000 LCD, I2C, and the
8x8 debug font (which itself is from PSPSDK, BSD licence).

**vita-libbaremetal has no licence file.** This folder is used with the
repository owner's decision to keep it in private builds only. It must be
removed (or permission obtained from xerpi) before this repository or its
builds are made public.

It is only used by the Diagnostics "Display test"; normal saves and loads
do not call it.
