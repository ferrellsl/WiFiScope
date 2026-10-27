// WiFiScope: the Wi-Fi networks the adapter hears, from Windows' Native Wifi API,
// with what their beacons say about them (channel width, Wi-Fi generation,
// security, rates, load, Wi-Fi 7 multi-link).
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace wlan {

  struct Bss {
    uint8_t mac[6] = {};
    std::wstring ssid;                 // empty: a hidden network
    int rssi = -100;                   // dBm
    int frequency = 0;                 // MHz, the primary 20 MHz channel's center
    int band = 0;                      // 2 (2.4 GHz), 5 or 6
    int channel = 0;                   // the primary channel's number
    int width = 20;                    // MHz: 20, 40, 80, 160 or 320
    int center = 0;                    // MHz, the center of the whole channel
    bool split = false;                // 80+80 MHz: two 80 MHz parts
    int center2 = 0;                   //   (the second part's center)
    int generation = 0;                // Wi-Fi 4 (n), 5 (ac), 6 (ax) or 7 (be); 0: older (a/b/g)
    std::wstring phy;                  // "b, g, n, ax" and the like
    std::wstring security;             // "Open", "WPA2", "WPA3", "WPA2/WPA3", "WPA2-Enterprise", ...
    int streams = 1;                   // spatial streams
    double maxRate = 0;                // Mbps, the most it could carry
    double minRate = 0;                // Mbps, the lowest basic rate
    int stations = -1;                 // clients, from its BSS Load element (-1: not given)
    int utilization = -1;              // percent of airtime busy, from the same (-1: not given)
    bool multiLink = false;            // Wi-Fi 7 multi-link (MLO)
    uint8_t mld[6] = {};               //   (the multi-link device's address, the same on all its radios)
    std::string country;               // "US", ...
    int ageMs = 0;                     // how long ago its last beacon was heard
    bool details = false;              // the report carried the beacon's elements (some reports
                                       //   don't: then width, security and the rest are unknown)
  };

  // Opens the API and starts scanning (a scan every few seconds); the window
  // gets `message` when a scan has finished. False if there is no Wi-Fi.
  bool start(HWND window, UINT message);
  void stop();
  void scan();                         // asks for a new scan (Windows may delay or skip it)
  std::vector<Bss> networks();         // what the adapter heard, all its interfaces
  std::wstring adapterName();
  std::wstring connectedBssid();       // "aa:bb:cc:dd:ee:ff" of the network we're on, or ""

  // channel numbers <-> MHz
  int frequencyOf(int band, int channel);

  // decodes a beacon's information elements into b (exposed for testing)
  void decode(const uint8_t * ies, size_t size, uint16_t capabilities, Bss & b);
}
