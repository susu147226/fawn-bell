#include <cstdio>

bool RunThumbnailCacheTests();
int main() {
    // Unbuffered: a failure abort must still leave every earlier result visible.
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("[start] thumbnail cache unit test\n");
    const bool ok = RunThumbnailCacheTests();
    std::printf("[done] %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
