package com.ferrellsl.wifiscope;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;

// The maker of a hardware address, from the IEEE registry (the Windows app's
// res/oui.txt, an asset here): "PREFIX<tab>Name" lines, 6, 7 or 9 hex digits.
public final class Oui {
    private Oui() {}

    // one map per prefix length (24, 28 and 36 bits)
    @SuppressWarnings("unchecked")
    private static final HashMap<Long, String>[] blocks = new HashMap[] { new HashMap<>(40000), new HashMap<>(8000), new HashMap<>(8000) };
    private static volatile boolean loaded = false;

    public static synchronized void load(InputStream in) throws IOException {
        if (loaded) return;
        try (BufferedReader r = new BufferedReader(new InputStreamReader(in, StandardCharsets.UTF_8), 1 << 16)) {
            String line;
            while ((line = r.readLine()) != null) {
                final int tab = line.indexOf('\t');
                if (tab < 0 || line.startsWith("#") || (tab != 6 && tab != 7 && tab != 9)) continue;
                long v = 0;
                boolean ok = true;
                for (int i = 0; i < tab; ++i) {
                    final char c = line.charAt(i);
                    final int d = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                    if (d < 0) { ok = false; break; }
                    v = (v << 4) | d;
                }
                if (ok) blocks[tab == 6 ? 0 : tab == 7 ? 1 : 2].put(v, line.substring(tab + 1));
            }
        }
        loaded = true;
    }

    public static boolean isLoaded() { return loaded; }

    // the registered organization, or "" if none is known; longest block first
    public static String vendor(byte[] mac) {
        if (!loaded) return "";
        final long a = Bss.keyOf(mac);
        final int[] bits = { 36, 28, 24 };
        final int[] table = { 2, 1, 0 };
        for (int i = 0; i < 3; ++i) {
            String s = blocks[table[i]].get(a >>> (48 - bits[i]));
            if (s != null) return s;
        }
        return "";
    }

    // a locally administered address (made up by a device) rather than a maker's
    public static boolean local(byte[] mac) { return (mac[0] & 2) != 0; }
}
