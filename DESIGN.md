# KIST: design decisions

The design as agreed before the build, and why. Changes go here first.

**Platform.** fm1-base on felucca-bsp: open (MPL-2.0), no closed code, ~430 KB of RAM for the app. Not M-VAVE's
FM1 Open: its closed SDK carries the whole groovebox engine (~200 KB of RAM) and cannot be left out. Not a fork of
the Felucca app: fm1-base is its platform without the 13 engines.

**Hardware facts it rests on.** 512 KB of RAM, no SDRAM. 360 MHz. A single-precision FPU (doubles in software, so
none are used). Audio at 24 MHz / 544 = 44,117.6 Hz, in 256-frame blocks. Two flash chips in the field (Puya P25Q80
in older units, UC25HQ80 in newer ones). A second core that works on some units and misreads shared memory on
others. 32x32->64-bit multiplies beside loads can corrupt a register (felucca-bsp docs/CPU.md): one more reason for
float, not fixed point.

**The organ.** One manual, 54 keys (C..f'''), after a Klop continuo organ: Gedackt 8', Rohrflöte 4', Principal 2',
Quint 2 2/3' (treble), all wood, split bass | treble. No reed, no pedal, no couplers, no combinations (four stops are
simple enough to draw by hand).

**Polyphony.** 12 notes: ten fingers and a margin for released pipes still dying away. At most 4 pipes a note:
48 pipes. The least important pipes go first when there is not enough (Quint, then Principal, Rohrflöte,
Gedackt).

**Expression.** No MPE: a tracker action cannot shape a held note. Velocity sets the pallet's opening time; TOUCH
sets the range. The sustain pedal is ignored.

**Tuning.** A = 300.00..500.99 Hz (K1 whole hertz, K2 hundredths). The top was capped at 500 because the highest
pipes have only ~7 samples a period at 44.1 kHz; raise it if measurements on the device allow. Four 12-note
temperaments with a root, and Equal with 5..72 divisions (12 the default, so no separate equal temperament). No
Scala import.

**The room.** Algorithmic only (early reflections, input diffusion, an 8-line feedback delay network, slow
modulation): convolution does not fit 512 KB beside the pipes. Five rooms.

**The second core.** Optional by nature: the organ plays fully on one. When it runs: the pipes above 1.5 kHz at 2x
(cleaner top), part of the 1x pipes, the richer room. Its output is checked before use; it is retired at the first
failure. Not a setting: the System page shows its state.

**Panel.** As drawn on the design canvas: the stops on the top row, HOME / SAVE / ARP (Quint) / SEQ (tremulant) /
PLAY (blower) / REC (cancel) on the bottom; SELECT pages, PRESETS room mix, ALGORITHM temperament.
