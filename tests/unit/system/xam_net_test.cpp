#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>

#include <rex/platform.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xsocket.h>
#include <rex/system/xthread.h>

#if REX_PLATFORM_WIN32
#include <WinSock2.h>

#include <WS2tcpip.h>
#endif

#if REX_PLATFORM_WIN32

using rex::X_STATUS;

namespace rex::kernel::xam {
using namespace rex::system;

u32 NetDll_accept_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR> addr_ptr,
                        mapped_u32 addrlen_ptr);
u32 NetDll_bind_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR_IN> name, u32 namelen);
u32 NetDll_closesocket_entry(u32 caller, u32 socket_handle);
u32 NetDll_ioctlsocket_entry(u32 caller, u32 socket_handle, u32 cmd, mapped_void arg_ptr);
u32 NetDll_listen_entry(u32 caller, u32 socket_handle, i32 backlog);
u32 NetDll_recvfrom_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len,
                          u32 flags, ppc_ptr_t<XSOCKADDR_IN> from_ptr, mapped_u32 fromlen_ptr);
u32 NetDll_socket_entry(u32 caller, u32 af, u32 type, u32 protocol);
}  // namespace rex::kernel::xam

namespace {

class ScratchDirectory {
 public:
  ScratchDirectory() {
    const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
    root =
        std::filesystem::temp_directory_path() / ("rexglue_xam_net_test_" + std::to_string(token));
    std::filesystem::create_directories(root / "game");
    std::filesystem::create_directories(root / "user");
  }

  ~ScratchDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }

  std::filesystem::path root;
};

class KernelCallThread final : public rex::system::XThread {
 public:
  KernelCallThread(rex::system::KernelState* kernel_state, std::function<void()> callback)
      : XThread(kernel_state, 16 * 1024, 0, 0, 0, 0, false), callback_(std::move(callback)) {}

  void Execute() override { callback_(); }

 private:
  std::function<void()> callback_;
};

bool RunKernelCall(rex::system::KernelState* kernel_state, std::function<void()> callback) {
  auto thread = rex::system::object_ref<KernelCallThread>(
      new KernelCallThread(kernel_state, std::move(callback)));
  if (thread->Create() != X_STATUS_SUCCESS) {
    return false;
  }
  return thread->Wait(0, 0, 0, nullptr) == X_STATUS_SUCCESS;
}

struct WinsockCleanup {
  ~WinsockCleanup() { WSACleanup(); }
};

struct SocketCleanup {
  SOCKET socket = INVALID_SOCKET;
  ~SocketCleanup() {
    if (socket != INVALID_SOCKET) {
      closesocket(socket);
    }
  }
};

bool ConnectClient(const sockaddr_in& address, SOCKET* socket_out) {
  const SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (client == INVALID_SOCKET) {
    return false;
  }
  if (connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    closesocket(client);
    return false;
  }
  *socket_out = client;
  return true;
}

}  // namespace

