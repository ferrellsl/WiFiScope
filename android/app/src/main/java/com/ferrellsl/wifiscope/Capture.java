package com.ferrellsl.wifiscope;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

// A scan recorded on Windows by tools/capture.exe (the raw beacons, and what the
// Windows decoder made of them), played back in place of the scanner - so both
// apps can be compared on the very same beacons.  Lines, tab-separated:
//   #connected <bssid>      #adapter <name>
//   bssid  rssi  MHz  capability(hex)  elements(hex)  | then the Windows decoder's fields
public final class Capture implements Model.Source {

    public static final class Line {
        public String bssid;
        public int rssi, frequency, capabilities;
        public byte[] ies;
        public String[] windows;                 // the Windows decoder's fields (see tools/capture.cpp)
    }

    public final List<Line> lines = new ArrayList<>();
    private String connected = "", adapter = "";

    public static Capture read(File f) throws IOException {
        Capture c = new Capture();
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(f), StandardCharsets.UTF_8))) {
            String s;
            while ((s = r.readLine()) != null) {
                if (s.isEmpty()) continue;
                String[] p = s.split("\t", -1);
                if (p[0].equals("#connected")) { c.connected = p.length > 1 ? p[1] : ""; continue; }
                if (p[0].equals("#adapter")) { c.adapter = p.length > 1 ? p[1] : ""; continue; }
                if (p[0].startsWith("#") || p.length < 5) continue;
                Line l = new Line();
                l.bssid = p[0];
                l.rssi = Integer.parseInt(p[1]);
                l.frequency = Integer.parseInt(p[2]);
                l.capabilities = Integer.parseInt(p[3], 16);
                l.ies = hex(p[4]);
                int bar = 5;
                while (bar < p.length && !p[bar].equals("|")) ++bar;
                l.windows = bar < p.length ? java.util.Arrays.copyOfRange(p, bar + 1, p.length) : new String[0];
                c.lines.add(l);
            }
        }
        return c;
    }

    static byte[] hex(String s) {
        byte[] out = new byte[s.length() / 2];
        for (int i = 0; i < out.length; ++i) out[i] = (byte) Integer.parseInt(s.substring(2 * i, 2 * i + 2), 16);
        return out;
    }

    // a line as a network, through the Android app's decoder
    public static Bss decode(Line l) {
        Bss b = new Bss();
        b.mac = Bss.parseMac(l.bssid);
        b.rssi = l.rssi;
        b.frequency = l.frequency;
        b.band = Decoder.bandOf(l.frequency);
        b.channel = Decoder.channelOf(l.frequency);
        Decoder.decode(l.ies, l.capabilities, b);
        String ssid = Decoder.ssidOf(l.ies);
        b.ssid = ssid == null ? "" : ssid;
        return b;
    }

    @Override public List<Bss> networks() {
        List<Bss> out = new ArrayList<>();
        for (Line l : lines) if (Bss.parseMac(l.bssid) != null) out.add(decode(l));
        return out;
    }

    @Override public String connectedBssid() { return connected; }
    @Override public String adapterName() { return adapter.isEmpty() ? "recorded scan" : adapter + " (recorded)"; }
    @Override public boolean hasSixGhz() { return adapter.contains("6E") || adapter.contains("BE2"); }
}
