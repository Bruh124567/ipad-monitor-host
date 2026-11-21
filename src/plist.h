// Tiny helpers for reading values out of the XML plists that usbmuxd sends.
#pragma once

#include <cstdlib>
#include <string>

namespace plist {

// Finds <key>KEY</key> followed directly by <TAG>value</TAG>.
inline bool valueAfterKey(const std::string& xml, const std::string& key, const std::string& tag, std::string& out) {
  const std::string k = "<key>" + key + "</key>";
  size_t p = xml.find(k);
  if (p == std::string::npos) return false;
  p += k.size();
  while (p < xml.size() && (xml[p] == ' ' || xml[p] == '\t' || xml[p] == '\n' || xml[p] == '\r')) ++p;
  const std::string open = "<" + tag + ">";
  if (xml.compare(p, open.size(), open) != 0) return false;
  p += open.size();
  const size_t e = xml.find("</" + tag + ">", p);
  if (e == std::string::npos) return false;
  out = xml.substr(p, e - p);
  return true;
}

inline std::string getString(const std::string& xml, const std::string& key, const std::string& def = "") {
  std::string v;
  return valueAfterKey(xml, key, "string", v) ? v : def;
}

inline long getInt(const std::string& xml, const std::string& key, long def) {
  std::string v;
  return valueAfterKey(xml, key, "integer", v) ? std::strtol(v.c_str(), nullptr, 10) : def;
}

}  // namespace plist
