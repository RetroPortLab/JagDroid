package com.retroportlab.jagdroid;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.text.format.DateUtils;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;

public class MainActivity extends Activity {
    // ---------------------------------------------------------------- look --
    private static final int BG = 0xFF0D0E12, PANEL = 0xFF181A21, PANEL_FOCUS = 0xFF262A35;
    private static final int ACCENT = 0xFFE4002B, TEXT = 0xFFF2F2F5, SUBTEXT = 0xFF9AA0AE, BADGE = 0xFF2E7D4F;

    private static final int PICK_GAME = 1, PICK_BIOS = 2, PICK_FOLDER = 3, PICK_CDBIOS = 4, PICK_AUDIO = 5;
    private static final String[] REGION_NAMES = {"Auto", "NTSC (60 Hz)", "PAL (50 Hz)"};
    private static final String[] SCANLINE_NAMES = {"Off", "Light", "Strong", "LCD grid"};
    private static final String[] SCALE_NAMES = {"1x", "2x"};
    private static final String[] HDR_PEAK_NAMES = {"2x", "3x", "4x"};
    private static final float[] HDR_PEAKS = {2f, 3f, 4f};
    private static final int SLOTS = 4;
    private static final int SCREEN_LIBRARY = 0, SCREEN_SETTINGS = 1, SCREEN_GAME = 2, SCREEN_UPDATE = 3, SCREEN_MEMTRACK = 4;

    private SharedPreferences prefs;
    private FrameLayout root;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private int screen = SCREEN_LIBRARY;

    // updates
    private Updater.Release foundUpdate = null;   // a newer release found this session
    private volatile boolean updateBusy = false, updateCancel = false;

    // in-game state
    private boolean menuOpen = false;
    private int keyMask = 0, axisMask = 0;
    private String gameKey = null, gameTitle = null;
    private View pauseMenu = null;
    private TextView fpsView;
    private final Runnable fpsTick = new Runnable() {
        @Override public void run() {
            if (screen != SCREEN_GAME || fpsView == null) return;
            fpsView.setText(String.format(Locale.US, "%.1f FPS", NativeLib.getFps()));
            ui.postDelayed(this, 500);
        }
    };

    /** One cartridge file in the library. */
    static final class Game {
        final Uri uri; final String fileName, title, tags;
        // Jaguar CD discs: the cue sheet's folder, for finding its track files.
        Uri tree; String folderId;
        boolean zipDisc;           // a .zip holding a cue sheet and tracks
        Game(Uri uri, String fileName) { this(uri, fileName, false); }
        Game(Uri uri, String fileName, boolean zipDisc) {
            this.zipDisc = zipDisc;
            this.uri = uri; this.fileName = fileName;
            String base = fileName.replaceFirst("\\.[^.]+$", "");
            // Download sites often turn spaces into underscores ("Battle_Morph__USA_").
            if (!base.contains(" ") && base.contains("_")) base = base.replace('_', ' ').replaceAll(" +", " ").trim();
            int p = base.indexOf(" (");
            title = (p > 0 ? base.substring(0, p) : base).trim();
            String t = p > 0 ? base.substring(p + 1).replace(")(", ", ").replace(") (", ", ").replace("(", "").replace(")", "").trim() : "";
            if (isCd()) t = t.isEmpty() ? "Jaguar CD" : "Jaguar CD, " + t;
            tags = t;
        }
        boolean isCd() { return zipDisc || fileName.toLowerCase(Locale.US).endsWith(".cue"); }
        boolean isZip() { return fileName.toLowerCase(Locale.US).endsWith(".zip"); }
        String key() { return sanitize(fileName.replaceFirst("\\.[^.]+$", "")); }
    }

    static String sanitize(String s) { return s.replaceAll("[^A-Za-z0-9 ._-]", "_"); }

