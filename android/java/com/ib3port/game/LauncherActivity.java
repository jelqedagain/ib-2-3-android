package com.ib3port.game;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ShortcutInfo;
import android.content.pm.ShortcutManager;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.PorterDuff;
import android.graphics.PorterDuffXfermode;
import android.graphics.RectF;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.Icon;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.Enumeration;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipOutputStream;

/**
 * Starts the game, or first installs it from the player's own .ipa: the Payload/SwordGame.app
 * folder is extracted into the app's files folder (game/). The same code serves IB2
 * and III: the app's label is the game's name, and the manifest's meta-data says which .ipa to
 * accept (ib.bundleId), which version to ask for (ib.ipaVersion) and the space it needs (ib.size).
 * Once installed, it shows a menu: Play, backing up and restoring saves (userdata/, as a .zip the
 * player keeps wherever they like), and sharing the logs for bug reports.
 * For testing: am start -n <package>/com.ib3port.game.LauncherActivity with
 *   --es ipa /path/to/file.ipa     install from that file
 *   --es backupTo /path/file.zip   back up the saves to that file
 *   --es restoreFrom /path/file.zip  restore saves from that file (no confirmation)
 *   --ez copyLogs true             copy the log report to the clipboard (what Share logs does)
 *   --ez shareLogs true            open the share sheet with the log report file (holding Share logs)
 *   --es logReportTo /path/file.txt  write the log report to that file
 */
public class LauncherActivity extends Activity {
    private static final int PICK_IPA = 1, PICK_BACKUP = 2, PICK_RESTORE = 3;
    private static final String APP_PREFIX = "Payload/SwordGame.app/";
    private static final String ARTWORK = "iTunesArtwork";  // the .ipa's 512 px App Store icon, kept in game/
    private static final String MARKER = "ib-port-saves.txt";  // in backups: which game they are from
    private static final long FILE_LOG_LIMIT = 4L << 20;  // bytes of each log in a shared file (a Discord upload is 10 MB)
    private static final long CLIP_LOG_LIMIT = 96L << 10;  // and in the clipboard, which holds well under 1 MB

    private String gameName, ipaVersion, bundleId, size;
    private TextView status;
    private ProgressBar progress;
    private View choose;
    private final List<View> menuButtons = new ArrayList<>();

    private File filesDir() {
        File dir = getExternalFilesDir(null);
        return dir != null ? dir : getFilesDir();
    }

    private File userData() {
        return new File(filesDir(), "userdata");
    }

    static boolean installed(File files) {
        File app = new File(files, "game/" + APP_PREFIX);
        return new File(app, "SwordGame").isFile() && new File(app, "CookedIPhone/Engine.xxx").isFile();
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        gameName = getApplicationInfo().loadLabel(getPackageManager()).toString();
        try {
            Bundle meta = getPackageManager().getApplicationInfo(getPackageName(), PackageManager.GET_META_DATA).metaData;
            ipaVersion = meta.getString("ib.ipaVersion");
            bundleId = meta.getString("ib.bundleId");
            size = meta.getString("ib.size");
        } catch (PackageManager.NameNotFoundException e) {
            throw new IllegalStateException(e);
        }
        Intent intent = getIntent();
        String ipa = intent.getStringExtra("ipa");
        if (ipa != null || !installed(filesDir())) {
            buildInstallScreen();
            if (ipa != null) install(null, new File(ipa));
            return;
        }
        buildMenuScreen();
        String backupTo = intent.getStringExtra("backupTo"), restoreFrom = intent.getStringExtra("restoreFrom");
        if (backupTo != null) backUp(null, new File(backupTo));
        else if (restoreFrom != null) restore(null, new File(restoreFrom));
        else if (intent.getBooleanExtra("copyLogs", false)) copyLogs();
        else if (intent.getBooleanExtra("shareLogs", false)) shareLogFile();
        else if (intent.getStringExtra("logReportTo") != null) {
            File to = new File(intent.getStringExtra("logReportTo"));
            runTask("Collecting the logs...", () -> {
                try (OutputStream out = new FileOutputStream(to)) {
                    writeLogReport(out, FILE_LOG_LIMIT);
                }
                return "Log report written to " + to.getName() + ".";
            });
        }
    }

    private void startGame() {
        Languages.savePhoneLanguage(this, filesDir());
        startActivity(new Intent(this, GameActivity.class));
        overridePendingTransition(0, 0);
        finish();
    }

    // This app's own icon is a plain placeholder: the game's artwork is not part of this app, and an
    // app cannot change its icon. Instead the icon from the player's own .ipa can be put on the home
    // screen as a shortcut that opens this app.
    private boolean canPinIcon() {
        ShortcutManager sm = getSystemService(ShortcutManager.class);
        return sm != null && sm.isRequestPinShortcutSupported() && gameIconFile() != null;
    }

