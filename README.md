# Scorched eMiYoolator

Scorched Earth 1.5 (Wendell Hicken, 1995) for the **Miyoo Mini / Mini Plus** on **OnionOS**,
as a native ARM program. It is the original game, not a remake: a static recompiler
translates the game's own `SCORCH.EXE` (16-bit DOS, Borland C++ 4.0 and Fastgraph 4.02)
into C, which is compiled for the Mini. A small emulated PC supplies what the program
expects around it: DOS, a VGA/VESA card, keyboard, mouse, timer and PC speaker.

The repository contains no game data and nothing generated from it. The build reads
your own copy of the game (`Original_EXE/`) and writes the translated code to `build/gen/`.

## Install

1. Download `scorched-emiyoolator-<version>.zip` from the
   [latest release](https://github.com/uncannyowly/scorched-emiyoolator/releases/latest).
2. Extract it to the root of the SD card of a Miyoo Mini, Mini v4 or Mini Plus running
   OnionOS. The zip holds a `Roms` folder that merges with the card's own.
3. On the Mini: Games > Ports > Strategy > Scorched Earth. If it is not listed yet, run
   "~Import ports" in the Ports list.

The release zip is ready to play: it includes the shareware version of Scorched Earth
1.5, unmodified (the author's manual says to share it with your friends), next to the
native build of the port. Updating works the same way and keeps your settings and saved
games. A first-game walkthrough is in
[`README.txt`](packaging/Roms/PORTS/Games/ScorchedEarth/README.txt), installed next to the
game.

## Controls

| Button | PC key | What it does |
|---|---|---|
| D-pad | arrow keys | Aim: left/right turns the turret, up/down sets the power (the numbers run fast while you hold). Menus, the shop's marker. |
| A | Space | Fire. Buy in the shop. |
| B | Esc | Back, cancel, finish buying. |
| X | Tab | Next weapon (hold L1 for the previous one). |
| Y | Enter | OK, Done. |
| L1, held | Shift | With the D-pad: aim at half speed. |
| R1, held | Alt | With the D-pad: one step per press (keep holding for slow repeats). |
| L2 / R2 | Page Down / Page Up | Power down / up fast. Shop pages. |
| START | F1 | System menu: save and restore games, new game, mass kill, quit. |
| SELECT, tap | | On-screen keyboard, for names and the menus' hot keys. |
| SELECT, hold | | Mouse pointer: tap the D-pad to move it one pixel, hold to glide (it speeds up the longer you hold). A and B click left and right. |
| MENU, tap | | Mouse click where the pointer is. |
| MENU, hold 2 s | | Quit at once. |

- **Aiming:** hold the D-pad to get close, then hold R1 and tap the D-pad for single steps.
- **On-screen keyboard:** the D-pad picks a key, A types it, X is backspace, Y shift,
  START Enter; B or SELECT closes it, and L1/R1 move it to the top or bottom of the
  screen. The menus' hot keys are the letters shown in colour (S = Start, H = Hardware...).
- **Mouse speed:** the game's own Mouse Rate (Hardware menu) scales the pointer too.
- **Quitting the usual way:** START, Quit Game, Yes, then A on the closing screen.
- **Screen:** the game starts in 320x240, which fills the screen with readable text. For
  more detail and smaller text, choose Hardware... > Graphics Mode 640x480 in the main
  menu, then Save Changes, quit and start again.

## How it works

| Part | Where |
|---|---|
| Static recompiler: finds the functions, writes one C function per x86 function | `tools/recomp.c` |
| Instruction semantics shared by the compiled code and the interpreter | `src/ops.h`, `src/ops.c`, `src/fpu.c` |
| Decoder (8086 to 286, the 386 forms the runtime uses, x87, Borland's FPU emulator interrupts) | `src/decode.c` |
| Interpreter: runs what the recompiler did not find, and is the reference in tests | `src/interp.c` |
| Run loops, interrupts, time, ports | `src/machine.c` |
| DOS (loader, memory, files, console), BIOS, VGA/VESA, timer and speaker, keyboard, mouse | `src/dos.c`, `src/bios.c`, `src/vga.c`, `src/pit.c`, `src/kbd.c`, `src/mouse.c` |
| Handheld controls and on-screen keyboard | `src/ui.c` |
| Desktop (SDL2, headless tests) and Miyoo (`/dev/fb0`, evdev, MI_AO) | `src/platform_sdl.c`, `src/platform_miyoo.c` |

Compiled functions call each other directly. When compiled code meets something it
cannot handle (a call through a pointer to code it does not know, an unexpected return
address), it hands over to the interpreter at the exact instruction, and the interpreter
calls compiled code again as soon as it reaches a known function. Addresses the
interpreter had to run are written to `uncompiled.txt`; `tools/entries.txt` collects them
for the next recompile.

The compiled code and the interpreter count instructions identically and take interrupts
at the same points (taken backward branches), so with virtual time two runs can be
compared exactly (`--trace-events`).

## Building

Desktop (needs SDL2 and dosbox, nasm for the VGA fonts, Python 3):

```sh
make recompile          # build/gen/*.c from Original_EXE/SCORCH.EXE
make                    # build/host/scorch (compiled game + runtime)
make interp             # build/host/scorch-interp (interpreter only)
build/host/scorch --dir path/to/game     # a copy of the game folder: it writes SCORCH.CFG
```

Desktop keys are the PC keyboard and mouse; `--pad` turns the keyboard into the Mini's
buttons instead (arrows = D-pad, X = A, Z = B, S = X, A = Y, Q/W = L1/R1, 1/2 = L2/R2,
Right Shift = SELECT, Enter = START, Esc = MENU).

Miyoo Mini (needs Docker; uses the GTA1 port's toolchain image `gta1-miyoo-toolchain`,
or builds it from `Dockerfile`):

```sh
scripts/package.sh                      # dist/Roms/... and dist/scorched-emiyoolator-<version>.zip
scripts/deploy-sd.sh /run/media/$USER/CARD
```

The `VGA ROM fonts` (`build/fonts.bin`) are dumped from DOSBox by `scripts/make-fonts.sh`.

## Testing

```sh
scripts/equivalence.sh       # compiled vs interpreter, and compiled at 30 fps: traces must be identical
scripts/equivalence.sh arm   # also the Miyoo build under QEMU (docker, linux/arm/v7)
```

The sessions in `tests/` are scripted input on a virtual 25 MIPS clock, so every run is
deterministic. The ARM runs need binfmt support for arm
(`docker run --privileged --rm tonistiigi/binfmt --install arm`). A single run:

```sh
build/host/scorch --dir path/to/game --headless --ms 30000 --script tests/walkfire.txt
```

Script lines are `<ms> key <name> [down|up]`, `type <text>`, `mouse <x> <y>`,
`mickeys <dx> <dy>`, `click [left|right]`, `pad <BUTTONS>`, `shot <file.ppm>`, `quit`.
`SCORCH_SLEEPQ=6000` makes real-time sleeps as coarse as the Mini's (a 0.2 ms sleep takes
about 9 ms there), to check that nothing timed depends on short sleeps.

## License

The recompiler and runtime are MIT licensed (`LICENSE`). Scorched Earth is (c) 1991-1995
Wendell Hicken; its files are not part of this repository.
