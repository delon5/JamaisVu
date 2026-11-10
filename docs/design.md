# JamaisVu design notes

How the menu is drawn, and how saving and loading are kept from damaging the Memory Card. The user-facing summary is in the [README](../README.md).

## How the menu is drawn

In a game, while the game's own frames are on the screen and the system has not shown anything on the plane for two seconds (its all-transparent refreshes, which some games trigger a few times a second, do not count and are kept off the plane while the menu is up), the menu lives on the display's second plane - the one the system uses for its own overlays - as a single 960x544 picture in VRAM that the display hardware blends over the game's frames (scaled to the output, 1080p on the PS TV included). Nothing is composed into the game's frames, the game cannot draw over it, and the menu needs only ~250 KB of kernel memory (thumbnails and a drawing band), so it opens in games that leave the kernel almost nothing. The 2 MB VRAM buffer is taken when the menu first opens in a game and given back once the system puts its own picture on the plane again (its overlays, every app switch), so it is never held across games; a game that leaves no 2 MB block gets the composed menu instead; the plane is claimed while the menu or a message is shown and handed back to the system afterwards. If the system puts its own overlay on the plane meanwhile (PS button menu, a notification), the menu closes rather than draw over it. In the LiveArea that plane is the LiveArea's own picture, so there the menu is composed into the frames as before.

When the menu is composed into the frames (the LiveArea, a game that leaves no 2 MB of video memory, an app that is not a game process), the stored menu image is compressed losslessly: flat areas and text as runs, the thumbnails by reference to the copies already in memory, about 50 KB instead of 516 KB, pixel for pixel the same (checked by the host tests). It grows in 64 KB pieces, two copies alternating so the one on screen is never written while it is read, and the composed menu needs about 300 KB in all instead of about 850 KB. The log says how much each menu took (`menu image: … KB compressed`).

To force the composed menu on a console - for comparison, or if the plane misbehaves with some setup - create an empty file `data/JamaisVu/noplane` on the Memory Card. The log notes it once (`overlay plane: off`); remove the file to go back.

## How it keeps the Memory Card safe

The original README warned that *"your SD Card and/or Memory Card may be corrupted"*. This version addresses each known cause.

**Saving cannot change the filesystem.**
- The slot file is created and sized by the Vita's own exFAT driver *before* the snapshot, so all filesystem metadata is written by the OS.
- The payload looks up exactly which sectors belong to that file.
- A sector write guard (`payload/guard.c`) blocks every write outside those sectors before it reaches the card driver.

**An interrupted save is never loadable.**
- The header sector is cleared first and written last, with CRCs over the header and all metadata.
- A slot only counts as used once its `.jvi` info file is written, which happens after the save succeeds.

**An incomplete snapshot is never taken.** If the list of running processes cannot be read in full at suspend time (more than 32, or a process context the kernel will not give), the payload refuses the save and the menu says so; nothing is written.

**Nothing is written until the whole file has been checked.** Before a load touches the card, it verifies:
- the header CRC and body CRC
- every savedata path (only `ux0:user/00/savedata/<TITLEID>/…` or `ux0:resavedata/<TITLEID>/…`; no `..`)
- every memory segment (only DRAM/VRAM and the two known system regions)
- that the state was made on the same Memory Card

A corrupted or tampered state is rejected with nothing changed.

**The restored memory is checked before the system runs on it.** A save stores a checksum of the memory contents in the slot (slot format 4; slots saved by earlier builds have none and load as before). A load compares it with the memory it has just restored. If they differ, the payload stops with the LED blinking, as after a read error, instead of resuming a system whose memory is not what was saved.
- The payload runs with the data cache off, where a pass over every byte of the image takes minutes. So the checksum samples one 32-bit word in every 64 bytes, eight per sector, in order. That catches a sector that is missing, stale, misplaced or from another file. A single flipped bit in a word that is not sampled is left to the card's own error correction and the bus CRC, as before.
- `log.txt` records it on every run (`memory image checksum: …`), with the time the sums took.

**Load writes are fenced.**
- Savedata restore can only write inside ux0's exFAT area, never the partition table or boot sectors.
- The mediaid sector is restored only to the sector it came from, and can no longer fall back to sector 0 (the MBR), which the original could.

**A power cut during a load cannot leave the card unsafe.** Putting the savedata back is the one step that changes the file system itself (through FatFs), so it is ordered to survive losing power after any single write:
- A file that is cut back gets its directory entry shortened before its clusters are freed. The other way round, which FatFs does by default, leaves for a moment an entry that points at free clusters, which the console could then give to another file.
- A directory entry that lies across two sectors takes two writes. A new one is written so that it only exists with the last write. An existing one is marked unused while its two sectors differ, like a removed file, and is back with the last write. The drive never holds an entry with a wrong checksum.
- What a cut can leave is therefore limited to clusters that are allocated and unused, and a file that is hidden for the moment. Loading the slot again finishes the job: before it reads a savedata folder, the load brings hidden entries back and clears leftovers (`f_fixdir` in `payload/FatFs/ff.c`), and it frees clusters still chained behind a file's end.
- `tests/host/test_state.c` proves this by brute force. It cuts the restore after every single card write in turn, on three cluster sizes and with the folder's entries shifted through all sixteen positions in a sector, runs `fsck.exfat` on each card, and loads the slot again. Over 2,300 cut points, none leaves anything unsafe and every one is finished by the second load.

