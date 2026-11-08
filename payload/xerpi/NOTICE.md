# Code from xerpi's vita-libbaremetal

`lcd.c` (except the parts marked "JamaisVu") comes from
[xerpi/vita-libbaremetal](https://github.com/xerpi/vita-libbaremetal):
the PCH-2000 LCD panel's set-up (its SPI command lists and I2C backlight
controller). `lcd.h`, `sysroot.h`, `utils.h` and `i2c.h` are JamaisVu's
own shims around it.

**vita-libbaremetal has no licence file.** This file is used with the
repository owner's decision to keep it in private builds only. It must be
removed (or permission obtained from xerpi) before this repository or its
builds are made public.

Everything else the display needs (the display link, the display
controller, the clocks, I2C, the PCH-1000 OLED panel and the PS TV's
HDMI bridge) is JamaisVu's own code in `payload/display/`, written from
the Vita developer wiki's register pages and the system's own register
setup read back on hardware. The 8x8 debug font there (`font_data.c`) is
from PSPSDK (BSD licence).

Without `lcd.c` the PCH-2000's screen would stay black while the payload
runs; saving and loading themselves do not depend on it.

Status (2026-11-08): the repository still has no licence (GitHub reports
none). Asking xerpi for one (e.g. MIT, like JamaisVu) is the clean way to
make public builds possible; a JamaisVu-own LCD file is the alternative.
