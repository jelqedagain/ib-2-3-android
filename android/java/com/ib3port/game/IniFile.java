package com.ib3port.game;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * settings.ini, which the game reads when it starts (src/settings.cpp). Edits keep every other line
 * as it is (key bindings, keys a newer version added); section and key names ignore case, as there.
 */
final class IniFile {
    private final File file;
    private final List<String> lines = new ArrayList<>();

    IniFile(File file) {
        this.file = file;
        if (!file.isFile()) return;
        try (InputStream in = new FileInputStream(file)) {
            byte[] all = new byte[(int) Math.min(file.length(), 1 << 20)];
            int n = 0;
            for (int r; n < all.length && (r = in.read(all, n, all.length - n)) > 0; ) n += r;
            for (String line : new String(all, 0, n, StandardCharsets.UTF_8).split("\n", -1)) lines.add(line.replace("\r", ""));
            if (!lines.isEmpty() && lines.get(lines.size() - 1).isEmpty()) lines.remove(lines.size() - 1);
        } catch (IOException ignored) {
        }
    }

    private static boolean isSection(String line, String section) {
        String t = line.trim();
        return t.startsWith("[") && t.endsWith("]") && t.substring(1, t.length() - 1).trim().equalsIgnoreCase(section);
    }

    private static String keyOf(String line) {
        int eq = line.indexOf('=');
        return eq > 0 && !line.trim().startsWith(";") ? line.substring(0, eq).trim() : null;
    }

    File file() {
        return file;
    }

    int get(String section, String key, int def) {
        try {
            return Integer.parseInt(get(section, key, Integer.toString(def)));
        } catch (NumberFormatException e) {
            return def;
        }
    }

    String get(String section, String key, String def) {
        boolean in = false;
        for (String line : lines) {
            if (line.trim().startsWith("[")) {
                in = isSection(line, section);
            } else if (in && key.equalsIgnoreCase(keyOf(line))) {
                return line.substring(line.indexOf('=') + 1).trim();
            }
        }
        return def;
    }

    void set(String section, String key, int value) {
        set(section, key, Integer.toString(value));
    }

    void set(String section, String key, String value) {
        String entry = key + "=" + value;
        int sectionAt = -1, end = lines.size();
        for (int i = 0; i < lines.size(); i++) {
            String line = lines.get(i);
            if (line.trim().startsWith("[")) {
                if (sectionAt >= 0) {
                    end = i;
                    break;
                }
                if (isSection(line, section)) sectionAt = i;
            } else if (sectionAt >= 0 && key.equalsIgnoreCase(keyOf(line))) {
                lines.set(i, entry);
                return;
            }
        }
        if (sectionAt < 0) {
            if (!lines.isEmpty() && !lines.get(lines.size() - 1).trim().isEmpty()) lines.add("");
            lines.add("[" + section + "]");
            lines.add(entry);
            return;
        }
        while (end > sectionAt + 1 && lines.get(end - 1).trim().isEmpty()) end--;  // before the blank lines
        lines.add(end, entry);
    }

    void save() throws IOException {
        StringBuilder s = new StringBuilder();
        for (String line : lines) s.append(line).append('\n');
        File tmp = new File(file.getPath() + ".tmp");
        try (OutputStream out = new FileOutputStream(tmp)) {
            out.write(s.toString().getBytes(StandardCharsets.UTF_8));
        }
        if (!tmp.renameTo(file)) throw new IOException("Could not save the settings.");
    }
}
