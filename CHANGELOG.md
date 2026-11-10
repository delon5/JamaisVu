# Changelog

## 2.0.0-rc2 - 2026-11-10

States saved with 2.0.0-rc1 still load. A state holds the whole system as
it was saved, JamaisVu included, so the fixes to what happens *after* a
load only apply to states saved with this version: after updating, reboot
and save new states.

### Fixed

- **Games that could not read their files after a load.** A game that
  kept a file open across a save could get an I/O error on it after a
  load. Catherine: Full Body (PCSG01179) ignored every button on its
  title screen, could not open Load Game, or stayed on a loading screen.
  After a load JamaisVu freed a small buffer the exFAT driver keeps for
  each open file; the driver only creates that buffer when a file is
  opened. The buffer is now kept and only its contents are forgotten.
  Confirmed on a PCH-2000.
- **Games that left gameplay or hung reading their save after a load**
  (seen on a PCH-1000 with the saves on the card through reSavedata).
  Putting the savedata back used to delete the folder and recreate every
  file. Now a file that already matches the state is not touched at all,
  a file that differs is overwritten in place, and what the state does
  not have is removed afterwards. Restored files and folders also get
  valid access and creation times.
- **Saves that failed in the card driver with an overclock profile
  active** (500 MHz). The save or load itself now runs at the console's
  default clocks; the profile is put back afterwards.
- `log.txt`: a run that failed before the savedata backup no longer shows
  the previous run's file count.

### Protection against Memory Card corruption

New in this version:

- **The restored memory is checked before the system runs on it.** A
  save stores a checksum of the memory contents in the slot (slot format
  4). A load compares it with what it has just restored. If they differ
  it stops, as after a read error, instead of resuming a system whose
  memory is not what was saved and letting it write to the card. Slots
  saved by earlier versions have no checksum and load as before.
- **A power cut while the savedata is put back cannot leave the card
  unsafe.** This is the one step of a load that changes the file system
  itself. Its writes are now ordered so that losing power after any
  single one leaves at most unused space and a file hidden for the
  moment, and loading the slot again finishes the job. A new test cuts
  the restore after every single card write (over 2,300 cut points),
  checks each card with `fsck.exfat` and loads the slot again.
- **A load writes far less to the card.** A savedata file that already
  matches the state is not written at all; one that differs is
  overwritten in place. The folder is no longer deleted and recreated.
- **The one raw write is checked on both sides.** After a wake-up the
  plugin reads the step record's sector back and writes to it only if it
  still carries this run's signature.
- **Saves and loads run at the console's default clocks**, whatever
  overclock profile is active (see Fixed).

Already there since 2.0.0-rc1 (details in `docs/design.md`):

- A save cannot change the file system: the slot file is created by the
  Vita's own driver beforehand, and a write guard blocks every write
  outside that file's sectors.
- An interrupted save is never loadable, and an incomplete snapshot is
  never taken.
- Nothing is written by a load until the whole state file has been
  checked, including that it was made on the same Memory Card.
- A load's writes are fenced to the card's exFAT area; the partition
  table and boot sectors cannot be written.
- After a load nothing can be written through stale caches: every drive
  is re-read first, and a drive whose open files no longer exist stays
  read-only until restart.
- Only the official Memory Card is written to.

### Changed

- **The menu opens faster the first time.** The slot list is read in the
  background shortly after the start and after each load, instead of
  while the menu stands empty.
- Switch files in `data/JamaisVu/` on the card, for narrowing down a
  problem. All are off unless the file exists:
  - `nosavedata`: saves and loads leave the games' savedata alone.
  - `cardinplace`: a load re-reads the card in place instead of mounting
    it afresh.
  - `fullmem` (experimental): a save also takes the memory between the
    mapped pages. Slots get larger; it has not shown a benefit.

### Development builds only

Not part of a release build:

- The Diagnostics screen can dump a game's threads, repeat a read a
  thread is stuck in, and read every file the game has open at fixed
  places with checksums. This is what found the file problem above.
- The dump 30 seconds after a load and the record of file calls after a
  load are on request only (`loaddump` and `iowatch` in
  `data/JamaisVu/`).

## 2.0.0-rc1 - 2026-11-09

First public release candidate.
