#include "convert_rules.h"

#include "batch_rename.h"
#include "session.h"
#include "../common/ffmpeg_tool.h"
#include "../common/localization.h"
#include "../common/utf8_file.h"
#include "../fs/fs_enum.h"

#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <algorithm>
#include <cwchar>
#include <cwctype>

namespace luling::app {

// Palette sizes for the lossy PNG path, mirroring what pngquant-style tools
// offer. Declared here rather than inside the anonymous namespace below, because
// both the encoder arguments and the size estimate use it.
int PaletteColorsFor(int quality) {
    if (quality >= 85) return 256;
    if (quality >= 70) return 128;
    if (quality >= 50) return 64;
    if (quality >= 30) return 32;
    return 16;
}

namespace {

// Pixel size of an image, read from the header only — cheap enough to run for
// every file in the selection. Returns false when the file is not an image WIC
// can open.
bool ReadImageSize(const std::wstring& path, int& width, int& height) {
    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))))
        return false;
    IWICBitmapDecoder* decoder = nullptr;
    const HRESULT opened = factory->CreateDecoderFromFilename(
        path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
    bool ok = false;
    if (SUCCEEDED(opened) && decoder) {
        IWICBitmapFrameDecode* frame = nullptr;
        if (SUCCEEDED(decoder->GetFrame(0, &frame)) && frame) {
            UINT w = 0, h = 0;
            if (SUCCEEDED(frame->GetSize(&w, &h)) && w > 0 && h > 0) {
                width = static_cast<int>(w);
                height = static_cast<int>(h);
                ok = true;
            }
            frame->Release();
        }
        decoder->Release();
    }
    factory->Release();
    return ok;
}

uint64_t FileSizeBytes(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

constexpr std::wstring_view kImageExtensions[] = {
    L".png", L".jpg", L".jpeg", L".bmp", L".gif", L".tif", L".tiff", L".webp",
};

bool IsImageExtension(std::wstring_view extension) {
    for (const auto known : kImageExtensions)
        if (known == extension) return true;
    return false;
}

std::wstring LowerExtension(const std::wstring& path) {
    std::wstring stem, ext;
    SplitFileName(path, stem, ext);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return ext;
}

// The scale filter that reaches the target box the way the user picked.
// Every variant needs only scale/pad/crop, all of which ship in this pack.
// The padding colour depends on the output format: a PNG can carry transparency,
// and filling it with black (ffmpeg's default) is the one thing users notice
// immediately. Formats without an alpha channel get white, which reads as "empty"
// rather than as a black bar.
std::wstring ScaleFilter(const ConvertOptions& options, const std::wstring& extension) {
    // ByFactor needs no box at all: it multiplies whatever the source is.
    if (options.scale_mode == ScaleMode::ByFactor) {
        wchar_t factor[64]{};
        swprintf_s(factor, L"%.6g", options.scale_factor > 0.01 ? options.scale_factor : 1.0);
        return L"scale=iw*" + std::wstring(factor) + L":ih*" + std::wstring(factor) + L":flags=lanczos";
    }
    if (options.width <= 0 && options.height <= 0) return {};
    // One side blank means "keep the ratio, derive the other".
    if (options.height <= 0)
        return L"scale=" + std::to_wstring(options.width) + L":-2:flags=lanczos";
    if (options.width <= 0)
        return L"scale=-2:" + std::to_wstring(options.height) + L":flags=lanczos";
    const std::wstring w = std::to_wstring(options.width);
    const std::wstring h = std::to_wstring(options.height);
    switch (options.size_fit) {
    case SizeFit::Stretch:
        return L"scale=" + w + L":" + h + L":flags=lanczos";
    case SizeFit::Crop:
        // Cover the box, then centre-crop the overflow.
        return L"scale=" + w + L":" + h + L":force_original_aspect_ratio=increase:flags=lanczos,"
               L"crop=" + w + L":" + h;
    case SizeFit::NoPad:
        // Fit inside and stop: no borders, no cropping, the picture itself is
        // just scaled to whatever the ratio allows.
        return L"scale=" + w + L":" + h + L":force_original_aspect_ratio=decrease:flags=lanczos";
    case SizeFit::Pad:
    default: {
        const bool alpha = extension == L"png" || extension == L"webp" ||
                           extension == L"gif" || extension == L"tif" || extension == L"tiff";
        const std::wstring fill = alpha ? L"color=#00000000" : L"color=white";
        return L"scale=" + w + L":" + h + L":force_original_aspect_ratio=decrease:flags=lanczos,"
               L"pad=" + w + L":" + h + L":(ow-iw)/2:(oh-ih)/2:" + fill;
    }
    }
}

// A free name beside `directory`, so a new output never overwrites a sibling.
std::wstring UniqueName(const std::wstring& directory, const std::wstring& stem,
                        const std::wstring& extension) {
    const std::wstring separator = directory.empty() || directory.back() == L'\\' ? L"" : L"\\";
    std::wstring candidate = directory + separator + stem + L"." + extension;
    for (int n = 2; n < 10000 && GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES; ++n) {
        candidate = directory + separator + stem + L" (" + std::to_wstring(n) + L")." + extension;
    }
    return candidate;
}

std::wstring DirectoryOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash);
}

