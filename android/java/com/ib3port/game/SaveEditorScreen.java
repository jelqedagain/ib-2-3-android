package com.ib3port.game;

import android.app.Activity;
import android.content.res.ColorStateList;
import android.text.InputFilter;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.inputmethod.EditorInfo;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.text.DateFormat;
import java.text.NumberFormat;
import java.util.Date;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;

/**
 * The Edit save page. The game writes the player's current numbers to saveedit-current.ini whenever it
 * runs; this page shows them and writes the changes to saveedit-pending.ini, which the game puts into the
 * save the next time it loads it (src/game/saveedit.cpp, where the keys below come from).
 */
final class SaveEditorScreen {
    // key, label, line under it, games (2 = IB2, 3 = IB3)
    private static final String[][] NUMBERS = {
        {"Gold", "Gold", "", "23"},
        {"Chips", "Chips", "The second currency.", "3"},
        {"Level", "Level", "A higher level also gives the 2 stat points per level a level-up gives. The new level starts with 0 XP.", "23"},
        {"XP", "XP", "Toward the next level.", "23"},
        {"StatPoints", "Unspent stat points", "Spend them on the Stats screen in the game.", "2"},
        {"StatPoints", "Unspent skill points", "Spend them on the Skills screen in the game.", "3"},
        {"StatHealth", "Health", "Points put into the stat.", "23"},
        {"StatDamage", "Attack", "Points put into the stat.", "23"},
        {"StatShield", "Shield", "Points put into the stat.", "23"},
        {"StatMagic", "Magic", "Points put into the stat.", "23"},
        {"Bloodline", "Bloodline", "Which bloodline (rebirth) you are on.", "2"},
        {"Bloodline", "Awakening", "Your awakening (rebirth) number, shown on the Stats screen.", "3"},
        {"GemCarry", "Gem bag upgrades", "How many times the gem bag was made bigger.", "3"},
    };
    private static final String[][] ACTIONS = {
        {"GiveAllItems", "Give every item", "Adds every weapon, shield, armor, helmet and magic to your inventory.", "23"},
        {"GiveAllPerks", "Give all perks", "The developers' cheat: it also makes your character level 50 with every stat at 100.", "3"},
    };

    private final Activity a;
    private final File dir;
    private final String game;
    private final Map<String, Long> current = new LinkedHashMap<>();
    private final Map<String, String> reported = new LinkedHashMap<>();  // the same file as text, with the game's [Limits]
    private final Map<String, String> pending = new LinkedHashMap<>();
    private final Map<String, EditText> inputs = new LinkedHashMap<>();
    private final Map<String, Switch> switches = new LinkedHashMap<>();
    private long currentTime;
    private boolean gameRunning;

    private SaveEditorScreen(Activity a, File dir, boolean ib2) {
        this.a = a;
        this.dir = dir;
        this.game = ib2 ? "2" : "3";
        read(new File(dir, "saveedit-current.ini"), current, true);
        read(new File(dir, "saveedit-current.ini"), reported, false);
        read(new File(dir, "saveedit-pending.ini"), pending, false);
        currentTime = new File(dir, "saveedit-current.ini").lastModified();
    }

    static View build(Activity a, File dir, boolean ib2, boolean gameRunning, Runnable back) {
        return new SaveEditorScreen(a, dir, ib2).build(gameRunning, back);
    }

    private int dp(float v) {
        return Ui.dp(a, v);
    }

    @SuppressWarnings("unchecked")
    private static void read(File f, Map<String, ?> out, boolean numbers) {
        if (!f.isFile()) return;
        try (InputStream in = new FileInputStream(f)) {
            byte[] all = new byte[(int) Math.min(f.length(), 1 << 16)];
            int n = 0;
            for (int r; n < all.length && (r = in.read(all, n, all.length - n)) > 0; ) n += r;
            for (String line : new String(all, 0, n, StandardCharsets.UTF_8).split("\n")) {
                line = line.trim();
                int eq = line.indexOf('=');
                if (eq <= 0 || line.startsWith(";") || line.startsWith("[")) continue;
                String key = line.substring(0, eq).trim(), value = line.substring(eq + 1).trim();
                if (!numbers) {
                    ((Map<String, String>) out).put(key, value);
                } else {
                    try {
                        ((Map<String, Long>) out).put(key, Long.parseLong(value));
                    } catch (NumberFormatException ignored) {
                    }
                }
            }
        } catch (IOException ignored) {
        }
    }

