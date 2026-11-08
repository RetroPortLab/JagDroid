# JagDroid v0.56-experimental

- Audio: removed the pitch wobble heard on many games. The queue-level correction used the raw level, which rises and falls with every frame, and bent the pitch by up to about 1%. It now steers from a slow average by at most 0.3%, and only a queue that is genuinely running dry stretches the sound.
