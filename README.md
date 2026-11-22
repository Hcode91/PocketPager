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

## Bold 9000 (BlackBerry OS 5)

A second family runs RIM's later Java handhelds at the OS level: our own VM
(`core/bbos`) loads RIM's `.cod` modules and runs the real OS 5.0 for the
Bold 9000, with the device's native services written in C. BrickBreaker
plays; other built-in games are in progress.

1. In the app tap **Bold 9000 (BlackBerry OS 5)**.
2. Tap **Import OS installer** and choose RIM's Windows installer for the
   Bold 9000 (`9000M_PBr5.0.0_..._Wind.exe`). The app unpacks its 2,300
   modules (about 100 MB); the installer is not kept.
3. Pick a game. Controls: the trackball (drag to roll, tap to press), the
   green/red call keys, Menu, Escape and the QWERTY keyboard (Shift and Alt
   latch for the next key). A gamepad or keyboard works too: D-pad rolls,
   A presses the trackball, B is Escape, Start is Menu.
4. **Add apps** takes more `.cod` files or a `.zip` of them.

Tools for this family: `tools/bbimport` (unpack an installer:
`bbimport installer.exe outdir`), `tools/bbrun` (boot headlessly:
`bbrun -d outdir net_rim_app_manager net.rim.device.api.system.ApplicationManagerApp`,
with `BBLAUNCH=module` and `BBINPUT="10:menu,10:click,5:shot"` scripted input).

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
* `core/bbos` – the BlackBerry OS family: `.cod` loader, RIM Java VM, device natives,
  installer importer (`bbimport.c`) and the front-end API (`bbhost.c`).
* `android/` – the app (Kotlin + JNI).