    // The game's square icon: the .ipa's App Store artwork (installs from this version on), else the
    // largest icon in the game's folder.
    private Bitmap gameIconFile() {
        Bitmap best = BitmapFactory.decodeFile(new File(filesDir(), "game/" + ARTWORK).getPath());
        if (best == null || best.getWidth() != best.getHeight()) {
            best = null;
            File[] files = new File(filesDir(), "game/" + APP_PREFIX).listFiles();
            if (files == null) return null;
            int bestSize = 0;
            for (File f : files) {
                String n = f.getName().toLowerCase(Locale.ROOT);
                if (!n.endsWith(".png") || !n.startsWith("icon")) continue;
                BitmapFactory.Options o = new BitmapFactory.Options();
                o.inJustDecodeBounds = true;
                BitmapFactory.decodeFile(f.getPath(), o);
                if (o.outWidth < 100 || o.outWidth != o.outHeight || o.outWidth > 1024 || o.outWidth <= bestSize) continue;
                Bitmap b = BitmapFactory.decodeFile(f.getPath());
                if (b != null) {
                    best = b;
                    bestSize = o.outWidth;
                }
            }
        }
        return best;
    }

    // The game's icon with the rounded corners iOS draws (for this app's own screens).
    private Bitmap gameIcon() {
        Bitmap best = gameIconFile();
        if (best == null) return null;
        int px = Math.min(best.getWidth(), 432);
        Bitmap out = Bitmap.createBitmap(px, px, Bitmap.Config.ARGB_8888);
        Canvas c = new Canvas(out);
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
        c.drawRoundRect(new RectF(0, 0, px, px), px * 0.2237f, px * 0.2237f, paint);
        paint.setXfermode(new PorterDuffXfermode(PorterDuff.Mode.SRC_IN));
        c.drawBitmap(Bitmap.createScaledBitmap(best, px, px, true), 0, 0, paint);
        return out;
    }

