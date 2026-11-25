# C64 emulator for the TI-Nspire CX II-T (Ndless)

A Commodore 64 (PAL) emulator: 6510 CPU (all official opcodes plus common
illegal ones), memory banking, VIC-II (text, multicolour, bitmap, ECM, 8 sprites,
collisions, raster IRQs, badline cycle stealing), two CIAs (timers, TOD, keyboard,
joysticks) and a KERNAL LOAD trap for `.d64` disk images.

## Status: read this first

- The portable core (`c64.c`) is compiled and tested on a PC (`make host_test`):
  CPU, decimal mode, CIA timer IRQs, text rendering and sprites pass.
- It has **not** been run with real C64 ROMs and **not** compiled against the Ndless
  SDK or run on a calculator. `nspire_main.c` was written from memory of libndls:
  expect to fix a `KEY_NSPIRE_*` name or two, and check the RTC address
  (`0x90090000`) used for frame pacing on your OS version.
- **No sound** (SID registers are stored, nothing is played).
- Not supported: fast loaders, copy protection, SAVE, OPEN/INPUT# file access,
  cartridges, tape. Games that load via the normal `LOAD"X",8,1` work.
  Raster tricks (open borders, FLD etc.) are approximate.

## You need

1. A CX II-T with **Ndless** installed.
2. The Ndless SDK to build.
3. The three C64 ROM dumps (not included, they are copyrighted):
   `kernal` (8192 bytes), `basic` (8192), `chargen` (4096). Use any legally
   obtained copy (e.g. from VICE's `C64` data directory).

## Build

    make            # produces c64.prg.tns
    make host_test  # optional, runs the PC self-test

## Install

Transfer to the calculator, adding `.tns` to each name, into `/documents/c64/`:

    kernal.bin.tns   basic.bin.tns   chargen.bin.tns
    mygame.prg.tns   mydisk.d64.tns

Put `c64.prg.tns` anywhere and launch it from the file browser.

## Controls

| Nspire | C64 |
|---|---|
| Letters, digits, Space, Enter, Del | same keys (Del = INST/DEL) |
| Esc | RUN/STOP |
| Tab | Commodore key |
| Shift | Shift |
| `+ - * / = , .` keys | same symbols |
| Ctrl+1/2/3/4 | F1/F3/F5/F7 |
| Arrow keys | joystick (port 2 by default) |
| Touchpad click | fire |
| Menu | toggle arrows between joystick and cursor keys |
| Ctrl+Menu | switch joystick port 1/2 |
| Ctrl+Esc | back to the file menu (Esc in the menu quits) |

`"` is Shift+2 as on a real C64. `:` and `;` have no mapped key yet.

## Using it

The menu lists `.prg` and `.d64` files plus "Boot to BASIC". A PRG is loaded
after BASIC reaches READY and started with `RUN` or `SYS`. For a disk, pick a
file from its directory; the disk stays mounted, so games that `LOAD` further
parts work, and `LOAD"$",8` lists the directory.

Speed is paced to about 50 fps using the RTC, calibrated at start-up (takes a
second or two). If the large static buffers (about 350 KB BSS) fail to load under
your Ndless version, switch them to `malloc`.
