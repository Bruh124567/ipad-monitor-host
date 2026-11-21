#include "usbmux.h"

#include <ws2tcpip.h>

#include <cstdio>
#include <string>

#include "net.h"
#include "plist.h"

namespace {

constexpr uint16_t kUsbmuxPort = 27015;  // AppleMobileDeviceService listens here on localhost

SOCKET openService() {
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return s;
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(kUsbmuxPort);
  inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
    closesocket(s);
    return INVALID_SOCKET;
  }
  net::setNoDelay(s);
  return s;
}

std::string buildRequest(const char* type, const std::string& extra) {
  return std::string(
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
             "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
             "<plist version=\"1.0\">\n<dict>\n"
             "<key>BundleID</key><string>com.ipadmonitor.host</string>\n"
             "<key>ClientVersionString</key><string>ipad-monitor 1.0</string>\n"
             "<key>ProgName</key><string>ipad-monitor</string>\n"
             "<key>kLibUSBMuxVersion</key><integer>3</integer>\n"
             "<key>MessageType</key><string>") +
         type + "</string>\n" + extra + "</dict>\n</plist>\n";
}

// usbmuxd framing: u32 length (incl. header), u32 version (1 = plist), u32 request (8 = plist), u32 tag.
bool sendPlist(SOCKET s, uint32_t tag, const std::string& xml) {
  const uint32_t hdr[4] = {static_cast<uint32_t>(16 + xml.size()), 1, 8, tag};
  return net::sendAll(s, hdr, sizeof(hdr)) && net::sendAll(s, xml.data(), xml.size());
}

bool recvPlist(SOCKET s, std::string& xml) {
  uint32_t hdr[4];
  if (!net::recvAll(s, hdr, sizeof(hdr))) return false;
  if (hdr[0] < 16 || hdr[0] > (4u << 20)) return false;
  xml.assign(hdr[0] - 16, '\0');
  return xml.empty() || net::recvAll(s, &xml[0], xml.size());
}

}  // namespace

bool winsockStart() {
  WSADATA d;
  return WSAStartup(MAKEWORD(2, 2), &d) == 0;
}

SOCKET usbmuxConnect(int deviceId, uint16_t port, int* result) {
  *result = -1;
  SOCKET s = openService();
  if (s == INVALID_SOCKET) return s;
  // PortNumber must be given in network byte order.
  const std::string extra = "<key>DeviceID</key><integer>" + std::to_string(deviceId) +
                            "</integer>\n<key>PortNumber</key><integer>" + std::to_string(htons(port)) +
                            "</integer>\n";
  std::string reply;
  if (!sendPlist(s, 2, buildRequest("Connect", extra)) || !recvPlist(s, reply)) {
    closesocket(s);
    return INVALID_SOCKET;
  }
  const long code = plist::getInt(reply, "Number", -1);
  if (code != 0) {
    *result = static_cast<int>(code);
    closesocket(s);
    return INVALID_SOCKET;
  }
  *result = 0;
  return s;  // from here on the socket is a plain TCP tunnel to the iPad app
}

void DeviceWatcher::start() { th_ = std::thread([this] { run(); }); th_.detach(); }

std::vector<int> DeviceWatcher::devices() {
  std::lock_guard<std::mutex> lk(mu_);
  return ids_;
}

void DeviceWatcher::run() {
  bool warned = false;
  for (;;) {
    SOCKET s = openService();
    if (s == INVALID_SOCKET) {
      if (!warned) {
        std::printf(
            "[usb] Can't reach Apple's USB service (127.0.0.1:27015). Install iTunes or the Apple Devices app "
            "and make sure \"Apple Mobile Device Service\" is running. Retrying...\n");
        warned = true;
      }
      Sleep(2000);
      continue;
    }
    warned = false;
    std::string xml;
    if (!sendPlist(s, 1, buildRequest("Listen", "")) || !recvPlist(s, xml) || plist::getInt(xml, "Number", -1) != 0) {
      closesocket(s);
      Sleep(2000);
      continue;
    }
    while (recvPlist(s, xml)) {
      const std::string type = plist::getString(xml, "MessageType");
      const int id = static_cast<int>(plist::getInt(xml, "DeviceID", -1));
      std::lock_guard<std::mutex> lk(mu_);
      if (type == "Attached") {
        if (plist::getString(xml, "ConnectionType", "USB") != "USB") continue;
        bool known = false;
        for (int e : ids_) known = known || e == id;
        if (!known) ids_.push_back(id);
        std::printf("[usb] device attached (id %d, serial %s)\n", id, plist::getString(xml, "SerialNumber").c_str());
      } else if (type == "Detached") {
        for (size_t i = 0; i < ids_.size(); ++i) {
          if (ids_[i] == id) {
            ids_.erase(ids_.begin() + static_cast<long>(i));
            std::printf("[usb] device detached (id %d)\n", id);
            break;
          }
        }
      }
    }
    { std::lock_guard<std::mutex> lk(mu_); ids_.clear(); }
    closesocket(s);
    Sleep(1000);
  }
}
