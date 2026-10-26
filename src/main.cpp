// WiFiScope: a Wi-Fi scanner for Windows - every network the adapter hears, in
// a table and on graphs of the 2.4, 5 and 6 GHz channels, or of their signal
// over the last five minutes.
//
// Scanning: Windows' Native Wifi API (Wlan.cpp) asks for a scan every few
// seconds; what each network's beacon says (width, generation, security,
// rates, load, Wi-Fi 7 multi-link) is decoded there.  Drawing: Direct2D and
// DirectWrite, in pixels; `ui` scales what needs it to the monitor's DPI.

#define _CRT_RAND_S                        // (rand_s, for the demo mode's salt)
#include <windows.h>
#include <windowsx.h>
#include <shlobj.h>
#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "Advice.h"
#include "Oui.h"
#include "Wlan.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")

namespace {

  const UINT WM_SCANNED = WM_APP + 1;
  const UINT_PTR TimerScan = 1, TimerRefresh = 2;
  const int ScanEveryMs = 5000;            // (Windows won't scan much more often anyway)
  const int RefreshEveryMs = 1000;
  const int FadeAfterMs = 30000;           // not heard for this long: drawn faded
  const int DropAfterMs = 120000;          // ... and then gone from the list
  const int HistorySeconds = 300;
  const int GapMs = 25000;                 // a signal line breaks where nothing was heard for this long

  // ------------------------------------------------------------- state
  struct Entry
  {
    wlan::Bss bss;
    ULONGLONG heard = 0;                   // GetTickCount64() of its last beacon
    uint32_t color = 0;
    std::wstring vendor;                   // the maker, from the IEEE registry (or what made a made-up address)
    std::deque<std::pair<ULONGLONG, int>> history;   // (time, dBm)
  };
  std::map<uint64_t, Entry> entries;       // key: the BSSID
  std::wstring connected;                  // our network's BSSID
  std::wstring adapter;

  // The table's rows.  Grouped by router, a router with several radios (its
  // bands, its extra networks, the links of a Wi-Fi 7 multi-link device) gets
  // a summary row, with its radios under it when it's opened.
  struct Group
  {
    uint64_t id = 0;                       // its first radio's BSSID (what "opened" remembers)
    std::vector<uint64_t> members;         // sorted like the table
    Entry summary;                         // what its summary row shows
    std::wstring names, channels, bands;
  };
  struct Row { int group; uint64_t key; bool summary; };
  std::vector<Group> groups;
  std::vector<Row> rows;
  std::vector<uint64_t> shown;             // every radio that passes the filter and the band switches
  std::map<uint64_t, uint64_t> groupOf;    // radio -> its group's id
  std::set<uint64_t> opened;               // groups shown open

  HWND window = nullptr, filterBox = nullptr;
  HBRUSH editBrush = nullptr;
  float ui = 1;
  ID2D1Factory * d2d = nullptr;
  IDWriteFactory * dwrite = nullptr;
  ID2D1HwndRenderTarget * target = nullptr;
  ID2D1SolidColorBrush * brush = nullptr;
  IDWriteTextFormat * fontText = nullptr, * fontBold = nullptr, * fontSmall = nullptr, * fontTitle = nullptr;

  // the view
  enum Column { ColSsid, ColBssid, ColVendor, ColSignal, ColChannel, ColWidth, ColBand, ColWifi, ColSecurity, ColMax, ColMin, ColClients, ColUtil, ColSeen, ColCount };
  struct ColumnInfo { const wchar_t * name; float width; bool right; };
  const ColumnInfo columns[ColCount] = {
    { L"SSID", 186, false }, { L"BSSID", 132, false }, { L"Vendor", 158, false }, { L"Signal", 110, false }, { L"Channel", 78, true },
    { L"Width", 70, true }, { L"Band", 62, true }, { L"Wi-Fi", 64, true }, { L"Security", 124, false },
    { L"Max rate", 84, true }, { L"Min", 52, true }, { L"Clients", 62, true }, { L"Busy", 54, true }, { L"Seen", 58, true },
  };
  int sortColumn = ColSignal;
  bool sortDown = true;                    // (signal: strongest first)
  bool showBand[3] = { true, true, true }; // 2.4, 5, 6 GHz
  bool grouped = false;                    // the table by router
  bool timeView = false;                   // the graphs: signal over time instead of channels
  uint64_t selected = 0;                   // a radio, or 0
  uint64_t selectedGroup = 0;              // a router's group (its summary row), or 0
  int scroll = 0;                          // the table's first row
  float split = 0.52f;                     // the table's share of the height
  bool dragging = false;
  std::wstring filter;
  std::wstring settingsFile;
  bool hasSixGhz = false;

  // the layout, set by layout()
  float W = 0, H = 0, top = 0, tableTop = 0, tableBottom = 0, rowH = 0, headH = 0, graphTop = 0;
  D2D1_RECT_F bandPill[3], groupPill, viewTab[2];
  struct Pane { int band; float x0, x1; double f0, f1; };
  std::vector<Pane> panes;

  // ------------------------------------------------------------- helpers
  uint64_t keyOf(const uint8_t * m)
  {
    uint64_t k = 0;
    for (int i = 0; i < 6; ++i) k = (k << 8) | m[i];
    return k;
  }

