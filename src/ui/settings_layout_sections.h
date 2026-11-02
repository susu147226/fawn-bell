// Included inside ui_renderer_internal.h's anonymous namespace.
// Default file manager card helpers shared by layout, drawing and tests.
struct IntegrationBadgeInfo { std::wstring text; fluent::BadgeKind kind = fluent::BadgeKind::Neutral; };
inline int IntegrationSelectedCount(const WindowViewModel& vm) {
    return int(vm.settings_integration_folders)+int(vm.settings_integration_this_pc)+
        int(vm.settings_integration_win_e)+int(vm.settings_integration_experimental);
}
inline IntegrationBadgeInfo MakeIntegrationBadge(const WindowViewModel& vm) {
    using I=l10n::StringId;
    if(vm.settings_integration_state==3) return {l10n::Get(I::IntegrationFailed),fluent::BadgeKind::Danger};
    if(vm.settings_integration_state==2) return {l10n::Get(I::IntegrationPartial),fluent::BadgeKind::Warning};
    if(!vm.settings_integration_enabled) return {l10n::Get(I::IntegrationOff),fluent::BadgeKind::Neutral};
    const int count=IntegrationSelectedCount(vm);
    if(count==0) return {l10n::Get(I::IntegrationNone),fluent::BadgeKind::Warning};
    wchar_t value[128]{};
    swprintf_s(value,l10n::Get(I::IntegrationOnCount).c_str(),count);
    return {value,fluent::BadgeKind::Success};
}
// Problems get an InfoBar with their own actions; healthy states need none.
inline bool IntegrationHasProblem(const WindowViewModel& vm) {
    return vm.settings_integration_state==2 || vm.settings_integration_state==3;
}
inline std::wstring IntegrationProblemMessage(const WindowViewModel& vm) {
    if(vm.settings_integration_state==3)
        return vm.settings_integration_summary.empty() ? l10n::Get(l10n::StringId::IntegrationFailed) : vm.settings_integration_summary;
    return vm.settings_integration_state==2 ? l10n::Get(l10n::StringId::IntegrationDriftDesc) : std::wstring{};
}
// One short next step, naming only the settings that are actually missing.
inline std::wstring IntegrationHint(const WindowViewModel& vm) {
    using I=l10n::StringId;
    if(!vm.settings_integration_enabled) return l10n::Get(I::IntegrationInactiveHint);
    if(IntegrationSelectedCount(vm)==0) return l10n::Get(I::IntegrationNoneHint);
    if(!vm.settings_integration_experimental) return {};
    if(!vm.settings_launch_on_startup && !vm.settings_keep_running) return l10n::Get(I::IntegrationRunningHint);
    if(!vm.settings_launch_on_startup) return l10n::Get(I::IntegrationHintLaunch);
    if(!vm.settings_keep_running) return l10n::Get(I::IntegrationHintKeep);
    return {};
}
inline constexpr l10n::StringId kIntegrationChips[]={l10n::StringId::IntegrationExperimentalTag,
    l10n::StringId::IntegrationFactRunning,l10n::StringId::IntegrationFactShift,l10n::StringId::IntegrationFactWin11};


float LayoutSettingsContent(SettingsLayout& l, const WindowViewModel& vm, float scale,
                           float y, const fluent::Painter* painter) {
    const float left=l.content.left+20*scale,right=l.content.right-20*scale;
    const bool narrow=right-left<560*scale;
    auto row=[&](float h) { auto r=D2D1::RectF(left,y,right,y+h*scale);y=r.bottom;return r; };
    y+=14*scale; l.section[1]=row(28);
    l.content_header=row(72);
    l.content_types=row(narrow ? 236.0f : 160.0f);
    if(vm.settings_content_folders.empty()) l.content_empty=row(84);
    else if (!vm.settings_content_instant) {
        auto r=row(52);
        const float bw=painter ? painter->MeasureButtonWidth(l10n::Get(vm.settings_content_paused ? l10n::StringId::ContentIndexResume : l10n::StringId::ContentIndexPause)) : 150*scale;
        l.content_pause=D2D1::RectF(left+16*scale,r.top+10*scale,left+16*scale+bw,r.bottom-10*scale);
    }
    l.content_options=row(64);
    if(!vm.settings_content_instant && !vm.settings_content_folders.empty()) l.content_rebuild=row(64);
    l.group[1]=D2D1::RectF(left,l.content_header.top,right,y);
    y+=8*scale; l.footer=row(40);
    y+=24*scale;
    return y;
}