    private View build(boolean gameRunning, Runnable back) {
        this.gameRunning = gameRunning;
        LinearLayout root = new LinearLayout(a);
        root.setOrientation(LinearLayout.VERTICAL);
        Ui.screenBackground(root);

        LinearLayout header = new LinearLayout(a);
        header.setGravity(Gravity.CENTER_VERTICAL);
        header.setPadding(dp(16), dp(10), dp(28), dp(6));
        TextView backButton = Ui.text(a, "‹  Back", 16, Ui.ACCENT, true);
        backButton.setPadding(dp(12), dp(10), dp(16), dp(10));
        backButton.setBackground(Ui.pressable(a, Ui.rounded(a, 0x00000000, 10, 0), 10));
        backButton.setClickable(true);
        backButton.setOnClickListener(v -> back.run());
        header.addView(backButton);
        TextView title = Ui.text(a, "Edit save", 24, Ui.TEXT, true);
        title.setPadding(dp(8), 0, 0, 0);
        header.addView(title, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        TextView save = Ui.primaryButton(a, "Save changes");
        save.setTextSize(15);
        save.setPadding(dp(22), 0, dp(22), 0);
        save.setOnClickListener(v -> saveChanges(back));
        header.addView(save, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, dp(44)));
        root.addView(header);

        LinearLayout content = new LinearLayout(a);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(28), 0, dp(28), dp(28));

