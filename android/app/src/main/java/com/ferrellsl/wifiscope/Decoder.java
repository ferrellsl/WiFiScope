package com.ferrellsl.wifiscope;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

// Decodes a beacon's information elements (IEEE 802.11-2020 and 802.11be: HT,
// VHT, HE, EHT, RSN, BSS Load, Multi-Link) - a line-for-line port of
// wlan::decode in the Windows app (src/Wlan.cpp), checked by DecoderTest
// against the same cases as tests/decode_test.cpp.
public final class Decoder {
    private Decoder() {}

    public static int frequencyOf(int band, int channel) {
        if (band == 2) return channel == 14 ? 2484 : 2407 + 5 * channel;
        if (band == 6) return channel == 2 ? 5935 : 5950 + 5 * channel;
        return 5000 + 5 * channel;
    }

    public static int bandOf(int mhz) { return mhz < 3000 ? 2 : mhz < 5925 ? 5 : 6; }

    public static int channelOf(int mhz) {
        int band = bandOf(mhz);
        return band == 2 ? (mhz == 2484 ? 14 : (mhz - 2407) / 5)
             : band == 6 ? (mhz == 5935 ? 2 : (mhz - 5950) / 5)
             : (mhz - 5000) / 5;
    }

    // per spatial stream, the shortest guard interval
    private static double htRate(int width) { return width >= 40 ? 150.0 : 72.2; }                     // MCS 7
    private static final double[][] VHT = { { 72.2, 86.7, 86.7 }, { 150, 180, 200 }, { 325, 390, 433.3 }, { 650, 780, 866.7 } };
    private static double vhtRate(int width, int maxMcs) {                                             // MCS 7, 8, 9
        int w = width >= 160 ? 3 : width >= 80 ? 2 : width >= 40 ? 1 : 0;
        return VHT[w][Math.max(0, Math.min(2, maxMcs - 7))];
    }
    private static final double[][] HE = { { 86.0, 114.7, 143.4 }, { 172.1, 229.4, 286.8 }, { 360.3, 480.4, 600.5 }, { 720.6, 960.8, 1201.0 } };
    private static double heRate(int width, int maxMcs) {                                              // MCS 7, 9, 11 (0.8 us)
        int w = width >= 160 ? 3 : width >= 80 ? 2 : width >= 40 ? 1 : 0;
        return HE[w][maxMcs >= 11 ? 2 : maxMcs >= 9 ? 1 : 0];
    }
    private static double ehtRate(int width) {                                                         // MCS 13 (0.8 us)
        return width >= 320 ? 2882.4 : width >= 160 ? 1441.2 : width >= 80 ? 720.6 : width >= 40 ? 344.1 : 172.1;
    }

    // streams and highest MCS code in a VHT / HE MCS map (2 bits a stream; 3: not supported)
    private static int[] mcsMap(int map) {
        int streams = 0, maxCode = -1;
        for (int s = 0; s < 8; ++s) {
            int code = (map >> (2 * s)) & 3;
            if (code != 3) { streams = s + 1; maxCode = Math.max(maxCode, code); }
        }
        return new int[] { streams, maxCode };
    }

    private static int u8(byte[] a, int i) { return a[i] & 0xFF; }
    private static int le16(byte[] a, int i) { return u8(a, i) | (u8(a, i + 1) << 8); }

