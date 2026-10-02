package com.ib3port.game;

import android.app.Activity;
import android.content.res.ColorStateList;
import android.text.Editable;
import android.text.TextWatcher;
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
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * The Cheats page. Switches marked ALWAYS ON are kept in settings.ini [Cheats] and saved at once; the game
 * applies them every time it runs (src/game/devmode.cpp), and the in-game CHEATS rows change the same keys.
 * Things marked NEXT PLAY wait in saveedit-pending.ini (Pending) until the next Play (src/game/saveedit.cpp).
 */
final class CheatsScreen {
    // saveedit-pending.ini keys this page owns.
    private static final List<String> OWNED = Arrays.asList("GiveAllItems", "GiveAllPerks", "GemShop", "GemShopHighEnd", "RandomGems");

    private enum When { ALWAYS, NEXT, IN_GAME }

    private final Activity a;
    private final File dir;
    private final boolean ib2;
    private final IniFile settings;
    private final Map<String, String> pending;
    private final Map<String, Switch> nextPlay = new LinkedHashMap<>();
    private final List<Object[]> searchable = new ArrayList<>();  // {row view, divider before it or null, card, header, text}

    private CheatsScreen(Activity a, File dir, boolean ib2) {
        this.a = a;
        this.dir = dir;
        this.ib2 = ib2;
        this.settings = new IniFile(new File(dir, "settings.ini"));
        this.pending = Pending.read(Pending.file(dir));
    }

    static View build(Activity a, File dir, boolean ib2, Runnable back) {
        return new CheatsScreen(a, dir, ib2).build(back);
    }

    private int dp(float v) {
        return Ui.dp(a, v);
    }

    private String storePath() {
        return ib2 ? "Menu › Character › Items › Store › Supplies" : "Menu › Items › gem tab › Store";
    }

    private View build(Runnable back) {
        LinearLayout root = new LinearLayout(a);
        root.setOrientation(LinearLayout.VERTICAL);
        Ui.screenBackground(root);
        root.addView(Ui.pageHeader(a, "Cheats", "Save changes", v -> saveChanges(back), back));

        LinearLayout content = new LinearLayout(a);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(28), 0, dp(28), dp(28));

        LinearLayout info = Ui.card(a);
        info.setPadding(dp(18), dp(14), dp(18), dp(14));
        TextView how = Ui.text(a, "ALWAYS ON switches work every time you play, until you turn them off. NEXT PLAY changes happen "
                + "the next time you press Play (tap Save changes). With Developer mode on, these switches and the game's developer "
                + "options are also at the top of the game's Options.", 13.5f, Ui.SUBTEXT, false);
        how.setLineSpacing(0, 1.15f);
        info.addView(how);
        String waiting = describe();
        if (!waiting.isEmpty()) {
            TextView queued = Ui.text(a, "Waiting for the next Play: " + waiting, 13.5f, Ui.ACCENT, true);
            queued.setPadding(0, dp(8), 0, 0);
            info.addView(queued);
        }
        content.addView(space(dp(14)));
        content.addView(info);

