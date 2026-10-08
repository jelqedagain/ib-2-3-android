package com.ib3port.game;

import android.app.Activity;
import android.app.AlertDialog;
import android.text.InputFilter;
import android.text.InputType;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.SecureRandom;

/**
 * IB3's ClashMob name: the name shown for the player on the ClashMob server's leaderboards (the public page and the
 * admin page; the game itself shows no names). The server checks it (3 to 16 letters, numbers and spaces, not taken,
 * not offensive). The player is known to the server by the id and key in clashmob-id / clashmob-key next to the
 * saves, the same ones the game sends (src/game/clashmob.cpp); they are made here if the game has not made them yet.
 */
final class ClashMobName {
    static final String DEFAULT_SERVER = "https://ibclashmobs.dev";

    private ClashMobName() {}

    /** The server's address, or null when online ClashMobs are off ([ClashMob] Server=off). */
    static String server(IniFile ini) {
        String s = ini.get("ClashMob", "Server", "").trim();
        if (s.equalsIgnoreCase("off")) return null;
        if (s.isEmpty()) s = DEFAULT_SERVER;
        while (s.endsWith("/")) s = s.substring(0, s.length() - 1);
        return s;
    }

    /** "X-ClashMob-Player: ..." and "X-ClashMob-Key: ..." header lines for GameActivity.httpRequest. */
    static String headers(File dir) throws IOException {
        SecureRandom random = new SecureRandom();
        String id = readOrMake(new File(dir, "clashmob-id"), () -> String.format("port-%08x%08x", random.nextInt(), random.nextInt()));
        String key = readOrMake(new File(dir, "clashmob-key"), () -> String.format("%08x%08x%08x%08x",
                random.nextInt(), random.nextInt(), random.nextInt(), random.nextInt()));
        return "X-ClashMob-Player: " + id + "\nX-ClashMob-Key: " + key + "\nX-ClashMob-Game: ib3\n";
    }

    private interface Maker { String make(); }

    private static String readOrMake(File f, Maker maker) throws IOException {
        if (f.exists()) {
            String s = new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8).trim();
            if (!s.isEmpty()) return s;
        }
        String s = maker.make();
        try (FileOutputStream out = new FileOutputStream(f)) {
            out.write(s.getBytes(StandardCharsets.UTF_8));
        }
        return s;
    }

    /** The server's answer: {status, body}, or null when it could not be reached. */
    static String[] ask(String method, String url, String headers, String body) {
        byte[] r = GameActivity.httpRequest(method, url, headers + "Content-Type: text/plain; charset=utf-8\n",
                body == null ? new byte[0] : body.getBytes(StandardCharsets.UTF_8), 10000);
        if (r == null || r.length < 4) return null;
        int status = (r[0] & 0xFF) << 24 | (r[1] & 0xFF) << 16 | (r[2] & 0xFF) << 8 | (r[3] & 0xFF);
        return new String[] {Integer.toString(status), new String(r, 4, r.length - 4, StandardCharsets.UTF_8)};
    }

    /** A JSON string value ("name", "error") from the server's small answers. */
    static String field(String json, String key) {
        java.util.regex.Matcher m = java.util.regex.Pattern.compile("\"" + key + "\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"").matcher(json);
        return m.find() ? m.group(1).replace("\\\"", "\"").replace("\\\\", "\\") : null;
    }

    /** Asks for a name and saves it on the server; done(name) with the name saved ("" when cleared). */
    static void edit(Activity a, IniFile ini, String current, java.util.function.Consumer<String> done) {
        String server = server(ini);
        if (server == null) {
            Toast.makeText(a, "Online ClashMobs are off: turn them on to set a name.", Toast.LENGTH_LONG).show();
            return;
        }
        int pad = Ui.dp(a, 20);
        LinearLayout box = new LinearLayout(a);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(pad, Ui.dp(a, 8), pad, 0);
        EditText input = new EditText(a);
        input.setSingleLine(true);
        input.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_CAP_WORDS);
        input.setFilters(new InputFilter[] {new InputFilter.LengthFilter(16)});
        input.setText(current);
        input.setSelection(input.getText().length());
        box.addView(input);
        TextView message = new TextView(a);
        message.setText("3 to 16 letters, numbers and spaces. Shown on the ClashMob leaderboards.");
        message.setPadding(0, Ui.dp(a, 6), 0, 0);
        box.addView(message);

        AlertDialog d = new AlertDialog.Builder(a)
                .setTitle("ClashMob name")
                .setView(box)
                .setPositiveButton("Save", null)
                .setNeutralButton("Remove name", null)
                .setNegativeButton("Cancel", null)
                .create();
        d.setOnShowListener(x -> {
            Button save = d.getButton(AlertDialog.BUTTON_POSITIVE), clear = d.getButton(AlertDialog.BUTTON_NEUTRAL);
            View.OnClickListener send = v -> {
                String name = v == clear ? "" : input.getText().toString().trim();
                save.setEnabled(false);
                clear.setEnabled(false);
                message.setText("Saving…");
                File dir = ini.file().getParentFile();
                new Thread(() -> {
                    String[] r;
                    try {
                        r = ask("POST", server + "/player/name", headers(dir), name);
                    } catch (IOException e) {
                        r = null;
                    }
                    String[] answer = r;
                    a.runOnUiThread(() -> {
                        save.setEnabled(true);
                        clear.setEnabled(true);
                        if (answer == null) {
                            message.setText("Could not reach the ClashMob server. Check your internet connection and try again.");
                        } else if (answer[0].equals("200")) {
                            String saved = field(answer[1], "name");
                            done.accept(saved == null ? name : saved);
                            d.dismiss();
                            Toast.makeText(a, name.isEmpty() ? "Name removed" : "Name saved", Toast.LENGTH_SHORT).show();
                        } else {
                            String error = field(answer[1], "error");
                            message.setText(error != null ? error : "The server refused it (" + answer[0] + ").");
                        }
                    });
                }).start();
            };
            save.setOnClickListener(send);
            clear.setOnClickListener(send);
        });
        d.show();
    }

    /** Fetches the name the server has (it may have been changed by an admin); done(name) on the UI thread. */
    static void refresh(Activity a, IniFile ini, java.util.function.Consumer<String> done) {
        String server = server(ini);
        if (server == null) return;
        File dir = ini.file().getParentFile();
        new Thread(() -> {
            try {
                String[] r = ask("GET", server + "/player", headers(dir), null);
                if (r != null && r[0].equals("200")) {
                    String name = field(r[1], "name");
                    if (name != null) a.runOnUiThread(() -> done.accept(name));
                }
            } catch (IOException ignored) {
            }
        }).start();
    }
}
