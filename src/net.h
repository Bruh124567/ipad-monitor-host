#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstddef>

namespace net {

inline bool sendAll(SOCKET s, const void* data, size_t n) {
  const char* p = static_cast<const char*>(data);
  while (n > 0) {
    const int chunk = n > (1u << 20) ? (1 << 20) : static_cast<int>(n);
    const int sent = send(s, p, chunk, 0);
    if (sent <= 0) return false;
    p += sent;
    n -= static_cast<size_t>(sent);
  }
  return true;
}

inline bool recvAll(SOCKET s, void* data, size_t n) {
  char* p = static_cast<char*>(data);
  while (n > 0) {
    const int chunk = n > (1u << 20) ? (1 << 20) : static_cast<int>(n);
    const int got = recv(s, p, chunk, 0);
    if (got <= 0) return false;
    p += got;
    n -= static_cast<size_t>(got);
  }
  return true;
}

inline void setNoDelay(SOCKET s) {
  BOOL on = TRUE;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
}

}  // namespace net
