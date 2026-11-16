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
| 0x007E0000 / 0x007F0000 | 0x02A00000 / 0x02A10000 | 32-bit register block (index / data) |
| 0x03800000–0x03BFFFFF | 0x03000000 | flash, alias |
| 0x03C00000–0x03FFFFFF | same | flash (OS at 0x03FB0000) |

## Flash

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

## I/O ports

| Port | Device |
|---|---|
| 0x20/0x21, 0xA0/0xA1 (and 0xF020…) | 8259 PIC pair (386EX internal) |
| 0x40–0x43 (and 0xF040…) | 8254 timers (386EX internal) |
| 0x70/0x71 | MC146818-compatible RTC (binary mode, 24 h) |
| 0x92 | fast reset (OS uses it after a fatal error) |
| 0x0000–0x0007 | 16550-style UART (8N1, divisor 12), probably the radio modem |
| 0x380 / 0x382 | **LCD controller**, command / data |
| 0xF4xx | 386EX chip selects, watchdog (0xF4C8) |
| 0xF8xx | 386EX pin configuration and I/O ports P1–P3 (0xF860–0xF876) |
| 0xFC00–0xFC3F | RIM custom chip |
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

## Boot status (milestone 1)

The 957 OS boots all the way to its idle loop and clears the LCD. Nothing is
drawn yet because there are no apps (ribbon.dll etc.) in flash. Next:
work out the module format the RIM `Programmer` writes and load DLLs into flash.
