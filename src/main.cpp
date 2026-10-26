// WiFiScope: a Wi-Fi scanner for Windows - every network the adapter hears, in
// a table and on graphs of the 2.4, 5 and 6 GHz channels.
//
// Scanning: Windows' Native Wifi API (Wlan.cpp) asks for a scan every few
// seconds; what each network's beacon says (width, generation, security,
// rates, load, Wi-Fi 7 multi-link) is decoded there.  Drawing: Direct2D and
// DirectWrite, in pixels; `ui` scales what needs it to the monitor's DPI.

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
#include <deque>
#include <map>
#include <string>
#include <vector>

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
  std::vector<uint64_t> rows;              // the table's rows, filtered and sorted
  std::wstring connected;                  // our network's BSSID
  std::wstring adapter;

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
    { L"SSID", 200, false }, { L"BSSID", 136, false }, { L"Vendor", 170, false }, { L"Signal", 110, false }, { L"Channel", 78, true },
    { L"Width", 70, true }, { L"Band", 62, true }, { L"Wi-Fi", 64, true }, { L"Security", 132, false },
    { L"Max rate", 84, true }, { L"Min", 52, true }, { L"Clients", 62, true }, { L"Busy", 54, true }, { L"Seen", 58, true },
  };
  int sortColumn = ColSignal;
  bool sortDown = true;                    // (signal: strongest first)
  bool showBand[3] = { true, true, true }; // 2.4, 5, 6 GHz
  uint64_t selected = 0;                   // 0: none
  int scroll = 0;                          // the table's first row
  float split = 0.52f;                     // the table's share of the height
  bool dragging = false;
  std::wstring filter;
  std::wstring settingsFile;
  bool hasSixGhz = false;

  // the layout, set by layout()
  float W = 0, H = 0, top = 0, tableTop = 0, tableBottom = 0, rowH = 0, headH = 0, graphTop = 0;
  D2D1_RECT_F bandPill[3];
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

  // ------------------------------------------------------------- settings
  void saveSettings()
  {
    auto put = [](const wchar_t * k, const std::wstring & v) { WritePrivateProfileStringW(L"WiFiScope", k, v.c_str(), settingsFile.c_str()); };
    put(L"Sort", format(L"%s %d", columns[sortColumn].name, sortDown ? 1 : 0));
    put(L"Bands", format(L"%d %d %d", showBand[0], showBand[1], showBand[2]));
    put(L"Split", format(L"%.3f", split));
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
  bool sibling(const uint8_t * a, const uint8_t * b)
  {
    if (a[1] != b[1] || a[2] != b[2]) return false;
    int same = 0;
    for (int i = 1; i < 6; ++i) same += a[i] == b[i];
    return same >= 4;
  }

  void refresh()
  {
    const ULONGLONG now = GetTickCount64();
    for (const wlan::Bss & b : wlan::networks()) {
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

    // the makers.  A made-up ("locally administered") address has none of its
    // own: if another network's registered address shares its second and third
    // bytes and at least four of its last five, it's that router's extra network
    // (guest, IoT, mesh - routers make those addresses from their own by
    // changing a byte or two); otherwise it's a private address (a phone's
    // hotspot and the like)
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

  void sortRows()
  {
    const std::wstring f = lower(filter);
    rows.clear();
    for (auto & [key, e] : entries) {
      if (!showBand[bandIndex(e.bss.band)]) continue;
      if (!f.empty() && lower(e.bss.ssid).find(f) == std::wstring::npos && macText(e.bss.mac).find(f) == std::wstring::npos &&
          lower(e.vendor).find(f) == std::wstring::npos)
        continue;
      rows.push_back(key);
    }
    auto value = [](const Entry & e, int c) -> double {
      const wlan::Bss & b = e.bss;
      switch (c) {
        case ColSignal: return b.rssi;
        case ColChannel: return b.band * 1000 + b.channel;
        case ColWidth: return b.width;
        case ColBand: return b.band;
        case ColWifi: return b.generation * 10 + (b.band == 6);
        case ColMax: return b.maxRate;
        case ColMin: return b.minRate;
        case ColClients: return b.stations;
        case ColUtil: return b.utilization;
        case ColSeen: return -(double) e.heard;
        default: return 0;
      }
    };
    std::sort(rows.begin(), rows.end(), [&](uint64_t a, uint64_t b) {
      const Entry & x = entries[a], & y = entries[b];
      int order = 0;
      if (sortColumn == ColSsid) {
        const std::wstring sx = lower(x.bss.ssid), sy = lower(y.bss.ssid);
        if (sx != sy) order = sx.empty() ? 1 : sy.empty() ? -1 : (sx < sy ? -1 : 1);   // (hidden ones last)
      }
      else if (sortColumn == ColBssid) order = a < b ? -1 : a > b ? 1 : 0;
      else if (sortColumn == ColVendor) {
        const std::wstring vx = lower(x.vendor), vy = lower(y.vendor);
        if (vx != vy) order = vx.empty() ? 1 : vy.empty() ? -1 : (vx < vy ? -1 : 1);
      }
      else if (sortColumn == ColSecurity) order = x.bss.security < y.bss.security ? -1 : x.bss.security > y.bss.security ? 1 : 0;
      else {
        const double vx = value(x, sortColumn), vy = value(y, sortColumn);
        order = vx < vy ? -1 : vx > vy ? 1 : 0;
      }
      if (order == 0) return x.bss.rssi > y.bss.rssi || (x.bss.rssi == y.bss.rssi && a < b);
      return sortDown ? order > 0 : order < 0;
    });
  }

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

    // the band switches, top right
    float x = W - 10 * ui;
    const wchar_t * names[3] = { L"6 GHz", L"5 GHz", L"2.4 GHz" };
    for (int i = 2; i >= 0; --i) {
      const float w = 64 * ui;
      bandPill[i] = D2D1::RectF(x - w, 10 * ui, x, 34 * ui);
      x -= w + 6 * ui;
      (void) names;
    }
    if (filterBox) {
      const int fw = (int) (240 * ui);
      MoveWindow(filterBox, (int) (x - fw - 10 * ui), (int) (11 * ui), fw, (int) (22 * ui), TRUE);
    }

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

  void drawTop()
  {
    brush->SetColor(rgb(0x141A21));
    target->FillRectangle(D2D1::RectF(0, 0, W, top), brush);
    text(L"WiFiScope", fontTitle, D2D1::RectF(12 * ui, 8 * ui, 200 * ui, 36 * ui), 0xFFFFFF);
    int fading = 0;
    const ULONGLONG now = GetTickCount64();
    for (auto & [k, e] : entries) if (e.heard + FadeAfterMs < now) ++fading;
    std::wstring status = format(L"%d networks", (int) entries.size() - fading);
    if (!adapter.empty()) status += L"   •   " + adapter;
    text(status, fontSmall, D2D1::RectF(130 * ui, 15 * ui, bandPill[0].left - 260 * ui, 34 * ui), 0x9FB0C2);
    const wchar_t * names[3] = { L"2.4 GHz", L"5 GHz", L"6 GHz" };
    for (int i = 0; i < 3; ++i) {
      brush->SetColor(rgb(showBand[i] ? 0x2E86DE : 0x2A3540));
      target->FillRoundedRectangle(D2D1::RoundedRect(bandPill[i], 11 * ui, 11 * ui), brush);
      text(names[i], fontSmall, D2D1::RectF(bandPill[i].left, bandPill[i].top + 4 * ui, bandPill[i].right, bandPill[i].bottom),
           showBand[i] ? 0xFFFFFF : 0x8FA3B8, 1, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
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
    target->PushAxisAlignedClip(D2D1::RectF(0, tableTop + headH, W, tableBottom), D2D1_ANTIALIAS_MODE_ALIASED);
    for (int i = 0; i < visible && scroll + i < (int) rows.size(); ++i) {
      const uint64_t key = rows[scroll + i];
      const Entry & e = entries[key];
      const wlan::Bss & b = e.bss;
      const float y = tableTop + headH + i * rowH;
      const bool faded = e.heard + FadeAfterMs < now;
      const bool ours = macText(b.mac) == connected;
      if (key == selected) {
        brush->SetColor(rgb(0x2E86DE, 0.35f));
        target->FillRectangle(D2D1::RectF(0, y, W, y + rowH), brush);
      }
      else if (i % 2) {
        brush->SetColor(rgb(0xFFFFFF, 0.025f));
        target->FillRectangle(D2D1::RectF(0, y, W, y + rowH), brush);
      }
      brush->SetColor(rgb(e.color, faded ? 0.35f : 1));
      target->FillRectangle(D2D1::RectF(6 * ui, y + 4 * ui, 14 * ui, y + rowH - 4 * ui), brush);
      const float alpha = faded ? 0.45f : 1;
      const uint32_t fg = 0xE8EEF4, dim = 0x9FB0C2;
      auto cell = [&](int c, const std::wstring & s, uint32_t color, IDWriteTextFormat * f = nullptr) {
        const float x = columnX(c), w = columns[c].width * ui;
        text(s, f ? f : fontText, D2D1::RectF(x + 4 * ui, y + 3 * ui, x + w - 6 * ui, y + rowH), color, alpha,
             columns[c].right ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
      };
      std::wstring name = b.ssid.empty() ? L"(hidden)" : b.ssid;
      if (ours) name = L"● " + name;
      cell(ColSsid, name, b.ssid.empty() ? dim : fg, ours ? fontBold : nullptr);
      cell(ColBssid, macText(b.mac) + (b.multiLink ? L" ≡" : L""), dim);
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
      cell(ColChannel, format(L"%d", b.channel), fg);
      cell(ColWidth, b.split ? L"80+80" : format(L"%d", b.width), fg);
      cell(ColBand, b.band == 2 ? L"2.4" : b.band == 5 ? L"5" : L"6", dim);
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

  void drawPane(const Pane & p)
  {
    const ULONGLONG now = GetTickCount64();
    const float x0 = p.x0, x1 = p.x1;
    const float y0 = graphTop + 20 * ui, y1 = H - 26 * ui;
    const float ax = x0 + 34 * ui;                                   // (dBm labels on the left)
    auto fx = [&](double mhz) { return (float) (ax + (mhz - p.f0) / (p.f1 - p.f0) * (x1 - ax)); };
    const double dbTop = -20, dbBottom = -100;
    auto fy = [&](double db) { return (float) (y0 + (dbTop - db) / (dbTop - dbBottom) * (y1 - y0)); };

    text(p.band == 2 ? L"2.4 GHz" : p.band == 5 ? L"5 GHz" : L"6 GHz", fontBold, D2D1::RectF(x0, graphTop, x1, y0), 0xB8C6D6);
    brush->SetColor(rgb(0x0E1318));
    target->FillRectangle(D2D1::RectF(ax, y0, x1, y1), brush);
    // grid: every 10 dBm, and the channels
    for (int db = -30; db >= -90; db -= 10) {
      brush->SetColor(rgb(0xFFFFFF, 0.07f));
      target->DrawLine(D2D1::Point2F(ax, fy(db)), D2D1::Point2F(x1, fy(db)), brush, 1);
      text(format(L"%d", db), fontSmall, D2D1::RectF(x0, fy(db) - 8 * ui, ax - 4 * ui, fy(db) + 8 * ui), 0x8FA3B8, 1, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
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
    // (strongest drawn last, on top; the selected one last of all)
    std::vector<uint64_t> order;
    for (uint64_t k : rows) if (entries[k].bss.band == p.band) order.push_back(k);
    std::sort(order.begin(), order.end(), [&](uint64_t a, uint64_t b) {
      if ((a == selected) != (b == selected)) return b == selected;
      return entries[a].bss.rssi < entries[b].bss.rssi;
    });
    struct Label { uint64_t key; float cx, yt, alpha; };
    std::vector<Label> names;
    target->PushAxisAlignedClip(D2D1::RectF(ax, y0 - 18 * ui, x1, y1), D2D1_ANTIALIAS_MODE_ALIASED);
    for (uint64_t k : order) {
      const Entry & e = entries[k];
      const wlan::Bss & b = e.bss;
      const bool faded = e.heard + FadeAfterMs < now;
      const bool dimmed = selected && k != selected;
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
        brush->SetColor(rgb(e.color, 0.10f * alpha + (k == selected ? 0.12f : 0)));
        target->FillGeometry(g, brush);
        brush->SetColor(rgb(e.color, alpha));
        target->DrawGeometry(g, brush, (k == selected ? 2.5f : 1.3f) * ui);
        g->Release();
        // its name, placed after all the shapes are drawn
        if (!dimmed || k == selected)
          names.push_back({ k, (lt + rt) / 2, yt, alpha });
      };
      if (b.split) { shape(b.center, 80); shape(b.center2, 80); }
      else shape(b.center, b.width);
    }

    // the names: the strongest (and the selected) placed first, just above
    // their shapes; one that would overlap another moves up a line, or down,
    // with a thin line to its shape when it has moved
    std::sort(names.begin(), names.end(), [&](const Label & a, const Label & b) {
      if ((a.key == selected) != (b.key == selected)) return a.key == selected;
      return a.yt < b.yt;
    });
    std::vector<D2D1_RECT_F> placed;
    const float lh = 15 * ui;
    for (const Label & l : names) {
      const Entry & e = entries[l.key];
      const std::wstring name = e.bss.ssid.empty() ? L"(hidden)" : e.bss.ssid;
      const float tw = textWidth(name, fontSmall) + 8 * ui;
      float left = std::max(ax + 1, std::min(x1 - tw - 1, l.cx - tw / 2));
      D2D1_RECT_F box = {};
      bool found = false;
      for (int step : { 0, -1, -2, -3, 1, 2, 3, 4, -4, 5, 6 }) {
        const float top = l.yt - 17 * ui + step * (lh + 1 * ui);
        if (top < y0 - 17 * ui || top + lh > y1) continue;
        const D2D1_RECT_F r = D2D1::RectF(left, top, left + tw, top + lh);
        bool clash = false;
        for (const auto & q : placed)
          if (r.left < q.right && q.left < r.right && r.top < q.bottom && q.top < r.bottom) { clash = true; break; }
        if (!clash) { box = r; found = true; break; }
      }
      if (!found) continue;                  // (no room: the table has it)
      placed.push_back(box);
      if (std::fabs(box.bottom - (l.yt - 2 * ui)) > 1 * ui) {
        brush->SetColor(rgb(e.color, 0.5f * l.alpha));
        const float ly = box.bottom < l.yt ? box.bottom : box.top;
        target->DrawLine(D2D1::Point2F(l.cx, ly), D2D1::Point2F(l.cx, l.yt), brush, 1);
      }
      brush->SetColor(rgb(0x0E1318, 0.8f * l.alpha));
      target->FillRectangle(box, brush);
      text(name, fontSmall, D2D1::RectF(box.left, box.top + 1 * ui, box.right, box.bottom), e.color, l.alpha, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
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
    for (const Pane & p : panes) drawPane(p);
    if (panes.empty())
      text(L"Choose a band at the top right.", fontText, D2D1::RectF(0, graphTop + 40 * ui, W, graphTop + 70 * ui), 0x8FA3B8, 1, DWRITE_TEXT_ALIGNMENT_CENTER);
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
                  (b.band == 2 ? L"2.4" : b.band == 5 ? L"5" : L"6"), wifiName(b).c_str(), b.security.c_str(),
                  rateText(b.maxRate).c_str(), b.phy.c_str());
  }

  void contextMenu(int x, int y)
  {
    const int r = rowAt((float) y);
    HMENU menu = CreatePopupMenu();
    if (r >= 0) {
      selected = rows[r];
      AppendMenuW(menu, MF_STRING, 1, L"Copy BSSID");
      AppendMenuW(menu, MF_STRING, 2, L"Copy this row");
    }
    AppendMenuW(menu, MF_STRING, 3, L"Copy all rows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 4, L"Scan now\tF5");
    POINT p = { x, y };
    ClientToScreen(window, &p);
    InvalidateRect(window, nullptr, FALSE);
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, window, nullptr);
    DestroyMenu(menu);
    const std::wstring header = L"SSID\tBSSID\tVendor\tdBm\tChannel\tWidth\tBand\tWi-Fi\tSecurity\tMax Mbps\tPHY\r\n";
    if (cmd == 1 && r >= 0) copyText(macText(entries[rows[r]].bss.mac));
    else if (cmd == 2 && r >= 0) copyText(header + rowText(entries[rows[r]]));
    else if (cmd == 3) {
      std::wstring all = header;
      for (uint64_t k : rows) all += rowText(entries[k]) + L"\r\n";
      copyText(all);
    }
    else if (cmd == 4) wlan::scan();
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
          if (x >= bandPill[i].left && x <= bandPill[i].right && y >= bandPill[i].top && y <= bandPill[i].bottom) {
            showBand[i] = !showBand[i];
            saveSettings();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
          }
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
        if (r >= 0) selected = rows[r] == selected ? 0 : rows[r];
        else if (y > tableBottom) selected = 0;
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
        for (size_t k = 0; k < rows.size(); ++k) if (rows[k] == selected) i = (int) k;
        if (wParam == VK_DOWN || wParam == VK_UP) {
          if (!rows.empty()) {
            i = std::max(0, std::min((int) rows.size() - 1, i + (wParam == VK_DOWN ? 1 : -1)));
            selected = rows[i];
            if (i < scroll) scroll = i;
            if (i >= scroll + visibleRows()) scroll = i - visibleRows() + 1;
          }
        }
        else if (wParam == VK_ESCAPE) selected = 0;
        else if (wParam == VK_F5) wlan::scan();
        else if (wParam == VK_PRIOR) scroll -= visibleRows();
        else if (wParam == VK_NEXT) scroll += visibleRows();
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

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
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
  SendMessageW(filterBox, EM_SETCUEBANNER, TRUE, (LPARAM) L"Filter by name or BSSID");
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
