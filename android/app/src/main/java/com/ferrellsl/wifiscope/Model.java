package com.ferrellsl.wifiscope;

import java.security.SecureRandom;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;

// What has been heard, and the table's rows - the data half of the Windows
// app's main.cpp, kept in the same order and with the same rules.
public final class Model {

    // where the networks come from: the scanner, or a recorded scan played back
    public interface Source {
        List<Bss> networks();
        String connectedBssid();
        String adapterName();
        boolean hasSixGhz();
    }

    public static final int FADE_AFTER_MS = 30000;     // not heard for this long: drawn faded
    public static final int DROP_AFTER_MS = 120000;    // ... and then gone from the list
    public static final int HISTORY_SECONDS = 300;
    public static final int GAP_MS = 25000;            // a signal line breaks where nothing was heard for this long

    public static final class Entry {
        public Bss bss = new Bss();
        public long heard = 0;                          // clock ms of its last beacon
        public int color = 0;
        public String vendor = "";
        public final ArrayDeque<long[]> history = new ArrayDeque<>();   // (time, dBm)
        int openReports = 0;

        Entry copy() {
            Entry e = new Entry();
            e.bss = bss.clone(); e.heard = heard; e.color = color; e.vendor = vendor;
            return e;
        }
    }

    public static final class Group {
        public long id;                                 // its first radio's BSSID
        public final List<Long> members = new ArrayList<>();
        public Entry summary;
        public String names = "", channels = "", bands = "";
    }

    public static final class Row {
        public final int group; public final long key; public final boolean summary;
        Row(int g, long k, boolean s) { group = g; key = k; summary = s; }
    }

    // the columns, in the order the phone shows them (signal and channel early)
    public enum Column {
        SSID("SSID", 150, false), SIGNAL("Signal", 104, false), CHANNEL("Channel", 66, true), WIDTH("Width", 58, true),
        BAND("Band", 50, true), WIFI("Wi-Fi", 50, true), SECURITY("Security", 112, false), BSSID("BSSID", 128, false),
        VENDOR("Vendor", 170, false), MAX("Max rate", 72, true), MIN("Min", 46, true), CLIENTS("Clients", 60, true),
        UTIL("Busy", 50, true), SEEN("Seen", 52, true);
        public final String title; public final float width; public final boolean right;
        Column(String t, float w, boolean r) { title = t; width = w; right = r; }
    }

    public final Map<Long, Entry> entries = new HashMap<>();
    // The newest beacon heard from any network.  Android may scan only 4 times in 2
    // minutes, so the newest results can be a minute or more old: a network fades
    // when the latest scans missed it (30 s older than the newest), not by the clock.
    public long lastHeard = 0;
    public static final long FORGET_AFTER_MS = 600000;   // (and gone however Android scans after 10 minutes)
    public String connected = "";
    public String adapter = "";
    public boolean hasSixGhz = false;

    public final List<Group> groups = new ArrayList<>();
    public final List<Row> rows = new ArrayList<>();
    public final List<Long> shown = new ArrayList<>();
    public final Map<Long, Long> groupOf = new HashMap<>();
    public final Set<Long> opened = new HashSet<>();

    public Column sortColumn = Column.SIGNAL;
    public boolean sortDown = true;
    public final boolean[] showBand = { true, true, true };
    public boolean grouped = false;
    public String filter = "";
    public long selected = 0, selectedGroup = 0;

    // ------------------------------------------------------------- helpers
    public static int bandIndex(int band) { return band == 2 ? 0 : band == 5 ? 1 : 2; }
    public static String bandName(int band) { return band == 2 ? "2.4" : band == 5 ? "5" : "6"; }

    public static String wifiName(Bss b) {
        switch (b.generation) {
            case 7: return "7";
            case 6: return b.band == 6 ? "6E" : "6";
            case 5: return "5";
            case 4: return "4";
            default: return b.details ? (b.band == 2 ? "b/g" : "a") : "";
        }
    }

    public static String rateText(double mbps) {
        if (mbps <= 0) return "";
        return mbps >= 100 ? String.format(Locale.US, "%.0f", mbps) : String.format(Locale.US, "%.1f", mbps);
    }

