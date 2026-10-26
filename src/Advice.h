// WiFiScope: which channel would be quietest for your own router, from the
// networks heard (yours left out).  Plain functions, tested in tests/decode_test.cpp.
#pragma once
#include <algorithm>
#include <vector>

namespace advice {

  // a radio heard: where its channel lies (MHz) and how loud and busy it is
  struct Radio
  {
    int band = 2;              // 2, 5 or 6
    int center = 0, width = 20;
    int center2 = 0;           // the second part of an 80+80 MHz channel (0: none)
    int rssi = -100;
    int utilization = -1;      // percent, -1 when not reported
  };

  // a channel your router could use, and how crowded it is (lower is better)
  struct Choice
  {
    int first = 0, last = 0;   // its 20 MHz channels, e.g. 149 and 161 for 80 MHz around 155
    int center = 0, width = 20;
    bool dfs = false;          // 5 GHz channels 52-144: shared with radar
    double crowding = 0;
  };

  inline int mhz(int band, int channel)
  {
    if (band == 2) return channel == 14 ? 2484 : 2407 + 5 * channel;
    if (band == 6) return channel == 2 ? 5935 : 5950 + 5 * channel;
    return 5000 + 5 * channel;
  }

  // How much a radio crowds a channel: its loudness above the noise (squared,
  // so a near neighbor counts far more than a faint one), times the share of
  // the channel its own channel covers, times how busy it says it is.
  inline double crowding(const Choice & c, const std::vector<Radio> & radios)
  {
    const double a0 = c.center - c.width / 2.0, a1 = c.center + c.width / 2.0;
    double total = 0;
    for (const Radio & r : radios) {
      const double loud = std::max(0, r.rssi + 95);
      double covered = 0;
      const int parts = r.center2 ? 2 : 1;
      for (int p = 0; p < parts; ++p) {
        const int center = p ? r.center2 : r.center, width = r.center2 ? 80 : r.width;
        const double b0 = center - width / 2.0, b1 = center + width / 2.0;
        covered += std::max(0.0, std::min(a1, b1) - std::max(a0, b0));
      }
      const double share = std::min(1.0, covered / c.width);
      const double busy = r.utilization >= 0 ? 0.5 + r.utilization / 100.0 : 1.0;
      total += loud * loud * share * busy;
    }
    return total;
  }

  // The channels for a band, quietest first: 2.4 GHz 1, 6 and 11 (20 MHz, the
  // ones that don't overlap); 5 and 6 GHz in 80 MHz blocks.  Radar (DFS)
  // channels count a little more crowded, as routers must leave them when
  // radar is heard; ties go to the lower channel.
  inline std::vector<Choice> rank(int band, const std::vector<Radio> & heard)
  {
    std::vector<Radio> radios;
    for (const Radio & r : heard) if (r.band == band) radios.push_back(r);
    std::vector<Choice> out;
    auto add = [&](int first, int last, int width, bool dfs) {
      Choice c;
      c.first = first; c.last = last; c.width = width; c.dfs = dfs;
      c.center = (mhz(band, first) + mhz(band, last)) / 2;
      c.crowding = crowding(c, radios) * (dfs ? 1.15 : 1.0);
      out.push_back(c);
    };
    if (band == 2) { add(1, 1, 20, false); add(6, 6, 20, false); add(11, 11, 20, false); }
    else if (band == 5) {
      add(36, 48, 80, false);
      for (int first : { 52, 100, 116, 132 }) add(first, first + 12, 80, true);
      add(149, 161, 80, false);
    }
    else for (int first = 1; first + 12 <= 233; first += 16) add(first, first + 12, 80, false);
    std::stable_sort(out.begin(), out.end(), [](const Choice & a, const Choice & b) { return a.crowding < b.crowding - 1e-9; });
    return out;
  }
}
