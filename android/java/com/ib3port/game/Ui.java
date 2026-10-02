package com.ib3port.game;

import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.TextView;

/** The launcher's look: colours and the few styled views its screens are built from (no XML layouts). */
final class Ui {
    static final int BG_TOP = 0xFF161924, BG_BOTTOM = 0xFF07080C;
    static final int CARD = 0xF01A1D27, CARD_LINE = 0xFF2B303D, DIVIDER = 0xFF262A35;
    static final int ACCENT = 0xFFE2B155, ON_ACCENT = 0xFF1E1606;
    static final int TEXT = 0xFFF2F2F5, SUBTEXT = 0xFF9AA0AE, FAINT = 0xFF6B7180;

    private Ui() {}

    static int dp(Context c, float v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, c.getResources().getDisplayMetrics());
    }

    static GradientDrawable rounded(Context c, int fill, float radiusDp, int stroke) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(fill);
        d.setCornerRadius(dp(c, radiusDp));
        if (stroke != 0) d.setStroke(dp(c, 1), stroke);
        return d;
    }

    // `base` with the touch ripple, clipped to the same rounded shape.
    static Drawable pressable(Context c, Drawable base, float radiusDp) {
        return new RippleDrawable(ColorStateList.valueOf(0x30FFFFFF), base, rounded(c, 0xFFFFFFFF, radiusDp, 0));
    }

    static void screenBackground(View v) {
        v.setBackground(new GradientDrawable(GradientDrawable.Orientation.TL_BR, new int[] {BG_TOP, BG_BOTTOM}));
    }

    static TextView text(Context c, String s, float sp, int color, boolean bold) {
        TextView t = new TextView(c);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        return t;
    }

    // The main action: filled with the accent colour.
    static TextView primaryButton(Context c, String label) {
        TextView b = text(c, label, 19, ON_ACCENT, true);
        b.setGravity(Gravity.CENTER);
        b.setLetterSpacing(0.04f);
        b.setBackground(pressable(c, rounded(c, ACCENT, 14, 0), 14));
        b.setClickable(true);
        b.setFocusable(true);
        return b;
    }

    // A panel with rounded corners that groups rows.
    static LinearLayout card(Context c) {
        LinearLayout card = new LinearLayout(c);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setBackground(rounded(c, CARD, 18, CARD_LINE));
        card.setClipToOutline(true);
        return card;
    }

    static View divider(Context c) {
        View v = new View(c);
        v.setBackgroundColor(DIVIDER);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 1);
        lp.leftMargin = lp.rightMargin = dp(c, 18);
        v.setLayoutParams(lp);
        return v;
    }

    // A tappable row of a card: a title, a line under it, and a chevron.
    static LinearLayout actionRow(Context c, String title, String subtitle, View.OnClickListener click) {
        LinearLayout row = new LinearLayout(c);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(c, 18), dp(c, 13), dp(c, 14), dp(c, 13));
        row.setBackground(pressable(c, rounded(c, 0x00000000, 0, 0), 0));
        row.setClickable(true);
        row.setFocusable(true);
        row.setOnClickListener(click);
        row.addView(labels(c, title, subtitle), new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        TextView chevron = text(c, "›", 24, FAINT, false);
        chevron.setPadding(dp(c, 12), 0, 0, dp(c, 2));
        row.addView(chevron);
        return row;
    }

    // A title with an optional smaller line under it.
    static LinearLayout labels(Context c, String title, String subtitle) {
        LinearLayout box = new LinearLayout(c);
        box.setOrientation(LinearLayout.VERTICAL);
        box.addView(text(c, title, 16, TEXT, true));
        if (subtitle != null && !subtitle.isEmpty()) {
            TextView sub = text(c, subtitle, 12.5f, SUBTEXT, false);
            sub.setPadding(0, dp(c, 2), 0, 0);
            box.addView(sub);
        }
        return box;
    }

    static TextView sectionHeader(Context c, String s) {
        TextView t = text(c, s.toUpperCase(java.util.Locale.ROOT), 12, ACCENT, true);
        t.setLetterSpacing(0.12f);
        t.setPadding(dp(c, 6), dp(c, 22), 0, dp(c, 8));
        return t;
    }

    // A page's top bar: "‹ Back", the title, and an optional button on the right.
    static LinearLayout pageHeader(Context c, String title, String button, View.OnClickListener onButton, Runnable back) {
        LinearLayout header = new LinearLayout(c);
        header.setGravity(Gravity.CENTER_VERTICAL);
        header.setPadding(dp(c, 16), dp(c, 10), dp(c, 28), dp(c, 6));
        TextView backButton = text(c, "‹  Back", 16, ACCENT, true);
        backButton.setPadding(dp(c, 12), dp(c, 10), dp(c, 16), dp(c, 10));
        backButton.setBackground(pressable(c, rounded(c, 0x00000000, 10, 0), 10));
        backButton.setClickable(true);
        backButton.setOnClickListener(v -> back.run());
        header.addView(backButton);
        TextView t = text(c, title, 24, TEXT, true);
        t.setPadding(dp(c, 8), 0, 0, 0);
        header.addView(t, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        if (button != null) {
            TextView b = primaryButton(c, button);
            b.setTextSize(15);
            b.setPadding(dp(c, 22), 0, dp(c, 22), 0);
            b.setOnClickListener(onButton);
            header.addView(b, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, dp(c, 44)));
        }
        return header;
    }

    // Views that look disabled while a task runs.
    static void setEnabled(View v, boolean enabled) {
        v.setEnabled(enabled);
        v.setAlpha(enabled ? 1f : 0.45f);
    }
}