  std::wstring macText(const uint8_t * m)
  {
    wchar_t t[24];
    swprintf_s(t, L"%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return t;
  }

  std::wstring format(const wchar_t * f, ...)
  {
    wchar_t t[256];
    va_list a;
    va_start(a, f);
    _vsnwprintf_s(t, _countof(t), _TRUNCATE, f, a);
    va_end(a);
    return t;
  }

  D2D1_COLOR_F rgb(uint32_t c, float a = 1)
  {
    return D2D1::ColorF(((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, a);
  }

  // a steady, distinct color for each BSSID
  uint32_t colorFor(uint64_t key)
  {
    uint64_t h = key * 0x9E3779B97F4A7C15ull;
    const double hue = (double) ((h >> 20) % 360), s = 0.55 + ((h >> 40) % 30) / 100.0, v = 0.95;
    const double c = v * s, x = c * (1 - std::fabs(fmod(hue / 60, 2) - 1)), m = v - c;
    double r = 0, g = 0, b = 0;
    if (hue < 60) { r = c; g = x; } else if (hue < 120) { r = x; g = c; } else if (hue < 180) { g = c; b = x; }
    else if (hue < 240) { g = x; b = c; } else if (hue < 300) { r = x; b = c; } else { r = c; b = x; }
    return ((uint32_t) ((r + m) * 255) << 16) | ((uint32_t) ((g + m) * 255) << 8) | (uint32_t) ((b + m) * 255);
  }

  std::wstring lower(std::wstring s)
  {
    for (auto & c : s) c = (wchar_t) towlower(c);
    return s;
  }

  int bandIndex(int band) { return band == 2 ? 0 : band == 5 ? 1 : 2; }
  const wchar_t * bandName(int band) { return band == 2 ? L"2.4" : band == 5 ? L"5" : L"6"; }

  std::wstring wifiName(const wlan::Bss & b)
  {
    switch (b.generation) {
      case 7: return L"7";
      case 6: return b.band == 6 ? L"6E" : L"6";
      case 5: return L"5";
      case 4: return L"4";
      default: return b.band == 2 ? L"b/g" : L"a";
    }
  }

  std::wstring rateText(double mbps)
  {
    if (mbps <= 0) return L"";
    return mbps >= 100 ? format(L"%.0f", mbps) : format(L"%.1f", mbps);
  }

  std::wstring nameOf(const wlan::Bss & b) { return b.ssid.empty() ? L"(hidden)" : b.ssid; }

  // ------------------------------------------------------------- settings
  void saveSettings()
  {
    auto put = [](const wchar_t * k, const std::wstring & v) { WritePrivateProfileStringW(L"WiFiScope", k, v.c_str(), settingsFile.c_str()); };
    put(L"Sort", format(L"%s %d", columns[sortColumn].name, sortDown ? 1 : 0));
    put(L"Bands", format(L"%d %d %d", showBand[0], showBand[1], showBand[2]));
    put(L"Split", format(L"%.3f", split));
    put(L"Grouped", grouped ? L"1" : L"0");
    put(L"Graph", timeView ? L"time" : L"channels");
    WINDOWPLACEMENT wp = { sizeof(wp) };
    if (GetWindowPlacement(window, &wp)) {
      const RECT & r = wp.rcNormalPosition;
      put(L"Window", format(L"%d %d %d %d %d", r.left, r.top, r.right, r.bottom, wp.showCmd == SW_SHOWMAXIMIZED ? 1 : 0));
    }
  }

  std::wstring setting(const wchar_t * k)
  {
    wchar_t t[256];
    GetPrivateProfileStringW(L"WiFiScope", k, L"", t, _countof(t), settingsFile.c_str());
    return t;
  }

  // ------------------------------------------------------------- the data
  // Two addresses of one router: routers make their other radios' and extra
  // networks' addresses from their own by changing a byte or two - here: the
  // second and third bytes the same, and at least four of the last five.
  bool sibling(const uint8_t * a, const uint8_t * b)
  {
    if (a[1] != b[1] || a[2] != b[2]) return false;
    int same = 0;
    for (int i = 1; i < 6; ++i) same += a[i] == b[i];
    return same >= 4;
  }

  bool connectedMac(uint8_t * m)
  {
    if (connected.size() != 17) return false;
    for (int i = 0; i < 6; ++i) m[i] = (uint8_t) wcstoul(connected.substr(i * 3, 2).c_str(), nullptr, 16);
    return true;
  }

  // Demo mode (WiFiScope --demo), for screenshots that can be shared: every
  // name is replaced by a made-up one, and each of the last three bytes of
  // every address goes through its own shuffled table, made at random each
  // run - so neither the neighbors' names nor the addresses (which Wi-Fi maps
  // can locate) show.  Bytes that were the same stay the same, so a router's
  // radios still group together; the makers' part of the addresses stays, so
  // the Vendor column still works.
  bool demo = false;
  uint64_t demoSalt = 0;
  uint8_t demoTable[3][256];
  std::map<std::wstring, std::wstring> demoNames;

  void makeDemoTables()
  {
    for (auto & t : demoTable) {
      for (int i = 0; i < 256; ++i) t[i] = (uint8_t) i;
      for (int i = 255; i > 0; --i) {
        unsigned int r = 0;
        rand_s(&r);
        std::swap(t[i], t[r % (i + 1)]);
      }
    }
  }

  void disguise(uint8_t * mac)
  {
    for (int i = 3; i < 6; ++i) mac[i] = demoTable[i - 3][mac[i]];
  }

  void disguise(wlan::Bss & b)
  {
    static const wchar_t * names[] = {
      L"Maple House", L"Blue Heron", L"Casa Verde", L"Nighthawk", L"Juniper", L"Saguaro", L"The Burrow",
      L"Starlight", L"Coyote Den", L"Lakeside", L"Copper Ridge", L"Bluebird", L"Quail Run", L"Desert Rose",
      L"Mesquite", L"Ironwood", L"Palo Verde", L"Sunset 5G", L"Granite Peak", L"Red Rock", L"Ocotillo",
      L"Thunderbird", L"Hummingbird", L"Pinecone", L"Riverbend", L"Cactus Flower", L"Willow", L"Aspen",
      L"Canyon View", L"Moonbeam", L"Roadrunner", L"Sycamore", L"Tumbleweed", L"Agave", L"Monsoon",
    };
    if (!b.ssid.empty()) {
      auto it = demoNames.find(b.ssid);
      if (it == demoNames.end()) {
        const size_t n = demoNames.size();
        std::wstring name = names[(n + (size_t) (demoSalt % _countof(names))) % _countof(names)];
        if (n >= _countof(names)) name += format(L" %d", (int) (n / _countof(names)) + 1);
        it = demoNames.emplace(b.ssid, name).first;
      }
      b.ssid = it->second;
    }
    disguise(b.mac);
    if (b.multiLink) disguise(b.mld);
  }

  void refresh()
  {
    const ULONGLONG now = GetTickCount64();
    for (wlan::Bss b : wlan::networks()) {
      if (demo) disguise(b);
      const uint64_t key = keyOf(b.mac);
      Entry & e = entries[key];
      if (!e.color) e.color = colorFor(key) | 1;
      const ULONGLONG heard = now - std::min<ULONGLONG>(now, (ULONGLONG) b.ageMs);
      if (heard + 1000 < e.heard) continue;      // (an older copy, from another interface)
      const bool newer = heard > e.heard + 500 || e.history.empty();
      e.bss = b;
      e.heard = std::max(e.heard, heard);
      if (newer) e.history.push_back({ heard, b.rssi });
      if (b.band == 6) hasSixGhz = true;
    }
    for (auto it = entries.begin(); it != entries.end();) {
      auto & h = it->second.history;
      while (!h.empty() && h.front().first + HistorySeconds * 1000ull < now) h.pop_front();
      if (it->second.heard + DropAfterMs < now) {
        if (selected == it->first) selected = 0;
        it = entries.erase(it);
      }
      else ++it;
    }
    connected = wlan::connectedBssid();
    uint8_t m[6];
    if (demo && connectedMac(m)) {
      disguise(m);
      connected = macText(m);
    }

    // the makers.  A made-up ("locally administered") address has none of its
    // own: if it is a sibling of another network's registered address, it's
    // that router's extra network (guest, IoT, mesh); otherwise it's a private
    // address (a phone's hotspot and the like)
    for (auto & [key, e] : entries) {
      if (!e.vendor.empty() && !oui::local(e.bss.mac)) continue;   // (made-up ones: again, as their router may be heard later)
      if (!oui::local(e.bss.mac)) {
        e.vendor = oui::vendor(e.bss.mac);
        continue;
      }
      std::wstring maker;
      for (auto & [k2, other] : entries)
        if (!oui::local(other.bss.mac) && sibling(other.bss.mac, e.bss.mac)) {
          maker = oui::vendor(other.bss.mac);
          if (!maker.empty()) break;
        }
      e.vendor = maker.empty() ? L"(private address)" : maker + L" (extra network)";
    }
  }

  // our own router: the radio we're connected to, and its siblings
  bool ours(const Entry & e)
  {
    uint8_t m[6];
    return connectedMac(m) && (memcmp(m, e.bss.mac, 6) == 0 || sibling(m, e.bss.mac));
  }

  // the quietest channel for each band, from everything heard but our own router
  std::vector<advice::Choice> suggestions(int band)
  {
    std::vector<advice::Radio> radios;
    for (auto & [k, e] : entries) {
      if (ours(e)) continue;
      advice::Radio r;
      r.band = e.bss.band; r.center = e.bss.center; r.width = e.bss.width;
      r.center2 = e.bss.split ? e.bss.center2 : 0;
      if (e.bss.split) r.width = 80;
      r.rssi = e.bss.rssi; r.utilization = e.bss.utilization;
      radios.push_back(r);
    }
    return advice::rank(band, radios);
  }

  // sorting: <0, 0, >0 for the table's column (ascending)
  int compare(const Entry & x, const Entry & y, uint64_t a, uint64_t b)
  {
    auto value = [](const Entry & e, int c) -> double {
      const wlan::Bss & s = e.bss;
      switch (c) {
        case ColSignal: return s.rssi;
        case ColChannel: return s.band * 1000 + s.channel;
        case ColWidth: return s.width;
        case ColBand: return s.band;
        case ColWifi: return s.generation * 10 + (s.band == 6);
        case ColMax: return s.maxRate;
        case ColMin: return s.minRate;
        case ColClients: return s.stations;
        case ColUtil: return s.utilization;
        case ColSeen: return -(double) e.heard;
        default: return 0;
      }
    };
    auto text = [](const std::wstring & p, const std::wstring & q) {   // (empty ones last)
      const std::wstring lp = lower(p), lq = lower(q);
      return lp == lq ? 0 : lp.empty() ? 1 : lq.empty() ? -1 : (lp < lq ? -1 : 1);
    };
    switch (sortColumn) {
      case ColSsid: return text(x.bss.ssid, y.bss.ssid);
      case ColBssid: return a < b ? -1 : a > b ? 1 : 0;
      case ColVendor: return text(x.vendor, y.vendor);
      case ColSecurity: return text(x.bss.security, y.bss.security);
      default: {
        const double vx = value(x, sortColumn), vy = value(y, sortColumn);
        return vx < vy ? -1 : vx > vy ? 1 : 0;
      }
    }
  }

  bool before(const Entry & x, const Entry & y, uint64_t a, uint64_t b)
  {
    const int order = compare(x, y, a, b);
    if (order == 0) return x.bss.rssi > y.bss.rssi || (x.bss.rssi == y.bss.rssi && a < b);
    return sortDown ? order > 0 : order < 0;
  }

  // The rows: filtered, grouped by router when that's on, and sorted.
  void sortRows()
  {
    const std::wstring f = lower(filter);
    shown.clear();
    for (auto & [key, e] : entries) {
      if (!showBand[bandIndex(e.bss.band)]) continue;
      if (!f.empty() && lower(e.bss.ssid).find(f) == std::wstring::npos && macText(e.bss.mac).find(f) == std::wstring::npos &&
          lower(e.vendor).find(f) == std::wstring::npos)
        continue;
      shown.push_back(key);
    }
    std::sort(shown.begin(), shown.end(), [](uint64_t a, uint64_t b) { return before(entries[a], entries[b], a, b); });

    // the groups: radios joined when their addresses are siblings, or when
    // they are links of the same Wi-Fi 7 multi-link device
    std::map<uint64_t, uint64_t> parent;
    std::function<uint64_t(uint64_t)> find = [&](uint64_t k) { uint64_t & p = parent[k]; if (p == 0 || p == k) { p = k; return k; } return p = find(p); };
    for (uint64_t k : shown) find(k);
    if (grouped)
      for (size_t i = 0; i < shown.size(); ++i)
        for (size_t j = i + 1; j < shown.size(); ++j) {
          const wlan::Bss & p = entries[shown[i]].bss, & q = entries[shown[j]].bss;
          if (sibling(p.mac, q.mac) || (p.multiLink && q.multiLink && memcmp(p.mld, q.mld, 6) == 0)) {
            const uint64_t a = find(shown[i]), b = find(shown[j]);
            if (a != b) parent[std::max(a, b)] = std::min(a, b);
          }
        }
    groups.clear();
    groupOf.clear();
    std::map<uint64_t, size_t> index;
    for (uint64_t k : shown) {               // (in table order: each group's radios sorted too)
      const uint64_t root = find(k);
      auto it = index.find(root);
      if (it == index.end()) {
        it = index.emplace(root, groups.size()).first;
        groups.push_back(Group());
        groups.back().id = root;
      }
      groups[it->second].members.push_back(k);
      groupOf[k] = root;
    }

    // what a router's summary row shows: its strongest radio, with the
    // others' channels, bands and names added
    for (Group & g : groups) {
      const Entry & first = entries[g.members.front()];
      Entry s = first;
      for (uint64_t k : g.members) if (entries[k].bss.rssi > s.bss.rssi) s = entries[k];
      std::vector<std::wstring> names, channels;
      bool band[3] = {};
      int stations = -1;
      for (uint64_t k : g.members) {
        const Entry & e = entries[k];
        const wlan::Bss & b = e.bss;
        if (!b.ssid.empty() && std::find(names.begin(), names.end(), b.ssid) == names.end()) names.push_back(b.ssid);
        const std::wstring ch = format(L"%d", b.channel);
        if (std::find(channels.begin(), channels.end(), ch) == channels.end()) channels.push_back(ch);
        band[bandIndex(b.band)] = true;
        s.heard = std::max(s.heard, e.heard);
        s.bss.width = std::max(s.bss.width, b.width);
        s.bss.maxRate = std::max(s.bss.maxRate, b.maxRate);
        s.bss.minRate = std::min(s.bss.minRate, b.minRate);
        s.bss.utilization = std::max(s.bss.utilization, b.utilization);
        if (b.generation > s.bss.generation) s.bss.generation = b.generation;
        if (b.stations >= 0) stations = std::max(0, stations) + b.stations;
        if (oui::local(s.bss.mac) && !oui::local(b.mac)) { s.vendor = e.vendor; memcpy(s.bss.mac, b.mac, 6); }
      }
      s.bss.stations = stations;
      s.bss.ssid = names.empty() ? L"" : names[0];
      g.names = names.empty() ? L"(hidden)" : names[0];
      for (size_t i = 1; i < names.size() && i < 3; ++i) g.names += L", " + names[i];
      if (names.size() > 3) g.names += format(L" +%d", (int) names.size() - 3);
      for (size_t i = 0; i < channels.size(); ++i) g.channels += (i ? L", " : L"") + channels[i];
      for (int i = 0; i < 3; ++i) if (band[i]) g.bands += (g.bands.empty() ? L"" : L"+") + std::wstring(bandName(i == 0 ? 2 : i == 1 ? 5 : 6));
      g.summary = s;
    }
    std::stable_sort(groups.begin(), groups.end(), [](const Group & a, const Group & b) {
      return before(a.summary, b.summary, a.id, b.id);
    });

    rows.clear();
    for (size_t i = 0; i < groups.size(); ++i) {
      const Group & g = groups[i];
      if (g.members.size() == 1) { rows.push_back({ (int) i, g.members[0], false }); continue; }
      rows.push_back({ (int) i, g.id, true });
      if (opened.count(g.id))
        for (uint64_t k : g.members) rows.push_back({ (int) i, k, false });
    }
    if (selectedGroup && !std::any_of(groups.begin(), groups.end(), [](const Group & g) { return g.id == selectedGroup && g.members.size() > 1; }))
      selectedGroup = 0;
  }

  bool highlighted(uint64_t key)
  {
    if (key == selected) return true;
    if (!selectedGroup) return false;
    auto it = groupOf.find(key);
    return it != groupOf.end() && it->second == selectedGroup;
  }
  bool anySelection() { return selected || selectedGroup; }

  // ------------------------------------------------------------- drawing
  void text(const std::wstring & s, IDWriteTextFormat * f, D2D1_RECT_F r, uint32_t color, float alpha = 1,
            DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING)
  {
    f->SetTextAlignment(align);
    brush->SetColor(rgb(color, alpha));
    target->DrawTextW(s.c_str(), (UINT32) s.size(), f, r, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
  }

  float textWidth(const std::wstring & s, IDWriteTextFormat * f)
  {
    IDWriteTextLayout * l = nullptr;
    float w = 0;
    if (SUCCEEDED(dwrite->CreateTextLayout(s.c_str(), (UINT32) s.size(), f, 2000, 100, &l))) {
      DWRITE_TEXT_METRICS m;
      l->GetMetrics(&m);
      w = m.widthIncludingTrailingWhitespace;
      l->Release();
    }
    return w;
  }

  bool inside(const D2D1_RECT_F & r, float x, float y) { return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom; }

  void layout()
  {
    RECT r;
    GetClientRect(window, &r);
    W = (float) r.right; H = (float) r.bottom;
    top = 44 * ui;
    headH = 26 * ui;
    rowH = 22 * ui;
    tableTop = top;
    const float graphs = H - top;
    tableBottom = top + std::max(headH + rowH * 3, std::min(graphs - 140 * ui, graphs * split));
    graphTop = tableBottom + 8 * ui;

    // the band switches and "By router", top right
    float x = W - 10 * ui;
    for (int i = 2; i >= 0; --i) {
      const float w = 64 * ui;
      bandPill[i] = D2D1::RectF(x - w, 10 * ui, x, 34 * ui);
      x -= w + 6 * ui;
    }
    groupPill = D2D1::RectF(x - 84 * ui, 10 * ui, x, 34 * ui);
    x -= 84 * ui + 10 * ui;
    if (filterBox) {
      const int fw = (int) (240 * ui);
      MoveWindow(filterBox, (int) (x - fw), (int) (11 * ui), fw, (int) (22 * ui), TRUE);
    }
    // the graph switch, right of the graphs' titles
    viewTab[1] = D2D1::RectF(W - 10 * ui - 128 * ui, graphTop, W - 10 * ui, graphTop + 18 * ui);
    viewTab[0] = D2D1::RectF(viewTab[1].left - 4 * ui - 84 * ui, graphTop, viewTab[1].left - 4 * ui, graphTop + 18 * ui);

    // the channel graphs: 2.4 | 5 | 6 GHz, side by side, sized by how much there is to see
    panes.clear();
    std::vector<Pane> want;
    if (showBand[0]) want.push_back({ 2, 0, 0, 2401, 2495 });
    if (showBand[1]) want.push_back({ 5, 0, 0, 5150, 5895 });
    if (showBand[2] && hasSixGhz) want.push_back({ 6, 0, 0, 5925, 7125 });
    const double weight[3] = { 1.0, 1.7, 1.5 };
    double total = 0;
    for (auto & p : want) total += weight[bandIndex(p.band)];
    float px = 10 * ui;
    const float avail = W - 20 * ui - (want.empty() ? 0 : (want.size() - 1) * 10 * ui);
    for (auto & p : want) {
      const float w = (float) (avail * weight[bandIndex(p.band)] / total);
      p.x0 = px; p.x1 = px + w;
      px += w + 10 * ui;
      panes.push_back(p);
    }
  }

  void pill(const D2D1_RECT_F & r, const wchar_t * label, bool on)
  {
    brush->SetColor(rgb(on ? 0x2E86DE : 0x2A3540));
    target->FillRoundedRectangle(D2D1::RoundedRect(r, (r.bottom - r.top) / 2, (r.bottom - r.top) / 2), brush);
    text(label, fontSmall, D2D1::RectF(r.left, r.top + (r.bottom - r.top - 16 * ui) / 2, r.right, r.bottom), on ? 0xFFFFFF : 0x8FA3B8, 1,
         DWRITE_TEXT_ALIGNMENT_CENTER);
  }

  void drawTop()
  {
    brush->SetColor(rgb(0x141A21));
    target->FillRectangle(D2D1::RectF(0, 0, W, top), brush);
    text(L"WiFiScope", fontTitle, D2D1::RectF(12 * ui, 8 * ui, 200 * ui, 36 * ui), 0xFFFFFF);
    int fading = 0;
    const ULONGLONG now = GetTickCount64();
    for (auto & [k, e] : entries) if (e.heard + FadeAfterMs < now) ++fading;
    std::wstring status = format(L"%d networks", (int) entries.size() - fading);
    if (grouped) {
      int routers = 0;
      for (const Group & g : groups) if (g.summary.heard + FadeAfterMs >= now) ++routers;
      status += format(L", %d routers", routers);
    }
    if (!adapter.empty()) status += L"   •   " + adapter;
    if (demo) status += L"   •   demo: names and addresses disguised";
    text(status, fontSmall, D2D1::RectF(130 * ui, 15 * ui, groupPill.left - 260 * ui, 34 * ui), 0x9FB0C2);
    pill(groupPill, L"By router", grouped);
    const wchar_t * names[3] = { L"2.4 GHz", L"5 GHz", L"6 GHz" };
    for (int i = 0; i < 3; ++i) pill(bandPill[i], names[i], showBand[i]);
  }

  float columnX(int c)
  {
    float x = 22 * ui;
    for (int i = 0; i < c; ++i) x += columns[i].width * ui;
    return x;
  }

  int visibleRows() { return std::max(1, (int) ((tableBottom - tableTop - headH) / rowH)); }

  void drawTable()
  {
    const ULONGLONG now = GetTickCount64();
    brush->SetColor(rgb(0x0E1318));
    target->FillRectangle(D2D1::RectF(0, tableTop, W, tableBottom), brush);
    brush->SetColor(rgb(0x1C2630));
    target->FillRectangle(D2D1::RectF(0, tableTop, W, tableTop + headH), brush);
    for (int c = 0; c < ColCount; ++c) {
      const float x = columnX(c), w = columns[c].width * ui;
      std::wstring name = columns[c].name;
      if (c == sortColumn) name += sortDown ? L" ▾" : L" ▴";
      text(name, fontBold, D2D1::RectF(x + 4 * ui, tableTop + 5 * ui, x + w - 6 * ui, tableTop + headH),
           c == sortColumn ? 0xFFFFFF : 0xB8C6D6, 1, columns[c].right ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    const int visible = visibleRows();
    scroll = std::max(0, std::min(scroll, (int) rows.size() - visible));
    uint8_t ourMac[6];
    const bool haveOurs = connectedMac(ourMac);
    target->PushAxisAlignedClip(D2D1::RectF(0, tableTop + headH, W, tableBottom), D2D1_ANTIALIAS_MODE_ALIASED);
    for (int i = 0; i < visible && scroll + i < (int) rows.size(); ++i) {
      const Row & row = rows[scroll + i];
      const Group & g = groups[row.group];
      const Entry & e = row.summary ? g.summary : entries[row.key];
      const wlan::Bss & b = e.bss;
      const bool child = !row.summary && g.members.size() > 1;
      const float y = tableTop + headH + i * rowH;
      const bool faded = e.heard + FadeAfterMs < now;
      bool mine = haveOurs && memcmp(ourMac, b.mac, 6) == 0;
      if (row.summary) for (uint64_t k : g.members) mine = mine || (haveOurs && memcmp(ourMac, entries[k].bss.mac, 6) == 0);
      const bool picked = row.summary ? selectedGroup == g.id : (row.key == selected || (selectedGroup && selectedGroup == g.id));
      if (picked) {
        brush->SetColor(rgb(0x2E86DE, row.summary || row.key == selected ? 0.35f : 0.18f));
        target->FillRectangle(D2D1::RectF(0, y, W, y + rowH), brush);
      }
      else if (row.summary) {
        brush->SetColor(rgb(0xFFFFFF, 0.05f));
        target->FillRectangle(D2D1::RectF(0, y, W, y + rowH), brush);
      }
      else if (i % 2) {
        brush->SetColor(rgb(0xFFFFFF, 0.025f));
        target->FillRectangle(D2D1::RectF(0, y, W, y + rowH), brush);
      }
      brush->SetColor(rgb(e.color, faded ? 0.35f : 1));
      target->FillRectangle(D2D1::RectF((child ? 12 : 6) * ui, y + 4 * ui, (child ? 16 : 14) * ui, y + rowH - 4 * ui), brush);
      const float alpha = faded ? 0.45f : 1;
      const uint32_t fg = 0xE8EEF4, dim = 0x9FB0C2;
      auto cell = [&](int c, const std::wstring & s, uint32_t color, IDWriteTextFormat * f = nullptr, float indent = 0) {
        const float x = columnX(c) + indent, w = columns[c].width * ui - indent;
        text(s, f ? f : fontText, D2D1::RectF(x + 4 * ui, y + 3 * ui, x + w - 6 * ui, y + rowH), color, alpha,
             columns[c].right ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
      };
      if (row.summary) {
        const bool open = opened.count(g.id) != 0;
        cell(ColSsid, std::wstring(open ? L"▾ " : L"▸ ") + (mine ? L"● " : L"") + g.names, fg, fontBold);
        cell(ColBssid, format(L"%d radios", (int) g.members.size()), dim);
        cell(ColChannel, g.channels, fg);
        cell(ColBand, g.bands, dim);
      }
      else {
        std::wstring name = nameOf(b);
        if (mine) name = L"● " + name;
        cell(ColSsid, name, b.ssid.empty() ? dim : fg, mine ? fontBold : nullptr, child ? 16 * ui : 0);
        cell(ColBssid, macText(b.mac) + (b.multiLink ? L" ≡" : L""), dim);
        cell(ColChannel, format(L"%d", b.channel), fg);
        cell(ColBand, bandName(b.band), dim);
      }
      cell(ColVendor, e.vendor, oui::local(b.mac) ? 0x8FA3B8 : 0xC8D6E5);
      // signal: the number, and a bar from -95 to -35 dBm
      {
        const float x = columnX(ColSignal) + 4 * ui, bw = 58 * ui;
        const float t = std::max(0.0f, std::min(1.0f, (b.rssi + 95) / 60.0f));
        brush->SetColor(rgb(0x2A3540, alpha));
        target->FillRectangle(D2D1::RectF(x, y + 8 * ui, x + bw, y + rowH - 8 * ui), brush);
        const uint32_t barColor = b.rssi >= -60 ? 0x2ECC71 : b.rssi >= -70 ? 0xF1C40F : b.rssi >= -80 ? 0xE67E22 : 0xE74C3C;
        brush->SetColor(rgb(barColor, alpha));
        target->FillRectangle(D2D1::RectF(x, y + 8 * ui, x + bw * t, y + rowH - 8 * ui), brush);
        text(format(L"%d", b.rssi), fontText, D2D1::RectF(x + bw + 6 * ui, y + 3 * ui, x + columns[ColSignal].width * ui, y + rowH), fg, alpha);
      }
      cell(ColWidth, b.split ? L"80+80" : format(L"%d", b.width), fg);
      cell(ColWifi, wifiName(b), b.generation >= 7 ? 0xFFD166 : fg);
      cell(ColSecurity, b.security, b.security == L"Open" || b.security == L"WEP" ? 0xFFB4A2 : fg);
      cell(ColMax, rateText(b.maxRate), fg);
      cell(ColMin, rateText(b.minRate), dim);
      cell(ColClients, b.stations >= 0 ? format(L"%d", b.stations) : L"", fg);
      cell(ColUtil, b.utilization >= 0 ? format(L"%d%%", b.utilization) : L"",
           b.utilization >= 70 ? 0xFF7B6B : b.utilization >= 40 ? 0xF1C40F : fg);
      const int seen = (int) ((now - std::min(now, e.heard)) / 1000);
      cell(ColSeen, seen < 2 ? L"now" : seen < 120 ? format(L"%d s", seen) : format(L"%d m", seen / 60), dim);
    }
    target->PopAxisAlignedClip();

    // a scroll bar when there are more rows than fit
    if ((int) rows.size() > visible) {
      const float h = tableBottom - tableTop - headH;
      const float th = std::max(20 * ui, h * visible / rows.size()), ty = tableTop + headH + (h - th) * scroll / (rows.size() - visible);
      brush->SetColor(rgb(0x5C7080, 0.7f));
      target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(W - 7 * ui, ty, W - 3 * ui, ty + th), 2 * ui, 2 * ui), brush);
    }
    // the splitter
    brush->SetColor(rgb(0x2A3540));
    target->FillRectangle(D2D1::RectF(0, tableBottom, W, tableBottom + 3 * ui), brush);
  }

  // The networks' names on a graph: the selected ones and the strongest placed
  // first, just above their point; one that would overlap another moves up a
  // line, or down, with a thin line to its point when it has moved.  Names
  // with no room left are skipped (the table has them).
  struct Label { uint64_t key; float x, y, alpha; bool right; };

  void placeLabels(std::vector<Label> & names, float left, float right, float top, float bottom)
  {
    std::sort(names.begin(), names.end(), [](const Label & a, const Label & b) {
      if (highlighted(a.key) != highlighted(b.key)) return highlighted(a.key);
      return a.y < b.y;
    });
    std::vector<D2D1_RECT_F> placed;
    const float lh = 15 * ui;
    for (const Label & l : names) {
      const Entry & e = entries[l.key];
      const std::wstring name = nameOf(e.bss);
      const float tw = textWidth(name, fontSmall) + 8 * ui;
      const float x = l.right ? std::min(right - tw - 1, l.x + 4 * ui) : std::max(left + 1, std::min(right - tw - 1, l.x - tw / 2));
      D2D1_RECT_F box = {};
      bool found = false;
      for (int step : { 0, -1, -2, -3, 1, 2, 3, 4, -4, 5, 6 }) {
        const float y = (l.right ? l.y - lh / 2 : l.y - 17 * ui) + step * (lh + 1 * ui);
        if (y < top - 17 * ui || y + lh > bottom) continue;
        const D2D1_RECT_F r = D2D1::RectF(x, y, x + tw, y + lh);
        bool clash = false;
        for (const auto & q : placed)
          if (r.left < q.right && q.left < r.right && r.top < q.bottom && q.top < r.bottom) { clash = true; break; }
        if (!clash) { box = r; found = true; break; }
      }
      if (!found) continue;
      placed.push_back(box);
      const float home = l.right ? l.y - lh / 2 : l.y - 17 * ui;
      if (std::fabs(box.top - home) > 1 * ui) {
        brush->SetColor(rgb(e.color, 0.5f * l.alpha));
        const float ly = box.bottom < l.y ? box.bottom : box.top;
        if (l.right) target->DrawLine(D2D1::Point2F(l.x, l.y), D2D1::Point2F(box.left, (box.top + box.bottom) / 2), brush, 1);
        else target->DrawLine(D2D1::Point2F(l.x, ly), D2D1::Point2F(l.x, l.y), brush, 1);
      }
      brush->SetColor(rgb(0x0E1318, 0.8f * l.alpha));
      target->FillRectangle(box, brush);
      text(name, fontSmall, D2D1::RectF(box.left, box.top + 1 * ui, box.right, box.bottom), e.color, l.alpha, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
  }

  const double dbTop = -20, dbBottom = -100;

  void drawDbGrid(float x0, float ax, float x1, float y0, float y1)
  {
    auto fy = [&](double db) { return (float) (y0 + (dbTop - db) / (dbTop - dbBottom) * (y1 - y0)); };
    brush->SetColor(rgb(0x0E1318));
    target->FillRectangle(D2D1::RectF(ax, y0, x1, y1), brush);
    for (int db = -30; db >= -90; db -= 10) {
      brush->SetColor(rgb(0xFFFFFF, 0.07f));
      target->DrawLine(D2D1::Point2F(ax, fy(db)), D2D1::Point2F(x1, fy(db)), brush, 1);
      text(format(L"%d", db), fontSmall, D2D1::RectF(x0, fy(db) - 8 * ui, ax - 4 * ui, fy(db) + 8 * ui), 0x8FA3B8, 1, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
  }

  void drawPane(const Pane & p)
  {
    const ULONGLONG now = GetTickCount64();
    const float x0 = p.x0, x1 = p.x1;
    const float y0 = graphTop + 22 * ui, y1 = H - 26 * ui;
    const float ax = x0 + 34 * ui;                                   // (dBm labels on the left)
    auto fx = [&](double mhz) { return (float) (ax + (mhz - p.f0) / (p.f1 - p.f0) * (x1 - ax)); };
    auto fy = [&](double db) { return (float) (y0 + (dbTop - db) / (dbTop - dbBottom) * (y1 - y0)); };

    // the title, and the quietest channel for a router of yours
    const std::wstring title = p.band == 2 ? L"2.4 GHz" : p.band == 5 ? L"5 GHz" : L"6 GHz";
    text(title, fontBold, D2D1::RectF(x0, graphTop, x1, y0), 0xB8C6D6);
    const std::vector<advice::Choice> best = suggestions(p.band);
    if (!best.empty()) {
      const advice::Choice & c = best[0];
      std::wstring hint = c.first == c.last ? format(L"quietest: %d", c.first) : format(L"quietest: %d–%d (%d MHz)", c.first, c.last, c.width);
      if (p.band == 5 && c.dfs) {
        // (a radar channel: say which ordinary one comes next)
        for (const advice::Choice & o : best) if (!o.dfs) { hint += format(L", radar rules; or %d–%d", o.first, o.last); break; }
      }
      const float tx = x0 + textWidth(title, fontBold) + 12 * ui;
      const float limit = (p.x1 >= viewTab[0].left - 4 * ui) ? viewTab[0].left - 8 * ui : x1;
      text(hint, fontSmall, D2D1::RectF(tx, graphTop + 2 * ui, limit, y0), 0x7FD88F);
      const float l = fx(c.center - c.width / 2.0), r = fx(c.center + c.width / 2.0);
      drawDbGrid(x0, ax, x1, y0, y1);
      brush->SetColor(rgb(0x2ECC71, 0.07f));
      target->FillRectangle(D2D1::RectF(std::max(ax, l), y0, std::min(x1, r), y1), brush);
    }
    else drawDbGrid(x0, ax, x1, y0, y1);

    std::vector<int> labels;
    if (p.band == 2) for (int c = 1; c <= 14; ++c) labels.push_back(c);
    else if (p.band == 5) labels = { 36, 44, 52, 60, 100, 108, 116, 124, 132, 140, 149, 157, 165, 173 };
    else for (int c = 1; c <= 233; c += 32) labels.push_back(c);
    for (int c : labels) {
      const float x = fx(wlan::frequencyOf(p.band, c));
      brush->SetColor(rgb(0xFFFFFF, 0.05f));
      target->DrawLine(D2D1::Point2F(x, y0), D2D1::Point2F(x, y1), brush, 1);
      text(format(L"%d", c), fontSmall, D2D1::RectF(x - 20 * ui, y1 + 4 * ui, x + 20 * ui, y1 + 22 * ui), 0x8FA3B8, 1, DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // the networks: one shape across each one's channel, as high as its signal
    // (strongest drawn last, on top; the selected ones last of all)
    std::vector<uint64_t> order;
    for (uint64_t k : shown) if (entries[k].bss.band == p.band) order.push_back(k);
    std::sort(order.begin(), order.end(), [&](uint64_t a, uint64_t b) {
      if (highlighted(a) != highlighted(b)) return highlighted(b);
      return entries[a].bss.rssi < entries[b].bss.rssi;
    });
    std::vector<Label> names;
    target->PushAxisAlignedClip(D2D1::RectF(ax, y0 - 18 * ui, x1, y1), D2D1_ANTIALIAS_MODE_ALIASED);
    for (uint64_t k : order) {
      const Entry & e = entries[k];
      const wlan::Bss & b = e.bss;
      const bool faded = e.heard + FadeAfterMs < now;
      const bool lit = highlighted(k), dimmed = anySelection() && !lit;
      const float alpha = faded ? 0.3f : dimmed ? 0.35f : 1;
      auto shape = [&](int center, int width) {
        const double half = width / 2.0, slope = std::min(width * 0.12, 4.0);
        const float l = fx(center - half), r = fx(center + half);
        const float lt = fx(center - half + slope), rt = fx(center + half - slope);
        const float yt = fy(std::max(dbBottom, std::min(dbTop, (double) b.rssi))), yb = y1;
        ID2D1PathGeometry * g = nullptr;
        if (FAILED(d2d->CreatePathGeometry(&g))) return;
        ID2D1GeometrySink * s = nullptr;
        g->Open(&s);
        s->BeginFigure(D2D1::Point2F(l, yb), D2D1_FIGURE_BEGIN_FILLED);
        s->AddLine(D2D1::Point2F(lt, yt));
        s->AddLine(D2D1::Point2F(rt, yt));
        s->AddLine(D2D1::Point2F(r, yb));
        s->EndFigure(D2D1_FIGURE_END_OPEN);
        s->Close();
        s->Release();
        brush->SetColor(rgb(e.color, 0.10f * alpha + (lit ? 0.12f : 0)));
        target->FillGeometry(g, brush);
        brush->SetColor(rgb(e.color, alpha));
        target->DrawGeometry(g, brush, (lit ? 2.5f : 1.3f) * ui);
        g->Release();
        if (!dimmed) names.push_back({ k, (lt + rt) / 2, yt, alpha, false });
      };
      if (b.split) { shape(b.center, 80); shape(b.center2, 80); }
      else shape(b.center, b.width);
    }
    placeLabels(names, ax, x1, y0, y1);
    target->PopAxisAlignedClip();
    brush->SetColor(rgb(0x2A3540));
    target->DrawRectangle(D2D1::RectF(ax, y0, x1, y1), brush, 1);
  }

  // Signal over the last five minutes: a line for each network, broken where
  // it wasn't heard; its name at its newest point.
  void drawTime()
  {
    const ULONGLONG now = GetTickCount64();
    const float x0 = 10 * ui, x1 = W - 10 * ui - 150 * ui;          // (room for names on the right)
    const float y0 = graphTop + 22 * ui, y1 = H - 26 * ui;
    const float ax = x0 + 34 * ui;
    const double span = HistorySeconds * 1000.0;
    auto fx = [&](ULONGLONG t) { return (float) (ax + (1 - (double) (now - std::min(now, t)) / span) * (x1 - ax)); };
    auto fy = [&](double db) { return (float) (y0 + (dbTop - db) / (dbTop - dbBottom) * (y1 - y0)); };

    text(L"Signal over the last 5 minutes", fontBold, D2D1::RectF(x0, graphTop, viewTab[0].left - 8 * ui, y0), 0xB8C6D6);
    drawDbGrid(x0, ax, x1, y0, y1);
    for (int s = 0; s <= HistorySeconds; s += 30) {
      const float x = (float) (ax + (1 - s * 1000.0 / span) * (x1 - ax));
      brush->SetColor(rgb(0xFFFFFF, 0.05f));
      target->DrawLine(D2D1::Point2F(x, y0), D2D1::Point2F(x, y1), brush, 1);
      const std::wstring label = s == 0 ? L"now" : s % 60 == 0 ? format(L"%d min", s / 60) : format(L"%d:%02d", s / 60, s % 60);
      text(label, fontSmall, D2D1::RectF(x - 30 * ui, y1 + 4 * ui, x + 30 * ui, y1 + 22 * ui), 0x8FA3B8, 1, DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    std::vector<uint64_t> order(shown);
    std::sort(order.begin(), order.end(), [&](uint64_t a, uint64_t b) {
      if (highlighted(a) != highlighted(b)) return highlighted(b);
      return entries[a].bss.rssi < entries[b].bss.rssi;
    });
    std::vector<Label> names;
    target->PushAxisAlignedClip(D2D1::RectF(ax, y0 - 18 * ui, W - 10 * ui, y1), D2D1_ANTIALIAS_MODE_ALIASED);
    for (uint64_t k : order) {
      const Entry & e = entries[k];
      if (e.history.empty()) continue;
      const bool lit = highlighted(k), dimmed = anySelection() && !lit;
      const bool faded = e.heard + FadeAfterMs < now;
      const float alpha = dimmed ? 0.22f : faded ? 0.4f : 0.9f;
      brush->SetColor(rgb(e.color, alpha));
      const float width = (lit ? 2.6f : 1.4f) * ui;
      const std::pair<ULONGLONG, int> * prev = nullptr;
      for (const auto & pt : e.history) {
        const float x = fx(pt.first), y = fy(std::max(dbBottom, std::min(dbTop, (double) pt.second)));
        if (prev && pt.first - prev->first <= (ULONGLONG) GapMs)
          target->DrawLine(D2D1::Point2F(fx(prev->first), fy(std::max(dbBottom, std::min(dbTop, (double) prev->second)))), D2D1::Point2F(x, y), brush, width);
        else
          target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), width, width), brush);
        prev = &pt;
      }
      // (still being heard: carried on to "now")
      if (!faded && prev) {
        const float y = fy(std::max(dbBottom, std::min(dbTop, (double) prev->second)));
        target->DrawLine(D2D1::Point2F(fx(prev->first), y), D2D1::Point2F(x1, y), brush, width);
      }
      if (!dimmed && !faded && prev)
        names.push_back({ k, x1, fy(std::max(dbBottom, std::min(dbTop, (double) prev->second))), alpha, true });
    }
    placeLabels(names, ax, W - 10 * ui, y0, y1);
    target->PopAxisAlignedClip();
    brush->SetColor(rgb(0x2A3540));
    target->DrawRectangle(D2D1::RectF(ax, y0, x1, y1), brush, 1);
  }

  void paint()
  {
    if (!target) {
      RECT r;
      GetClientRect(window, &r);
      D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
      props.dpiX = props.dpiY = 96;
      if (FAILED(d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(window, D2D1::SizeU(r.right, r.bottom)), &target)))
        return;
      target->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &brush);
    }
    layout();
    sortRows();
    target->BeginDraw();
    target->Clear(rgb(0x0B1015));
    drawTop();
    drawTable();
    if (timeView) drawTime();
    else {
      for (const Pane & p : panes) drawPane(p);
      if (panes.empty())
        text(L"Choose a band at the top right.", fontText, D2D1::RectF(0, graphTop + 40 * ui, W, graphTop + 70 * ui), 0x8FA3B8, 1, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    pill(viewTab[0], L"Channels", !timeView);
    pill(viewTab[1], L"Signal over time", timeView);
    if (target->EndDraw() == D2DERR_RECREATE_TARGET) {
      brush->Release(); brush = nullptr;
      target->Release(); target = nullptr;
    }
  }

  void makeFonts()
  {
    auto make = [](float size, DWRITE_FONT_WEIGHT weight, IDWriteTextFormat ** f) {
      if (*f) (*f)->Release();
      dwrite->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size * ui, L"en-us", f);
      (*f)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
      DWRITE_TRIMMING trim = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
      (*f)->SetTrimming(&trim, nullptr);
    };
    make(13, DWRITE_FONT_WEIGHT_NORMAL, &fontText);
    make(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, &fontBold);
    make(11.5f, DWRITE_FONT_WEIGHT_NORMAL, &fontSmall);
    make(18, DWRITE_FONT_WEIGHT_SEMI_BOLD, &fontTitle);
  }

  // ------------------------------------------------------------- input
  int rowAt(float y)
  {
    if (y < tableTop + headH || y >= tableBottom) return -1;
    const int i = scroll + (int) ((y - tableTop - headH) / rowH);
    return i < (int) rows.size() ? i : -1;
  }

  void selectRow(int i)
  {
    const Row & r = rows[i];
    if (r.summary) { selectedGroup = groups[r.group].id; selected = 0; }
    else { selected = r.key; selectedGroup = 0; }
  }

  void copyText(const std::wstring & s)
  {
    if (!OpenClipboard(window)) return;
    EmptyClipboard();
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
    if (g) {
      memcpy(GlobalLock(g), s.c_str(), (s.size() + 1) * sizeof(wchar_t));
      GlobalUnlock(g);
      SetClipboardData(CF_UNICODETEXT, g);
    }
    CloseClipboard();
  }

  std::wstring rowText(const Entry & e)
  {
    const wlan::Bss & b = e.bss;
    return format(L"%s\t%s\t%s\t%d\t%d\t%s\t%s\t%s\t%s\t%s\t%s",
                  b.ssid.c_str(), macText(b.mac).c_str(), e.vendor.c_str(), b.rssi, b.channel,
                  (b.split ? std::wstring(L"80+80") : format(L"%d", b.width)).c_str(),
                  bandName(b.band), wifiName(b).c_str(), b.security.c_str(),
                  rateText(b.maxRate).c_str(), b.phy.c_str());
  }

  void contextMenu(int x, int y)
  {
    const int r = rowAt((float) y);
    HMENU menu = CreatePopupMenu();
    if (r >= 0) {
      selectRow(r);
      if (rows[r].summary) AppendMenuW(menu, MF_STRING, 2, L"Copy this router's radios");
      else {
        AppendMenuW(menu, MF_STRING, 1, L"Copy BSSID");
        AppendMenuW(menu, MF_STRING, 2, L"Copy this row");
      }
    }
    AppendMenuW(menu, MF_STRING, 3, L"Copy all rows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (grouped ? MF_CHECKED : 0), 5, L"Group by router\tG");
    AppendMenuW(menu, MF_STRING | (timeView ? MF_CHECKED : 0), 6, L"Signal over time\tT");
    AppendMenuW(menu, MF_STRING, 4, L"Scan now\tF5");
    POINT p = { x, y };
    ClientToScreen(window, &p);
    InvalidateRect(window, nullptr, FALSE);
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, window, nullptr);
    DestroyMenu(menu);
    const std::wstring header = L"SSID\tBSSID\tVendor\tdBm\tChannel\tWidth\tBand\tWi-Fi\tSecurity\tMax Mbps\tPHY\r\n";
    if (cmd == 1 && r >= 0) copyText(macText(entries[rows[r].key].bss.mac));
    else if (cmd == 2 && r >= 0) {
      std::wstring out = header;
      if (rows[r].summary) for (uint64_t k : groups[rows[r].group].members) out += rowText(entries[k]) + L"\r\n";
      else out += rowText(entries[rows[r].key]);
      copyText(out);
    }
    else if (cmd == 3) {
      std::wstring all = header;
      for (uint64_t k : shown) all += rowText(entries[k]) + L"\r\n";
      copyText(all);
    }
    else if (cmd == 4) wlan::scan();
    else if (cmd == 5) { grouped = !grouped; scroll = 0; saveSettings(); }
    else if (cmd == 6) { timeView = !timeView; saveSettings(); }
    InvalidateRect(window, nullptr, FALSE);
  }

  LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
  {
    switch (msg) {
      case WM_CREATE:
        window = hwnd;
        return 0;
      case WM_SIZE:
        if (target) target->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
        layout();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      case WM_DPICHANGED: {
        ui = HIWORD(wParam) / 96.0f;
        makeFonts();
        const RECT * r = (const RECT *) lParam;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
        return 0;
      }
      case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        paint();
        EndPaint(hwnd, &ps);
        return 0;
      }
      case WM_ERASEBKGND:
        return 1;
      case WM_SCANNED:
        refresh();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      case WM_TIMER:
        if (wParam == TimerScan) wlan::scan();
        else { refresh(); InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
      case WM_LBUTTONDOWN: {
        const float x = (float) GET_X_LPARAM(lParam), y = (float) GET_Y_LPARAM(lParam);
        SetFocus(hwnd);
        for (int i = 0; i < 3; ++i)
          if (inside(bandPill[i], x, y)) {
            showBand[i] = !showBand[i];
            saveSettings();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
          }
        if (inside(groupPill, x, y)) { grouped = !grouped; scroll = 0; saveSettings(); InvalidateRect(hwnd, nullptr, FALSE); return 0; }
        for (int i = 0; i < 2; ++i)
          if (inside(viewTab[i], x, y)) { timeView = i == 1; saveSettings(); InvalidateRect(hwnd, nullptr, FALSE); return 0; }
        if (y >= tableBottom - 2 * ui && y <= tableBottom + 6 * ui) { dragging = true; SetCapture(hwnd); return 0; }
        if (y >= tableTop && y < tableTop + headH) {
          for (int c = ColCount - 1; c >= 0; --c)
            if (x >= columnX(c)) {
              if (sortColumn == c) sortDown = !sortDown;
              else { sortColumn = c; sortDown = c == ColSignal || c == ColMax || c == ColWidth || c == ColWifi || c == ColClients || c == ColUtil; }
              saveSettings();
              break;
            }
          InvalidateRect(hwnd, nullptr, FALSE);
          return 0;
        }
        const int r = rowAt(y);
        if (r >= 0) {
          const Row row = rows[r];
          if (row.summary) {
            // a router's row: open or close it (and pick it out on the graphs)
            const uint64_t id = groups[row.group].id;
            if (opened.count(id)) opened.erase(id); else opened.insert(id);
            selectedGroup = id;
            selected = 0;
          }
          else if (row.key == selected) selected = 0;
          else { selected = row.key; selectedGroup = 0; }
        }
        else if (y > tableBottom) { selected = 0; selectedGroup = 0; }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      case WM_MOUSEMOVE: {
        const float y = (float) GET_Y_LPARAM(lParam);
        if (dragging) {
          split = std::max(0.15f, std::min(0.85f, (y - top) / std::max(1.0f, H - top)));
          InvalidateRect(hwnd, nullptr, FALSE);
        }
        SetCursor(LoadCursor(nullptr, dragging || (y >= tableBottom - 2 * ui && y <= tableBottom + 6 * ui) ? IDC_SIZENS : IDC_ARROW));
        return 0;
      }
      case WM_LBUTTONUP:
        if (dragging) { dragging = false; ReleaseCapture(); saveSettings(); }
        return 0;
      case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) return TRUE;   // (WM_MOUSEMOVE sets it)
        break;
      case WM_MOUSEWHEEL:
        scroll -= GET_WHEEL_DELTA_WPARAM(wParam) / 120 * 3;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      case WM_RBUTTONUP:
        contextMenu(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
      case WM_KEYDOWN: {
        int i = -1;
        for (size_t k = 0; k < rows.size(); ++k)
          if (rows[k].summary ? (selectedGroup && groups[rows[k].group].id == selectedGroup && !selected)
                              : (selected && rows[k].key == selected)) i = (int) k;
        if (wParam == VK_DOWN || wParam == VK_UP) {
          if (!rows.empty()) {
            i = std::max(0, std::min((int) rows.size() - 1, i + (wParam == VK_DOWN ? 1 : -1)));
            selectRow(i);
            if (i < scroll) scroll = i;
            if (i >= scroll + visibleRows()) scroll = i - visibleRows() + 1;
          }
        }
        else if ((wParam == VK_RIGHT || wParam == VK_RETURN) && i >= 0 && rows[i].summary) opened.insert(groups[rows[i].group].id);
        else if (wParam == VK_LEFT && i >= 0) {
          const uint64_t id = groups[rows[i].group].id;
          opened.erase(id);
          if (!rows[i].summary && groups[rows[i].group].members.size() > 1) { selectedGroup = id; selected = 0; }
        }
        else if (wParam == VK_ESCAPE) { selected = 0; selectedGroup = 0; }
        else if (wParam == VK_F5) wlan::scan();
        else if (wParam == VK_PRIOR) scroll -= visibleRows();
        else if (wParam == VK_NEXT) scroll += visibleRows();
        else if (wParam == 'G') { grouped = !grouped; scroll = 0; saveSettings(); }
        else if (wParam == 'T') { timeView = !timeView; saveSettings(); }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      case WM_COMMAND:
        if ((HWND) lParam == filterBox && HIWORD(wParam) == EN_CHANGE) {
          wchar_t t[128];
          GetWindowTextW(filterBox, t, _countof(t));
          filter = t;
          scroll = 0;
          InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
      case WM_CTLCOLOREDIT:
        if ((HWND) lParam == filterBox) {
          HDC dc = (HDC) wParam;
          SetTextColor(dc, RGB(0xE8, 0xEE, 0xF4));
          SetBkColor(dc, RGB(0x24, 0x30, 0x3C));
          if (!editBrush) editBrush = CreateSolidBrush(RGB(0x24, 0x30, 0x3C));
          return (LRESULT) editBrush;
        }
        break;
      case WM_DESTROY:
        saveSettings();
        wlan::stop();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
  }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int show)
{
  if (commandLine && wcsstr(commandLine, L"--demo")) {
    demo = true;
    unsigned int r1 = 0, r2 = 0;
    rand_s(&r1); rand_s(&r2);
    demoSalt = ((uint64_t) r1 << 32) | r2;
    makeDemoTables();
  }
  D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d);
  DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown **) &dwrite);

  wchar_t * appData = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
    std::wstring dir = std::wstring(appData) + L"\\WiFiScope";
    CreateDirectoryW(dir.c_str(), nullptr);
    settingsFile = dir + L"\\WiFiScope.ini";
    CoTaskMemFree(appData);
  }
  {
    // "Signal 1": the column's name, and 1 for descending
    const std::wstring s = setting(L"Sort");
    const size_t space = s.rfind(L' ');
    if (space != std::wstring::npos)
      for (int c = 0; c < ColCount; ++c)
        if (s.compare(0, space, columns[c].name) == 0) { sortColumn = c; sortDown = s.substr(space + 1) != L"0"; }
  }
  {
    int a = 1, b = 1, c = 1;
    if (swscanf_s(setting(L"Bands").c_str(), L"%d %d %d", &a, &b, &c) == 3) { showBand[0] = a; showBand[1] = b; showBand[2] = c; }
    const std::wstring s = setting(L"Split");
    if (!s.empty()) split = std::max(0.15f, std::min(0.85f, (float) _wtof(s.c_str())));
    grouped = setting(L"Grouped") == L"1";
    timeView = setting(L"Graph") == L"time";
  }

  WNDCLASSEXW wc = { sizeof(wc) };
  wc.lpfnWndProc = proc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
  wc.hIconSm = wc.hIcon;
  wc.lpszClassName = L"WiFiScope";
  RegisterClassExW(&wc);

  int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = 1400, h = 900, maximized = 0;
  swscanf_s(setting(L"Window").c_str(), L"%d %d %d %d %d", &x, &y, &w, &h, &maximized);
  if (x != CW_USEDEFAULT) { w -= x; h -= y; }
  HWND hwnd = CreateWindowExW(0, L"WiFiScope", L"WiFiScope", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              x, y, w, h, nullptr, nullptr, instance, nullptr);
  ui = GetDpiForWindow(hwnd) / 96.0f;
  if (x == CW_USEDEFAULT)
    SetWindowPos(hwnd, nullptr, 0, 0, (int) (1360 * ui), (int) (860 * ui), SWP_NOMOVE | SWP_NOZORDER);
  BOOL dark = TRUE;
  DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
  makeFonts();

  filterBox = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd, nullptr, instance, nullptr);
  SendMessageW(filterBox, EM_SETCUEBANNER, TRUE, (LPARAM) L"Filter by name, BSSID or vendor");
  HFONT editFont = CreateFontW((int) (-13 * ui), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
  SendMessageW(filterBox, WM_SETFONT, (WPARAM) editFont, TRUE);

  if (!wlan::start(hwnd, WM_SCANNED))
    MessageBoxW(hwnd, L"Windows' Wi-Fi service (WLAN AutoConfig) isn't available, or there is no Wi-Fi adapter.", L"WiFiScope", MB_ICONWARNING);
  adapter = wlan::adapterName();
  hasSixGhz = adapter.find(L"6E") != std::wstring::npos || adapter.find(L"BE2") != std::wstring::npos ||
              adapter.find(L"Wi-Fi 7") != std::wstring::npos;
  refresh();
  SetTimer(hwnd, TimerScan, ScanEveryMs, nullptr);
  SetTimer(hwnd, TimerRefresh, RefreshEveryMs, nullptr);

  ShowWindow(hwnd, maximized ? SW_SHOWMAXIMIZED : show);
  MSG m;
  while (GetMessageW(&m, nullptr, 0, 0) > 0) {
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
  return 0;
}
