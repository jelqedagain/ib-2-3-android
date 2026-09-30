package com.ib3port.game;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.drawable.Drawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
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
 * folder is extracted into the app's files folder (game/). The same code serves Infinity Blade II
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
    private static final String MARKER = "ib-port-saves.txt";  // in backups: which game they are from
    private static final long FILE_LOG_LIMIT = 4L << 20;  // bytes of each log in a shared file (a Discord upload is 10 MB)
    private static final long CLIP_LOG_LIMIT = 96L << 10;  // and in the clipboard, which holds well under 1 MB

    private String gameName, ipaVersion, bundleId, size;
    private TextView status;
    private ProgressBar progress;
    private Button choose;
    private final List<Button> menuButtons = new ArrayList<>();

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
        startActivity(new Intent(this, GameActivity.class));
        overridePendingTransition(0, 0);
        finish();
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

    // The screen's column, starting with the game's icon and name.
    private LinearLayout column() {
        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        column.setGravity(Gravity.CENTER_HORIZONTAL);
        column.setPadding(dp(32), dp(24), dp(32), dp(24));

        ImageView icon = new ImageView(this);
        Drawable d = getApplicationInfo().loadIcon(getPackageManager());
        icon.setImageDrawable(d);
        column.addView(icon, new LinearLayout.LayoutParams(dp(72), dp(72)));

        TextView title = new TextView(this);
        title.setText(gameName);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 26);
        title.setTextColor(Color.WHITE);
        title.setGravity(Gravity.CENTER);
        title.setPadding(0, dp(12), 0, dp(8));
        column.addView(title);
        return column;
    }

    private void addStatus(LinearLayout column) {
        status = new TextView(this);
        status.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        status.setTextColor(0xFFDDDDDD);
        status.setGravity(Gravity.CENTER);
        status.setPadding(0, dp(10), 0, 0);
        column.addView(status, new LinearLayout.LayoutParams(dp(520), LinearLayout.LayoutParams.WRAP_CONTENT));
    }

    private void show(LinearLayout column) {
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setVerticalScrollBarEnabled(false);
        scroll.setBackgroundColor(Color.BLACK);
        LinearLayout center = new LinearLayout(this);
        center.setGravity(Gravity.CENTER);
        center.addView(column);
        scroll.addView(center);
        setContentView(scroll);
    }

    private void buildInstallScreen() {
        LinearLayout column = column();
        TextView body = new TextView(this);
        body.setText("Bring your own IPA.\n\nThis app runs the original iOS release of " + gameName + " (version " + ipaVersion + "). "
                + "Choose your .ipa file and the game is installed into this app, which needs about " + size + " of free space. "
                + "Your .ipa is only read, never changed or sent anywhere.");
        body.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        body.setTextColor(0xFFCCCCCC);
        body.setGravity(Gravity.CENTER);
        column.addView(body, new LinearLayout.LayoutParams(dp(520), LinearLayout.LayoutParams.WRAP_CONTENT));

        choose = new Button(this);
        choose.setText("Choose .ipa file");
        choose.setOnClickListener(v -> pickIpa());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(20);
        column.addView(choose, lp);

        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(1000);
        progress.setVisibility(View.GONE);
        LinearLayout.LayoutParams pp = new LinearLayout.LayoutParams(dp(420), LinearLayout.LayoutParams.WRAP_CONTENT);
        pp.topMargin = dp(20);
        column.addView(progress, pp);

        addStatus(column);
        show(column);
    }

    private void buildMenuScreen() {
        LinearLayout column = column();
        TextView version = new TextView(this);
        version.setText("Android port " + appVersion());
        version.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        version.setTextColor(0xFF888888);
        version.setGravity(Gravity.CENTER);
        column.addView(version);

        Button play = new Button(this);
        play.setText("Play");
        play.setTextSize(TypedValue.COMPLEX_UNIT_SP, 20);
        play.setOnClickListener(v -> startGame());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(dp(260), LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(20);
        column.addView(play, lp);
        menuButtons.add(play);

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER);
        addMenuButton(row, "Back up saves", v -> pickBackupFile());
        addMenuButton(row, "Restore saves", v -> pickRestoreFile());
        addMenuButton(row, "Share logs", v -> copyLogs()).setOnLongClickListener(v -> {
            shareLogFile();
            return true;
        });
        LinearLayout.LayoutParams rp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        rp.topMargin = dp(12);
        column.addView(row, rp);

        addStatus(column);
        show(column);
    }

    private Button addMenuButton(LinearLayout row, String text, View.OnClickListener click) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setOnClickListener(click);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.leftMargin = lp.rightMargin = dp(6);
        row.addView(b, lp);
        menuButtons.add(b);
        return b;
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
                    startGame();
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
                    throw new IOException("Extracting " + z.getName() + " failed: " + ex.getMessage());
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
        for (Button b : menuButtons) b.setEnabled(false);
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
                for (Button b : menuButtons) b.setEnabled(true);
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
        boolean any = appendLog(out, "Latest run", new File(filesDir(), "ib3rt.log"), limit);
        any |= appendLog(out, "The run before", new File(filesDir(), "ib3rt-previous.log"), limit);
        if (!any) throw new IOException("There are no logs yet: play the game first.");
    }

    // Appends a log under a heading; a very long one keeps its start and its end.
    private static boolean appendLog(OutputStream out, String heading, File log, long limit) throws IOException {
        out.write(("\n===== " + heading + " (" + log.getName() + ") =====\n").getBytes(StandardCharsets.UTF_8));
        if (!log.isFile()) {
            out.write("(none)\n".getBytes(StandardCharsets.UTF_8));
            return false;
        }
        try (RandomAccessFile f = new RandomAccessFile(log, "r")) {
            long length = f.length();
            if (length <= limit) {
                copy(f, out, length);
            } else {
                long head = limit / 4, tail = limit - head;
                copy(f, out, head);
                out.write(("\n[... " + (length - head - tail) + " bytes left out ...]\n").getBytes(StandardCharsets.UTF_8));
                f.seek(length - tail);
                copy(f, out, tail);
            }
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
