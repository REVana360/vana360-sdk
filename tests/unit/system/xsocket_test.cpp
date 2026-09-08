#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>

#include <rex/cvar.h>
#include <rex/platform.h>
#include <rex/runtime.h>
#include <rex/system/xsocket.h>

#if REX_PLATFORM_WIN32
#include <WinSock2.h>

#include <WS2tcpip.h>
#elif REX_PLATFORM_LINUX
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using rex::X_STATUS;

TEST_CASE("Normalized IPv4 socket addresses preserve wire byte order", "[system][xsocket]") {
  rex::system::XSOCKADDR_IN guest{};
  guest.sin_family = rex::system::XSocket::X_AF_INET;
  guest.sin_port = 54230;
  guest.sin_addr = 0x7F000001;

  const rex::system::N_XSOCKADDR_IN native(&guest);
  const auto bytes = std::bit_cast<std::array<uint8_t, sizeof(native)>>(native);
  CHECK(bytes[0] == 2);
  CHECK(bytes[1] == 0);
  CHECK(bytes[2] == 0xD3);
  CHECK(bytes[3] == 0xD6);
  CHECK(bytes[4] == 0x7F);
  CHECK(bytes[5] == 0);
  CHECK(bytes[6] == 0);
  CHECK(bytes[7] == 1);
  CHECK(std::all_of(bytes.begin() + 8, bytes.end(), [](uint8_t byte) { return byte == 0; }));
}

#if REX_PLATFORM_LINUX
TEST_CASE("Linux translates Xbox socket ioctl commands", "[system][xsocket][linux]") {
  rex::system::XSocket receiver(nullptr);
  REQUIRE(receiver.Initialize(rex::system::XSocket::X_AF_INET, rex::system::XSocket::X_SOCK_DGRAM,
                              rex::system::XSocket::X_IPPROTO_UDP) == X_STATUS_SUCCESS);

  rex::system::N_XSOCKADDR_IN bind_address{};
  std::memset(&bind_address, 0, sizeof(bind_address));
  bind_address.sin_family = rex::system::XSocket::X_AF_INET;
  bind_address.sin_addr = 0x7F000001;
  REQUIRE(receiver.Bind(&bind_address, sizeof(bind_address)) == X_STATUS_SUCCESS);

  sockaddr_in receiver_address{};
  socklen_t receiver_address_length = sizeof(receiver_address);
  REQUIRE(getsockname(static_cast<int>(receiver.native_handle()),
                      reinterpret_cast<sockaddr*>(&receiver_address),
                      &receiver_address_length) == 0);

  uint32_t nonblocking = 1;
  REQUIRE(receiver.IOControl(0x8004667E, reinterpret_cast<uint8_t*>(&nonblocking)) ==
          X_STATUS_SUCCESS);

  std::array<uint8_t, 4> received{};
  CHECK(receiver.Recv(received.data(), received.size(), 0) == -1);

  const int sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  REQUIRE(sender >= 0);
  struct SenderCleanup {
    int socket;
    ~SenderCleanup() { close(socket); }
  } sender_cleanup{sender};

  constexpr std::array<uint8_t, 4> payload{0x10, 0x20, 0x30, 0x40};
  REQUIRE(sendto(sender, payload.data(), payload.size(), 0,
                 reinterpret_cast<const sockaddr*>(&receiver_address),
                 sizeof(receiver_address)) == static_cast<ssize_t>(payload.size()));

  uint32_t queued_bytes = 0;
  REQUIRE(receiver.IOControl(0x4004667F, reinterpret_cast<uint8_t*>(&queued_bytes)) ==
          X_STATUS_SUCCESS);
  CHECK(queued_bytes == payload.size());
  REQUIRE(receiver.Recv(received.data(), received.size(), 0) == payload.size());
  CHECK(received == payload);
}
#endif