// x264/x265 take -preset; the AV1/VP9 encoders take a cpu-used number.
const wchar_t* PresetForSpeed(VideoSpeed speed) {
    switch (speed) {
    case VideoSpeed::Fast: return L"veryfast";
    case VideoSpeed::Medium: return L"medium";
    case VideoSpeed::Slow: return L"slow";
    }
    return L"medium";
}

const wchar_t* CpuUsedForSpeed(VideoSpeed speed) {
    switch (speed) {
    case VideoSpeed::Fast: return L"6";
    case VideoSpeed::Medium: return L"4";
    case VideoSpeed::Slow: return L"2";
    }
    return L"4";
}

// One output option list per container + codec + speed. The audio codec follows
// the container (webm takes Opus), and mp4/mov get faststart so the result
// streams instead of needing a full download first.
std::vector<std::wstring> VideoFormatArgs(const std::wstring& container, VideoCodec codec,
                                          VideoSpeed speed) {
    std::vector<std::wstring> args;
    switch (codec) {
    case VideoCodec::H264:
        args = { L"-c:v", L"libx264", L"-preset", PresetForSpeed(speed), L"-crf", L"23",
                 L"-pix_fmt", L"yuv420p" };
        break;
    case VideoCodec::H265:
        args = { L"-c:v", L"libx265", L"-preset", PresetForSpeed(speed), L"-crf", L"28",
                 L"-pix_fmt", L"yuv420p" };
        break;
    case VideoCodec::AV1:
        args = { L"-c:v", L"libaom-av1", L"-crf", L"32", L"-b:v", L"0",
                 L"-cpu-used", CpuUsedForSpeed(speed), L"-row-mt", L"1" };
        break;
    case VideoCodec::VP9:
        args = { L"-c:v", L"libvpx-vp9", L"-crf", L"32", L"-b:v", L"0",
                 L"-deadline", L"good", L"-cpu-used", CpuUsedForSpeed(speed), L"-row-mt", L"1" };
        break;
    }
    if (container == L"webm") {
        args.push_back(L"-c:a"); args.push_back(L"libopus");
        args.push_back(L"-b:a"); args.push_back(L"128k");
    } else {
        args.push_back(L"-c:a"); args.push_back(L"aac");
        args.push_back(L"-b:a"); args.push_back(L"160k");
    }
    if (container == L"mp4" || container == L"mov") {
        args.push_back(L"-movflags");
        args.push_back(L"+faststart");
    }
    return args;
}

std::vector<std::wstring> AudioFormatArgs(const std::wstring& format) {
    if (format == L"mp3") return { L"-c:a", L"libmp3lame", L"-q:a", L"2" };
    if (format == L"m4a" || format == L"aac") return { L"-c:a", L"aac", L"-b:a", L"192k" };
    if (format == L"flac") return { L"-c:a", L"flac" };
    if (format == L"opus") return { L"-c:a", L"libopus", L"-b:a", L"128k" };
    if (format == L"wav") return { L"-c:a", L"pcm_s16le" };
    if (format == L"ogg") return { L"-c:a", L"libvorbis", L"-q:a", L"5" };
    return { L"-c:a", L"libmp3lame", L"-q:a", L"2" };
}

