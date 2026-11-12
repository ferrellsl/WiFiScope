package com.ferrellsl.wifiscope;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import java.io.ByteArrayOutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

import org.junit.Test;

// The Windows app's tests/decode_test.cpp checks, on the Java port.
public class DecoderTest {

    static final class Ies {
        final ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        Ies add(int id, int... data) {
            bytes.write(id);
            bytes.write(data.length);
            for (int v : data) bytes.write(v);
            return this;
        }
        Ies raw(int... data) { for (int v : data) bytes.write(v); return this; }
        byte[] get() { return bytes.toByteArray(); }
    }

    static Bss decoded(Ies ies, int band, int channel, int capabilities) {
        Bss b = new Bss();
        b.band = band;
        b.channel = channel;
        b.frequency = Decoder.frequencyOf(band, channel);
        Decoder.decode(ies.get(), capabilities, b);
        return b;
    }
    static Bss decoded(Ies ies, int band, int channel) { return decoded(ies, band, channel, 0x0011); }

    // RSN with the given AKM suite types (00-0F-AC-n), CCMP
    static Ies rsn(Ies i, int... akms) {
        List<Integer> d = new ArrayList<>(Arrays.asList(1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4, akms.length, 0));
        for (int a : akms) { d.add(0x00); d.add(0x0F); d.add(0xAC); d.add(a); }
        d.add(0); d.add(0);
        i.bytes.write(48);
        i.bytes.write(d.size());
        for (int v : d) i.bytes.write(v);
        return i;
    }

    static Ies heCaps(Ies i) {
        int[] caps = new int[1 + 6 + 11 + 4];
        caps[0] = 35;
        caps[18] = 0xFA; caps[19] = 0xFF;                    // Rx MCS <= 80: 2 streams, MCS 0-11
        i.bytes.write(255); i.bytes.write(caps.length);
        for (int v : caps) i.bytes.write(v);
        return i;
    }

    @Test public void ht24() {
        Ies i = new Ies();
        i.add(1, 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24)
         .add(45, 0, 0, 0, 0xFF, 0xFF, 0, 0)
         .add(61, 6, 0x05, 0, 0, 0)
         .add(11, 12, 0, 128, 0, 0);
        rsn(i, 2);
        Bss b = decoded(i, 2, 6);
        assertTrue("2.4 GHz HT 40 MHz above channel 6: center 2447 MHz", b.width == 40 && b.center == 2447);
        assertTrue("802.11b/g/n: Wi-Fi 4", b.generation == 4 && b.phy.equals("b, g, n"));
        assertTrue("two streams at 40 MHz: 300 Mbps", b.streams == 2 && b.maxRate == 300.0);
        assertTrue("lowest basic rate 1 Mbps", b.minRate == 1.0);
        assertEquals("WPA2", b.security);
        assertTrue("BSS load: 12 stations, 50% busy", b.stations == 12 && b.utilization == 50);
    }

    @Test public void vht80() {
        Ies i = new Ies();
        i.add(1, 0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C)
         .add(45, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF)
         .add(61, 36, 0x05, 0, 0, 0)
         .add(191, 0, 0, 0, 0, 0xAA, 0xFF, 0, 0, 0xAA, 0xFF, 0, 0)
         .add(192, 1, 42, 0, 0, 0);
        rsn(i, 2, 8);
        Bss b = decoded(i, 5, 36);
        assertTrue("VHT 80 MHz, CCFS0 42: center 5210 MHz", b.width == 80 && b.center == 5210);
        assertEquals(5, b.generation);
        assertTrue("four streams at 80 MHz: 1733.3 Mbps", b.streams == 4 && b.maxRate > 1733 && b.maxRate < 1734);
        assertEquals("WPA2/WPA3", b.security);
        assertTrue("lowest basic rate 6 Mbps", b.minRate == 6.0);
    }

    @Test public void vht160() {
        Ies i = new Ies();
        i.add(191, 0, 0, 0, 0, 0xFA, 0xFF, 0, 0, 0xFA, 0xFF, 0, 0).add(192, 1, 42, 50, 0, 0);
        Bss b = decoded(i, 5, 36);
        assertTrue("VHT 160 MHz (CCFS1 50): center 5250 MHz", b.width == 160 && b.center == 5250 && !b.split);
    }

    @Test public void vht80plus80() {
        Ies i = new Ies();
        i.add(191, 0, 0, 0, 0, 0xFA, 0xFF, 0, 0, 0xFA, 0xFF, 0, 0).add(192, 1, 42, 155, 0, 0);
        Bss b = decoded(i, 5, 36);
        assertTrue("VHT 80+80: parts at 5210 and 5775 MHz", b.width == 160 && b.split && b.center == 5210 && b.center2 == 5775);
    }

    @Test public void he6ghz160() {
        Ies i = heCaps(new Ies());
        i.add(255, 36, 0x00, 0x00, 0x02, 0, 0, 0, 37, 0x03, 39, 47, 6);
        rsn(i, 8);
        Bss b = decoded(i, 6, 37, 0x0011);
        assertTrue("6 GHz HE 160 MHz, CCFS1 47: center 6185 MHz", b.width == 160 && b.center == 6185);
        assertTrue("6 GHz HE only: Wi-Fi 6E, PHY ax", b.generation == 6 && b.phy.equals("ax"));
        assertEquals("WPA3", b.security);
        assertTrue("two streams at 160 MHz HE: 2402 Mbps", b.streams == 2 && b.maxRate > 2401 && b.maxRate < 2403);
    }

