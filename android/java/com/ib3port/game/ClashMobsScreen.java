package com.ib3port.game;

import android.app.Activity;
import android.content.Intent;
import android.content.res.ColorStateList;
import android.net.Uri;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.IOException;
import java.util.Locale;
import java.util.function.Consumer;

/**
 * IB3's ClashMobs page, from the launcher's menu: the player's ClashMob name, online ClashMobs on or off, and the
 * events running on the community server now (from its /status), with a link to its web page.
 */
final class ClashMobsScreen {
    private final Activity a;
    private final IniFile ini;
    private LinearLayout live;  // the "Live now" card, filled when the server answers

    private ClashMobsScreen(Activity a, File iniFile) {
        this.a = a;
        this.ini = new IniFile(iniFile);
    }

    static View build(Activity a, File iniFile, Runnable back) {
        return new ClashMobsScreen(a, iniFile).build(back);
    }

    private int dp(float v) {
        return Ui.dp(a, v);
    }

    private View build(Runnable back) {
        LinearLayout root = new LinearLayout(a);
        root.setOrientation(LinearLayout.VERTICAL);
        Ui.screenBackground(root);
        root.addView(Ui.pageHeader(a, "ClashMobs", null, null, back));

        LinearLayout content = new LinearLayout(a);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(28), dp(8), dp(28), dp(28));

        content.addView(Ui.sectionHeader(a, "You"));
        LinearLayout you = Ui.card(a);
        addRow(you, nameRow());
        addRow(you, onlineRow());
        content.addView(you);

        content.addView(Ui.sectionHeader(a, "Live now"));
        live = Ui.card(a);
        content.addView(live);
        TextView how = Ui.text(a, "Play them from the world map in the game: their markers show where they are.", 12.5f, Ui.SUBTEXT, false);
        how.setPadding(dp(6), dp(10), 0, 0);
        content.addView(how);
        loadEvents();

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

    private void addRow(LinearLayout card, View row) {
        if (card.getChildCount() > 0) card.addView(Ui.divider(a));
        card.addView(row);
    }

    private void store(String section, String key, String value) {
        ini.set(section, key, value);
        try {
            ini.save();
        } catch (IOException e) {
            Toast.makeText(a, e.getMessage(), Toast.LENGTH_SHORT).show();
        }
    }

