package com.retroportlab.jagdroid;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInstaller;
import android.net.Uri;
import android.os.Build;
import android.provider.Settings;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;

/**
 * Checks the project's GitHub releases for a newer JagDroid and installs it.
 *
 * Releases are tagged "v0.27" and so on, with the APK attached as an asset.
 * The download goes to the app's cache and is handed to the system installer,
 * which asks the player to confirm. Android only accepts the update if it is
 * signed with the same key as the installed app.
 */
final class Updater {
    static final String REPO = "RetroPortLab/jagdroid";
    static final String RELEASES_PAGE = "https://github.com/" + REPO + "/releases";
    private static final String API = "https://api.github.com/repos/" + REPO + "/releases?per_page=10";

    /** A published release that has an APK attached. */
    static final class Release {
        String tag, name, notes, apkUrl, pageUrl;
        long apkSize;
    }

    interface Progress { boolean update(long done, long total); }   // return false to cancel

    private Updater() { }

    /** Version numbers from a name like "0.26-experimental" or a tag like "v0.26": {0, 26}. */
    static int[] versionParts(String v) {
        if (v == null) return new int[0];
        String s = v.trim();
        if (s.startsWith("v") || s.startsWith("V")) s = s.substring(1);
        int end = 0;
        while (end < s.length() && (Character.isDigit(s.charAt(end)) || s.charAt(end) == '.')) end++;
        String[] bits = s.substring(0, end).split("\\.");
        int n = 0;
        int[] out = new int[bits.length];
        for (String b : bits) if (!b.isEmpty()) out[n++] = Integer.parseInt(b);
        int[] trimmed = new int[n];
        System.arraycopy(out, 0, trimmed, 0, n);
        return trimmed;
    }

    /** True if version a is newer than version b. */
    static boolean newer(String a, String b) {
        int[] x = versionParts(a), y = versionParts(b);
        for (int i = 0; i < Math.max(x.length, y.length); i++) {
            int p = i < x.length ? x[i] : 0, q = i < y.length ? y[i] : 0;
            if (p != q) return p > q;
        }
        return false;
    }

    private static HttpURLConnection open(String url) throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setConnectTimeout(15000);
        c.setReadTimeout(60000);
        c.setRequestProperty("User-Agent", "JagDroid-updater");
        c.setInstanceFollowRedirects(true);   // asset downloads redirect to GitHub's storage host
        return c;
    }

    /** The newest release with an APK, or null if there are none. Runs on a background thread. */
    static Release latest() throws IOException {
        HttpURLConnection c = open(API);
        c.setRequestProperty("Accept", "application/vnd.github+json");
        try {
            int code = c.getResponseCode();
            if (code == 404) throw new IOException("No releases have been published yet");
            if (code == 403) throw new IOException("GitHub is limiting requests right now; try again later");
            if (code != 200) throw new IOException("GitHub replied " + code);
            ByteArrayOutputStream buf = new ByteArrayOutputStream();
            try (InputStream in = c.getInputStream()) {
                byte[] b = new byte[16384];
                for (int n; (n = in.read(b)) > 0; ) buf.write(b, 0, n);
            }
            JSONArray list = new JSONArray(buf.toString("UTF-8"));
            Release best = null;
            for (int i = 0; i < list.length(); i++) {
                JSONObject r = list.getJSONObject(i);
                if (r.optBoolean("draft")) continue;
                JSONArray assets = r.optJSONArray("assets");
                if (assets == null) continue;
                for (int k = 0; k < assets.length(); k++) {
                    JSONObject a = assets.getJSONObject(k);
                    if (!a.optString("name").toLowerCase().endsWith(".apk")) continue;
                    Release rel = new Release();
                    rel.tag = r.optString("tag_name");
                    rel.name = r.optString("name", rel.tag);
                    rel.notes = r.optString("body", "");
                    rel.pageUrl = r.optString("html_url", RELEASES_PAGE);
                    rel.apkUrl = a.optString("browser_download_url");
                    rel.apkSize = a.optLong("size");
                    if (best == null || newer(rel.tag, best.tag)) best = rel;
                    break;
                }
            }
            return best;
        } catch (org.json.JSONException e) {
            throw new IOException("Couldn't read GitHub's reply");
        } finally {
            c.disconnect();
        }
    }

    /** Downloads the release's APK into the cache. Returns null if cancelled. */
    static File download(Context ctx, Release r, Progress p) throws IOException {
        IOException last = null;
        for (int attempt = 1; attempt <= 3; attempt++) {
            try {
                return downloadOnce(ctx, r, p);
            } catch (IOException e) {
                last = e;
                try { Thread.sleep(1500L * attempt); } catch (InterruptedException ie) { break; }
            }
        }
        String why = last.getMessage() != null ? last.getMessage() : "no details";
        throw new IOException("Download failed: " + last.getClass().getSimpleName() + " - " + why + " (tried 3 times)");
    }

    private static File downloadOnce(Context ctx, Release r, Progress p) throws IOException {
        File dir = new File(ctx.getCacheDir(), "update");
        dir.mkdirs();
        File[] old = dir.listFiles();
        if (old != null) for (File f : old) f.delete();
        File out = new File(dir, "JagDroid-" + r.tag + ".apk");
        HttpURLConnection c = open(r.apkUrl);
        try {
            if (c.getResponseCode() != 200) throw new IOException("Download failed (" + c.getResponseCode() + ")");
            long total = c.getContentLengthLong() > 0 ? c.getContentLengthLong() : r.apkSize, done = 0;
            try (InputStream in = c.getInputStream(); OutputStream o = new FileOutputStream(out)) {
                byte[] b = new byte[65536];
                for (int n; (n = in.read(b)) > 0; ) {
                    o.write(b, 0, n);
                    done += n;
                    if (p != null && !p.update(done, total)) { o.close(); out.delete(); return null; }
                }
            }
            if (total > 0 && done != total) { out.delete(); throw new IOException("Download was cut short"); }
            return out;
        } finally {
            c.disconnect();
        }
    }

    /** Whether Android lets JagDroid install apps; if not, the player has to allow it once in Settings. */
    static boolean canInstall(Context ctx) {
        return Build.VERSION.SDK_INT < 26 || ctx.getPackageManager().canRequestPackageInstalls();
    }

    static void openInstallPermission(Activity a) {
        a.startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:" + a.getPackageName())));
    }

    /** Hands the APK to the system installer; it shows its own confirmation, then restarts into the new version. */
    static void install(Context ctx, File apk) throws IOException {
        PackageInstaller pi = ctx.getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        params.setAppPackageName(ctx.getPackageName());
        params.setSize(apk.length());
        if (Build.VERSION.SDK_INT >= 31) params.setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED);
        int id = pi.createSession(params);
        try (PackageInstaller.Session s = pi.openSession(id)) {
            try (InputStream in = new FileInputStream(apk); OutputStream out = s.openWrite("jagdroid.apk", 0, apk.length())) {
                byte[] b = new byte[65536];
                for (int n; (n = in.read(b)) > 0; ) out.write(b, 0, n);
                s.fsync(out);
            }
            Intent result = new Intent(ctx, UpdateReceiver.class);
            int flags = PendingIntent.FLAG_UPDATE_CURRENT | (Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0);
            PendingIntent pending = PendingIntent.getBroadcast(ctx, id, result, flags);
            s.commit(pending.getIntentSender());
        }
    }
}
