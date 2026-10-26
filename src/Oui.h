// WiFiScope: the maker of a hardware (MAC) address, from the IEEE registry
// built into the program (res/oui.txt, made by tools/make_oui.py).
#pragma once
#include <cstdint>
#include <string>

namespace oui {

  // the registered organization for the address, or "" if none is known;
  // longest block first (MA-S 36 bits, MA-M 28, MA-L 24)
  std::wstring vendor(const uint8_t * mac);

  // a locally administered address (made up by a device: a router's extra
  // networks, a phone's private address) rather than one from a maker's block
  inline bool local(const uint8_t * mac) { return (mac[0] & 2) != 0; }
}