TEST_CASE("Guest accept bounds null, short, and oversized address buffers", "[system][xam_net]") {
  WSADATA winsock_data{};
  REQUIRE(WSAStartup(MAKEWORD(2, 2), &winsock_data) == 0);
  WinsockCleanup winsock_cleanup;

  ScratchDirectory scratch;
  rex::Runtime runtime(scratch.root / "game", scratch.root / "user");
  rex::RuntimeConfig config;
  config.tool_mode = true;
  REQUIRE(runtime.Setup(std::move(config)) == X_STATUS_SUCCESS);

  using namespace rex::kernel::xam;
  const u32 listener_handle =
      NetDll_socket_entry(0, rex::system::XSocket::X_AF_INET, rex::system::XSocket::X_SOCK_STREAM,
                          rex::system::XSocket::X_IPPROTO_TCP);
  REQUIRE(listener_handle != UINT32_MAX);

  rex::system::XSOCKADDR_IN bind_address{};
  bind_address.sin_family = rex::system::XSocket::X_AF_INET;
  bind_address.sin_addr = 0x7F000001;
  REQUIRE(NetDll_bind_entry(0, listener_handle, ppc_ptr_t<XSOCKADDR_IN>::from_host(&bind_address),
                            sizeof(bind_address)) == 0);
  REQUIRE(NetDll_listen_entry(0, listener_handle, 2) == 0);

  be_u32 nonblocking = 1;
  REQUIRE(NetDll_ioctlsocket_entry(0, listener_handle, 0x8004667E,
                                   mapped_void::from_host(&nonblocking)) == 0);

  sockaddr_in listener_address{};
  int listener_address_length = sizeof(listener_address);
  // The guest handle is not the native descriptor; retrieve it through the
  // kernel object that owns the listener.
  auto listener = runtime.kernel_state()->object_table()->LookupObject<rex::system::XSocket>(
      listener_handle + rex::system::XObject::kHandleBase);
  REQUIRE(listener);
  REQUIRE(getsockname(static_cast<SOCKET>(listener->native_handle()),
                      reinterpret_cast<sockaddr*>(&listener_address),
                      &listener_address_length) == 0);

  std::array<uint8_t, sizeof(XSOCKADDR) + 8> output{};
  std::fill(output.begin(), output.end(), 0xA5);
  be_u32 output_length = 1;
  u32 call_result = UINT32_MAX;
  REQUIRE(RunKernelCall(runtime.kernel_state(), [&] {
    call_result = NetDll_accept_entry(
        0, listener_handle,
        ppc_ptr_t<XSOCKADDR>::from_host(reinterpret_cast<XSOCKADDR*>(output.data())),
        mapped_u32::from_host(&output_length));
  }));
  CHECK(call_result == UINT32_MAX);
  CHECK(std::all_of(output.begin(), output.end(), [](uint8_t byte) { return byte == 0xA5; }));

  call_result = 0;
  REQUIRE(RunKernelCall(runtime.kernel_state(), [&] {
    call_result = NetDll_accept_entry(
        0, listener_handle,
        ppc_ptr_t<XSOCKADDR>::from_host(reinterpret_cast<XSOCKADDR*>(output.data())),
        mapped_u32(nullptr));
  }));
  CHECK(call_result == UINT32_MAX);
  CHECK(std::all_of(output.begin(), output.end(), [](uint8_t byte) { return byte == 0xA5; }));

  SOCKET client = INVALID_SOCKET;
  REQUIRE(ConnectClient(listener_address, &client));
  SocketCleanup client_cleanup{client};

  output_length = 1;
  call_result = UINT32_MAX;
  REQUIRE(RunKernelCall(runtime.kernel_state(), [&] {
    call_result = NetDll_accept_entry(
        0, listener_handle,
        ppc_ptr_t<XSOCKADDR>::from_host(reinterpret_cast<XSOCKADDR*>(output.data())),
        mapped_u32::from_host(&output_length));
  }));
  REQUIRE(call_result != UINT32_MAX);
  CHECK(std::all_of(output.begin() + sizeof(XSOCKADDR), output.end(),
                    [](uint8_t byte) { return byte == 0xA5; }));
  CHECK(output[0] == 0);
  CHECK(output_length == sizeof(XSOCKADDR));
  CHECK(NetDll_closesocket_entry(0, call_result) == 0);

  SOCKET client2 = INVALID_SOCKET;
  REQUIRE(ConnectClient(listener_address, &client2));
  SocketCleanup client2_cleanup{client2};

  std::fill(output.begin(), output.end(), 0xA5);
  output_length = sizeof(output);
  call_result = UINT32_MAX;
  REQUIRE(RunKernelCall(runtime.kernel_state(), [&] {
    call_result = NetDll_accept_entry(
        0, listener_handle,
        ppc_ptr_t<XSOCKADDR>::from_host(reinterpret_cast<XSOCKADDR*>(output.data())),
        mapped_u32::from_host(&output_length));
  }));
  REQUIRE(call_result != UINT32_MAX);
  CHECK(output[0] == 0);
  CHECK(output[1] == rex::system::XSocket::X_AF_INET);
  CHECK(output_length == sizeof(XSOCKADDR));
  CHECK(std::all_of(output.begin() + sizeof(XSOCKADDR), output.end(),
                    [](uint8_t byte) { return byte == 0xA5; }));
  CHECK(NetDll_closesocket_entry(0, call_result) == 0);

  std::fill(output.begin(), output.end(), 0xA5);
  output_length = sizeof(output);
  call_result = 0;
  REQUIRE(RunKernelCall(runtime.kernel_state(), [&] {
    call_result = NetDll_accept_entry(
        0, listener_handle,
        ppc_ptr_t<XSOCKADDR>::from_host(reinterpret_cast<XSOCKADDR*>(output.data())),
        mapped_u32::from_host(&output_length));
  }));
  CHECK(call_result == UINT32_MAX);
  CHECK(output_length == sizeof(output));
  CHECK(std::all_of(output.begin(), output.end(), [](uint8_t byte) { return byte == 0xA5; }));

  REQUIRE(RunKernelCall(runtime.kernel_state(), [&] {
    call_result =
        NetDll_accept_entry(0, listener_handle, ppc_ptr_t<XSOCKADDR>(nullptr), mapped_u32(nullptr));
  }));
  CHECK(call_result == UINT32_MAX);

  REQUIRE(NetDll_closesocket_entry(0, listener_handle) == 0);
}

