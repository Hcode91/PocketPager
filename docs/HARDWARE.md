# RIM 85x / 95x hardware notes

Everything here was worked out from the OS images (OS32 2.5.5, May 10 2002)
by running them in the emulator and tracing what they touch.

## OS image (`PagerXXX.bin`)

* 262,160 bytes: a 256 KB image plus a 16-byte trailer
  `u32, u32, u32 scrambled_len, "SCRM"`.
* The first `scrambled_len` bytes are XORed with a repeating 2048-byte key,
  then bit-inverted. The rest (the export table and boot record) is stored as-is.
* The key is not needed: erased flash (0xFF) runs inside the image become
  copies of the key, so `core/rim/scramble.c` recovers it from the most common
  2048-byte block.
* All four images (850, 857, 950, 957) use the same key.

## CPU and memory

* Intel 386EX (no FPU). The OS turns on protected mode and **paging**.
* The image runs in place from flash at **0x03FB0000** (256 KB).
* Entry point: offset 0, 16-bit flat real mode (CS = the image, GS flat).
  The boot ROM at 0x03FF0000+ is not dumped, so we start the OS directly.
* SRAM holds the OS data: GDT at 0x609A34, page directory at 0x601000,
  and data from 0x5F0000 up to 0x60C000.

| Linear | Physical | What |
|---|---|---|
| 0x00400000–0x0060BFFF | same | RAM |
| 0x007E0000 / 0x007F0000 | 0x02A00000 / 0x02A10000 | radio DSP IDMA port (address / data) |
| 0x03800000–0x03BFFFFF | 0x03000000 | flash chip 2 (AMD command set): file system + serial loader |
| 0x03C00000–0x03FFFFFF | same | flash chip 1 (Intel): OS at 0x03FB0000, apps below it, boot blocks on top |

## Flash

There are two chips.

### Chip 2: file system (AMD command set, x16, 4 MB, physical 0x03000000)

* Unlock cycles at word 0x555/0x2AA (byte 0xAAA/0x554), program 0xA0, sector
  erase 0x80…0x30, CFI query 0x98 at word 0x55 ("QRY", size code 0x16).
* The file area starts at linear 0x03810000.
* A separate **serial loader** program lives at linear 0x03B00000–0x03B0FFFF.
  The OS checks for its entry code (`FA E9`, i.e. `cli; jmp`) at 0x03B0FFF0
  before handing over; if it isn't there, the OS just resets.

### Chip 1: OS and apps

* Intel boot-block command set, x16, 4 MB (`28F320B3` style, device ID 0x8896).
  The OS accepts IDs 0x8890–0x8896 and also reads the CFI table ("QRY",
  size code 0x16).
* Commands seen: 0x90 ID, 0x98 CFI, 0x40 program, 0x50 clear status, 0xFF and 0xF0 read.
* The OS programs flash with code copied into SRAM. It reads the array
  straight after 0x50, so 0x50 also returns the chip to read mode.
* Command and status mode apply to the block being written, so the OS keeps
  running from other blocks.
* Apps (DLLs) are stored as modules growing **downwards** from 0x03FB0000. Each
  module has a 0x48-byte header at its top, with a self-pointer at +0x44.
  The OS's boot record at 0x03FEFFB8 starts the chain.

## Top boot block (0x03FF0000–0x03FFFFFF, eight 8 KB parameter blocks)

