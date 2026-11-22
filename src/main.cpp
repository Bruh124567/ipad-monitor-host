// Step 1 host: finds the iPad over USB, does the handshake, and streams a test pattern.
// Keys while running:  t = toggle touch   a = toggle audio   o = toggle overlay   q = quit

#include <winsock2.h>
#include <windows.h>
#include <conio.h>
#include <mmsystem.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net.h"
#include "protocol.h"
#include "usbmux.h"

namespace {

struct Config {
  int width = 480;
  int height = 270;
  int fps = 30;
};

// Features switched on at the PC. The effective set is (iPad settings) AND (these).
std::atomic<uint16_t> g_pcFlags{proto::kAllFeatures};

class Session;
std::mutex g_activeMu;
Session* g_active = nullptr;

uint64_t nowUs() {
  using namespace std::chrono;
  return static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

// Colour bars, a sweeping inverted line, and a 32-bit frame counter along the bottom edge.
void renderPattern(uint8_t* px, int w, int h, uint64_t n) {
  static const uint8_t bars[8][3] = {{255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
                                     {255, 0, 255},   {255, 0, 0},   {0, 0, 255},   {0, 0, 0}};
  const int barsH = h * 3 / 4;
  const int counterTop = h - 14;
  const int cell = w / 32 > 0 ? w / 32 : 1;
  const int lineX = static_cast<int>((n * 6) % static_cast<uint64_t>(w));
  for (int y = 0; y < h; ++y) {
    uint8_t* row = px + static_cast<size_t>(y) * w * 4;
    for (int x = 0; x < w; ++x) {
      uint8_t r, g, b;
      if (y < barsH) {
        const uint8_t* c = bars[x * 8 / w];
        r = c[0]; g = c[1]; b = c[2];
      } else if (y >= counterTop) {
        const int bit = x / cell;
        const bool on = bit < 32 && ((n >> (31 - bit)) & 1u);
        r = g = b = on ? 255 : 0;
      } else {
        r = g = b = 40;
      }
      if (x >= lineX && x < lineX + 6) { r = 255 - r; g = 255 - g; b = 255 - b; }
      row[x * 4 + 0] = b;
      row[x * 4 + 1] = g;
      row[x * 4 + 2] = r;
      row[x * 4 + 3] = 255;
    }
  }
}

class Session {
 public:
  Session(SOCKET s, const Config& c) : sock_(s), cfg_(c) {}
  ~Session() { closesocket(sock_); }

  void run() {
    // Handshake: the iPad speaks first.
    DWORD timeoutMs = 5000;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
    proto::Header h;
    std::vector<uint8_t> body;
    proto::SessionParams hello;
    if (!recvFrame(h, body) || h.channel != proto::kControl || h.type != proto::kHello ||
        !proto::decodeSessionParams(body.data(), body.size(), hello)) {
      std::printf("[link] handshake failed (no valid Hello from the iPad)\n");
      return;
    }
    if (hello.version != proto::kVersion) {
      std::printf("[link] protocol version mismatch (iPad %u, PC %u)\n", hello.version, static_cast<unsigned>(proto::kVersion));
      return;
    }
    timeoutMs = 0;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
    ipadPrefs_ = hello.flags;
    std::printf("[link] iPad screen %ux%u, up to %u Hz. iPad settings: touch=%d audio=%d overlay=%d\n", hello.width,
                hello.height, hello.fps, !!(hello.flags & proto::kFeatureTouch), !!(hello.flags & proto::kFeatureAudio),
                !!(hello.flags & proto::kFeatureOverlay));

    proto::SessionParams ack;
    ack.width = static_cast<uint32_t>(cfg_.width);
    ack.height = static_cast<uint32_t>(cfg_.height);
    ack.fps = static_cast<uint16_t>(cfg_.fps);
    ack.flags = effectiveFlags();
    if (!sendFrame(proto::kControl, proto::kHelloAck, 0, proto::encodeSessionParams(ack))) return;
    std::printf("[link] streaming %dx%d @ %d fps (test pattern)\n", cfg_.width, cfg_.height, cfg_.fps);

    {
      std::lock_guard<std::mutex> lk(g_activeMu);
      g_active = this;
    }
    running_ = true;
    std::thread sender(&Session::senderLoop, this);

    while (running_ && recvFrame(h, body)) {
      if (h.channel == proto::kControl && h.type == proto::kPong) {
        proto::Reader r(body.data(), body.size());
        const uint64_t sent = r.u64();
        if (r.ok()) rttMs_ = static_cast<double>(nowUs() - sent) / 1000.0;
      } else if (h.channel == proto::kControl && h.type == proto::kBye) {
        break;
      } else if (h.channel == proto::kTelemetry && h.type == proto::kDeviceStatus) {
        proto::DeviceStatus st;
        if (proto::decodeDeviceStatus(body.data(), body.size(), st)) {
          char bat[16];
          if (st.battery == 255) std::snprintf(bat, sizeof(bat), "?");
          else std::snprintf(bat, sizeof(bat), "%d%%", st.battery);
          std::printf("\r[ipad] battery %s%s   drawing %u fps   round trip %.1f ms      ", bat,
                      st.charging ? " (charging)" : "", st.fps, rttMs_.load());
          std::fflush(stdout);
        }
      }
    }

    running_ = false;
    shutdown(sock_, SD_BOTH);
    sender.join();
    std::lock_guard<std::mutex> lk(g_activeMu);
    g_active = nullptr;
  }

  // Tells the iPad which features are now effectively on (called when a PC toggle changes).
  void pushSettings() {
    proto::Writer w;
    w.u16(effectiveFlags());
    w.u16(0);
    sendFrame(proto::kControl, proto::kSettingsUpdate, 0, w.buf);
  }

 private:
  uint16_t effectiveFlags() const { return static_cast<uint16_t>(ipadPrefs_ & g_pcFlags.load()); }

  bool recvFrame(proto::Header& h, std::vector<uint8_t>& body) {
    uint8_t raw[proto::kHeaderSize];
    if (!net::recvAll(sock_, raw, sizeof(raw))) return false;
    if (!proto::decodeHeader(raw, h)) return false;
    body.assign(h.length, 0);
    return h.length == 0 || net::recvAll(sock_, body.data(), body.size());
  }

  bool sendFrame(uint8_t channel, uint16_t type, uint64_t pts, const std::vector<uint8_t>& payload) {
    proto::Writer w;
    proto::encodeHeader(w, channel, type, static_cast<uint32_t>(payload.size()), pts);
    std::lock_guard<std::mutex> lk(sendMu_);
    return net::sendAll(sock_, w.buf.data(), w.buf.size()) &&
           (payload.empty() || net::sendAll(sock_, payload.data(), payload.size()));
  }

  void senderLoop() {
    const int w = cfg_.width, h = cfg_.height;
    std::vector<uint8_t> payload(12 + static_cast<size_t>(w) * h * 4);
    {
      proto::Writer hdr;  // raw frame header: width, height, stride
      hdr.u32(static_cast<uint32_t>(w));
      hdr.u32(static_cast<uint32_t>(h));
      hdr.u32(static_cast<uint32_t>(w * 4));
      std::memcpy(payload.data(), hdr.buf.data(), 12);
    }
    const auto period = std::chrono::microseconds(1000000 / cfg_.fps);
    auto next = std::chrono::steady_clock::now();
    const uint64_t t0 = nowUs();
    uint64_t frameNo = 0, lastPing = 0;
    while (running_) {
      renderPattern(payload.data() + 12, w, h, frameNo++);
      const uint64_t now = nowUs();
      if (!sendFrame(proto::kVideo, proto::kFrameRawBgra, now - t0, payload)) break;
      if (now - lastPing > 2000000) {
        lastPing = now;
        proto::Writer p;
        p.u64(now);
        if (!sendFrame(proto::kControl, proto::kPing, 0, p.buf)) break;
      }
      next += period;
      const auto t = std::chrono::steady_clock::now();
      if (next < t) next = t;  // fell behind: don't try to catch up with a burst
      std::this_thread::sleep_until(next);
    }
    running_ = false;
    shutdown(sock_, SD_BOTH);  // wakes the receiver
  }

  SOCKET sock_;
  Config cfg_;
  uint16_t ipadPrefs_ = 0;
  std::atomic<bool> running_{false};
  std::atomic<double> rttMs_{0.0};
  std::mutex sendMu_;
};

void keyLoop() {
  for (;;) {
    const int c = _getch();
    uint16_t flag = 0;
    const char* name = nullptr;
    switch (c) {
      case 't': case 'T': flag = proto::kFeatureTouch; name = "Touch"; break;
      case 'a': case 'A': flag = proto::kFeatureAudio; name = "Audio"; break;
      case 'o': case 'O': flag = proto::kFeatureOverlay; name = "Overlay"; break;
      case 'q': case 'Q': ExitProcess(0);
      default: continue;
    }
    const uint16_t now = static_cast<uint16_t>(g_pcFlags.fetch_xor(flag) ^ flag);
    std::printf("\n[pc] %s %s\n", name, (now & flag) ? "ON" : "OFF");
    std::lock_guard<std::mutex> lk(g_activeMu);
    if (g_active) g_active->pushSettings();
  }
}

bool parseArgs(int argc, char** argv, Config& c) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (i + 1 >= argc) return false;
    const int v = std::atoi(argv[++i]);
    if (a == "--width") c.width = v;
    else if (a == "--height") c.height = v;
    else if (a == "--fps") c.fps = v;
    else return false;
  }
  return c.width >= 64 && c.width <= 4096 && c.height >= 64 && c.height <= 4096 && c.fps >= 1 && c.fps <= 120;
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  if (!parseArgs(argc, argv, cfg)) {
    std::printf("Usage: ipad_host [--width 480] [--height 270] [--fps 30]\n");
    return 1;
  }
  // Held for the life of the process. A second launch sees it and exits.
  HANDLE instanceLock = CreateMutexW(nullptr, TRUE, L"Local\\ipad-monitor-host");
  if (!instanceLock) {
    std::printf("Could not create the single-instance lock (error %lu)\n", GetLastError());
    return 1;
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    std::printf("Another iPad Monitor host is already running.\n");
    std::printf("Press any key to exit...\n");
    _getch();
    CloseHandle(instanceLock);
    return 1;
  }

  timeBeginPeriod(1);  // finer sleep granularity for frame pacing
  if (!winsockStart()) {
    std::printf("Winsock failed to start\n");
    return 1;
  }
  std::printf("iPad Monitor host (step 1: USB link + test pattern)\n");
  std::printf("Keys: t = touch on/off, a = audio on/off, o = overlay on/off, q = quit\n\n");

  DeviceWatcher watcher;
  watcher.start();
  std::thread(keyLoop).detach();

  bool announcedWaiting = false;
  for (;;) {
    bool connected = false;
    const std::vector<int> ids = watcher.devices();
    for (int id : ids) {
      int result = 0;
      SOCKET s = usbmuxConnect(id, proto::kPort, &result);
      if (s == INVALID_SOCKET) continue;
      net::setNoDelay(s);
      announcedWaiting = false;
      connected = true;
      std::printf("[link] connected to the iPad app (device %d)\n", id);
      {
        Session session(s, cfg);
        session.run();
      }
      std::printf("\n[link] disconnected\n");
      break;
    }
    if (!connected) {
      if (!ids.empty() && !announcedWaiting) {
        std::printf("[link] iPad found, waiting for the iPad Monitor app to be open...\n");
        announcedWaiting = true;
      }
      if (ids.empty()) announcedWaiting = false;
      Sleep(1000);
    } else {
      Sleep(500);
    }
  }
}