    // ------------------------------------------------------------ lifecycle --
    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        prefs = getSharedPreferences("jagdroid", MODE_PRIVATE);
        root = new FrameLayout(this);
        root.setBackgroundColor(BG);
        setContentView(root);
        hideSystemUi();
        NativeLib.setScanlines(prefs.getInt("scanlines", 0));
        NativeLib.setCropOverscan(prefs.getBoolean("cropOverscan", true));
        Uri launched = getIntent() != null ? getIntent().getData() : null;
        if (launched != null) startGame(launched, null); else showLibrary();
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        if (intent.getData() != null) startGame(intent.getData(), null);
    }

    @Override protected void onPause() { super.onPause(); NativeLib.setPaused(true); }
    @Override protected void onResume() {
        super.onResume();
        hideSystemUi();
        if (screen == SCREEN_GAME && !menuOpen) NativeLib.setPaused(false);
    }
    @Override protected void onDestroy() { NativeLib.stop(); super.onDestroy(); }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemUi();
    }

    // ------------------------------------------------------------- widgets --
    private int dp(float v) { return Math.round(v * getResources().getDisplayMetrics().density); }

    private GradientDrawable rounded(int fill, int stroke) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(fill);
        g.setCornerRadius(dp(10));
        if (stroke != 0) g.setStroke(dp(2), stroke);
        return g;
    }

    /** Card background that lights up with an accent outline when focused by the gamepad. */
    private StateListDrawable cardBg() {
        StateListDrawable s = new StateListDrawable();
        s.addState(new int[]{android.R.attr.state_focused}, rounded(PANEL_FOCUS, ACCENT));
        s.addState(new int[]{android.R.attr.state_pressed}, rounded(PANEL_FOCUS, ACCENT));
        s.addState(new int[]{}, rounded(PANEL, 0));
        return s;
    }

    private TextView text(String s, float sp, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(sp);
        t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    /** A focusable row with a title, optional subtitle and optional value on the right. */
    private LinearLayout row(String title, String subtitle, String value, View.OnClickListener click) {
        LinearLayout r = new LinearLayout(this);
        r.setOrientation(LinearLayout.HORIZONTAL);
        r.setGravity(Gravity.CENTER_VERTICAL);
        r.setPadding(dp(16), dp(10), dp(16), dp(10));
        r.setBackground(cardBg());
        r.setFocusable(true);
        r.setClickable(true);
        r.setOnClickListener(click);
        LinearLayout texts = new LinearLayout(this);
        texts.setOrientation(LinearLayout.VERTICAL);
        texts.addView(text(title, 17, TEXT, true));
        if (subtitle != null && !subtitle.isEmpty()) texts.addView(text(subtitle, 13, SUBTEXT, false));
        r.addView(texts, new LinearLayout.LayoutParams(0, -2, 1));
        if (value != null) {
            TextView v = text(value, 15, ACCENT, true);
            v.setTag("value");
            r.addView(v);
        }
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
        lp.bottomMargin = dp(6);
        r.setLayoutParams(lp);
        return r;
    }

    private void setRowValue(View rowView, String value) {
        View v = rowView.findViewWithTag("value");
        if (v instanceof TextView) ((TextView) v).setText(value);
    }

    private TextView sectionTitle(String s) {
        TextView t = text(s.toUpperCase(Locale.US), 12, SUBTEXT, true);
        t.setLetterSpacing(0.12f);
        t.setPadding(dp(4), dp(14), 0, dp(6));
        return t;
    }

    /** Header bar: logo, a subtitle and a few action buttons. */
    private LinearLayout header(String subtitle, View... actions) {
        LinearLayout h = new LinearLayout(this);
        h.setOrientation(LinearLayout.HORIZONTAL);
        h.setGravity(Gravity.CENTER_VERTICAL);
        h.setPadding(dp(24), dp(16), dp(24), dp(8));
        TextView logo = text("JAG", 26, ACCENT, true);
        logo.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD_ITALIC));
        TextView logo2 = text("DROID", 26, TEXT, true);
        logo2.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD_ITALIC));
        h.addView(logo);
        h.addView(logo2);
        h.addView(text("   " + subtitle, 14, SUBTEXT, false), new LinearLayout.LayoutParams(0, -2, 1));
        for (View a : actions) {
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-2, -2);
            lp.leftMargin = dp(8);
            h.addView(a, lp);
        }
        return h;
    }

    private TextView pill(String label, View.OnClickListener click) {
        TextView b = text(label, 14, TEXT, true);
        b.setPadding(dp(16), dp(8), dp(16), dp(8));
        b.setBackground(cardBg());
        b.setFocusable(true);
        b.setClickable(true);
        b.setOnClickListener(click);
        return b;
    }

    private ScrollView scroller(LinearLayout content) {
        ScrollView sv = new ScrollView(this);
        sv.setFillViewport(true);
        sv.setPadding(dp(24), 0, dp(24), dp(16));
        sv.setClipToPadding(false);
        sv.addView(content);
        return sv;
    }

    private LinearLayout page(String subtitle, LinearLayout list, View... actions) {
        root.removeAllViews();
        root.setBackgroundColor(BG);
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.addView(header(subtitle, actions));
        list.setOrientation(LinearLayout.VERTICAL);
        page.addView(scroller(list), new LinearLayout.LayoutParams(-1, 0, 1));
        TextView foot = text("A  select     B  back     Select + Start  in-game menu", 12, SUBTEXT, false);
        foot.setPadding(dp(28), dp(4), 0, dp(10));
        page.addView(foot);
        root.addView(page, new FrameLayout.LayoutParams(-1, -1));
        return page;
    }

    private String versionName() {
        try { return getPackageManager().getPackageInfo(getPackageName(), 0).versionName; }
        catch (Exception e) { return "?"; }
    }

    // ------------------------------------------------------------- library --
    private void showLibrary() {
        NativeLib.stop();
        screen = SCREEN_LIBRARY; menuOpen = false; pauseMenu = null;
        applyHdr();
        ui.removeCallbacks(fpsTick);
        LinearLayout list = new LinearLayout(this);
        page("v" + versionName(), list, pill("Music (VLM)", v -> pick(PICK_AUDIO)), pill("Open file", v -> pick(PICK_GAME)), pill("Settings", v -> showSettings()));

        String folder = prefs.getString("folder", null);
        if (folder == null) {
            list.addView(sectionTitle("Welcome"));
            TextView intro = text("Choose the folder that holds your Jaguar games (.j64, .jag, .rom or .zip files, and .cue "
                    + "discs for Jaguar CD) and they'll appear here. A BIOS is optional; CD games need the CD BIOS, "
                    + "which you can add in Settings.", 15, SUBTEXT, false);
            intro.setPadding(dp(4), 0, dp(4), dp(12));
            list.addView(intro);
            View choose = row("Choose games folder", "Pick the folder once; JagDroid remembers it", null, v -> pick(PICK_FOLDER));
            list.addView(choose);
            list.addView(row("Open a single game file", null, null, v -> pick(PICK_GAME)));
            choose.requestFocus();
            return;
        }

        if (foundUpdate != null) {
            list.addView(updateBanner());
        } else {
            autoCheckForUpdate(list);
        }
        TextView scanning = text("Looking for games…", 15, SUBTEXT, false);
        scanning.setPadding(dp(4), dp(16), 0, 0);
        list.addView(scanning);
        Uri tree = Uri.parse(folder);
        new Thread(() -> {
            List<Game> games = scanFolder(tree);
            ui.post(() -> { if (screen == SCREEN_LIBRARY && list.isAttachedToWindow()) fillLibrary(list, games); });
        }).start();
    }

    private List<Game> scanFolder(Uri tree) {
        List<Game> out = new ArrayList<>();
        try {
            scanDir(tree, DocumentsContract.getTreeDocumentId(tree), out, true);
        } catch (Exception ignored) { }
        Collections.sort(out, (a, b) -> a.title.compareToIgnoreCase(b.title));
        return out;
    }

    /** Cartridges and CD cue sheets in one folder; CD games usually sit in a folder each, so look one level down. */
    private void scanDir(Uri tree, String dirId, List<Game> out, boolean descend) {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, dirId);
        try (Cursor c = getContentResolver().query(children, new String[]{
                DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE}, null, null, null)) {
            while (c != null && c.moveToNext()) {
                String id = c.getString(0), name = c.getString(1);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(c.getString(2))) {
                    if (descend) scanDir(tree, id, out, false);
                    continue;
                }
                String lower = name.toLowerCase(Locale.US);
                if (looksLikeBios(lower)) continue;
                if (lower.endsWith(".j64") || lower.endsWith(".jag") || lower.endsWith(".rom")) {
                    if (descend) out.add(new Game(DocumentsContract.buildDocumentUriUsingTree(tree, id), name));
                } else if (lower.endsWith(".zip")) {
                    Uri u = DocumentsContract.buildDocumentUriUsingTree(tree, id);
                    int kind = zipKind(u);   // 1 cartridge, 2 disc
                    if (kind == 1) out.add(new Game(u, name, false));
                    else if (kind == 2) out.add(new Game(u, name, true));
                } else if (lower.endsWith(".cue")) {
                    Game g = new Game(DocumentsContract.buildDocumentUriUsingTree(tree, id), name);
                    g.tree = tree; g.folderId = dirId;
                    out.add(g);
                }
            }
        } catch (Exception ignored) { }
    }

    /** BIOS images sitting in the games folder are not games. */
    static boolean looksLikeBios(String lower) {
        return lower.contains("bios") || lower.startsWith("jagboot") || lower.startsWith("jagcd")
                || lower.contains("jaguar cd boot") || lower.contains("[bios]");
    }

    /** 1 if the zip holds a cartridge, 2 if it holds a CD (cue sheet), 0 otherwise. */
    private int zipKind(Uri u) {
        try (android.os.ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(u, "r")) {
            if (pfd == null) return 0;
            int kind = 0;
            for (String n : NativeLib.zipEntries(pfd.getFd())) {
                String l = n.toLowerCase(Locale.US);
                if (l.endsWith(".cue")) return 2;
                if (l.endsWith(".j64") || l.endsWith(".jag") || l.endsWith(".rom")) kind = 1;
            }
            return kind;
        } catch (Exception e) { return 0; }
    }

    private void fillLibrary(LinearLayout list, List<Game> games) {
        list.removeAllViews();
        if (foundUpdate != null) list.addView(updateBanner());
        if (games.isEmpty()) {
            list.addView(sectionTitle("No games found"));
            list.addView(text("That folder has no .j64, .jag, .rom, .cue or .zip games, or access to it was lost.", 15, SUBTEXT, false));
            View r = row("Choose games folder", null, null, v -> pick(PICK_FOLDER));
            list.addView(r);
            r.requestFocus();
            return;
        }
        View first = null;
        // Continue playing: the most recent few games that are still in the folder.
        List<Game> recent = new ArrayList<>();
        for (String key : recentKeys()) for (Game g : games) if (g.key().equals(key)) { recent.add(g); break; }
        if (!recent.isEmpty()) {
            list.addView(sectionTitle("Continue playing"));
            for (int i = 0; i < recent.size() && i < 3; i++) {
                View r = gameRow(recent.get(i));
                list.addView(r);
                if (first == null) first = r;
            }
        }
        list.addView(sectionTitle("All games  ·  " + games.size()));
        for (Game g : games) {
            View r = gameRow(g);
            list.addView(r);
            if (first == null) first = r;
        }
        first.requestFocus();
    }

    private View gameRow(Game g) {
        StringBuilder sub = new StringBuilder(g.tags);
        long last = prefs.getLong("played_" + g.key(), 0);
        if (last > 0) sub.append(sub.length() > 0 ? "  ·  " : "").append("Played ")
                .append(DateUtils.getRelativeTimeSpanString(last, System.currentTimeMillis(), DateUtils.MINUTE_IN_MILLIS));
        boolean hasSave = new File(saveDir(), g.key() + ".eep").exists();
        boolean hasState = false;
        for (int s = 1; s <= SLOTS; s++) hasState |= stateFile(g.key(), s).exists();
        String badge = hasState ? "SAVE STATE" : (hasSave ? "SAVE" : null);
        LinearLayout r = row((g.isCd() ? "\uD83D\uDCBF  " : "") + g.title, sub.toString(), badge, v -> launch(g));
        if (badge != null) {
            TextView b = (TextView) r.findViewWithTag("value");
            b.setTextSize(11);
            b.setTextColor(TEXT);
            b.setPadding(dp(8), dp(3), dp(8), dp(3));
            GradientDrawable bg = new GradientDrawable();
            bg.setColor(BADGE); bg.setCornerRadius(dp(6));
            b.setBackground(bg);
        }
        return r;
    }

    private List<String> recentKeys() {
        List<String> out = new ArrayList<>();
        for (String k : prefs.getString("recent", "").split("\n")) if (!k.isEmpty()) out.add(k);
        return out;
    }

    private void markPlayed(String key) {
        List<String> r = recentKeys();
        r.remove(key);
        r.add(0, key);
        while (r.size() > 8) r.remove(r.size() - 1);
        prefs.edit().putString("recent", String.join("\n", r)).putLong("played_" + key, System.currentTimeMillis()).apply();
    }

    // ------------------------------------------------------------ settings --
    private void showSettings() {
        screen = SCREEN_SETTINGS;
        LinearLayout list = new LinearLayout(this);
        page("Settings", list, pill("Back", v -> showLibrary()));

        list.addView(sectionTitle("Library"));
        String folder = prefs.getString("folder", null);
        View first = row("Games folder", folder == null ? "Not chosen" : Uri.parse(folder).getLastPathSegment(), "Change", v -> pick(PICK_FOLDER));
        list.addView(first);

        list.addView(sectionTitle("Display"));
        list.addView(cycleRow("Scanlines", "CRT-style lines between the picture's rows", "scanlines", SCANLINE_NAMES,
                NativeLib::setScanlines));
        list.addView(toggleRow("Crop overscan", "Hide the picture's outer border, which a TV would cut off (some games leave junk there)",
                "cropOverscan", true, () -> NativeLib.setCropOverscan(prefs.getBoolean("cropOverscan", true))));
        list.addView(toggleRow("Smooth Gouraud shading", "Full-precision colour on smoothly shaded 3D surfaces (removes banding)",
                "smoothShade", false, this::applyEnhancements));
        list.addView(cycleRow("Render scale", "2x doubles the picture and smooths shaded surfaces between pixels; "
                + "polygon edges stay at the Jaguar's resolution", "renderScale", SCALE_NAMES, v -> applyEnhancements()));
        boolean hdrScreen = screenSupportsHdr();
        list.addView(toggleRow("HDR", hdrScreen
                        ? "Brighter highlights and full-brightness scanlines on this HDR screen"
                        : "This screen doesn't report HDR support, so this probably won't do anything",
                "hdr", false, this::applyHdr));
        list.addView(cycleRow("HDR highlight boost", "How much brighter than normal white the brightest parts get",
                "hdrPeak", HDR_PEAK_NAMES, v -> applyHdr()));
        list.addView(toggleRow("Show FPS counter", "Frames per second in the corner while playing", "showFps", false));

        list.addView(sectionTitle("System"));
        list.addView(cycleRow("Console region", "Auto picks PAL only for files marked Europe/PAL", "region", REGION_NAMES, null));
        String bios = prefs.getString("bios", null);
        list.addView(row("BIOS file", bios == null ? "None: games boot directly" : Uri.parse(bios).getLastPathSegment(), "Choose", v -> pick(PICK_BIOS)));
        list.addView(toggleRow("Boot with BIOS", "Show the Jaguar start-up logo (needs a BIOS file)", "useBios", true));
        String cdBios = prefs.getString("cdbios", null);
        list.addView(row("Jaguar CD BIOS", cdBios == null ? "Not chosen: needed for CD games" : Uri.parse(cdBios).getLastPathSegment(),
                "Choose", v -> pick(PICK_CDBIOS)));

        list.addView(sectionTitle("Saves"));
        list.addView(row("Memory Track", "The CD save cartridge: see what each game has saved, delete saves, free up space",
                "Manage", v -> showMemoryTrack()));

        list.addView(sectionTitle("Controls"));
        list.addView(row("Button layout", "X = A   ·   A = B   ·   B = C   ·   Y / Select = Option   ·   Start = Pause\n"
                + "L1 R1 L2 R2 = keypad 1-4   ·   L3 = 0   ·   R3 = #   ·   Select + Start = menu", null, v -> { }));

        list.addView(sectionTitle("Updates"));
        final View[] check = new View[1];
        check[0] = row("Check for updates", "Looks for a newer JagDroid on GitHub and installs it",
                foundUpdate != null ? foundUpdate.tag + " available" : "Check", v -> checkForUpdate(check[0]));
        list.addView(check[0]);
        list.addView(toggleRow("Check automatically", "Look for a new version when JagDroid starts (at most twice a day)",
                "autoUpdate", true));

        list.addView(sectionTitle("About"));
        list.addView(row("JagDroid " + versionName(), "An experimental Atari Jaguar emulator written from scratch.\n"
                + "Game saves and save states are kept in the app's private storage.", null, v -> { }));
        first.requestFocus();
    }


    // ---------------------------------------------------------- memory track --
    private static String sizeText(long bytes) {
        return bytes >= 1024 ? (bytes / 1024) + " KB" : bytes + " bytes";
    }

    /** Settings > Memory Track: the cartridge's files, with delete and erase-all. */
    private void showMemoryTrack() {
        screen = SCREEN_MEMTRACK;
        MemoryTrack mt = new MemoryTrack(new File(saveDir(), "jaguar-cd-memory-track-cartridge.bin"));
        LinearLayout list = new LinearLayout(this);
        page("Memory Track", list, pill("Back", v -> showSettings()));
        java.util.List<MemoryTrack.Entry> files = mt.list();
        View first = row("Space", sizeText(mt.freeBytes()) + " free of " + sizeText(mt.totalBytes())
                + (files.isEmpty() ? "\nNo saves yet. CD games that use the Memory Track create their files the first time they save." : ""),
                null, v -> { });
        list.addView(first);
        if (!files.isEmpty()) list.addView(sectionTitle("Saved files (select to delete)"));
        for (MemoryTrack.Entry e : files) {
            final View[] self = new View[1];
            final boolean[] armed = new boolean[1];
            self[0] = row(e.app.isEmpty() ? "(unnamed)" : e.app, e.file + "  ·  " + sizeText(e.bytes()), "Delete", v -> {
                if (!armed[0]) { armed[0] = true; setRowValue(self[0], "Sure?"); return; }
                mt.delete(e);
                toast("Deleted " + e.file);
                showMemoryTrack();
            });
            list.addView(self[0]);
        }
        if (!files.isEmpty()) {
            list.addView(sectionTitle("Everything"));
            final View[] all = new View[1];
            final boolean[] armedAll = new boolean[1];
            all[0] = row("Erase all saves", "Deletes every file on the cartridge, for every game", "Erase", v -> {
                if (!armedAll[0]) { armedAll[0] = true; setRowValue(all[0], "Sure?"); return; }
                mt.eraseAll();
                toast("Memory Track erased");
                showMemoryTrack();
            });
            list.addView(all[0]);
        }
        first.requestFocus();
    }

    // ------------------------------------------------------------- updates --
    /** On the library screen: quietly look for a new release now and then, and show a banner if there is one. */
    private void autoCheckForUpdate(LinearLayout list) {
        if (!prefs.getBoolean("autoUpdate", true) || updateBusy) return;
        long last = prefs.getLong("updateChecked", 0);
        if (System.currentTimeMillis() - last < 12 * DateUtils.HOUR_IN_MILLIS) return;
        prefs.edit().putLong("updateChecked", System.currentTimeMillis()).apply();
        new Thread(() -> {
            try {
                Updater.Release r = Updater.latest();
                if (r != null && Updater.newer(r.tag, versionName())) ui.post(() -> {
                    foundUpdate = r;
                    if (screen == SCREEN_LIBRARY && list.isAttachedToWindow()) list.addView(updateBanner(), 0);
                });
            } catch (Exception ignored) { }   // offline or GitHub unreachable: try again next time
        }).start();
    }

    private View updateBanner() {
        LinearLayout r = row("Update available: " + foundUpdate.tag, "You have v" + versionName() + ". Select to see what's new and install.",
                "Update", v -> showUpdate(foundUpdate));
        TextView b = (TextView) r.findViewWithTag("value");
        b.setTextColor(TEXT);
        b.setPadding(dp(10), dp(4), dp(10), dp(4));
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(ACCENT); bg.setCornerRadius(dp(6));
        b.setBackground(bg);
        return r;
    }

    /** Settings > Check for updates. */
    private void checkForUpdate(View rowView) {
        if (updateBusy) return;
        updateBusy = true;
        setRowValue(rowView, "Checking…");
        new Thread(() -> {
            String msg; Updater.Release found = null;
            try {
                Updater.Release r = Updater.latest();
                if (r == null) msg = "No releases yet";
                else if (Updater.newer(r.tag, versionName())) { found = r; msg = r.tag + " available"; }
                else msg = "Up to date";
            } catch (Exception e) {
                msg = e instanceof java.net.UnknownHostException ? "No internet" : "Couldn't check";
                final String detail = e.getMessage();
                ui.post(() -> { if (detail != null) toast(detail); });
            }
            final String m = msg; final Updater.Release f = found;
            ui.post(() -> {
                updateBusy = false;
                prefs.edit().putLong("updateChecked", System.currentTimeMillis()).apply();
                if (f != null) { foundUpdate = f; showUpdate(f); }
                else if (rowView.isAttachedToWindow()) setRowValue(rowView, m);
            });
        }).start();
    }

    /** What's new in a release, with a button to download and install it. */
    private void showUpdate(Updater.Release r) {
        screen = SCREEN_UPDATE;
        updateCancel = false;
        LinearLayout list = new LinearLayout(this);
        page("Update", list, pill("Back", v -> { updateCancel = true; showSettings(); }));
        list.addView(sectionTitle("JagDroid " + r.tag));
        TextView sub = text("You have v" + versionName() + (r.apkSize > 0
                ? String.format(Locale.US, "  ·  download %.1f MB", r.apkSize / 1048576.0) : ""), 14, SUBTEXT, false);
        sub.setPadding(dp(4), 0, dp(4), dp(10));
        list.addView(sub);
        final View[] go = new View[1];
        go[0] = row("Download and install", "Your games, saves and settings are kept", null, v -> downloadAndInstall(r, go[0]));
        list.addView(go[0]);
        list.addView(row("Open the releases page", Updater.RELEASES_PAGE, null, v -> {
            try { startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(r.pageUrl))); }
            catch (Exception e) { toast("No web browser found"); }
        }));
        if (r.notes != null && !r.notes.trim().isEmpty()) {
            list.addView(sectionTitle("What's new"));
            TextView notes = text(r.notes.replace("\r", "").replaceAll("(?m)^#+\\s*", "").replace("**", "").trim(), 15, TEXT, false);
            notes.setPadding(dp(4), 0, dp(4), dp(12));
            notes.setLineSpacing(0, 1.15f);
            list.addView(notes);
        }
        go[0].requestFocus();
    }

    private void downloadAndInstall(Updater.Release r, View rowView) {
        if (updateBusy) { updateCancel = true; return; }   // pressing again cancels the download
        if (!Updater.canInstall(this)) {
            toast("Allow JagDroid to install apps, then press Back and select Download again");
            try { Updater.openInstallPermission(this); } catch (Exception e) { toast("Open Android Settings > Apps > JagDroid > Install unknown apps"); }
            return;
        }
        updateBusy = true; updateCancel = false;
        TextView title = (TextView) ((LinearLayout) ((LinearLayout) rowView).getChildAt(0)).getChildAt(0);
        title.setText("Downloading…  (select again to cancel)");
        new Thread(() -> {
            String error = null; java.io.File apk = null;
            try {
                long[] lastUi = {0};
                apk = Updater.download(this, r, (done, total) -> {
                    long now = System.currentTimeMillis();
                    if (now - lastUi[0] > 200) {
                        lastUi[0] = now;
                        final String t = total > 0 ? String.format(Locale.US, "Downloading…  %d%%  (select again to cancel)", done * 100 / total)
                                : String.format(Locale.US, "Downloading…  %.1f MB", done / 1048576.0);
                        ui.post(() -> title.setText(t));
                    }
                    return !updateCancel;
                });
                if (apk != null) Updater.install(this, apk);
            } catch (Exception e) {
                error = e.getMessage() != null ? e.getMessage() : "Download failed";
            }
            final String err = error; final boolean cancelled = apk == null && err == null;
            ui.post(() -> {
                updateBusy = false;
                if (!title.isAttachedToWindow()) return;
                if (err != null) { title.setText("Download and install"); toast(err); }
                else if (cancelled) title.setText("Download and install");
                else title.setText("Installing…  confirm on the next screen");
            });
        }).start();
    }

    interface IntAction { void run(int value); }

    private View cycleRow(String title, String sub, String pref, String[] names, IntAction onChange) {
        int cur = Math.max(0, Math.min(names.length - 1, prefs.getInt(pref, 0)));
        final View[] holder = new View[1];
        holder[0] = row(title, sub, names[cur], v -> {
            int n = (prefs.getInt(pref, 0) + 1) % names.length;
            prefs.edit().putInt(pref, n).apply();
            setRowValue(holder[0], names[n]);
            if (onChange != null) onChange.run(n);
        });
        return holder[0];
    }

    private View toggleRow(String title, String sub, String pref, boolean def) {
        return toggleRow(title, sub, pref, def, null);
    }

    private View toggleRow(String title, String sub, String pref, boolean def, Runnable onChange) {
        final View[] holder = new View[1];
        holder[0] = row(title, sub, prefs.getBoolean(pref, def) ? "On" : "Off", v -> {
            boolean n = !prefs.getBoolean(pref, def);
            prefs.edit().putBoolean(pref, n).apply();
            setRowValue(holder[0], n ? "On" : "Off");
            if (onChange != null) onChange.run();
        });
        return holder[0];
    }

    private boolean screenSupportsHdr() {
        try {
            android.view.Display d = getWindowManager().getDefaultDisplay();
            return d != null && d.isHdr();
        } catch (Exception e) { return false; }
    }

    private void applyHdr() {
        boolean on = prefs.getBoolean("hdr", false);
        float peak = HDR_PEAKS[Math.max(0, Math.min(2, prefs.getInt("hdrPeak", 1)))];
        NativeLib.setHdr(on, peak);
        // Ask the system to put the screen in HDR mode while a game is showing.
        boolean hdrNow = on && screen == SCREEN_GAME;
        getWindow().setColorMode(hdrNow ? android.content.pm.ActivityInfo.COLOR_MODE_HDR
                                        : android.content.pm.ActivityInfo.COLOR_MODE_DEFAULT);
        if (android.os.Build.VERSION.SDK_INT >= 35) {
            try { getWindow().setDesiredHdrHeadroom(hdrNow ? peak : 0f); } catch (Exception ignored) { }
        }
    }

    private void applyEnhancements() {
        NativeLib.setEnhancements(prefs.getBoolean("smoothShade", false), prefs.getInt("renderScale", 0) == 1 ? 2 : 1);
    }

    // --------------------------------------------------------------- files --
    private void pick(int code) {
        Intent i;
        if (code == PICK_FOLDER) {
            i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        } else {
            i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
        }
        try { startActivityForResult(i, code); } catch (Exception e) { toast("No file picker available"); }
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (res != RESULT_OK || data == null || data.getData() == null) return;
        Uri uri = data.getData();
        try {
            getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException ignored) { }
        switch (req) {
            case PICK_FOLDER: prefs.edit().putString("folder", uri.toString()).apply(); showLibrary(); break;
            case PICK_BIOS:
            case PICK_CDBIOS: {
                // The console BIOS is 128 KB and the Jaguar CD BIOS 256 KB; put each in its own slot.
                byte[] b = readAll(uri);
                int kind = b == null ? 0 : biosKind(b);
                boolean wantCd = req == PICK_CDBIOS;
                if (kind == 1 && wantCd) {
                    toast("That's the console BIOS, not the Jaguar CD BIOS (a 256 KB file). It's been set as the console BIOS instead.");
                    prefs.edit().putString("bios", uri.toString()).putBoolean("useBios", true).apply();
                } else if (kind == 2 && !wantCd) {
                    toast("That's the Jaguar CD BIOS. It's been set as the CD BIOS; the console BIOS is a 128 KB file.");
                    prefs.edit().putString("cdbios", uri.toString()).apply();
                } else if (kind == 0) {
                    toast(b == null ? "Couldn't read that file" : "That doesn't look like a Jaguar BIOS (expected 128 KB for the console or 256 KB for the CD)");
                } else if (wantCd) {
                    prefs.edit().putString("cdbios", uri.toString()).apply();
                } else {
                    prefs.edit().putString("bios", uri.toString()).putBoolean("useBios", true).apply();
                }
                showSettings();
                break;
            }
            case PICK_GAME: startGame(uri, null); break;
            case PICK_AUDIO: startAudio(uri); break;
        }
    }

    /** 1 = console BIOS (128 KB), 2 = Jaguar CD BIOS (256 KB), 0 = neither. */
    static int biosKind(byte[] b) {
        if (b.length == 128 * 1024) return 1;
        if (b.length == 256 * 1024) return 2;
        return 0;
    }

    /** Reads the chosen CD BIOS, or explains what's wrong and returns null. */
    private byte[] cdBiosOrComplain() {
        String cdBiosUri = prefs.getString("cdbios", null);
        if (cdBiosUri == null) { toast("Choose the Jaguar CD BIOS in Settings first"); showSettings(); return null; }
        byte[] cdBios = readAll(Uri.parse(cdBiosUri));
        if (cdBios == null) { toast("Couldn't read the CD BIOS file; choose it again in Settings"); showSettings(); return null; }
        if (biosKind(cdBios) != 2) {
            toast(biosKind(cdBios) == 1 ? "The CD BIOS slot holds the console BIOS. Choose the Jaguar CD BIOS (256 KB) in Settings."
                    : "The file in the CD BIOS slot isn't a Jaguar CD BIOS (256 KB). Choose it again in Settings.");
            showSettings();
            return null;
        }
        return cdBios;
    }

    private String displayName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) return c.getString(0);
        } catch (Exception ignored) { }
        String s = uri.getLastPathSegment();
        return s == null ? "game" : s.substring(s.lastIndexOf('/') + 1);
    }

    private byte[] readAll(Uri uri) {
        try (InputStream in = getContentResolver().openInputStream(uri)) {
            if (in == null) return null;
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            return out.toByteArray();
        } catch (Exception e) {
            return null;
        }
    }

    private File saveDir() {
        File d = new File(getFilesDir(), "saves");
        if (!d.exists()) d.mkdirs();
        return d;
    }

    private File stateFile(String key, int slot) { return new File(saveDir(), key + ".state" + slot); }
    private File thumbFile(String key, int slot) { return new File(saveDir(), key + ".state" + slot + ".png"); }

    /**
     * Jaguar cartridges carry no region flag (most are region-free and check the
     * console at run time), so Auto goes by the usual dump naming: a file marked
     * only for Europe or PAL runs as a PAL console, anything else as NTSC.
     */
    static boolean regionIsPal(int setting, String fileName) {
        if (setting == 1) return false;
        if (setting == 2) return true;
        String n = fileName == null ? "" : fileName.toLowerCase(Locale.US);
        boolean eu = n.contains("europe") || n.contains("(e)") || n.contains("pal") || n.contains("(uk)")
                || n.contains("germany") || n.contains("france");
        boolean us = n.contains("usa") || n.contains("(u)") || n.contains("ntsc") || n.contains("world") || n.contains("japan");
        return eu && !us;
    }

    // ---------------------------------------------------------------- game --
    private void launch(Game g) {
        if (g.zipDisc) startZipDisc(g);
        else if (g.isCd()) startDisc(g);
        else startGame(g.uri, g.fileName);
    }

    private void startDisc(Game g) {
        byte[] cdBios = cdBiosOrComplain();
        if (cdBios == null) return;
        byte[] cueBytes = readAll(g.uri);
        if (cueBytes == null || g.tree == null) { toast("Couldn't read the cue sheet"); return; }
        String cue = new String(cueBytes, java.nio.charset.StandardCharsets.UTF_8);

        // The track files the cue sheet names, found in its folder by display name.
        List<String> wanted = new ArrayList<>();
        for (String line : cue.split("\\r?\\n")) {
            String t = line.trim();
            if (!t.regionMatches(true, 0, "FILE", 0, 4)) continue;
            int a = t.indexOf('"'), b = t.lastIndexOf('"');
            if (a >= 0 && b > a) wanted.add(t.substring(a + 1, b));
        }
        java.util.Map<String, Uri> inFolder = new java.util.HashMap<>();
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(g.tree, g.folderId);
        try (Cursor c = getContentResolver().query(children, new String[]{
                DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME}, null, null, null)) {
            while (c != null && c.moveToNext())
                inFolder.put(c.getString(1), DocumentsContract.buildDocumentUriUsingTree(g.tree, c.getString(0)));
        } catch (Exception ignored) { }

        List<android.os.ParcelFileDescriptor> open = new ArrayList<>();
        String[] names = wanted.toArray(new String[0]);
        int[] fds = new int[names.length];
        try {
            for (int i = 0; i < names.length; i++) {
                Uri u = inFolder.get(names[i]);
                if (u == null) { toast("Missing track file: " + names[i]); return; }
                android.os.ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(u, "r");
                if (pfd == null) { toast("Couldn't open " + names[i]); return; }
                open.add(pfd);
                fds[i] = pfd.getFd();
            }
            NativeLib.setPal(regionIsPal(prefs.getInt("region", 0), g.fileName));
            NativeLib.setEepromPath(new File(saveDir(), g.key() + ".eep").getAbsolutePath());
            NativeLib.setScanlines(prefs.getInt("scanlines", 0));
            applyEnhancements();
            if (!NativeLib.loadDisc(cdBios, cue, names, fds)) {
                toast(NativeLib.lastError());
                showLibrary();
                return;
            }
        } catch (Exception e) {
            toast("Couldn't open the disc: " + e.getMessage());
            return;
        } finally {
            for (android.os.ParcelFileDescriptor p : open) try { p.close(); } catch (Exception ignored) { }
        }
        enterGame(g);
    }

    /** Plays an audio file through the Jaguar CD BIOS player, with its VLM visualiser. */
    private void startAudio(Uri uri) {
        byte[] cdBios = cdBiosOrComplain();
        if (cdBios == null) return;
        String name = "Music";
        try (Cursor c = getContentResolver().query(uri, new String[]{android.provider.OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst() && c.getString(0) != null) name = c.getString(0);
        } catch (Exception ignored) { }
        final Game g = new Game(uri, "VLM " + name.replaceFirst("\\.[^.]+$", "") + ".audio");
        File dir = new File(getCacheDir(), "audiodisc");
        deleteTree(dir);

        screen = SCREEN_SETTINGS;
        LinearLayout box = new LinearLayout(this);
        page("Preparing " + name, box, pill("Cancel", v -> AudioDisc.cancel = true));
        TextView status = text("Decoding audio…", 16, TEXT, false);
        box.addView(status);
        TextView hint = text("The file is converted to a CD audio track for the Jaguar CD BIOS. Press Play (the B button) on the player, and choose VLM for the visualiser.", 14, SUBTEXT, false);
        hint.setPadding(0, dp(8), 0, 0);
        box.addView(hint);
        Runnable poll = new Runnable() {
            @Override public void run() {
                float p = AudioDisc.progress;
                if (p >= 0) status.setText(String.format(Locale.US, "Decoding audio… %d%%", Math.round(p * 100)));
                if (status.isAttachedToWindow()) ui.postDelayed(this, 250);
            }
        };
        ui.postDelayed(poll, 250);
        new Thread(() -> {
            String err = AudioDisc.build(this, uri, dir);
            ui.post(() -> {
                ui.removeCallbacks(poll);
                if (err != null) { toast(err); showLibrary(); return; }
                try (android.os.ParcelFileDescriptor pfd = android.os.ParcelFileDescriptor.open(new File(dir, "track.bin"), android.os.ParcelFileDescriptor.MODE_READ_ONLY)) {
                    NativeLib.setPal(false);
                    NativeLib.setEepromPath(new File(saveDir(), "vlm.eep").getAbsolutePath());
                    NativeLib.setScanlines(prefs.getInt("scanlines", 0));
                    applyEnhancements();
                    String cue = "FILE \"track.bin\" BINARY\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n";
                    if (!NativeLib.loadDisc(cdBios, cue, new String[]{"track.bin"}, new int[]{pfd.getFd()})) {
                        toast(NativeLib.lastError()); showLibrary(); return;
                    }
                } catch (Exception e) { toast("Couldn't open the audio disc: " + e.getMessage()); showLibrary(); return; }
                enterGame(g);
            });
        }).start();
    }

    /** Zipped CD: unpacks compressed tracks into the cache (once) with a progress screen. */
    private void startZipDisc(Game g) {
        byte[] cdBios = cdBiosOrComplain();
        if (cdBios == null) return;
        android.os.ParcelFileDescriptor pfd;
        try { pfd = getContentResolver().openFileDescriptor(g.uri, "r"); } catch (Exception e) { pfd = null; }
        if (pfd == null) { toast("Couldn't open " + g.fileName); return; }

        // Keep only this disc's unpacked tracks; a disc can be several hundred MB.
        File discs = new File(getCacheDir(), "discs");
        File mine = new File(discs, g.key());
        File[] old = discs.listFiles();
        if (old != null) for (File d : old) if (!d.equals(mine)) deleteTree(d);
        mine.mkdirs();

        NativeLib.setPal(regionIsPal(prefs.getInt("region", 0), g.fileName));
        NativeLib.setEepromPath(new File(saveDir(), g.key() + ".eep").getAbsolutePath());
        NativeLib.setScanlines(prefs.getInt("scanlines", 0));
        applyEnhancements();

        screen = SCREEN_SETTINGS;   // not the library: keeps the back key from relisting mid-load
        LinearLayout box = new LinearLayout(this);
        page("Loading " + g.title, box, pill("Cancel", v -> NativeLib.cancelUnpack()));
        TextView status = text("Opening disc…", 16, TEXT, false);
        box.addView(status);
        TextView hint = text("Compressed discs are unpacked once and kept until you play another zipped disc.", 14, SUBTEXT, false);
        hint.setPadding(0, dp(8), 0, 0);
        box.addView(hint);
        final android.os.ParcelFileDescriptor fpfd = pfd;
        Runnable poll = new Runnable() {
            @Override public void run() {
                float p = NativeLib.unpackProgress();
                if (p >= 0) status.setText(String.format(Locale.US, "Unpacking disc… %d%%", Math.round(p * 100)));
                if (status.isAttachedToWindow()) ui.postDelayed(this, 250);
            }
        };
        ui.postDelayed(poll, 250);
        new Thread(() -> {
            boolean ok = NativeLib.loadZipDisc(cdBios, fpfd.getFd(), mine.getAbsolutePath());
            try { fpfd.close(); } catch (Exception ignored) { }
            ui.post(() -> {
                ui.removeCallbacks(poll);
                if (ok) enterGame(g);
                else { toast(NativeLib.lastError()); showLibrary(); }
            });
        }).start();
    }

    private static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) deleteTree(k);
        f.delete();
    }

    private void startGame(Uri uri, String fileName) {
        if (fileName == null) fileName = displayName(uri);
        if (fileName.toLowerCase(Locale.US).endsWith(".zip") && zipKind(uri) == 2) {
            startZipDisc(new Game(uri, fileName, true));
            return;
        }
        if (fileName.toLowerCase(Locale.US).endsWith(".cue")) {
            toast("Open CD games from your games folder so their track files can be found");
            if (screen != SCREEN_GAME) showLibrary();
            return;
        }
        boolean zip = fileName.toLowerCase(Locale.US).endsWith(".zip");
        byte[] rom = zip ? null : readAll(uri);
        if (!zip && rom == null) { toast("Couldn't read the game file"); if (screen != SCREEN_GAME) showLibrary(); return; }
        byte[] bios = null;
        String b = prefs.getString("bios", null);
        if (b != null && prefs.getBoolean("useBios", true)) bios = readAll(Uri.parse(b));
        Game g = new Game(uri, fileName);
        NativeLib.setPal(regionIsPal(prefs.getInt("region", 0), fileName));
        NativeLib.setEepromPath(new File(saveDir(), g.key() + ".eep").getAbsolutePath());
        NativeLib.setScanlines(prefs.getInt("scanlines", 0));
        applyEnhancements();
        boolean loaded;
        if (zip) {
            try (android.os.ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, "r")) {
                loaded = pfd != null && NativeLib.loadZipGame(pfd.getFd(), bios);
            } catch (Exception e) { loaded = false; }
        } else loaded = NativeLib.loadGame(rom, bios);
        if (!loaded) {
            toast(NativeLib.lastError());
            showLibrary();
            return;
        }
        enterGame(g);
    }

    private void enterGame(Game g) {
        gameKey = g.key(); gameTitle = g.title; new File(saveDir(), gameKey + ".dspcrash.state").delete();
        markPlayed(gameKey);

        screen = SCREEN_GAME;
        applyHdr();
        ui.postDelayed(() -> {
            if (screen == SCREEN_GAME && prefs.getBoolean("hdr", false) && !NativeLib.hdrActive())
                toast("HDR isn't available on this screen, so the picture is in normal range");
        }, 1500);
        root.removeAllViews();
        root.setBackgroundColor(Color.BLACK);
        SurfaceView view = new AspectSurface(this);
        view.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder h) { NativeLib.setSurface(h.getSurface()); }
            @Override public void surfaceChanged(SurfaceHolder h, int f, int w, int hh) { NativeLib.setSurface(h.getSurface()); }
            @Override public void surfaceDestroyed(SurfaceHolder h) { NativeLib.setSurface(null); }
        });
        root.addView(view, new FrameLayout.LayoutParams(-2, -2, Gravity.CENTER));
        keyMask = axisMask = 0;
        menuOpen = false; pauseMenu = null;
        fpsView = null;
        ui.removeCallbacks(fpsTick);
        if (prefs.getBoolean("showFps", false)) {
            fpsView = text("-- FPS", 14, 0xFFFFE066, true);
            fpsView.setBackgroundColor(0x80000000);
            fpsView.setPadding(dp(8), dp(2), dp(8), dp(2));
            root.addView(fpsView, new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.START));
            ui.postDelayed(fpsTick, 500);
        }
        NativeLib.start();
    }

    private void quitToLibrary() {
        NativeLib.flushSave();
        showLibrary();
    }

    /** Keeps the picture at 4:3 and as large as the screen allows. */
    static class AspectSurface extends SurfaceView {
        AspectSurface(Context c) { super(c); }
        @Override protected void onMeasure(int wSpec, int hSpec) {
            int w = MeasureSpec.getSize(wSpec), h = MeasureSpec.getSize(hSpec);
            if (w * 3 > h * 4) w = h * 4 / 3; else h = w * 3 / 4;
            setMeasuredDimension(w, h);
        }
    }

    // ---------------------------------------------------------- pause menu --
    private void openPauseMenu() {
        if (menuOpen) return;
        menuOpen = true;
        keyMask = axisMask = 0;
        NativeLib.setButtons(0);
        NativeLib.setPaused(true);

        FrameLayout dim = new FrameLayout(this);
        dim.setBackgroundColor(0xB0000000);
        dim.setClickable(true);

        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.HORIZONTAL);
        panel.setPadding(dp(20), dp(20), dp(20), dp(20));
        panel.setBackground(rounded(BG, 0x40FFFFFF));

        // Left: actions
        LinearLayout actions = new LinearLayout(this);
        actions.setOrientation(LinearLayout.VERTICAL);
        actions.addView(text(gameTitle, 20, TEXT, true));
        TextView hint = text("Paused", 13, SUBTEXT, false);
        hint.setPadding(0, 0, 0, dp(12));
        actions.addView(hint);

        ImageView thumb = new ImageView(this);
        TextView slotInfo = text("", 13, SUBTEXT, false);
        final View[] slotRow = new View[1];
        Runnable refreshSlot = () -> {
            int s = prefs.getInt("slot", 1);
            setRowValue(slotRow[0], "◀  " + s + "  ▶");
            File f = stateFile(gameKey, s), tf = thumbFile(gameKey, s);
            Bitmap bm = tf.exists() ? BitmapFactory.decodeFile(tf.getAbsolutePath()) : null;
            thumb.setImageBitmap(bm);
            thumb.setBackground(bm == null ? rounded(PANEL, 0) : null);
            slotInfo.setText(f.exists() ? "Saved " + DateUtils.getRelativeTimeSpanString(f.lastModified(),
                    System.currentTimeMillis(), DateUtils.MINUTE_IN_MILLIS) : "Empty slot");
        };
        View resume = row("Resume", null, null, v -> closePauseMenu());
        actions.addView(resume);
        slotRow[0] = row("State slot", "Left / right to change", "◀  1  ▶", v -> {
            prefs.edit().putInt("slot", prefs.getInt("slot", 1) % SLOTS + 1).apply();
            refreshSlot.run();
        });
        slotRow[0].setOnKeyListener((v, code, e) -> {
            if (e.getAction() != KeyEvent.ACTION_DOWN) return false;
            int d = code == KeyEvent.KEYCODE_DPAD_LEFT ? -1 : code == KeyEvent.KEYCODE_DPAD_RIGHT ? 1 : 0;
            if (d == 0) return false;
            int s = prefs.getInt("slot", 1) + d;
            if (s < 1) s = SLOTS;
            if (s > SLOTS) s = 1;
            prefs.edit().putInt("slot", s).apply();
            refreshSlot.run();
            return true;
        });
        actions.addView(slotRow[0]);
        actions.addView(row("Save state", null, null, v -> {
            int s = prefs.getInt("slot", 1);
            if (NativeLib.saveState(stateFile(gameKey, s).getAbsolutePath())) {
                saveThumbnail(thumbFile(gameKey, s));
                refreshSlot.run();
                toast("Saved to slot " + s);
            } else toast(NativeLib.lastError());
        }));
        actions.addView(row("Load state", null, null, v -> {
            int s = prefs.getInt("slot", 1);
            if (NativeLib.loadState(stateFile(gameKey, s).getAbsolutePath())) { closePauseMenu(); toast("Loaded slot " + s); }
            else toast(NativeLib.lastError());
        }));
        actions.addView(row("Export state", "Copy slot to Downloads", null, v -> {
            int s = prefs.getInt("slot", 1);
            File f = stateFile(gameKey, s);
            if (!f.exists()) { toast("Slot " + s + " is empty"); return; }
            toast(exportState(f));
        }));
        actions.addView(row("Export earlier moment", "About 30 s ago, for bug reports", null, v -> {
            File snap = new File(saveDir(), gameKey + ".earlier.state");
            if (NativeLib.saveSnapshot(snap.getAbsolutePath())) toast(exportState(snap));
            else toast(NativeLib.lastError());
        }));
        File crash = new File(saveDir(), gameKey + ".dspcrash.state");
        if (crash.exists()) {
            actions.addView(row("Export sound-crash state", "For bug reports", null, v -> toast(exportState(crash))));
        }
        actions.addView(row("Reset game", null, null, v -> { NativeLib.reset(); closePauseMenu(); }));
        final View[] scan = new View[1];
        scan[0] = row("Scanlines", null, SCANLINE_NAMES[prefs.getInt("scanlines", 0)], v -> {
            int n = (prefs.getInt("scanlines", 0) + 1) % SCANLINE_NAMES.length;
            prefs.edit().putInt("scanlines", n).apply();
            NativeLib.setScanlines(n);
            setRowValue(scan[0], SCANLINE_NAMES[n]);
        });
        actions.addView(scan[0]);
        actions.addView(row("Quit to library", null, null, v -> quitToLibrary()));
        panel.addView(actions, new LinearLayout.LayoutParams(dp(340), -2));

        // Right: slot preview
        LinearLayout preview = new LinearLayout(this);
        preview.setOrientation(LinearLayout.VERTICAL);
        preview.setPadding(dp(20), dp(52), 0, 0);
        TextView pt = text("SLOT PREVIEW", 12, SUBTEXT, true);
        pt.setPadding(0, 0, 0, dp(6));
        preview.addView(pt);
        thumb.setScaleType(ImageView.ScaleType.FIT_CENTER);
        preview.addView(thumb, new LinearLayout.LayoutParams(dp(240), dp(180)));
        slotInfo.setPadding(0, dp(6), 0, 0);
        preview.addView(slotInfo);
        panel.addView(preview);

        ScrollView sv = new ScrollView(this);
        sv.addView(panel);
        dim.addView(sv, new FrameLayout.LayoutParams(-2, -2, Gravity.CENTER));
        root.addView(dim, new FrameLayout.LayoutParams(-1, -1));
        pauseMenu = dim;
        refreshSlot.run();
        resume.requestFocus();
    }

    /** Copies a save state to the public Downloads folder so it can be shared. */
    private String exportState(File f) {
        String name = "JagDroid-" + f.getName().replaceAll("[^A-Za-z0-9._-]", "_");
        try {
            java.io.InputStream in = new java.io.FileInputStream(f);
            java.io.OutputStream out;
            String where;
            if (android.os.Build.VERSION.SDK_INT >= 29) {
                android.content.ContentValues cv = new android.content.ContentValues();
                cv.put(android.provider.MediaStore.Downloads.DISPLAY_NAME, name);
                cv.put(android.provider.MediaStore.Downloads.MIME_TYPE, "application/octet-stream");
                cv.put(android.provider.MediaStore.Downloads.RELATIVE_PATH, "Download");
                android.net.Uri u = getContentResolver().insert(
                        android.provider.MediaStore.Downloads.EXTERNAL_CONTENT_URI, cv);
                if (u == null) { in.close(); return "Export failed"; }
                out = getContentResolver().openOutputStream(u);
                where = "Downloads/" + name;
            } else {
                File d = android.os.Environment.getExternalStoragePublicDirectory(
                        android.os.Environment.DIRECTORY_DOWNLOADS);
                d.mkdirs();
                out = new FileOutputStream(new File(d, name));
                where = "Download/" + name;
            }
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            out.close();
            in.close();
            return "Exported to " + where;
        } catch (Exception e) {
            return "Export failed: " + e.getMessage();
        }
    }

    private void closePauseMenu() {
        if (!menuOpen) return;
        if (pauseMenu != null) root.removeView(pauseMenu);
        pauseMenu = null;
        menuOpen = false;
        keyMask = axisMask = 0;
        NativeLib.setButtons(0);
        NativeLib.setPaused(false);
    }

    private void saveThumbnail(File f) {
        int[] px = NativeLib.getThumbnail();
        if (px == null || px.length < 2) return;
        int w = px[0], h = px[1];
        if (w <= 0 || h <= 0 || px.length < 2 + w * h) return;
        Bitmap bm = Bitmap.createBitmap(px, 2, w, w, h, Bitmap.Config.ARGB_8888);
        try (FileOutputStream out = new FileOutputStream(f)) { bm.compress(Bitmap.CompressFormat.PNG, 100, out); }
        catch (Exception ignored) { }
    }

    // --------------------------------------------------------------- input --
    // Retroid / Xbox-style layout: left face = Jaguar A, bottom = B, right = C.
    private static int mapKey(int code) {
        switch (code) {
            case KeyEvent.KEYCODE_DPAD_UP: return NativeLib.UP;
            case KeyEvent.KEYCODE_DPAD_DOWN: return NativeLib.DOWN;
            case KeyEvent.KEYCODE_DPAD_LEFT: return NativeLib.LEFT;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return NativeLib.RIGHT;
            case KeyEvent.KEYCODE_BUTTON_X: return NativeLib.A;
            case KeyEvent.KEYCODE_BUTTON_A: return NativeLib.B;
            case KeyEvent.KEYCODE_BUTTON_B: return NativeLib.C;
            case KeyEvent.KEYCODE_BUTTON_Y: return NativeLib.OPTION;
            case KeyEvent.KEYCODE_BUTTON_START: return NativeLib.PAUSE;
            case KeyEvent.KEYCODE_BUTTON_SELECT: return NativeLib.OPTION;
            case KeyEvent.KEYCODE_BUTTON_L1: return NativeLib.K1;
            case KeyEvent.KEYCODE_BUTTON_R1: return NativeLib.K2;
            case KeyEvent.KEYCODE_BUTTON_L2: return NativeLib.K3;
            case KeyEvent.KEYCODE_BUTTON_R2: return NativeLib.K4;
            case KeyEvent.KEYCODE_BUTTON_THUMBL: return NativeLib.K0;
            case KeyEvent.KEYCODE_BUTTON_THUMBR: return NativeLib.HASH;
        }
        return 0;
    }

    private boolean selectHeld, startHeld;
    private int swallowUps = 0;   // releases of the keys that opened the pause menu

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        int code = e.getKeyCode();
        boolean down = e.getAction() == KeyEvent.ACTION_DOWN, up = e.getAction() == KeyEvent.ACTION_UP;

        if (menuOpen && swallowUps > 0 && up && (code == KeyEvent.KEYCODE_BUTTON_START || code == KeyEvent.KEYCODE_BUTTON_SELECT)) {
            swallowUps--;
            return true;
        }
        if (screen != SCREEN_GAME || menuOpen) {
            // Menus: A / Enter clicks the focused item, B / Back goes back, Start closes the pause menu.
            if (code == KeyEvent.KEYCODE_BUTTON_A || code == KeyEvent.KEYCODE_ENTER || code == KeyEvent.KEYCODE_DPAD_CENTER) {
                if (up) { View f = getCurrentFocus(); if (f != null) f.performClick(); }
                return true;
            }
            if (code == KeyEvent.KEYCODE_BUTTON_B || code == KeyEvent.KEYCODE_BACK || code == KeyEvent.KEYCODE_BUTTON_START) {
                if (up) {
                    if (menuOpen) closePauseMenu();
                    else if (screen == SCREEN_SETTINGS) showLibrary();
                    else if (screen == SCREEN_MEMTRACK) showSettings();
                    else if (screen == SCREEN_UPDATE) { updateCancel = true; showSettings(); }
                    else if (code == KeyEvent.KEYCODE_BACK) finish();
                }
                return true;
            }
            if (code == KeyEvent.KEYCODE_BUTTON_SELECT) return true;
            return super.dispatchKeyEvent(e);
        }

        if (code == KeyEvent.KEYCODE_BUTTON_SELECT) selectHeld = down;
        if (code == KeyEvent.KEYCODE_BUTTON_START) startHeld = down;
        if (selectHeld && startHeld) { selectHeld = startHeld = false; swallowUps = 2; openPauseMenu(); return true; }
        if (code == KeyEvent.KEYCODE_BACK) { if (up) openPauseMenu(); return true; }
        int bit = mapKey(code);
        if (bit == 0) return super.dispatchKeyEvent(e);
        if (down) keyMask |= bit; else if (up) keyMask &= ~bit;
        NativeLib.setButtons(keyMask | axisMask);
        return true;
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent e) {
        if (screen == SCREEN_GAME && !menuOpen && (e.getSource() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
                && e.getAction() == MotionEvent.ACTION_MOVE) {
            float x = e.getAxisValue(MotionEvent.AXIS_HAT_X), y = e.getAxisValue(MotionEvent.AXIS_HAT_Y);
            float lx = e.getAxisValue(MotionEvent.AXIS_X), ly = e.getAxisValue(MotionEvent.AXIS_Y);
            int m = 0;
            if (x < -0.5f || lx < -0.5f) m |= NativeLib.LEFT;
            if (x > 0.5f || lx > 0.5f) m |= NativeLib.RIGHT;
            if (y < -0.5f || ly < -0.5f) m |= NativeLib.UP;
            if (y > 0.5f || ly > 0.5f) m |= NativeLib.DOWN;
            float lt = e.getAxisValue(MotionEvent.AXIS_LTRIGGER), rt = e.getAxisValue(MotionEvent.AXIS_RTRIGGER);
            if (lt > 0.5f) m |= NativeLib.K3;
            if (rt > 0.5f) m |= NativeLib.K4;
            axisMask = m;
            NativeLib.setButtons(keyMask | axisMask);
            return true;
        }
        return super.dispatchGenericMotionEvent(e);
    }

    private void hideSystemUi() {
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController c = getWindow().getInsetsController();
            if (c != null) {
                c.hide(WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        }
    }

    private void toast(String s) { Toast.makeText(this, s, Toast.LENGTH_SHORT).show(); }
}