    public static String nameOf(Bss b) { return b.ssid.isEmpty() ? "(hidden)" : b.ssid; }

    // a steady, distinct color for each BSSID (the same as on Windows)
    public static int colorFor(long key) {
        final long h = key * 0x9E3779B97F4A7C15L;
        final double hue = (double) Long.remainderUnsigned(h >>> 20, 360), s = 0.55 + Long.remainderUnsigned(h >>> 40, 30) / 100.0, v = 0.95;
        final double c = v * s, x = c * (1 - Math.abs((hue / 60) % 2 - 1)), m = v - c;
        double r = 0, g = 0, b = 0;
        if (hue < 60) { r = c; g = x; } else if (hue < 120) { r = x; g = c; } else if (hue < 180) { g = c; b = x; }
        else if (hue < 240) { g = x; b = c; } else if (hue < 300) { r = x; b = c; } else { r = c; b = x; }
        return ((int) ((r + m) * 255) << 16) | ((int) ((g + m) * 255) << 8) | (int) ((b + m) * 255);
    }

    // Two addresses of one router: the second and third bytes the same, and at
    // least four of the last five.
    public static boolean sibling(byte[] a, byte[] b) {
        if (a[1] != b[1] || a[2] != b[2]) return false;
        int same = 0;
        for (int i = 1; i < 6; ++i) if (a[i] == b[i]) ++same;
        return same >= 4;
    }

    byte[] connectedMac() { return Bss.parseMac(connected); }

    // ------------------------------------------------------------- demo mode
    // For screenshots that can be shared: made-up names, and each of the last
    // three address bytes through its own shuffled table (random each run).
    private boolean demo = false;
    private long demoSalt = 0;
    private final int[][] demoTable = new int[3][256];
    private final Map<String, String> demoNames = new HashMap<>();
    private static final String[] DEMO_NAMES = {
        "Maple House", "Blue Heron", "Casa Verde", "Nighthawk", "Juniper", "Saguaro", "The Burrow",
        "Starlight", "Coyote Den", "Lakeside", "Copper Ridge", "Bluebird", "Quail Run", "Desert Rose",
        "Mesquite", "Ironwood", "Palo Verde", "Sunset 5G", "Granite Peak", "Red Rock", "Ocotillo",
        "Thunderbird", "Hummingbird", "Pinecone", "Riverbend", "Cactus Flower", "Willow", "Aspen",
        "Canyon View", "Moonbeam", "Roadrunner", "Sycamore", "Tumbleweed", "Agave", "Monsoon",
    };

    public boolean demo() { return demo; }

    public void setDemo(boolean on) {
        if (on == demo) return;
        demo = on;
        entries.clear();
        opened.clear();
        selected = selectedGroup = 0;
        if (!on) return;
        SecureRandom random = new SecureRandom();
        demoSalt = random.nextLong();
        demoNames.clear();
        for (int[] t : demoTable) {
            for (int i = 0; i < 256; ++i) t[i] = i;
            for (int i = 255; i > 0; --i) {
                int j = random.nextInt(i + 1);
                int s = t[i]; t[i] = t[j]; t[j] = s;
            }
        }
    }

    private void disguise(byte[] mac) {
        for (int i = 3; i < 6; ++i) mac[i] = (byte) demoTable[i - 3][mac[i] & 0xFF];
    }

    private void disguise(Bss b) {
        if (!b.ssid.isEmpty()) {
            String name = demoNames.get(b.ssid);
            if (name == null) {
                final int n = demoNames.size(), count = DEMO_NAMES.length;
                name = DEMO_NAMES[(int) ((n + Long.remainderUnsigned(demoSalt, count)) % count)];
                if (n >= count) name += " " + (n / count + 1);
                demoNames.put(b.ssid, name);
            }
            b.ssid = name;
        }
        disguise(b.mac);
        if (b.multiLink) disguise(b.mld);
    }

