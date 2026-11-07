**New**
- Memory Track cartridge: CD games that save to the Memory Track (Battle Morph, Blue Lightning, Primal Rage and others) now find it, create their save files and read them back on the next boot. It is emulated at the level the games use it (the cartridge's NVM BIOS module and its 128 KB filesystem), so no cartridge ROM is needed. Saves are kept in `jaguar-cd-memory-track-cartridge.bin`.

**Known issues**
- The Memory Track's own manager screen (hold Option at power-on) isn't emulated.
- Aliens vs Predator: no music on the character-select screen.
- Ruiner Pinball graphics glitches.
- Old save states from earlier versions still load for cartridge games; CD game states made before this version may behave oddly because the module now lives in RAM.