// ---- 预览增强包 page (5) ---------------------------------------------------
// Texts shared by layout (measuring) and drawing. l10n::Pick converts to
// Traditional Chinese at run time, like the Quick Look format catalog.
namespace pack_text {
inline const wchar_t* Title() { return l10n::Pick(L"快速预览", L"Quick Look"); }
inline const wchar_t* Intro() { return l10n::Pick(
    L"按空格键预览文件，在这里查看格式支持并管理扩展。",
    L"Press Space to preview files. Explore format support and manage extensions here."); }
inline const wchar_t* PacksSection() { return l10n::Pick(L"预览增强包", L"Preview packs"); }
inline const wchar_t* FormatsSection() { return l10n::Pick(L"格式", L"Formats"); }
inline std::wstring FormatsSummary(const WindowViewModel& vm) {
    const int packs = int(vm.settings_pack_media_installed) + int(vm.settings_pack_images_installed) +
                      int(vm.settings_pack_raw_installed) + int(vm.settings_pack_archive_installed);
    int codecs = 0;
    for (int i = 0; i < kPreviewCodecCount; ++i) if (vm.settings_preview_codecs & (1u << i)) ++codecs;
    const auto count = std::to_wstring(PreviewFormatCount());
    std::wstring result = l10n::IsChinese()
        ? l10n::Cn(L"共 ") + count + l10n::Cn(L" 种格式 · 增强包 ") + std::to_wstring(packs) + L"/4"
        : count + L" formats · Preview packs " + std::to_wstring(packs) + L"/4";
    if (vm.settings_preview_codecs & kPreviewCodecsDetected)
        result += std::wstring(l10n::Pick(L" · 系统扩展 ", L" · System extensions ")) + std::to_wstring(codecs) + L"/4";
    else result += l10n::Pick(L" · 系统扩展检测中", L" · Checking system extensions");
    return result;
}
inline const wchar_t* FormatsTitle() { return l10n::Pick(L"支持的格式", L"Supported formats"); }
inline const wchar_t* SystemTitle() { return l10n::Pick(L"系统扩展", L"System extensions"); }
inline const wchar_t* Legend(int source) {
    if (source == 0) return l10n::Pick(L"内置", L"Built in");
    if (source == 1) return l10n::Pick(L"预览增强包", L"Preview pack");
    return l10n::Pick(L"增强包或系统扩展", L"Pack or system extension");
}
inline const wchar_t* LegendNote() { return l10n::Pick(
    L"虚线：对应的增强支持尚未启用",
    L"Dashed: enhanced support is not enabled"); }
inline const wchar_t* SystemHint(const WindowViewModel& vm) { return vm.settings_pack_images_installed && vm.settings_pack_images_enabled
    ? l10n::Pick(L"已启用现代图像格式增强包：HEIC / AVIF 可以由增强包解码，无需另装对应的系统扩展。",
                 L"The image pack is enabled: it decodes HEIC / AVIF without the corresponding system extensions.")
    : l10n::Pick(L"HEIC / AVIF 也可通过现代图像格式增强包预览，无需微软账户。",
                 L"The image pack also supports HEIC / AVIF without a Microsoft account."); }

inline const wchar_t* TriggerSection() { return l10n::Pick(L"预览方式", L"Preview trigger"); }
inline const wchar_t* TriggerTitle() { return l10n::Pick(L"打开预览", L"Open previews"); }
inline const wchar_t* TriggerDesc(int trigger) {
    return trigger == 1
        ? l10n::Pick(L"选中文件后按空格键预览", L"Select a file and press Space")
        : l10n::Pick(L"指针停在文件上时显示预览卡片，空格键仍可展开完整预览",
                     L"Rest the pointer on a file for a preview card; Space still opens the full preview");
}
inline const wchar_t* TriggerChoice(int i) {
    return i == 0 ? l10n::Pick(L"悬停预览", L"Hover") : l10n::Pick(L"空格预览", L"Space");
}
inline const wchar_t* DelayTitle() { return l10n::Pick(L"悬停延迟", L"Hover delay"); }
inline const wchar_t* DelayDesc() { return l10n::Pick(L"指针停留多久后显示预览卡片",
                                                      L"How long the pointer rests before the card appears"); }
inline const wchar_t* DelayChoice(int i) {
    if (i == 0) return l10n::Pick(L"0.3 秒", L"0.3 s");
    if (i == 1) return l10n::Pick(L"0.5 秒", L"0.5 s");
    return l10n::Pick(L"0.8 秒", L"0.8 s");
}
inline const wchar_t* SizeTitle() { return l10n::Pick(L"卡片尺寸", L"Card size"); }
inline const wchar_t* SizeDesc() {
    return l10n::Pick(L"卡片随内容变形；按住 Ctrl 临时放大",
                      L"Follows the content's shape; hold Ctrl to enlarge");
}
inline const wchar_t* SizeChoice(int i) {
    if (i == 0) return l10n::Pick(L"小", L"Small");
    if (i == 1) return l10n::Pick(L"中", L"Medium");
    return l10n::Pick(L"大", L"Large");
}

inline const wchar_t* OpenFolder() { return l10n::Pick(L"打开文件夹", L"Open folder"); }
inline const wchar_t* Media() { return l10n::Pick(L"媒体", L"Media"); }
inline const wchar_t* Advanced() { return l10n::Pick(L"高级", L"Advanced"); }
inline const wchar_t* MediaTitle() { return l10n::Pick(L"FFmpeg 媒体增强", L"FFmpeg media"); }
inline const wchar_t* MediaDesc() { return l10n::Pick(
    L"系统缺少解码器的视频也能生成缩略图、显示时长和媒体信息：HEVC、AV1、VP9、ProRes、FLV、RMVB、MPEG-TS 等。",
    L"Thumbnails, playing time and media details for videos Windows has no decoder for: HEVC, AV1, VP9, ProRes, FLV, RMVB, MPEG-TS and more."); }
inline const wchar_t* MediaMeta() { return l10n::Pick(L"LGPL-2.1 · 仅解码构建，不含 GPL 组件", L"LGPL-2.1 · decode-only build, no GPL parts"); }
inline const wchar_t* Images() { return l10n::Pick(L"图像", L"Images"); }
inline const wchar_t* ImagesTitle() { return l10n::Pick(L"现代图像格式", L"Modern image formats"); }
inline const wchar_t* ImagesDesc() { return l10n::Pick(
    L"不依赖微软商店扩展，直接预览 iPhone 照片（HEIC）、AVIF、JPEG XL 和 HDR / EXR 图像，文件夹里也能看到缩略图。",
    L"Preview iPhone photos (HEIC), AVIF, JPEG XL and HDR / EXR pictures without the Microsoft Store extensions, thumbnails included."); }
inline const wchar_t* ImagesMeta() { return l10n::Pick(L"libheif · dav1d · libjxl · OpenEXR · LGPL-3.0 / BSD", L"libheif · dav1d · libjxl · OpenEXR · LGPL-3.0 / BSD"); }
inline const wchar_t* RawTitle() { return l10n::Pick(L"RAW 相机照片", L"RAW camera photos"); }
inline const wchar_t* RawDesc() { return l10n::Pick(
    L"通过 LibRaw 为 CR2、CR3、NEF、ARW、DNG、RAF 等相机原片提供预览和缩略图。",
    L"Use LibRaw for previews and thumbnails of camera originals such as CR2, CR3, NEF, ARW, DNG and RAF."); }
inline const wchar_t* RawMeta() { return L"LibRaw · LGPL-2.1 / CDDL"; }
inline const wchar_t* Archive() { return l10n::Pick(L"压缩包", L"Archives"); }
inline const wchar_t* ArchiveTitle() { return l10n::Pick(L"7-Zip 压缩包增强", L"7-Zip archives"); }
inline const wchar_t* ArchiveDesc() { return l10n::Pick(
    L"通过 7-Zip 扩展压缩包内容列表预览，支持 7z、RAR、ISO、CAB、WIM 等格式。",
    L"Use 7-Zip to preview archive contents, including 7z, RAR, ISO, CAB and WIM."); }
inline const wchar_t* ArchiveMeta() { return L"7-Zip · LGPL-2.1 + unRAR"; }
inline const wchar_t* TranscodeSection() { return l10n::Pick(L"转换", L"Conversion"); }
inline const wchar_t* TranscodeTitle() { return l10n::Pick(L"转换引擎", L"Conversion engine"); }
inline const wchar_t* TranscodeDesc() { return l10n::Pick(
    L"右键「转换」里的格式转换、调整尺寸与压缩需要它：带编码器的 FFmpeg，可输出 H.264 / H.265 / AV1 / VP9 与 JPEG / WebP。",
    L"Powers the 转换 submenu — format conversion, resizing and compression. An FFmpeg with encoders for H.264 / H.265 / AV1 / VP9 and JPEG / WebP."); }
inline const wchar_t* TranscodeMeta() { return l10n::Pick(L"FFmpeg · GPL-2.0+ · 含 libx264 / libx265", L"FFmpeg · GPL-2.0+ · includes libx264 / libx265"); }
inline const wchar_t* Install() { return l10n::Pick(L"下载并安装", L"Download and install"); }
inline const wchar_t* Remove() { return l10n::Pick(L"卸载", L"Remove"); }
inline const wchar_t* CustomTitle() { return l10n::Pick(L"使用已有 FFmpeg", L"Use existing FFmpeg"); }
inline const wchar_t* CustomDesc() { return l10n::Pick(
    L"可选来源；关闭后使用 鹿铃 管理的增强包",
    L"Optional source; turn off to use the Luling-managed pack"); }
inline const wchar_t* NoCustom() { return l10n::Pick(L"尚未选择 ffmpeg.exe", L"No ffmpeg.exe chosen"); }
inline const wchar_t* Browse() { return l10n::Pick(L"浏览…", L"Browse…"); }
inline const wchar_t* UseDetected() { return l10n::Pick(L"使用检测到的", L"Use found"); }
inline const wchar_t* RemoveTitle() { return l10n::Pick(L"卸载 鹿铃 时删除预览增强包", L"Remove packs when uninstalling Luling"); }
inline const wchar_t* RemoveDesc() { return l10n::Pick(L"否则保留，重新安装 鹿铃 后可直接使用", L"Otherwise they stay and work again after reinstalling Luling"); }
inline const wchar_t* AutoUpdateTitle() { return l10n::Pick(L"自动更新预览增强包", L"Update preview packs automatically"); }
inline const wchar_t* AutoUpdateDesc() { return l10n::Pick(L"随 鹿铃 更新检查一起进行，在后台下载", L"Runs with the Luling update check and downloads in the background"); }
inline const wchar_t* Note() { return l10n::Pick(
    L"预览增强包在隔离的预览进程中运行：崩溃或超时只影响当前预览，鹿铃 会回退到内置的缩略图和文本视图。",
    L"Packs run in the isolated preview process: a crash or timeout only affects that preview, and Luling falls back to its built-in views."); }
inline std::wstring Bytes(uint64_t bytes) {
    wchar_t text[48]{};
    if (bytes >= 1024ull * 1024 * 1024) swprintf_s(text, L"%.1f GB", bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024ull * 1024) swprintf_s(text, L"%.0f MB", bytes / (1024.0 * 1024));
    else swprintf_s(text, L"%.0f KB", bytes / 1024.0);
    return text;
}
inline std::wstring Summary(const WindowViewModel& vm) {
    if (!vm.settings_pack_installed) return l10n::Pick(L"尚未安装预览增强包", L"No preview packs installed");
    wchar_t text[128]{};
    swprintf_s(text, l10n::Pick(L"已安装 %u 个 · 占用 %s", L"%u installed · %s"),
               vm.settings_pack_installed, Bytes(vm.settings_pack_bytes).c_str());
    return text;
}
inline std::wstring Found(const std::wstring& path) {
    return std::wstring(l10n::Pick(L"检测到：", L"Found: ")) + path;
}
struct Badge { std::wstring text; fluent::BadgeKind kind = fluent::BadgeKind::Neutral; };
inline const wchar_t* PrimaryLabel(bool available, bool installed, bool installing) {
    if (installing) return l10n::Pick(L"取消", L"Cancel");
    if (installed) return Remove();
    return available ? Install() : l10n::Pick(L"尚未发布", L"Not published");
}
inline bool PrimaryEnabled(bool available, bool installed, bool installing) {
    return available || installed || installing;
}
// "更新到 1.0.2" beside 卸载 while an installed pack is older than the
// catalog; empty (no button) while it downloads or when it is current.
inline std::wstring UpdateLabel(bool installed, bool installing, const std::wstring& version) {
    if (!installed || installing || version.empty()) return {};
    return std::wstring(l10n::Pick(L"更新到 ", L"Update to ")) + version;
}
inline Badge PackBadge(bool available, bool installed, bool installing, bool enabled, const std::wstring& version) {
    if (installing) return {installed ? l10n::Pick(L"正在更新", L"Updating") : l10n::Pick(L"正在安装", L"Installing"),
                            fluent::BadgeKind::Accent};
    if (!installed) return {l10n::Pick(available ? L"未安装" : L"尚未发布", available ? L"Not installed" : L"Not published"), fluent::BadgeKind::Neutral};
    if (!enabled) return {l10n::Pick(L"已停用", L"Off"), fluent::BadgeKind::Neutral};
    std::wstring text = l10n::Pick(L"已安装", L"Installed");
    if (!version.empty()) text += L" " + version;
    return {text, fluent::BadgeKind::Success};
}
inline Badge MediaBadge(const WindowViewModel& vm) {
    return PackBadge(vm.settings_pack_media_available, vm.settings_pack_media_installed,
        vm.settings_pack_installing, true, vm.settings_pack_version);
}
inline const wchar_t* Primary(const WindowViewModel& vm) {
    return PrimaryLabel(vm.settings_pack_media_available, vm.settings_pack_media_installed, vm.settings_pack_installing);
}
inline std::wstring MediaUpdate(const WindowViewModel& vm) {
    return UpdateLabel(vm.settings_pack_media_installed, vm.settings_pack_installing, vm.settings_pack_update_version);
}
inline const wchar_t* SourceTitle() { return l10n::Pick(L"启用媒体增强", L"Enable media enhancement"); }
inline const wchar_t* SourceStatus(const WindowViewModel& vm) {
    if (!vm.settings_pack_use_custom && !vm.settings_pack_media_installed)
        return vm.settings_pack_media_available
            ? l10n::Pick(L"尚无可用媒体来源；可下载增强包或选择已有 FFmpeg", L"No media source available; download a pack or choose existing FFmpeg")
            : l10n::Pick(L"尚无可用媒体来源；可选择已有 FFmpeg", L"No media source available; choose existing FFmpeg");
    if (vm.settings_pack_use_custom && vm.settings_pack_ffmpeg != 2)
        return l10n::Pick(L"当前来源：已有 FFmpeg（路径无效，请重新选择）", L"Source: existing FFmpeg (invalid path; choose again)");
    if (!vm.settings_pack_ffmpeg_enabled)
        return vm.settings_pack_use_custom
            ? l10n::Pick(L"已停用；所选来源：已有 FFmpeg", L"Off; selected source: existing FFmpeg")
            : l10n::Pick(L"已停用；所选来源：鹿铃 管理的增强包", L"Off; selected source: Luling-managed pack");
    if (vm.settings_pack_use_custom)
        return l10n::Pick(L"当前来源：已有 FFmpeg（用户文件，不做完整性校验）", L"Source: existing FFmpeg (user file, not verified)");
    return l10n::Pick(L"当前来源：鹿铃 管理的增强包", L"Source: Luling-managed pack");
}
// "正在下载… 42%" while a pack downloads.
inline std::wstring Downloading(float progress) {
    const int percent = static_cast<int>(progress * 100.0f + 0.5f);
    return std::wstring(l10n::Pick(L"正在下载… ", L"Downloading… ")) +
           std::to_wstring(percent < 0 ? 0 : percent > 100 ? 100 : percent) + L"%";
}
inline std::wstring Downloading(const WindowViewModel& vm) { return Downloading(vm.settings_pack_progress); }
inline Badge ImagesBadge(const WindowViewModel& vm) {
    return PackBadge(vm.settings_pack_images_available, vm.settings_pack_images_installed,
        vm.settings_pack_images_installing, vm.settings_pack_images_enabled, vm.settings_pack_images_version);
}
inline const wchar_t* ImagesPrimary(const WindowViewModel& vm) {
    return PrimaryLabel(vm.settings_pack_images_available, vm.settings_pack_images_installed, vm.settings_pack_images_installing);
}
inline bool ShowsImagesEnable(const WindowViewModel& vm) { return vm.settings_pack_images_installed; }
inline std::wstring ImagesUpdate(const WindowViewModel& vm) {
    return UpdateLabel(vm.settings_pack_images_installed, vm.settings_pack_images_installing,
                       vm.settings_pack_images_update_version);
}
inline Badge RawBadge(const WindowViewModel& vm) {
    return PackBadge(vm.settings_pack_raw_available, vm.settings_pack_raw_installed,
        vm.settings_pack_raw_installing, vm.settings_pack_raw_enabled, vm.settings_pack_raw_version);
}
inline const wchar_t* RawPrimary(const WindowViewModel& vm) {
    return PrimaryLabel(vm.settings_pack_raw_available, vm.settings_pack_raw_installed, vm.settings_pack_raw_installing);
}
inline bool ShowsRawEnable(const WindowViewModel& vm) { return vm.settings_pack_raw_installed; }
inline std::wstring RawUpdate(const WindowViewModel& vm) {
    return UpdateLabel(vm.settings_pack_raw_installed, vm.settings_pack_raw_installing,
                       vm.settings_pack_raw_update_version);
}
inline Badge ArchiveBadge(const WindowViewModel& vm) {
    return PackBadge(vm.settings_pack_archive_available, vm.settings_pack_archive_installed,
        vm.settings_pack_archive_installing, vm.settings_pack_archive_enabled, vm.settings_pack_archive_version);
}
inline const wchar_t* ArchivePrimary(const WindowViewModel& vm) {
    return PrimaryLabel(vm.settings_pack_archive_available, vm.settings_pack_archive_installed, vm.settings_pack_archive_installing);
}
inline bool ShowsArchiveEnable(const WindowViewModel& vm) { return vm.settings_pack_archive_installed; }
inline std::wstring ArchiveUpdate(const WindowViewModel& vm) {
    return UpdateLabel(vm.settings_pack_archive_installed, vm.settings_pack_archive_installing,
                       vm.settings_pack_archive_update_version);
}
inline Badge TranscodeBadge(const WindowViewModel& vm) {
    return PackBadge(vm.settings_pack_transcode_available, vm.settings_pack_transcode_installed,
        vm.settings_pack_transcode_installing, vm.settings_pack_transcode_enabled, vm.settings_pack_transcode_version);
}
inline const wchar_t* TranscodePrimary(const WindowViewModel& vm) {
    return PrimaryLabel(vm.settings_pack_transcode_available, vm.settings_pack_transcode_installed, vm.settings_pack_transcode_installing);
}
inline bool ShowsTranscodeEnable(const WindowViewModel& vm) { return vm.settings_pack_transcode_installed; }
inline std::wstring TranscodeUpdate(const WindowViewModel& vm) {
    return UpdateLabel(vm.settings_pack_transcode_installed, vm.settings_pack_transcode_installing,
                       vm.settings_pack_transcode_update_version);
}
// Switch and path rows exist once there is something to switch.
inline bool ShowsEnable(const WindowViewModel& vm) { return vm.settings_pack_ffmpeg != 0 || vm.settings_pack_use_custom; }
inline bool ShowsPath(const WindowViewModel&) {
    return true;
}
inline bool ShowsDetect(const WindowViewModel& vm) {
    return !vm.settings_pack_detected_path.empty() &&
        _wcsicmp(vm.settings_pack_detected_path.c_str(), vm.settings_pack_custom_path.c_str()) != 0;
}
} // namespace pack_text

