package com.ib3port.game;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Collection;
import java.util.LinkedHashMap;
import java.util.Map;

/**
 * saveedit-pending.ini: changes waiting for the next Play (src/game/saveedit.cpp applies them). The Edit save
 * and Cheats pages each own some keys; each writes only its own and keeps the other's.
 */
final class Pending {
    private Pending() {
    }

    static File file(File dir) {
        return new File(dir, "saveedit-pending.ini");
    }

    // key=value lines of an ini-like file (sections and comments skipped).
    static Map<String, String> read(File f) {
        Map<String, String> out = new LinkedHashMap<>();
        if (!f.isFile()) return out;
        try (InputStream in = new FileInputStream(f)) {
            byte[] all = new byte[(int) Math.min(f.length(), 1 << 16)];
            int n = 0;
            for (int r; n < all.length && (r = in.read(all, n, all.length - n)) > 0; ) n += r;
            for (String line : new String(all, 0, n, StandardCharsets.UTF_8).split("\n")) {
                line = line.trim();
                int eq = line.indexOf('=');
                if (eq <= 0 || line.startsWith(";") || line.startsWith("[")) continue;
                out.put(line.substring(0, eq).trim(), line.substring(eq + 1).trim());
            }
        } catch (IOException ignored) {
        }
        return out;
    }

    // Replaces the keys in `owned` with `edits` (their new values), keeping every other waiting change.
    static void write(File dir, Collection<String> owned, Map<String, String> edits) throws IOException {
        Map<String, String> all = read(file(dir));
        for (String k : owned) all.remove(k);
        all.putAll(edits);
        if (all.isEmpty()) {
            file(dir).delete();
            return;
        }
        StringBuilder s = new StringBuilder("; Written by the launcher; the game applies it the next time you press Play.\n[Edits]\n");
        for (Map.Entry<String, String> e : all.entrySet()) s.append(e.getKey()).append('=').append(e.getValue()).append('\n');
        try (OutputStream out = new FileOutputStream(file(dir))) {
            out.write(s.toString().getBytes(StandardCharsets.UTF_8));
        }
    }
}
