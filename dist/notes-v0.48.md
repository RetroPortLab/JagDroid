# JagDroid v0.48-experimental

- **Alien vs Predator**: fixed the Marine campaign doors, which rendered as smeared horizontal streaks or black stair steps. AvP's wall renderer draws a tall texture column in two blits and reads the blitter's A1 pixel pointer back from $F02204 (the blitter returns A1_PIXEL there, and A1_FPIXEL at $F02208) to continue the column. JagDroid returned the A1 flags instead, so the second part sampled the wrong texels.
- Debug builds: `JAG_LOADSTATE` loads a decompressed save state in the desktop tools.
