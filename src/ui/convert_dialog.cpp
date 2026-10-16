#include "convert_dialog.h"
#include "edit_host.h"
#include "FluentTokens.h"
#include "fluent_components.h"
#include "typography.h"
#include "ui_compositor.h"
#include "window_helpers.h"
#include "../common/ffmpeg_tool.h"
#include "../common/localization.h"
#include "../common/preview_packs.h"
#include "../common/temp_files.h"
#include "../common/windows_compat.h"

#include <windowsx.h>
#include <algorithm>
#include <cmath>

namespace luling::ui {
namespace {

// Readable byte count for the preview column.
std::wstring FormatFactor(double factor) {
    wchar_t text[32]{};
    swprintf_s(text, L"%.2f", factor > 0.01 ? factor : 1.0);
    return text;
}

std::wstring FormatSize(uint64_t bytes) {
    wchar_t text[48]{};
    if (bytes >= 1024ull * 1024ull)
        swprintf_s(text, L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else if (bytes >= 1024ull)
        swprintf_s(text, L"%.0f KB", static_cast<double>(bytes) / 1024.0);
    else
        swprintf_s(text, L"%llu B", static_cast<unsigned long long>(bytes));
    return text;
}

// Decodes an image into a D2D bitmap. Used for the before/after thumbnails.
ID2D1Bitmap* LoadBitmapFromFile(ID2D1DeviceContext* dc, const std::wstring& path) {
    if (!dc || path.empty()) return nullptr;
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic))))
        return nullptr;
    ID2D1Bitmap* bitmap = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    if (SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                 WICDecodeMetadataCacheOnDemand, &decoder)) && decoder) {
        IWICBitmapFrameDecode* frame = nullptr;
        if (SUCCEEDED(decoder->GetFrame(0, &frame)) && frame) {
            IWICFormatConverter* converter = nullptr;
            if (SUCCEEDED(wic->CreateFormatConverter(&converter)) && converter) {
                if (SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA,
                                                    WICBitmapDitherTypeNone, nullptr, 0.0,
                                                    WICBitmapPaletteTypeMedianCut)))
                    dc->CreateBitmapFromWicBitmap(converter, nullptr, &bitmap);
                converter->Release();
            }
            frame->Release();
        }
        decoder->Release();
    }
    wic->Release();
    return bitmap;
}

constexpr wchar_t kClass[] = L"LulingConvertWindow";
constexpr float kDlgW = 620.0f;
constexpr float kDlgH = 620.0f;
constexpr float kRowH = 32.0f;
// The before/after preview runs a real ffmpeg pass (and a second one for the
// thumbnail) on the UI thread, which costs a few hundred milliseconds. Rebuilt
// views arm this timer instead of calling it outright, so a burst of clicks or a
// slider drag settles first and the buttons answer immediately.
constexpr UINT_PTR kComparisonTimer = 1;
constexpr UINT kComparisonDelayMs = 250;

D2D1_RECT_F Rect(float scale, float x, float y, float width, float height) {
    return luling::ui::DipRect(scale, x, y, width, height);
}

using app::ConvertKind;
using app::ConvertMode;
using app::ConvertStatus;
using app::CompressTier;
using app::ScaleMode;

// Two questions, not six modes: how the size is given, and how the picture
// meets the box.
constexpr ScaleMode kScaleModes[] = { ScaleMode::BySize, ScaleMode::ByFactor, ScaleMode::ByDelta };
constexpr app::SizeFit kSizeFits[] = {
    app::SizeFit::NoPad, app::SizeFit::Pad, app::SizeFit::Crop, app::SizeFit::Stretch,
};

std::wstring ModeTitle(ConvertMode mode) {
    switch (mode) {
    case ConvertMode::Format: return l10n::Pick(L"转换格式", L"Convert format");
    case ConvertMode::Resize: return l10n::Pick(L"调整尺寸", L"Resize");
    case ConvertMode::Compress: return l10n::Pick(L"压缩", L"Compress");
    }
    return {};
}

std::wstring StatusText(ConvertStatus status) {
    switch (status) {
    case ConvertStatus::Ok: return {};
    case ConvertStatus::Unsupported: return l10n::Pick(L"不支持", L"Unsupported");
    case ConvertStatus::Offline: return l10n::Pick(L"云端文件未下载", L"Not downloaded");
    case ConvertStatus::Protected: return l10n::Pick(L"只读或系统文件", L"Read-only or system file");
    case ConvertStatus::Collision: return l10n::Pick(L"重名", L"Name collision");
    }
    return {};
}

class ConvertWindow {
public:
    ConvertDialogResult Show(HWND owner, const std::vector<std::wstring>& paths, bool dark,
                             D2D1_COLOR_F accent, ConvertMode mode, const app::ConvertOptions* initial) {
        owner_ = owner;
        paths_ = paths;
        dark_ = dark;
        accent_ = accent;
        options_.mode = mode;
        if (initial) options_.format = initial->format;
        if (initial) options_.tier = initial->tier;
        if (initial) options_.target = initial->target;
        scale_ = static_cast<float>(luling::compat::WindowDpi(owner ? owner : GetDesktopWindow())) / 96.0f;
        // The first item decides which format list the dialog offers.
        kind_ = paths_.empty() ? ConvertKind::Other : app::ClassifyConvertFile(paths_.front());
        Rebuild();

        WNDCLASSEXW wc{ sizeof(wc) };
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = WndProc;
        wc.lpszClassName = kClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!GetClassInfoExW(wc.hInstance, kClass, &wc)) RegisterClassExW(&wc);

