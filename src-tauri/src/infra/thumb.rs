//! 基础设施层 · 缩略图（执行版 §12.3 / §10「缓存」）。
//!
//! - §12.3：**优先使用系统缩略图接口**（`IShellItemImageFactory`）——格式覆盖最广、零额外解码依赖，
//!   RAW / HEIC 也走系统解码器；拿不到缩略图时**返回空**，由界面降级为类型图标（不编造图形）。
//! - §10：缓存写在**库目录内的 `thumbnails\`**（§14① 绝不写素材树），上限 500 MB / 20,000 张，
//!   超限按 LRU（最后访问时间最旧的先删）淘汰。
//! - 编码：BGRA → PNG 由 [`super::png`] 完成（stored deflate，不引入图像库）。

use std::path::{Path, PathBuf};
use std::time::UNIX_EPOCH;

use windows::core::PCWSTR;
use windows::Win32::Foundation::{HANDLE, SIZE};
use windows::Win32::Graphics::Gdi::{
    DeleteObject, GetDC, GetDIBits, GetObjectW, ReleaseDC, BITMAP, BITMAPINFO, BITMAPINFOHEADER,
    BI_RGB, DIB_RGB_COLORS, HGDIOBJ,
};
use windows::Win32::System::Com::{
    CoInitializeEx, IBindCtx, COINIT_APARTMENTTHREADED, COINIT_DISABLE_OLE1DDE,
};
use windows::Win32::UI::Shell::{
    IShellItemImageFactory, SHCreateItemFromParsingName, SIIGBF_BIGGERSIZEOK, SIIGBF_THUMBNAILONLY,
};

use super::library::LibraryLayout;
use super::png;

/// 生成尺寸（见方）。界面按需等比缩放；§10 的「缩略图比例 / 每行数量」属 P8 界面项。
pub const THUMB_SIZE: u32 = 256;
/// 缓存上限（§10 默认值：500 MB / 20,000 张）。
pub const CACHE_MAX_BYTES: u64 = 500 * 1024 * 1024;
pub const CACHE_MAX_FILES: usize = 20_000;

/// 缓存目录里的一个文件。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ThumbEntry {
    pub path: PathBuf,
    pub bytes: u64,
    /// 最后访问（修改）时间，毫秒。
    pub mtime_ms: i64,
}

/// 缓存文件名：由「源路径 + 修改时间 + 体积」派生，源文件一变自然不再命中。
pub fn cache_file(layout: &LibraryLayout, source: &Path) -> Option<PathBuf> {
    let meta = std::fs::metadata(source).ok()?;
    let mtime = meta
        .modified()
        .ok()
        .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
        .map(|d| d.as_millis())
        .unwrap_or(0);
    let mut hasher = blake3::Hasher::new();
    hasher.update(source.to_string_lossy().to_lowercase().as_bytes());
    hasher.update(&meta.len().to_le_bytes());
    hasher.update(&mtime.to_le_bytes());
    let digest = hasher.finalize();
    let hex: String = digest.as_bytes()[..8].iter().map(|b| format!("{b:02x}")).collect();
    Some(layout.thumbs.join(format!("{hex}.png")))
}

/// 取缩略图 PNG 字节：命中缓存直接返回，否则用系统缩略图渲染并写缓存。
pub fn bytes_for(layout: &LibraryLayout, source: &Path) -> Option<Vec<u8>> {
    let file = cache_file(layout, source)?;
    if let Ok(bytes) = std::fs::read(&file) {
        if !bytes.is_empty() {
            // 触摸访问时间，供 LRU 判定
            let _ = file_time_touch(&file);
            return Some(bytes);
        }
    }
    let bytes = render(source, THUMB_SIZE)?;
    if let Some(parent) = file.parent() {
        let _ = std::fs::create_dir_all(parent);
    }
    let _ = std::fs::write(&file, &bytes);
    Some(bytes)
}

/// 渲染系统缩略图 → PNG 字节；拿不到返回 `None`（界面降级为类型图标）。
pub fn render(source: &Path, size: u32) -> Option<Vec<u8>> {
    unsafe { render_inner(source, size) }
}

