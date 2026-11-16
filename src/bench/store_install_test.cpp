// store_install_util: package checksums and extension removal.

#include "../app/store_install_util.h"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;
using namespace luling::app;

namespace {

int failures = 0;

void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}

std::string ReadText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void WriteText(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

} // namespace

int main() {
    std::wstring digest;
    Check(Sha256BytesHex("abc", 3, digest), "hash bytes");
    Check(digest == L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "sha256 matches the known vector");

    wchar_t temp[MAX_PATH]{};
    GetTempPathW(ARRAYSIZE(temp), temp);
    const fs::path root =
        fs::path(temp) / (L"luling_store_test_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    const fs::path file = root / L"abc.txt";
    WriteText(file, "abc");
    std::wstring file_digest;
    Check(Sha256FileHex(file.wstring(), file_digest), "hash file");
    Check(file_digest == digest, "file and byte digests agree");

    const fs::path ext_root = root / L"extensions";
    fs::create_directories(ext_root / L"keep", ec);
    fs::create_directories(ext_root / L"drop", ec);
    WriteText(ext_root / L"keep" / L"x.txt", "x");
    WriteText(ext_root / L"installed.json",
              R"({"version":1,"installed":[{"id":"keep","version":"1"},{"id":"drop","version":"2"}]})");
    WriteText(ext_root / L"state.json",
              R"({"version":2,"extensions":[{"id":"keep","enabled":true},{"id":"drop","enabled":true}]})");

    std::wstring error;
    Check(UninstallExtensionFrom(ext_root.wstring(), L"drop", error), "uninstall succeeds");
    Check(!fs::exists(ext_root / L"drop"), "extension folder removed");
    Check(fs::exists(ext_root / L"keep" / L"x.txt"), "other folders untouched");
    const std::string installed = ReadText(ext_root / L"installed.json");
    Check(installed.find("\"drop\"") == std::string::npos, "installed.json entry removed");
    Check(installed.find("\"keep\"") != std::string::npos, "other installed entry kept");
    const std::string state = ReadText(ext_root / L"state.json");
    Check(state.find("\"drop\"") == std::string::npos, "state.json entry removed");
    Check(state.find("\"keep\"") != std::string::npos, "other state entry kept");

    Check(!UninstallExtensionFrom(ext_root.wstring(), L"..\\evil", error),
          "an id with separators is rejected");
    Check(fs::exists(ext_root), "nothing was deleted for the rejected id");

    fs::remove_all(root, ec);

    if (failures == 0) {
        std::printf("[done] PASS\n");
        return 0;
    }
    std::printf("[done] FAIL (%d)\n", failures);
    return 1;
}