        EditText search = new EditText(a);
        search.setHint("Search cheats, e.g. gem");
        search.setSingleLine(true);
        search.setImeOptions(EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_FULLSCREEN | EditorInfo.IME_FLAG_NO_EXTRACT_UI);
        search.setTextColor(Ui.TEXT);
        search.setHintTextColor(Ui.FAINT);
        search.setTextSize(15);
        search.setBackground(Ui.rounded(a, 0xFF11131A, 12, Ui.CARD_LINE));
        search.setPadding(dp(14), dp(10), dp(14), dp(10));
        LinearLayout.LayoutParams sp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        sp.topMargin = dp(14);
        content.addView(search, sp);
        search.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int st, int c, int af) {}
            @Override public void onTextChanged(CharSequence s, int st, int b, int c) {}
            @Override public void afterTextChanged(Editable s) { filter(s.toString()); }
        });

        // Developer mode first: it is what people look for.
        TextView devHeader = Ui.sectionHeader(a, "Developer mode");
        LinearLayout dev = Ui.card(a);
        content.addView(devHeader);
        content.addView(dev);
        add(dev, devHeader, alwaysSwitch("InGame", "Developer mode",
                "Adds a cheat menu to the game's Options: god mode, kill boss, give gold, get every item, gem shop refills, "
                + "reload checkpoint, next bloodline and more, ready mid-fight.",
                "In the game: Menu › gear › Options, at the top", When.IN_GAME));

        // Items
        TextView itemsHeader = Ui.sectionHeader(a, "Items");
        LinearLayout items = Ui.card(a);
        content.addView(itemsHeader);
        content.addView(items);
        add(items, itemsHeader, nextPlaySwitch("GiveAllItems", "Get every item",
                "Every weapon, shield, armor, helmet and magic ring you don't have yet.", "Your inventory"));
        if (!ib2)
            add(items, itemsHeader, nextPlaySwitch("GiveAllPerks", "Get all perks",
                    "The developers' cheat: it also makes your character level 50 with every stat at 100.", "Menu › Skills"));

        // Gems
        TextView gemsHeader = Ui.sectionHeader(a, "Gems");
        LinearLayout gems = Ui.card(a);
        content.addView(gemsHeader);
        content.addView(gems);
        add(gems, gemsHeader, alwaysSwitch("AllGems", "All gems", "The gem shop sells every gem in the game, each at its highest level (the strongest version). Off: the game's normal shop.",
                storePath(), When.ALWAYS));
        add(gems, gemsHeader, alwaysSwitch("GemShopRestock", "Restock after buying", "A gem you buy goes back on the shelf, so the shop stays full.",
                storePath(), When.ALWAYS));

        // While playing
        TextView playHeader = Ui.sectionHeader(a, "While playing");
        LinearLayout play = Ui.card(a);
        content.addView(playHeader);
        content.addView(play);
        add(play, playHeader, alwaysSwitch("GodMode", "God mode", "You take no damage.", null, When.ALWAYS));
        add(play, playHeader, alwaysSwitch("UnlimitedSuper", "Unlimited super and magic", "Your super move and magic are always ready.", null, When.ALWAYS));
        add(play, playHeader, alwaysSwitch("FastForward", "Always fast forward", "The game always runs fast-forwarded.", null, When.ALWAYS));
        if (!ib2)
            add(play, playHeader, alwaysSwitch("FastWheel", "Fast prize wheel", "Once you spin a prize wheel it lands at once, with the same prize.",
                    "Menu › Supplies › a grab bag", When.ALWAYS));

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

    private void add(LinearLayout card, TextView header, View row) {
        View divider = null;
        if (card.getChildCount() > 0) {
            divider = Ui.divider(a);
            card.addView(divider);
        }
        card.addView(row);
        searchable.add(new Object[] {row, divider, card, header, String.valueOf(row.getTag()).toLowerCase(Locale.US)});
    }

    // Shows only the rows whose words contain `query`, and only the sections that still have rows.
    private void filter(String query) {
        String q = query.trim().toLowerCase(Locale.US);
        Map<LinearLayout, Integer> shown = new LinkedHashMap<>();
        Map<LinearLayout, TextView> headers = new LinkedHashMap<>();
        for (Object[] r : searchable) {
            boolean show = q.isEmpty() || ((String) r[4]).contains(q);
            LinearLayout card = (LinearLayout) r[2];
            headers.put(card, (TextView) r[3]);
            int before = shown.getOrDefault(card, 0);
            ((View) r[0]).setVisibility(show ? View.VISIBLE : View.GONE);
            if (r[1] != null) ((View) r[1]).setVisibility(show && before > 0 ? View.VISIBLE : View.GONE);
            shown.put(card, before + (show ? 1 : 0));
        }
        for (Map.Entry<LinearLayout, Integer> e : shown.entrySet()) {
            int vis = e.getValue() > 0 ? View.VISIBLE : View.GONE;
            e.getKey().setVisibility(vis);
            headers.get(e.getKey()).setVisibility(vis);
        }
    }

    // A row: title, a tag saying when it takes effect, the line under it, and where it shows in the game.
    private LinearLayout row(String title, When when, String subtitle, String where, View control) {
        LinearLayout row = new LinearLayout(a);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(18), dp(11), dp(16), dp(11));
        LinearLayout text = new LinearLayout(a);
        text.setOrientation(LinearLayout.VERTICAL);
        LinearLayout top = new LinearLayout(a);
        top.setGravity(Gravity.CENTER_VERTICAL);
        top.addView(Ui.text(a, title, 16, Ui.TEXT, true));
        TextView tag = tag(when);
        LinearLayout.LayoutParams tp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        tp.leftMargin = dp(10);
        top.addView(tag, tp);
        text.addView(top);
        TextView sub = Ui.text(a, subtitle, 13, Ui.SUBTEXT, false);
        sub.setPadding(0, dp(3), 0, 0);
        text.addView(sub);
        if (where != null) {
            TextView w = Ui.text(a, "→ " + where, 12, Ui.FAINT, false);
            w.setPadding(0, dp(2), 0, 0);
            text.addView(w);
        }
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1);
        lp.rightMargin = dp(16);
        row.addView(text, lp);
        row.addView(control);
        row.setTag(title + " " + subtitle + " " + (where == null ? "" : where));
        return row;
    }

    private TextView tag(When when) {
        String label = when == When.ALWAYS ? "ALWAYS ON" : when == When.NEXT ? "NEXT PLAY" : "IN GAME";
        int fg = when == When.ALWAYS ? 0xFF9FD3A8 : when == When.NEXT ? Ui.ACCENT : 0xFF9CC2EC;
        int bg = when == When.ALWAYS ? 0x29509F64 : when == When.NEXT ? 0x24E2B155 : 0x295A8CD2;
        TextView t = Ui.text(a, label, 11, fg, true);
        t.setBackground(Ui.rounded(a, bg, 6, 0));
        t.setPadding(dp(7), dp(2), dp(7), dp(2));
        return t;
    }

    private Switch newSwitch(boolean on) {
        Switch s = new Switch(a);
        s.setChecked(on);
        s.setThumbTintList(new ColorStateList(new int[][] {{android.R.attr.state_checked}, {}}, new int[] {Ui.ACCENT, 0xFFB8BCC6}));
        s.setTrackTintList(new ColorStateList(new int[][] {{android.R.attr.state_checked}, {}}, new int[] {0x80E2B155, 0xFF3A3F4C}));
        return s;
    }

    private LinearLayout clickable(LinearLayout row, Switch s) {
        row.setClickable(true);
        row.setBackground(Ui.pressable(a, Ui.rounded(a, 0x00000000, 0, 0), 0));
        row.setOnClickListener(v -> s.toggle());
        return row;
    }

    // An ALWAYS ON (or IN GAME) switch: settings.ini [Cheats] <key>, saved as soon as it changes.
    private View alwaysSwitch(String key, String title, String subtitle, String where, When when) {
        Switch s = newSwitch(settings.get("Cheats", key, legacy(key)) != 0);
        s.setOnCheckedChangeListener((v, on) -> {
            settings.set("Cheats", key, on ? 1 : 0);
            try {
                settings.save();
            } catch (IOException e) {
                Toast.makeText(a, e.getMessage(), Toast.LENGTH_SHORT).show();
            }
        });
        return clickable(row(title, when, subtitle, where, s), s);
    }

    // The [Game] keys of the 1.6 / 1.7 tests, read when [Cheats] has no value yet.
    private int legacy(String key) {
        if (key.equals("AllGems")) return settings.get("Cheats", "GemShop", "").isEmpty() ? 0 : 1;  // the 1.7 tests' gem shop choice
        String old = key.equals("InGame") ? "DeveloperMode" : key;
        return key.equals("InGame") || key.equals("FastWheel") || key.equals("GemShopRestock") ? settings.get("Game", old, 0) : 0;
    }

    private View nextPlaySwitch(String key, String title, String subtitle, String where) {
        Switch s = newSwitch("1".equals(pending.get(key)));
        nextPlay.put(key, s);
        return clickable(row(title, When.NEXT, subtitle, where, s), s);
    }

    private String describe() {
        List<String> parts = new ArrayList<>();
        if ("1".equals(pending.get("GiveAllItems"))) parts.add("get every item");
        if ("1".equals(pending.get("GiveAllPerks"))) parts.add("get all perks");
        return String.join(", ", parts);
    }

    private void saveChanges(Runnable back) {
        Map<String, String> edits = new LinkedHashMap<>();
        for (Map.Entry<String, Switch> e : nextPlay.entrySet())
            if (e.getValue().isChecked()) edits.put(e.getKey(), "1");
        try {
            Pending.write(dir, OWNED, edits);
        } catch (IOException e) {
            Toast.makeText(a, "Could not save: " + e.getMessage(), Toast.LENGTH_LONG).show();
            return;
        }
        Toast.makeText(a, edits.isEmpty() ? "Saved. Nothing waiting for the next Play." : "Saved. These happen when you press Play.",
                Toast.LENGTH_LONG).show();
        back.run();
    }
}
