package com.retroportlab.jagdroid;

import android.content.Context;
import android.media.AudioFormat;
import android.media.MediaCodec;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.net.Uri;

import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.ShortBuffer;

/**
 * Turns any audio file Android can decode into a one-track audio CD image
 * (44.1 kHz, 16-bit stereo, raw little-endian) so the Jaguar CD BIOS can play it
 * and drive its VLM visualiser.
 */
final class AudioDisc {
    static volatile boolean cancel;
    static volatile float progress = -1;

    /** Decodes to dir/track.bin and writes dir/disc.cue; returns an error message or null. */
    static String build(Context ctx, Uri uri, File dir) {
        cancel = false; progress = 0;
        dir.mkdirs();
        File bin = new File(dir, "track.bin");
        MediaExtractor ex = new MediaExtractor();
        MediaCodec codec = null;
        long total = 0;
        try (BufferedOutputStream out = new BufferedOutputStream(new FileOutputStream(bin), 1 << 16)) {
            ex.setDataSource(ctx, uri, null);
            int track = -1; MediaFormat fmt = null;
            for (int i = 0; i < ex.getTrackCount(); i++) {
                MediaFormat f = ex.getTrackFormat(i);
                String m = f.getString(MediaFormat.KEY_MIME);
                if (m != null && m.startsWith("audio/")) { track = i; fmt = f; break; }
            }
            if (track < 0) return "That file has no audio track";
            ex.selectTrack(track);
            long durUs = fmt.containsKey(MediaFormat.KEY_DURATION) ? fmt.getLong(MediaFormat.KEY_DURATION) : 0;
            codec = MediaCodec.createDecoderByType(fmt.getString(MediaFormat.KEY_MIME));
            codec.configure(fmt, null, null, 0);
            codec.start();

            int inRate = fmt.getInteger(MediaFormat.KEY_SAMPLE_RATE);
            int inCh = fmt.getInteger(MediaFormat.KEY_CHANNEL_COUNT);
            int enc = AudioFormat.ENCODING_PCM_16BIT;
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            boolean inDone = false, outDone = false;
            double pos = 0;            // position in input frames of the next output sample
            double step = inRate / 44100.0;
            short[] prev = {0, 0};     // last input frame consumed
            long inFrames = 0;
            byte[] buf = new byte[1 << 15]; int bn = 0;
            while (!outDone) {
                if (cancel) return "Cancelled";
                if (!inDone) {
                    int ib = codec.dequeueInputBuffer(10000);
                    if (ib >= 0) {
                        ByteBuffer b = codec.getInputBuffer(ib);
                        int n = ex.readSampleData(b, 0);
                        if (n < 0) { codec.queueInputBuffer(ib, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM); inDone = true; }
                        else { codec.queueInputBuffer(ib, 0, n, ex.getSampleTime(), 0); ex.advance(); }
                    }
                }
                int ob = codec.dequeueOutputBuffer(info, 10000);
                if (ob == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    MediaFormat of = codec.getOutputFormat();
                    inRate = of.getInteger(MediaFormat.KEY_SAMPLE_RATE);
                    inCh = of.getInteger(MediaFormat.KEY_CHANNEL_COUNT);
                    if (of.containsKey(MediaFormat.KEY_PCM_ENCODING)) enc = of.getInteger(MediaFormat.KEY_PCM_ENCODING);
                    step = inRate / 44100.0;
                } else if (ob >= 0) {
                    ByteBuffer ob2 = codec.getOutputBuffer(ob);
                    ob2.order(ByteOrder.LITTLE_ENDIAN);
                    int frames;
                    short[] pcm;
                    if (enc == AudioFormat.ENCODING_PCM_FLOAT) {
                        java.nio.FloatBuffer fb = ob2.asFloatBuffer();
                        int n = Math.min(fb.remaining(), info.size / 4);
                        pcm = new short[n];
                        for (int i = 0; i < n; i++) { float v = fb.get(); pcm[i] = (short) Math.max(-32768, Math.min(32767, Math.round(v * 32767f))); }
                    } else {
                        ShortBuffer sb = ob2.asShortBuffer();
                        int n = Math.min(sb.remaining(), info.size / 2);
                        pcm = new short[n]; sb.get(pcm, 0, n);
                    }
                    frames = pcm.length / Math.max(1, inCh);
                    for (int f = 0; f < frames; f++) {
                        short l = pcm[f * inCh], r = inCh > 1 ? pcm[f * inCh + 1] : l;
                        // Emit every output sample whose position falls between the previous and this input frame.
                        while (pos < inFrames) {
                            double t = pos - (inFrames - 1);   // 0..1 between the previous and current input frame
                            if (t < 0) t = 0;
                            short ol = (short) (prev[0] + (l - prev[0]) * t);
                            short orr = (short) (prev[1] + (r - prev[1]) * t);
                            if (bn + 4 > buf.length) { out.write(buf, 0, bn); bn = 0; }
                            buf[bn++] = (byte) ol; buf[bn++] = (byte) (ol >> 8);
                            buf[bn++] = (byte) orr; buf[bn++] = (byte) (orr >> 8);
                            total++;
                            pos += step;
                        }
                        prev[0] = l; prev[1] = r; inFrames++;
                    }
                    if (durUs > 0) progress = Math.min(0.99f, (float) (info.presentationTimeUs / (double) durUs));
                    codec.releaseOutputBuffer(ob, false);
                    if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) outDone = true;
                }
            }
            // Pad to whole sectors (588 samples) plus a short tail of silence so the end is clean.
            long want = ((total + 588 * 150 + 587) / 588) * 588;
            byte[] z = new byte[4];
            if (bn > 0) { out.write(buf, 0, bn); bn = 0; }
            for (long i = total; i < want; i++) out.write(z, 0, 4);
            out.flush();
            if (total == 0) return "Couldn't decode any audio from that file";
        } catch (Exception e) {
            return "Couldn't decode that file: " + e.getMessage();
        } finally {
            try { if (codec != null) { codec.stop(); codec.release(); } } catch (Exception ignored) { }
            ex.release();
            progress = -1;
        }
        try (java.io.FileWriter w = new java.io.FileWriter(new File(dir, "disc.cue"))) {
            w.write("FILE \"track.bin\" BINARY\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n");
        } catch (Exception e) { return "Couldn't write the disc: " + e.getMessage(); }
        return null;
    }
}
