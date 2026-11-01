**Fixes**
- Fight for Life: the menu no longer bounces up and down. Long blits froze the 68000 so the video interrupt ran tens of lines late; the blitter now leaves the CPU a share of the bus.
- Sound: all audio is resampled to a fixed 48 kHz with smoothing before it reaches Android, to stop the BIOS jingle (and other low-rate sound) crackling.

**Known issues**
- Aliens vs Predator: no sound on the character-select screen (still investigating).
- Super Burnout: yellow band in the distance.
- Ruiner Pinball graphics glitches.
- Breakout 2000: most bricks are missing.
- AvP Marine campaign doors.
