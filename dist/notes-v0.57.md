# JagDroid v0.57-experimental

- New: Jaguar executables (.jag files) now load and run. These are programs that are loaded straight into memory rather than cartridge images, for example homebrew demos and games. They start without the BIOS intro. The Native Demo / "Native" shooter by Durandik (1997) now runs: credits, title screen and gameplay. It has no sound in these emulated parts.
- Fix: a timing race between the 68000 and the sound processor. When the 68000 handed the sound processor a job and checked straight away for its busy flag, our processor had not yet had its turn, so the 68000 carried on and the job stalled forever. The sound processor now gets a short burst of time when the 68000 writes the word it is waiting on.