    // ------------------------------------------------------------- the data
    public void refresh(Source source, long now) {
        adapter = source.adapterName();
        if (source.hasSixGhz()) hasSixGhz = true;
        for (Bss b : source.networks()) {
            if (demo) disguise(b);
            final long key = Bss.keyOf(b.mac);
            Entry e = entries.get(key);
            if (e == null) { e = new Entry(); entries.put(key, e); }
            if (e.color == 0) e.color = colorFor(key) | 1;
            final long heard = now - Math.min(now, b.ageMs);
            if (heard + 1000 < e.heard) continue;           // (an older copy)
            final boolean newer = heard > e.heard + 500 || e.history.isEmpty();
            // a report without the beacon's details keeps what was decoded before, and a
            // secured network is shown as Open only once three reports in a row say so
            if (!b.details && e.bss.details) {
                Bss kept = e.bss.clone();
                kept.rssi = b.rssi;
                kept.ageMs = b.ageMs;
                b = kept;
            } else if (b.security.equals("Open") && !e.bss.security.isEmpty() && !e.bss.security.equals("Open") &&
                       !e.bss.security.equals("?") && ++e.openReports < 3) {
                b.security = e.bss.security;
            } else if (!b.security.equals("Open")) {
                e.openReports = 0;
            }
            e.bss = b;
            e.heard = Math.max(e.heard, heard);
            if (newer) e.history.addLast(new long[] { heard, b.rssi });
            if (b.band == 6) hasSixGhz = true;
        }
        lastHeard = 0;
        for (Entry e : entries.values()) lastHeard = Math.max(lastHeard, e.heard);
        for (Iterator<Map.Entry<Long, Entry>> it = entries.entrySet().iterator(); it.hasNext();) {
            Map.Entry<Long, Entry> me = it.next();
            ArrayDeque<long[]> h = me.getValue().history;
            while (!h.isEmpty() && h.peekFirst()[0] + HISTORY_SECONDS * 1000L < now) h.pollFirst();
            final long heard = me.getValue().heard;
            if (heard + DROP_AFTER_MS < lastHeard || heard + FORGET_AFTER_MS < now) {
                if (selected == me.getKey()) selected = 0;
                it.remove();
            }
        }
        connected = source.connectedBssid();
        byte[] m = connectedMac();
        if (demo && m != null) {
            disguise(m);
            connected = Bss.macText(m);
        }

        // the makers; a made-up address that is a sibling of a registered one is that
        // router's extra network, otherwise a private address
        for (Entry e : entries.values()) {
            if (!e.vendor.isEmpty() && !Oui.local(e.bss.mac)) continue;
            if (!Oui.local(e.bss.mac)) { e.vendor = Oui.vendor(e.bss.mac); continue; }
            String maker = "";
            for (Entry other : entries.values())
                if (!Oui.local(other.bss.mac) && sibling(other.bss.mac, e.bss.mac)) {
                    maker = Oui.vendor(other.bss.mac);
                    if (!maker.isEmpty()) break;
                }
            e.vendor = maker.isEmpty() ? "(private address)" : maker + " (extra network)";
        }
    }

    // our own router: the radio we're connected to, and its siblings
    public boolean ours(Entry e) {
        byte[] m = connectedMac();
        return m != null && (Arrays.equals(m, e.bss.mac) || sibling(m, e.bss.mac));
    }

    public boolean isConnected(byte[] mac) {
        byte[] m = connectedMac();
        return m != null && Arrays.equals(m, mac);
    }

    public List<Advice.Choice> suggestions(int band) {
        List<Advice.Radio> radios = new ArrayList<>();
        for (Entry e : entries.values()) {
            if (ours(e)) continue;
            Advice.Radio r = new Advice.Radio();
            r.band = e.bss.band; r.center = e.bss.center; r.width = e.bss.width;
            r.center2 = e.bss.split ? e.bss.center2 : 0;
            if (e.bss.split) r.width = 80;
            r.rssi = e.bss.rssi; r.utilization = e.bss.utilization;
            radios.add(r);
        }
        return Advice.rank(band, radios);
    }

    // ------------------------------------------------------------- sorting and rows
    private static double value(Entry e, Column c) {
        final Bss s = e.bss;
        switch (c) {
            case SIGNAL: return s.rssi;
            case CHANNEL: return s.band * 1000 + s.channel;
            case WIDTH: return s.width;
            case BAND: return s.band;
            case WIFI: return s.generation * 10 + (s.band == 6 ? 1 : 0);
            case MAX: return s.maxRate;
            case MIN: return s.minRate;
            case CLIENTS: return s.stations;
            case UTIL: return s.utilization;
            case SEEN: return -(double) e.heard;
            default: return 0;
        }
    }