    public static void decode(byte[] ies, int capabilities, Bss b) {
        final int size = ies == null ? 0 : ies.length;
        boolean ht = false, vht = false, he = false, eht = false, rsn = false, wpa1 = false;
        boolean htWide = false;
        int htSecondary = 0;
        int vhtWidth = -1, vhtCcfs0 = 0, vhtCcfs1 = 0;
        int heWidth = -1, heCcfs0 = 0, heCcfs1 = 0, hePrimary = 0;
        int ehtWidth = -1, ehtCcfs0 = 0, ehtCcfs1 = 0;
        int htStreams = 1, vhtStreams = 0, vhtMcs = 9, heStreams = 0, heMcs = 11;
        List<Double> basic = new ArrayList<>(), all = new ArrayList<>();
        boolean akmPsk = false, akmSae = false, akm8021x = false, akmSuiteB = false, akmOwe = false;
        boolean hasB = false, hasG = false;
        b.details = size > 0;

        int at = 0;
        while (at + 2 <= size) {
            final int id = u8(ies, at), len = u8(ies, at + 1);
            final int d = at + 2;                       // (the element's data, as an index into ies)
            if (at + 2 + len > size) break;
            switch (id) {
                case 1: case 50:                        // supported rates, extended supported rates (500 kbps)
                    for (int i = 0; i < len; ++i) {
                        final int v = u8(ies, d + i);
                        final double rate = (v & 0x7F) * 0.5;
                        if ((v & 0x7F) == 0x7F || (v & 0x7F) == 0x7E) continue;   // (membership selectors)
                        all.add(rate);
                        if ((v & 0x80) != 0) basic.add(rate);
                        if (rate == 1 || rate == 2 || rate == 5.5 || rate == 11) hasB = true;
                        else hasG = true;
                    }
                    break;
                case 7:                                 // country
                    if (len >= 2) b.country = new String(new char[] { (char) u8(ies, d), (char) u8(ies, d + 1) });
                    break;
                case 11:                                // BSS Load: stations, channel utilization (of 255)
                    if (len >= 3) {
                        b.stations = le16(ies, d);
                        b.utilization = (int) Math.round(u8(ies, d + 2) * 100.0 / 255.0);
                    }
                    break;
                case 45:                                // HT capabilities: the supported MCS set at 3
                    ht = true;
                    if (len >= 7) {
                        htStreams = 0;
                        for (int s = 0; s < 4; ++s) if (u8(ies, d + 3 + s) != 0) htStreams = s + 1;
                        htStreams = Math.max(1, htStreams);
                    }
                    break;
                case 61:                                // HT operation
                    if (len >= 2) {
                        final int offset = u8(ies, d + 1) & 3;
                        htWide = (u8(ies, d + 1) & 4) != 0 && (offset == 1 || offset == 3);
                        htSecondary = offset == 1 ? 1 : offset == 3 ? -1 : 0;
                    }
                    break;
                case 48: {                              // RSN: group, pairwise suites, AKM suites
                    rsn = true;
                    int p = 2 + 4;                      // (version, group cipher)
                    if (len >= p + 2) {
                        final int pairwise = le16(ies, d + p); p += 2 + 4 * pairwise;
                        if (len >= p + 2) {
                            final int akms = le16(ies, d + p); p += 2;
                            for (int i = 0; i < akms && p + 4 <= len; ++i, p += 4) {
                                if (u8(ies, d + p) != 0x00 || u8(ies, d + p + 1) != 0x0F || u8(ies, d + p + 2) != 0xAC) continue;
                                switch (u8(ies, d + p + 3)) {
                                    case 1: case 3: case 5: akm8021x = true; break;
                                    case 11: case 12: case 13: akmSuiteB = true; break;
                                    case 2: case 4: case 6: akmPsk = true; break;
                                    case 8: case 9: case 24: case 25: akmSae = true; break;
                                    case 18: akmOwe = true; break;
                                }
                            }
                        }
                    }
                    break;
                }
                case 221:                               // vendor: Microsoft WPA (version 1)
                    if (len >= 4 && u8(ies, d) == 0x00 && u8(ies, d + 1) == 0x50 && u8(ies, d + 2) == 0xF2 && u8(ies, d + 3) == 1) wpa1 = true;
                    break;
                case 191:                               // VHT capabilities: the Rx MCS map at 4
                    vht = true;
                    if (len >= 6) {
                        int[] m = mcsMap(le16(ies, d + 4));
                        vhtStreams = m[0];
                        vhtMcs = 7 + Math.max(0, m[1]);
                    }
                    break;
                case 192:                               // VHT operation: width, CCFS0, CCFS1
                    if (len >= 3) { vhtWidth = u8(ies, d); vhtCcfs0 = u8(ies, d + 1); vhtCcfs1 = u8(ies, d + 2); }
                    break;
                case 255: {                             // extension elements
                    if (len < 1) break;
                    final int ext = u8(ies, d);
                    final int e = d + 1;
                    final int elen = len - 1;
                    if (ext == 35) {                    // HE capabilities: MAC (6), PHY (11), then MCS maps
                        he = true;
                        if (elen >= 19) {
                            int[] m = mcsMap(le16(ies, e + 17));
                            heStreams = m[0];
                            heMcs = m[1] >= 2 ? 11 : m[1] == 1 ? 9 : 7;
                        }
                    } else if (ext == 36 && elen >= 6) { // HE operation
                        final int params = u8(ies, e) | (u8(ies, e + 1) << 8) | (u8(ies, e + 2) << 16);
                        int p = 6;
                        if ((params & (1 << 14)) != 0) p += 3;   // (VHT operation information)
                        if ((params & (1 << 15)) != 0) p += 1;   // (max co-hosted BSSID indicator)
                        if ((params & (1 << 17)) != 0 && elen >= p + 5) {   // 6 GHz operation information
                            hePrimary = u8(ies, e + p);
                            heWidth = u8(ies, e + p + 1) & 3;
                            heCcfs0 = u8(ies, e + p + 2);
                            heCcfs1 = u8(ies, e + p + 3);
                        }
                    } else if (ext == 108) eht = true;  // EHT capabilities
                    else if (ext == 106 && elen >= 5) { // EHT operation
                        eht = true;
                        if ((u8(ies, e) & 1) != 0 && elen >= 8) {
                            ehtWidth = u8(ies, e + 5) & 7;
                            ehtCcfs0 = u8(ies, e + 6);
                            ehtCcfs1 = u8(ies, e + 7);
                        }
                    } else if (ext == 107 && elen >= 9) { // Multi-Link: control (2), common info length (1), MLD address
                        if ((u8(ies, e) & 7) == 0) {    // (basic multi-link element)
                            b.multiLink = true;
                            System.arraycopy(ies, e + 3, b.mld, 0, 6);
                        }
                    }
                    break;
                }
            }
            at += 2 + len;
        }

        // the channel: 20 MHz unless the operation elements say more; the widest one wins
        b.width = 20;
        b.center = b.frequency;
        b.split = false;
        final int band = b.band;
        if (ht && htWide && band != 6) {
            b.width = 40;
            b.center = b.frequency + 10 * htSecondary;
        }
        if (vhtWidth >= 1 && band == 5) {
            if (vhtWidth == 1 && vhtCcfs1 != 0 && Math.abs(vhtCcfs1 - vhtCcfs0) == 8) { b.width = 160; b.center = frequencyOf(band, vhtCcfs1); }
            else if (vhtWidth == 1 && vhtCcfs1 != 0 && Math.abs(vhtCcfs1 - vhtCcfs0) > 16) {
                b.width = 160; b.split = true; b.center = frequencyOf(band, vhtCcfs0); b.center2 = frequencyOf(band, vhtCcfs1);
            }
            else if (vhtWidth == 2) { b.width = 160; b.center = frequencyOf(band, vhtCcfs0); }
            else if (vhtWidth == 3) { b.width = 160; b.split = true; b.center = frequencyOf(band, vhtCcfs0); b.center2 = frequencyOf(band, vhtCcfs1); }
            else { b.width = 80; b.center = frequencyOf(band, vhtCcfs0); }
        }
        if (heWidth >= 0 && band == 6) {
            final int[] widths = { 20, 40, 80, 160 };
            b.width = widths[heWidth];
            b.center = frequencyOf(band, heCcfs0);
            if (heWidth == 3 && heCcfs1 != 0) {
                if (Math.abs(heCcfs1 - heCcfs0) == 8) b.center = frequencyOf(band, heCcfs1);
                else { b.split = true; b.center2 = frequencyOf(band, heCcfs1); }
            }
            if (hePrimary != 0) b.channel = hePrimary;
        }
        if (ehtWidth >= 0) {
            final int[] widths = { 20, 40, 80, 160, 320, 320, 320, 320 };
            final int w = widths[ehtWidth];
            if (w > b.width) {
                b.width = w;
                b.split = false;
                b.center = frequencyOf(band, w >= 160 ? ehtCcfs1 : ehtCcfs0);
            }
        }

        // the generation and the PHY list
        b.generation = eht ? 7 : he ? 6 : vht ? 5 : ht ? 4 : 0;
        StringBuilder phy = new StringBuilder();
        if (band == 2) { if (hasB) add(phy, "b"); if (hasG) add(phy, "g"); }
        else if (band == 5) add(phy, "a");
        if (ht) add(phy, "n");
        if (vht) add(phy, "ac");
        if (he) add(phy, "ax");
        if (eht) add(phy, "be");
        b.phy = phy.toString();

        // the most it could carry: its widest operating channel and its streams
        if (eht) { b.streams = Math.max(1, heStreams); b.maxRate = ehtRate(b.width) * b.streams; }
        else if (he) { b.streams = Math.max(1, heStreams); b.maxRate = heRate(b.width, heMcs) * b.streams; }
        else if (vht) { b.streams = Math.max(1, vhtStreams); b.maxRate = vhtRate(b.width, vhtMcs) * b.streams; }
        else if (ht) { b.streams = htStreams; b.maxRate = htRate(b.width) * b.streams; }
        else { b.streams = 1; b.maxRate = all.isEmpty() ? 0 : Collections.max(all); }
        b.minRate = !basic.isEmpty() ? Collections.min(basic) : !all.isEmpty() ? Collections.min(all) : 6.0;

        // security
        if (rsn) {
            if (akmOwe && !akmPsk && !akmSae && !akm8021x) b.security = "OWE";
            else if (akmSuiteB) b.security = "WPA3-Enterprise";
            else if (akm8021x) b.security = "WPA2-Enterprise";
            else if (akmSae && akmPsk) b.security = "WPA2/WPA3";
            else if (akmSae) b.security = "WPA3";
            else if (akmPsk) b.security = wpa1 ? "WPA/WPA2" : "WPA2";
            else b.security = "WPA2";
        }
        else if (wpa1) b.security = "WPA";
        else if ((capabilities & 0x10) != 0) b.security = "WEP";
        else if (!b.details) b.security = "?";
        else b.security = "Open";
    }

