# Scorched eMiYoolator

Scorched Earth 1.5 (Wendell Hicken, 1995) for the **Miyoo Mini / Mini Plus** on **OnionOS**,
as a native ARM program. It is the original game, not a remake: a static recompiler
translates the game's own `SCORCH.EXE` (16-bit DOS, Borland C++ 4.0 and Fastgraph 4.02)
into C, which is compiled for the Mini. A small emulated PC supplies what the program
expects around it: DOS, a VGA/VESA card, keyboard, mouse, timer and PC speaker.

The repository contains no game data and nothing generated from it. The build reads
your own copy of the game (`Original_EXE/`) and writes the translated code to `build/gen/`.

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

## Playing

Controls and a first-game walkthrough are in
[`packaging/Roms/PORTS/Games/ScorchedEarth/README.txt`](packaging/Roms/PORTS/Games/ScorchedEarth/README.txt),
which is installed next to the game.

## License

The recompiler and runtime are MIT licensed (`LICENSE`). Scorched Earth is (c) 1991-1995
Wendell Hicken; its files are not part of this repository.