TEST_CASE("Guest recvfrom bounds null, short, and oversized address buffers", "[system][xam_net]") {
  WSADATA winsock_data{};
  REQUIRE(WSAStartup(MAKEWORD(2, 2), &winsock_data) == 0);
  WinsockCleanup winsock_cleanup;

  ScratchDirectory scratch;
  rex::Runtime runtime(scratch.root / "game", scratch.root / "user");
  rex::RuntimeConfig config;
  config.tool_mode = true;
  REQUIRE(runtime.Setup(std::move(config)) == X_STATUS_SUCCESS);

  using namespace rex::kernel::xam;
  const u32 receiver_handle =
      NetDll_socket_entry(0, rex::system::XSocket::X_AF_INET, rex::system::XSocket::X_SOCK_DGRAM,
                          rex::system::XSocket::X_IPPROTO_UDP);
  REQUIRE(receiver_handle != UINT32_MAX);

  rex::system::XSOCKADDR_IN bind_address{};
  bind_address.sin_family = rex::system::XSocket::X_AF_INET;
  bind_address.sin_addr = 0x7F000001;
  REQUIRE(NetDll_bind_entry(0, receiver_handle, ppc_ptr_t<XSOCKADDR_IN>::from_host(&bind_address),
                            sizeof(bind_address)) == 0);

  auto receiver = runtime.kernel_state()->object_table()->LookupObject<rex::system::XSocket>(
      receiver_handle + rex::system::XObject::kHandleBase);
  REQUIRE(receiver);
  sockaddr_in receiver_address{};
  int receiver_address_length = sizeof(receiver_address);
  REQUIRE(getsockname(static_cast<SOCKET>(receiver->native_handle()),
                      reinterpret_cast<sockaddr*>(&receiver_address),
                      &receiver_address_length) == 0);

  SOCKET sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  REQUIRE(sender != INVALID_SOCKET);
  SocketCleanup sender_cleanup{sender};

  be_u32 nonblocking = 1;
  REQUIRE(NetDll_ioctlsocket_entry(0, receiver_handle, 0x8004667E,
                                   mapped_void::from_host(&nonblocking)) == 0);

  constexpr std::array<uint8_t, 4> payload{0x10, 0x20, 0x30, 0x40};
  REQUIRE(sendto(sender, reinterpret_cast<const char*>(payload.data()), payload.size(), 0,
                 reinterpret_cast<const sockaddr*>(&receiver_address),
                 sizeof(receiver_address)) == payload.size());

  std::array<uint8_t, sizeof(XSOCKADDR_IN) + 8> output{};
  std::fill(output.begin(), output.end(), 0xA5);
  std::array<uint8_t, payload.size()> received{};
  be_u32 output_length = 1;
  u32 call_result = UINT32_MAX;
  REQUIRE(RunKernelCall(runtime.kernel_state(), [&] {
    call_result = NetDll_recvfrom_entry(
        0, receiver_handle, mapped_void::from_host(received.data()), received.size(), 0,
        ppc_ptr_t<XSOCKADDR_IN>::from_host(reinterpret_cast<XSOCKADDR_IN*>(output.data())),
        mapped_u32(nullptr));
  }));
  CHECK(call_result == UINT32_MAX);
  CHECK(std::all_of(output.begin(), output.end(), [](uint8_t byte) { return byte == 0xA5; }));

  call_result = UINT32_MAX;
  REQUIRE(RunKernelCall(runtime.kernel_state(), [&] {
    call_result = NetDll_recvfrom_entry(
        0, receiver_handle, mapped_void::from_host(received.data()), received.size(), 0,
        ppc_ptr_t<XSOCKADDR_IN>::from_host(reinterpret_cast<XSOCKADDR_IN*>(output.data())),
        mapped_u32::from_host(&output_length));
  }));
  REQUIRE(call_result == payload.size());
  CHECK(received == payload);
  CHECK(output[0] == 0);
  CHECK(std::all_of(output.begin() + sizeof(XSOCKADDR_IN), output.end(),
                    [](uint8_t byte) { return byte == 0xA5; }));

  REQUIRE(sendto(sender, reinterpret_cast<const char*>(payload.data()), payload.size(), 0,
                 reinterpret_cast<const sockaddr*>(&receiver_address),
                 sizeof(receiver_address)) == payload.size());
  std::fill(output.begin(), output.end(), 0xA5);
  output_length = sizeof(output);
  REQUIRE(RunKernelCall(runtime.kernel_state(), [&] {
    call_result = NetDll_recvfrom_entry(
        0, receiver_handle, mapped_void::from_host(received.data()), received.size(), 0,
        ppc_ptr_t<XSOCKADDR_IN>::from_host(reinterpret_cast<XSOCKADDR_IN*>(output.data())),
        mapped_u32::from_host(&output_length));
  }));
  REQUIRE(call_result == payload.size());
  CHECK(received == payload);
  CHECK(reinterpret_cast<XSOCKADDR_IN*>(output.data())->sin_family ==
        rex::system::XSocket::X_AF_INET);
  CHECK(std::all_of(output.begin() + sizeof(XSOCKADDR_IN), output.end(),
                    [](uint8_t byte) { return byte == 0xA5; }));

  std::fill(output.begin(), output.end(), 0xA5);
  output_length = sizeof(output);
  REQUIRE(RunKernelCall(runtime.kernel_state(), [&] {
    call_result = NetDll_recvfrom_entry(
        0, receiver_handle, mapped_void::from_host(received.data()), received.size(), 0,
        ppc_ptr_t<XSOCKADDR_IN>::from_host(reinterpret_cast<XSOCKADDR_IN*>(output.data())),
        mapped_u32::from_host(&output_length));
  }));
  CHECK(call_result == UINT32_MAX);
  CHECK(output_length == sizeof(output));
  CHECK(std::all_of(output.begin(), output.end(), [](uint8_t byte) { return byte == 0xA5; }));

  REQUIRE(RunKernelCall(runtime.kernel_state(), [&] {
    call_result = NetDll_recvfrom_entry(0, receiver_handle, mapped_void::from_host(received.data()),
                                        received.size(), 0, ppc_ptr_t<XSOCKADDR_IN>(nullptr),
                                        mapped_u32(nullptr));
  }));
  CHECK(call_result == UINT32_MAX);
  CHECK(NetDll_closesocket_entry(0, receiver_handle) == 0);
}

#endif
