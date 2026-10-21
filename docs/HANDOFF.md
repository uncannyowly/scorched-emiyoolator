# Handoff: Scorched eMiYoolator

Last updated: 2026-10-21 (0.3.3). Keep this file current; it is what the next agent starts from.

## Goal (the user's words)

"Rewrite [SCORCH.EXE] to ARM and play on the Miyoo Mini. Put together a native running
port for this game so I can easily play it on the Miyoo Mini." The user confirmed they
want it true to the original, which is why this is a static recompilation of the original
executable and not a remake (the GTA1 clean-room engine in ../gta1-miyoo kept diverging
from the original; see ../gta-emiyoolator/docs/HANDOFF.md).

Rules: never commit the game (Original_EXE/) or anything generated from it (build/gen,
build/fonts.bin come from the user's copy and DOSBox at build time). Written copy: no em
dashes (user preference).

## Where things are

| What | Where |
|---|---|
| This repo | `/mnt/data/git/MiYoo-Mini/scorched-emiyoolator` (not yet a git repo; the user creates GitHub repos on the web) |
| The user's game files | `Original_EXE/` (Scorched Earth 1.5 shareware: SCORCH.EXE 415456 bytes, 10 MTN mountains) |
| SD card | `/run/media/shoots/E010-96EB` (OnionOS 4.4.0 beta); port in `Roms/PORTS/Games/ScorchedEarth`, shortcut `Roms/PORTS/Shortcuts/Strategy/Scorched Earth.port` |
| Toolchain image | `gta1-miyoo-toolchain` (shared with the GTA1 port; `Dockerfile` here builds the same) |

## The executable (recon)

- MZ, TLINK 6.1 (Borland C++ 4.0), 388320-byte image, 6136 relocations, no overlays,
  no packer. Large model: about 110 code segments, DGROUP at image segment 4F38h.
- Graphics: Fastgraph 4.02 (Ted Gruber). It probes for Video7 (INT 10h 6Fxx) and uses VESA
  when the BIOS has it; reads the VGA ROM fonts through INT 10h AX=1130h (BH=2,3,6).
- Floating point: Borland's emulator interrupts INT 34h..3Dh (decoded as x87), and INT 3Eh
  "shortcuts" `CD 3E xx 90`: EC sin, EE cos, F0 tan, F2 atan, F4 ln, FA exp.
- CPU detection: the RTL checks for a 386 (word at DGROUP:C5BA) before 386-only register
  saves; we present a 286 (FLAGS bits 12-15 read 0), so those paths stay unused.
- Keyboard: "Bios Keyboard" (BIOS_KEYBOARD=On/Off in SCORCH.CFG, flag DGROUP:502E)
  defaults Off, but the game forces it On when INT 33h AX=24h reports a PS/2 mouse
  (CH=4; DOSBox does that). With it On, keys come through DOS (Borland getch(): INT 21h
  AH=07h, extended keys as 0 then the scan code) and aiming moves one step per key event:
  Shift and Alt do nothing. With it Off, the game's own INT 9 handler (22F6:012E) reads
  port 60h, ignores a byte equal to the previous one, keeps held keys / an event queue
  with the BDA shift flags, and chains to the BIOS INT 9. Aiming then runs at about 244
  units/s while an arrow is held (timer paced, same at any MIPS), half that with Shift,
  one step per make code with Alt. That is the manual's behaviour, so we report a serial
  mouse (CH=2, IRQ 4).
- Mouse: only INT 33h AX=0, 24h, 3 and 0Bh. The game draws its own pointer from the
  motion counters (mickeys) scaled by its Mouse Rate. Function 0Ch is never used.
- Timer: the game reprograms PIT channel 0 to 3648.87 Hz.
- Borland's int86 writes `push bp; int NN; pop bp; retf` on the stack (SS=5FCA) and far
  calls it: those stubs are always interpreted (outside the image), which is cheap.
- On quit the game switches to text mode, prints the shareware screen through DOS and
  waits for a key.

## Design

- Load segment fixed: PSP 0100h, image at 0110h; the recompiler loads the exe the same
  way, so relocated segment values in the code are constants. The runtime checks an FNV
  checksum of the loaded image against the compiled code's and falls back to the
  interpreter if they differ.
- One C function per x86 function (entry points: program entry, call targets, Borland
  switch tables, relocated far pointers in data, and `tools/entries.txt`). Instructions use
  `static const Insn` + the inline semantics in ops.h; GCC folds them to plain code.
- "Bail": compiled code that cannot continue stores CS:IP, sets cpu.bail and returns; the
  run loop continues in the interpreter. RET/RETF/IRET are C returns; after every call the
  caller checks CS:IP (RETURNED macro).
- Flags: unpacked fields; the recompiler does flag liveness per function and passes a mask
  (Insn.fmask) so dead flags are not computed. Calls, INT, PUSHF, returns and exits count
  as reading all flags (a callee may do INT 21h; PUSHF and observe them).
- Interrupts only at taken backward branches (and in HLE waits), in both interpreter and
  compiled code; instruction counts are flushed at the same points. With `--headless`
  (virtual time, 25 MIPS) interpreter, compiled PC build and compiled ARM build produce
  byte-identical event traces. Verified 2026-10-20 over 750M instructions of gameplay
  (PC compiled vs PC interpreter, and ARM compiled under QEMU vs PC interpreter).
- Idle: when a mouse poll finds no input, nothing was drawn and under 20000 instructions ran
  since the previous poll, sleep until the next timer interrupt (mouse.c poll_throttle).
  Main menu: about 93% idle. A cruder 2 ms poll interval broke the game (it polls the
  mouse inside busy drawing loops too); do not go back to that.
- Video: VGA planes interleaved (vram[offset*4+plane]) with the full write/read pipeline;
  geometry from the CRTC. VESA VBE 1.2 with 64 KB banks, window function F000:0180.
  Text mode is rendered with the 8x16 font; DOS console output goes to the text screen.
- Sound: PC speaker events time-stamped and replayed by the audio thread 50 ms behind.
- Default on the Mini: GRAPHICS_MODE=320x240 (scorch-miyoo.cfg copied to SCORCH.CFG on
  first start): doubled it fills 640x480 with readable text. 640x480 works (VESA) but its
  text is under 1 mm tall on the 2.8" screen.

## Controls (ui.c)

D-pad arrows, A Space (fire/buy), B Esc, X Tab, Y Enter, L1/R1 held Shift/Alt, L2/R2
PgDn/PgUp, START F1 (system menu). Arrows and PgUp/PgDn are sent as the grey keys (E0
prefix): the keypad ones become digits under Shift in the BIOS translation, which made
L1 useless in 0.3.1. SELECT tap = on-screen keyboard, SELECT hold = mouse
pointer (D-pad moves, A/B click), MENU tap = left click (user request after 0.2.0),
MENU held 2 s = quit. The keyboard must not open on SELECT's release after SELECT steered
the pointer (0.2.0 bug: the check looked at the already masked buttons). The README's first-game walkthrough was
verified with a pad-only script (build/s_walk.txt).

## Status

- [x] Game runs on desktop and under ARM emulation: menus, options, player setup, terrain
      (scanned MTN mountains), firing, explosions, AI turns, shop, system menu, quit screen.
- [x] Native code covers all executed instructions in the test sessions (the int86 stack
      stubs `55 CD nn 5D CB` run natively in gen.h int86_stub).
- [x] Packaging for OnionOS (scripts/package.sh, scripts/deploy-sd.sh), box art from the
      title screen.
- [x] 0.1.0 on the device (2026-10-20): "played pretty slowly", on-screen keyboard laggy.
      Log: 7.2 M instructions/s while busy, idle 0%, ~55 frames/s presented. Causes:
      (1) the renderer divided per pixel and armv7-a code has no hardware divide (libgcc
      calls); (2) real time checked interrupts only every 4000 instructions, so the game's
      3649 Hz timer got ~1800/s and everything timed ran at half speed; (3) ARM GCC 8.3 does
      not fold `static const Insn` loads even at -O2 (the PC's GCC 15 does).
- [x] 0.2.0 fixes, installed on the card: picture fetch only when VGA changed plus
      frame skip on identical pictures, division-free 2x scaler, 30 fps on the Mini, opaque
      keyboard overlay; interrupt checks aimed at the next timer tick (measured
      instructions/us); `-mcpu=cortex-a7`; the recompiler emits plain C for the common
      instructions (MOV, ALU, INC/DEC, shifts, MUL/DIV, PUSH/POP, LES/LDS, ENTER...),
      generated code at -O2 (ARM build ~30 s); SS-relative accesses skip the VGA check;
      unchained Mode X write fast path. Under QEMU (real time): idle 56%, timer 3586/s.
      Binary 4.6 MB (was 9.7 MB).
- [x] 0.2.0 on the device: "a rather good game session, played 5 rounds" (532 s): 39.4 M
      instructions/s while busy (5.5x 0.1.0), 30 fps, but idle 0% and timer 3103/s (85%).
      Cause: rendering and the page flip (FBIOPAN_DISPLAY likely waits for vsync) ran on
      the emulation thread, so ticks piled up past the 8-tick cap and the idle sleep never
      ran (it skipped sleeping whenever a tick was owed). User report: releasing SELECT after
      steering the mouse opened the keyboard; wants the click on MENU (keep the 2 s quit).
- [x] 0.3.0, installed: render thread on the second core (Mini only; the game thread just
      snapshots the picture), idle check brings the timer up to date before deciding,
      MENU tap = left click, SELECT-release keyboard bug fixed, +124 entry points from the
      device log (1312 functions). QEMU with the fake framebuffer: idle 54%, timer 3494/s.
- [x] 0.3.0 on the device (211 s): 46.4 M instructions/s, timer 3560/s (97.6%), still
      idle 0%; "mouse was very hard to control. down barely registered then took off like
      crazy, top just sped off upward". Causes: (1) the pointer went screen pixels ->
      driver coordinates -> mickeys with integer truncation; the driver range came from the
      text mode at reset (640x400), so a 1-pixel step down rounded to no movement until the
      acceleration made steps big; (2) mouse "pending" bits were never cleared (Scorch
      installs no event handler), so after the first mouse use the idle sleep never ran.
- [x] 0.3.1, installed: the D-pad sends mickeys directly (the game moves 0.5 px per mickey
      on both axes at Mouse Rate 0.5, measured): tap = 1 pixel, hold glides at 40 px/s after
      150 ms and ramps to 240 px/s from 0.4 s, fractions kept. Idle check uses counters of
      new mouse/keyboard input instead of pending flags. Real time on the PC after mouse
      use: 68% idle.
- [x] 0.3.1 on the device (342 s): "gameplay was good", "L1/R1 do nothing", mouse "much
      more useable in general, not perfect but much better". Log: 2.7 M instructions/s
      "while busy", idle 1%, timer 1330/s (36%). Cause: the Mini's kernel seems to round
      short sleeps up to its tick (about 10 ms or more): the idle code asked for ~0.2 ms
      sleeps, the statistics counted the requested time, and the 8-tick cap on owed ticks
      dropped most ticks that fell due during each sleep. Reproduced on the PC with
      SCORCH_SLEEPQ=10000 (timer 750/s with the old code).
- [x] 0.3.2: ticks that fall due during a sleep are all owed (up to 100 ms of them,
      pit_update_slept); statistics count the real sleep time and the log shows sleeps and
      their average length, plus "timer: a 200 us sleep takes N us" at start. Serial mouse
      (Bios Keyboard Off: L1/R1 work as in the manual), grey arrow keys, +22 entry points
      (1334 functions; the game's INT 9 handler was interpreted). New IRQ requests are
      looked at on the next checkpoint (pic_raise sets the deadline): before, a keyboard
      byte fed at the previous byte's EOI waited for the next timed event, which could be
      a frame, so traces depended on the frame rate. Verified: compiled == interpreter on
      4 sessions (cover 90 s, mouse, walkfire, mod2), 30 fps == 60 fps, ARM (QEMU) == PC
      on cover/walkfire/mod2. Real time on the PC: timer 3643/s, idle 80%; with 10 ms
      sleep steps: 3625/s, idle 82%. ARM real time (QEMU, fake fb): timer 3512/s, idle 72%.
- [x] 0.3.2 on the device (555 s): "timer: a 200 us sleep takes 8708 us" (coarse kernel
      sleeps confirmed); timer 3458/s (95%), idle 64% (39282 sleeps of 9.0 ms), 25.5 M
      instructions/s while busy; 1.56 M instructions interpreted, mostly segment 30CD.
      No comment from the user on the controls yet.
- [x] 0.3.3 (from that log and a simulation of its sleeps, SCORCH_SLEEPQ):
      (1) With 9 ms sleeps the held-aim loop, which waits a few ms per step while polling
      the mouse, slept a whole step every time: on the PC with 10 ms sleep steps, Up held
      2 s gave +103 instead of +487 and Shift made no difference. Now poll_throttle sleeps
      only after the game has been idle for 4 sleep steps (2 to 50 ms; the step is
      measured at start, sleep_step_us). Same test now: +479, Shift +244.
      (2) In play the game spins on 3DAh for ~90% of every frame (vertical retrace wait,
      ~122,000 reads per frame; it never polls the mouse there). status1() now sleeps in
      that wait when the retrace is more than 1.5 sleep steps away, and a retrace slept
      through is shown once, late, so no frame is lost. Virtual time sleeps exactly to the
      retrace: the 90 s cover session runs 157 M instructions instead of 460 M.
      (3) Recompiler: far pointers stored in DGROUP may name code segments no instruction
      mentions (30CD is a module at 3005:0C80 reached only through function tables; also
      162B, 27F6, 3086, 2880, 326D, 331D, 35C2, 1242, 15D8, 2492: all Borland functions
      with the stack-check prologue); runtime addresses are taken in address order, so
      later ones inside a function added in the same round become its resume points
      (before, they became separate functions with copied code: 1384 functions and
      11.1 MB of C; now 1044 functions, 846 resume points, 8.8 MB); explore() no longer
      holds a pointer into funcs[] across add_func() (realloc).
      (4) Log: dropped timer ticks and the longest sleep.
      Verified: scripts/equivalence.sh arm (all identical). Note the cover session never
      leaves player 3's setup (true since 0.1.0); walkfire and mod2 cover play.
- [ ] 0.3.3 on the device: "speed:" line (idle %, ticks dropped, longest sleep); aiming
      speed with D-pad, L1, R1; what is still off with the pointer.

## Things learned the hard way

- Deterministic runs must not depend on the frame rate or host: virtual-time deadlines are
  absolute (icount_at), any sleep resets cpu.deadline, the 8-tick cap on owed timer ticks
  applies only in real time, traces log only real deliveries, and the time limit is
  checked on the 4 ms poll grid. With that, the PC interpreter at 60 fps and the ARM build
  at 30 fps give byte-identical traces.
- Do not rely on GCC folding loads from `static const` structs: GCC 8.3 (the Miyoo
  toolchain) does not. Emit scalar code (tools/recomp.c emit_direct).

- Resume points: runtime addresses inside an existing function become `switch (cpu.ip)`
  entries of that function, not copies (copies blew one segment up to 13.7 MB).
- Deterministic runs must not depend on the host: the game seeds its random numbers from
  the DOS clock, so virtual time uses gmtime (localtime differed between the PC and the
  container).
- `cp` is aliased to `cp -i` in this shell: use `command cp -f`.
- An interrupt request raised outside machine_event must move cpu.deadline to now, or it
  waits for the next timed event (frame events made traces frame-rate dependent).
- Real-time sleeps on the Mini are coarse (a 200 us usleep takes ~8.7 ms): never assume
  usleep() returns on time, account for the time actually slept, and never sleep inside
  a short timed wait of the game. SCORCH_SLEEPQ=6000..10000 imitates such a kernel on the
  PC (then compare held-aim results with and without it).
- `scripts/equivalence.sh [arm]` runs the sessions in `tests/` (cover: menus, options,
  play, quit; mouse: pad pointer; walkfire: the README walkthrough plus a shot; mod2: pad
  aiming with L1/R1) as compiled, interpreter and compiled at 30 fps, and with `arm` the
  Miyoo build under QEMU; every trace must be identical. Run it after any change to the
  runtime or the recompiler.
