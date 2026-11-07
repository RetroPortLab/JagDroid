## JagDroid v0.44 (experimental)

**Battle Morph: the mountains and sky are fixed.**

- The mountain backdrop in Battle Morph's flight levels was sheared sideways into diagonal streaks. It now draws as clean mesas above a flat horizon, matching real hardware.
- Cause: when the blitter copies from a source that sits at a different position inside its 64-bit phrase than the destination, the source pointer now advances past the extra phrase the hardware reads, so the game's row step lands correctly.
- Checked against the 57-game regression set: no other game changes.
