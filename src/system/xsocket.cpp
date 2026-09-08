/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cstring>

#include <rex/kernel/xam/module.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xsocket.h>
// #include <rex/system/xnet.h>

#include <rex/net/socket.h>

// Standard socket types used by Xbox API emulation
#if REX_PLATFORM_WIN32
#include <WinSock2.h>

#include <WS2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#endif

REXCVAR_DEFINE_BOOL(guest_network_enabled, true, "Networking",
                    "Allow guest code to create host network sockets")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(guest_network_trace, false, "Networking",
                    "Log redacted guest socket operations")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::system {

static_assert(sizeof(N_XSOCKADDR_IN) == sizeof(sockaddr_in));
static_assert(offsetof(N_XSOCKADDR_IN, sin_family) == offsetof(sockaddr_in, sin_family));
static_assert(offsetof(N_XSOCKADDR_IN, sin_port) == offsetof(sockaddr_in, sin_port));
static_assert(offsetof(N_XSOCKADDR_IN, sin_addr) == offsetof(sockaddr_in, sin_addr));

XSocket::XSocket(KernelState* kernel_state) : XObject(kernel_state, kObjectType) {}

XSocket::XSocket(KernelState* kernel_state, uint64_t native_handle)
    : XObject(kernel_state, kObjectType), native_handle_(native_handle) {}

XSocket::~XSocket() {
  Close();
}

X_STATUS XSocket::Initialize(AddressFamily af, Type type, Protocol proto) {
  if (!REXCVAR_GET(guest_network_enabled)) {
    REXSYS_WARN("Guest socket creation blocked because guest networking is disabled");
    return X_STATUS_UNSUCCESSFUL;
  }

  af_ = af;
  type_ = type;
  proto_ = proto;

  if (proto == Protocol::X_IPPROTO_VDP) {
    // VDP is a layer on top of UDP.
    proto = Protocol::X_IPPROTO_UDP;
  }

  native_handle_ = socket(af, type, proto);
  if (native_handle_ == -1) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Close() {
  int ret = rex::net::socket_close(native_handle_);
  if (ret != 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::SetOption(uint32_t level, uint32_t optname, void* optval_ptr, uint32_t optlen) {
  if (level == 0xFFFF && (optname == 0x5801 || optname == 0x5802)) {
    // Disable socket encryption
    secure_ = false;
    return X_STATUS_SUCCESS;
  }

  int ret = setsockopt(native_handle_, level, optname, (char*)optval_ptr, optlen);
  if (ret < 0) {
    // TODO: WSAGetLastError()
    return X_STATUS_UNSUCCESSFUL;
  }

  // SO_BROADCAST
  if (level == 0xFFFF && optname == 0x0020) {
    broadcast_socket_ = true;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::IOControl(uint32_t cmd, uint8_t* arg_ptr) {
  int ret = rex::net::socket_ioctl(native_handle_, cmd, arg_ptr);
  if (ret < 0) {
    // TODO: Get last error
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Connect(N_XSOCKADDR* name, int name_len) {
  if (name->address_family == X_AF_INET && name_len >= static_cast<int>(sizeof(N_XSOCKADDR_IN))) {
    // A nonblocking connect normally reports WSAEWOULDBLOCK while the peer is
    // already fixed. Preserve it for send/receive hooks during completion.
    peer_port_ = reinterpret_cast<N_XSOCKADDR_IN*>(name)->sin_port;
  }

  int ret = connect(native_handle_, (sockaddr*)name, name_len);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Bind(N_XSOCKADDR_IN* name, int name_len) {
  int ret = bind(native_handle_, (sockaddr*)name, name_len);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  bound_ = true;
  bound_port_ = name->sin_port;

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Listen(int backlog) {
  int ret = listen(native_handle_, backlog);
  if (ret < 0) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

object_ref<XSocket> XSocket::Accept(N_XSOCKADDR* name, int* name_len) {
  sockaddr n_sockaddr{};
  const size_t name_capacity =
      name ? (name_len
                  ? static_cast<size_t>(std::clamp(*name_len, 0, static_cast<int>(sizeof(*name))))
                  : sizeof(*name))
           : 0;
  socklen_t n_name_len =
      static_cast<socklen_t>(std::min(name_capacity, static_cast<size_t>(sizeof(n_sockaddr))));
  uintptr_t ret =
      accept(native_handle_, name ? &n_sockaddr : nullptr, name_len ? &n_name_len : nullptr);
  if (ret == -1) {
    if (name) {
      std::memset(name, 0, name_capacity);
    }
    if (name_len) {
      *name_len = 0;
    }
    return nullptr;
  }

  if (name) {
    std::memcpy(name, &n_sockaddr,
                std::min({sizeof(*name), name_capacity, static_cast<size_t>(n_name_len)}));
  }
  if (name_len) {
    *name_len = n_name_len;
  }

  // Create a kernel object to represent the new socket, and copy parameters
  // over.
  auto socket = object_ref<XSocket>(new XSocket(kernel_state_, ret));
  socket->af_ = af_;
  socket->type_ = type_;
  socket->proto_ = proto_;

  return socket;
}

int XSocket::Shutdown(int how) {
  return shutdown(native_handle_, how);
}

int XSocket::Recv(uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  return recv(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags);
}

int XSocket::RecvFrom(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* from,
                      uint32_t* from_len) {
  // Pop from secure packets first
  // TODO(DrChat): Enable when I commit XNet
  /*
  {
    std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
    if (incoming_packets_.size()) {
      packet* pkt = (packet*)incoming_packets_.front();
      int data_len = pkt->data_len;
      std::memcpy(buf, pkt->data, std::min((uint32_t)pkt->data_len, buf_len));

      from->sin_family = 2;
      from->sin_addr = pkt->src_ip;
      from->sin_port = pkt->src_port;

      incoming_packets_.pop();
      uint8_t* pkt_ui8 = (uint8_t*)pkt;
      delete[] pkt_ui8;

      return data_len;
    }
  }
  */

  sockaddr_in nfrom{};
  const size_t from_capacity =
      from ? (from_len ? static_cast<size_t>(*from_len) : sizeof(*from)) : 0;
  socklen_t nfromlen =
      static_cast<socklen_t>(std::min(from_capacity, static_cast<size_t>(sizeof(nfrom))));
  int ret = recvfrom(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags,
                     from ? reinterpret_cast<sockaddr*>(&nfrom) : nullptr,
                     from_len ? &nfromlen : nullptr);
  if (ret >= 0 && from) {
    // The native and normalized Xbox structures have the same wire layout:
    // native-endian family followed by network-endian port and address.
    std::memcpy(from, &nfrom,
                std::min({sizeof(*from), from_capacity, static_cast<size_t>(nfromlen)}));
  }

  if (ret >= 0 && from_len) {
    *from_len = nfromlen;
  }

  return ret;
}

int XSocket::Send(const uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  return send(native_handle_, reinterpret_cast<const char*>(buf), buf_len, flags);
}

int XSocket::SendTo(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* to,
                    uint32_t to_len) {
  // Send 2 copies of the packet: One to XNet (for network security) and an
  // unencrypted copy for other Xenia hosts.
  // TODO(DrChat): Enable when I commit XNet.
  /*
  auto xam = kernel_state()->GetKernelModule<xam::XamModule>("xam.xex");
  auto xnet = xam->xnet();
  if (xnet) {
    xnet->SendPacket(this, to, buf, buf_len);
  }
  */

  sockaddr_in native_to{};
  if (to) {
    std::memcpy(&native_to, to, sizeof(native_to));
  }
  return sendto(native_handle_, reinterpret_cast<char*>(buf), buf_len, flags,
                to ? reinterpret_cast<sockaddr*>(&native_to) : nullptr, to_len);
}

bool XSocket::QueuePacket(uint32_t src_ip, uint16_t src_port, const uint8_t* buf, size_t len) {
  packet* pkt = reinterpret_cast<packet*>(new uint8_t[sizeof(packet) + len]);
  pkt->src_ip = src_ip;
  pkt->src_port = src_port;

  pkt->data_len = (uint16_t)len;
  std::memcpy(pkt->data, buf, len);

  std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
  incoming_packets_.push((uint8_t*)pkt);

  // TODO: Limit on number of incoming packets?
  return true;
}

}  // namespace rex::system
