# jvdump

A small, read-only kernel plugin that collects what is needed to add a firmware (e.g. 3.60) to JamaisVu's offset table. It does not hook or patch anything.

## Use

1. Build it (`mkdir build && cd build && cmake .. && make`) or use a prebuilt `jvdump.skprx`. Copy it to `ux0:tai/jvdump.skprx`.
2. Add it under `*KERNEL` in `ux0:tai/config.txt`:

   ```
   *KERNEL
   ux0:tai/jvdump.skprx
   ```

3. Reboot. Wait about 30 seconds after the LiveArea appears, until `ux0:data/JamaisVu/dump/done.txt` exists (check with VitaShell).
4. Remove the line from `config.txt` again and reboot.
5. Copy the whole `ux0:data/JamaisVu/dump/` folder to your PC and send it over. It is about 1 MB.

## What it writes

- `<Module>_<NID>_seg0.bin`: code, `seg1.bin`: data, for SceExfatfs, SceIofilemgr, SceRtc, SceSysmem and SceCtrl.
- `info.txt`: module NIDs and the address and size of every segment.

The data segments are a snapshot of live kernel memory, so they contain kernel addresses from that boot. Keep the dump to yourself and the people working on the port. Sony's code is copyrighted; it is only needed to locate functions and offsets.