    private static int text(String p, String q) {        // (empty ones last)
        final String lp = p.toLowerCase(Locale.ROOT), lq = q.toLowerCase(Locale.ROOT);
        return lp.equals(lq) ? 0 : lp.isEmpty() ? 1 : lq.isEmpty() ? -1 : (lp.compareTo(lq) < 0 ? -1 : 1);
    }

    private int compare(Entry x, Entry y, long a, long b) {
        switch (sortColumn) {
            case SSID: return text(x.bss.ssid, y.bss.ssid);
            case BSSID: return Long.compare(a, b);
            case VENDOR: return text(x.vendor, y.vendor);
            case SECURITY: return text(x.bss.security, y.bss.security);
            default: return Double.compare(value(x, sortColumn), value(y, sortColumn));
        }
    }

    // <0 when x comes first
    private int order(Entry x, Entry y, long a, long b) {
        final int o = compare(x, y, a, b);
        if (o == 0) {
            if (x.bss.rssi != y.bss.rssi) return x.bss.rssi > y.bss.rssi ? -1 : 1;
            return Long.compare(a, b);
        }
        return sortDown ? -o : o;
    }

    public void setSort(Column c) {
        if (sortColumn == c) sortDown = !sortDown;
        else {
            sortColumn = c;
            sortDown = c == Column.SIGNAL || c == Column.MAX || c == Column.WIDTH || c == Column.WIFI ||
                       c == Column.CLIENTS || c == Column.UTIL;
        }
    }

    public void sortRows() {
        final String f = filter.toLowerCase(Locale.ROOT);
        shown.clear();
        for (Map.Entry<Long, Entry> me : entries.entrySet()) {
            final Entry e = me.getValue();
            if (!showBand[bandIndex(e.bss.band)]) continue;
            if (!f.isEmpty() && !e.bss.ssid.toLowerCase(Locale.ROOT).contains(f) && !Bss.macText(e.bss.mac).contains(f) &&
                !e.vendor.toLowerCase(Locale.ROOT).contains(f))
                continue;
            shown.add(me.getKey());
        }
        shown.sort((a, b) -> order(entries.get(a), entries.get(b), a, b));

        // the groups: radios joined when their addresses are siblings, or when they are
        // links of the same Wi-Fi 7 multi-link device (a union-find)
        final Map<Long, Long> parent = new HashMap<>();
        for (long k : shown) parent.put(k, k);
        if (grouped)
            for (int i = 0; i < shown.size(); ++i)
                for (int j = i + 1; j < shown.size(); ++j) {
                    final Bss p = entries.get(shown.get(i)).bss, q = entries.get(shown.get(j)).bss;
                    if (sibling(p.mac, q.mac) || (p.multiLink && q.multiLink && Arrays.equals(p.mld, q.mld))) {
                        final long a = find(parent, shown.get(i)), b = find(parent, shown.get(j));
                        if (a != b) parent.put(Math.max(a, b), Math.min(a, b));
                    }
                }
        groups.clear();
        groupOf.clear();
        final Map<Long, Integer> index = new LinkedHashMap<>();
        for (long k : shown) {
            final long root = find(parent, k);
            Integer at = index.get(root);
            if (at == null) {
                at = groups.size();
                index.put(root, at);
                Group g = new Group();
                g.id = root;
                groups.add(g);
            }
            groups.get(at).members.add(k);
            groupOf.put(k, root);
        }

        // a router's summary row: its strongest radio, with the others' channels, bands and names
        for (Group g : groups) {
            Entry s = entries.get(g.members.get(0)).copy();
            for (long k : g.members) if (entries.get(k).bss.rssi > s.bss.rssi) s = entries.get(k).copy();
            List<String> names = new ArrayList<>(), channels = new ArrayList<>();
            boolean[] band = new boolean[3];
            int stations = -1;
            for (long k : g.members) {
                final Entry e = entries.get(k);
                final Bss b = e.bss;
                if (!b.ssid.isEmpty() && !names.contains(b.ssid)) names.add(b.ssid);
                final String ch = Integer.toString(b.channel);
                if (!channels.contains(ch)) channels.add(ch);
                band[bandIndex(b.band)] = true;
                s.heard = Math.max(s.heard, e.heard);
                s.bss.width = Math.max(s.bss.width, b.width);
                s.bss.maxRate = Math.max(s.bss.maxRate, b.maxRate);
                s.bss.minRate = Math.min(s.bss.minRate, b.minRate);
                s.bss.utilization = Math.max(s.bss.utilization, b.utilization);
                if (b.generation > s.bss.generation) s.bss.generation = b.generation;
                if (b.stations >= 0) stations = Math.max(0, stations) + b.stations;
                if (Oui.local(s.bss.mac) && !Oui.local(b.mac)) { s.vendor = e.vendor; s.bss.mac = b.mac.clone(); }
            }
            s.bss.stations = stations;
            s.bss.ssid = names.isEmpty() ? "" : names.get(0);
            StringBuilder n = new StringBuilder(names.isEmpty() ? "(hidden)" : names.get(0));
            for (int i = 1; i < names.size() && i < 3; ++i) n.append(", ").append(names.get(i));
            if (names.size() > 3) n.append(" +").append(names.size() - 3);
            g.names = n.toString();
            g.channels = String.join(", ", channels);
            StringBuilder bs = new StringBuilder();
            for (int i = 0; i < 3; ++i) if (band[i]) { if (bs.length() > 0) bs.append('+'); bs.append(bandName(i == 0 ? 2 : i == 1 ? 5 : 6)); }
            g.bands = bs.toString();
            g.summary = s;
        }
        groups.sort((a, b) -> order(a.summary, b.summary, a.id, b.id));   // (List.sort is stable)

        rows.clear();
        for (int i = 0; i < groups.size(); ++i) {
            final Group g = groups.get(i);
            if (g.members.size() == 1) { rows.add(new Row(i, g.members.get(0), false)); continue; }
            rows.add(new Row(i, g.id, true));
            if (opened.contains(g.id)) for (long k : g.members) rows.add(new Row(i, k, false));
        }
        if (selectedGroup != 0) {
            boolean still = false;
            for (Group g : groups) if (g.id == selectedGroup && g.members.size() > 1) still = true;
            if (!still) selectedGroup = 0;
        }
    }

