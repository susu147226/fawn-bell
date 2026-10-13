#pragma once
#ifdef LULING_PREVIEW_IMAGE_CACHE_RACE_TEST
namespace luling::preview {
// Called only by the dedicated executable; all filesystem operations stay real.
void PreviewImageCacheTestStage(unsigned stage); // 1: opened; 2: closed, before rename
}
#endif
