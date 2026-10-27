// WiFiScope: scanning (Native Wifi API) and decoding the beacons' information
// elements (IEEE 802.11-2020 and 802.11be: HT, VHT, HE, EHT, RSN, BSS Load,
// Multi-Link).
#include "Wlan.h"

#include <wlanapi.h>
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <mutex>

#pragma comment(lib, "wlanapi.lib")

namespace wlan {

  namespace {
    HANDLE client = nullptr;
    HWND notifyWindow = nullptr;
    UINT notifyMessage = 0;
    std::mutex lock;

    void CALLBACK onNotification(PWLAN_NOTIFICATION_DATA data, PVOID)
    {
      if (data->NotificationSource == WLAN_NOTIFICATION_SOURCE_ACM &&
          (data->NotificationCode == wlan_notification_acm_scan_complete ||
           data->NotificationCode == wlan_notification_acm_scan_fail ||
           data->NotificationCode == wlan_notification_acm_scan_list_refresh))
        PostMessageW(notifyWindow, notifyMessage, 0, 0);
    }

    std::wstring wide(const char * s, size_t n)
    {
      if (n == 0) return L"";
      int w = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, (int) n, nullptr, 0);
      if (w <= 0) {
        // (not UTF-8: as Latin-1)
        std::wstring out;
        for (size_t i = 0; i < n; ++i) out += (wchar_t) (uint8_t) s[i];
        return out;
      }
      std::wstring out(w, L'\0');
      MultiByteToWideChar(CP_UTF8, 0, s, (int) n, &out[0], w);
      return out;
    }

    // per spatial stream, the shortest guard interval
    double htRate(int width) { return width >= 40 ? 150.0 : 72.2; }                     // MCS 7
    double vhtRate(int width, int maxMcs)                                                // MCS 7, 8, 9
    {
      static const double r[4][3] = { { 72.2, 86.7, 86.7 }, { 150, 180, 200 }, { 325, 390, 433.3 }, { 650, 780, 866.7 } };
      int w = width >= 160 ? 3 : width >= 80 ? 2 : width >= 40 ? 1 : 0;
      return r[w][std::max(0, std::min(2, maxMcs - 7))];
    }
    double heRate(int width, int maxMcs)                                                 // MCS 7, 9, 11 (0.8 us)
    {
      static const double r[4][3] = { { 86.0, 114.7, 143.4 }, { 172.1, 229.4, 286.8 }, { 360.3, 480.4, 600.5 }, { 720.6, 960.8, 1201.0 } };
      int w = width >= 160 ? 3 : width >= 80 ? 2 : width >= 40 ? 1 : 0;
      return r[w][maxMcs >= 11 ? 2 : maxMcs >= 9 ? 1 : 0];
    }
    double ehtRate(int width)                                                            // MCS 13 (0.8 us)
    {
      return width >= 320 ? 2882.4 : width >= 160 ? 1441.2 : width >= 80 ? 720.6 : width >= 40 ? 344.1 : 172.1;
    }

    // streams and highest MCS in a VHT / HE MCS map (2 bits a stream; 3: not supported)
    void mcsMap(uint16_t map, int & streams, int & maxCode)
    {
      streams = 0; maxCode = -1;
      for (int s = 0; s < 8; ++s) {
        int code = (map >> (2 * s)) & 3;
        if (code != 3) { streams = s + 1; maxCode = std::max(maxCode, code); }
      }
    }

