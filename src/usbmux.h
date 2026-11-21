// Talks to Apple's USB multiplexer (AppleMobileDeviceService, installed with iTunes or the
// Apple Devices app) so the PC can reach a TCP port on a USB-connected iPad.
#pragma once

#include <winsock2.h>

#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

bool winsockStart();

// Connects to TCP `port` on the iOS device. Returns a socket that is a raw tunnel to the device.
// On failure returns INVALID_SOCKET and sets *result: -1 = service unreachable, otherwise
// usbmuxd's code (2 = unknown device, 3 = connection refused, i.e. the iPad app isn't listening).
SOCKET usbmuxConnect(int deviceId, uint16_t port, int* result);

// Tracks which iPads/iPhones are attached over USB (Wi-Fi sync devices are ignored).
class DeviceWatcher {
 public:
  void start();
  std::vector<int> devices();

 private:
  void run();
  std::mutex mu_;
  std::vector<int> ids_;
  std::thread th_;
};
