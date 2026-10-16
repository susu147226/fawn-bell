// convert_rules.h — Preview-only 转换 rules (no filesystem writes).
//
// The 转换 submenu (0.2.0) opens one dialog for three jobs: convert format,
// resize, compress. Everything the dialog shows and everything the worker runs
// comes from here, so the preview a user approves is exactly what gets executed.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace luling::app {

enum class ConvertMode { Format, Resize, Compress };

// 图片压缩四档. Lossless keeps every pixel; the other three trade quality for
// size, in steps a user can see.
enum class CompressTier { Lossless, Light, Medium, Strong };

// What happens to the original: a new file beside it, or the original replaced.
enum class ConvertTarget { NewFile, Overwrite };

// How a resize reaches the target box. The dialog shows these by name, because
// "resize to 1920x1080" means three different pictures depending on the answer.
// How a resize is specified. The old design had six separate modes; they were
// really two questions — "by what measurement" and "how does it meet the box" —
// so the first is the mode and the second is a fit.
enum class ScaleMode {
    BySize,     // 按尺寸: fill in width/height, then choose how it meets the box
    ByFactor,   // 按倍率: multiply the original size (1.2 = 20% larger)
    ByDelta,    // 按增量: add this many pixels to each side of every picture, so a
                // selection of mixed sizes keeps its own proportions
};

// What happens when the picture's ratio differs from the box.
enum class SizeFit {
    NoPad,    // 等比缩放: keep the ratio, fit inside, no padding — the result is
              // simply smaller in one dimension
    Pad,      // 居中留边: same, then pad the rest with the backdrop colour
    Crop,     // 居中裁剪: cover the box, keep the ratio, crop the overflow
    Stretch,  // 拉伸: ignore the ratio
};

// A named width×height pair, built in or saved by the user. (Advanced search
// already owns the name `SizePreset`, for its own size filter.)
struct ConvertSizePreset {
    int width = 0;
    int height = 0;
};

// Video codec, chosen independently of the container: an mp4 can hold H.264,
// H.265 or AV1. The combinations a container cannot carry are filtered out by
// CodecsForContainer, so the dialog never offers webm + H.264.
enum class VideoCodec { H264, H265, AV1, VP9 };

// How hard the encoder works. Slower buys a smaller file at the same quality,
// which is what HandBrake's speed presets offer.
enum class VideoSpeed { Fast, Medium, Slow };

enum class ConvertKind { Image, Audio, Video, Other };

// Resize works per distinct source size: a selection of mixed sizes is grouped
// by its original dimensions, and each group gets its own target box. A group
// with a single member is simply "that one file", so the same UI covers both
// "grouped automatically" and "set this one individually".
struct ResizeGroup {
    int source_width = 0;
    int source_height = 0;
    int target_width = 0;
    int target_height = 0;
};

struct ConvertOptions {
    ConvertMode mode = ConvertMode::Format;
    std::wstring format;                       // target extension without dot: "jpg", "mp4"
    CompressTier tier = CompressTier::Medium;  // Compress only
    int width = 0;                             // BySize; 0 = derive from height
    int height = 0;                            // BySize; 0 = derive from width
    double scale_factor = 1.0;                 // ByFactor: 1.0 = unchanged
    int delta = 0;                             // ByDelta: pixels added to each side
    ScaleMode scale_mode = ScaleMode::BySize;
    SizeFit size_fit = SizeFit::Pad;           // BySize only
    ConvertTarget target = ConvertTarget::NewFile;
    VideoCodec video_codec = VideoCodec::H264;  // 转换格式, video only
    VideoSpeed video_speed = VideoSpeed::Medium;
    int quality = 90;                           // 转换格式: JPEG/WebP quality, 0-100
    // Resize only. Empty means "use width/height for everything"; otherwise each
    // item's box comes from the group matching its original size.
    std::vector<ResizeGroup> groups;
};

enum class ConvertStatus { Ok, Unsupported, Offline, Protected, Collision };