// ffmpeg's mjpeg encoder takes a 1–31 qscale, where 1 is best. The rest of the
// world (and the user) thinks in 0–100.
const wchar_t* QscaleForQuality(int quality) {
    static thread_local wchar_t text[8];
    const int clamped = (std::max)(1, (std::min)(100, quality));
    swprintf_s(text, L"%d", (std::max)(1, (std::min)(31, (100 - clamped) * 31 / 100)));
    return text;
}

// Target-format writers for 转换格式. JPEG/WebP use the user's quality; the rest
// are lossless containers where quality has no meaning.
std::vector<std::wstring> ImageFormatArgs(const std::wstring& format, int quality) {
    if (format == L"png") return { L"-c:v", L"png" };
    if (format == L"bmp") return { L"-c:v", L"bmp" };
    if (format == L"gif") return { L"-c:v", L"gif" };
    if (format == L"tif" || format == L"tiff") return { L"-c:v", L"tiff" };
    if (format == L"webp") return { L"-c:v", L"libwebp", L"-quality", std::to_wstring(quality) };
    return { L"-c:v", L"mjpeg", L"-q:v", QscaleForQuality(quality) };  // jpg / jpeg
}

// The encoder that writes `extension`, used whenever the format must stay as it
// is: resizing and compressing never change the container or codec family.
std::vector<std::wstring> SameFormatArgs(const std::wstring& extension) {
    if (extension == L"png") return { L"-c:v", L"png" };
    if (extension == L"bmp") return { L"-c:v", L"bmp" };
    if (extension == L"gif") return { L"-c:v", L"gif" };
    if (extension == L"tif" || extension == L"tiff") return { L"-c:v", L"tiff" };
    if (extension == L"webp") return { L"-c:v", L"libwebp", L"-quality", L"92" };
    if (extension == L"jpg" || extension == L"jpeg") return { L"-c:v", L"mjpeg", L"-q:v", L"2" };
    if (extension == L"mp4") return VideoFormatArgs(L"mp4", VideoCodec::H264, VideoSpeed::Medium);
    if (extension == L"mov") return VideoFormatArgs(L"mov", VideoCodec::H264, VideoSpeed::Medium);
    if (extension == L"mkv") return VideoFormatArgs(L"mkv", VideoCodec::H264, VideoSpeed::Medium);
    if (extension == L"webm") return VideoFormatArgs(L"webm", VideoCodec::VP9, VideoSpeed::Medium);
    // Audio containers.
    return AudioFormatArgs(extension);
}

// Compression keeps the format *and* the pixel size. A PNG stays a PNG; only the
// encoder effort changes. The tier is a coarse shortcut over the same 0–100
// quality the slider sets; Lossless means "no pixels touched", and for a format
// that is already lossy (JPEG, WebP) it means the highest quality that encoder
// offers, because a truly lossless pass does not exist for them.
//
// PNG note: ffmpeg's default predictor (none) beats every explicit -pred we
// measured — "-pred mixed" reliably made files *larger* than the original, which
// is exactly what the tiers are supposed to avoid — so only the zlib level is set.
std::vector<std::wstring> ImageCompressArgs(const std::wstring& extension, int quality) {
    const bool lossless = quality >= 100;
    if (extension == L"png") {
        if (lossless)
            return { L"-c:v", L"png", L"-compression_level", L"9" };
        // Lossy PNG: quantise to a palette. This is what pngquant does and it is
        // the only way a PNG gets dramatically smaller while staying a PNG — the
        // zlib effort alone only ever buys a few percent.
        const std::wstring colors = std::to_wstring(PaletteColorsFor(quality));
        return { L"-vf", L"split[a][b];[a]palettegen=max_colors=" + colors +
                          L":stats_mode=diff[p];[b][p]paletteuse=dither=sierra2_4a",
                 L"-c:v", L"png" };
    }
    if (extension == L"jpg" || extension == L"jpeg")
        return { L"-c:v", L"mjpeg", L"-q:v", QscaleForQuality(quality) };
    if (extension == L"webp") {
        if (lossless) return { L"-c:v", L"libwebp", L"-lossless", L"1", L"-compression_level", L"6" };
        return { L"-c:v", L"libwebp", L"-quality", std::to_wstring(quality) };
    }
    if (extension == L"bmp") return { L"-c:v", L"bmp" };
    if (extension == L"gif") return { L"-c:v", L"gif" };
    if (extension == L"tif" || extension == L"tiff")
        return { L"-c:v", L"tiff", L"-compression_algo", lossless ? L"zip" : L"lzw" };
    return { L"-c:v", L"png", L"-compression_level", L"9" };
}