**The one raw write is checked on both sides.** The step record of a run lives in one sector of `data/JamaisVu/trace.bin`, which the payload and, after the wake-up, the plugin write through the card's raw device. The payload only writes it after finding this run's signature in that sector. After the wake-up the plugin reads the sector back first as well, and writes only if the signature is still there; otherwise nothing is written for that run and the log says so.

**The restored OS cannot write with stale caches.** The payload arms a write block *inside the restored memory image*. The plugin re-reads every exFAT drive from its storage before writes are allowed again. If a file that was open at snapshot time no longer exists, **that drive** stays **read-only until restart** rather than risk writing to the wrong clusters. The other drives, including the system's own internal storage, stay writable. (Blocking writes on every drive, as an earlier version did, left the system unable to write anything and made it restart.)

- Each entry of the kernel's open-file table is decided under its own lock, read afresh: an entry the kernel is reclaiming or setting up at that moment, or one that cannot be locked, is left to the kernel. A file that is gone is freed only when nothing holds it open; a drive whose lookup could not be made is never counted as changed.
- Entries nothing holds open (the kernel's name cache for files the game opened earlier - ~950 of 1024 in GTA: San Andreas) are given back under the kernel reclaim's own conditions instead of being looked up on the drive one by one (~20 ms each, 17 s in all). Only files actually open are looked up again; the next open of a cached file reads it from the drive, as after a cold start.
- The Memory Card on uma0/xmc0 stays **read-only from the wake-up until it has been mounted afresh** (about 18 s after a load). Before, the restored image's view of the card was writable for that time.
- After a load, `log.txt` records what the re-read found (`open files after the load: … gone … changed size … busy`), how the card came back (`mounted afresh` or `re-read in place`), and a check of a memory canary written before the load, which catches a restore that left stale cache lines behind.

**Storage that is away during the wake-up is not touched.** With a storage plugin, the drives behind ux0 and uma0 come back from the wake-up at their own pace:
- **A USB drive as ux0** (usbmc's PSVSD or USB stick) is only re-read once one sector of it can be read again. JamaisVu waits up to 3 s during the wake-up, and up to 20 s more afterwards. Until then it stays exactly as it was and read-only. Re-reading it too early failed and left ux0 unusable: the game went black, VitaShell reported its data as corrupt, and the PS TV restarted.
- **The Memory Card on uma0/xmc0** is not re-read in place. It is mounted afresh once the wake-up is over. During the first 15 s after JamaisVu's own wake-ups, the **card guard** refuses the system storage driver's attempt to unmount it.
- **A drive that disappears** while the wake-up checks the drives (its mount is being taken down) is left alone, neither remounted nor read.

**Unsupported firmware is never patched.** The original hooked 3.65 offsets on every firmware. On 3.60 that means patching random instructions inside the filesystem driver.

**Driver fixes.**
- Card transfers are split into ≤128 sectors (the original could send a wrong sector count for transfers above 255 sectors).
- The payload clears its `.bss` at startup, and several buffer overflows are fixed.

## What is still not guaranteed

No software can promise "100 %". These are the remaining risks:

- **Hardware coverage.** Tested on 3.60 only: a PCH-2000 (card as ux0), a PCH-1000 and a PS TV (both with usbmc). Other storage plugins and setups are untested. See the README's Compatibility section.
- **Load is inherently invasive.** It replaces the entire OS memory. The ux0 remount (`plugin/fs.c`, inherited from the original) refreshes the exFAT driver's caches, but it is reverse-engineered code. JamaisVu's protections make an inconsistency fail safe (read-only) rather than silently corrupt, but it can't rule out every bug in that path.
- **A card I/O error or a power cut in the middle of a load.** The payload stops (the LED blinks) instead of resuming a half-restored system. The savedata of the loaded game may be partly the slot's and partly what it was. The file system itself stays valid (see above), and loading the slot again completes the restore. Until then the game may not find all of its save files.
- **A slot brings back the system as it was when it was saved**, plugin settings included. A slot saved while something was wrong will bring it back on every load. Overwrite it with a new save rather than loading it again.
- **An oversized home screen.** On the PS TV, loads froze while the LiveArea had more apps than it could show and rebuilt its database at every restart. They worked again once the home screen was cleaned up. This is likely but not proven to be the cause. If loads start freezing, check that every installed app has a bubble.
- **What the log shows.** Every run is recorded in `data/JamaisVu/log.txt`. When it grows past 64 KB it is renamed `log.old.txt` and a new one starts, so the run before a restart is always kept. A load's step record (`trace.bin`) stays open for 60 s after the wake-up, so a later restart still shows how long the system lived.
