// Checks of the beacon decoder (wlan::decode) on hand-made information elements.
// A console program: prints each check, and exits with the number of failures.
#include "../src/Advice.h"
#include "../src/Wlan.h"

#include <cstdio>
#include <initializer_list>
#include <vector>

static int failures = 0;

static void check(bool ok, const char * what)
{
  printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++failures;
}

struct Ies
{
  std::vector<uint8_t> bytes;
  Ies & add(uint8_t id, std::initializer_list<uint8_t> data)
  {
    bytes.push_back(id);
    bytes.push_back((uint8_t) data.size());
    bytes.insert(bytes.end(), data);
    return *this;
  }
};

static wlan::Bss decoded(const Ies & ies, int band, int channel, uint16_t capabilities = 0x0011)
{
  wlan::Bss b;
  b.band = band;
  b.channel = channel;
  b.frequency = wlan::frequencyOf(band, channel);
  wlan::decode(ies.bytes.data(), ies.bytes.size(), capabilities, b);
  return b;
}

// RSN with the given AKM suite types (00-0F-AC-n), CCMP
static Ies & rsn(Ies & i, std::initializer_list<uint8_t> akms)
{
  std::vector<uint8_t> d = { 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4, (uint8_t) akms.size(), 0 };
  for (uint8_t a : akms) { d.push_back(0x00); d.push_back(0x0F); d.push_back(0xAC); d.push_back(a); }
  d.push_back(0); d.push_back(0);   // (RSN capabilities)
  i.bytes.push_back(48);
  i.bytes.push_back((uint8_t) d.size());
  i.bytes.insert(i.bytes.end(), d.begin(), d.end());
  return i;
}