// Video compression is a CRF ladder: 18 is visually lossless, 33 clearly small.
// The container is whatever the source already used.
std::vector<std::wstring> VideoCompressArgs(const std::wstring& extension, CompressTier tier) {
    const wchar_t* crf = tier == CompressTier::Lossless ? L"18"
                       : tier == CompressTier::Light ? L"23"
                       : tier == CompressTier::Medium ? L"28" : L"33";
    if (extension == L"webm")
        return { L"-c:v", L"libvpx-vp9", L"-crf", crf, L"-b:v", L"0", L"-c:a", L"libopus", L"-b:a", L"128k" };
    std::vector<std::wstring> args{ L"-c:v", L"libx264", L"-preset", L"medium", L"-crf", crf,
                                    L"-c:a", L"aac", L"-b:a", L"160k" };
    if (extension == L"mp4" || extension == L"mov") {
        args.push_back(L"-movflags");
        args.push_back(L"+faststart");
    }
    return args;
}

// Audio compression keeps the codec; only the bitrate/quality step moves.
std::vector<std::wstring> AudioCompressArgs(const std::wstring& extension, CompressTier tier) {
    const bool light = tier == CompressTier::Light || tier == CompressTier::Lossless;
    if (extension == L"mp3") return { L"-c:a", L"libmp3lame", L"-q:a", light ? L"0" : L"5" };
    if (extension == L"m4a" || extension == L"aac") return { L"-c:a", L"aac", L"-b:a", light ? L"256k" : L"128k" };
    if (extension == L"flac") return { L"-c:a", L"flac" };
    if (extension == L"opus") return { L"-c:a", L"libopus", L"-b:a", light ? L"160k" : L"96k" };
    if (extension == L"ogg") return { L"-c:a", L"libvorbis", L"-q:a", light ? L"7" : L"4" };
    if (extension == L"wav") return { L"-c:a", L"pcm_s16le" };
    return { L"-c:a", L"libmp3lame", L"-q:a", light ? L"0" : L"5" };
}

bool IsOfflinePlaceholder(DWORD attributes) {
    constexpr DWORD kRecall = FILE_ATTRIBUTE_OFFLINE | 0x00400000u | 0x00040000u;  // OFFLINE | RECALL_ON_DATA_ACCESS | RECALL_ON_OPEN
    return (attributes & kRecall) != 0;
}

} // namespace

// Measured on real files (an 18 KB PNG icon, encoded to each format at a range of
// qualities). The shape is the same for every image: a floor that lower quality
// cannot get under, plus a cubic term that only matters near 100%. Fitting it
// means the dialog can show a size for every row on every slider tick without
// running the encoder — which is what makes the update feel instant.
double EstimatedCompressRatio(int quality, const std::wstring& extension) {    // Lossless containers ignore the quality setting entirely — ImageCompressArgs
    // only changes the zlib effort for them — so they must NOT be run through the
    // lossy curve, which used to promise half of what the encoder actually wrote.
    if (extension == L"png") return 0.79;    // measured 16840 -> 13305
    if (extension == L"tif" || extension == L"tiff") return 0.85;
    if (extension == L"bmp") return 1.0;     // no compression to speak of
    if (extension == L"gif") return 0.95;
    const int q = (std::max)(1, (std::min)(100, quality));
    if (q >= 100) return 0.95;               // highest quality a lossy encoder offers
    const double t = (q / 100.0) * (q / 100.0) * (q / 100.0);
    if (extension == L"webp") return 0.10 + 0.32 * t;
    // jpg / jpeg and anything else routed to the JPEG encoder.
    return 0.17 + 0.34 * t;
}

bool FormatUsesQuality(const std::wstring& extension) {
    return extension == L"jpg" || extension == L"jpeg" || extension == L"webp";
}

