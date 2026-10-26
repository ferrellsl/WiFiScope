// WiFiScope: the IEEE registry (res/oui.txt, an RCDATA resource "OUI"):
// "PREFIX<tab>Name" lines, the prefix 6, 7 or 9 hex digits.
#include "Oui.h"

#include <windows.h>
#include <unordered_map>

namespace oui {

  namespace {
    // key: the prefix's value; one map per prefix length (24, 28 and 36 bits)
    std::unordered_map<uint64_t, std::wstring> blocks[3];
    bool loaded = false;

    void load()
    {
      loaded = true;
      HRSRC res = FindResourceW(nullptr, L"OUI", RT_RCDATA);
      if (!res) return;
      HGLOBAL g = LoadResource(nullptr, res);
      const char * p = g ? (const char *) LockResource(g) : nullptr;
      if (!p) return;
      const char * end = p + SizeofResource(nullptr, res);
      for (int i = 0; i < 3; ++i) blocks[i].reserve(i == 0 ? 40000 : 8000);
      while (p < end) {
        const char * line = p;
        while (p < end && *p != '\n') ++p;
        const char * tab = line;
        while (tab < p && *tab != '\t') ++tab;
        const int digits = (int) (tab - line);
        if (tab < p && *line != '#' && (digits == 6 || digits == 7 || digits == 9)) {
          uint64_t v = 0;
          bool ok = true;
          for (const char * c = line; c < tab; ++c) {
            const int d = *c >= '0' && *c <= '9' ? *c - '0' : *c >= 'A' && *c <= 'F' ? *c - 'A' + 10 : -1;
            if (d < 0) { ok = false; break; }
            v = (v << 4) | (uint64_t) d;
          }
          if (ok) {
            const int n = MultiByteToWideChar(CP_UTF8, 0, tab + 1, (int) (p - tab - 1), nullptr, 0);
            std::wstring name(n, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, tab + 1, (int) (p - tab - 1), &name[0], n);
            blocks[digits == 6 ? 0 : digits == 7 ? 1 : 2].emplace(v, std::move(name));
          }
        }
        ++p;
      }
    }
  }

  std::wstring vendor(const uint8_t * mac)
  {
    if (!loaded) load();
    uint64_t a = 0;
    for (int i = 0; i < 6; ++i) a = (a << 8) | mac[i];
    static const int bits[3] = { 36, 28, 24 };
    static const int table[3] = { 2, 1, 0 };
    for (int i = 0; i < 3; ++i) {
      auto it = blocks[table[i]].find(a >> (48 - bits[i]));
      if (it != blocks[table[i]].end()) return it->second;
    }
    return L"";
  }
}
