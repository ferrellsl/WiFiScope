package com.ferrellsl.wifiscope;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.net.wifi.ScanResult;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.SystemClock;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;

// Scanning with Android's WifiManager.  Android 11 and later hand over each
// beacon's information elements, which go through the same decoder as on
// Windows; before that (or when a driver leaves them out) the width, security
// and generation come from what ScanResult itself says.
public final class Scanner implements Model.Source {
    public interface Listener { void scanned(); }

    private final Context context;
    private final WifiManager wifi;
    private BroadcastReceiver receiver;
    private int refusedInARow = 0;

    public Scanner(Context context) {
        this.context = context.getApplicationContext();
        this.wifi = (WifiManager) this.context.getSystemService(Context.WIFI_SERVICE);
    }

    public boolean available() { return wifi != null; }
    public boolean wifiOn() { return wifi != null && wifi.isWifiEnabled(); }

    public void start(Listener listener) {
        if (wifi == null || receiver != null) return;
        receiver = new BroadcastReceiver() {
            @Override public void onReceive(Context c, Intent i) { listener.scanned(); }
        };
        IntentFilter f = new IntentFilter(WifiManager.SCAN_RESULTS_AVAILABLE_ACTION);
        if (Build.VERSION.SDK_INT >= 33) context.registerReceiver(receiver, f, Context.RECEIVER_NOT_EXPORTED);
        else context.registerReceiver(receiver, f);
    }

    public void stop() {
        if (receiver != null) {
            try { context.unregisterReceiver(receiver); } catch (IllegalArgumentException ignored) {}
            receiver = null;
        }
    }

    // Asks for a scan.  Android lets an app in front scan 4 times in 2 minutes
    // unless "Wi-Fi scan throttling" is turned off in the developer options;
    // a refused scan returns false (the results then come from the system's own scans).
    @SuppressWarnings("deprecation")
    public boolean scan() {
        if (wifi == null) return false;
        boolean ok;
        try { ok = wifi.startScan(); } catch (SecurityException e) { ok = false; }
        refusedInARow = ok ? 0 : refusedInARow + 1;
        return ok;
    }

    public boolean throttled() { return refusedInARow >= 2; }

    @Override public String adapterName() { return Build.MANUFACTURER + " " + Build.MODEL; }

    @Override public boolean hasSixGhz() {
        return wifi != null && Build.VERSION.SDK_INT >= 30 && wifi.is6GHzBandSupported();
    }

    @SuppressWarnings("deprecation")
    @Override public String connectedBssid() {
        if (wifi == null) return "";
        try {
            WifiInfo info = wifi.getConnectionInfo();
            String b = info == null ? null : info.getBSSID();
            if (b == null || b.equals("02:00:00:00:00:00")) return "";   // (hidden from apps without location)
            return b.toLowerCase();
        } catch (SecurityException e) {
            return "";
        }
    }

    @Override public List<Bss> networks() {
        List<Bss> out = new ArrayList<>();
        if (wifi == null) return out;
        List<ScanResult> results;
        try { results = wifi.getScanResults(); } catch (SecurityException e) { return out; }
        if (results == null) return out;
        final long now = SystemClock.elapsedRealtime();
        for (ScanResult r : results) {
            Bss b = from(r);
            if (b == null) continue;
            b.ageMs = Math.max(0, now - r.timestamp / 1000);
            out.add(b);
        }
        return out;
    }

    static Bss from(ScanResult r) {
        byte[] mac = Bss.parseMac(r.BSSID);
        if (mac == null || r.frequency <= 0) return null;
        // (no signal measured: a link Android knows of only from another radio's report -
        // a Wi-Fi 7 router's other links, say - with "0 dBm"; it wasn't heard itself)
        if (r.level >= 0) return null;
        Bss b = new Bss();
        b.mac = mac;
        b.rssi = r.level;
        b.frequency = r.frequency;
        b.band = Decoder.bandOf(r.frequency);
        b.channel = Decoder.channelOf(r.frequency);
        final byte[] ies = Build.VERSION.SDK_INT >= 30 ? elements(r) : null;
        final String caps = r.capabilities == null ? "" : r.capabilities;
        // (ScanResult has no capability field: WEP is the privacy bit's only use left)
        Decoder.decode(ies, caps.contains("WEP") ? 0x0011 : 0x0001, b);
        String ssid = Decoder.ssidOf(ies);
        if (ssid == null) ssid = ssidOf(r);
        b.ssid = ssid;
        if (!b.details) fromSummary(r, caps, b);
        return b;
    }