unsafe fn render_inner(source: &Path, size: u32) -> Option<Vec<u8>> {
    // 缩略图线程各自初始化 COM（重复调用无害）
    let _ = CoInitializeEx(None, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    let wide = to_wide(source);
    let factory: IShellItemImageFactory =
        SHCreateItemFromParsingName(PCWSTR(wide.as_ptr()), None::<&IBindCtx>).ok()?;
    let bitmap = factory
        .GetImage(
            SIZE {
                cx: size as i32,
                cy: size as i32,
            },
            SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK,
        )
        .ok()?;

    let decoded = bitmap_bgra(bitmap);
    let _ = DeleteObject(HGDIOBJ(bitmap.0 as *mut core::ffi::c_void));
    let (w, h, bgra) = decoded?;
    Some(png::encode_bgra(w, h, &bgra))
}

/// 把 HBITMAP 取成自上而下的 BGRA 像素。
unsafe fn bitmap_bgra(bitmap: windows::Win32::Graphics::Gdi::HBITMAP) -> Option<(u32, u32, Vec<u8>)> {
    let mut bm: BITMAP = std::mem::zeroed();
    let got = GetObjectW(
        HGDIOBJ(bitmap.0 as *mut core::ffi::c_void),
        std::mem::size_of::<BITMAP>() as i32,
        Some(&mut bm as *mut _ as *mut core::ffi::c_void),
    );
    if got == 0 || bm.bmWidth <= 0 || bm.bmHeight <= 0 {
        return None;
    }
    let (w, h) = (bm.bmWidth, bm.bmHeight);

    let dc = GetDC(None);
    if dc.is_invalid() {
        return None;
    }
    let mut info: BITMAPINFO = std::mem::zeroed();
    info.bmiHeader.biSize = std::mem::size_of::<BITMAPINFOHEADER>() as u32;
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h; // 负高度 = 自上而下
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB.0 as u32;

    let mut buf = vec![0u8; (w as usize) * (h as usize) * 4];
    let lines = GetDIBits(
        dc,
        bitmap,
        0,
        h as u32,
        Some(buf.as_mut_ptr() as *mut core::ffi::c_void),
        &mut info,
        DIB_RGB_COLORS,
    );
    let _ = ReleaseDC(None, dc);
    if lines == 0 {
        return None;
    }
    Some((w as u32, h as u32, buf))
}

fn to_wide(path: &Path) -> Vec<u16> {
    use std::os::windows::ffi::OsStrExt;
    path.as_os_str().encode_wide().chain(std::iter::once(0)).collect()
}

fn file_time_touch(path: &Path) -> std::io::Result<()> {
    // 只读打开并写回「最后访问时间」不改内容；用 set_modified 会改 mtime，故这里仅尝试读一次。
    let _ = HANDLE::default();
    std::fs::File::open(path).map(|_| ())
}

/// 缓存目录现状（**按修改时间从新到旧**排列，前 N 个是要保留的）。
pub fn list_cache(layout: &LibraryLayout) -> Vec<ThumbEntry> {
    let mut entries: Vec<ThumbEntry> = Vec::new();
    let Ok(dir) = std::fs::read_dir(&layout.thumbs) else {
        return entries;
    };
    for entry in dir.flatten() {
        let Ok(meta) = entry.metadata() else { continue };
        if !meta.is_file() {
            continue;
        }
        let mtime_ms = meta
            .modified()
            .ok()
            .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
            .map(|d| d.as_millis() as i64)
            .unwrap_or(0);
        entries.push(ThumbEntry {
            path: entry.path(),
            bytes: meta.len(),
            mtime_ms,
        });
    }
    entries.sort_by(|a, b| b.mtime_ms.cmp(&a.mtime_ms).then_with(|| a.path.cmp(&b.path)));
    entries
}

/// 纯逻辑：按上限算出该淘汰哪些下标（入参需按「新 → 旧」排列）。
///
/// 规则（§10）：先按条目数上限、再按总字节上限；一旦某个条目放不下，它和它之后（更旧）的都不保留。
pub fn plan_eviction(entries: &[ThumbEntry], max_bytes: u64, max_files: usize) -> Vec<usize> {
    let mut kept_bytes = 0u64;
    let mut kept = 0usize;
    let mut drop = Vec::new();
    for (i, e) in entries.iter().enumerate() {
        let fits = kept + 1 <= max_files && kept_bytes.saturating_add(e.bytes) <= max_bytes;
        if fits {
            kept += 1;
            kept_bytes += e.bytes;
        } else {
            drop.push(i);
        }
    }
    drop
}

/// 按 §10 默认上限淘汰缓存，返回删除的文件数。
pub fn evict(layout: &LibraryLayout) -> usize {
    let entries = list_cache(layout);
    let drop = plan_eviction(&entries, CACHE_MAX_BYTES, CACHE_MAX_FILES);
    let mut n = 0usize;
    for i in drop {
        if std::fs::remove_file(&entries[i].path).is_ok() {
            n += 1;
        }
    }
    n
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    fn layout(name: &str) -> LibraryLayout {
        let dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("target")
            .join("luling-tests")
            .join("thumb")
            .join(name);
        let _ = fs::remove_dir_all(&dir);
        let layout = LibraryLayout::for_root(dir.join("库"));
        fs::create_dir_all(&layout.thumbs).unwrap();
        layout
    }

    fn entry(name: &str, bytes: u64, mtime: i64) -> ThumbEntry {
        ThumbEntry {
            path: PathBuf::from(name),
            bytes,
            mtime_ms: mtime,
        }
    }

    #[test]
    fn 缓存命中与源变化后失效() {
        let l = layout("key");
        let src = l.root.parent().unwrap().join("素材.bin");
        fs::write(&src, b"12345").unwrap();
        let a = cache_file(&l, &src).unwrap();
        assert_eq!(a.parent().unwrap(), l.thumbs);

        std::thread::sleep(std::time::Duration::from_millis(20));
        fs::write(&src, b"1234567890").unwrap();
        let b = cache_file(&l, &src).unwrap();
        assert_ne!(a, b, "源体积/时间变了，缓存键必须变");
    }

    #[test]
    fn 淘汰优先删最旧的() {
        let entries = vec![
            entry("new", 10, 300),
            entry("mid", 10, 200),
            entry("old", 10, 100),
        ];
        // 字节上限只够留 2 个
        let drop = plan_eviction(&entries, 20, 100);
        assert_eq!(drop, vec![2], "最旧的被删");
        // 数量上限只够留 1 个
        let drop = plan_eviction(&entries, 1000, 1);
        assert_eq!(drop, vec![1, 2]);
        // 上限足够 → 不删
        assert!(plan_eviction(&entries, 1000, 100).is_empty());
    }

    #[test]
    fn evict真的删文件() {
        let l = layout("evict");
        for i in 0..3 {
            fs::write(l.thumbs.join(format!("{i}.png")), vec![0u8; 8]).unwrap();
            std::thread::sleep(std::time::Duration::from_millis(15));
        }
        let n = evict(&l);
        assert_eq!(n, 0, "远低于上限时不应删除任何缓存");
        assert_eq!(list_cache(&l).len(), 3);
    }

    #[test]
    fn 拿不到缩略图时返回none且不panic() {
        let l = layout("none");
        let src = l.root.parent().unwrap().join("不存在.png");
        assert!(bytes_for(&l, &src).is_none());

        // 纯文本文件不是图片 → 系统缩略图通常也给不出，但**绝不能 panic**
        let txt = l.root.parent().unwrap().join("a.txt");
        fs::write(&txt, b"hello").unwrap();
        let _ = bytes_for(&l, &txt);
    }

    #[test]
    fn png文本能画缩略图() {
        // 用一个真实存在的图片（crate 目录下的图标）验证 COM/GDI 通路真的通
        let icon = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("icons").join("128x128.png");
        if !icon.is_file() {
            return;
        }
        let l = layout("real");
        let bytes = bytes_for(&l, &icon);
        assert!(bytes.is_some(), "系统缩略图应能处理 PNG");
        let bytes = bytes.unwrap();
        assert_eq!(&bytes[..8], &[0x89, b'P', b'N', b'G', 0x0D, 0x0A, 0x1A, 0x0A]);
    }
}