    // Labels on the left, the control on the right.
    private LinearLayout row(String title, String subtitle, View control) {
        LinearLayout row = new LinearLayout(a);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(18), dp(12), dp(16), dp(12));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1);
        lp.rightMargin = dp(16);
        row.addView(Ui.labels(a, title, subtitle), lp);
        if (control != null) row.addView(control);
        return row;
    }

    // The player's name on the ClashMob leaderboards: kept by the server ([ClashMob] Name only remembers it here).
    private View nameRow() {
        TextView value = Ui.text(a, "", 14, Ui.TEXT, true);
        value.setPadding(dp(14), dp(8), dp(14), dp(8));
        value.setBackground(Ui.pressable(a, Ui.rounded(a, 0xFF11131A, 10, Ui.CARD_LINE), 10));
        value.setClickable(true);
        Consumer<String> show = name -> {
            value.setText((name.isEmpty() ? "Set a name" : name) + "  ✎");
            if (!name.equals(ini.get("ClashMob", "Name", ""))) store("ClashMob", "Name", name);
        };
        show.accept(ini.get("ClashMob", "Name", ""));
        ClashMobName.refresh(a, ini, show);  // (an admin may have changed it)
        value.setOnClickListener(v -> ClashMobName.edit(a, ini, ini.get("ClashMob", "Name", ""), show));
        return row("Your name", "How you show up on the ClashMob leaderboards. No two players can have the same name.", value);
    }

    // Online ClashMobs: the community server's events; off, the game plays its offline ClashMobs ([ClashMob] Server=off).
    private View onlineRow() {
        Switch s = new Switch(a);
        s.setChecked(ClashMobName.server(ini) != null);
        ColorStateList thumb = new ColorStateList(new int[][] {{android.R.attr.state_checked}, {}}, new int[] {Ui.ACCENT, 0xFFB8BCC6});
        ColorStateList track = new ColorStateList(new int[][] {{android.R.attr.state_checked}, {}}, new int[] {0x80E2B155, 0xFF3A3F4C});
        s.setThumbTintList(thumb);
        s.setTrackTintList(track);
        s.setOnCheckedChangeListener((v, on) -> {
            store("ClashMob", "Server", on ? "" : "off");
            loadEvents();
        });
        LinearLayout row = row("Online ClashMobs", "Play the community's live events with everyone. Off: the offline ClashMobs, on your own.", s);
        row.setClickable(true);
        row.setBackground(Ui.pressable(a, Ui.rounded(a, 0x00000000, 0, 0), 0));
        row.setOnClickListener(v -> s.toggle());
        return row;
    }

    // ---- Live now ----

    private void message(String title, String subtitle) {
        live.removeAllViews();
        live.addView(row(title, subtitle, null));
    }

    private void loadEvents() {
        String server = ClashMobName.server(ini);
        if (server == null) {
            message("Online ClashMobs are off", "Turn them on above to play the community's events.");
            return;
        }
        message("Loading…", null);
        new Thread(() -> {
            String[] r = ClashMobName.ask("GET", server + "/status", "", null);
            a.runOnUiThread(() -> {
                if (r == null || !r[0].equals("200")) {
                    message("Could not reach the ClashMob server", "Check your internet connection. The offline ClashMobs still work.");
                    return;
                }
                try {
                    showEvents(server, new JSONObject(r[1]));
                } catch (Exception e) {
                    message("Could not read the events", e.getMessage());
                }
            });
        }).start();
    }

    private void showEvents(String server, JSONObject d) throws Exception {
        live.removeAllViews();
        long now = d.getLong("now");
        JSONArray events = d.getJSONArray("events");
        for (String wanted : new String[] {"live", "upcoming"}) {
            for (int i = 0; i < events.length(); i++) {
                JSONObject e = events.getJSONObject(i);
                if (e.getString("status").equals(wanted)) addRow(live, eventRow(e, now));
            }
        }
        if (live.getChildCount() == 0) addRow(live, row("No events right now", "New ones are posted from time to time.", null));
        addRow(live, Ui.actionRow(a, "Leaderboards and all events", "Opens the ClashMobs page in your browser", v -> {
            try {
                a.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(server + "/")));
            } catch (Exception ex) {
                Toast.makeText(a, "No browser to open it with.", Toast.LENGTH_SHORT).show();
            }
        }));
    }

    private View eventRow(JSONObject e, long now) throws Exception {
        String kind = e.getString("kind"), status = e.getString("status");
        String kindName = kind.equals("Tournament") ? "Aegis Tournament" : kind;
        JSONObject cur = e;
        StringBuilder sub = new StringBuilder(kindName);
        if (e.has("stages")) {
            JSONArray stages = e.getJSONArray("stages");
            int active = e.getInt("activeStage");
            cur = stages.getJSONObject(active);
            sub.append(" · stage ").append(active + 1).append(" of ").append(stages.length());
        }
        if (status.equals("upcoming")) sub.append(" · starts in ").append(span(e.getLong("start") - now));
        else sub.append(" · ").append(span(e.getLong("end") - now)).append(" left");
        if (kind.equals("ClashMob") && status.equals("live"))
            sub.append(" · mob ").append(num(Math.min(cur.getLong("total"), cur.getLong("goal")))).append(" of ").append(num(cur.getLong("goal")));
        JSONArray rewards = cur.getJSONArray("rewards");
        if (rewards.length() > 0) {
            String prize = prize(rewards.getJSONObject(kind.equals("Trial") ? rewards.length() - 1 : 0));
            if (!prize.isEmpty()) sub.append(kind.equals("Trial") ? " · top prize: " : " · prize: ").append(prize);
        }
        String title = e.optString("title", e.getString("name"));
        return row(title.isEmpty() ? e.getString("name") : title, sub.toString(), null);
    }

    private static String span(long secs) {
        secs = Math.max(0, secs);
        long d = secs / 86400, h = secs % 86400 / 3600, m = secs % 3600 / 60;
        return d > 0 ? d + "d " + h + "h" : h > 0 ? h + "h " + m + "m" : m + "m";
    }

    private static String num(long n) {
        return String.format(Locale.US, "%,d", n);
    }

    private static String prize(JSONObject r) {
        String type = r.optString("type"), data = r.optString("data");
        if (type.equals("TRA_Random_Gold")) {
            String[] p = data.split("\\.");
            long n = 0;
            try {
                n = Long.parseLong(p[1]);
            } catch (RuntimeException ignored) {
            }
            return num(n) + (p[0].equalsIgnoreCase("CHIPS") ? " battle chips" : " gold");
        }
        switch (type) {
            case "TRA_GrabBag_Uber": return "ClashMob prize wheel";
            case "TRA_GrabBag_LargeGem": return "rare gem wheel";
            case "TRA_GrabBag_MediumGem": case "TRA_GrabBag_SmallGem": return "gem wheel";
            case "TRA_GrabBag_Large": case "TRA_GrabBag_Medium": case "TRA_GrabBag_Small": return "prize wheel";
            case "TRA_Gold_Large": case "TRA_Gold_Medium": case "TRA_Gold_Small": return "gold";
            case "TRA_Chips_Large": return "15 battle chips";
            case "TRA_Chips_Medium": return "10 battle chips";
            case "TRA_Chips_Small": return "5 battle chips";
            case "TRA_Item_Fixed":
                switch (data) {
                    case "Sword_222": return "Anarchax";
                    case "Magic_149": return "Squee";
                    case "Helmet_134": return "Radian";
                    case "Helmet_150": return "Serk";
                    case "Helmet_111": return "Colossal Helm";
                    default: return "an item";
                }
            case "TRA_Gem_Fixed": return "a gem";
            default: return "";
        }
    }
}