struct ConvertItem {
    std::wstring source;
    std::wstring source_name;
    std::wstring target;       // full path to write
    std::wstring target_name;
    ConvertKind kind = ConvertKind::Other;
    ConvertStatus status = ConvertStatus::Ok;
    uint64_t source_size = 0;  // bytes; shown so compression has a baseline
    int source_width = 0;      // pixels, images only (0 when unknown)
    int source_height = 0;
};

// Image vs audio vs video from the extension (video/audio tables come from
// ffmpeg_tool.h, so a format the pack can play is a format it can convert).
ConvertKind ClassifyConvertFile(const std::wstring& path);

// Groups the items by their original pixel size, in first-seen order, giving
// each group the fallback box. Items whose size could not be read are left out
// and fall back to width/height (or the first group's box).
std::vector<ResizeGroup> BuildResizeGroups(const std::vector<ConvertItem>& items,
                                           int fallback_width, int fallback_height);

// Output formats offered for a kind, longest-lived first. Empty for Other.
std::vector<std::wstring> ConvertFormatChoices(ConvertKind kind);

// Video codecs that `container` can actually carry, in the order to show them.
std::vector<VideoCodec> CodecsForContainer(const std::wstring& container);
// Display name of a codec ("H.264"…).
const wchar_t* VideoCodecName(VideoCodec codec);
// Display name of an encoder speed ("快 / 中 / 慢").
const wchar_t* VideoSpeedName(VideoSpeed speed);

// The tier's human-readable name, for the preview column and tests.
const wchar_t* CompressTierName(CompressTier tier);
// The same tier described for a specific format: a quality percentage for JPEG
// and WebP, a palette size for PNG (where the tiers quantise colours).
std::wstring CompressTierLabel(CompressTier tier, const std::wstring& extension);
// The 0–100 quality a tier stands for; the chips are shortcuts onto the same
// scale the compression slider moves over.
int TierQuality(CompressTier tier);
// Estimated compressed size as a fraction of the original, for one quality and
// output format. Fitted to measurements, so it costs nothing to call and can
// follow a slider in real time — no encoder run involved.
double EstimatedCompressRatio(int quality, const std::wstring& extension);
// Whether a quality setting changes anything for this format. Lossless
// containers (PNG, BMP, GIF, TIFF) ignore it, so the dialogs hide the quality
// controls for them instead of offering a knob that does nothing.
bool FormatUsesQuality(const std::wstring& extension);
// Same question for 压缩, where the answer differs: PNG has no quality when it
// is a *conversion target* (the encoder is lossless), but compressing a PNG can
// quantise it to a palette, which is exactly how pngquant and the online
// compressors shrink PNGs by half. BMP and GIF have no such mode.
bool CompressUsesQuality(const std::wstring& extension);
// The scale mode's name, shown next to the size fields.
const wchar_t* ScaleModeName(ScaleMode mode);
// Reads a size field: a plain number, or a small arithmetic expression over them
// ("1920/2", "800+200", "1920x1080" — the part before the x is taken). `*` and `/`
// bind tighter than `+` and `-`. False when the text cannot be read, so a caller
// can keep the value it already had instead of showing a zero.
bool EvaluateExpression(const std::wstring& text, double& value);
// One line explaining what the mode does, for the hint under the fields.
const wchar_t* ScaleModeHint(ScaleMode mode);
// The fit's name and explanation ("等比缩放", "居中留边"…).
const wchar_t* SizeFitName(SizeFit fit);
const wchar_t* SizeFitHint(SizeFit fit);

// Built-in presets plus the user's saved ones. The built-in list is deliberately
// short — 1080×2640 is the phone-portrait case the author asked for.
std::vector<ConvertSizePreset> LoadConvertSizePresets();
// Appends a user preset (deduplicated) and writes it to the data directory.
bool SaveConvertSizePreset(const ConvertSizePreset& preset);

// Pure logic: works out each source's target path and whether the job can run.
// Reads file attributes (offline / read-only) but writes nothing.
std::vector<ConvertItem> PreviewConvert(const std::vector<std::wstring>& paths,
                                        const ConvertOptions& options);

// FFmpeg output options for one item — everything between "-i <source>" and the
// output path. Empty for an item that cannot run.
std::vector<std::wstring> BuildConvertArgs(const ConvertItem& item,
                                           const ConvertOptions& options);

} // namespace luling::app
