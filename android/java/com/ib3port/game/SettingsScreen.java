package com.ib3port.game;

import android.app.Activity;
import android.content.res.ColorStateList;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.IOException;

/**
 * The settings page: writes settings.ini (src/settings.cpp reads it when the game starts). The two apps
 * show only what their game uses: IB3 takes all of its renderer's options; IB2 only the shadow
 * resolution and frame rate (src/game/config.cpp), plus its full-screen layout.
 */
final class SettingsScreen {
    private final Activity a;
    private final IniFile ini;
    private final boolean ib2;

    private SettingsScreen(Activity a, File iniFile, boolean ib2) {
        this.a = a;
        this.ini = new IniFile(iniFile);
        this.ib2 = ib2;
    }

    static View build(Activity a, File iniFile, boolean ib2, boolean gameRunning, Runnable back) {
        return new SettingsScreen(a, iniFile, ib2).build(gameRunning, back);
    }

    private int dp(float v) {
        return Ui.dp(a, v);
    }

    private View build(boolean gameRunning, Runnable back) {
        LinearLayout root = new LinearLayout(a);
        root.setOrientation(LinearLayout.VERTICAL);
        Ui.screenBackground(root);

        // Header: back, title, and when the changes take effect.
        LinearLayout header = new LinearLayout(a);
        header.setGravity(Gravity.CENTER_VERTICAL);
        header.setPadding(dp(16), dp(10), dp(28), dp(6));
        TextView backButton = Ui.text(a, "‹  Back", 16, Ui.ACCENT, true);
        backButton.setPadding(dp(12), dp(10), dp(16), dp(10));
        backButton.setBackground(Ui.pressable(a, Ui.rounded(a, 0x00000000, 10, 0), 10));
        backButton.setClickable(true);
        backButton.setOnClickListener(v -> back.run());
        header.addView(backButton);
        TextView title = Ui.text(a, "Settings", 24, Ui.TEXT, true);
        title.setPadding(dp(8), 0, 0, 0);
        header.addView(title, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        TextView note = Ui.text(a, gameRunning ? "The game is running: close it and start it again to apply changes."
                                               : "Changes apply when you press Play.", 12.5f, Ui.SUBTEXT, false);
        note.setGravity(Gravity.END);
        header.addView(note);
        root.addView(header);

        LinearLayout content = new LinearLayout(a);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(28), 0, dp(28), dp(28));

        content.addView(Ui.sectionHeader(a, "Display"));
        LinearLayout display = Ui.card(a);
        if (ib2) {
            addRow(display, choice("Screen", "Full screen fits the game to your phone. 16:9 keeps the original shape, with black bars at the sides.",
                    "Display", "Widescreen", 1, new int[] {1, 0}, new String[] {"Full screen", "16:9"}));
        }
        addRow(display, choice("Frame rate limit", "120 is experimental: it needs a 120 Hz screen and uses more battery.",
                "Display", "MaxFPS", 60, new int[] {30, 60, 120}, new String[] {"30", "60", "120"}));
        if (!ib2) {
            addRow(display, choice("Resolution", "Lower is smoother on weaker phones.",
                    "Display", "RenderResolution", 1080, new int[] {720, 1080, 1440}, new String[] {"720p", "1080p", "1440p"}));
        }
        addRow(display, toggle("Show FPS", "A frames-per-second counter in the corner.", "Display", "ShowFPS", 0));
        content.addView(display);

        content.addView(Ui.sectionHeader(a, "Graphics"));
        LinearLayout graphics = Ui.card(a);
        if (!ib2) {
            addRow(graphics, choice("Anti-aliasing", "Smooths jagged edges. MSAA looks best and costs the most.",
                    "Graphics", "AntiAliasing", 1, new int[] {0, 1, 2}, new String[] {"Off", "FXAA", "MSAA 4x"}));
            addRow(graphics, toggle("Shadows", "Characters cast shadows.", "Graphics", "DynamicShadows", 1));
        }
        addRow(graphics, toggle("Sharper shadows", "Higher-resolution character shadows.", "Graphics", "HighResShadows", 0));
        if (!ib2) {
            addRow(graphics, toggle("Light shafts", "Beams of light through windows and trees.", "Graphics", "LightShafts", 1));
            addRow(graphics, toggle("Bloom", "Glow around bright light.", "Graphics", "Bloom", 1));
            addRow(graphics, toggle("Depth of field", "Blurs the background in cutscenes and menus.", "Graphics", "DepthOfField", 1));
            addRow(graphics, choice("Texture filtering", "Sharper textures at an angle.",
                    "Graphics", "Anisotropy", 4, new int[] {1, 4, 8, 16}, new String[] {"Off", "4x", "8x", "16x"}));
        }
        content.addView(graphics);

        content.addView(Ui.sectionHeader(a, "Sound"));
        LinearLayout sound = Ui.card(a);
        addRow(sound, slider("Music", "Audio", "MusicVolume", 100, 0, 100));
        addRow(sound, slider("Effects", "Audio", "EffectsVolume", 100, 0, 100));
        content.addView(sound);

        content.addView(Ui.sectionHeader(a, "Controller"));
        LinearLayout pad = Ui.card(a);
        addRow(pad, toggle("Game controllers", "Built-in handheld controls and Bluetooth/USB pads.", "Controller", "Enabled", 1));
        addRow(pad, slider("Cursor speed", "Controller", "CursorSpeed", 100, 20, 400));
        addRow(pad, slider("Camera speed", "Controller", "CameraSpeed", 100, 20, 400));
        addRow(pad, slider("Swipe length", "Controller", "SwipeSize", 100, 30, 300));
        content.addView(pad);

        // Centered, and no wider than reads well on a tablet.
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

    private void store(String section, String key, int value) {
        ini.set(section, key, value);
        try {
            ini.save();
        } catch (IOException e) {
            Toast.makeText(a, e.getMessage(), Toast.LENGTH_SHORT).show();
        }
    }

    // A row: labels on the left, the control on the right (or under them when there is no room).
    private LinearLayout row(String title, String subtitle, View control) {
        LinearLayout row = new LinearLayout(a);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(18), dp(12), dp(16), dp(12));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1);
        lp.rightMargin = dp(16);
        row.addView(Ui.labels(a, title, subtitle), lp);
        row.addView(control);
        return row;
    }

    private View choice(String title, String subtitle, String section, String key, int def, int[] values, String[] labels) {
        LinearLayout group = new LinearLayout(a);
        group.setBackground(Ui.rounded(a, 0xFF11131A, 10, Ui.CARD_LINE));
        group.setPadding(dp(3), dp(3), dp(3), dp(3));
        TextView[] pills = new TextView[values.length];
        Runnable[] refresh = new Runnable[1];
        int current = ini.get(section, key, def);
        boolean known = false;
        for (int v : values) known |= v == current;
        final int[] selected = {known ? current : def};
        for (int i = 0; i < values.length; i++) {
            final int value = values[i];
            TextView pill = Ui.text(a, labels[i], 14, Ui.SUBTEXT, true);
            pill.setGravity(Gravity.CENTER);
            pill.setPadding(dp(14), dp(7), dp(14), dp(7));
            pill.setClickable(true);
            pill.setOnClickListener(v -> {
                selected[0] = value;
                store(section, key, value);
                refresh[0].run();
            });
            pills[i] = pill;
            group.addView(pill);
        }
        refresh[0] = () -> {
            for (int i = 0; i < values.length; i++) {
                boolean on = values[i] == selected[0];
                pills[i].setTextColor(on ? Ui.ON_ACCENT : Ui.SUBTEXT);
                pills[i].setBackground(on ? Ui.rounded(a, Ui.ACCENT, 8, 0) : Ui.pressable(a, Ui.rounded(a, 0x00000000, 8, 0), 8));
            }
        };
        refresh[0].run();
        return row(title, subtitle, group);
    }

    private View toggle(String title, String subtitle, String section, String key, int def) {
        Switch s = new Switch(a);
        s.setChecked(ini.get(section, key, def) != 0);
        ColorStateList thumb = new ColorStateList(new int[][] {{android.R.attr.state_checked}, {}}, new int[] {Ui.ACCENT, 0xFFB8BCC6});
        ColorStateList track = new ColorStateList(new int[][] {{android.R.attr.state_checked}, {}}, new int[] {0x80E2B155, 0xFF3A3F4C});
        s.setThumbTintList(thumb);
        s.setTrackTintList(track);
        s.setOnCheckedChangeListener((v, on) -> store(section, key, on ? 1 : 0));
        LinearLayout row = row(title, subtitle, s);
        row.setClickable(true);
        row.setBackground(Ui.pressable(a, Ui.rounded(a, 0x00000000, 0, 0), 0));
        row.setOnClickListener(v -> s.toggle());
        return row;
    }

    private View slider(String title, String section, String key, int def, int min, int max) {
        int value = Math.max(min, Math.min(max, ini.get(section, key, def)));
        LinearLayout box = new LinearLayout(a);
        box.setGravity(Gravity.CENTER_VERTICAL);
        TextView label = Ui.text(a, value + "%", 14, Ui.TEXT, true);
        label.setGravity(Gravity.END);
        SeekBar bar = new SeekBar(a);
        bar.setMax(max - min);
        bar.setProgress(value - min);
        bar.setThumbTintList(ColorStateList.valueOf(Ui.ACCENT));
        bar.setProgressTintList(ColorStateList.valueOf(Ui.ACCENT));
        bar.setProgressBackgroundTintList(ColorStateList.valueOf(0xFF3A3F4C));
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s, int p, boolean user) {
                label.setText((p + min) + "%");
            }
            @Override public void onStartTrackingTouch(SeekBar s) {}
            @Override public void onStopTrackingTouch(SeekBar s) {
                store(section, key, s.getProgress() + min);
            }
        });
        box.addView(bar, new LinearLayout.LayoutParams(dp(240), LinearLayout.LayoutParams.WRAP_CONTENT));
        box.addView(label, new LinearLayout.LayoutParams(dp(52), LinearLayout.LayoutParams.WRAP_CONTENT));
        return row(title, null, box);
    }
}
