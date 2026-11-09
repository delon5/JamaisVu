# Why dejavu works on 3.65 but not 3.60, and how to port it

> **Status: supported and hardware-tested.** The 3.60 offset table is complete, every offset was checked against a dump of a retail 3.60 console (details below), and a dry run, a save and a load (including the ux0 remount) passed on a PCH-2000 with an official Memory Card as ux0. 3.60 is enabled in the normal build (`JamaisVu.skprx`); it only arms if SceExfatfs is module NID `0x23AA294A`.
>
> Getting there on hardware took three payload fixes, each found from logs: crash recovery called ARM code from Thumb with a plain `BL`; `-mgeneral-regs-only` (added on a wrong theory) kept the payload from even reaching the LED; and with the MMU off every misaligned access faults, so the payload is built with `-mno-unaligned-access` (GCC had used word loads for the packed MBR partition table and merged FatFs byte loads).
>
> | Module (3.60) | NID | Result |
> | --- | --- | --- |
> | SceExfatfs | `0x23AA294A` | All 13 offsets of 3.65 land on the same functions in 3.60: identical prologues; `sceExfatfsVopUmount` references the shared resource at data+0x80; `UVFAT_ReleaseFnode` reads fnode+0x20c; the patch site 0x140fe is the same `bne.w`. The live shared resource decodes with the 3.65 layout: 0x100 file caches, file cache pointer at +0x5cd0, 0x400 fnodes of 0x290 bytes at +0x5cd8 with real paths, fileSystems every 0x430 bytes. |
> | SceIofilemgr | `0x9642948C` | `clear_ncache` 0x5a39 and `invalidate_buff_cache` 0x126f9 are function starts. The vnode loop at text+0xcf04 reads the count at data+0x24, the array at data+0x19dc and the size at data+0x19e0 (values 0x400 / heap pointer / 0x100). |
> | SceRtc | `0x1E923AD1` | Data segment is exactly the 0x1c0-byte RTC context at offset 0. |
> | SceIofilemgr mount table | `0x9642948C` | `sceIoFindMountPoint` at text+0x138c1 (as used by StorageMgr) loops over 32 entries of 0x38 bytes at data+0x1a90 comparing the id. Decoding the table from the dump gives xmc0 → `sdstor0:xmc-lp-ign-userext`, gro0/grw0 → `sdstor0:gcd-…`, imc0 → `sdstor0:int-lp-ign-userext`, with ux0/uma0 remapped by a storage plugin. JamaisVu uses it (read-only) to find the official Memory Card. |

## The cause

Most of what the plugin does goes through **NID exports**: named, numbered functions that every firmware provides at whatever address they live. Those work on 3.60 and 3.65 alike. This includes the syscon reset hook, the system event handler, file I/O, the controller and the display. The plugin already looked up both the 3.60 and 3.65 variants of the two SceSysmem functions whose NIDs changed in 3.63.

Loading a state, however, needs to reach **inside the exFAT driver (SceExfatfs) and the I/O manager (SceIofilemgr)**. It has to flush caches and remount ux0 after the memory image is replaced. None of these internals are exported, so the original plugin used **hard-coded offsets** into those modules:

| What | Original code | 3.60 handling in the original |
| --- | --- | --- |
| 13 SceExfatfs function offsets, the `UVFAT_WriteDevice` hook, a 4-byte NOP patch, the shared-resource global | `patch_exfatfs()` | **none: 3.65 values used unconditionally** |
| SceIofilemgr `clear_ncache`, `invalidate_buff_cache` | `get_iofilemgr_offsets()` | 3.60 values present ✔ |
| SceIofilemgr vnode table globals (`0x24`, `0x19dc`, `0x19e0`) | `get_iofilemgr_offsets()` | **3.65 values used unconditionally** |
| SceCtrl callback pointer (`0xabc`) | `patch_ctrl()` | **3.65 value used unconditionally** |
| exFAT structure layout (`0x295d5c`, `0x5cd0`, `0x290`, `0x430`, …) | `fs.c` | **3.65 layout assumed** |

So on 3.60 the original plugin:

1. hooked and NOP-patched **random instructions inside the 3.60 exFAT driver**,
2. overwrote a random word in SceCtrl's data, so the button combination never worked, or crashed, and
3. walked the exFAT driver's memory with the wrong structure layout during a load.

Item 1 alone explains "works on 3.65, not 3.60". It is also a Memory Card corruption risk in its own right, because it modifies the filesystem driver's code.

## What has changed in JamaisVu

