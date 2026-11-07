package com.retroportlab.jagdroid;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.util.ArrayList;
import java.util.List;

/**
 * Reads and edits the Memory Track cartridge's 128 KB save file (the same layout the core's
 * NVM BIOS module uses): 256 blocks of 512 bytes, block 0 = FAT (one byte per block) with a
 * checksum in bytes 0-1 and the used-block count in byte 3, blocks 1-7 = directory
 * (199 entries of 18 bytes), blocks 8+ = data.
 */
final class MemoryTrack {
    static final int SIZE = 128 * 1024, BLOCK = 512, DATA_BLOCKS = 248, DIR_OFFSET = 512, DIR_ENTRY = 18, DIR_ENTRIES = 199;
    private static final String CHARSET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:'. ";

    static final class Entry {
        int index, start, blocks;
        String app, file;
        long bytes() { return (long) blocks * BLOCK; }
    }

    private final File path;
    private byte[] data;
    boolean formatted;   // false: no file yet or a damaged one (the game formats it on first use)

    MemoryTrack(File path) {
        this.path = path;
        data = new byte[SIZE];
        java.util.Arrays.fill(data, (byte) 0xFF);
        try (FileInputStream in = new FileInputStream(path)) {
            byte[] d = new byte[SIZE];
            int n = 0, r;
            while (n < SIZE && (r = in.read(d, n, SIZE - n)) > 0) n += r;
            if (n > 0) System.arraycopy(d, 0, data, 0, n);
        } catch (Exception ignored) { }
        formatted = ((get(0) << 8 | get(1)) & 0xFFFF) == checksum();
    }

    private int get(int a) { return data[a] & 0xFF; }
    private void put(int a, int v) { data[a] = (byte) v; }

    private int checksum() {
        int s = 0;
        for (int i = 2; i < BLOCK; i++) s = (s + get(i)) & 0xFFFF;
        return s;
    }

    private String name(int a, int words) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < words; i++) {
            int v = (get(a + i * 2) << 8) | get(a + i * 2 + 1);
            char[] c = new char[3];
            c[2] = CHARSET.charAt(v % 40); v /= 40;
            c[1] = CHARSET.charAt(v % 40); v /= 40;
            c[0] = CHARSET.charAt(v % 40);
            sb.append(c);
        }
        return sb.toString().trim();
    }

    List<Entry> list() {
        List<Entry> out = new ArrayList<>();
        if (!formatted) return out;
        for (int n = 0; n < DIR_ENTRIES; n++) {
            int a = DIR_OFFSET + n * DIR_ENTRY;
            if (get(a) == 0) continue;
            Entry e = new Entry();
            e.index = n; e.start = get(a); e.blocks = get(a + 1);
            e.app = name(a + 2, 5); e.file = name(a + 12, 3);
            out.add(e);
        }
        return out;
    }

    int usedBlocks() { return formatted ? Math.min(get(3), DATA_BLOCKS) : 0; }
    long totalBytes() { return (long) DATA_BLOCKS * BLOCK; }
    long freeBytes() { return (long) (DATA_BLOCKS - usedBlocks()) * BLOCK; }

    private void setChecksum() { int s = checksum(); put(0, s >> 8); put(1, s & 0xFF); }

    void delete(Entry e) {
        if (!formatted) return;
        int a = DIR_OFFSET + e.index * DIR_ENTRY;
        put(a, 0); put(a + 1, 0);
        int d2 = e.start, used = get(3);
        while (used != 0 && d2 > 2) {   // walk the FAT chain, freeing each block
            int next = get(d2);
            put(d2, 0);
            d2 = next;
            used--;
        }
        put(3, used);
        setChecksum();
        save();
    }

    void eraseAll() {
        for (int i = 0; i < 8 * BLOCK; i++) put(i, 0);   // an all-zero first 4 KB is a valid empty cartridge
        formatted = true;
        save();
    }

    private void save() {
        File tmp = new File(path.getPath() + ".tmp");
        try (FileOutputStream out = new FileOutputStream(tmp)) {
            out.write(data);
            out.getFD().sync();
        } catch (Exception e) { return; }
        tmp.renameTo(path);
    }
}