bool CompressUsesQuality(const std::wstring& extension) {
    // PNG compresses lossily by quantising colours; TIFF switches between zip
    // and lzw; JPEG and WebP are the usual quality encoders.
    return extension == L"png" || extension == L"tif" || extension == L"tiff" ||
           extension == L"jpg" || extension == L"jpeg" || extension == L"webp";
}

std::wstring CompressTierLabel(CompressTier tier, const std::wstring& extension) {
    // A PNG has no quality knob — it has a palette size, which is what the tier
    // really controls. Saying "90%" there would be a lie the user could check.
    if (extension == L"png") {
        switch (tier) {
        case CompressTier::Lossless: return l10n::Pick(L"无损优化", L"Lossless");
        case CompressTier::Light: return l10n::Pick(L"轻度 · 256 色", L"Light · 256 colours");
        case CompressTier::Medium: return l10n::Pick(L"中度 · 128 色", L"Medium · 128 colours");
        case CompressTier::Strong: return l10n::Pick(L"强力 · 64 色", L"Strong · 64 colours");
        }
    }
    return CompressTierName(tier);
}

// The quality each tier stands for; the chips are shortcuts onto the same scale
// the slider moves over.
int TierQuality(CompressTier tier) {
    switch (tier) {
    case CompressTier::Lossless: return 100;
    case CompressTier::Light: return 90;
    case CompressTier::Medium: return 75;
    case CompressTier::Strong: return 55;
    }
    return 75;
}

std::vector<VideoCodec> CodecsForContainer(const std::wstring& container) {
    // webm only carries VP9/AV1 video; mp4/mov take the MPEG family and AV1;
    // mkv takes everything.
    if (container == L"webm") return { VideoCodec::VP9, VideoCodec::AV1 };
    if (container == L"mp4" || container == L"mov")
        return { VideoCodec::H264, VideoCodec::H265, VideoCodec::AV1 };
    return { VideoCodec::H264, VideoCodec::H265, VideoCodec::AV1, VideoCodec::VP9 };
}

const wchar_t* VideoCodecName(VideoCodec codec) {
    switch (codec) {
    case VideoCodec::H264: return L"H.264";
    case VideoCodec::H265: return L"H.265";
    case VideoCodec::AV1: return L"AV1";
    case VideoCodec::VP9: return L"VP9";
    }
    return L"";
}

const wchar_t* VideoSpeedName(VideoSpeed speed) {
    switch (speed) {
    case VideoSpeed::Fast: return l10n::Pick(L"快", L"Fast");
    case VideoSpeed::Medium: return l10n::Pick(L"中", L"Medium");
    case VideoSpeed::Slow: return l10n::Pick(L"慢（更小）", L"Slow (smaller)");
    }
    return L"";
}

ConvertKind ClassifyConvertFile(const std::wstring& path) {
    const std::wstring extension = LowerExtension(path);
    if (extension.empty()) return ConvertKind::Other;
    if (IsImageExtension(extension)) return ConvertKind::Image;
    if (ffmpeg::IsVideoExtension(extension)) return ConvertKind::Video;
    if (ffmpeg::IsAudioExtension(extension)) return ConvertKind::Audio;
    return ConvertKind::Other;
}

std::vector<std::wstring> ConvertFormatChoices(ConvertKind kind) {
    switch (kind) {
    case ConvertKind::Image: return { L"jpg", L"png", L"webp", L"bmp", L"gif", L"tiff" };
    case ConvertKind::Video: return { L"mp4", L"mkv", L"webm", L"mov" };
    case ConvertKind::Audio: return { L"mp3", L"m4a", L"flac", L"opus", L"wav", L"ogg" };
    case ConvertKind::Other: return {};
    }
    return {};
}

// Tier names carry their quality, the way Squoosh and TinyPNG present it: a
// bare "medium" tells the user nothing about what they will get.
const wchar_t* CompressTierName(CompressTier tier) {
    switch (tier) {
    case CompressTier::Lossless: return L"无损压缩";
    case CompressTier::Light: return L"轻度 · 90%";
    case CompressTier::Medium: return L"中度 · 75%";
    case CompressTier::Strong: return L"强力 · 55%";
    }
    return L"";
}

