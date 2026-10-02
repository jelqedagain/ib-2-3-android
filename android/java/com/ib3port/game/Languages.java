package com.ib3port.game;

import android.content.Context;

import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * The game's languages: each is a set of text files in the .ipa (CookedIPhone/Coalesced_FRA.bin...),
 * named by Unreal's language suffix. settings.ini [Game] Language holds the player's choice (empty:
 * the phone's language), [Game] PhoneLanguage the phone's language as of the last Play; the game
 * reads both when it starts (src/game/language.cpp).
 */
final class Languages {
    static final String[] SUFFIXES = {
        "INT", "FRA", "DEU", "ITA", "ESN", "ESM", "BRA", "POR", "DUT", "SWE", "POL", "CZE", "HUN", "SLO", "RUS", "JPN", "KOR", "CHN",
        "THA", "IND",
    };
    private static final String[] NAMES = {
        "English", "Français", "Deutsch", "Italiano", "Español (España)", "Español (Latinoamérica)", "Português (Brasil)",
        "Português (Portugal)", "Nederlands", "Svenska", "Polski", "Čeština", "Magyar", "Slovenčina", "Русский", "日本語", "한국어",
        "中文", "ไทย", "Bahasa Indonesia",
    };

    private Languages() {}

    static String name(String suffix) {
        for (int i = 0; i < SUFFIXES.length; i++)
            if (SUFFIXES[i].equals(suffix)) return NAMES[i];
        return suffix;
    }

    /** The languages the installed game has text for, in the order above. */
    static List<String> available(File filesDir) {
        File cooked = new File(filesDir, "game/Payload/SwordGame.app/CookedIPhone");
        List<String> out = new ArrayList<>();
        for (String s : SUFFIXES)
            if (s.equals("INT") || new File(cooked, "Coalesced_" + s + ".bin").isFile()) out.add(s);
        return out;
    }

    /** The phone's language, if the game has it (else English). */
    static String phone(Context c, List<String> available) {
        Locale l = c.getResources().getConfiguration().getLocales().get(0);
        String lang = l.getLanguage(), country = l.getCountry();
        String[] wanted;
        switch (lang) {
            case "fr": wanted = new String[] {"FRA"}; break;
            case "de": wanted = new String[] {"DEU"}; break;
            case "it": wanted = new String[] {"ITA"}; break;
            case "es": wanted = country.isEmpty() || country.equals("ES") ? new String[] {"ESN", "ESM"} : new String[] {"ESM", "ESN"}; break;
            case "pt": wanted = country.equals("PT") ? new String[] {"POR", "BRA"} : new String[] {"BRA", "POR"}; break;
            case "nl": wanted = new String[] {"DUT"}; break;
            case "sv": wanted = new String[] {"SWE"}; break;
            case "pl": wanted = new String[] {"POL"}; break;
            case "cs": wanted = new String[] {"CZE"}; break;
            case "hu": wanted = new String[] {"HUN"}; break;
            case "sk": wanted = new String[] {"SLO"}; break;
            case "ru": wanted = new String[] {"RUS"}; break;
            case "ja": wanted = new String[] {"JPN"}; break;
            case "ko": wanted = new String[] {"KOR"}; break;
            case "zh": wanted = new String[] {"CHN"}; break;
            case "th": wanted = new String[] {"THA"}; break;
            case "id": case "in": wanted = new String[] {"IND"}; break;  // Java still says "in" for Indonesian
            default: wanted = new String[0];
        }
        for (String s : wanted)
            if (available.contains(s)) return s;
        return "INT";
    }

    /** Records the phone's language for the game (before it starts). */
    static void savePhoneLanguage(Context c, File filesDir) {
        IniFile ini = new IniFile(new File(filesDir, "settings.ini"));
        String phone = phone(c, available(filesDir));
        if (phone.equals(ini.get("Game", "PhoneLanguage", ""))) return;
        ini.set("Game", "PhoneLanguage", phone);
        try {
            ini.save();
        } catch (java.io.IOException ignored) {
            // the game then uses the language saved before, or English
        }
    }
}