* 0x03FF6000: loader status word. -1 = last load never finished ("Last load
  was incomplete"), -5 = go to the loader, 0 = defragment applications. Any other
  value is a completed load; the emulator writes -2 (RIM's default release ID).
* 0x03FF4000…: device information; 0x03FFA000…: flash allocation log;
  0x03FFFFC0+: boot ROM data (not dumped).

## Radio DSP (ADSP-218x, seen through IDMA)

* IDMA address register at physical 0x02A00000, data register at 0x02A10000.
  Bit 14 of the address selects data memory; addresses auto-increment;
  24-bit program words take two 16-bit transfers.
* The OS loads the Mobitex firmware, then writes the entry point to PM 0 to
  start the DSP.
* DSP → host queue: DM 0x2080–0x20FC, write index DM 0x20FE, read index DM
  0x20FF, status DM 0x20FD (must be 0). Message = `[length][words…]`.
* Host → DSP queue: DM 0x2000–0x207C, write index DM 0x207E, read index DM
  0x207F. Message = `[length][command][params…]`. The host interrupts the DSP
  by toggling bit 4 of P1LTC (0xF862).
* The DSP interrupts the host on master PIC IRQ 5. Replies carry the command ID.
* Command 2 = power down; the DSP acknowledges on bit 0 of port 0xF863.
* The emulator fakes the DSP: it posts `1` after start, echoes every command
  ID, and acknowledges power-down. That's enough for the radio to initialise
  (state goes 2, then 1 = sleeping).

## Serial / loader protocol

* The cradle port is COM2: 16450 UART at 0x2F8, IRQ 3 (divisor 12 at boot).
* Programmer.exe opens the link by sending `0E AA 0F F0`. The OS recognises
  this and jumps to the serial loader (see chip 2).
* The OS jumps to a loader stub that must start `FA E9` at linear 0x3B0FFF0
  (target physical 0x0300FFF4). PocketPager places a halt there and runs the
  loader in C.
* Packets are `01 <cmd> 04` + arguments; ACK `06`, NAK `15`, `7F` reconnects.
  `B` baud, `U` scramble (NAK it), `v` programmer version, `p` params,
  `H` 62-byte device descriptor, `I` flash ID (0x8896), `R` read (addr32,
  len16 → data + CRC-16/KERMIT), `W` write (addr32, len16, data, CRC-16),
  `C` erase block, `T` blank check, `V`/`P` CRC-32, `X` disconnect.

## I/O ports

| Port | Device |
|---|---|
| 0x20/0x21, 0xA0/0xA1 (and 0xF020…) | 8259 PIC pair (386EX internal) |
| 0x40–0x43 (and 0xF040…) | 8254 timers (386EX internal) |
| 0x70/0x71 | MC146818-compatible RTC (binary mode, 24 h) |
| 0x92 | fast reset (OS uses it after a fatal error) |
| 0x2F8–0x2FF | COM2 UART (cradle / loader), IRQ 3 |
| 0xF862 bit 4 | host → DSP interrupt |
| 0xF863 bit 0 | DSP power-down acknowledge |
| 0x380 / 0x382 | **LCD controller**, command / data |
| 0xF4xx | 386EX chip selects, watchdog (0xF4C8) |
| 0xF8xx | 386EX pin configuration and I/O ports P1–P3 (0xF860–0xF876) |
| 0xFC00–0xFC3F | RIM custom chip |
| 0xFC30 | keyboard column select (active-low one-hot; 0 = all columns) |
| 0xFC34 | keyboard rows of the selected columns (active low, 7 bits) |
| 0xFC38/9 | keyboard mode: 0x14A = interrupt on any key, 0xC0/0xC1 = periodic scan |
| 0x380 read | LCD status, must return 0x62 or apps refuse to load |
| 0xF863 bit 2 + IRQ 13 | trackwheel step one way |
| 0xF867 bit 6 + IRQ 12 | trackwheel step the other way |
| 0xF877 bit 3 | holster sensor (1 = out of holster) |
| 0xFCA0–0xFCAE | RIM sleep timer (idle loop arms it, then reads back elapsed time) |

## LCD (957/857: 160×160, 1 bit per pixel)

| Command | Meaning |
|---|---|
| 0x56 | row = 0 |
| 0x15 + 2 bytes | row = byte 0 |
| 0x5C + 20 bytes | write one row (MSB = left pixel) |
| 0xBE | next row |
| 0xC6 + 1 byte | contrast |
| 0xA6 / 0xA8 0x27 / 0x94 / 0xAF | init: normal, duty, wake, display on |

The OS keeps a 160×20-byte shadow framebuffer and sends only the dirty rows.

## Keyboard

IRQ 1. The OS key table (found by searching the image for `'Q',0,'W',0`) is
64 words indexed by `row << 3 | col`. Key codes follow the SDK's Rim.h; the
trackwheel click is the matrix key with code 0xFFFF (row 0, column 7).

## Status (milestone 3)

The 957 boots to the home screen with the system apps and 12 games
installed through the real Programmer.exe, and games (RimVaders tested)
launch and play.

## Boot status (milestone 2)

The 957, 950 and 850 OS images boot through radio initialisation, the flash
file system and the OS self-check (CRC-32 over the image) into normal
multitasking, with the cradle serial port running. The screen stays blank
because the fonts and UI come with the apps. The 857 image hits a
general-protection loop that still needs looking at.


