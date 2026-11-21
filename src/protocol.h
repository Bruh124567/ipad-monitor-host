// Wire protocol shared by the Windows host and the iPad app. See PROTOCOL.md.
// All integers are little-endian.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace proto {

constexpr uint32_t kMagic = 0x4E4D5049u;  // bytes on the wire: 49 50 4D 4E ("IPMN")
constexpr uint8_t kVersion = 1;
constexpr uint16_t kPort = 7878;  // TCP port the iPad app listens on (reached through USB)
constexpr size_t kHeaderSize = 20;
constexpr uint32_t kMaxPayload = 64u * 1024u * 1024u;

enum Channel : uint8_t { kControl = 0, kVideo = 1, kAudio = 2, kTouch = 3, kTelemetry = 4 };
enum ControlType : uint16_t { kHello = 1, kHelloAck = 2, kSettingsUpdate = 3, kPing = 4, kPong = 5, kBye = 6 };
enum VideoType : uint16_t { kFrameRawBgra = 1 };
enum TelemetryType : uint16_t { kDeviceStatus = 1 };
enum Feature : uint16_t { kFeatureTouch = 1 << 0, kFeatureAudio = 1 << 1, kFeatureOverlay = 1 << 2 };
constexpr uint16_t kAllFeatures = kFeatureTouch | kFeatureAudio | kFeatureOverlay;

struct Header {
  uint8_t channel = 0;
  uint16_t type = 0;
  uint32_t length = 0;
  uint64_t ptsUs = 0;
};

class Writer {
 public:
  std::vector<uint8_t> buf;
  void u8(uint8_t v) { buf.push_back(v); }
  void u16(uint16_t v) { u8(static_cast<uint8_t>(v & 0xFF)); u8(static_cast<uint8_t>(v >> 8)); }
  void u32(uint32_t v) { u16(static_cast<uint16_t>(v & 0xFFFF)); u16(static_cast<uint16_t>(v >> 16)); }
  void u64(uint64_t v) { u32(static_cast<uint32_t>(v & 0xFFFFFFFFu)); u32(static_cast<uint32_t>(v >> 32)); }
};

class Reader {
 public:
  Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
  bool ok() const { return ok_; }
  uint8_t u8() {
    if (pos_ + 1 > n_) { ok_ = false; return 0; }
    return p_[pos_++];
  }
  uint16_t u16() {
    uint16_t a = u8();
    uint16_t b = u8();
    return static_cast<uint16_t>(a | (b << 8));
  }
  uint32_t u32() {
    uint32_t a = u16();
    uint32_t b = u16();
    return a | (b << 16);
  }
  uint64_t u64() {
    uint64_t a = u32();
    uint64_t b = u32();
    return a | (b << 32);
  }

 private:
  const uint8_t* p_;
  size_t n_;
  size_t pos_ = 0;
  bool ok_ = true;
};

inline void encodeHeader(Writer& w, uint8_t channel, uint16_t type, uint32_t length, uint64_t ptsUs) {
  w.u32(kMagic);
  w.u8(kVersion);
  w.u8(channel);
  w.u16(type);
  w.u32(length);
  w.u64(ptsUs);
}

inline bool decodeHeader(const uint8_t* p, Header& h) {
  Reader r(p, kHeaderSize);
  if (r.u32() != kMagic) return false;
  if (r.u8() != kVersion) return false;
  h.channel = r.u8();
  h.type = r.u16();
  h.length = r.u32();
  h.ptsUs = r.u64();
  return r.ok() && h.length <= kMaxPayload;
}

// Body of Hello (iPad -> PC) and HelloAck (PC -> iPad). Same layout, different meaning:
//   Hello:    width/height = iPad screen in pixels, fps = max refresh rate, flags = iPad's preferences
//   HelloAck: width/height = stream size,           fps = stream rate,      flags = effective features
struct SessionParams {
  uint32_t version = kVersion;
  uint32_t width = 0;
  uint32_t height = 0;
  uint16_t fps = 0;
  uint16_t flags = 0;
};

inline std::vector<uint8_t> encodeSessionParams(const SessionParams& m) {
  Writer w;
  w.u32(m.version);
  w.u32(m.width);
  w.u32(m.height);
  w.u16(m.fps);
  w.u16(m.flags);
  return w.buf;
}

inline bool decodeSessionParams(const uint8_t* p, size_t n, SessionParams& m) {
  Reader r(p, n);
  m.version = r.u32();
  m.width = r.u32();
  m.height = r.u32();
  m.fps = r.u16();
  m.flags = r.u16();
  return r.ok();
}

struct DeviceStatus {
  uint8_t battery = 255;  // 0-100, 255 = unknown
  uint8_t charging = 0;
  uint16_t fps = 0;       // frames per second actually drawn on the iPad
};

inline bool decodeDeviceStatus(const uint8_t* p, size_t n, DeviceStatus& s) {
  Reader r(p, n);
  s.battery = r.u8();
  s.charging = r.u8();
  s.fps = r.u16();
  return r.ok();
}

}  // namespace proto
