package com.ib3port.game;

import android.app.AlertDialog;
import android.app.NativeActivity;
import android.content.DialogInterface;
import android.text.InputType;
import android.util.TypedValue;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/**
 * The game (libib3.so, a NativeActivity). Adds what native code cannot do on its own: the game's
 * iOS alerts are shown here as Android dialogs (see src/port/android/dialogs.cpp).
 */
public class GameActivity extends NativeActivity {
    /** Whether the game has run in this process (it keeps its save files in memory until it exits). */
    static boolean started;

    @Override
    protected void onCreate(android.os.Bundle state) {
        started = true;
        // A Java exception ends the app without a word in the game's log: add the stack trace to it.
        Thread.UncaughtExceptionHandler previous = Thread.getDefaultUncaughtExceptionHandler();
        java.io.File dir = getExternalFilesDir(null);
        java.io.File log = new java.io.File(dir != null ? dir : getFilesDir(), "ib3rt.log");
        Thread.setDefaultUncaughtExceptionHandler((thread, ex) -> {
            try (java.io.PrintWriter w = new java.io.PrintWriter(new java.io.FileOutputStream(log, true))) {
                w.println("[ERROR] JAVA CRASH on thread \"" + thread.getName() + "\":");
                ex.printStackTrace(w);
            } catch (Exception ignored) {
            }
            if (previous != null) previous.uncaughtException(thread, ex);
        });
        super.onCreate(state);
    }

    /**
     * An HTTP request for the ClashMob server (src/game/clashmob.cpp), made on the calling thread (not the UI
     * thread). headers: "Name: value" lines. Returns the status as 4 big-endian bytes followed by the body, or
     * null when the server could not be reached.
     */
    static byte[] httpRequest(String method, String url, String headers, byte[] body, int timeoutMs) {
        java.net.HttpURLConnection c = null;
        try {
            c = (java.net.HttpURLConnection) new java.net.URL(url).openConnection();
            c.setRequestMethod(method);
            c.setConnectTimeout(timeoutMs);
            c.setReadTimeout(timeoutMs);
            c.setUseCaches(false);
            for (String line : headers.split("\n")) {
                int colon = line.indexOf(':');
                if (colon > 0) c.setRequestProperty(line.substring(0, colon).trim(), line.substring(colon + 1).trim());
            }
            if (body != null && body.length > 0) {
                c.setDoOutput(true);
                c.setFixedLengthStreamingMode(body.length);
                try (java.io.OutputStream out = c.getOutputStream()) {
                    out.write(body);
                }
            }
            int status = c.getResponseCode();
            java.io.InputStream in = status >= 400 ? c.getErrorStream() : c.getInputStream();
            java.io.ByteArrayOutputStream all = new java.io.ByteArrayOutputStream();
            all.write(new byte[] {(byte) (status >> 24), (byte) (status >> 16), (byte) (status >> 8), (byte) status});
            if (in != null) {
                try (java.io.InputStream s = in) {
                    byte[] buf = new byte[16384];
                    for (int n; (n = s.read(buf)) > 0;) all.write(buf, 0, n);
                }
            }
            return all.toByteArray();
        } catch (Exception e) {
            return null;
        } finally {
            if (c != null) c.disconnect();
        }
    }

    /** Reports the button chosen (and the text typed, for text prompts) for alert `id`. */
    static native void nativeAlertResult(int id, int button, String text);

    /**
     * Shows an alert; called by the game's thread. style: 0 buttons only, 1 password prompt,
     * 2 text prompt (buttons are then {cancel, ok}). cancel: index of the button Back picks, or -1.
     */
    public void showAlert(int id, String title, String message, String[] buttons, int cancel, int style, String text) {
        runOnUiThread(() -> {
            AlertDialog.Builder b = new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert);
            if (title != null && !title.isEmpty()) b.setTitle(title);
            final boolean[] answered = {false};
            final EditText field = style != 0 ? new EditText(this) : null;
            final AlertDialog[] dialog = {null};
            java.util.function.IntConsumer answer = index -> {
                if (answered[0]) return;
                answered[0] = true;
                nativeAlertResult(id, index, field != null ? field.getText().toString() : "");
                if (dialog[0] != null) dialog[0].dismiss();
            };
            if (field != null) {
                field.setText(text);
                field.setSingleLine();
                field.setInputType(InputType.TYPE_CLASS_TEXT | (style == 1 ? InputType.TYPE_TEXT_VARIATION_PASSWORD : 0));
                LinearLayout box = new LinearLayout(this);
                box.setOrientation(LinearLayout.VERTICAL);
                int pad = dp(20);
                box.setPadding(pad, pad / 2, pad, 0);
                if (message != null && !message.isEmpty()) {
                    TextView t = new TextView(this);
                    t.setText(message);
                    box.addView(t);
                }
                box.addView(field);
                b.setView(box);
            } else if (message != null && !message.isEmpty() && buttons.length <= 3) {
                b.setMessage(message);
            }
            if (buttons.length <= 3) {
                // Android lays out up to three buttons: negative, neutral, positive (left to right).
                DialogInterface.OnClickListener click = (d, which) ->
                    answer.accept(which == DialogInterface.BUTTON_POSITIVE ? buttons.length - 1
                                  : which == DialogInterface.BUTTON_NEGATIVE ? 0 : 1);
                if (buttons.length >= 1) b.setPositiveButton(buttons[buttons.length - 1], click);
                if (buttons.length >= 2) b.setNegativeButton(buttons[0], click);
                if (buttons.length == 3) b.setNeutralButton(buttons[1], click);
            } else {
                // Many buttons: a scrolling column of them under the message.
                LinearLayout column = new LinearLayout(this);
                column.setOrientation(LinearLayout.VERTICAL);
                int pad = dp(16);
                column.setPadding(pad, pad / 2, pad, pad / 2);
                if (message != null && !message.isEmpty()) {
                    TextView t = new TextView(this);
                    t.setText(message);
                    t.setPadding(0, 0, 0, pad / 2);
                    column.addView(t);
                }
                for (int i = 0; i < buttons.length; i++) {
                    final int index = i;
                    Button button = new Button(this);
                    button.setText(buttons[i]);
                    button.setOnClickListener(v -> answer.accept(index));
                    column.addView(button);
                }
                ScrollView scroll = new ScrollView(this);
                scroll.addView(column);
                b.setView(scroll);
            }
            b.setCancelable(cancel >= 0);
            if (cancel >= 0) b.setOnCancelListener(d -> answer.accept(cancel));
            dialog[0] = b.create();
            dialog[0].show();
        });
    }

    private int dp(float v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }
}
