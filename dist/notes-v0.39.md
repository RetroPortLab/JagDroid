**Fixes**
- Breakout 2000: bricks now disappear when the ball hits them. The game erases a brick by copying a cleared save area back over it, which relies on the blitter's Z comparison running during a source-Z copy; we skipped that comparison, so the brick was copied straight back.

**Known issues**
- The separate Memory Track cartridge hardware isn't emulated yet.
- Aliens vs Predator: no music on the character-select screen.
- Super Burnout: yellow band in the distance.
- Ruiner Pinball graphics glitches.