#if REX_PLATFORM_WIN32
TEST_CASE("recvfrom rejects a short address buffer without consuming data", "[system][xsocket]") {
  WSADATA winsock_data{};
  REQUIRE(WSAStartup(MAKEWORD(2, 2), &winsock_data) == 0);
  struct WinsockCleanup {
    ~WinsockCleanup() { WSACleanup(); }
  } winsock_cleanup;

  rex::system::XSocket receiver(nullptr);
  REQUIRE(receiver.Initialize(rex::system::XSocket::X_AF_INET, rex::system::XSocket::X_SOCK_DGRAM,
                              rex::system::XSocket::X_IPPROTO_UDP) == X_STATUS_SUCCESS);

  rex::system::N_XSOCKADDR_IN bind_address{};
  std::memset(&bind_address, 0, sizeof(bind_address));
  bind_address.sin_family = rex::system::XSocket::X_AF_INET;
  bind_address.sin_addr = 0x7F000001;
  REQUIRE(receiver.Bind(&bind_address, sizeof(bind_address)) == X_STATUS_SUCCESS);

  sockaddr_in receiver_address{};
  int receiver_address_length = sizeof(receiver_address);
  REQUIRE(getsockname(static_cast<SOCKET>(receiver.native_handle()),
                      reinterpret_cast<sockaddr*>(&receiver_address),
                      &receiver_address_length) == 0);

  const SOCKET sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  REQUIRE(sender != INVALID_SOCKET);
  struct SocketCleanup {
    SOCKET socket;
    ~SocketCleanup() { closesocket(socket); }
  } sender_cleanup{sender};

  constexpr std::array<uint8_t, 4> payload{0x10, 0x20, 0x30, 0x40};
  REQUIRE(sendto(sender, reinterpret_cast<const char*>(payload.data()), payload.size(), 0,
                 reinterpret_cast<const sockaddr*>(&receiver_address),
                 sizeof(receiver_address)) == payload.size());

  std::array<uint8_t, payload.size()> received{};
  rex::system::N_XSOCKADDR_IN source_address;
  std::memset(&source_address, 0xA5, sizeof(source_address));
  uint32_t source_address_length = sizeof(source_address) - 1;

  WSASetLastError(0);
  CHECK(receiver.RecvFrom(received.data(), received.size(), 0, &source_address,
                          &source_address_length) == SOCKET_ERROR);
  CHECK(WSAGetLastError() == WSAEFAULT);
  CHECK(source_address_length == sizeof(source_address) - 1);
  const auto failed_address_bytes =
      std::bit_cast<std::array<uint8_t, sizeof(source_address)>>(source_address);
  CHECK(std::all_of(failed_address_bytes.begin(), failed_address_bytes.end(),
                    [](uint8_t byte) { return byte == 0xA5; }));

  source_address_length = sizeof(source_address);
  REQUIRE(receiver.RecvFrom(received.data(), received.size(), 0, &source_address,
                            &source_address_length) == payload.size());
  CHECK(received == payload);
  CHECK(source_address_length == sizeof(source_address));
  CHECK(source_address.sin_family == rex::system::XSocket::X_AF_INET);
  CHECK(static_cast<uint32_t>(source_address.sin_addr) == 0x7F000001);
}
#endif

TEST_CASE("Guest networking can block host socket creation", "[system][xsocket]") {
  const bool previous_value = REXCVAR_GET(guest_network_enabled);
  REXCVAR_SET(guest_network_enabled, false);

  rex::system::XSocket socket(nullptr);
  CHECK(socket.Initialize(rex::system::XSocket::X_AF_INET, rex::system::XSocket::X_SOCK_STREAM,
                          rex::system::XSocket::X_IPPROTO_TCP) == X_STATUS_UNSUCCESSFUL);
  CHECK(socket.native_handle() == uint64_t(-1));

  REXCVAR_SET(guest_network_enabled, previous_value);
}

TEST_CASE("Network send hooks receive an owned copy", "[system][xsocket]") {
  constexpr std::array<uint8_t, 4> source{1, 2, 3, 4};
  rex::NetworkHooks hooks;
  hooks.before_send = [](uint32_t caller, uint16_t peer_port, std::span<uint8_t> bytes) {
    CHECK(caller == 0x1234);
    CHECK(peer_port == 54001);
    bytes[1] = 0xAA;
  };

  const auto transformed = rex::ApplyNetworkSendHook(hooks, 0x1234, 54001, source);
  CHECK(source[1] == 2);
  CHECK(transformed[1] == 0xAA);
}

TEST_CASE("Network send consume hooks see transformed bytes", "[system][xsocket]") {
  constexpr std::array<uint8_t, 4> source{1, 2, 3, 4};
  rex::NetworkHooks hooks;
  hooks.before_send = [](uint32_t, uint16_t, std::span<uint8_t> bytes) { bytes[1] = 0xAA; };

  bool consumed = false;
  hooks.consume_send = [&consumed](uint32_t caller, uint16_t peer_port,
                                   std::span<const uint8_t> bytes) {
    consumed = true;
    CHECK(caller == 0x1234);
    CHECK(peer_port == 54001);
    CHECK(bytes.size() == 4);
    CHECK(bytes[1] == 0xAA);
    return true;
  };

  const auto transformed = rex::ApplyNetworkSendHook(hooks, 0x1234, 54001, source);
  CHECK(rex::ConsumeNetworkSendHook(hooks, 0x1234, 54001, transformed));
  CHECK(consumed);

  hooks.consume_send = [](uint32_t, uint16_t, std::span<const uint8_t>) { return false; };
  CHECK_FALSE(rex::ConsumeNetworkSendHook(hooks, 0x1234, 54001, transformed));
}