    uint16_t le16(const uint8_t * p) { return (uint16_t) (p[0] | (p[1] << 8)); }
  }

  int frequencyOf(int band, int channel)
  {
    if (band == 2) return channel == 14 ? 2484 : 2407 + 5 * channel;
    if (band == 6) return channel == 2 ? 5935 : 5950 + 5 * channel;
    return 5000 + 5 * channel;
  }

  void decode(const uint8_t * ies, size_t size, uint16_t capabilities, Bss & b)
  {
    bool ht = false, vht = false, he = false, eht = false, rsn = false, wpa1 = false;
    bool htWide = false;                     // HT operation: 40 MHz, secondary above (+) or below (-)
    int htSecondary = 0;
    int vhtWidth = -1, vhtCcfs0 = 0, vhtCcfs1 = 0;
    int heWidth = -1, heCcfs0 = 0, heCcfs1 = 0, hePrimary = 0;     // (6 GHz operation information)
    int ehtWidth = -1, ehtCcfs0 = 0, ehtCcfs1 = 0;
    int htStreams = 1, vhtStreams = 0, vhtMcs = 9, heStreams = 0, heMcs = 11;
    std::vector<double> basic, all;
    bool akmPsk = false, akmSae = false, akm8021x = false, akmSuiteB = false, akmOwe = false;
    bool hasB = false, hasG = false;
    b.details = size > 0;

    size_t at = 0;
    while (at + 2 <= size) {
      const uint8_t id = ies[at], len = ies[at + 1];
      const uint8_t * d = ies + at + 2;
      if (at + 2 + len > size) break;
      switch (id) {
        case 1: case 50:                     // supported rates, extended supported rates (500 kbps)
          for (int i = 0; i < len; ++i) {
            const double rate = (d[i] & 0x7F) * 0.5;
            if ((d[i] & 0x7F) == 0x7F || (d[i] & 0x7F) == 0x7E) continue;   // (membership selectors: SAE H2E, HT/VHT PHY)
            all.push_back(rate);
            if (d[i] & 0x80) basic.push_back(rate);
            if (rate == 1 || rate == 2 || rate == 5.5 || rate == 11) hasB = true;
            else hasG = true;
          }
          break;
        case 7:                               // country
          if (len >= 2) b.country.assign((const char *) d, 2);
          break;
        case 11:                              // BSS Load: stations, channel utilization (of 255)
          if (len >= 3) {
            b.stations = le16(d);
            b.utilization = (int) std::lround(d[2] * 100.0 / 255.0);
          }
          break;
        case 45:                              // HT capabilities: the supported MCS set at 3
          ht = true;
          if (len >= 7) {
            htStreams = 0;
            for (int s = 0; s < 4; ++s) if (d[3 + s]) htStreams = s + 1;
            htStreams = std::max(1, htStreams);
          }
          break;
        case 61:                              // HT operation
          if (len >= 2) {
            const int offset = d[1] & 3;
            htWide = (d[1] & 4) && (offset == 1 || offset == 3);
            htSecondary = offset == 1 ? 1 : offset == 3 ? -1 : 0;
          }
          break;
        case 48: {                            // RSN: group, pairwise suites, AKM suites
          rsn = true;
          size_t p = 2 + 4;                   // (version, group cipher)
          if (len >= p + 2) {
            const int pairwise = le16(d + p); p += 2 + 4 * pairwise;
            if (len >= p + 2) {
              const int akms = le16(d + p); p += 2;
              for (int i = 0; i < akms && p + 4 <= len; ++i, p += 4) {
                if (d[p] != 0x00 || d[p + 1] != 0x0F || d[p + 2] != 0xAC) continue;
                switch (d[p + 3]) {
                  case 1: case 3: case 5: akm8021x = true; break;
                  case 11: case 12: case 13: akmSuiteB = true; break;
                  case 2: case 4: case 6: akmPsk = true; break;
                  case 8: case 9: case 24: case 25: akmSae = true; break;
                  case 18: akmOwe = true; break;
                }
              }
            }
          }
          break;
        }
        case 221:                             // vendor: Microsoft WPA (version 1)
          if (len >= 4 && d[0] == 0x00 && d[1] == 0x50 && d[2] == 0xF2 && d[3] == 1) wpa1 = true;
          break;
        case 191:                             // VHT capabilities: the Rx MCS map at 4
          vht = true;
          if (len >= 6) {
            int code; mcsMap(le16(d + 4), vhtStreams, code);
            vhtMcs = 7 + std::max(0, code);
          }
          break;
        case 192:                             // VHT operation: width, CCFS0, CCFS1
          if (len >= 3) { vhtWidth = d[0]; vhtCcfs0 = d[1]; vhtCcfs1 = d[2]; }
          break;
        case 255: {                           // extension elements
          if (len < 1) break;
          const uint8_t ext = d[0];
          const uint8_t * e = d + 1;
          const int elen = len - 1;
          if (ext == 35) {                    // HE capabilities: MAC (6), PHY (11), then MCS maps
            he = true;
            if (elen >= 19) {
              int code; mcsMap(le16(e + 17), heStreams, code);
              heMcs = code >= 2 ? 11 : code == 1 ? 9 : 7;
            }
          }
          else if (ext == 36 && elen >= 6) {  // HE operation
            const uint32_t params = e[0] | (e[1] << 8) | (e[2] << 16);
            int p = 6;
            if (params & (1u << 14)) p += 3;  // (VHT operation information)
            if (params & (1u << 15)) p += 1;  // (max co-hosted BSSID indicator)
            if ((params & (1u << 17)) && elen >= p + 5) {   // 6 GHz operation information
              hePrimary = e[p];
              heWidth = e[p + 1] & 3;
              heCcfs0 = e[p + 2];
              heCcfs1 = e[p + 3];
            }
          }
          else if (ext == 108) eht = true;    // EHT capabilities
          else if (ext == 106 && elen >= 5) { // EHT operation
            eht = true;
            if ((e[0] & 1) && elen >= 8) {
              ehtWidth = e[5] & 7;
              ehtCcfs0 = e[6];
              ehtCcfs1 = e[7];
            }
          }
          else if (ext == 107 && elen >= 9) { // Multi-Link: control (2), common info length (1), MLD address
            if ((e[0] & 7) == 0) {            // (basic multi-link element)
              b.multiLink = true;
              memcpy(b.mld, e + 3, 6);
            }
          }
          break;
        }
      }
      at += 2 + len;
    }

    // the channel: 20 MHz unless the operation elements say more; the widest one wins
    b.width = 20;
    b.center = b.frequency;
    b.split = false;
    auto centerOf = [&](int ccfs) { return frequencyOf(b.band, ccfs); };
    if (ht && htWide && b.band != 6) {
      b.width = 40;
      b.center = b.frequency + 10 * htSecondary;
    }
    if (vhtWidth >= 1 && b.band == 5) {
      if (vhtWidth == 1 && vhtCcfs1 != 0 && std::abs(vhtCcfs1 - vhtCcfs0) == 8) { b.width = 160; b.center = centerOf(vhtCcfs1); }
      else if (vhtWidth == 1 && vhtCcfs1 != 0 && std::abs(vhtCcfs1 - vhtCcfs0) > 16) {
        b.width = 160; b.split = true; b.center = centerOf(vhtCcfs0); b.center2 = centerOf(vhtCcfs1);
      }
      else if (vhtWidth == 2) { b.width = 160; b.center = centerOf(vhtCcfs0); }
      else if (vhtWidth == 3) { b.width = 160; b.split = true; b.center = centerOf(vhtCcfs0); b.center2 = centerOf(vhtCcfs1); }
      else { b.width = 80; b.center = centerOf(vhtCcfs0); }
    }
    if (heWidth >= 0 && b.band == 6) {
      static const int widths[] = { 20, 40, 80, 160 };
      b.width = widths[heWidth];
      b.center = centerOf(heCcfs0);
      if (heWidth == 3 && heCcfs1 != 0) {
        if (std::abs(heCcfs1 - heCcfs0) == 8) b.center = centerOf(heCcfs1);
        else { b.split = true; b.center2 = centerOf(heCcfs1); }
      }
      if (hePrimary) b.channel = hePrimary;
    }
    if (ehtWidth >= 0) {
      static const int widths[] = { 20, 40, 80, 160, 320, 320, 320, 320 };
      const int w = widths[ehtWidth];
      if (w > b.width) {
        b.width = w;
        b.split = false;
        b.center = centerOf(w >= 160 ? ehtCcfs1 : ehtCcfs0);
      }
    }

    // the generation and the PHY list
    b.generation = eht ? 7 : he ? 6 : vht ? 5 : ht ? 4 : 0;
    std::wstring phy;
    auto add = [&](const wchar_t * s) { if (!phy.empty()) phy += L", "; phy += s; };
    if (b.band == 2) { if (hasB) add(L"b"); if (hasG) add(L"g"); }
    else if (b.band == 5) add(L"a");
    if (ht) add(L"n");
    if (vht) add(L"ac");
    if (he) add(L"ax");
    if (eht) add(L"be");
    b.phy = phy;

    // the most it could carry: its widest operating channel and its streams
    if (eht) { b.streams = std::max(1, heStreams); b.maxRate = ehtRate(b.width) * b.streams; }
    else if (he) { b.streams = std::max(1, heStreams); b.maxRate = heRate(b.width, heMcs) * b.streams; }
    else if (vht) { b.streams = std::max(1, vhtStreams); b.maxRate = vhtRate(b.width, vhtMcs) * b.streams; }
    else if (ht) { b.streams = htStreams; b.maxRate = htRate(b.width) * b.streams; }
    else { b.streams = 1; b.maxRate = all.empty() ? 0 : *std::max_element(all.begin(), all.end()); }
    b.minRate = !basic.empty() ? *std::min_element(basic.begin(), basic.end())
              : !all.empty() ? *std::min_element(all.begin(), all.end()) : 6.0;

    // security
    if (rsn) {
      if (akmOwe && !akmPsk && !akmSae && !akm8021x) b.security = L"OWE";
      else if (akmSuiteB) b.security = L"WPA3-Enterprise";
      else if (akm8021x) b.security = L"WPA2-Enterprise";
      else if (akmSae && akmPsk) b.security = L"WPA2/WPA3";
      else if (akmSae) b.security = L"WPA3";
      else if (akmPsk) b.security = wpa1 ? L"WPA/WPA2" : L"WPA2";
      else b.security = L"WPA2";
    }
    else if (wpa1) b.security = L"WPA";
    else if (capabilities & 0x10) b.security = L"WEP";
    else if (!b.details) b.security = L"?";
    else b.security = L"Open";
  }

  bool start(HWND window, UINT message)
  {
    notifyWindow = window;
    notifyMessage = message;
    DWORD version = 0;
    if (WlanOpenHandle(2, nullptr, &version, &client) != ERROR_SUCCESS) { client = nullptr; return false; }
    WlanRegisterNotification(client, WLAN_NOTIFICATION_SOURCE_ACM, TRUE, onNotification, nullptr, nullptr, nullptr);
    scan();
    return true;
  }

  void stop()
  {
    if (!client) return;
    WlanRegisterNotification(client, WLAN_NOTIFICATION_SOURCE_NONE, TRUE, nullptr, nullptr, nullptr, nullptr);
    WlanCloseHandle(client, nullptr);
    client = nullptr;
  }

  void scan()
  {
    if (!client) return;
    PWLAN_INTERFACE_INFO_LIST list = nullptr;
    if (WlanEnumInterfaces(client, nullptr, &list) != ERROR_SUCCESS) return;
    for (DWORD i = 0; i < list->dwNumberOfItems; ++i)
      WlanScan(client, &list->InterfaceInfo[i].InterfaceGuid, nullptr, nullptr, nullptr);
    WlanFreeMemory(list);
  }

  std::wstring adapterName()
  {
    std::wstring name;
    PWLAN_INTERFACE_INFO_LIST list = nullptr;
    if (client && WlanEnumInterfaces(client, nullptr, &list) == ERROR_SUCCESS) {
      if (list->dwNumberOfItems > 0) name = list->InterfaceInfo[0].strInterfaceDescription;
      WlanFreeMemory(list);
    }
    return name;
  }

  std::wstring connectedBssid()
  {
    std::wstring out;
    PWLAN_INTERFACE_INFO_LIST list = nullptr;
    if (!client || WlanEnumInterfaces(client, nullptr, &list) != ERROR_SUCCESS) return out;
    for (DWORD i = 0; i < list->dwNumberOfItems && out.empty(); ++i) {
      if (list->InterfaceInfo[i].isState != wlan_interface_state_connected) continue;
      DWORD size = 0;
      PWLAN_CONNECTION_ATTRIBUTES attributes = nullptr;
      if (WlanQueryInterface(client, &list->InterfaceInfo[i].InterfaceGuid, wlan_intf_opcode_current_connection,
                             nullptr, &size, (PVOID *) &attributes, nullptr) == ERROR_SUCCESS) {
        const UCHAR * m = attributes->wlanAssociationAttributes.dot11Bssid;
        wchar_t text[24];
        swprintf_s(text, L"%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
        out = text;
        WlanFreeMemory(attributes);
      }
    }
    WlanFreeMemory(list);
    return out;
  }

  std::vector<Bss> networks()
  {
    std::vector<Bss> out;
    if (!client) return out;
    std::lock_guard<std::mutex> guard(lock);
    PWLAN_INTERFACE_INFO_LIST list = nullptr;
    if (WlanEnumInterfaces(client, nullptr, &list) != ERROR_SUCCESS) return out;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const ULONGLONG now = ((ULONGLONG) ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
      PWLAN_BSS_LIST bsses = nullptr;
      if (WlanGetNetworkBssList(client, &list->InterfaceInfo[i].InterfaceGuid, nullptr, dot11_BSS_type_any,
                                FALSE, nullptr, &bsses) != ERROR_SUCCESS)
        continue;
      for (DWORD k = 0; k < bsses->dwNumberOfItems; ++k) {
        const WLAN_BSS_ENTRY & e = bsses->wlanBssEntries[k];
        Bss b;
        memcpy(b.mac, e.dot11Bssid, 6);
        b.ssid = wide((const char *) e.dot11Ssid.ucSSID, std::min<ULONG>(e.dot11Ssid.uSSIDLength, 32));
        bool blank = true;
        for (wchar_t c : b.ssid) if (c != 0) blank = false;
        if (blank) b.ssid.clear();
        b.rssi = e.lRssi;
        b.frequency = (int) (e.ulChCenterFrequency / 1000);
        b.band = b.frequency < 3000 ? 2 : b.frequency < 5925 ? 5 : 6;
        b.channel = b.band == 2 ? (b.frequency == 2484 ? 14 : (b.frequency - 2407) / 5)
                  : b.band == 6 ? (b.frequency == 5935 ? 2 : (b.frequency - 5950) / 5)
                  : (b.frequency - 5000) / 5;
        decode((const uint8_t *) &e + e.ulIeOffset, e.ulIeSize, e.usCapabilityInformation, b);
        b.ageMs = e.ullHostTimestamp && now > e.ullHostTimestamp ? (int) std::min<ULONGLONG>((now - e.ullHostTimestamp) / 10000, INT_MAX) : 0;
        out.push_back(std::move(b));
      }
      WlanFreeMemory(bsses);
    }
    WlanFreeMemory(list);
    return out;
  }
}
