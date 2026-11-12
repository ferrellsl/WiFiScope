package com.ferrellsl.wifiscope;

import java.util.Arrays;

// A network (one radio's BSSID) as heard, with what its beacon says about it.
// The same fields as wlan::Bss in the Windows app (src/Wlan.h).
public final class Bss implements Cloneable {
    public byte[] mac = new byte[6];
    public String ssid = "";             // empty: a hidden network
    public int rssi = -100;              // dBm
    public int frequency = 0;            // MHz, the primary 20 MHz channel's center
    public int band = 0;                 // 2 (2.4 GHz), 5 or 6
    public int channel = 0;              // the primary channel's number
    public int width = 20;               // MHz: 20, 40, 80, 160 or 320
    public int center = 0;               // MHz, the center of the whole channel
    public boolean split = false;        // 80+80 MHz: two 80 MHz parts
    public int center2 = 0;              //   (the second part's center)
    public int generation = 0;           // Wi-Fi 4 (n), 5 (ac), 6 (ax) or 7 (be); 0: older (a/b/g)
    public String phy = "";              // "b, g, n, ax" and the like
    public String security = "";         // "Open", "WPA2", "WPA3", "WPA2/WPA3", "WPA2-Enterprise", ...
    public int streams = 1;              // spatial streams
    public double maxRate = 0;           // Mbps, the most it could carry
    public double minRate = 0;           // Mbps, the lowest basic rate
    public int stations = -1;            // clients, from its BSS Load element (-1: not given)
    public int utilization = -1;         // percent of airtime busy, from the same (-1: not given)
    public boolean multiLink = false;    // Wi-Fi 7 multi-link (MLO)
    public byte[] mld = new byte[6];     //   (the multi-link device's address, the same on all its radios)
    public String country = "";          // "US", ...
    public long ageMs = 0;               // how long ago its last beacon was heard
    public boolean details = false;      // the report carried the beacon's elements

    @Override
    public Bss clone() {
        try {
            Bss b = (Bss) super.clone();
            b.mac = Arrays.copyOf(mac, 6);
            b.mld = Arrays.copyOf(mld, 6);
            return b;
        } catch (CloneNotSupportedException e) {
            throw new AssertionError(e);
        }
    }

    public static long keyOf(byte[] m) {
        long k = 0;
        for (int i = 0; i < 6; ++i) k = (k << 8) | (m[i] & 0xFF);
        return k;
    }

    public static String macText(byte[] m) {
        return String.format("%02x:%02x:%02x:%02x:%02x:%02x",
                m[0] & 0xFF, m[1] & 0xFF, m[2] & 0xFF, m[3] & 0xFF, m[4] & 0xFF, m[5] & 0xFF);
    }

    // "aa:bb:cc:dd:ee:ff" -> 6 bytes, or null
    public static byte[] parseMac(String s) {
        if (s == null || s.length() != 17) return null;
        byte[] m = new byte[6];
        try {
            for (int i = 0; i < 6; ++i) m[i] = (byte) Integer.parseInt(s.substring(i * 3, i * 3 + 2), 16);
        } catch (NumberFormatException e) {
            return null;
        }
        return m;
    }
}