const wchar_t* ScaleModeName(ScaleMode mode) {
    switch (mode) {
    case ScaleMode::BySize: return l10n::Pick(L"按尺寸", L"By size");
    case ScaleMode::ByFactor: return l10n::Pick(L"按倍率", L"By factor");
    }
    return L"";
}

const wchar_t* ScaleModeHint(ScaleMode mode) {
    switch (mode) {
    case ScaleMode::BySize: return l10n::Pick(
        L"填入目标宽高，再选它怎么落进这个框。只填一边时按原比例算另一边。",
        L"Give a target width and height, then choose how the picture meets that box. One side alone keeps the original ratio.");
    case ScaleMode::ByFactor: return l10n::Pick(
        L"按倍率整体缩放：1.0 保持原样、1.2 放大两成、0.5 减半，比例不变。",
        L"Multiplies the original size by the factor — 1.0 leaves it alone, 1.2 enlarges it by 20%, 0.5 halves it. The ratio never changes.");
    }
    return L"";
}

const wchar_t* SizeFitName(SizeFit fit) {
    switch (fit) {
    case SizeFit::NoPad: return l10n::Pick(L"等比缩放", L"Scale");
    case SizeFit::Pad: return l10n::Pick(L"居中留边", L"Pad");
    case SizeFit::Crop: return l10n::Pick(L"居中裁剪", L"Crop");
    case SizeFit::Stretch: return l10n::Pick(L"拉伸变形", L"Stretch");
    }
    return L"";
}

const wchar_t* SizeFitHint(SizeFit fit) {
    switch (fit) {
    case SizeFit::NoPad: return l10n::Pick(
        L"保持比例缩到框内，不加白边也不裁剪；输出尺寸随比例。",
        L"Keeps the ratio and fits inside, with no borders and no cropping.");
    case SizeFit::Pad: return l10n::Pick(
        L"保持比例缩到框内，四周留白补边，输出恰好是目标尺寸。",
        L"Keeps the ratio, fits inside and pads the rest, so the output is exactly the target size.");
    case SizeFit::Crop: return l10n::Pick(
        L"保持比例铺满整个框，多出来的部分居中裁掉。",
        L"Keeps the ratio, covers the box and centre-crops the overflow.");
    case SizeFit::Stretch: return l10n::Pick(
        L"直接拉成目标宽高；比例不同时画面会变形。",
        L"Scales straight to the box; the picture distorts when the ratio differs.");
    }
    return L"";
}

namespace {

// 1080×2640 (phone portrait) is the one size the author asked to keep built in;
// everything else is whatever the user saves.
constexpr ConvertSizePreset kBuiltinPresets[] = { {1080, 2640} };

std::wstring PresetsPath() {
    const std::wstring dir = GetLulingDataDir();
    return dir.empty() ? std::wstring{} : dir + L"\\convert_presets.txt";
}

} // namespace

std::vector<ConvertSizePreset> LoadConvertSizePresets() {
    std::vector<ConvertSizePreset> presets(kBuiltinPresets,
                                    kBuiltinPresets + sizeof(kBuiltinPresets) / sizeof(kBuiltinPresets[0]));
    std::wstring text;
    const std::wstring path = PresetsPath();
    if (path.empty() || !ReadUtf8File(path, text)) return presets;
    // One "<width>x<height>" per line; anything else is ignored.
    for (size_t at = 0; at < text.size();) {
        size_t end = text.find(L'\n', at);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
        const size_t x = line.find(L'x');
        if (x == std::wstring::npos || x == 0) continue;
        const ConvertSizePreset preset{ _wtoi(line.substr(0, x).c_str()), _wtoi(line.substr(x + 1).c_str()) };
        if (preset.width <= 0 || preset.height <= 0) continue;
        const bool duplicate = std::any_of(presets.begin(), presets.end(), [&](const ConvertSizePreset& p) {
            return p.width == preset.width && p.height == preset.height;
        });
        if (!duplicate) presets.push_back(preset);
    }
    return presets;
}

