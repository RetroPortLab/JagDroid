**Fixes**
- Jaguar CD: Primal Rage now boots into the game. The disc data could land four bytes out of place depending on timing, so the game's code ran misaligned and crashed. CD data reads now never pick up a phantom empty word.
- Jaguar CD saves: the CD unit's save chip is kept between sessions in `jaguar-cd-memory-track.bin`, next to your other saves, and shared by all discs.
- JERRY random number register implemented. Without it the CD BIOS hung on its second boot once a save existed.

**Tested on desktop**
- Primal Rage (reaches fights), Myst, Blue Lightning, World Tour Racing and Battle Morph all boot to intro or gameplay.

**Known issues**
- The separate Memory Track cartridge hardware isn't emulated yet.
- Aliens vs Predator: no music on the character-select screen.
- Super Burnout: yellow band in the distance.
- Ruiner Pinball graphics glitches.
- Breakout 2000: most bricks are missing.
