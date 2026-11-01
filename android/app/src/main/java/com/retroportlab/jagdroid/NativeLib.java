package com.retroportlab.jagdroid;

import android.view.Surface;

/** Thin JNI wrapper around the native emulator core. */
public final class NativeLib {
    static { System.loadLibrary("jagdroid"); }
    private NativeLib() {}

    public static native boolean loadGame(byte[] rom, byte[] bios);
    /**
     * Boots a Jaguar CD disc: cdBios is the CD unit's BIOS, cue the cue sheet's text,
     * and names/fds the track files it lists, already opened (the native side dups them).
     */
    public static native boolean loadDisc(byte[] cdBios, String cue, String[] names, int[] fds);
    /** File names inside a zip (fd stays owned by the caller). */
    public static native String[] zipEntries(int fd);
    /** Loads the first .j64/.jag/.rom in a zip. */
    public static native boolean loadZipGame(int fd, byte[] bios);
    /** Loads a zipped Jaguar CD (cue + bins); compressed tracks unpack into cacheDir. Blocking. */
    public static native boolean loadZipDisc(byte[] cdBios, int fd, String cacheDir);
    /** 0..1 while a zipped disc is unpacking, -1 otherwise. */
    public static native float unpackProgress();
    public static native void cancelUnpack();
    public static native String lastError();
    public static native void setSurface(Surface surface);
    public static native void start();
    public static native void stop();
    public static native void reset();
    public static native void setPaused(boolean paused);
    public static native void setButtons(int mask);
    public static native float getFps();
    /** Emulate a PAL (50 Hz) console instead of NTSC (60 Hz). Takes effect on the next loadGame. */
    public static native void setPal(boolean pal);
    /**
     * Display enhancements: smoothShade keeps the full precision of Gouraud-shaded
     * surfaces; scale 2 draws the picture at twice the size, smoothing shaded
     * surfaces between pixels while edges and 2D art stay sharp.
     */
    public static native void setEnhancements(boolean smoothShade, int scale);
    /** HDR output: highlights reach peak x normal white; scanline dimming is made up. */
    public static native void setHdr(boolean on, float peak);
    /** False if HDR is off or the screen/driver refused the HDR buffer format. */
    public static native boolean hdrActive();
    /** 0 off, 1 light scanlines, 2 strong scanlines, 3 LCD grid. */
    public static native void setScanlines(int mode);
    /** File holding this game's cartridge save chip; set before loadGame. Empty for none. */
    public static native void setEepromPath(String path);
    /** Write the save chip now if the game changed it. */
    public static native void flushSave();
    public static native boolean saveState(String path);
    public static native boolean loadState(String path);
    /** Half-size copy of the current picture: [width, height, ARGB pixels...]. */
    public static native int[] getThumbnail();

    // Must match jag::JoyButton in core/jaguar.h
    public static final int UP = 1 << 0, DOWN = 1 << 1, LEFT = 1 << 2, RIGHT = 1 << 3;
    public static final int A = 1 << 4, B = 1 << 5, C = 1 << 6, PAUSE = 1 << 7, OPTION = 1 << 8;
    public static final int K0 = 1 << 9, K1 = 1 << 10, K2 = 1 << 11, K3 = 1 << 12, K4 = 1 << 13;
    public static final int K5 = 1 << 14, K6 = 1 << 15, K7 = 1 << 16, K8 = 1 << 17, K9 = 1 << 18;
    public static final int STAR = 1 << 19, HASH = 1 << 20;
}
