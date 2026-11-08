# JagDroid v0.49-experimental

- **Sound-crash capture** (for Power Drive Rally's sound dropping out in races): the app keeps a rolling set of one-second snapshots, and if the game's sound DSP runs off into non-DSP memory (after which sound stays silent), it saves the snapshot from about 7 seconds earlier. The pause menu then shows **Export sound-crash state**, which copies it to Downloads.
- Desktop tools: `JAG_LOADSTATE`, `JAG_SAVESTATE`, `JAG_DSPWATCH` diagnostics.