int main()
{
  // 2.4 GHz, 802.11n, 40 MHz above channel 6, two streams, WPA2-Personal
  {
    Ies i;
    i.add(1, { 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24 })        // 1 2 5.5 11 (basic), 6 9 12 18
     .add(45, { 0, 0, 0, 0xFF, 0xFF, 0, 0 })                             // HT caps: MCS 0-15
     .add(61, { 6, 0x05, 0, 0, 0 })                                     // HT op: secondary above, any width
     .add(11, { 12, 0, 128, 0, 0 });                                    // BSS load: 12 stations, 128/255
    rsn(i, { 2 });
    const wlan::Bss b = decoded(i, 2, 6);
    check(b.width == 40 && b.center == 2447, "2.4 GHz HT 40 MHz above channel 6: center 2447 MHz");
    check(b.generation == 4 && b.phy == L"b, g, n", "802.11b/g/n: Wi-Fi 4");
    check(b.streams == 2 && b.maxRate == 300.0, "two streams at 40 MHz: 300 Mbps");
    check(b.minRate == 1.0, "lowest basic rate 1 Mbps");
    check(b.security == L"WPA2", "AKM PSK: WPA2");
    check(b.stations == 12 && b.utilization == 50, "BSS load: 12 stations, 50% busy");
  }
  // 5 GHz, 802.11ac, 80 MHz on 36-48, four streams, WPA2/WPA3 transition
  {
    Ies i;
    i.add(1, { 0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C })        // 6 (basic) ...
     .add(45, { 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF })
     .add(61, { 36, 0x05, 0, 0, 0 })
     .add(191, { 0, 0, 0, 0, 0xAA, 0xFF, 0, 0, 0xAA, 0xFF, 0, 0 })      // VHT caps: 4 streams, MCS 0-9
     .add(192, { 1, 42, 0, 0, 0 });                                     // VHT op: 80 MHz around 42
    rsn(i, { 2, 8 });
    const wlan::Bss b = decoded(i, 5, 36);
    check(b.width == 80 && b.center == 5210, "VHT 80 MHz, CCFS0 42: center 5210 MHz");
    check(b.generation == 5, "VHT: Wi-Fi 5");
    check(b.streams == 4 && b.maxRate > 1733 && b.maxRate < 1734, "four streams at 80 MHz: 1733.3 Mbps");
    check(b.security == L"WPA2/WPA3", "AKM PSK + SAE: WPA2/WPA3");
    check(b.minRate == 6.0, "lowest basic rate 6 Mbps");
  }
  // 5 GHz VHT 160 MHz (CCFS0 50 = the 80 with the primary, CCFS1 = 160 center... the newer way: CCFS0 42, CCFS1 50)
  {
    Ies i;
    i.add(191, { 0, 0, 0, 0, 0xFA, 0xFF, 0, 0, 0xFA, 0xFF, 0, 0 })      // 2 streams
     .add(192, { 1, 42, 50, 0, 0 });
    const wlan::Bss b = decoded(i, 5, 36);
    check(b.width == 160 && b.center == 5250 && !b.split, "VHT 160 MHz (CCFS1 50): center 5250 MHz");
  }
  // 5 GHz VHT 80+80
  {
    Ies i;
    i.add(191, { 0, 0, 0, 0, 0xFA, 0xFF, 0, 0, 0xFA, 0xFF, 0, 0 })
     .add(192, { 1, 42, 155, 0, 0 });
    const wlan::Bss b = decoded(i, 5, 36);
    check(b.width == 160 && b.split && b.center == 5210 && b.center2 == 5775, "VHT 80+80: parts at 5210 and 5775 MHz");
  }
  // 6 GHz, 802.11ax, 160 MHz from the 6 GHz operation information, WPA3, 2 streams
  {
    Ies i;
    std::vector<uint8_t> heCaps(1 + 6 + 11 + 4, 0);
    heCaps[0] = 35;
    heCaps[18] = 0xFA; heCaps[19] = 0xFF;                               // Rx MCS <= 80: 2 streams, MCS 0-11
    i.bytes.push_back(255); i.bytes.push_back((uint8_t) heCaps.size());
    i.bytes.insert(i.bytes.end(), heCaps.begin(), heCaps.end());
    i.add(255, { 36, 0x00, 0x00, 0x02, 0, 0, 0, 37, 0x03, 39, 47, 6 }); // HE op: 6 GHz info, 160 MHz
    rsn(i, { 8 });
    const wlan::Bss b = decoded(i, 6, 37, 0x0011);
    check(b.width == 160 && b.center == 6185, "6 GHz HE 160 MHz, CCFS1 47: center 6185 MHz");
    check(b.generation == 6 && b.phy == L"ax", "6 GHz HE only: Wi-Fi 6E, PHY ax");
    check(b.security == L"WPA3", "AKM SAE: WPA3");
    check(b.streams == 2 && b.maxRate > 2401 && b.maxRate < 2403, "two streams at 160 MHz HE: 2402 Mbps");
  }
  // 6 GHz, 802.11be, 320 MHz, multi-link
  {
    Ies i;
    std::vector<uint8_t> heCaps(1 + 6 + 11 + 4, 0);
    heCaps[0] = 35;
    heCaps[18] = 0xFA; heCaps[19] = 0xFF;
    i.bytes.push_back(255); i.bytes.push_back((uint8_t) heCaps.size());
    i.bytes.insert(i.bytes.end(), heCaps.begin(), heCaps.end());
    i.add(255, { 36, 0x00, 0x00, 0x02, 0, 0, 0, 37, 0x03, 39, 47, 6 })
     .add(255, { 108, 0, 0, 0 })                                        // EHT caps
     .add(255, { 106, 0x01, 0, 0, 0, 0, 0x04, 47, 63 })                 // EHT op: 320 MHz, CCFS1 63
     .add(255, { 107, 0x00, 0x00, 7, 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 });   // multi-link: MLD 02:11:22:33:44:55
    rsn(i, { 24 });
    const wlan::Bss b = decoded(i, 6, 37);
    check(b.width == 320 && b.center == 6265, "EHT 320 MHz, CCFS1 63: center 6265 MHz");
    check(b.generation == 7 && b.phy == L"ax, be", "EHT: Wi-Fi 7");
    check(b.multiLink && b.mld[0] == 0x02 && b.mld[5] == 0x55, "multi-link element: MLD address 02:11:22:33:44:55");
    check(b.security == L"WPA3", "AKM SAE-EXT-KEY: WPA3");
    check(b.maxRate > 5764 && b.maxRate < 5765, "two streams at 320 MHz EHT: 5764.8 Mbps");
  }
  // open, WEP, WPA (1), enterprise, OWE
  {
    Ies i; i.add(1, { 0x82, 0x84 });
    check(decoded(i, 2, 1, 0x0001).security == L"Open", "no RSN, no privacy bit: Open");
    check(decoded(i, 2, 1, 0x0011).security == L"WEP", "no RSN, privacy bit: WEP");
    Ies w; w.add(221, { 0x00, 0x50, 0xF2, 1, 1, 0 });
    check(decoded(w, 2, 1).security == L"WPA", "Microsoft WPA element only: WPA");
    Ies e; rsn(e, { 1 });
    check(decoded(e, 5, 36).security == L"WPA2-Enterprise", "AKM 802.1X: WPA2-Enterprise");
    Ies o; rsn(o, { 18 });
    check(decoded(o, 5, 36).security == L"OWE", "AKM OWE: OWE (Enhanced Open)");
  }
  // truncated and malformed elements don't read past the end
  {
    Ies i; i.add(192, { 1 }).add(61, {}).add(255, {});
    i.bytes.push_back(45); i.bytes.push_back(200);                      // (claims more than there is)
    const wlan::Bss b = decoded(i, 5, 36);
    check(b.width == 20, "short and truncated elements: ignored, 20 MHz");
  }
  // channel numbers <-> MHz
  check(wlan::frequencyOf(2, 1) == 2412 && wlan::frequencyOf(2, 14) == 2484, "2.4 GHz channels 1 and 14");
  check(wlan::frequencyOf(5, 36) == 5180 && wlan::frequencyOf(5, 165) == 5825, "5 GHz channels 36 and 165");
  check(wlan::frequencyOf(6, 1) == 5955 && wlan::frequencyOf(6, 2) == 5935 && wlan::frequencyOf(6, 233) == 7115, "6 GHz channels 1, 2 and 233");

  // the quietest channel (advice::rank)
  {
    auto radio = [](int band, int channel, int width, int rssi, int util = -1) {
      advice::Radio r;
      r.band = band; r.width = width; r.rssi = rssi; r.utilization = util;
      r.center = advice::mhz(band, channel);
      return r;
    };
    using V = std::vector<advice::Radio>;
    auto best = [](int band, const V & v) { return advice::rank(band, v).front(); };
    check(best(2, V{ radio(2, 1, 20, -50), radio(2, 11, 20, -50), radio(2, 6, 20, -85) }).first == 6,
          "2.4 GHz: loud neighbors on 1 and 11, a faint one on 6: 6");
    check(best(2, V{}).first == 1, "2.4 GHz, nothing heard: 1 (ties go to the lower channel)");
    check(best(2, V{ radio(2, 3, 20, -55) }).first == 11, "2.4 GHz: a neighbor on 3 overlaps 1 and 6: 11");
    {
      auto order = advice::rank(2, V{ radio(2, 1, 20, -60, 90), radio(2, 6, 20, -60, 10), radio(2, 11, 20, -60, 50) });
      check(order[0].first == 6 && order[1].first == 11 && order[2].first == 1, "2.4 GHz, equal signals: the least busy first (6, 11, 1)");
    }
    {
      auto order = advice::rank(5, V{ radio(5, 42, 80, -60), radio(5, 155, 80, -60) });
      bool firstPlain = false;
      for (auto & c : order) if (!c.dfs) { firstPlain = c.crowding > 0; break; }
      check(order[0].dfs && order[0].first == 52 && firstPlain, "5 GHz: 36-48 and 149-161 taken: 52-64 (radar rules), the ordinary ones crowded");
    }
    check(best(5, V{ radio(5, 155, 80, -80) }).first == 36, "5 GHz: only a faint 149-161 neighbor: 36-48");
    {
      advice::Radio r = radio(5, 42, 80, -55);
      r.center2 = advice::mhz(5, 155);
      auto order = advice::rank(5, V{ r });
      check(order.back().first == 36 || order.back().first == 149, "5 GHz 80+80 neighbor: both of its parts crowded");
      check(order[order.size() - 1].crowding > 0 && order[order.size() - 2].crowding > 0, "80+80: two blocks crowded, not one");
    }
    check(best(6, V{ radio(6, 7, 80, -50) }).first == 17, "6 GHz: a neighbor on 1-13: 17-29");
  }

  printf("%d failure(s)\n", failures);
  return failures;
}
