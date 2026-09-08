#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <rex/runtime.h>
#include <rex/system/xam/content_manager.h>

using rex::X_RESULT;
using rex::X_STATUS;

namespace {

class ScratchDirectory {
 public:
  ScratchDirectory() {
    const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
    root = std::filesystem::temp_directory_path() /
           ("rexglue_external_content_test_" + std::to_string(token));
    std::filesystem::create_directories(root);
  }

  ~ScratchDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }

  std::filesystem::path root;
};

rex::system::xam::XCONTENT_AGGREGATE_DATA MakeContentData() {
  rex::system::xam::XCONTENT_AGGREGATE_DATA data{};
  data.device_id = 1;
  data.content_type = rex::system::XContentType::kPublisher;
  data.set_file_name("R000100");
  data.xuid = 0;
  data.title_id = 0x12345678;
  return data;
}

std::vector<uint8_t> MakeTestXex() {
  constexpr uint32_t kHeaderSize = 0x400;
  constexpr uint32_t kImageSize = 0x400;
  std::vector<uint8_t> xex(kHeaderSize + kImageSize);

  auto write_be32 = [&xex](size_t offset, uint32_t value) {
    xex[offset] = static_cast<uint8_t>(value >> 24);
    xex[offset + 1] = static_cast<uint8_t>(value >> 16);
    xex[offset + 2] = static_cast<uint8_t>(value >> 8);
    xex[offset + 3] = static_cast<uint8_t>(value);
  };
  auto write_le16 = [&xex](size_t offset, uint16_t value) {
    xex[offset] = static_cast<uint8_t>(value);
    xex[offset + 1] = static_cast<uint8_t>(value >> 8);
  };
  auto write_le32 = [&xex](size_t offset, uint32_t value) {
    xex[offset] = static_cast<uint8_t>(value);
    xex[offset + 1] = static_cast<uint8_t>(value >> 8);
    xex[offset + 2] = static_cast<uint8_t>(value >> 16);
    xex[offset + 3] = static_cast<uint8_t>(value >> 24);
  };

  write_be32(0x00, 0x58455832);  // XEX2
  write_be32(0x04, 1);           // XEX_MODULE_TITLE
  write_be32(0x08, kHeaderSize);
  write_be32(0x10, 0x200);       // security info offset
  write_be32(0x14, 2);           // optional header count
  write_be32(0x18, 0x000003FF);  // XEX_HEADER_FILE_FORMAT_INFO
  write_be32(0x1C, 0x28);
  write_be32(0x20, 0x00040006);  // XEX_HEADER_EXECUTION_INFO
  write_be32(0x24, 0x40);
  write_be32(0x28, 0x0C);               // file format info size
  write_be32(0x40 + 0x00, 0x12345678);  // media ID
  write_be32(0x40 + 0x0C, 0x12345678);  // title ID

  write_be32(0x200 + 0x04, kImageSize);   // image size
  write_be32(0x200 + 0x110, 0x82000000);  // load address
  write_be32(0x200 + 0x180, 0);           // no page descriptors

  const auto image = kHeaderSize;
  write_le32(image + 0x00, 0x00905A4D);  // MZ + DOS stub marker
  write_le32(image + 0x3C, 0x40);        // NT header offset
  write_le32(image + 0x40, 0x00004550);  // PE\0\0
  write_le16(image + 0x44, 0x01F2);      // PowerPC big-endian
  write_le16(image + 0x54, 0x00E0);      // optional header size
  write_le16(image + 0x56, 0x0100);      // 32-bit image
  write_le16(image + 0x58, 0x010B);      // PE32 optional header
  write_le16(image + 0x58 + 68, 14);     // Xbox subsystem
  return xex;
}

}  // namespace

TEST_CASE("External content registration validates directories", "[system][content]") {
  ScratchDirectory scratch;
  const auto external = scratch.root / "external";
  const auto regular_file = scratch.root / "regular-file";
  std::filesystem::create_directory(external);
  std::ofstream(regular_file) << "not a directory";

  rex::system::xam::ContentManager manager(nullptr, scratch.root / "managed");
  const auto data = MakeContentData();

  CHECK(manager.RegisterExternalContent(0, data, external) == X_ERROR_SUCCESS);
  CHECK(manager.ContentExists(0, data));
  CHECK(manager.RegisterExternalContent(0, data, regular_file) == X_ERROR_PATH_NOT_FOUND);
  CHECK(manager.RegisterExternalContent(0, data, scratch.root / "missing") ==
        X_ERROR_PATH_NOT_FOUND);
}

TEST_CASE("External content rejects destructive operations", "[system][content]") {
  ScratchDirectory scratch;
  const auto external = scratch.root / "external";
  std::filesystem::create_directory(external);

  rex::system::xam::ContentManager manager(nullptr, scratch.root / "managed");
  const auto data = MakeContentData();
  REQUIRE(manager.RegisterExternalContent(0, data, external) == X_ERROR_SUCCESS);

  CHECK(manager.SetContentThumbnail(0, data, {1, 2, 3}) == X_ERROR_ACCESS_DENIED);
  CHECK(manager.DeleteContent(0, data) == X_ERROR_ACCESS_DENIED);
  CHECK(manager.UnmountAndDeleteContent(0, data) == X_ERROR_ACCESS_DENIED);
  CHECK(std::filesystem::is_directory(external));
}

TEST_CASE("Denied external unmount-delete preserves the guest mount", "[system][content]") {
  ScratchDirectory scratch;
  const auto game_root = scratch.root / "game";
  const auto user_root = scratch.root / "user";
  const auto external = scratch.root / "external";
  const auto executable_path = game_root / "fixture.xex";
  std::filesystem::create_directories(game_root);
  std::filesystem::create_directories(user_root);
  std::filesystem::create_directory(external);
  std::ofstream(external / "marker.txt") << "mounted";
  const auto executable = MakeTestXex();
  std::ofstream executable_file(executable_path, std::ios::binary);
  executable_file.write(reinterpret_cast<const char*>(executable.data()), executable.size());
  executable_file.close();
  REQUIRE(executable_file.good());

  rex::Runtime runtime(game_root, user_root);
  rex::RuntimeConfig config;
  config.tool_mode = true;
  REQUIRE(runtime.Setup(std::move(config)) == X_STATUS_SUCCESS);
  REQUIRE(runtime.LoadXexImage("game:\\fixture.xex") == X_STATUS_SUCCESS);

  auto* manager = runtime.kernel_state()->content_manager();
  const auto data = MakeContentData();
  REQUIRE(manager->RegisterExternalContent(0, data, external) == X_ERROR_SUCCESS);

  uint32_t content_license = 0;
  REQUIRE(manager->OpenContent("external", 0, data, content_license) == X_ERROR_SUCCESS);
  REQUIRE(runtime.file_system()->ResolvePath("external:\\marker.txt") != nullptr);

  CHECK(manager->UnmountAndDeleteContent(0, data) == X_ERROR_ACCESS_DENIED);
  CHECK(runtime.file_system()->ResolvePath("external:\\marker.txt") != nullptr);
  CHECK(manager->CloseContent("external") == X_ERROR_SUCCESS);
}
