// capture: records one scan - each network's raw beacon elements, and what the
// Windows decoder (src/Wlan.cpp) makes of them - so the Android app can play
// the same beacons back and its decoder can be checked against this one
// (android/app/src/test/.../CaptureTest.java).  Usage: capture [file]
//
// The file holds the neighbors' names and addresses (which Wi-Fi maps can
// locate): keep it out of the repository (android/captures/ is ignored).
#include "../src/Wlan.h"

#include <wlanapi.h>
#include <cstdio>
#include <string>

static std::string utf8(const std::wstring & w)
{
  if (w.empty()) return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int) w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int) w.size(), &s[0], n, nullptr, nullptr);
  return s;
}

int wmain(int argc, wchar_t ** argv)
{
  FILE * out = stdout;
  if (argc > 1 && _wfopen_s(&out, argv[1], L"wb") != 0) { fwprintf(stderr, L"can't write %s\n", argv[1]); return 1; }

  HANDLE client = nullptr;
  DWORD version = 0;
  if (WlanOpenHandle(2, nullptr, &version, &client) != ERROR_SUCCESS) { fprintf(stderr, "no Wi-Fi service\n"); return 1; }
  PWLAN_INTERFACE_INFO_LIST list = nullptr;
  WlanEnumInterfaces(client, nullptr, &list);
  for (DWORD i = 0; i < list->dwNumberOfItems; ++i) WlanScan(client, &list->InterfaceInfo[i].InterfaceGuid, nullptr, nullptr, nullptr);
  Sleep(5000);

  wlan::start(nullptr, 0);
  fprintf(out, "#connected\t%s\n", utf8(wlan::connectedBssid()).c_str());
  fprintf(out, "#adapter\t%s\n", utf8(wlan::adapterName()).c_str());
  fprintf(out, "#fields\tbssid rssi MHz capability elements | ssid width center split center2 generation phy security streams maxRate minRate stations utilization multiLink mld country details\n");
  int count = 0;
  for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
    PWLAN_BSS_LIST bsses = nullptr;
    if (WlanGetNetworkBssList(client, &list->InterfaceInfo[i].InterfaceGuid, nullptr, dot11_BSS_type_any, FALSE, nullptr, &bsses) != ERROR_SUCCESS)
      continue;
    for (DWORD k = 0; k < bsses->dwNumberOfItems; ++k) {
      const WLAN_BSS_ENTRY & e = bsses->wlanBssEntries[k];
      const uint8_t * ies = (const uint8_t *) &e + e.ulIeOffset;
      const uint8_t * m = e.dot11Bssid;
      wlan::Bss b;
      memcpy(b.mac, m, 6);
      b.frequency = (int) (e.ulChCenterFrequency / 1000);
      b.band = b.frequency < 3000 ? 2 : b.frequency < 5925 ? 5 : 6;
      b.channel = b.band == 2 ? (b.frequency == 2484 ? 14 : (b.frequency - 2407) / 5)
                : b.band == 6 ? (b.frequency == 5935 ? 2 : (b.frequency - 5950) / 5)
                : (b.frequency - 5000) / 5;
      wlan::decode(ies, e.ulIeSize, e.usCapabilityInformation, b);
      fprintf(out, "%02x:%02x:%02x:%02x:%02x:%02x\t%ld\t%d\t%04x\t", m[0], m[1], m[2], m[3], m[4], m[5], e.lRssi, b.frequency, e.usCapabilityInformation);
      for (ULONG j = 0; j < e.ulIeSize; ++j) fprintf(out, "%02x", ies[j]);
      std::string ssid;
      for (ULONG j = 0; j < e.dot11Ssid.uSSIDLength && j < 32; ++j) { char h[3]; sprintf_s(h, "%02x", e.dot11Ssid.ucSSID[j]); ssid += h; }
      fprintf(out, "\t|\t%s\t%d\t%d\t%d\t%d\t%d\t%s\t%s\t%d\t%.1f\t%.1f\t%d\t%d\t%d\t%02x:%02x:%02x:%02x:%02x:%02x\t%s\t%d\n",
              ssid.c_str(), b.width, b.center, b.split ? 1 : 0, b.split ? b.center2 : 0, b.generation, utf8(b.phy).c_str(),
              utf8(b.security).c_str(), b.streams, b.maxRate, b.minRate, b.stations, b.utilization, b.multiLink ? 1 : 0,
              b.mld[0], b.mld[1], b.mld[2], b.mld[3], b.mld[4], b.mld[5], b.country.c_str(), b.details ? 1 : 0);
      ++count;
    }
    WlanFreeMemory(bsses);
  }
  WlanFreeMemory(list);
  wlan::stop();
  WlanCloseHandle(client, nullptr);
  fprintf(stderr, "%d networks\n", count);
  if (out != stdout) fclose(out);
  return 0;
}
