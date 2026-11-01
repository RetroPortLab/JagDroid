**Fixes**
- Vertical shake fixed (I-War, Missile Command 3D, Wolfenstein 3D). GPU reads of the console's own chip registers were being counted as main-bus traffic, which starved the 68000 so its once-a-frame display update ran late and the picture jumped for a frame.
- Battle Morph: menus now look right (darkened panels, yellow letters, red and green icons), and the in-game background, creatures and hint boxes no longer smear. The blitter's source-shading mode (SRCSHADE), which adjusts the brightness and colour of copied pixels, is now emulated.

**Known issues**
- AvP: doors in the Marine campaign show black stripes; under investigation.
- Wolfenstein 3D still jolts the screen when you're shot; the game does that deliberately.