    @Test public void eht320multiLink() {
        Ies i = heCaps(new Ies());
        i.add(255, 36, 0x00, 0x00, 0x02, 0, 0, 0, 37, 0x03, 39, 47, 6)
         .add(255, 108, 0, 0, 0)
         .add(255, 106, 0x01, 0, 0, 0, 0, 0x04, 47, 63)
         .add(255, 107, 0x00, 0x00, 7, 0x02, 0x11, 0x22, 0x33, 0x44, 0x55);
        rsn(i, 24);
        Bss b = decoded(i, 6, 37);
        assertTrue("EHT 320 MHz, CCFS1 63: center 6265 MHz", b.width == 320 && b.center == 6265);
        assertTrue("EHT: Wi-Fi 7", b.generation == 7 && b.phy.equals("ax, be"));
        assertTrue("multi-link element: MLD 02:11:22:33:44:55", b.multiLink && b.mld[0] == 0x02 && b.mld[5] == 0x55);
        assertEquals("WPA3", b.security);
        assertTrue("two streams at 320 MHz EHT: 5764.8 Mbps", b.maxRate > 5764 && b.maxRate < 5765);
    }

    @Test public void security() {
        Ies i = new Ies().add(1, 0x82, 0x84);
        assertEquals("Open", decoded(i, 2, 1, 0x0001).security);
        assertEquals("WEP", decoded(i, 2, 1, 0x0011).security);
        assertEquals("WPA", decoded(new Ies().add(221, 0x00, 0x50, 0xF2, 1, 1, 0), 2, 1).security);
        assertEquals("WPA2-Enterprise", decoded(rsn(new Ies(), 1), 5, 36).security);
        assertEquals("OWE", decoded(rsn(new Ies(), 18), 5, 36).security);
    }

    @Test public void malformed() {
        Ies i = new Ies().add(192, 1).add(61).add(255).raw(45, 200);
        assertEquals("short and truncated elements: ignored, 20 MHz", 20, decoded(i, 5, 36).width);
    }

    @Test public void frequencies() {
        assertTrue(Decoder.frequencyOf(2, 1) == 2412 && Decoder.frequencyOf(2, 14) == 2484);
        assertTrue(Decoder.frequencyOf(5, 36) == 5180 && Decoder.frequencyOf(5, 165) == 5825);
        assertTrue(Decoder.frequencyOf(6, 1) == 5955 && Decoder.frequencyOf(6, 2) == 5935 && Decoder.frequencyOf(6, 233) == 7115);
        assertEquals(6, Decoder.channelOf(2437));
        assertEquals(52, Decoder.channelOf(5260));
        assertEquals(37, Decoder.channelOf(6135));
    }

    static Advice.Radio radio(int band, int channel, int width, int rssi, int util) {
        Advice.Radio r = new Advice.Radio();
        r.band = band; r.width = width; r.rssi = rssi; r.utilization = util;
        r.center = Advice.mhz(band, channel);
        return r;
    }
    static Advice.Radio radio(int band, int channel, int width, int rssi) { return radio(band, channel, width, rssi, -1); }
    static Advice.Choice best(int band, Advice.Radio... v) { return Advice.rank(band, Arrays.asList(v)).get(0); }

    @Test public void quietest() {
        assertEquals(6, best(2, radio(2, 1, 20, -50), radio(2, 11, 20, -50), radio(2, 6, 20, -85)).first);
        assertEquals(1, best(2).first);
        assertEquals(11, best(2, radio(2, 3, 20, -55)).first);
        List<Advice.Choice> order = Advice.rank(2, Arrays.asList(radio(2, 1, 20, -60, 90), radio(2, 6, 20, -60, 10), radio(2, 11, 20, -60, 50)));
        assertTrue(order.get(0).first == 6 && order.get(1).first == 11 && order.get(2).first == 1);
        order = Advice.rank(5, Arrays.asList(radio(5, 42, 80, -60), radio(5, 155, 80, -60)));
        boolean firstPlain = false;
        for (Advice.Choice c : order) if (!c.dfs) { firstPlain = c.crowding > 0; break; }
        assertTrue(order.get(0).dfs && order.get(0).first == 52 && firstPlain);
        assertEquals(36, best(5, radio(5, 155, 80, -80)).first);
        Advice.Radio r = radio(5, 42, 80, -55);
        r.center2 = Advice.mhz(5, 155);
        order = Advice.rank(5, Arrays.asList(r));
        int last = order.get(order.size() - 1).first;
        assertTrue(last == 36 || last == 149);
        assertTrue(order.get(order.size() - 1).crowding > 0 && order.get(order.size() - 2).crowding > 0);
        assertEquals(17, best(6, radio(6, 7, 80, -50)).first);
    }

    @Test public void ssidElement() {
        Ies i = new Ies().add(0, 'b', 'l', 'u', 'e').add(1, 0x82);
        assertEquals("blue", Decoder.ssidOf(i.get()));
        assertEquals("", Decoder.ssidOf(new Ies().add(0, 0, 0, 0).get()));
        assertEquals("café", Decoder.ssidOf(new Ies().add(0, 'c', 'a', 'f', 0xC3, 0xA9).get()));
        assertEquals("é", Decoder.ssidOf(new Ies().add(0, 0xE9).get()));   // (not UTF-8: Latin-1)
    }
}
