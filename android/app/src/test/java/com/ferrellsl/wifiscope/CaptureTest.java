package com.ferrellsl.wifiscope;

import static org.junit.Assert.assertEquals;
import static org.junit.Assume.assumeTrue;

import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

import org.junit.Test;

// Real beacons recorded on Windows by tools/capture.exe (in android/captures/,
// not in the repository): the Java decoder must make of each one exactly what
// the Windows decoder made of it.  Skipped when there is no recording.
public class CaptureTest {

    @Test public void sameAsWindows() throws Exception {
        File dir = new File("../captures");
        File[] files = dir.listFiles((d, n) -> n.endsWith(".txt"));
        assumeTrue("no recorded scans in android/captures", files != null && files.length > 0);
        List<String> differences = new ArrayList<>();
        int checked = 0;
        for (File f : files) {
            for (Capture.Line l : Capture.read(f).lines) {
                final Bss b = Capture.decode(l);
                final String[] w = l.windows;
                String ssidHex = w[0];
                String windowsSsid = Decoder.ssidText(Capture.hex(ssidHex), 0, ssidHex.length() / 2);
                String[] mine = {
                    b.ssid, Integer.toString(b.width), Integer.toString(b.center), b.split ? "1" : "0",
                    Integer.toString(b.split ? b.center2 : 0), Integer.toString(b.generation), b.phy, b.security,
                    Integer.toString(b.streams), String.format(Locale.US, "%.1f", b.maxRate), String.format(Locale.US, "%.1f", b.minRate),
                    Integer.toString(b.stations), Integer.toString(b.utilization), b.multiLink ? "1" : "0", Bss.macText(b.mld),
                    b.country, b.details ? "1" : "0",
                };
                String[] theirs = w.clone();
                theirs[0] = windowsSsid;
                final String[] names = { "ssid", "width", "center", "split", "center2", "generation", "phy", "security", "streams",
                                         "maxRate", "minRate", "stations", "utilization", "multiLink", "mld", "country", "details" };
                for (int i = 0; i < names.length; ++i)
                    if (!mine[i].equals(theirs[i]))
                        differences.add(f.getName() + " " + l.bssid + " " + names[i] + ": Android '" + mine[i] + "', Windows '" + theirs[i] + "'");
                ++checked;
            }
        }
        System.out.println("checked " + checked + " recorded networks, " + differences.size() + " differences");
        for (String d : differences) System.out.println("  " + d);
        assertEquals(String.join("\n", differences), 0, differences.size());
    }
}
