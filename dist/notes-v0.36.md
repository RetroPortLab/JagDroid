**Fixes**
- Sound: output now goes through a buffered queue that the sound card pulls from in its own callback, with a gentle fade on any gap and a slight speed trim to hold the queue level. The old path wrote straight to the stream, so any late emulation frame became an audible pop or scratch (heard clearly in the BIOS jingle).

**Known issues**
- Aliens vs Predator: no music on the character-select screen.
- Super Burnout: yellow band in the distance.
- Ruiner Pinball graphics glitches.
- Breakout 2000: most bricks are missing.
- AvP Marine campaign doors.
