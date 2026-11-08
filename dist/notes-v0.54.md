# JagDroid v0.54-experimental

- Audio: fixed the brief "slowing down" heard in the BIOS intro and in games. v0.46's pitch-stretch fired on ordinary late frames; it now only engages when the sound queue is nearly empty, eases in and out, and the queue is a little deeper (about 110 ms) so late frames pass unnoticed.
