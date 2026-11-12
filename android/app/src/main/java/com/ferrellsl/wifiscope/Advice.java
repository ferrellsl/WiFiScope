package com.ferrellsl.wifiscope;

import java.util.ArrayList;
import java.util.List;

// Which channel would be quietest for your own router, from the networks heard
// (yours left out) - a port of src/Advice.h, checked in DecoderTest.
public final class Advice {
    private Advice() {}

    // a radio heard: where its channel lies (MHz) and how loud and busy it is
    public static final class Radio {
        public int band = 2, center = 0, width = 20, center2 = 0, rssi = -100, utilization = -1;
    }

    // a channel your router could use, and how crowded it is (lower is better)
    public static final class Choice {
        public int first, last, center, width = 20;
        public boolean dfs;
        public double crowding;
    }

    public static int mhz(int band, int channel) { return Decoder.frequencyOf(band, channel); }

    // loudness above the noise (squared), times the share of the channel covered, times how busy
    public static double crowding(Choice c, List<Radio> radios) {
        final double a0 = c.center - c.width / 2.0, a1 = c.center + c.width / 2.0;
        double total = 0;
        for (Radio r : radios) {
            final double loud = Math.max(0, r.rssi + 95);
            double covered = 0;
            final int parts = r.center2 != 0 ? 2 : 1;
            for (int p = 0; p < parts; ++p) {
                final int center = p != 0 ? r.center2 : r.center, width = r.center2 != 0 ? 80 : r.width;
                final double b0 = center - width / 2.0, b1 = center + width / 2.0;
                covered += Math.max(0.0, Math.min(a1, b1) - Math.max(a0, b0));
            }
            final double share = Math.min(1.0, covered / c.width);
            final double busy = r.utilization >= 0 ? 0.5 + r.utilization / 100.0 : 1.0;
            total += loud * loud * share * busy;
        }
        return total;
    }

    // The channels for a band, quietest first: 2.4 GHz 1, 6 and 11; 5 and 6 GHz in
    // 80 MHz blocks.  Radar (DFS) channels count a little more crowded; ties go to
    // the lower channel.
    public static List<Choice> rank(int band, List<Radio> heard) {
        List<Radio> radios = new ArrayList<>();
        for (Radio r : heard) if (r.band == band) radios.add(r);
        List<Choice> out = new ArrayList<>();
        if (band == 2) { add(out, radios, band, 1, 1, 20, false); add(out, radios, band, 6, 6, 20, false); add(out, radios, band, 11, 11, 20, false); }
        else if (band == 5) {
            add(out, radios, band, 36, 48, 80, false);
            for (int first : new int[] { 52, 100, 116, 132 }) add(out, radios, band, first, first + 12, 80, true);
            add(out, radios, band, 149, 161, 80, false);
        }
        else for (int first = 1; first + 12 <= 233; first += 16) add(out, radios, band, first, first + 12, 80, false);
        // (a stable insertion sort with the C++ version's comparison: a before b when clearly less crowded)
        for (int i = 1; i < out.size(); ++i) {
            Choice c = out.get(i);
            int j = i - 1;
            while (j >= 0 && c.crowding < out.get(j).crowding - 1e-9) { out.set(j + 1, out.get(j)); --j; }
            out.set(j + 1, c);
        }
        return out;
    }

    private static void add(List<Choice> out, List<Radio> radios, int band, int first, int last, int width, boolean dfs) {
        Choice c = new Choice();
        c.first = first; c.last = last; c.width = width; c.dfs = dfs;
        c.center = (mhz(band, first) + mhz(band, last)) / 2;
        c.crowding = crowding(c, radios) * (dfs ? 1.15 : 1.0);
        out.add(c);
    }
}