- **The controller is firmware-independent now.** The menu polls `ksceCtrlPeekBufferPositive` and masks game input by hooking the `sceCtrl*` syscall exports by NID, so there is no SceCtrl offset any more.
- **The display overlay is firmware-independent** (`ksceDisplaySetFrameBufInternal`, by NID).
- **Every remaining offset is in one table**, `plugin/offsets.c`, keyed by the SceIofilemgr module NID. The plugin refuses to arm (no hooks, no patches, nothing changed) unless the running firmware has a complete entry that is marked as verified.
- **The 3.60 entry exists**, with the two known SceIofilemgr offsets filled in. Everything else is `0` (= unknown), so 3.60 is currently disabled.

## What is needed for 3.60

Fill in the zero fields of the `"3.60"` entry in `plugin/offsets.c`:

| Field | Module / segment | 3.65 value | Function or meaning |
| --- | --- | --- | --- |
| `iofilemgr_num_vnodes` | SceIofilemgr, data | `0x24` | number of vnodes |
| `iofilemgr_vnodes` | SceIofilemgr, data | `0x19dc` | pointer to the vnode array |
| `iofilemgr_vnode_size` | SceIofilemgr, data | `0x19e0` | size of one vnode |
| `exfat_mount` | SceExfatfs, text | `0x7169` | `UVFAT_Mount` |
| `exfat_search_file_common` | SceExfatfs, text | `0x10e39` | `UVFAT_SearchFile_common` |
| `exfat_get_parent_fnode` | SceExfatfs, text | `0x7a79` | `UVFAT_GetParentFnode` |
| `exfat_release_fnode` | SceExfatfs, text | `0x7ae9` | `UVFAT_ReleaseFnode` |
| `exfat_cleanup_cluster_cache` | SceExfatfs, text | `0x2695` | `cleanup_cluster_cache_area` |
| `exfat_invalidate_cluster_cache` | SceExfatfs, text | `0x2191` | `invalidate_cluster_cache_data` |
| `exfat_free_fp_cache` | SceExfatfs, text | `0x7ebd` | `FreeFpCache` |
| `exfat_sfree` | SceExfatfs, text | `0x68a5` | `sfree` |
| `exfat_write_device` | SceExfatfs, text | `0x1315c` | `UVFAT_WriteDevice` (hooked; even = ARM/Thumb flag handled by taiHEN) |
| `exfat_read_mgmt_patch` | SceExfatfs, text | `0x140fe` | the 4-byte check inside `UVFAT_ReadManagementArea` that is replaced with two Thumb NOPs so the management area can be re-read on remount |
| `exfat_shared_resource` | SceExfatfs, data | `0x80` | the driver's shared resource pointer |
| `rtc_ctx` | SceRtc, data | `0x0` | RTC context (`0x1c0` bytes). May well be `0` on 3.60 too, but must be confirmed |
| `sr_num_files`, `sr_files`, `sr_num_fnodes`, `sr_fnodes`, `fnode_size`, `fs_stride`, `fs_count` | exFAT structures | see table | shared-resource layout; the offsets inside fnode/fileSystem/drive used in `plugin/fs.c` (`0x20c`, `0xac`, `0x1e0`, `0x280`…) must also be confirmed |

### How to find them

1. On your 3.60 Vita, run the read-only dump plugin [`tools/jvdump`](../tools/jvdump/README.md). It writes the loaded (already decrypted) code and data segments of SceExfatfs, SceIofilemgr, SceRtc, SceSysmem and SceCtrl, plus their module NIDs and segment sizes, to `ux0:data/JamaisVu/dump/`.
2. Optional but helpful: the same dump from a **3.65** console, so the two versions can be diffed directly. Without it, the 3.60 functions are identified from what each one does, using the 3.65 offsets and `plugin/fs.c` as the reference.
3. Load both versions in Ghidra or IDA with a Vita loader. For every 3.65 offset above, find the same function in 3.60, usually by matching:
   - calls to the same exported functions (e.g. `ksceKernelLockFastMutex` on `drive + 0x1e0`),
   - error constants and string references,
   - basic-block structure (BinDiff / Ghidra Version Tracking work well, since 3.60 → 3.65 changed little in these modules).
4. Check the structure offsets used in `plugin/fs.c` at the matched call sites.
5. Put the values in the 3.60 entry and build with `-DJAMAISVU_ALLOW_UNVERIFIED_FW=ON`. Test on a console with a **backed-up / spare Memory Card**:
   - save → load in the same game,
   - load after creating and deleting files,
   - load after rebooting,
   - load from the LiveArea.

   Run `fsck` on the card from a PC after each test.
6. When it passes, set `.verified = 1` for 3.60.

If you can share the decrypted 3.60 and 3.65 modules (or a project already annotated for them), the matching in step 3 can be done for you. Without the binaries the offsets cannot be derived, because they come from Sony's compiled code.
