# JagDroid v0.59-experimental

- Fix: Syndicate and Theme Park no longer crash to a black screen right after the BIOS. The GPU control register now reports its chip version bits; both games use a divide-by-that-value as a delay loop, which trapped when the register read as zero. Both now reach their menus and run.
