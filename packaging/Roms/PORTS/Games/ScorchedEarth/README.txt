SCORCHED EARTH 1.5 (1995) FOR MIYOO MINI / MINI PLUS
=====================================================

This is Wendell Hicken's original game, not a remake. Its own program,
SCORCH.EXE from the 1.5 shareware release, was translated instruction by
instruction into native ARM code. The menus, physics, weapons, computer
players, shop and PC speaker sounds are all the original's. Only the 1995
PC around it is new: DOS, the VGA card, keyboard, mouse, timer and speaker
are supplied by the port and mapped to the Mini's screen, buttons and sound.

INSTALL
-------
For OnionOS on the Miyoo Mini, Mini v4 and Mini Plus. Extract the release
zip to the root of the SD card: it holds a Roms folder, which merges with
the card's own. To update, do the same; your settings, market prices and
saved games are kept. To remove the game, delete
Roms/PORTS/Games/ScorchedEarth, Roms/PORTS/Shortcuts/Strategy/Scorched
Earth.port and Roms/PORTS/Imgs/Scorched Earth.png.

START
-----
Games > Ports > Strategy > Scorched Earth. If it is not listed yet, run
"~Import ports" in the Ports list.

The game starts in 320x240, which fills the screen with big, readable
text. For more detail and smaller text: Hardware... > Graphics Mode
640x480 in the main menu, then Save Changes, quit and start again (the
original needs a restart for this).

BUTTONS
-------
  D-pad          arrow keys: aim (left/right turns the turret, up/down
                 sets the power; the numbers run fast while you hold),
                 menus, the shop's marker
  A              Space: fire, buy in the shop
  B              Esc: back, cancel, finish buying
  X              Tab: next weapon (hold L1 for the previous one)
  Y              Enter: OK, Done
  L1 (hold)      Shift: with the D-pad, aim at half speed
  R1 (hold)      Alt: with the D-pad, one step per press (keep holding
                 for slow repeats); best for the final touch
  L2 / R2        Page Down / Page Up: power down / up fast, shop pages
  START          F1: the system menu (save and restore games, new game,
                 mass kill, quit)
  SELECT, tap    on-screen keyboard, for names and the menus' hot keys
                 (the letter shown in colour: S = Start, H = Hardware...)
                 D-pad picks a key, A types it, X backspace, Y shift,
                 START Enter, B or SELECT closes it, L1/R1 move it up
                 or down the screen
  SELECT, hold   mouse pointer: tap the D-pad to move it one pixel, hold
                 to glide (it speeds up the longer you hold); while
                 holding SELECT, A and B click left and right. The
                 game's Mouse Rate (Hardware menu) sets the speed too.
  MENU, tap      mouse click where the pointer is
  MENU, hold 2 s quit at once

To quit the usual way: START, Quit Game, Yes, then A on the closing
screen.

YOUR FIRST GAME
---------------
1. Main menu: tap SELECT, pick S, press A, then B to close the keyboard.
   (Or hold SELECT, steer the pointer onto Start with the D-pad, let go
   and tap MENU.)
2. Player 1 is you: tap SELECT, type your name, press START twice (the
   first accepts the name, the second is Done), then B.
3. Player 2: B, then X switches to a computer player. Tap SELECT, type
   its letter (M for Moron is the friendliest), press START, then B.
4. Your turn: left/right aims, up/down sets the power, A fires. Hold
   the D-pad to get close, then hold R1 and tap it for single steps.
   Win rounds, earn money, buy bigger weapons between rounds (up/down
   picks, A buys, B finishes) and pick them with X.

Hardware... > Bios Keyboard is off, as the original recommends. Turned
on, the D-pad moves one step per press (holding repeats slowly) and L1
and R1 do nothing; that is how the original behaves with that option.

The manual (SCORCH.DOC, the original) explains every weapon, option and
computer opponent.

FILES
-----
SCORCH.CFG holds your settings (delete it to start over with the port's
defaults), SCORCH.MKT the market prices; saved games go here too.
scorch.log is the log of the last run. uncompiled.txt lists code the port
ran in its built-in interpreter instead of as native code; it helps the
next version, nothing to worry about.

Scorched Earth is shareware, (c) 1991-1995 Wendell Hicken.