    private static long find(Map<Long, Long> parent, long k) {
        long root = k;
        while (parent.get(root) != root) root = parent.get(root);
        while (parent.get(k) != root) { long next = parent.get(k); parent.put(k, root); k = next; }
        return root;
    }

    public boolean highlighted(long key) {
        if (key == selected) return true;
        if (selectedGroup == 0) return false;
        Long g = groupOf.get(key);
        return g != null && g == selectedGroup;
    }

    public boolean anySelection() { return selected != 0 || selectedGroup != 0; }

    public Entry entryOf(Row r) { return r.summary ? groups.get(r.group).summary : entries.get(r.key); }

    // ------------------------------------------------------------- copying
    public static final String HEADER = "SSID\tBSSID\tVendor\tdBm\tChannel\tWidth\tBand\tWi-Fi\tSecurity\tMax Mbps\tPHY\n";

    public static String rowText(Entry e) {
        final Bss b = e.bss;
        return String.join("\t", b.ssid, Bss.macText(b.mac), e.vendor, Integer.toString(b.rssi), Integer.toString(b.channel),
                b.split ? "80+80" : Integer.toString(b.width), bandName(b.band), wifiName(b), b.security,
                rateText(b.maxRate), b.phy);
    }

    public String allRowsText() {
        StringBuilder s = new StringBuilder(HEADER);
        for (long k : shown) s.append(rowText(entries.get(k))).append('\n');
        return s.toString();
    }

    // not in the latest scans (see lastHeard)
    public boolean faded(Entry e) { return e.heard + FADE_AFTER_MS < lastHeard; }

    public int routersHeard() {
        int n = 0;
        for (Group g : groups) if (!faded(g.summary)) ++n;
        return n;
    }

    public int networksHeard() {
        int n = 0;
        for (Entry e : entries.values()) if (!faded(e)) ++n;
        return n;
    }
}