bool SaveConvertSizePreset(const ConvertSizePreset& preset) {
    if (preset.width <= 0 || preset.height <= 0) return false;
    const std::wstring path = PresetsPath();
    if (path.empty()) return false;
    const auto all = LoadConvertSizePresets();
    if (std::any_of(all.begin(), all.end(), [&](const ConvertSizePreset& p) {
            return p.width == preset.width && p.height == preset.height;
        }))
        return true;  // already offered, including the built-in
    std::wstring text;
    ReadUtf8File(path, text);
    if (!text.empty() && text.back() != L'\n') text += L"\r\n";
    text += std::to_wstring(preset.width) + L"x" + std::to_wstring(preset.height) + L"\r\n";
    return WriteUtf8FileAtomic(path, text);
}

std::vector<ResizeGroup> BuildResizeGroups(const std::vector<ConvertItem>& items,
                                           int fallback_width, int fallback_height) {
    std::vector<ResizeGroup> groups;
    for (const auto& item : items) {
        if (item.source_width <= 0 || item.source_height <= 0) continue;
        const bool known = std::any_of(groups.begin(), groups.end(), [&](const ResizeGroup& g) {
            return g.source_width == item.source_width && g.source_height == item.source_height;
        });
        if (!known) {
            ResizeGroup group;
            group.source_width = item.source_width;
            group.source_height = item.source_height;
            group.target_width = fallback_width > 0 ? fallback_width : item.source_width;
            group.target_height = fallback_height > 0 ? fallback_height : item.source_height;
            groups.push_back(group);
        }
    }
    return groups;
}

// The box this item should be written at: its group's target when it has one,
// otherwise the dialog-wide width/height.
void ResolveTargetBox(const ConvertItem& item, const ConvertOptions& options,
                      int& width, int& height) {
    width = options.width;
    height = options.height;
    for (const auto& group : options.groups) {
        if (group.source_width == item.source_width && group.source_height == item.source_height) {
            width = group.target_width;
            height = group.target_height;
            return;
        }
    }
}

// Formats that can store an alpha channel. Writing a transparent picture to any
// other format drops it, and ffmpeg's default is to leave the see-through parts
// black — which is what users notice first. Those targets get a white backdrop
// composited underneath instead.
bool ContainerSupportsAlpha(const std::wstring& extension) {
    return extension == L"png" || extension == L"webp" || extension == L"gif" ||
           extension == L"tif" || extension == L"tiff";
}

std::vector<ConvertItem> PreviewConvert(const std::vector<std::wstring>& paths,
                                        const ConvertOptions& options) {
    std::vector<ConvertItem> items;
    items.reserve(paths.size());
    for (const std::wstring& path : paths) {
        ConvertItem item;
        item.source = path;
        item.source_name = path.substr(path.find_last_of(L"\\/") == std::wstring::npos
            ? 0 : path.find_last_of(L"\\/") + 1);
        item.kind = ClassifyConvertFile(path);
        item.source_size = FileSizeBytes(path);
        if (item.kind == ConvertKind::Image)
            ReadImageSize(path, item.source_width, item.source_height);
        std::wstring stem, extension;
        SplitFileName(item.source_name, stem, extension);
        const std::wstring source_extension = LowerExtension(path);

        if (item.kind == ConvertKind::Other) {
            item.status = ConvertStatus::Unsupported;
            items.push_back(std::move(item));
            continue;
        }
        const DWORD attributes = fs::IsVirtualPath(path) ? INVALID_FILE_ATTRIBUTES
                                                         : GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            item.status = ConvertStatus::Offline;
            items.push_back(std::move(item));
            continue;
        }
        if (IsOfflinePlaceholder(attributes)) {
            item.status = ConvertStatus::Offline;
            items.push_back(std::move(item));
            continue;
        }
        if ((attributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM)) != 0) {
            item.status = ConvertStatus::Protected;
            items.push_back(std::move(item));
            continue;
        }

        std::wstring target_extension;
        std::wstring target_stem = stem;
        if (options.mode == ConvertMode::Format) {
            target_extension = options.format.empty() ? source_extension.substr(1) : options.format;
        } else if (options.mode == ConvertMode::Resize) {
            target_extension = source_extension.substr(1);
            std::wstring w, h;
            if (options.scale_mode == ScaleMode::ByFactor) {
                // The factor multiplies the file's own size, so the name shows the
                // result rather than a box the user never set.
                const double factor = options.scale_factor > 0.01 ? options.scale_factor : 1.0;
                w = std::to_wstring(static_cast<long long>(std::lround(item.source_width * factor)));
                h = std::to_wstring(static_cast<long long>(std::lround(item.source_height * factor)));
            } else {
                int box_w = 0, box_h = 0;
                ResolveTargetBox(item, options, box_w, box_h);
                w = box_w > 0 ? std::to_wstring(box_w) : L"auto";
                h = box_h > 0 ? std::to_wstring(box_h) : L"auto";
            }
            target_stem = stem + L"-" + w + L"x" + h;
        } else {
            // Compression keeps the format and the pixel size; only the encoder
            // effort changes. (A PNG therefore stays a PNG.)
            target_extension = source_extension.substr(1);
            target_stem = stem + (options.tier == CompressTier::Lossless ? L"-lossless" : L"-compressed");
        }

        item.target_name = target_stem + L"." + target_extension;
        if (options.target == ConvertTarget::Overwrite) {
            item.target = DirectoryOf(path) + L"\\" + item.target_name;
            if (item.target == path) item.target = DirectoryOf(path) + L"\\" + target_stem + L"." + target_extension;
        } else {
            item.target = UniqueName(DirectoryOf(path), target_stem, target_extension);
            item.target_name = item.target.substr(item.target.find_last_of(L'\\') + 1);
        }
        item.status = ConvertStatus::Ok;
        items.push_back(std::move(item));
    }
    return items;
}

