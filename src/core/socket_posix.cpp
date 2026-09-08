#include <rex/net/socket.h>
#include <rex/platform.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

#include <sys/ioctl.h>
#include <unistd.h>

namespace rex::net {

int socket_close(SocketHandle handle) {
  return close(static_cast<int>(handle));
}

int socket_ioctl(SocketHandle handle, uint32_t cmd, uint8_t* arg) {
#if REX_PLATFORM_LINUX
  // Xbox uses Winsock command numbers. Linux has its own ioctl ABI; keep this
  // translation Linux-specific rather than assuming macOS shares it.
  constexpr uint32_t kXboxFionread = 0x4004667F;
  constexpr uint32_t kXboxFionbio = 0x8004667E;
  if (cmd == kXboxFionread) {
    cmd = FIONREAD;
  } else if (cmd == kXboxFionbio) {
    cmd = FIONBIO;
  }
#endif
  return ioctl(static_cast<int>(handle), cmd, arg);
}

}  // namespace rex::net