float LayoutSettingsPacks(SettingsLayout& l, const WindowViewModel& vm, float scale,
                          float y, const fluent::Painter* painter) {
    const float left = l.content.left + 20*scale, right = l.content.right - 20*scale;
    auto row = [&](float h) { auto r = D2D1::RectF(left, y, right, y + h*scale); y = r.bottom; return r; };
    auto button_w = [&](std::wstring_view text) { return painter ? painter->MeasureButtonWidth(text) : 96*scale; };
    auto caption_h = [&](std::wstring_view text, float width) {
        return text.empty() ? 0.0f : painter
            ? painter->MeasureWrappedCaptionHeight(text, (std::max)(40*scale, width)) : 40*scale;
    };
    y += caption_h(pack_text::Intro(), right - left) + 10*scale;
    const float font = static_cast<float>(vm.settings_ui_font_scale) / 100.0f;
    // 预览方式: one card per choice row, segments at the right (below when narrow).
    {
        const bool narrow = right - left < 560*scale;
        auto segments = [&](D2D1_RECT_F r, D2D1_RECT_F* output, int count, float width) {
            const auto c = D2D1::RectF(narrow ? r.left + 16*scale : r.right - (width + 16)*scale,
                r.bottom - 44*scale, r.right - 16*scale, r.bottom - 12*scale);
            const float w = (c.right - c.left) / static_cast<float>(count);
            for (int i = 0; i < count; ++i)
                output[i] = D2D1::RectF(c.left + w*static_cast<float>(i), c.top, c.left + w*static_cast<float>(i + 1), c.bottom);
        };
        const float h = narrow ? 112.0f : 64.0f + 8.0f*(font - 1.0f)*4.0f;
        l.trigger_section = row(28*font);
        l.trigger_row = row(h);
        segments(l.trigger_row, l.trigger_choice, 2, 220*font);
        if (vm.settings_preview_trigger == 0) {
            y += 8*scale;
            l.hover_delay_row = row(h);
            segments(l.hover_delay_row, l.hover_delay_choice, 3, 240*font);
            y += 8*scale;
            l.hover_size_row = row(h);
            segments(l.hover_size_row, l.hover_size_choice, 3, 240*font);
        }
        y += 24*scale;
    }
    l.preview_section = row(28*font);
    const float header_top = y;
    l.preview_header_title = D2D1::RectF(left + 54*scale, y + 12*scale, right - 48*scale, y + (12 + 26*font)*scale);
    l.preview_header_summary = D2D1::RectF(left + 54*scale, l.preview_header_title.bottom, right - 48*scale,
        l.preview_header_title.bottom + caption_h(pack_text::FormatsSummary(vm), right - left - 102*scale));
    y = l.preview_header_summary.bottom + 12*scale;
    l.disclosure[2] = D2D1::RectF(left, header_top, right, y);
    if (vm.settings_expanded & 4u) {
        l.preview_legend = D2D1::RectF(left + 54*scale, y + 4*scale, right - 16*scale, y + 4*scale);
        float legend_x = l.preview_legend.left, legend_y = l.preview_legend.top;
        for (int i = 0; i < 3; ++i) {
            const float width = painter ? painter->MeasureBadgeWidth(pack_text::Legend(i)) : (i == 2 ? 180 : 100)*scale;
            if (legend_x > l.preview_legend.left && legend_x + width > l.preview_legend.right) {
                legend_x = l.preview_legend.left; legend_y += 30*font*scale;
            }
            l.preview_legend_chip[i] = D2D1::RectF(legend_x, legend_y, (std::min)(legend_x + width, l.preview_legend.right), legend_y + 24*font*scale);
            legend_x += width + 10*scale;
        }
        const bool inline_note = l.preview_legend.right - legend_x > 250*font*scale;
        const float note_left = inline_note ? legend_x + 6*scale : l.preview_legend.left;
        const float note_top = inline_note ? legend_y + 2*scale : legend_y + 32*font*scale;
        l.preview_legend_note = D2D1::RectF(note_left, note_top, right - 16*scale,
            note_top + caption_h(pack_text::LegendNote(), right - 16*scale - note_left));
        y = (std::max)(legend_y + 24*font*scale, l.preview_legend_note.bottom) + 14*scale;
        l.preview_legend.bottom = y;
        l.preview_formats = D2D1::RectF(left, y, right, y);
        y += LayoutPreviewFormats(l.preview_formats, scale, nullptr, nullptr, caption_h);
        l.preview_formats.bottom = y;
    }
    l.preview_group = D2D1::RectF(left, header_top, right, y);
    y += 24*scale; l.preview_codec_section = row(28*font);
    const float codecs_top = y;
    const bool stacked_codecs = right - left < 620*font*scale;
    for (int i = 0; i < kPreviewCodecCount; ++i) {
        const auto info = PreviewCodec(i);
        const bool detected = (vm.settings_preview_codecs & kPreviewCodecsDetected) != 0;
        const bool has = (vm.settings_preview_codecs & (1u << i)) != 0;
        const bool button = detected && !has && info.store_id;
        const auto status = !detected ? l10n::Pick(L"检测中", L"Checking") : has
            ? l10n::Pick(L"已安装", L"Installed") : l10n::Pick(L"未安装", L"Not installed");
        const float badge_w = painter ? painter->MeasureBadgeWidth(status) : 110*scale;
        const float bw = button ? button_w(l10n::Pick(L"获取", L"Get")) : 0;
        const float text_right = stacked_codecs ? right - 16*scale : right - 28*scale - badge_w - (button ? bw + 10*scale : 0);
        const float desc_h = caption_h(info.description, text_right - left - 54*scale);
        const float text_h = 14*scale + 26*font*scale + desc_h;
        const float row_h = (std::max)(64*scale, text_h + (stacked_codecs ? 46 : 12)*scale);
        l.preview_codec_row[i] = row(row_h / scale);
        const auto r = l.preview_codec_row[i];
        l.preview_codec_text[i] = D2D1::RectF(left + 54*scale, r.top + 10*scale, text_right, r.top + text_h);
        const float controls_y = stacked_codecs ? r.bottom - 42*scale : (r.top + r.bottom - 32*scale)/2;
        if (button) l.preview_codec_button[i] = D2D1::RectF(right - 16*scale - bw, controls_y, right - 16*scale, controls_y + 32*scale);
        const float badge_right = button ? l.preview_codec_button[i].left - 10*scale : right - 16*scale;
        l.preview_codec_badge[i] = D2D1::RectF(badge_right - badge_w, controls_y + (32 - 24*font)*scale/2,
                                            badge_right, controls_y + (32 + 24*font)*scale/2);
    }
    l.preview_codec_hint = D2D1::RectF(left + 16*scale, y + 12*scale, right - 16*scale,
        y + 12*scale + caption_h(pack_text::SystemHint(vm), right - left - 32*scale));
    y = l.preview_codec_hint.bottom + 14*scale;
    l.preview_codec_group = D2D1::RectF(left, codecs_top, right, y);
    y += 30*scale; l.pack_section = row(32*font);
    // Summary: how many packs, their disk use and where they live.
    const bool compact_pack = right - left < 560*font*scale;
    l.pack_summary = row(compact_pack ? 116.0f : 76.0f);
    const float open_w = button_w(pack_text::OpenFolder());
    l.pack_open = D2D1::RectF(right - 16*scale - open_w, l.pack_summary.bottom - 48*scale,
                              right - 16*scale, l.pack_summary.bottom - 16*scale);

    // One pack card: icon, title + badge (+ switch), description, meta line
    // with the primary button (and 更新到 left of it), and the last action's
    // message.
    const float text_left = left + 54*scale, inner_right = right - 16*scale;
    struct CardRects { D2D1_RECT_F *card, *badge, *enable, *primary, *update, *notice; float* desc_h; };
    auto card_layout = [&](const CardRects& out, bool shows_enable, const pack_text::Badge& badge,
                           std::wstring_view desc, std::wstring_view primary, std::wstring_view update,
                           const std::wstring& notice) {
        const float top = y;
        float badge_right = inner_right;
        const float badge_top = top + (compact_pack ? 44*font : 19)*scale;
        if (shows_enable) {
            *out.enable = D2D1::RectF(inner_right - 42*scale, badge_top - 5*scale, inner_right, badge_top + 27*scale);
            badge_right = out.enable->left - 12*scale;
        }
        const float badge_w = painter ? painter->MeasureBadgeWidth(badge.text) : 96*scale;
        *out.badge = D2D1::RectF(badge_right - badge_w, badge_top, badge_right, badge_top + 24*font*scale);
        *out.desc_h = caption_h(desc, inner_right - text_left);
        const float desc_top = compact_pack ? out.badge->bottom + 12*scale : top + 40*scale;
        const float footer = desc_top + *out.desc_h + 12*scale;
        const float primary_w = button_w(primary);
        *out.primary = D2D1::RectF(inner_right - primary_w, footer, inner_right, footer + 32*scale);
        if (!update.empty()) {
            const float update_w = button_w(update);
            *out.update = D2D1::RectF(out.primary->left - 8*scale - update_w, footer,
                                      out.primary->left - 8*scale, footer + 32*scale);
        }
        float bottom = footer + 32*scale + 14*scale;
        if (!notice.empty()) {
            const float notice_h = caption_h(notice, inner_right - text_left - 12*scale);
            *out.notice = D2D1::RectF(text_left - 8*scale, bottom - 4*scale, inner_right, bottom + notice_h + 12*scale);
            bottom = out.notice->bottom + 12*scale;
        }
        *out.card = D2D1::RectF(left, top, right, bottom);
        y = bottom;
    };

    // Media: the FFmpeg pack card.
    y += 24*scale; l.pack_media_section = row(28);
    card_layout({&l.pack_card, &l.pack_badge, &l.pack_enable, &l.pack_primary, &l.pack_update, &l.pack_notice,
                 &l.pack_desc_h},
                false, pack_text::MediaBadge(vm), pack_text::MediaDesc(),
                pack_text::Primary(vm), pack_text::MediaUpdate(vm), vm.settings_pack_notice);

    // Optional existing FFmpeg is adjacent to the independently installable media pack.
    y += 12*scale;
    const float source_top = y;
    const bool source_switch = pack_text::ShowsEnable(vm);
    const float source_text_width = right - left - (source_switch ? 88 : 32)*scale;
    l.pack_source_status = row((caption_h(pack_text::SourceStatus(vm), source_text_width) + 47*scale) / scale);
    if (source_switch) {
        const float cy = (l.pack_source_status.top + l.pack_source_status.bottom) * 0.5f;
        l.pack_enable = D2D1::RectF(inner_right - 42*scale, cy - 16*scale, inner_right, cy + 16*scale);
    }
    l.pack_custom_row = row((caption_h(pack_text::CustomDesc(), right - left - 126*scale) + 47*scale) / scale);
    if (pack_text::ShowsPath(vm)) {
        l.pack_path_row = row(pack_text::ShowsDetect(vm) ? 110.0f : 90.0f);
        const float browse_w = button_w(pack_text::Browse());
        const float button_top = l.pack_path_row.bottom - 44*scale;
        l.pack_browse = D2D1::RectF(inner_right - browse_w, button_top, inner_right, button_top + 32*scale);
        if (pack_text::ShowsDetect(vm)) {
            const float detect_w = button_w(pack_text::UseDetected());
            l.pack_detect = D2D1::RectF(l.pack_browse.left - 8*scale - detect_w, button_top,
                                        l.pack_browse.left - 8*scale, button_top + 32*scale);
        }
    }
    l.pack_source_group = D2D1::RectF(left, source_top, right, y);

    // Images: the image pack card.
    y += 24*scale; l.pack_images_section = row(28);
    card_layout({&l.pack_images_card, &l.pack_images_badge, &l.pack_images_enable, &l.pack_images_primary,
                 &l.pack_images_update, &l.pack_images_notice, &l.pack_images_desc_h},
                pack_text::ShowsImagesEnable(vm), pack_text::ImagesBadge(vm), pack_text::ImagesDesc(),
                pack_text::ImagesPrimary(vm), pack_text::ImagesUpdate(vm), vm.settings_pack_images_notice);

    // RAW camera photos card.
    y += 12*scale;
    card_layout({&l.pack_raw_card, &l.pack_raw_badge, &l.pack_raw_enable, &l.pack_raw_primary,
                 &l.pack_raw_update, &l.pack_raw_notice, &l.pack_raw_desc_h},
                pack_text::ShowsRawEnable(vm), pack_text::RawBadge(vm), pack_text::RawDesc(),
                pack_text::RawPrimary(vm), pack_text::RawUpdate(vm), vm.settings_pack_raw_notice);

    // 7-Zip archive contents card.
    y += 24*scale; l.pack_archive_section = row(28);
    card_layout({&l.pack_archive_card, &l.pack_archive_badge, &l.pack_archive_enable, &l.pack_archive_primary,
                 &l.pack_archive_update, &l.pack_archive_notice, &l.pack_archive_desc_h},
                pack_text::ShowsArchiveEnable(vm), pack_text::ArchiveBadge(vm), pack_text::ArchiveDesc(),
                pack_text::ArchivePrimary(vm), pack_text::ArchiveUpdate(vm), vm.settings_pack_archive_notice);

    // 转换 engine card (0.2.0).
    y += 24*scale; l.pack_transcode_section = row(28);
    card_layout({&l.pack_transcode_card, &l.pack_transcode_badge, &l.pack_transcode_enable, &l.pack_transcode_primary,
                 &l.pack_transcode_update, &l.pack_transcode_notice, &l.pack_transcode_desc_h},
                pack_text::ShowsTranscodeEnable(vm), pack_text::TranscodeBadge(vm), pack_text::TranscodeDesc(),
                pack_text::TranscodePrimary(vm), pack_text::TranscodeUpdate(vm), vm.settings_pack_transcode_notice);

    // One button for every pack that is missing: a fresh install otherwise means
    // walking all four cards one by one.
    y += 24*scale;
    const float install_all_w = button_w(l10n::Get(l10n::StringId::PackInstallAll));
    l.pack_install_all = D2D1::RectF(inner_right - install_all_w, y, inner_right, y + 32*scale);
    y = l.pack_install_all.bottom + 16*scale;

    // Background updates and the uninstall choice remain separate from the media source.
    y += 24*scale; l.pack_advanced_section = row(28);
    const float group_top = y;
    l.pack_auto_update_row = row((caption_h(pack_text::AutoUpdateDesc(), right - left - 126*scale) + 47*scale) / scale);
    l.pack_remove_row = row((caption_h(pack_text::RemoveDesc(), right - left - 126*scale) + 47*scale) / scale);
    l.pack_group = D2D1::RectF(left, group_top, right, y);
    y += 12*scale;
    l.pack_note = D2D1::RectF(left, y, right, y + caption_h(pack_text::Note(), right - left - 8*scale) + 4*scale);
    y = l.pack_note.bottom + 24*scale;
    return y;
}
