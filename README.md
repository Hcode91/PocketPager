# PocketPager

An Android emulator for the RIM 850, 857, 950 and 957 two-way pagers
(Intel 386EX based, OS 2.x). It emulates the hardware and boots the real
OS image.

**Status: games run.** On the 957, the OS boots to the home screen, the
real RIM Programmer can install apps over the emulated cradle port, and
games such as RimVaders launch and play with the keyboard and trackwheel.
See [docs/HARDWARE.md](docs/HARDWARE.md).

## Using it

1. Install the APK from the latest GitHub Actions run (artifact `PocketPager-apk`).
2. Tap **Load OS** and choose an OS image: `Pager957.bin`, `Pager950.bin`,
   `Pager857.bin` or `Pager850.bin` (from RIM's handheld SDK 2.x). The app
   descrambles it itself.
3. Tap **Load Flash** and choose an 8 MB PocketPager flash image with apps
   installed (made with `ppcli` + Programmer.exe, see below). The flash is
   saved automatically, so this survives restarting the app. Loading a new
   OS starts over from an empty device.
4. Controls: ▲/▼ roll the trackwheel, **Click** presses it, **ESC** goes
   back, plus the full thumb keyboard.
5. **Log** shows what the emulated hardware is doing.

### Making a flash image with apps

Run `ppcli` with `PP_SERIAL=1 PP_PTYLINK=/tmp/pp_com PP_SAVEFLASH=flash.bin`,
point Wine's COM1 at `/tmp/pp_com`, and load apps with RIM's
`Programmer.exe` (e.g. `programmer load -p1 app.dll ...`). Stop `ppcli`
with Ctrl+C and the flash is saved.

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
  * `PP_BP=addr,addr` logs registers at addresses (with `PP_TRACE`)
  * `PP_DSPLOG=1` logs radio DSP traffic, `PP_UARTLOG=1` UART register access
  * `PP_SERIAL=1` runs in real time with COM2 on a pseudo-terminal
    (`PP_PTYLINK=/tmp/pp_com` makes a stable link to it, e.g. for Wine's COM1)
  * `PP_SAVEFLASH=file` saves both flash chips (8 MB) at exit,
    `PP_LOADFLASH=file` starts from such an image
  * `PP_INPUT="ms:key[:up],..."` scripted input (letters, `WU`/`WD` wheel,
    `CL` click, `ESC`, `ENT`, `SP`, `BS`, `ALT`, `SH`),
    `PP_SHOT="ms:file.pgm,..."` screenshots at given times
* `android/` – the app (Kotlin + JNI).