    private void pinHomeIcon() {
        try {
            ShortcutManager sm = getSystemService(ShortcutManager.class);
            Bitmap src = gameIconFile();
            if (sm == null || src == null) return;
            // An adaptive icon, so the home screen gives it its own shape like any app icon (a plain bitmap is
            // shrunk onto a white disc). The visible part is the middle 72 of 108 units; the icon covers 80, so
            // every mask shape is filled and only its corners are cut.
            int size = 432, art = size * 80 / 108;
            Bitmap icon = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888);
            Canvas c = new Canvas(icon);
            c.drawColor(src.getPixel(src.getWidth() / 2, 1) | 0xFF000000);
            int at = (size - art) / 2;
            c.drawBitmap(Bitmap.createScaledBitmap(src, art, art, true), at, at, new Paint(Paint.FILTER_BITMAP_FLAG));
            Intent open = new Intent(this, LauncherActivity.class).setAction(Intent.ACTION_MAIN);
            sm.requestPinShortcut(new ShortcutInfo.Builder(this, "game-icon").setShortLabel(gameName)
                    .setIcon(Icon.createWithAdaptiveBitmap(icon)).setIntent(open).build(), null);
        } catch (RuntimeException e) {
            if (status != null) status.setText("Your home screen app does not allow adding icons.");
        }
    }

    // After installing the game: ask whether to put its icon on the home screen, then start.
    private void offerHomeIcon() {
        if (!canPinIcon()) {
            startGame();
            return;
        }
        new AlertDialog.Builder(this)
                .setTitle("Add the game's icon?")
                .setMessage("This app's own icon is a plain placeholder. Put the icon from your .ipa on the home screen?")
                .setCancelable(false)
                .setNegativeButton("Not now", (d, w) -> startGame())
                .setPositiveButton("Add icon", (d, w) -> {
                    buildMenuScreen();
                    pinHomeIcon();  // Android asks to confirm: stay on the menu so its prompt stays visible
                })
                .show();
    }

    private int dp(float v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }

    private String appVersion() {
        try {
            return getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (PackageManager.NameNotFoundException e) {
            return "?";
        }
    }

    // The left half of the screen: the game's icon, its name and a line under it.
    private LinearLayout brandPanel(String subtitle) {
        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setGravity(Gravity.CENTER_VERTICAL | Gravity.START);

        ImageView icon = new ImageView(this);
        Bitmap game = installed(filesDir()) ? gameIcon() : null;
        if (game != null) {
            icon.setImageBitmap(game);
        } else {
            Drawable d = getApplicationInfo().loadIcon(getPackageManager());
            icon.setImageDrawable(d);
        }
        icon.setBackground(Ui.rounded(this, 0xFF000000, 20, 0));
        icon.setClipToOutline(true);
        icon.setElevation(dp(6));
        panel.addView(icon, new LinearLayout.LayoutParams(dp(84), dp(84)));

        TextView title = Ui.text(this, gameName, 30, Ui.TEXT, true);
        title.setPadding(0, dp(16), 0, 0);
        panel.addView(title);
        TextView sub = Ui.text(this, subtitle, 13, Ui.SUBTEXT, false);
        sub.setPadding(0, dp(3), 0, 0);
        panel.addView(sub);
        return panel;
    }

    private TextView makeStatus() {
        status = Ui.text(this, "", 13.5f, Ui.SUBTEXT, false);
        status.setPadding(0, dp(14), 0, 0);
        return status;
    }

    // Two halves side by side (the launcher is landscape); each scrolls if the screen is short.
    private void show(View left, View right) {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.HORIZONTAL);
        root.setGravity(Gravity.CENTER_VERTICAL);
        root.setPadding(dp(48), dp(20), dp(40), dp(20));
        Ui.screenBackground(root);
        root.addView(scrollable(left), new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.MATCH_PARENT, 1));
        View gap = new View(this);
        root.addView(gap, new LinearLayout.LayoutParams(dp(32), 1));
        root.addView(scrollable(right), new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.MATCH_PARENT, 1.15f));
        setContentView(root);
        showingSettings = false;
        hideSystemBars();
    }

    // Full screen like the game; a swipe from the edge shows the bars for a moment.
    private void hideSystemBars() {
        android.view.WindowInsetsController c = getWindow().getInsetsController();
        if (c == null) return;
        c.hide(android.view.WindowInsets.Type.statusBars() | android.view.WindowInsets.Type.navigationBars());
        c.setSystemBarsBehavior(android.view.WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
    }

    private View scrollable(View content) {
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setVerticalScrollBarEnabled(false);
        LinearLayout center = new LinearLayout(this);
        center.setGravity(Gravity.CENTER_VERTICAL);
        center.addView(content, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        scroll.addView(center);
        return scroll;
    }

    private void buildInstallScreen() {
        LinearLayout left = brandPanel("Android port " + appVersion());

        LinearLayout card = Ui.card(this);
        card.setPadding(dp(22), dp(20), dp(22), dp(22));
        card.addView(Ui.text(this, "Bring your own IPA", 18, Ui.TEXT, true));
        TextView body = Ui.text(this, "This app runs the original iOS release of " + gameName + " (version " + ipaVersion + "). "
                + "Choose your .ipa file and the game is installed into this app, which needs about " + size + " of free space.\n\n"
                + "Your .ipa is only read, never changed or sent anywhere.", 14, Ui.SUBTEXT, false);
        body.setLineSpacing(0, 1.15f);
        body.setPadding(0, dp(8), 0, 0);
        card.addView(body);

        choose = Ui.primaryButton(this, "Choose .ipa file");
        choose.setOnClickListener(v -> pickIpa());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(52));
        lp.topMargin = dp(20);
        card.addView(choose, lp);

        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(1000);
        progress.setProgressTintList(android.content.res.ColorStateList.valueOf(Ui.ACCENT));
        progress.setVisibility(View.GONE);
        LinearLayout.LayoutParams pp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        pp.topMargin = dp(16);
        card.addView(progress, pp);
        card.addView(makeStatus());

        show(left, card);
    }

    private void buildMenuScreen() {
        menuButtons.clear();
        LinearLayout left = brandPanel("Android port " + appVersion());
        TextView play = Ui.primaryButton(this, "▶   Play");
        play.setOnClickListener(v -> startGame());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(dp(260), dp(56));
        lp.topMargin = dp(26);
        left.addView(play, lp);
        menuButtons.add(play);
        left.addView(makeStatus(), new LinearLayout.LayoutParams(dp(320), LinearLayout.LayoutParams.WRAP_CONTENT));

        LinearLayout card = Ui.card(this);
        addMenuRow(card, "Cheats", "Developer mode, items, gems, god mode", v -> showCheats());
        addMenuRow(card, "Saves", "Edit, back up or restore your progress", v -> showSaves());
        if (!isIb2()) addMenuRow(card, "ClashMobs", "Live events, your name on the leaderboards", v -> showClashMobs());
        addMenuRow(card, "Settings", "Language, graphics, sound, controls", v -> showSettings());
        addMenuRow(card, "Help", "Report a problem, home screen icon, where to find things", v -> showHelp());
        show(left, card);
    }

    // A page of rows under a title (Saves, Help); Back returns to the menu.
    private void showPage(String title, View... cards) {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        Ui.screenBackground(root);
        root.addView(Ui.pageHeader(this, title, null, null, this::buildMenuScreen));
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(28), dp(8), dp(28), dp(28));
        for (View c : cards) content.addView(c);
        LinearLayout center = new LinearLayout(this);
        center.setGravity(Gravity.CENTER_HORIZONTAL);
        center.addView(content, new LinearLayout.LayoutParams(Math.min(dp(760), getResources().getDisplayMetrics().widthPixels),
                LinearLayout.LayoutParams.WRAP_CONTENT));
        ScrollView scroll = new ScrollView(this);
        scroll.setVerticalScrollBarEnabled(false);
        scroll.addView(center);
        root.addView(scroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));
        setContentView(root);
        showingSettings = true;
        backTarget = this::buildMenuScreen;
        hideSystemBars();
    }

    private void showSaves() {
        menuButtons.clear();
        LinearLayout card = Ui.card(this);
        addMenuRow(card, "Edit save", "Gold, level, XP, stats, bloodline. Changes happen the next time you press Play.", v -> showSaveEditor());
        addMenuRow(card, "Back up saves", "Save your progress to a .zip file", v -> pickBackupFile());
        addMenuRow(card, "Restore saves", "Load your progress from a backup", v -> pickRestoreFile());
        showPage("Saves", Ui.sectionHeader(this, "Your progress"), card, makeStatus());
    }

    private void showHelp() {
        menuButtons.clear();
        LinearLayout card = Ui.card(this);
        addMenuRow(card, "Report a problem", "Copies the log to paste in Discord. Hold to send it as a file.", v -> copyLogs())
            .setOnLongClickListener(v -> {
                shareLogFile();
                return true;
            });
        if (canPinIcon()) addMenuRow(card, "Home screen icon", "Adds the icon from your .ipa to the home screen", v -> pinHomeIcon());
        LinearLayout where = Ui.card(this);
        where.setPadding(dp(18), dp(14), dp(18), dp(14));
        String store = isIb2() ? "Menu › Character › Items › Store, then › until Supplies" : "Menu › Items, the gem tab, then Store";
        String[][] faq = {
            {"Developer mode (dev mode)", "Cheats › Developer mode, the first switch. Then in the game: Menu › gear › Options, at the top"},
            {"Gold, level, stats, bloodline", "Saves › Edit save"},
            {"Get every item, god mode, gem shop", "Cheats"},
            {"The gem shop in the game", store},
            {"Graphics, language, sound, controller", "Settings"},
            {"Back up or restore saves", "Saves"},
        };
        for (int i = 0; i < faq.length; i++) {
            TextView q = Ui.text(this, faq[i][0], 15, Ui.TEXT, true);
            q.setPadding(0, i == 0 ? 0 : dp(12), 0, 0);
            where.addView(q);
            where.addView(Ui.text(this, "→ " + faq[i][1], 13.5f, Ui.SUBTEXT, false));
        }
        showPage("Help", Ui.sectionHeader(this, "Help"), card, Ui.sectionHeader(this, "Where do I find…"), where, makeStatus());
    }

    private void showCheats() {
        setContentView(CheatsScreen.build(this, filesDir(), isIb2(), this::buildMenuScreen));
        showingSettings = true;
        backTarget = this::buildMenuScreen;
        hideSystemBars();
    }

    private View addMenuRow(LinearLayout card, String title, String subtitle, View.OnClickListener click) {
        if (card.getChildCount() > 0) card.addView(Ui.divider(this));
        LinearLayout row = Ui.actionRow(this, title, subtitle, click);
        card.addView(row);
        menuButtons.add(row);
        return row;
    }

    private boolean showingSettings;  // a page opened from the menu: Back returns to backTarget
    private Runnable backTarget = this::buildMenuScreen;

    private boolean isIb2() {
        return getPackageName().equals("com.ib2port.game");
    }

    private void showClashMobs() {
        setContentView(ClashMobsScreen.build(this, new File(filesDir(), "settings.ini"), this::buildMenuScreen));
        showingSettings = true;
        backTarget = this::buildMenuScreen;
        hideSystemBars();
    }

    private void showSettings() {
        setContentView(SettingsScreen.build(this, new File(filesDir(), "settings.ini"), isIb2(), GameActivity.started, this::buildMenuScreen,
                this::showCheats));
        showingSettings = true;
        backTarget = this::buildMenuScreen;
        hideSystemBars();
    }

    private void showSaveEditor() {
        setContentView(SaveEditorScreen.build(this, filesDir(), isIb2(), GameActivity.started, this::showSaves));
        showingSettings = true;
        backTarget = this::showSaves;
        hideSystemBars();
    }

    @Override
    public void onBackPressed() {
        if (showingSettings) backTarget.run();
        else super.onBackPressed();
    }

    private void pickIpa() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, PICK_IPA);
    }

    private void pickBackupFile() {
        Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("application/zip");
        String date = new SimpleDateFormat("yyyy-MM-dd HHmm", Locale.US).format(new Date());
        i.putExtra(Intent.EXTRA_TITLE, gameName + " saves " + date + ".zip");
        startActivityForResult(i, PICK_BACKUP);
    }

    private void pickRestoreFile() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, PICK_RESTORE);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null || data.getData() == null) return;
        Uri uri = data.getData();
        if (request == PICK_IPA) install(uri, null);
        else if (request == PICK_BACKUP) backUp(uri, null);
        else if (request == PICK_RESTORE) {
            new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Restore saves?")
                .setMessage("Your current " + gameName + " progress will be replaced by the saves in " + displayName(uri) + ".")
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Restore", (d, w) -> restore(uri, null))
                .show();
        }
    }

    private String displayName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst() && c.getString(0) != null) return c.getString(0);
        } catch (Exception ignored) {
        }
        return "the chosen file";
    }

    private void install(Uri uri, File file) {
        choose.setEnabled(false);
        progress.setVisibility(View.VISIBLE);
        progress.setProgress(0);
        status.setText("Reading the .ipa...");
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        new Thread(() -> {
            String error = null;
            try {
                if (file != null) {
                    extract(new ZipFile(file));
                } else {
                    try (ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, "r")) {
                        if (pfd == null) throw new IOException("Could not open the file.");
                        extract(openZip(pfd, "Copying the .ipa..."));
                    }
                }
            } catch (Exception e) {
                error = e.getMessage() != null ? e.getMessage() : e.toString();
            }
            final String failure = error;
            runOnUiThread(() -> {
                getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                if (failure == null) {
                    status.setText("Installed. Starting the game...");
                    offerHomeIcon();
                } else {
                    progress.setVisibility(View.GONE);
                    status.setText(failure);
                    choose.setEnabled(true);
                }
            });
        }).start();
    }

    // Zip archives need random access: files picked from local storage have it; anything else
    // (a cloud file streamed by its app) is copied into the cache first.
    private ZipFile openZip(ParcelFileDescriptor pfd, String copying) throws IOException {
        try {
            return new ZipFile(new File("/proc/self/fd/" + pfd.getFd()));
        } catch (IOException e) {
            File copy = new File(getCacheDir(), "download.zip");
            setStatus(copying);
            try (InputStream in = new FileInputStream(pfd.getFileDescriptor()); OutputStream out = new FileOutputStream(copy)) {
                byte[] buf = new byte[1 << 18];
                for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
            }
            copy.deleteOnExit();
            return new ZipFile(copy);
        }
    }

    private void setStatus(String text) {
        runOnUiThread(() -> status.setText(text));
    }

    private void extract(ZipFile zip) throws IOException {
        try {
            long total = 0;
            boolean binary = false, engine = false, thisGame = false;
            for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements(); ) {
                ZipEntry z = e.nextElement();
                if (!z.getName().startsWith(APP_PREFIX)) continue;
                total += Math.max(0, z.getSize());
                binary |= z.getName().equals(APP_PREFIX + "SwordGame");
                engine |= z.getName().equals(APP_PREFIX + "CookedIPhone/Engine.xxx");
                if (z.getName().equals(APP_PREFIX + "Info.plist")) thisGame = contains(zip.getInputStream(z), bundleId);
            }
            if (!binary || !engine || !thisGame) throw new IOException("This .ipa does not contain " + gameName + ".");
            File files = filesDir();
            if (files.getUsableSpace() < total + (256L << 20))
                throw new IOException("Not enough free space: installing needs about " + (total / (1L << 30) + 1) + " GB.");

            // Extract into a staging folder and move it into place only when everything succeeded.
            File staging = new File(files, "game.partial");
            deleteTree(staging);
            String root = staging.getCanonicalPath() + File.separator;
            long done = 0;
            int shown = -1;
            byte[] buf = new byte[1 << 18];
            for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements(); ) {
                ZipEntry z = e.nextElement();
                if (!z.getName().startsWith(APP_PREFIX) || z.isDirectory()) continue;
                File dest = new File(staging, z.getName());
                if (!dest.getCanonicalPath().startsWith(root)) continue;  // no paths outside the folder
                File parent = dest.getParentFile();
                if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("Could not create " + parent);
                try (InputStream in = zip.getInputStream(z); OutputStream out = new FileOutputStream(dest)) {
                    for (int n; (n = in.read(buf)) > 0; ) {
                        out.write(buf, 0, n);
                        done += n;
                        int permille = total > 0 ? (int) (done * 1000 / total) : 0;
                        if (permille != shown) {
                            shown = permille;
                            final long d = done, t = total;
                            runOnUiThread(() -> {
                                progress.setProgress(permille);
                                status.setText(String.format("Installing... %d%%  (%.1f of %.1f GB)", permille / 10, d / 1e9, t / 1e9));
                            });
                        }
                    }
                } catch (IOException ex) {
                    deleteTree(staging);
                    // A ZipException here ("invalid block type", a bad CRC) means the .ipa's data is damaged, usually by an
                    // interrupted or corrupted download.
                    if (ex instanceof java.util.zip.ZipException)
                        throw new IOException("Your .ipa file is damaged: " + z.getName().substring(APP_PREFIX.length())
                                + " cannot be unpacked (" + ex.getMessage() + "). Download the .ipa again and choose the new copy.");
                    throw new IOException("Extracting " + z.getName() + " failed: " + ex.getMessage());
                }
            }
            ZipEntry artwork = zip.getEntry(ARTWORK);
            if (artwork != null && artwork.getSize() < (4L << 20)) {
                try (InputStream in = zip.getInputStream(artwork); OutputStream out = new FileOutputStream(new File(staging, ARTWORK))) {
                    for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
                } catch (IOException ex) {
                    // only the home screen icon uses it (the game's own icons are the fallback)
                }
            }
            File game = new File(files, "game");
            deleteTree(game);
            if (!staging.renameTo(game)) throw new IOException("Could not move the installed files into place.");
            if (!installed(files)) throw new IOException("The installed files are incomplete.");
        } finally {
            zip.close();
        }
    }

    // --- Menu actions: each runs off the UI thread, with the menu disabled and its result shown below it.

    private interface Task {
        String run() throws Exception;  // the message to show when done
    }

    private void runTask(String working, Task task) {
        for (View b : menuButtons) Ui.setEnabled(b, false);
        status.setText(working);
        new Thread(() -> {
            String message;
            try {
                message = task.run();
            } catch (Exception e) {
                message = e.getMessage() != null ? e.getMessage() : e.toString();
            }
            final String text = message;
            runOnUiThread(() -> {
                for (View b : menuButtons) Ui.setEnabled(b, true);
                if (text != null) status.setText(text);
            });
        }).start();
    }

    // Saves are the game's iOS home folder (userdata/) minus its caches and temporary files.
    private static boolean skipped(String relative) {
        return relative.equals("tmp") || relative.startsWith("tmp/") || relative.startsWith("Library/Caches")
               || relative.startsWith("tempdbg_") || relative.equals(MARKER);
    }

    private void backUp(Uri uri, File file) {
        runTask("Backing up your saves...", () -> {
            File home = userData();
            if (!new File(home, "Documents/SAVE").isDirectory())
                throw new IOException("There are no saves yet: play the game first.");
            try (OutputStream raw = file != null ? new FileOutputStream(file) : getContentResolver().openOutputStream(uri, "w")) {
                if (raw == null) throw new IOException("Could not write the backup file.");
                ZipOutputStream zip = new ZipOutputStream(raw);
                zip.putNextEntry(new ZipEntry(MARKER));
                zip.write(("game=" + gameName + "\nbundleId=" + bundleId + "\napp=" + appVersion() + "\n").getBytes(StandardCharsets.UTF_8));
                zip.closeEntry();
                int count = addTree(zip, home, "");
                zip.finish();
                return "Saves backed up (" + count + " files) to " + (file != null ? file.getName() : displayName(uri)) + ".";
            }
        });
    }

    private static int addTree(ZipOutputStream zip, File dir, String relative) throws IOException {
        File[] children = dir.listFiles();
        if (children == null) return 0;
        int count = 0;
        byte[] buf = new byte[1 << 16];
        for (File c : children) {
            String rel = relative + c.getName();
            if (skipped(rel)) continue;
            if (c.isDirectory()) {
                count += addTree(zip, c, rel + "/");
                continue;
            }
            zip.putNextEntry(new ZipEntry("userdata/" + rel));
            try (InputStream in = new FileInputStream(c)) {
                for (int n; (n = in.read(buf)) > 0; ) zip.write(buf, 0, n);
            }
            zip.closeEntry();
            count++;
        }
        return count;
    }

    // Accepts this launcher's backups, and zips of a userdata, Documents or SAVE folder made by hand
    // (the Windows port keeps the same folders). A backup is checked to be from this game.
    private void restore(Uri uri, File file) {
        runTask("Restoring saves...", () -> {
            ZipFile zip;
            ParcelFileDescriptor pfd = null;
            if (file != null) {
                zip = new ZipFile(file);
            } else {
                pfd = getContentResolver().openFileDescriptor(uri, "r");
                if (pfd == null) throw new IOException("Could not open the file.");
                zip = openZip(pfd, "Copying the backup...");
            }
            try {
                restoreFrom(zip);
            } finally {
                zip.close();
                if (pfd != null) pfd.close();
            }
            if (GameActivity.started) {
                // The game read the old saves into memory and would write them back: start afresh.
                runOnUiThread(() -> new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                    .setMessage("Saves restored. " + gameName + " will close now; open it again to play.")
                    .setCancelable(false)
                    .setPositiveButton("OK", (d, w) -> {
                        finishAffinity();
                        android.os.Process.killProcess(android.os.Process.myPid());
                    })
                    .show());
            }
            return "Saves restored.";
        });
    }

    private void restoreFrom(ZipFile zip) throws IOException {
        // Where the SAVE folder is inside the zip decides what the zip holds.
        String savePrefix = null;
        for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements(); ) {
            String name = e.nextElement().getName().replace('\\', '/');
            if (name.equals(MARKER)) {
                ZipEntry m = zip.getEntry(MARKER);
                String text = readAll(zip.getInputStream(m));
                if (!text.contains("bundleId=" + bundleId + "\n")) {
                    String other = text.startsWith("game=") ? text.substring(5, Math.max(5, text.indexOf('\n'))) : "another game";
                    throw new IOException("These saves are from " + other + ", not " + gameName + ".");
                }
            }
            int at = name.startsWith("SAVE/") ? 0 : name.indexOf("/SAVE/") + 1;
            if (at < 0 || (at == 0 && !name.startsWith("SAVE/"))) continue;
            String prefix = name.substring(0, at);
            if (savePrefix == null || prefix.length() < savePrefix.length()) savePrefix = prefix;
        }
        if (savePrefix == null) throw new IOException("There are no " + gameName + " saves in this file (no SAVE folder).");

        File home = userData();
        File staging = new File(filesDir(), "userdata.restore");
        deleteTree(staging);
        String sourceRoot, destRoot;
        if (savePrefix.endsWith("Documents/") || savePrefix.equals("Documents/")) {
            // A whole home folder: it replaces userdata/.
            sourceRoot = savePrefix.substring(0, savePrefix.length() - "Documents/".length());
            destRoot = "";
        } else {
            // Only the SAVE folder: keep the rest of userdata/ as it is.
            copyTree(home, staging);
            deleteTree(new File(staging, "Documents/SAVE"));
            sourceRoot = savePrefix + "SAVE/";
            destRoot = "Documents/SAVE/";
        }
        String root = staging.getCanonicalPath() + File.separator;
        int count = 0;
        byte[] buf = new byte[1 << 16];
        for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements(); ) {
            ZipEntry z = e.nextElement();
            String name = z.getName().replace('\\', '/');
            if (z.isDirectory() || !name.startsWith(sourceRoot)) continue;
            String rel = destRoot + name.substring(sourceRoot.length());
            if (skipped(rel)) continue;
            File dest = new File(staging, rel);
            if (!dest.getCanonicalPath().startsWith(root)) continue;  // no paths outside the folder
            File parent = dest.getParentFile();
            if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("Could not create " + parent);
            try (InputStream in = zip.getInputStream(z); OutputStream out = new FileOutputStream(dest)) {
                for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
            }
            count++;
        }
        if (count == 0 || !new File(staging, "Documents/SAVE").isDirectory()) {
            deleteTree(staging);
            throw new IOException("There are no " + gameName + " saves in this file.");
        }
        // Swap the folders; the replaced saves stay in userdata-before-restore until the next restore.
        File previous = new File(filesDir(), "userdata-before-restore");
        deleteTree(previous);
        if (home.exists() && !home.renameTo(previous)) {
            deleteTree(staging);
            throw new IOException("Could not replace the current saves.");
        }
        if (!staging.renameTo(home)) {
            previous.renameTo(home);
            throw new IOException("Could not move the restored saves into place.");
        }
    }

    // The log report: the device, then this run's and the last run's logs. Share logs copies it, to
    // paste into Discord; holding the button sends it as a file to the app the player picks.
    private void copyLogs() {
        runTask("Collecting the logs...", () -> {
            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
            writeLogReport(out, CLIP_LOG_LIMIT);
            String text = new String(out.toByteArray(), StandardCharsets.UTF_8);
            runOnUiThread(() -> {
                android.content.ClipboardManager clipboard = getSystemService(android.content.ClipboardManager.class);
                clipboard.setPrimaryClip(ClipData.newPlainText(gameName + " log", text));
                android.widget.Toast.makeText(this, "Log copied to the clipboard", android.widget.Toast.LENGTH_SHORT).show();
            });
            return "Log copied: paste it into Discord.\nIt can be long: hold Share logs to send it as a file instead.";
        });
    }

    private void shareLogFile() {
        runTask("Collecting the logs...", () -> {
            File dir = ShareProvider.folder(this);
            deleteTree(dir);
            if (!dir.mkdirs()) throw new IOException("Could not write the log report.");
            String date = new SimpleDateFormat("yyyy-MM-dd HHmm", Locale.US).format(new Date());
            File report = new File(dir, gameName + " log " + date + ".txt");
            try (OutputStream out = new FileOutputStream(report)) {
                writeLogReport(out, FILE_LOG_LIMIT);
            }
            Uri uri = ShareProvider.uriFor(this, report);
            Intent send = new Intent(Intent.ACTION_SEND);
            send.setType("text/plain");
            send.putExtra(Intent.EXTRA_STREAM, uri);
            send.putExtra(Intent.EXTRA_SUBJECT, gameName + " log");
            send.setClipData(ClipData.newRawUri(report.getName(), uri));
            send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            runOnUiThread(() -> startActivity(Intent.createChooser(send, "Share the " + gameName + " log")));
            return "Logs ready: pick where to send them.";
        });
    }

    private void writeLogReport(OutputStream out, long limit) throws IOException {
        StringBuilder head = new StringBuilder();
        head.append(gameName).append(" for Android ").append(appVersion()).append('\n');
        head.append("Device: ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL);
        if (Build.VERSION.SDK_INT >= 31) head.append(" (").append(Build.SOC_MANUFACTURER).append(' ').append(Build.SOC_MODEL).append(')');
        head.append("\nAndroid ").append(Build.VERSION.RELEASE).append(" (API ").append(Build.VERSION.SDK_INT).append(")\n");
        out.write(head.toString().getBytes(StandardCharsets.UTF_8));
        appendExitReasons(out);
        boolean any = appendLog(out, "Latest run", new File(filesDir(), "ib3rt.log"), limit);
        any |= appendLog(out, "The run before", new File(filesDir(), "ib3rt-previous.log"), limit);
        if (!any) throw new IOException("There are no logs yet: play the game first.");
    }

    // Android's own record of how the app's last processes ended. When the game dies without a word in
    // its log (killed for memory, an abort in native code, a Java exception), this says which, and for
    // a native crash includes the readable parts of Android's crash report (the tombstone).
    private void appendExitReasons(OutputStream out) throws IOException {
        StringBuilder s = new StringBuilder("\n===== How the app closed recently (Android's record) =====\n");
        try {
            android.app.ActivityManager am = getSystemService(android.app.ActivityManager.class);
            List<android.app.ApplicationExitInfo> exits = am.getHistoricalProcessExitReasons(null, 0, 5);
            if (exits.isEmpty()) s.append("(none)\n");
            SimpleDateFormat time = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US);
            boolean traced = false;
            for (android.app.ApplicationExitInfo e : exits) {
                s.append(time.format(new Date(e.getTimestamp()))).append("  ").append(exitReason(e.getReason()))
                        .append(", status ").append(e.getStatus())
                        .append(", memory ").append(e.getPss() / 1024).append(" MB (PSS) / ").append(e.getRss() / 1024).append(" MB (RSS)");
                if (e.getDescription() != null) s.append("\n    ").append(e.getDescription());
                s.append('\n');
                int reason = e.getReason();
                boolean crash = reason == android.app.ApplicationExitInfo.REASON_CRASH_NATIVE
                        || reason == android.app.ApplicationExitInfo.REASON_ANR;
                if (crash && !traced) {  // the most recent one is enough, and they are long
                    InputStream trace = e.getTraceInputStream();
                    if (trace != null) {
                        traced = true;
                        String text = readAll(trace);
                        // Since Android 12 a native crash report is a protobuf: keep its readable text.
                        if (reason == android.app.ApplicationExitInfo.REASON_CRASH_NATIVE) text = printableRuns(text);
                        if (text.length() > 16000) text = text.substring(0, 16000) + "\n[...]";
                        s.append(text).append('\n');
                    }
                }
            }
        } catch (Exception ex) {
            s.append("(not available: ").append(ex).append(")\n");
        }
        out.write(s.toString().getBytes(StandardCharsets.UTF_8));
    }

    private static String exitReason(int reason) {
        switch (reason) {
            case android.app.ApplicationExitInfo.REASON_EXIT_SELF: return "exited by itself";
            case android.app.ApplicationExitInfo.REASON_SIGNALED: return "killed by a signal";
            case android.app.ApplicationExitInfo.REASON_LOW_MEMORY: return "KILLED FOR LOW MEMORY";
            case android.app.ApplicationExitInfo.REASON_CRASH: return "JAVA CRASH";
            case android.app.ApplicationExitInfo.REASON_CRASH_NATIVE: return "NATIVE CRASH";
            case android.app.ApplicationExitInfo.REASON_ANR: return "NOT RESPONDING (ANR)";
            case android.app.ApplicationExitInfo.REASON_INITIALIZATION_FAILURE: return "failed to start";
            case android.app.ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE: return "killed for using too many resources";
            case android.app.ApplicationExitInfo.REASON_USER_REQUESTED: return "closed by the user";
            case android.app.ApplicationExitInfo.REASON_USER_STOPPED: return "stopped by the user";
            case android.app.ApplicationExitInfo.REASON_DEPENDENCY_DIED: return "a dependency died";
            case android.app.ApplicationExitInfo.REASON_FREEZER: return "killed while frozen";
            case android.app.ApplicationExitInfo.REASON_PACKAGE_UPDATED: return "app updated";
            case android.app.ApplicationExitInfo.REASON_OTHER: return "other (" + reason + ")";
            default: return "reason " + reason;
        }
    }

    // Runs of 4+ printable characters, one per line (like the "strings" tool).
    private static String printableRuns(String s) {
        StringBuilder out = new StringBuilder(), run = new StringBuilder();
        for (int i = 0; i <= s.length(); i++) {
            char c = i < s.length() ? s.charAt(i) : 0;
            if (c >= 0x20 && c < 0x7f) {
                run.append(c);
                continue;
            }
            if (run.length() >= 4) out.append(run).append('\n');
            run.setLength(0);
        }
        return out.toString();
    }

    // Appends a log under a heading; a very long one keeps its start and its end.
    private static boolean appendLog(OutputStream out, String heading, File log, long limit) throws IOException {
        out.write(("\n===== " + heading + " (" + log.getName() + ") =====\n").getBytes(StandardCharsets.UTF_8));
        if (!log.isFile()) {
            out.write("(none)\n".getBytes(StandardCharsets.UTF_8));
            return false;
        }
        // The game writes a line for every file it opens ("OutPath"): thousands of lines that tell nothing.
        // Without them a log is a fraction of the size, and a pasted report (Discord and some keyboards cut
        // long text) keeps the lines that matter: the perf lines and the end of the run.
        StringBuilder text = new StringBuilder();
        try (java.io.BufferedReader in = new java.io.BufferedReader(new java.io.InputStreamReader(new FileInputStream(log), StandardCharsets.UTF_8))) {
            for (String line; (line = in.readLine()) != null; )
                if (!line.contains("] [guest] OutPath : ")) text.append(line).append('\n');
        }
        byte[] all = text.toString().getBytes(StandardCharsets.UTF_8);
        if (all.length <= limit) {
            out.write(all);
        } else {
            int head = (int) (limit / 4), tail = (int) (limit - head);
            out.write(all, 0, head);
            out.write(("\n[... " + (all.length - head - tail) + " bytes left out ...]\n").getBytes(StandardCharsets.UTF_8));
            out.write(all, all.length - tail, tail);
        }
        return true;
    }

    private static void copy(RandomAccessFile f, OutputStream out, long count) throws IOException {
        byte[] buf = new byte[1 << 16];
        while (count > 0) {
            int n = f.read(buf, 0, (int) Math.min(buf.length, count));
            if (n <= 0) break;
            out.write(buf, 0, n);
            count -= n;
        }
    }

    private static String readAll(InputStream stream) throws IOException {
        try (InputStream in = stream) {
            byte[] all = new byte[1 << 16];
            int n = 0;
            for (int r; n < all.length && (r = in.read(all, n, all.length - n)) > 0; ) n += r;
            return new String(all, 0, n, StandardCharsets.UTF_8);
        }
    }

    private static void copyTree(File from, File to) throws IOException {
        if (from.isDirectory()) {
            if (!to.isDirectory() && !to.mkdirs()) throw new IOException("Could not create " + to);
            File[] children = from.listFiles();
            if (children != null)
                for (File c : children) copyTree(c, new File(to, c.getName()));
        } else if (from.isFile()) {
            try (InputStream in = new FileInputStream(from); OutputStream out = new FileOutputStream(to)) {
                byte[] buf = new byte[1 << 16];
                for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
            }
        }
    }

    // Whether the stream contains `text`. A bundle id is stored as plain text, even in binary plists.
    private static boolean contains(InputStream stream, String text) throws IOException {
        try (InputStream in = stream) {
            byte[] all = new byte[1 << 20];
            int n = 0;
            for (int r; n < all.length && (r = in.read(all, n, all.length - n)) > 0; ) n += r;
            return new String(all, 0, n, "ISO-8859-1").contains(text);
        }
    }

    private static void deleteTree(File f) {
        File[] children = f.listFiles();
        if (children != null)
            for (File c : children) deleteTree(c);
        f.delete();
    }
}
