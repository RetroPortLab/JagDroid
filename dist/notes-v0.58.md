# JagDroid v0.58-experimental

- Fix: World Tour Racing (Jaguar CD) no longer hangs after the intro. The game waits for the blitter to report extra status bits when idle; it now does.
- Audio: restored the v0.45 sound steering (70 ms queue, simple level-based correction) after reports that sound had got worse since then, and the periodic safety snapshots now happen every 10 seconds instead of every 2, to avoid small hitches.