std::vector<std::wstring> BuildConvertArgs(const ConvertItem& item,
                                           const ConvertOptions& options) {
    if (item.status != ConvertStatus::Ok) return {};
    std::wstring target_extension = item.target_name.substr(item.target_name.find_last_of(L'.') + 1);
    std::transform(target_extension.begin(), target_extension.end(), target_extension.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });

    // Each mode does exactly one thing: 转换格式 swaps the container, 调整尺寸
    // keeps it and moves the pixels, 压缩 keeps both and re-encodes harder.
    std::vector<std::wstring> args;
    if (options.mode == ConvertMode::Compress) {
        if (item.kind == ConvertKind::Image) args = ImageCompressArgs(target_extension, options.quality);
        else if (item.kind == ConvertKind::Video) args = VideoCompressArgs(target_extension, options.tier);
        else args = AudioCompressArgs(target_extension, options.tier);
    } else if (options.mode == ConvertMode::Resize) {
        args = SameFormatArgs(target_extension);
        int box_w = 0, box_h = 0;
        ResolveTargetBox(item, options, box_w, box_h);
        ConvertOptions box = options;
        box.width = box_w;
        box.height = box_h;
        const std::wstring filter = ScaleFilter(box, target_extension);
        if (!filter.empty()) {
            if (!ContainerSupportsAlpha(target_extension) && box_w > 0 && box_h > 0) {
                // Scale and the white backdrop have to live in one graph: -vf and
                // -filter_complex cannot be used together.
                args.push_back(L"-filter_complex");
                args.push_back(L"color=white:s=" + std::to_wstring(box_w) + L"x" +
                               std::to_wstring(box_h) + L"[bg];[0]" + filter +
                               L"[fg];[bg][fg]overlay=format=auto");
            } else {
                args.push_back(L"-vf");
                args.push_back(filter);
            }
        }
    } else if (item.kind == ConvertKind::Image) {
        args = ImageFormatArgs(target_extension, options.quality);
        // Converting a transparent picture to JPEG/BMP: lay it on white first.
        if (!ContainerSupportsAlpha(target_extension) &&
            item.source_width > 0 && item.source_height > 0) {
            args.push_back(L"-filter_complex");
            args.push_back(L"color=white:s=" + std::to_wstring(item.source_width) + L"x" +
                           std::to_wstring(item.source_height) + L"[bg];[bg][0]overlay=format=auto");
        }
    } else if (item.kind == ConvertKind::Video) {
        args = VideoFormatArgs(target_extension, options.video_codec, options.video_speed);
    } else {
        args = AudioFormatArgs(target_extension);
    }

    if (item.kind == ConvertKind::Image) {
        args.push_back(L"-frames:v");
        args.push_back(L"1");
    }
    return args;
}

} // namespace luling::app
