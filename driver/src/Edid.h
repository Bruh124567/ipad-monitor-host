// Builds a 128-byte EDID 1.4 base block for the virtual monitor.
// The EDID mainly gives Windows a stable identity for the monitor; the usable modes are
// reported separately by the driver (see ParseMonitorDescription).
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

inline std::array<uint8_t, 128> BuildEdid(uint32_t width, uint32_t height, uint32_t hz, uint32_t serial) {
  std::array<uint8_t, 128> e{};
  static const uint8_t header[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
  std::memcpy(e.data(), header, 8);

  // Manufacturer ID "IPM" (three 5-bit letters, A = 1), big-endian.
  const uint16_t mfg = static_cast<uint16_t>(((('I' - 64) & 0x1F) << 10) | ((('P' - 64) & 0x1F) << 5) | (('M' - 64) & 0x1F));
  e[8] = static_cast<uint8_t>(mfg >> 8);
  e[9] = static_cast<uint8_t>(mfg & 0xFF);
  e[10] = 0x01;  // product code
  e[11] = 0x00;
  for (int i = 0; i < 4; ++i) e[12 + i] = static_cast<uint8_t>((serial >> (8 * i)) & 0xFF);
  e[16] = 1;   // week of manufacture
  e[17] = 36;  // year of manufacture - 1990 (2026)
  e[18] = 1;   // EDID version 1.4
  e[19] = 4;
  e[20] = 0xA2;  // digital input, 8 bits per colour, HDMI-a
  e[21] = 0;     // physical size unknown
  e[22] = 0;
  e[23] = 120;   // gamma 2.2
  e[24] = 0x06;  // sRGB, preferred timing is the first descriptor
  static const uint8_t srgb[10] = {0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54};
  std::memcpy(&e[25], srgb, 10);
  for (int i = 0; i < 8; ++i) {  // standard timings: unused
    e[38 + 2 * i] = 0x01;
    e[39 + 2 * i] = 0x01;
  }

  // Descriptor 1 (offset 54): detailed timing for the preferred mode (reduced blanking style).
  // EDID can't describe more than 4095 pixels per line; the real mode list comes from the driver.
  const uint32_t w = std::min<uint32_t>(width, 4095), h = std::min<uint32_t>(height, 4095);
  const uint32_t hBlank = 160, hFront = 48, hSync = 32, vFront = 3, vSync = 5, vBlank = 30;
  uint64_t clk = static_cast<uint64_t>(w + hBlank) * (h + vBlank) * hz / 10000;  // units of 10 kHz
  if (clk > 65535) clk = 65535;
  uint8_t* d = &e[54];
  d[0] = static_cast<uint8_t>(clk & 0xFF);
  d[1] = static_cast<uint8_t>(clk >> 8);
  d[2] = static_cast<uint8_t>(w & 0xFF);
  d[3] = static_cast<uint8_t>(hBlank & 0xFF);
  d[4] = static_cast<uint8_t>(((w >> 8) << 4) | ((hBlank >> 8) & 0x0F));
  d[5] = static_cast<uint8_t>(h & 0xFF);
  d[6] = static_cast<uint8_t>(vBlank & 0xFF);
  d[7] = static_cast<uint8_t>(((h >> 8) << 4) | ((vBlank >> 8) & 0x0F));
  d[8] = static_cast<uint8_t>(hFront & 0xFF);
  d[9] = static_cast<uint8_t>(hSync & 0xFF);
  d[10] = static_cast<uint8_t>(((vFront & 0x0F) << 4) | (vSync & 0x0F));
  d[11] = 0;
  d[17] = 0x1A;  // digital separate sync, +hsync, -vsync

  // Descriptor 2 (offset 72): monitor range limits.
  uint8_t* r = &e[72];
  r[3] = 0xFD;
  r[5] = 24;    // min vertical Hz
  r[6] = 240;   // max vertical Hz
  r[7] = 15;    // min horizontal kHz
  r[8] = 255;   // max horizontal kHz
  r[9] = 66;    // max pixel clock, units of 10 MHz
  r[10] = 0x00;
  r[11] = 0x0A;
  for (int i = 12; i < 18; ++i) r[i] = 0x20;

  // Descriptor 3 (offset 90): monitor name.
  uint8_t* n = &e[90];
  n[3] = 0xFC;
  static const char name[] = "iPad Monitor\n";  // 13 characters including the line feed
  std::memcpy(&n[5], name, 13);

  // Descriptor 4 (offset 108): dummy.
  e[108 + 3] = 0x10;

  e[126] = 0;  // no extension blocks
  unsigned sum = 0;
  for (int i = 0; i < 127; ++i) sum += e[i];
  e[127] = static_cast<uint8_t>((256 - (sum & 0xFF)) & 0xFF);
  return e;
}
