# PocketPager

An Android emulator for the RIM 850, 857, 950 and 957 two-way pagers
(Intel 386EX based, OS 2.x). It emulates the hardware and boots the real
OS image.

**Status: early.** The 957 OS boots to its idle loop with the LCD running.
Apps aren't loaded into flash yet, so the screen stays blank.
See [docs/HARDWARE.md](docs/HARDWARE.md).

## Using it

1. Install the APK from the latest GitHub Actions run (artifact `PocketPager-apk`).
2. Tap **Load OS** and choose an OS image: `Pager957.bin`, `Pager950.bin`,
   `Pager857.bin` or `Pager850.bin` (from RIM's handheld SDK 2.x). The app
   descrambles it itself.
3. **Log** shows what the emulated hardware is doing.

No RIM firmware or software is included in this repository.

## Layout

* `core/cpu` – 386 CPU, 8259 and 8254, from [tiny386](https://github.com/hchunhui/tiny386)
  (BSD-3-Clause / MIT, see `core/cpu/LICENSE.tiny386`), with small hooks for this board.
* `core/rim` – the pager board: memory map, flash, RTC, LCD, image descrambler.
* `tools/ppcli` – headless runner used for reverse engineering:
  `make -C tools && tools/ppcli Pager957.bin 3000 screen.pgm`
  * `PP_TRACE=1` keeps an instruction trace and prints it on reset or crash
  * `PP_STOPAT=<hex eip>` stops at an address (with `PP_TRACE`)
  * `PP_DUMP=file` writes the 64 MB address space at exit
  * `PP_PORTLOG=<hex port>` logs every access to that port group
  * `PP_FLASHLOG=1` logs flash commands
* `android/` – the app (Kotlin + JNI).