    private static void add(StringBuilder s, String part) {
        if (s.length() > 0) s.append(", ");
        s.append(part);
    }

    // The SSID element (0), the raw bytes as the Windows app reads them: UTF-8, or
    // Latin-1 when not; all zeros (or none) is a hidden network.  Null if absent.
    public static String ssidOf(byte[] ies) {
        if (ies == null) return null;
        int at = 0;
        while (at + 2 <= ies.length) {
            final int id = u8(ies, at), len = u8(ies, at + 1);
            if (at + 2 + len > ies.length) break;
            if (id == 0) return ssidText(ies, at + 2, Math.min(len, 32));
            at += 2 + len;
        }
        return null;
    }

    public static String ssidText(byte[] a, int off, int n) {
        boolean blank = true;
        for (int i = 0; i < n; ++i) if (a[off + i] != 0) blank = false;
        if (blank) return "";
        java.nio.charset.CharsetDecoder utf8 = java.nio.charset.StandardCharsets.UTF_8.newDecoder();
        try {
            return utf8.decode(java.nio.ByteBuffer.wrap(a, off, n)).toString();
        } catch (java.nio.charset.CharacterCodingException e) {
            return new String(a, off, n, java.nio.charset.StandardCharsets.ISO_8859_1);
        }
    }
}