        // How it works, and what the numbers shown are.
        LinearLayout info = Ui.card(a);
        info.setPadding(dp(18), dp(14), dp(18), dp(14));
        String when = current.isEmpty()
            ? "Launch the game once to see your current gold and stats here."
            : "\"Now\" is your save as of " + DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT).format(new Date(currentTime)) + ".";
        String how = "Changes are made to your save the next time you press Play. "
            + (gameRunning ? "The game is running now, so saving changes closes it (progress since its last save is lost). " : "")
            + "Back up your saves first if you may want to go back.";
        TextView infoText = Ui.text(a, when + "\n" + how, 13.5f, Ui.SUBTEXT, false);
        infoText.setLineSpacing(0, 1.15f);
        info.addView(infoText);
        if (!pending.isEmpty()) {
            TextView queued = Ui.text(a, "Changes waiting for the next Play: " + describe(pending), 13.5f, Ui.ACCENT, true);
            queued.setPadding(0, dp(8), 0, 0);
            info.addView(queued);
            TextView discard = Ui.text(a, "Discard waiting changes", 13.5f, Ui.TEXT, true);
            discard.setPadding(0, dp(10), 0, dp(2));
            discard.setClickable(true);
            discard.setOnClickListener(v -> {
                new File(dir, "saveedit-pending.ini").delete();
                Toast.makeText(a, "Waiting changes discarded", Toast.LENGTH_SHORT).show();
                back.run();
            });
            info.addView(discard);
        }
        content.addView(space(dp(14)));
        content.addView(info);

        content.addView(Ui.sectionHeader(a, "Character"));
        LinearLayout numbers = Ui.card(a);
        for (String[] f : NUMBERS)
            if (f[3].contains(game)) addRow(numbers, numberRow(f[0], f[1], f[2]));
        content.addView(numbers);

        LinearLayout actions = Ui.card(a);
        for (String[] f : ACTIONS)
            if (f[3].contains(game)) addRow(actions, actionRow(f[0], f[1], f[2]));
        if (actions.getChildCount() > 0) {
            content.addView(Ui.sectionHeader(a, "Items"));
            content.addView(actions);
        }
        if (game.equals("3")) addGemRows(content);

        LinearLayout center = new LinearLayout(a);
        center.setGravity(Gravity.CENTER_HORIZONTAL);
        center.addView(content, new LinearLayout.LayoutParams(Math.min(dp(760), a.getResources().getDisplayMetrics().widthPixels),
                LinearLayout.LayoutParams.WRAP_CONTENT));
        ScrollView scroll = new ScrollView(a);
        scroll.setVerticalScrollBarEnabled(false);
        scroll.addView(center);
        root.addView(scroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));
        return root;
    }

    private View space(int h) {
        View v = new View(a);
        v.setLayoutParams(new LinearLayout.LayoutParams(1, h));
        return v;
    }

    private void addRow(LinearLayout card, View row) {
        if (card.getChildCount() > 0) card.addView(Ui.divider(a));
        card.addView(row);
    }

    private static String number(long n) {
        return NumberFormat.getIntegerInstance(Locale.US).format(n);
    }

    private String describe(Map<String, String> edits) {
        StringBuilder s = new StringBuilder();
        for (Map.Entry<String, String> e : edits.entrySet()) {
            String label = e.getKey();
            for (String[] f : NUMBERS) if (f[0].equals(e.getKey()) && f[3].contains(game)) label = f[1];
            for (String[] f : ACTIONS) if (f[0].equals(e.getKey())) label = f[1];
            if (e.getKey().equals("GemShopHighEnd")) continue;
            if (s.length() > 0) s.append(", ");
            if (e.getKey().equals("RandomGems")) {
                s.append(e.getValue()).append(" random gems");
                continue;
            }
            if (e.getKey().equals("GemShop")) {
                s.append("gem shop: ").append(gemShopLabel(e.getValue()).toLowerCase(Locale.US))
                 .append("1".equals(edits.get("GemShopHighEnd")) ? ", strongest" : "");
                continue;
            }
            boolean action = e.getKey().startsWith("Give");
            s.append(label).append(action ? "" : " " + e.getValue());
        }
        return s.toString();
    }

    private LinearLayout row(String title, String subtitle, View control) {
        LinearLayout row = new LinearLayout(a);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(18), dp(10), dp(16), dp(10));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1);
        lp.rightMargin = dp(16);
        row.addView(Ui.labels(a, title, subtitle), lp);
        row.addView(control);
        return row;
    }

    // --- Limits: what the game can take. The game reports its XP table and its own caps ([Limits]).

    private static final int MAX_LEVEL = 50;  // the last level the games' XP table (NextLevelXPTargets) covers

    private long reportedLong(String key, long fallback) {
        try {
            return Long.parseLong(reported.get(key));
        } catch (Exception e) {
            return fallback;
        }
    }

    // XP that level `level` needs for the next one (the game's NextLevelXPTargets[level]).
    private long xpToNext(long level) {
        String table = reported.get("XPTable");
        if (table == null) return 25000;
        String[] steps = table.split(",");
        try {
            return Long.parseLong(steps[(int) Math.max(0, Math.min(level, steps.length - 1))].trim());
        } catch (NumberFormatException e) {
            return 25000;
        }
    }

    private long min(String key) {
        return key.startsWith("Stat") && !key.equals("StatPoints") || key.equals("Level") || key.equals("Bloodline") ? 1 : 0;
    }

    // `level` is the level the save will have (for the XP limit).
    private long max(String key, long level) {
        long now = current.containsKey(key) ? current.get(key) : 0, limit;
        switch (key) {
            case "Level": limit = MAX_LEVEL; break;
            case "XP":  // a save can hold more (e.g. at level 50), and keeping its level keeps its XP
                limit = Math.max(0, xpToNext(level) - 1);
                return current.containsKey("Level") && current.get("Level") == level ? Math.max(limit, now) : limit;
            case "StatPoints": limit = 999; break;
            case "Bloodline": limit = game.equals("3") ? reportedLong("MaxBloodline", 100) : 99; break;
            case "GemCarry": return reportedLong("MaxGemCarry", 3);
            default:
                if (key.startsWith("Stat")) limit = game.equals("3") ? 100 : 999;  // IB3's own max-out cheat uses 100
                else limit = 999_999_999;  // gold, chips: room for the game to add to them
        }
        return Math.max(limit, now);  // never less than a save already has
    }

    private EditText numberInput() {
        EditText input = new EditText(a);
        input.setInputType(InputType.TYPE_CLASS_NUMBER);
        input.setFilters(new InputFilter[] {new InputFilter.LengthFilter(9)});
        input.setSingleLine(true);
        // Landscape keyboards otherwise cover the page with their own text box.
        input.setImeOptions(EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_FULLSCREEN | EditorInfo.IME_FLAG_NO_EXTRACT_UI);
        input.setSelectAllOnFocus(true);  // typing replaces the number
        input.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
        input.setTextColor(Ui.TEXT);
        input.setHintTextColor(Ui.FAINT);
        input.setTextSize(16);
        input.setBackground(Ui.rounded(a, 0xFF11131A, 10, Ui.CARD_LINE));
        input.setPadding(dp(12), dp(8), dp(12), dp(8));
        return input;
    }

    // --- Gems (IB3). The game reports its kinds of gems (GemKind.<name>=<shown name>|<description>) and the
    // gem bag (Gems, GemBagSize); the actions are its own gem cheats (src/game/saveedit.cpp gems_ib3).

    private static final String SHOP_EVERY_KIND = "*";
    private EditText randomGems;
    private String gemShop;  // null: leave the shop as it is
    private Switch gemShopHighEnd;

    // [template name, label] of each kind, sorted by label.
    private java.util.List<String[]> gemKinds() {
        java.util.List<String[]> kinds = new java.util.ArrayList<>();
        for (Map.Entry<String, String> e : reported.entrySet()) {
            if (!e.getKey().startsWith("GemKind.")) continue;
            String name = e.getKey().substring(8), value = e.getValue();
            int bar = value.indexOf('|');
            String shown = bar < 0 ? value : value.substring(0, bar), desc = bar < 0 ? "" : value.substring(bar + 1).trim();
            String label = Character.toUpperCase(shown.charAt(0)) + shown.substring(1).toLowerCase(Locale.US);
            if (name.endsWith("In")) label += " (indoors)";
            else if (name.endsWith("Out")) label += " (outdoors)";
            if (!desc.isEmpty() && !desc.contains("Get 0")) label += ": " + desc;  // the stat gems say "Get 0 Stat"
            kinds.add(new String[] {name, label});
        }
        kinds.sort((x, y) -> x[1].compareToIgnoreCase(y[1]));
        return kinds;
    }

    private String gemShopLabel(String choice) {
        if (choice == null) return "Leave it as it is";
        if (choice.equals(SHOP_EVERY_KIND)) return "One of every kind of gem";
        for (String[] k : gemKinds()) if (k[0].equals(choice)) return "Only " + k[1];
        return choice;
    }

    private void addGemRows(LinearLayout content) {
        content.addView(Ui.sectionHeader(a, "Gems"));
        LinearLayout card = Ui.card(a);
        Long have = current.get("Gems"), size = current.get("GemBagSize");
        long free = have != null && size != null ? Math.max(0, size - have) : 0;
        randomGems = numberInput();
        randomGems.setHint("0");
        if (pending.containsKey("RandomGems")) randomGems.setText(pending.get("RandomGems"));
        String bag = have != null && size != null ? "Your gem bag holds " + have + " of " + size + ", so up to " + free + " more fit." : "Launch the game once first.";
        addRow(card, row("Add random gems", "New gems like the ones fights give. " + bag, randomGems, dp(170)));

        gemShop = pending.get("GemShop");
        TextView choice = Ui.text(a, gemShopLabel(gemShop) + "  ›", 15, Ui.ACCENT, true);
        LinearLayout shopRow = row("Gem shop", "Fills the shop in Items › Gems › Store with the gems you pick, to buy with gold.", choice);
        shopRow.setClickable(true);
        shopRow.setBackground(Ui.pressable(a, Ui.rounded(a, 0x00000000, 0, 0), 0));
        shopRow.setOnClickListener(v -> {
            java.util.List<String[]> kinds = gemKinds();
            if (kinds.isEmpty()) {
                Toast.makeText(a, "Launch the game once first, so it can list its gems.", Toast.LENGTH_LONG).show();
                return;
            }
            String[] names = new String[kinds.size() + 2], labels = new String[kinds.size() + 2];
            labels[0] = gemShopLabel(null);
            labels[1] = gemShopLabel(SHOP_EVERY_KIND);
            names[1] = SHOP_EVERY_KIND;
            for (int i = 0; i < kinds.size(); i++) {
                names[i + 2] = kinds.get(i)[0];
                labels[i + 2] = kinds.get(i)[1];
            }
            new android.app.AlertDialog.Builder(a, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Gem shop")
                .setItems(labels, (d, which) -> {
                    gemShop = names[which];
                    choice.setText(gemShopLabel(gemShop) + "  ›");
                })
                .show();
        });
        addRow(card, shopRow);

        gemShopHighEnd = new Switch(a);
        gemShopHighEnd.setChecked("1".equals(pending.get("GemShopHighEnd")));
        gemShopHighEnd.setThumbTintList(new ColorStateList(new int[][] {{android.R.attr.state_checked}, {}}, new int[] {Ui.ACCENT, 0xFFB8BCC6}));
        gemShopHighEnd.setTrackTintList(new ColorStateList(new int[][] {{android.R.attr.state_checked}, {}}, new int[] {0x80E2B155, 0xFF3A3F4C}));
        LinearLayout high = row("Strongest gems in the shop", "With a gem shop choice: the most powerful versions, at their price.", gemShopHighEnd);
        high.setClickable(true);
        high.setBackground(Ui.pressable(a, Ui.rounded(a, 0x00000000, 0, 0), 0));
        high.setOnClickListener(v -> gemShopHighEnd.toggle());
        addRow(card, high);
        content.addView(card);
    }

    private View numberRow(String key, String label, String subtitle) {
        Long now = current.get(key);
        long lvl = current.containsKey("Level") ? current.get("Level") : 1;
        String range = number(min(key)) + " to " + number(max(key, lvl));
        String sub = (now != null ? "Now: " + number(now) + ". " : "") + (subtitle.isEmpty() ? "" : subtitle + " ") + range + ".";
        EditText input = numberInput();
        String waiting = pending.get(key);
        if (waiting != null) input.setText(waiting);
        else if (now != null) input.setText(String.valueOf(now));
        else input.setHint("unchanged");
        // A number outside what the game takes is corrected as soon as the player leaves the box.
        input.setOnFocusChangeListener((v, focused) -> {
            Long value = typed(key);
            if (focused || value == null) return;
            Long typedLevel = typed("Level");
            long lo = min(key), hi = max(key, typedLevel != null ? typedLevel : lvl);
            if (value > hi || value < lo) {
                long fixed = Math.max(lo, Math.min(value, hi));
                input.setText(String.valueOf(fixed));
                Toast.makeText(a, label + ": " + (value > hi ? "at most " : "at least ") + number(fixed), Toast.LENGTH_SHORT).show();
            }
        });
        inputs.put(key, input);
        return row(label, sub, input, dp(170));
    }

    private LinearLayout row(String title, String subtitle, View control, int controlWidth) {
        LinearLayout row = row(title, subtitle, control);
        control.setLayoutParams(new LinearLayout.LayoutParams(controlWidth, LinearLayout.LayoutParams.WRAP_CONTENT));
        return row;
    }

    private View actionRow(String key, String label, String subtitle) {
        Switch s = new Switch(a);
        s.setChecked("1".equals(pending.get(key)));
        s.setThumbTintList(new ColorStateList(new int[][] {{android.R.attr.state_checked}, {}}, new int[] {Ui.ACCENT, 0xFFB8BCC6}));
        s.setTrackTintList(new ColorStateList(new int[][] {{android.R.attr.state_checked}, {}}, new int[] {0x80E2B155, 0xFF3A3F4C}));
        switches.put(key, s);
        LinearLayout row = row(label, subtitle, s);
        row.setClickable(true);
        row.setBackground(Ui.pressable(a, Ui.rounded(a, 0x00000000, 0, 0), 0));
        row.setOnClickListener(v -> s.toggle());
        return row;
    }

    // Only what the player changed goes into the file: numbers that differ from the save, and actions.
    private Long typed(String key) {
        EditText input = inputs.get(key);
        String text = input == null ? "" : input.getText().toString().trim();
        try {
            return text.isEmpty() ? null : Long.parseLong(text);
        } catch (NumberFormatException e) {
            return null;
        }
    }

    private String labelOf(String key) {
        for (String[] f : NUMBERS) if (f[0].equals(key) && f[3].contains(game)) return f[1];
        return key;
    }

    private void saveChanges(Runnable back) {
        Map<String, String> edits = new LinkedHashMap<>();
        StringBuilder fixed = new StringBuilder();  // values moved into range, to tell the player
        Long newLevel = typed("Level");
        long level = newLevel != null ? Math.max(1, Math.min(newLevel, max("Level", 0))) : current.containsKey("Level") ? current.get("Level") : 1;
        boolean levelChanged = newLevel != null && !newLevel.equals(current.get("Level"));
        for (String key : inputs.keySet()) {
            Long value = typed(key);
            if (value == null) continue;
            Long now = current.get(key);
            // A new level starts with 0 XP (the game does that); XP typed along with it is kept if it fits.
            if (key.equals("XP") && levelChanged && value.equals(now)) continue;
            long lo = min(key), hi = max(key, level);
            long clamped = Math.max(lo, Math.min(value, hi));
            if (clamped != value) {
                fixed.append("\n• ").append(labelOf(key)).append(": ").append(number(clamped))
                     .append(clamped == hi ? " (the most the game allows)" : " (the least the game allows)");
                inputs.get(key).setText(String.valueOf(clamped));
            }
            if (now == null || now != clamped || pending.containsKey(key)) edits.put(key, String.valueOf(clamped));
        }
        for (Map.Entry<String, Switch> e : switches.entrySet())
            if (e.getValue().isChecked()) edits.put(e.getKey(), "1");
        if (randomGems != null) {
            long want = 0;
            try {
                want = Long.parseLong(randomGems.getText().toString().trim());
            } catch (NumberFormatException ignored) {
            }
            Long have = current.get("Gems"), size = current.get("GemBagSize");
            long free = have != null && size != null ? Math.max(0, size - have) : want;
            if (want > free) {
                fixed.append("\n• Random gems: ").append(free).append(" (what fits in your gem bag)");
                randomGems.setText(String.valueOf(free));
                want = free;
            }
            if (want > 0) edits.put("RandomGems", String.valueOf(want));
        }
        if (gemShop != null) {
            edits.put("GemShop", gemShop);
            if (gemShopHighEnd.isChecked()) edits.put("GemShopHighEnd", "1");
        }
        File file = new File(dir, "saveedit-pending.ini");
        if (edits.isEmpty()) {
            file.delete();
            if (fixed.length() > 0) {
                new android.app.AlertDialog.Builder(a, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                    .setTitle("Nothing to change")
                    .setMessage("These were outside what the game can take, so they were set to:" + fixed
                                + "\n\nThat is what your save has already.")
                    .setPositiveButton("OK", null)
                    .show();
            } else {
                Toast.makeText(a, "Nothing changed", Toast.LENGTH_SHORT).show();
            }
            return;
        }
        StringBuilder s = new StringBuilder("; Written by the launcher's Edit save page; the game applies it the next time it loads the save.\n[Edits]\n");
        for (Map.Entry<String, String> e : edits.entrySet()) s.append(e.getKey()).append('=').append(e.getValue()).append('\n');
        try (OutputStream out = new FileOutputStream(file)) {
            out.write(s.toString().getBytes(StandardCharsets.UTF_8));
        } catch (IOException ex) {
            Toast.makeText(a, "Could not save the changes: " + ex.getMessage(), Toast.LENGTH_LONG).show();
            return;
        }
        if (fixed.length() > 0) {
            new android.app.AlertDialog.Builder(a, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Saved, with some values changed")
                .setMessage("These were outside what the game can take, so they were set to:" + fixed
                            + "\n\nThe changes are made when you press Play.")
                .setPositiveButton("OK", (d, w) -> done(back))
                .setCancelable(false)
                .show();
            return;
        }
        Toast.makeText(a, "Saved. The changes are made when you press Play.", Toast.LENGTH_LONG).show();
        done(back);
    }

    // The game runs in this process and loads the save only when it starts: a running game would never
    // read the changes, and would save over them. So it is closed, and the next Play starts it afresh.
    private void done(Runnable back) {
        if (!gameRunning) {
            back.run();
            return;
        }
        new android.app.AlertDialog.Builder(a, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setMessage("Changes saved. The game is still running and only reads your save when it starts, so it will close now. Open it again and press Play.")
            .setCancelable(false)
            .setPositiveButton("OK", (d, w) -> {
                a.finishAffinity();
                android.os.Process.killProcess(android.os.Process.myPid());
            })
            .show();
    }
}