    // the elements as one buffer, as a beacon carries them
    private static byte[] elements(ScanResult r) {
        List<ScanResult.InformationElement> list;
        try { list = r.getInformationElements(); } catch (RuntimeException e) { return null; }
        if (list == null || list.isEmpty()) return null;
        ByteArrayOutputStream out = new ByteArrayOutputStream(512);
        for (ScanResult.InformationElement e : list) {
            ByteBuffer data = e.getBytes();
            byte[] d = new byte[data == null ? 0 : data.remaining()];
            if (data != null) data.duplicate().get(d);
            final boolean ext = e.getId() == 255;
            final int len = d.length + (ext ? 1 : 0);
            if (len > 255) continue;
            out.write(e.getId());
            out.write(len);
            if (ext) out.write(e.getIdExt());
            out.write(d, 0, d.length);
        }
        return out.toByteArray();
    }

    @SuppressWarnings("deprecation")
    private static String ssidOf(ScanResult r) {
        if (Build.VERSION.SDK_INT >= 33 && r.getWifiSsid() != null) {
            byte[] s = r.getWifiSsid().getBytes();
            return Decoder.ssidText(s, 0, Math.min(32, s.length));
        }
        String s = r.SSID;
        if (s == null || s.equals("<unknown ssid>")) return "";
        if (s.length() >= 2 && s.startsWith("\"") && s.endsWith("\"")) s = s.substring(1, s.length() - 1);
        return s;
    }

    // Without the elements: Android's own summary of the channel, security and standard.
    static void fromSummary(ScanResult r, String caps, Bss b) {
        switch (r.channelWidth) {
            case 1: b.width = 40; break;
            case 2: b.width = 80; break;
            case 3: b.width = 160; break;
            case 4: b.width = 160; b.split = true; break;
            case 5: b.width = 320; break;
            default: b.width = 20;
        }
        b.center = r.centerFreq0 > 0 && b.width > 20 ? r.centerFreq0 : r.frequency;
        if (b.split) b.center2 = r.centerFreq1;
        b.security = securityOf(caps);
        if (Build.VERSION.SDK_INT >= 30) {
            switch (r.getWifiStandard()) {
                case 4: b.generation = 4; break;   // (WIFI_STANDARD_11N)
                case 5: b.generation = 5; break;   // (11AC)
                case 6: b.generation = 6; break;   // (11AX)
                case 8: b.generation = 7; break;   // (11BE)
                default: b.generation = 0;
            }
        }
        b.maxRate = 0;
        b.minRate = 0;
    }

    // "[WPA2-PSK-CCMP][RSN-PSK+SAE-CCMP][ESS]" and the like
    static String securityOf(String caps) {
        final String c = caps.toUpperCase();
        if (c.contains("SUITE_B") || c.contains("SUITE-B")) return "WPA3-Enterprise";
        if (c.contains("EAP")) return "WPA2-Enterprise";
        final boolean sae = c.contains("SAE"), psk = c.contains("PSK");
        if (c.contains("OWE") && !sae && !psk) return "OWE";
        if (sae && psk) return "WPA2/WPA3";
        if (sae) return "WPA3";
        if (psk) {
            final boolean rsn = c.contains("WPA2") || c.contains("RSN");
            final boolean wpa1 = c.contains("[WPA-");
            return rsn ? (wpa1 ? "WPA/WPA2" : "WPA2") : "WPA";
        }
        if (c.contains("WEP")) return "WEP";
        return "Open";
    }
}
