package com.ib3port.game;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.drawable.Drawable;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
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
import java.util.Enumeration;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * Starts the game, or first installs it from the player's own .ipa: the Payload/SwordGame.app
 * folder is extracted into the app's files folder (game/). The same code serves Infinity Blade II
 * and III: the app's label is the game's name, and the manifest's meta-data says which .ipa to
 * accept (ib.bundleId), which version to ask for (ib.ipaVersion) and the space it needs (ib.size).
 * For testing: am start -n <package>/com.ib3port.game.LauncherActivity --es ipa /path/to/file.ipa
 */
public class LauncherActivity extends Activity {
    private static final int PICK_IPA = 1;
    private static final String APP_PREFIX = "Payload/SwordGame.app/";

    private String gameName, ipaVersion, bundleId, size;
    private TextView status;
    private ProgressBar progress;
    private Button choose;

    private File filesDir() {
        File dir = getExternalFilesDir(null);
        return dir != null ? dir : getFilesDir();
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
        String ipa = getIntent().getStringExtra("ipa");
        if (ipa == null && installed(filesDir())) {
            startGame();
            return;
        }
        buildScreen();
        if (ipa != null) install(null, new File(ipa));
    }

    private void startGame() {
        startActivity(new Intent(this, GameActivity.class));
        overridePendingTransition(0, 0);
        finish();
    }

    private int dp(float v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }

    private void buildScreen() {
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

        status = new TextView(this);
        status.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        status.setTextColor(0xFFDDDDDD);
        status.setGravity(Gravity.CENTER);
        status.setPadding(0, dp(10), 0, 0);
        column.addView(status, new LinearLayout.LayoutParams(dp(520), LinearLayout.LayoutParams.WRAP_CONTENT));

        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(Color.BLACK);
        LinearLayout center = new LinearLayout(this);
        center.setGravity(Gravity.CENTER);
        center.addView(column);
        scroll.addView(center);
        setContentView(scroll);
    }

    private void pickIpa() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, PICK_IPA);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request == PICK_IPA && result == RESULT_OK && data != null && data.getData() != null) install(data.getData(), null);
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
                        extract(openZip(pfd));
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
    private ZipFile openZip(ParcelFileDescriptor pfd) throws IOException {
        try {
            return new ZipFile(new File("/proc/self/fd/" + pfd.getFd()));
        } catch (IOException e) {
            File copy = new File(getCacheDir(), "install.ipa");
            setStatus("Copying the .ipa...");
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