        const int width = static_cast<int>(kDlgW * scale_);
        const int height = static_cast<int>(kDlgH * scale_);
        hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kClass, ModeTitle(mode).c_str(),
            WS_POPUP | WS_THICKFRAME | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, width, height,
            owner, nullptr, wc.hInstance, this);
        if (!hwnd_) return result_;
        luling::ui::CenterOwnedWindow(hwnd_, owner_, width, height);
        if (owner_) EnableWindow(owner_, FALSE);
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (IsDialogMessageW(hwnd_, &message)) continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        done_ = true;
        if (IsWindow(hwnd_)) HideComposedDialog(hwnd_, owner_);
        if (IsWindow(hwnd_)) DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        if (owner_) EnableWindow(owner_, TRUE);
        return result_;
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<ConvertWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<ConvertWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
        return self->Handle(message, wparam, lparam);
    }

    // ---- geometry ---------------------------------------------------------
    D2D1_RECT_F CloseRect() const { return Rect(scale_, kDlgW - 46, 0, 46, 36); }
    D2D1_RECT_F CancelRect() const { return Rect(scale_, kDlgW - 196, kDlgH - 48, 80, kRowH); }
    D2D1_RECT_F ApplyRect() const { return Rect(scale_, kDlgW - 108, kDlgH - 48, 88, kRowH); }
    // Row 1 carries the mode's main choice: formats, scale modes or tiers. Five
    // per row, since the scale-mode list is now five long.
    static constexpr int kChipsPerRow = 5;
    D2D1_RECT_F ChipRect(int index) const {
        const float width = (kDlgW - 40 - 8 * (kChipsPerRow - 1)) / kChipsPerRow;
        return Rect(scale_, 20 + (index % kChipsPerRow) * (width + 8),
                    56 + (index / kChipsPerRow) * (kRowH + 8), width, kRowH);
    }
    // Row 2 for resize: how the picture meets the box (only when a box is given).
    D2D1_RECT_F FitRect(int index) const {
        const float width = (kDlgW - 40 - 8 * 3) / 4.0f;
        return Rect(scale_, 20 + (index % 4) * (width + 8), 196, width, kRowH);
    }
    // Row 3: the saved sizes.
    D2D1_RECT_F PresetRect(int index) const {
        const float width = (kDlgW - 40 - 8 * 3) / 4.0f;
        return Rect(scale_, 20 + (index % 4) * (width + 8), 236 + (index / 4) * (kRowH + 8), width, kRowH);
    }
    D2D1_RECT_F WidthField() const { return Rect(scale_, 76, 150, 120, kRowH); }
    D2D1_RECT_F HeightField() const { return Rect(scale_, 262, 150, 120, kRowH); }
    // ByFactor reuses the width field's slot for the multiplier.
    D2D1_RECT_F FactorField() const { return Rect(scale_, 76, 150, 160, kRowH); }
    // ByDelta wants one number too, so it takes the same slot.
    D2D1_RECT_F DeltaField() const { return Rect(scale_, 76, 150, 160, kRowH); }
    D2D1_RECT_F SavePresetRect() const { return Rect(scale_, 418, 150, 162, kRowH); }
    // Row 3: image quality when converting formats, encoder speed for video.
    D2D1_RECT_F QualityRect(int index) const {
        const float width = (kDlgW - 40 - 8 * 3) / 4.0f;
        return Rect(scale_, 20 + (index % 4) * (width + 8), 152, width, kRowH);
    }
    D2D1_RECT_F KeepRect() const { return Rect(scale_, 20, 284, 140, kRowH); }
    D2D1_RECT_F OverwriteRect() const { return Rect(scale_, 168, 284, 140, kRowH); }
    D2D1_RECT_F PreviewBounds() const { return Rect(scale_, 20, 332, kDlgW - 40, kDlgH - 332 - 56); }
    // The right-hand column: the resize diagram, or the before/after preview.
    D2D1_RECT_F SideBounds() const {
        return Rect(scale_, kDlgW - 20 - 216, 332, 216, kDlgH - 332 - 56);
    }
    D2D1_RECT_F ListBounds() const {
        return Rect(scale_, 20, 332, kDlgW - 40 - 216, kDlgH - 332 - 56);
    }
    D2D1_RECT_F DiagramBounds() const {
        return Rect(scale_, kDlgW - 20 - 216, 332, 216, 216);
    }
    D2D1_RECT_F SummaryRect() const { return Rect(scale_, 20, kDlgH - 48, kDlgW - 228, kRowH); }

    bool resizing() const { return options_.mode == ConvertMode::Resize; }

    // Two rows of four; a long list simply stops being drawn rather than
    // spilling over the 原文件 row.
    static constexpr int kMaxVisiblePresets = 8;
    // How many size groups the resize list shows before it stops.
    static constexpr int kMaxVisibleGroups = 6;
    int VisiblePresetCount() const {
        return resizing() ? (std::min)(kMaxVisiblePresets, static_cast<int>(presets_.size())) : 0;
    }
    // One row per size group, drawn where the file list normally goes.
    D2D1_RECT_F GroupRowRect(int index) const {
        const auto area = ListBounds();
        const float top = area.top + 8.0f * scale_ + index * 28.0f * scale_;
        return D2D1::RectF(area.left + 6.0f * scale_, top, area.right - 6.0f * scale_, top + 26.0f * scale_);
    }

    int ChipCount() const {
        switch (options_.mode) {
        case ConvertMode::Format: return static_cast<int>(formats_.size());
        case ConvertMode::Resize: return static_cast<int>(std::size(kScaleModes));
        // A lossless target has no tiers to choose: PNG stays PNG either way.
        case ConvertMode::Compress: return compressionHasQuality() ? 4 : 0;
        }
        return 0;
    }

    // Row 2 for resize: how the picture meets the box (only when a box is given).
    int FitCount() const {
        return resizing() && options_.scale_mode == ScaleMode::BySize
            ? static_cast<int>(std::size(kSizeFits)) : 0;
    }

    int Hit(float x, float y) const {
        if (luling::ui::ContainsRect(CloseRect(), x, y)) return 1;
        if (luling::ui::ContainsRect(CancelRect(), x, y)) return 2;
        if (luling::ui::ContainsRect(ApplyRect(), x, y)) return can_apply_ ? 3 : 0;
        if (luling::ui::ContainsRect(KeepRect(), x, y)) return 30;
        if (luling::ui::ContainsRect(OverwriteRect(), x, y)) return 31;
        for (int i = 0; i < ChipCount(); ++i)
            if (luling::ui::ContainsRect(ChipRect(i), x, y)) return 10 + i;
        if (resizing()) {
            // Row 2: the fit, which only means something with a box.
            for (int i = 0; i < FitCount(); ++i) {
                if (luling::ui::ContainsRect(FitRect(i), x, y)) return 50 + i;
            }
            for (int i = 0; i < VisiblePresetCount(); ++i)
                if (luling::ui::ContainsRect(PresetRect(i), x, y)) return 20 + i;
            if (luling::ui::ContainsRect(SavePresetRect(), x, y)) return 28;
            const int groups = static_cast<int>(VisibleGroups().size());
            for (int i = 0; i < groups; ++i)
                if (luling::ui::ContainsRect(GroupRowRect(i), x, y)) return 40 + i;
        }
        // The second chip row is the codec list when converting a video.
        const int codecs = static_cast<int>(CodecChoices().size());
        for (int i = 0; i < codecs; ++i)
            if (luling::ui::ContainsRect(PresetRect(i), x, y)) return 60 + i;
        // The third row is quality (images) or encoder speed (video).
        for (int i = 0; i < ThirdRowCount(); ++i)
            if (luling::ui::ContainsRect(QualityRect(i), x, y)) return 70 + i;
        return 0;
    }

    // ---- state ------------------------------------------------------------
    void Rebuild() {
        formats_ = app::ConvertFormatChoices(kind_);
        if (options_.format.empty() && !formats_.empty()) options_.format = formats_.front();
        // Changing the container can invalidate the codec (webm has no H.264), so
        // fall back to the first one this container actually carries.
        if (kind_ == ConvertKind::Video && options_.mode == ConvertMode::Format) {
            const auto codecs = app::CodecsForContainer(options_.format);
            if (!codecs.empty() &&
                std::find(codecs.begin(), codecs.end(), options_.video_codec) == codecs.end())
                options_.video_codec = codecs.front();
        }
        if (resizing() && presets_.empty()) presets_ = app::LoadConvertSizePresets();

        items_ = app::PreviewConvert(paths_, options_);
        if (resizing() && options_.groups.empty()) {
            // Group by original size. The fallback box is the first item's own
            // size, so opening the mode shows the files as they are.
            int fallback_w = options_.width, fallback_h = options_.height;
            if (fallback_w <= 0 && !items_.empty() && items_.front().source_width > 0) {
                fallback_w = items_.front().source_width;
                fallback_h = items_.front().source_height;
            } else if (fallback_w <= 0 && !presets_.empty()) {
                fallback_w = presets_.front().width;
                fallback_h = presets_.front().height;
            }
            options_.width = fallback_w;
            options_.height = fallback_h;
            options_.groups = app::BuildResizeGroups(items_, fallback_w, fallback_h);
            items_ = app::PreviewConvert(paths_, options_);
        }
        if (resizing() && !options_.groups.empty()) {
            if (selected_group_ >= static_cast<int>(options_.groups.size())) selected_group_ = 0;
            const auto& group = options_.groups[static_cast<size_t>(selected_group_)];
            options_.width = group.target_width;
            options_.height = group.target_height;
            if (src_w_ <= 0) { src_w_ = group.source_width; src_h_ = group.source_height; }
            SetSizeFieldsIfDifferent(group.target_width, group.target_height);
        }

        RefreshCounts();
        // Deferred: see kComparisonTimer. Re-arming an already-armed timer only
        // restarts it, so rapid changes collapse into one run.
        if (hwnd_) SetTimer(hwnd_, kComparisonTimer, kComparisonDelayMs, nullptr);
        // The chips decide which fields are on screen (按尺寸 two boxes, 按倍率 /
        // 按增量 one), so switching mode has to re-place them — laying them out only
        // at creation left the delta field hidden forever.
        LayoutEdits();
    }

    void RefreshCounts() {
        ok_count_ = 0;
        for (const auto& item : items_)
            if (item.status == ConvertStatus::Ok) ++ok_count_;
        // Without the engine there is nothing to run, so 开始 stays disabled.
        can_apply_ = ok_count_ > 0 && engine_ready();
    }

    // The video codecs the current container can carry; empty unless we are
    // converting a video's format.
    std::vector<app::VideoCodec> CodecChoices() const {
        if (options_.mode != ConvertMode::Format || kind_ != ConvertKind::Video) return {};
        return app::CodecsForContainer(options_.format);
    }

    // Row 3 content: quality choices for image format conversion, speed for video.
    // 100% leads, because converting a PNG to JPEG without touching the quality is
    // a thing people ask for and it should be one click, not a slider hunt. Four
    // entries only: QualityRect lays them out in one row and has no wrap.
    static constexpr int kQualities[] = { 100, 90, 80, 65 };
    static constexpr app::VideoSpeed kSpeeds[] = {
        app::VideoSpeed::Fast, app::VideoSpeed::Medium, app::VideoSpeed::Slow,
    };
    bool showsQuality() const {
        return options_.mode == ConvertMode::Format && kind_ == ConvertKind::Image &&
               app::FormatUsesQuality(options_.format);
    }
    // Lower-case extension of the compression target.
    std::wstring compressionExtension() const {
        if (items_.empty()) return {};
        std::wstring extension = items_.front().target_name.substr(
            items_.front().target_name.find_last_of(L'.') + 1);
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        return extension;
    }

    // Compression keeps the source format, so whether the tiers do anything
    // depends on what that format is — PNG counts here (palette quantisation),
    // even though it does not when it is a conversion target.
    bool compressionHasQuality() const {
        if (options_.mode != ConvertMode::Compress || items_.empty()) return false;
        return app::CompressUsesQuality(compressionExtension());
    }
    bool showsSpeed() const {
        return options_.mode == ConvertMode::Format && kind_ == ConvertKind::Video;
    }
    int ThirdRowCount() const {
        if (showsQuality()) return static_cast<int>(std::size(kQualities));
        if (showsSpeed()) return static_cast<int>(std::size(kSpeeds));
        return 0;
    }

    // Runs the real conversion on the first file and shows the result next to the
    // original, so the quality a tier promises is something the user can see
    // before committing (the idea behind Squoosh's side-by-side view).
    // Video is previewed through its first frame: encoding a whole clip here
    // would stall the dialog.
    void UpdateComparison() {
        if (options_.mode == ConvertMode::Resize || items_.empty()) return;
        const app::ConvertItem& item = items_.front();
        if (item.status != ConvertStatus::Ok) return;
        const std::wstring key = item.source + L"|" + std::to_wstring(static_cast<int>(options_.mode)) +
            L"|" + options_.format + L"|" + std::to_wstring(static_cast<int>(options_.tier)) +
            L"|" + std::to_wstring(options_.quality) + L"|" + std::to_wstring(static_cast<int>(options_.target)) +
            L"|" + std::to_wstring(static_cast<int>(options_.video_codec));
        // Bail out before claiming the key: on the very first Rebuild (before the
        // window exists) there is no device context, and caching then would stop
        // the thumbnails from ever being built.
        const std::wstring tool =
            luling::packs::PackToolPath(luling::packs::PackId::Transcode, L"ffmpeg.exe");
        if (tool.empty() || !compositor_.Dc()) return;
        if (key == comparison_key_) return;
        comparison_key_ = key;
        ReleaseComparison();

        // Everything this preview writes goes through MakeTempPath, and every
        // cleanup through DeleteTempFile, which refuses paths outside %TEMP% —
        // so a wrong scratch path can never reach the user's own file.
        const std::wstring base = common::MakeTempPath(L"luling-cmp", L"");
        if (base.empty()) return;
        const bool video = item.kind == ConvertKind::Video;
        const std::wstring source_frame = video ? base + L"-src.png" : item.source;
        const std::wstring result_frame = base + L"-dst.png";
        common::DeleteTempFile(source_frame);
        common::DeleteTempFile(result_frame);
        if (video) ExtractFirstFrame(tool, item.source, source_frame);

        const std::wstring converted = video ? base + L"-clip.mp4" : base + L"." + options_.format;
        const std::wstring thumb = base + L"-thumb.png";
        common::DeleteTempFile(converted);
        common::DeleteTempFile(thumb);
        if (RunTool(tool, item, converted, video)) {
            const std::wstring produced = video ? result_frame : converted;
            if (video) ExtractFirstFrame(tool, converted, result_frame);
            // Shrink for display; a second pass keeps the geometry correct.
            std::wstring command = L"-hide_banner -nostdin -loglevel error -y -i " +
                ffmpeg::QuoteArgument(ffmpeg::InputArgument(produced)) +
                L" -vf " + ffmpeg::QuoteArgument(L"scale=240:240:force_original_aspect_ratio=decrease:flags=lanczos") +
                L" -frames:v 1 " + ffmpeg::QuoteArgument(thumb);
            RunCommand(tool, command);
            cmp_after_ = LoadBitmapFromFile(compositor_.Dc(), thumb);
        }
        cmp_before_ = LoadBitmapFromFile(compositor_.Dc(), source_frame);
        // DeleteTempFile ignores `source_frame` when it is the user's own image,
        // because that path is not inside %TEMP%.
        common::DeleteTempFile(source_frame);
        common::DeleteTempFile(result_frame);
        common::DeleteTempFile(converted);
        common::DeleteTempFile(thumb);
    }

    // The still the before/after preview uses for a video.
    void ExtractFirstFrame(const std::wstring& tool, const std::wstring& from, const std::wstring& to) {
        std::wstring command = L"-hide_banner -nostdin -loglevel error -y -i " +
            ffmpeg::QuoteArgument(ffmpeg::InputArgument(from)) +
            L" -frames:v 1 " + ffmpeg::QuoteArgument(to);
        RunCommand(tool, command, 15000);
    }

    bool RunCommand(const std::wstring& tool, const std::wstring& command, DWORD timeout_ms = 8000) {
        ffmpeg::Process process;
        if (!process.Start(tool, command)) return false;
        const ULONGLONG start = GetTickCount64();
        bool finished = false;
        while (!(finished = process.Wait(120)))
            if (GetTickCount64() - start > timeout_ms) { process.Terminate(); break; }
        return finished && process.ExitCode() == 0;
    }

    // One ffmpeg run over the dialog's own options, writing `out`. Video is
    // limited to a few seconds so the preview stays responsive.
    bool RunTool(const std::wstring& tool, const app::ConvertItem& item, const std::wstring& out,
                 bool video) {
        std::wstring command = L"-hide_banner -nostdin -loglevel error -y -i " +
            ffmpeg::QuoteArgument(ffmpeg::InputArgument(item.source));
        for (const std::wstring& argument : app::BuildConvertArgs(item, options_))
            command += L" " + ffmpeg::QuoteArgument(argument);
        if (video) command += L" -t 2";
        command += L" " + ffmpeg::QuoteArgument(out);
        return RunCommand(tool, command, video ? 30000 : 8000);
    }

    void ReleaseComparison() {
        if (cmp_before_) { cmp_before_->Release(); cmp_before_ = nullptr; }
        if (cmp_after_) { cmp_after_->Release(); cmp_after_ = nullptr; }
    }

    // Compression gets a slider under the tier chips: the chips are shortcuts,
    // the slider is the free adjustment the author asked for. Kept clear of the
    // chip row above (56–88) so the "质量" label has room of its own.
    // The slider's own label is drawn 22px above the track, so the track has to
    // start below whatever sits above it. 转换格式 stacks three things here — the
    // format chips (up to two rows), the quality chips, then the slider — while
    // 压缩 only has its tier chips above, so it keeps the higher slot.
    D2D1_RECT_F SliderBounds() const {
        const float top = options_.mode == ConvertMode::Format ? 206.0f : 126.0f;
        return Rect(scale_, 20, top, kDlgW - 40, 28);
    }
    // 转换格式 had only the four preset chips, so a PNG-to-JPEG conversion could
    // not ask for 100% — anyone who just wanted the format changed had no way to
    // say "leave the quality alone". The slider is that way.
    bool showsSlider() const {
        return (options_.mode == ConvertMode::Compress && compressionHasQuality()) ||
               (options_.mode == ConvertMode::Format && showsQuality());
    }

    void SetQualityFromX(float x) {
        const auto bar = SliderBounds();
        const float left = bar.left + 12.0f * scale_;
        const float right = bar.right - 12.0f * scale_;
        if (right <= left) return;
        const float t = (std::max)(0.0f, (std::min)(1.0f, (x - left) / (right - left)));
        options_.quality = 1 + static_cast<int>(std::lround(t * 99.0f));
        // Keep the chips in step with the slider.
        if (options_.quality >= 100) options_.tier = CompressTier::Lossless;
        else if (options_.quality >= 85) options_.tier = CompressTier::Light;
        else if (options_.quality >= 65) options_.tier = CompressTier::Medium;
        else options_.tier = CompressTier::Strong;
    }

    void DrawSlider(const Theme& theme) {
        const auto bar = SliderBounds();
        const float cy = (bar.top + bar.bottom) / 2.0f;
        const float left = bar.left + 12.0f * scale_;
        const float right = bar.right - 12.0f * scale_;
        painter_.FillRoundedRect(D2D1::RectF(left, cy - 2.0f * scale_, right, cy + 2.0f * scale_),
                                 2.0f * scale_, theme.stroke_card);
        const float t = (options_.quality - 1) / 99.0f;
        const float x = left + (right - left) * t;
        painter_.FillRoundedRect(D2D1::RectF(left, cy - 2.0f * scale_, x, cy + 2.0f * scale_),
                                 2.0f * scale_, theme.accent);
        painter_.FillRoundedRect(D2D1::RectF(x - 8.0f * scale_, cy - 8.0f * scale_,
                                             x + 8.0f * scale_, cy + 8.0f * scale_),
                                 8.0f * scale_, theme.accent);
        wchar_t text[48]{};
        if (options_.quality >= 100)
            swprintf_s(text, L"%s", l10n::Pick(L"质量 100%（无损原样）", L"Quality 100% (untouched)"));
        else
            swprintf_s(text, l10n::Pick(L"质量 %d%%", L"Quality %d%%"), options_.quality);
        painter_.DrawText(text, D2D1::RectF(left, bar.top - 22.0f * scale_, right, bar.top),
                          compositor_.SmallFormat(), theme.text_secondary);
    }

    std::vector<const app::ResizeGroup*> VisibleGroups() const {        std::vector<const app::ResizeGroup*> groups;
        if (!resizing()) return groups;
        for (const auto& group : options_.groups) {
            if (static_cast<int>(groups.size()) >= kMaxVisibleGroups) break;
            groups.push_back(&group);
        }
        return groups;
    }

    int GroupItemCount(const app::ResizeGroup& group) const {
        int count = 0;
        for (const auto& item : items_)
            if (item.source_width == group.source_width && item.source_height == group.source_height)
                ++count;
        return count;
    }

    static bool engine_ready() {
        return luling::packs::ResolvePack(luling::packs::PackId::Transcode).source
            != luling::packs::ToolSource::None;
    }

    void SelectChip(int index) {
        if (options_.mode == ConvertMode::Format) {
            if (index < static_cast<int>(formats_.size())) options_.format = formats_[index];
        } else if (options_.mode == ConvertMode::Resize) {
            if (index < static_cast<int>(std::size(kScaleModes))) options_.scale_mode = kScaleModes[index];
        } else {
            options_.tier = static_cast<CompressTier>(index);
            // The chips are shortcuts: move the slider to match.
            options_.quality = app::TierQuality(options_.tier);
        }
        Rebuild();
    }

    bool ChipSelected(int index) const {
        if (options_.mode == ConvertMode::Format)
            return index < static_cast<int>(formats_.size()) && formats_[index] == options_.format;
        if (options_.mode == ConvertMode::Resize)
            return index < static_cast<int>(std::size(kScaleModes)) && kScaleModes[index] == options_.scale_mode;
        return static_cast<int>(options_.tier) == index;
    }

    // ---- size fields ------------------------------------------------------
    D2D1_COLOR_F EditForeground() const { return ColorFromRef(EditTextColor(dark_)); }
    D2D1_COLOR_F EditBackground() const { return ColorFromRef(EditBackColor(dark_)); }

    HWND CreateField(const std::wstring& text, bool number = true) {
        HWND edit = CreateChildEdit(hwnd_, text.c_str(), number ? ES_NUMBER : 0);
        if (!edit) return nullptr;
        SetWindowTheme(edit, L"", L"");
        if (font_) SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        return edit;
    }

    std::wstring EditText(HWND edit) const {
        if (!edit) return {};
        const int n = GetWindowTextLengthW(edit);
        std::wstring text(static_cast<size_t>(n), L'\0');
        if (n > 0) GetWindowTextW(edit, text.data(), n + 1);
        return text;
    }

    static bool Visible(const D2D1_RECT_F& r) { return r.right > r.left; }

    void PlaceEdit(HWND edit, const D2D1_RECT_F& cell, bool show) {
        if (!edit || !hwnd_) return;
        if (!show || !Visible(cell)) {
            ShowWindow(edit, SW_HIDE);
            return;
        }
        const POINT pt{ static_cast<int>(std::lround(cell.left + 8.0f * scale_)),
                        static_cast<int>(std::lround(cell.top)) };
        const int w = (std::max)(40, static_cast<int>(std::lround(cell.right - cell.left - 16.0f * scale_)));
        const int cell_h = (std::max)(18, static_cast<int>(std::lround(cell.bottom - cell.top)));
        const int line_h = EditLineHeight(edit, font_, cell_h);
        SetWindowPos(edit, HWND_TOP, pt.x, pt.y + (std::max)(0, (cell_h - line_h) / 2), w, line_h,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        PresentChildEdit(compositor_, compositor_.TextFormat(), EditForeground(), EditBackground(), edit);
    }

    void LayoutEdits() {
        if (done_) return;
        const bool resize = resizing();
        // 按倍率 and 按增量 both ask for a single number, not a box.
        const bool box = resize && options_.scale_mode == ScaleMode::BySize;
        PlaceEdit(edit_width_, WidthField(), box);
        PlaceEdit(edit_height_, HeightField(), box);
        PlaceEdit(edit_factor_, FactorField(), resize && options_.scale_mode == ScaleMode::ByFactor);
        PlaceEdit(edit_delta_, DeltaField(), resize && options_.scale_mode == ScaleMode::ByDelta);
    }

    int FieldValue(HWND edit, int fallback) const {
        if (!edit) return fallback;
        // A field may hold an expression ("1920/2", "800+200"); anything that
        // cannot be read keeps the value it already had instead of snapping to 0.
        double value = 0.0;
        if (!app::EvaluateExpression(EditText(edit), value)) return fallback;
        const int rounded = static_cast<int>(std::lround(value));
        return rounded > 0 ? rounded : fallback;
    }

    // The compressed size shown next to each row, and the number in the summary
    // line. Computed from the fitted ratio, so it follows the slider with no
    // encoder run — the whole point of not "previewing" by actually encoding.
    uint64_t EstimatedSizeFor(const app::ConvertItem& item) const {
        if (options_.mode != ConvertMode::Compress || item.source_size == 0) return 0;
        std::wstring extension = item.target_name.substr(item.target_name.find_last_of(L'.') + 1);
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        const double ratio = app::EstimatedCompressRatio(options_.quality, extension);
        return static_cast<uint64_t>(static_cast<double>(item.source_size) * ratio);
    }

    void SyncSizeFromEdits() {
        if (syncing_) return;
        if (resizing() && options_.scale_mode == ScaleMode::ByFactor) {
            // A multiplier, not a pixel size: anything unusable falls back to 1.0
            // rather than scaling by zero.
            const double value = _wtof(EditText(edit_factor_).c_str());
            options_.scale_factor = value > 0.01 ? value : 1.0;
        } else if (resizing() && options_.scale_mode == ScaleMode::ByDelta) {
            // An offset, so zero and negative numbers are both meaningful: only
            // text that cannot be read at all keeps the previous value.
            double value = 0.0;
            if (app::EvaluateExpression(EditText(edit_delta_), value))
                options_.delta = static_cast<int>(std::lround(value));
        } else if (resizing() && selected_group_ >= 0 &&
            selected_group_ < static_cast<int>(options_.groups.size())) {
            auto& group = options_.groups[static_cast<size_t>(selected_group_)];
            group.target_width = FieldValue(edit_width_, group.target_width);
            group.target_height = FieldValue(edit_height_, group.target_height);
            options_.width = group.target_width;
            options_.height = group.target_height;
        } else {
            const app::ConvertSizePreset first = presets_.empty()
                ? app::ConvertSizePreset{ 1080, 2640 } : presets_.front();
            options_.width = FieldValue(edit_width_, first.width);
            options_.height = FieldValue(edit_height_, first.height);
        }
        // Refresh the preview only; rebuilding the groups here would fight the
        // user's typing.
        items_ = app::PreviewConvert(paths_, options_);
        RefreshCounts();
    }

    // Writing the fields fires EN_CHANGE, which would re-enter SyncSizeFromEdits
    // and rebuild the groups mid-edit.
    void SetSizeFieldsIfDifferent(int width, int height) {
        syncing_ = true;
        if (edit_width_ && EditText(edit_width_) != std::to_wstring(width))
            SetWindowTextW(edit_width_, std::to_wstring(width).c_str());
        if (edit_height_ && EditText(edit_height_) != std::to_wstring(height))
            SetWindowTextW(edit_height_, std::to_wstring(height).c_str());
        syncing_ = false;
    }

    void SetSizeFields(int width, int height) { SetSizeFieldsIfDifferent(width, height); }

    // ---- messages ---------------------------------------------------------
    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE:
            scale_ = static_cast<float>(luling::compat::WindowDpi(hwnd_)) / 96.0f;
            backdrop_enabled_ = luling::ui::ApplyBackdrop(hwnd_, dark_);
            if (!compositor_.Init(hwnd_)) return -1;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
            font_ = typography::CreateEditFont(scale_);
            if (edit_brush_) { DeleteObject(edit_brush_); edit_brush_ = nullptr; }
            edit_brush_ = CreateSolidBrush(dark_ ? RGB(30, 30, 30) : RGB(255, 255, 255));
            // The size fields accept expressions ("1920/2", "800+200"), so they
            // must not carry ES_NUMBER, which rejects every operator as it is typed.
            edit_width_ = CreateField(std::to_wstring(options_.width), false);
            edit_height_ = CreateField(std::to_wstring(options_.height), false);
            // The multiplier needs a decimal point, so no ES_NUMBER filter.
            edit_factor_ = CreateField(FormatFactor(options_.scale_factor), false);
            edit_delta_ = CreateField(std::to_wstring(options_.delta), false);
            LayoutEdits();
            // The device context exists only now, so this is the first chance to
            // build the comparison thumbnails and the size estimate.
            Rebuild();
            return 0;
        case WM_CTLCOLOREDIT: {
            const HDC hdc = reinterpret_cast<HDC>(wparam);
            SetTextColor(hdc, EditTextColor(dark_));
            SetBkColor(hdc, EditBackColor(dark_));
            return reinterpret_cast<LRESULT>(edit_brush_);
        }
        case WM_COMMAND:
            if (HIWORD(wparam) == EN_CHANGE) {
                SyncSizeFromEdits();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case WM_NCCALCSIZE:
            return 0;
        case WM_NCHITTEST:
            return luling::ui::BorderlessHitTest(hwnd_, lparam, 36 * scale_, CloseRect());
        case WM_SIZE:
            if (compositor_.Dc()) compositor_.Resize(LOWORD(lparam), HIWORD(lparam));
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_MOVE:
            compositor_.UpdateTextRenderingParams(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
            return 0;
        case WM_TIMER:
            if (wparam == kComparisonTimer) {
                KillTimer(hwnd_, kComparisonTimer);
                UpdateComparison();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_MOUSEMOVE: {
            if (dragging_slider_) {
                SetQualityFromX(static_cast<float>(GET_X_LPARAM(lparam)));
                // Dragging must stay responsive, so only the cheap parts run here:
                // the size estimate and thumbnails wait for the mouse to come up.
                items_ = app::PreviewConvert(paths_, options_);
                RefreshCounts();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            const int next = Hit(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
            if (next != hover_) { hover_ = next; InvalidateRect(hwnd_, nullptr, FALSE); }
            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd_, 0 };
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            hover_ = 0;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            if (showsSlider() &&
                luling::ui::ContainsRect(SliderBounds(),
                                         static_cast<float>(GET_X_LPARAM(lparam)),
                                         static_cast<float>(GET_Y_LPARAM(lparam)))) {
                dragging_slider_ = true;
                SetCapture(hwnd_);
                SetQualityFromX(static_cast<float>(GET_X_LPARAM(lparam)));
                Rebuild();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            pressed_ = Hit(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            if (dragging_slider_) {
                dragging_slider_ = false;
                ReleaseCapture();
                // Now that the drag is over, pay for the estimate and thumbnails.
                Rebuild();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            const int hit = Hit(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
            const int was = pressed_;
            pressed_ = 0;
            ReleaseCapture();
            if (hit != was) { InvalidateRect(hwnd_, nullptr, FALSE); return 0; }
            if (hit == 1 || hit == 2) { Complete(false); return 0; }
            if (hit == 3) { Complete(true); return 0; }
            if (hit == 30) {
                options_.target = app::ConvertTarget::NewFile;
            } else if (hit == 31) {
                options_.target = app::ConvertTarget::Overwrite;
            } else if (hit == 28) {
                app::SaveConvertSizePreset({ options_.width, options_.height });
                presets_ = app::LoadConvertSizePresets();
            } else if (hit >= 20 && hit < 28) {
                const int index = hit - 20;
                if (index < static_cast<int>(presets_.size())) {
                    options_.width = presets_[static_cast<size_t>(index)].width;
                    options_.height = presets_[static_cast<size_t>(index)].height;
                    SetSizeFields(options_.width, options_.height);
                }
            } else if (hit >= 50 && hit < 54) {
                const int index = hit - 50;
                if (index < static_cast<int>(std::size(kSizeFits))) {
                    options_.size_fit = kSizeFits[index];
                    Rebuild();
                }
            } else if (hit >= 70 && hit < 74) {
                const int index = hit - 70;
                if (showsQuality() && index < static_cast<int>(std::size(kQualities))) {
                    options_.quality = kQualities[index];
                    Rebuild();
                } else if (showsSpeed() && index < static_cast<int>(std::size(kSpeeds))) {
                    options_.video_speed = kSpeeds[index];
                    Rebuild();
                }
            } else if (hit >= 60 && hit < 68) {
                const auto codecs = CodecChoices();
                const int index = hit - 60;
                if (index < static_cast<int>(codecs.size())) {
                    options_.video_codec = codecs[static_cast<size_t>(index)];
                    Rebuild();
                }
            } else if (hit >= 40 && hit < 40 + kMaxVisibleGroups) {
                const int index = hit - 40;
                if (index < static_cast<int>(options_.groups.size())) {
                    selected_group_ = index;
                    // Rebuild pulls that group's box into the two fields.
                    Rebuild();
                }
            } else if (hit >= 10) {
                SelectChip(hit - 10);
            }
            Rebuild();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEWHEEL:
            scroll_ = (GET_WHEEL_DELTA_WPARAM(wparam) > 0) ? (std::max)(0, scroll_ - 3) : scroll_ + 3;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) { Complete(false); return 0; }
            if (wparam == VK_RETURN && can_apply_) { Complete(true); return 0; }
            break;
        case WM_CLOSE:
            Complete(false);
            return 0;
        case WM_PAINT:
            Paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            ReleaseComparison();
            if (font_) { DeleteObject(font_); font_ = nullptr; }
            if (edit_brush_) { DeleteObject(edit_brush_); edit_brush_ = nullptr; }
            compositor_.Shutdown();
            hwnd_ = nullptr;
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wparam, lparam);
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    // Accept keeps the previewed list; every other exit leaves an empty result,
    // so "cancel" can never leak a half-approved job into the ops queue.
    void Complete(bool accept) {
        if (accept) {
            result_.accepted = true;
            result_.options = options_;
            result_.items = items_;
        }
        done_ = true;
    }

    // Before/after thumbnails, stacked in the right column.
    void DrawComparison(const Theme& theme) {
        const auto box = SideBounds();
        painter_.FillRoundedRect(box, 8.0f * scale_, theme.fill_input);
        painter_.StrokeRoundedRect(box, 8.0f * scale_, theme.stroke_card);
        if (!cmp_before_ && !cmp_after_) return;
        const float inset = 10.0f * scale_;
        const float half = ((box.bottom - box.top) - inset * 3) / 2;
        DrawThumbnail(cmp_before_,
                      D2D1::RectF(box.left + inset, box.top + inset, box.right - inset, box.top + inset + half),
                      l10n::Pick(L"原图", L"Original"), theme);
        DrawThumbnail(cmp_after_,
                      D2D1::RectF(box.left + inset, box.top + inset * 2 + half, box.right - inset, box.bottom - inset),
                      l10n::Pick(L"效果", L"Result"), theme);
    }

    void DrawThumbnail(ID2D1Bitmap* bitmap, const D2D1_RECT_F& cell, const std::wstring& label,
                       const Theme& theme) {
        painter_.DrawText(label, D2D1::RectF(cell.left, cell.top, cell.right, cell.top + 16.0f * scale_),
                          compositor_.SmallFormat(), theme.text_secondary);
        if (!bitmap || !compositor_.Dc()) return;
        const D2D1_SIZE_F size = bitmap->GetSize();
        if (size.width <= 0 || size.height <= 0) return;
        const D2D1_RECT_F area = D2D1::RectF(cell.left, cell.top + 18.0f * scale_, cell.right, cell.bottom);
        const float aw = area.right - area.left;
        const float ah = area.bottom - area.top;
        if (aw <= 0 || ah <= 0) return;
        float w = aw, h = aw * size.height / size.width;
        if (h > ah) { h = ah; w = ah * size.width / size.height; }
        const float x = area.left + (aw - w) / 2;
        const float y = area.top + (ah - h) / 2;
        compositor_.Dc()->DrawBitmap(bitmap, D2D1::RectF(x, y, x + w, y + h), 1.0f,
                                     D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    // What the chosen scale mode does, drawn with the real source ratio against
    // the real target box: fit leaves borders, fill crops, stretch distorts.
    // (For video the first frame's size is unknown, so the box is shown square.)
    void DrawScaleDiagram(const Theme& theme) {
        const auto box = DiagramBounds();
        painter_.FillRoundedRect(box, 8.0f * scale_, theme.fill_input);
        painter_.StrokeRoundedRect(box, 8.0f * scale_, theme.stroke_card);
        const float inset = 16.0f * scale_;
        const float availW = (box.right - box.left) - inset * 2;
        const float availH = (box.bottom - box.top) - inset * 2;
        if (availW <= 0 || availH <= 0) return;

        const float targetAspect = (options_.width > 0 && options_.height > 0)
            ? static_cast<float>(options_.width) / static_cast<float>(options_.height) : 1.0f;
        float fw = availW, fh = availW / targetAspect;
        if (fh > availH) { fh = availH; fw = availH * targetAspect; }
        const float fx = box.left + inset + (availW - fw) / 2;
        const float fy = box.top + inset + (availH - fh) / 2;
        const D2D1_RECT_F frame = D2D1::RectF(fx, fy, fx + fw, fy + fh);
        painter_.FillRoundedRect(frame, 2.0f * scale_, theme.fill_input);
        painter_.StrokeRoundedRect(frame, 2.0f * scale_, theme.stroke_divider);

        const float srcAspect = (src_w_ > 0 && src_h_ > 0)
            ? static_cast<float>(src_w_) / static_cast<float>(src_h_) : targetAspect;
        float iw = fw, ih = fw / srcAspect;
        if (options_.scale_mode == ScaleMode::ByFactor) {
            // A factor has no box to meet, so show it the same way as 等比缩放.
            if (ih > fh) { ih = fh; iw = fh * srcAspect; }
        } else {
            switch (options_.size_fit) {
            case app::SizeFit::Stretch:
                ih = fh;
                break;
            case app::SizeFit::Crop:
                if (ih < fh) { ih = fh; iw = fh * srcAspect; }
                break;
            case app::SizeFit::NoPad:
            case app::SizeFit::Pad:
            default:
                if (ih > fh) { ih = fh; iw = fh * srcAspect; }
                break;
            }
        }
        const float ix = frame.left + (fw - iw) / 2;
        const float iy = frame.top + (fh - ih) / 2;
        D2D1_RECT_F image = D2D1::RectF(ix, iy, ix + iw, iy + ih);
        if (options_.scale_mode == ScaleMode::BySize && options_.size_fit == app::SizeFit::Crop) {
            image.left = (std::max)(image.left, frame.left);
            image.top = (std::max)(image.top, frame.top);
            image.right = (std::min)(image.right, frame.right);
            image.bottom = (std::min)(image.bottom, frame.bottom);
        }
        painter_.FillRoundedRect(image, 2.0f * scale_, theme.accent);
    }

    // ---- painting ---------------------------------------------------------
    void Paint() {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd_, &ps);
        const bool high_contrast = IsHighContrast();
        const Theme theme = high_contrast ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
        luling::ui::BeginSurface(compositor_, painter_, theme, dark_, high_contrast, backdrop_enabled_, scale_);

        painter_.DrawText(ModeTitle(options_.mode), Rect(scale_, 16, 0, 360, 36),
                          compositor_.SmallFormat(), theme.text);
        fluent::ControlState close_state{};
        close_state.hovered = hover_ == 1;
        close_state.pressed = pressed_ == 1;
        painter_.DrawTitleBarButton(CloseRect(), fluent::TitleBarButtonRole::Close, {}, close_state);
        painter_.FillRoundedRect(Rect(scale_, 0, 36, kDlgW, 1), 0, theme.stroke_divider);

        // Row 1: the mode's main choice — formats, scale modes or tiers.
        for (int i = 0; i < ChipCount(); ++i) {
            std::wstring label;
            if (options_.mode == ConvertMode::Format) label = L"." + formats_[static_cast<size_t>(i)];
            else if (options_.mode == ConvertMode::Resize) label = app::ScaleModeName(kScaleModes[i]);
            else label = app::CompressTierLabel(static_cast<CompressTier>(i), compressionExtension());
            fluent::ControlState state{};
            state.hovered = hover_ == 10 + i;
            state.pressed = pressed_ == 10 + i;
            state.checked = ChipSelected(i);
            painter_.DrawButton({ ChipRect(i), label, {},
                                  state.checked ? fluent::ButtonKind::Primary : fluent::ButtonKind::Standard,
                                  state });
        }

        if (showsSlider()) DrawSlider(theme);
        if (options_.mode == ConvertMode::Compress && !compressionHasQuality() && !items_.empty()) {
            // Say why the tiers are missing rather than leaving a blank row.
            painter_.DrawText(l10n::Pick(L"无损格式：按最优参数重编码，质量档对它不起作用",
                                         L"Lossless format: re-encoded at maximum effort — a quality setting would do nothing"),
                              Rect(scale_, 20, 80, kDlgW - 40, 20),
                              compositor_.SmallFormat(), theme.text_secondary);
        }

        if (resizing()) {
            // Row 2: how the picture meets the box.
            for (int i = 0; i < FitCount(); ++i) {
                const bool selected = options_.size_fit == kSizeFits[i];
                fluent::ControlState state{};
                state.hovered = hover_ == 50 + i;
                state.pressed = pressed_ == 50 + i;
                state.checked = selected;
                painter_.DrawButton({ FitRect(i), app::SizeFitName(kSizeFits[i]), {},
                                      selected ? fluent::ButtonKind::Primary : fluent::ButtonKind::Standard,
                                      state });
            }
            // Resize gets an extra block: what the mode does, the two numbers, and
            // the saved sizes. 按倍率 and 按增量 are both a single number, so they
            // share the label layout and differ only in wording.
            const bool single_value = options_.scale_mode != ScaleMode::BySize;
            painter_.DrawText(single_value ? app::ScaleModeHint(options_.scale_mode)
                                           : app::SizeFitHint(options_.size_fit),
                              Rect(scale_, 20, 92, kDlgW - 40, 24),
                              compositor_.SmallFormat(), theme.text_secondary);
            if (options_.scale_mode == ScaleMode::ByFactor) {
                painter_.DrawText(l10n::Pick(L"倍率", L"Factor"),
                                  Rect(scale_, 20, 157, 52, 20), compositor_.SmallFormat(), theme.text_secondary);
                // On its own line: the row above already carries the preset button.
                painter_.DrawText(l10n::Pick(L"1.0 保持原样 · 1.2 放大两成 · 0.5 减半",
                                             L"1.0 leaves it alone · 1.2 enlarges 20% · 0.5 halves"),
                                  Rect(scale_, 20, 180, kDlgW - 40, 20),
                                  compositor_.SmallFormat(), theme.text_secondary);
            } else if (options_.scale_mode == ScaleMode::ByDelta) {
                painter_.DrawText(l10n::Pick(L"增量", L"Delta"),
                                  Rect(scale_, 20, 157, 52, 20), compositor_.SmallFormat(), theme.text_secondary);
                // On its own line: the row above already carries the preset button.
                painter_.DrawText(l10n::Pick(L"每张按自己的原尺寸加这么多：正数放大、负数缩小，宽高都加",
                                             L"Added to each picture's own size on both sides — positive grows, negative shrinks"),
                                  Rect(scale_, 20, 180, kDlgW - 40, 20),
                                  compositor_.SmallFormat(), theme.text_secondary);
            } else {
                painter_.DrawText(l10n::Pick(L"宽度", L"Width"),
                                  Rect(scale_, 20, 157, 52, 20), compositor_.SmallFormat(), theme.text_secondary);
                painter_.DrawText(l10n::Pick(L"高度", L"Height"),
                                  Rect(scale_, 206, 157, 52, 20), compositor_.SmallFormat(), theme.text_secondary);
            }
            fluent::ControlState save_state{};
            save_state.hovered = hover_ == 28;
            save_state.pressed = pressed_ == 28;
            painter_.DrawButton({ SavePresetRect(), l10n::Pick(L"保存为预设", L"Save as preset"), {},
                                  fluent::ButtonKind::Standard, save_state });
            for (int i = 0; i < VisiblePresetCount(); ++i) {
                const auto& preset = presets_[static_cast<size_t>(i)];
                const std::wstring label = std::to_wstring(preset.width) + L" × " + std::to_wstring(preset.height);
                fluent::ControlState state{};
                state.hovered = hover_ == 20 + i;
                state.pressed = pressed_ == 20 + i;
                state.checked = options_.width == preset.width && options_.height == preset.height;
                painter_.DrawButton({ PresetRect(i), label, {},
                                      state.checked ? fluent::ButtonKind::Primary : fluent::ButtonKind::Standard,
                                      state });
            }
        }

        // Row 2 for video format conversion: the codec, independent of container.
        const auto codecs = CodecChoices();
        if (!codecs.empty()) {
            painter_.DrawText(l10n::Pick(L"编码", L"Codec"),
                              Rect(scale_, 20, 90, 120, 16), compositor_.SmallFormat(), theme.text_secondary);
            for (size_t i = 0; i < codecs.size(); ++i) {
                fluent::ControlState state{};
                state.hovered = hover_ == 60 + static_cast<int>(i);
                state.pressed = pressed_ == 60 + static_cast<int>(i);
                state.checked = codecs[i] == options_.video_codec;
                painter_.DrawButton({ PresetRect(static_cast<int>(i)), app::VideoCodecName(codecs[i]), {},
                                      state.checked ? fluent::ButtonKind::Primary : fluent::ButtonKind::Standard,
                                      state });
            }
        }

        // Row 3: quality for images, encoder speed for video.
        if (showsQuality()) {
            painter_.DrawText(l10n::Pick(L"质量", L"Quality"),
                              Rect(scale_, 20, 138, 120, 16), compositor_.SmallFormat(), theme.text_secondary);
            for (size_t i = 0; i < std::size(kQualities); ++i) {
                fluent::ControlState state{};
                state.hovered = hover_ == 70 + static_cast<int>(i);
                state.pressed = pressed_ == 70 + static_cast<int>(i);
                state.checked = options_.quality == kQualities[i];
                painter_.DrawButton({ QualityRect(static_cast<int>(i)),
                                      std::to_wstring(kQualities[i]) + L"%", {},
                                      state.checked ? fluent::ButtonKind::Primary : fluent::ButtonKind::Standard,
                                      state });
            }
        } else if (showsSpeed()) {
            painter_.DrawText(l10n::Pick(L"速度", L"Speed"),
                              Rect(scale_, 20, 138, 120, 16), compositor_.SmallFormat(), theme.text_secondary);
            for (size_t i = 0; i < std::size(kSpeeds); ++i) {
                fluent::ControlState state{};
                state.hovered = hover_ == 70 + static_cast<int>(i);
                state.pressed = pressed_ == 70 + static_cast<int>(i);
                state.checked = options_.video_speed == kSpeeds[i];
                painter_.DrawButton({ QualityRect(static_cast<int>(i)), app::VideoSpeedName(kSpeeds[i]), {},
                                      state.checked ? fluent::ButtonKind::Primary : fluent::ButtonKind::Standard,
                                      state });
            }
        }

        painter_.DrawText(l10n::Pick(L"原文件", L"Original file"),
                          Rect(scale_, 20, 264, 200, 18), compositor_.SmallFormat(), theme.text_secondary);
        for (int which = 0; which < 2; ++which) {
            fluent::ControlState state{};
            state.hovered = hover_ == 20 + which;
            state.pressed = pressed_ == 20 + which;
            state.checked = (which == 0) == (options_.target == app::ConvertTarget::NewFile);
            painter_.DrawButton({ which == 0 ? KeepRect() : OverwriteRect(),
                                  which == 0 ? l10n::Pick(L"保留原文件", L"Keep the original")
                                             : l10n::Pick(L"覆盖原文件", L"Replace the original"),
                                  {},
                                  state.checked ? fluent::ButtonKind::Primary : fluent::ButtonKind::Standard,
                                  state });
        }

        // A format change cannot replace the original — the result would carry a
        // different extension, so there would be two files. Say so next to the
        // choice instead of quietly writing a second file and calling it done.
        if (options_.mode == ConvertMode::Format && options_.target == app::ConvertTarget::Overwrite)
            painter_.DrawText(l10n::Pick(L"转格式无法覆盖原文件（缩放、压缩可以）",
                                         L"A format change cannot replace the original (resize and compress can)"),
                              Rect(scale_, 322, 288, kDlgW - 342, 20),
                              compositor_.SmallFormat(), theme.text_secondary);

        const auto preview = ListBounds();
        painter_.FillRoundedRect(preview, 8.0f * scale_, theme.fill_input);
        painter_.StrokeRoundedRect(preview, 8.0f * scale_, theme.stroke_card);
        if (resizing()) {
            DrawScaleDiagram(theme);
        } else {
            DrawComparison(theme);
        }
        if (resizing()) {
            // Resize lists size groups instead of files: pick a row, then set its
            // box with the fields above. A group of one is "just this file".
            const auto groups = VisibleGroups();
            const int line_h = static_cast<int>(26.0f * scale_);
            for (size_t i = 0; i < groups.size(); ++i) {
                const auto& group = *groups[i];
                const bool selected = static_cast<int>(i) == selected_group_;
                const auto row = GroupRowRect(static_cast<int>(i));
                if (selected)
                    painter_.FillRoundedRect(row, 4.0f * scale_, theme.accent);
                else if (hover_ == 40 + static_cast<int>(i))
                    painter_.FillRoundedRect(row, 4.0f * scale_, theme.stroke_card);
                const D2D1_COLOR_F text = selected ? theme.accent_text : theme.text;
                const D2D1_COLOR_F muted = selected ? theme.accent_text : theme.text_secondary;
                painter_.DrawText(std::to_wstring(group.source_width) + L" × " + std::to_wstring(group.source_height),
                                  D2D1::RectF(row.left + 10 * scale_, row.top, row.left + 158 * scale_, row.top + line_h),
                                  compositor_.SmallFormat(), text);
                wchar_t count[32]{};
                swprintf_s(count, l10n::Pick(L"%d 项", L"%d files"), GroupItemCount(group));
                painter_.DrawText(count,
                                  D2D1::RectF(row.left + 158 * scale_, row.top, row.left + 230 * scale_, row.top + line_h),
                                  compositor_.SmallFormat(), muted);
                painter_.DrawText(L"→",
                                  D2D1::RectF(row.left + 236 * scale_, row.top, row.left + 258 * scale_, row.top + line_h),
                                  compositor_.SmallFormat(), muted);
                painter_.DrawText(std::to_wstring(group.target_width) + L" × " + std::to_wstring(group.target_height),
                                  D2D1::RectF(row.left + 262 * scale_, row.top, row.right - 10 * scale_, row.top + line_h),
                                  compositor_.SmallFormat(), text);
            }
        }
        // The resize list holds groups, drawn above; the file rows are what the
        // other two modes show.
        if (!resizing()) {
            float y = preview.top + 8 * scale_;
            const float line = 22 * scale_;
            const float left = preview.left + 12 * scale_;
            for (size_t i = static_cast<size_t>(scroll_); i < items_.size(); ++i) {
                if (y + line > preview.bottom - 4 * scale_) break;
                const auto& item = items_[i];
                const bool ok = item.status == ConvertStatus::Ok;
                // Columns by proportion, so they hold up at any list width:
                // name | current size | → | result name | result size
                const float w = preview.right - preview.left;
                const float name_right = preview.left + w * 0.34f;
                const float size_left = preview.left + w * 0.36f;
                const float size_right = preview.left + w * 0.50f;
                const float arrow_left = preview.left + w * 0.52f;
                const float out_left = preview.left + w * 0.57f;
                const float est_left = preview.left + w * 0.83f;
                const float est_right = preview.right - 6 * scale_;
                painter_.DrawText(item.source_name, D2D1::RectF(left, y, name_right, y + line),
                                  compositor_.SmallFormat(), ok ? theme.text : theme.text_secondary);
                painter_.DrawText(FormatSize(item.source_size),
                                  D2D1::RectF(size_left, y, size_right, y + line),
                                  compositor_.SmallFormat(), theme.text_secondary);
                painter_.DrawText(L"→", D2D1::RectF(arrow_left, y, arrow_left + 14 * scale_, y + line),
                                  compositor_.SmallFormat(), theme.text_secondary);
                const std::wstring outcome = ok ? item.target_name : StatusText(item.status);
                const uint64_t estimate = EstimatedSizeFor(item);
                painter_.DrawText(outcome,
                                  D2D1::RectF(out_left, y, estimate > 0 ? est_left - 4 * scale_ : est_right, y + line),
                                  compositor_.SmallFormat(), ok ? theme.text : HexColor(0xC42B1C));
                // The compressed size rides along on every row, coloured so it
                // reads as a prediction rather than the file's current size.
                if (estimate > 0)
                    painter_.DrawText(FormatSize(estimate), D2D1::RectF(est_left, y, est_right, y + line),
                                      compositor_.SmallFormat(), theme.accent);
                y += line;
            }
        }

        if (!engine_ready())
            painter_.DrawText(l10n::Pick(L"转换需要在设置中安装转换引擎（预览增强包 › 转换引擎）",
                                         L"Conversion needs the engine from Settings › Preview packs"),
                              SummaryRect(), compositor_.SmallFormat(), HexColor(0xC42B1C));
        else {
            // A selection of mixed sizes is normal; what matters is that the user
            // can see it and knows each file keeps its own ratio (fit/fill) rather
            // than being squashed into the box.
            bool mixed_sizes = false;
            if (resizing() && !items_.empty() && items_.front().source_width > 0) {
                for (const auto& item : items_) {
                    if (item.source_width > 0 &&
                        (item.source_width != items_.front().source_width ||
                         item.source_height != items_.front().source_height)) {
                        mixed_sizes = true;
                        break;
                    }
                }
            }
            wchar_t summary[192]{};
            if (options_.mode == ConvertMode::Compress) {
                // Totals across the whole selection: with a per-row estimate there
                // is no reason to report only the first file any more.
                uint64_t before = 0, after = 0;
                for (const auto& item : items_) {
                    if (item.status != ConvertStatus::Ok) continue;
                    before += item.source_size;
                    after += EstimatedSizeFor(item);
                }
                swprintf_s(summary, l10n::Pick(L"压缩 %zu 项 · %s → 约 %s",
                                               L"Compress %zu · %s → about %s"),
                           ok_count_, FormatSize(before).c_str(), FormatSize(after).c_str());
            } else if (mixed_sizes) {
                swprintf_s(summary, l10n::Pick(L"将转换 %zu 项；素材尺寸不一，各自按原比例缩放",
                                               L"Converts %zu; sizes differ, each keeps its own ratio"),
                           ok_count_);
            } else {
                swprintf_s(summary, l10n::Pick(L"将转换 %zu 项，跳过 %zu 项",
                                               L"Converts %zu, skips %zu"),
                           ok_count_, items_.size() - ok_count_);
            }
            painter_.DrawText(summary, SummaryRect(), compositor_.SmallFormat(), theme.text_secondary);
        }

        fluent::ControlState cancel_state{};
        cancel_state.hovered = hover_ == 2;
        cancel_state.pressed = pressed_ == 2;
        painter_.DrawButton({ CancelRect(), l10n::Get(l10n::StringId::Cancel), {},
                              fluent::ButtonKind::Standard, cancel_state });
        fluent::ControlState apply_state{};
        apply_state.hovered = hover_ == 3;
        apply_state.pressed = pressed_ == 3;
        painter_.DrawButton({ ApplyRect(), l10n::Pick(L"开始", L"Start"), {},
                              can_apply_ ? fluent::ButtonKind::Primary : fluent::ButtonKind::Standard,
                              apply_state });

        // BeginSurface opens the D2D surface; without EndSurface nothing is
        // submitted and the window shows the empty backdrop.
        luling::ui::EndSurface(compositor_);
        // The layered child edits upload their own bitmaps, so they have to be
        // re-presented after every frame — presenting them only at WM_CREATE
        // left the size boxes blank until the user clicked into them.
        LayoutEdits();
        EndPaint(hwnd_, &ps);
    }

    Compositor compositor_;
    fluent::Painter painter_;
    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    HWND edit_width_ = nullptr;
    HWND edit_height_ = nullptr;
    HWND edit_factor_ = nullptr;
    HWND edit_delta_ = nullptr;
    HFONT font_ = nullptr;
    HBRUSH edit_brush_ = nullptr;
    std::vector<std::wstring> paths_;
    std::vector<std::wstring> formats_;
    std::vector<app::ConvertSizePreset> presets_;
    std::vector<app::ConvertItem> items_;
    app::ConvertOptions options_;
    ConvertKind kind_ = ConvertKind::Other;
    int src_w_ = 0, src_h_ = 0;   // first file's pixel size, for the diagram
    ConvertDialogResult result_;
    bool dark_ = false;
    bool backdrop_enabled_ = false;
    bool done_ = false;
    bool can_apply_ = false;
    float scale_ = 1.0f;
    D2D1_COLOR_F accent_ = HexColor(0x0078D4);
    int hover_ = 0;
    int pressed_ = 0;
    int scroll_ = 0;
    int selected_group_ = 0;   // which size group the fields are editing
    bool syncing_ = false;     // guard while writing the edit fields
    bool dragging_slider_ = false;
    ID2D1Bitmap* cmp_before_ = nullptr;   // before/after preview thumbnails
    ID2D1Bitmap* cmp_after_ = nullptr;
    std::wstring comparison_key_;
    size_t ok_count_ = 0;
};

} // namespace

ConvertDialogResult ShowConvertDialog(HWND owner, const std::vector<std::wstring>& paths, bool dark,
                                      D2D1_COLOR_F accent, app::ConvertMode mode,
                                      const app::ConvertOptions* initial) {
    ConvertWindow window;
    return window.Show(owner, paths, dark, accent, mode, initial);
}

} // namespace luling::ui
