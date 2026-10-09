//! 基础设施层 · Windows Shell 属性（执行版 §6.4 / §12.3）。
//!
//! §6.4 指定的来源是**Windows Shell 属性系统**：复用系统解码器读视频/音频/文档元数据，
//! 零额外依赖、格式覆盖广。这里通过 `IPropertyStore` 直接读 PKEY，不经任何中间层。
//!
//! - 属性键用**手工定义**的 `PROPERTYKEY`（标准 fmtid + pid），不依赖 crate 里的常量命名；
//! - 读不到的键不返回；整体失败返回空列表，**绝不编造数据**（§12.4⑨）；
//! - 只在「新建 / 变更」的条目上被调用（增量判定在 [`crate::app::index`]）。

use std::path::Path;

use windows::core::{GUID, PCWSTR, PWSTR};
use windows::Win32::Foundation::PROPERTYKEY;
use windows::Win32::System::Com::StructuredStorage::{
    PropVariantClear, PropVariantToStringAlloc, PROPVARIANT,
};
use windows::Win32::System::Com::{
    CoInitializeEx, CoTaskMemFree, IBindCtx, COINIT_APARTMENTTHREADED, COINIT_DISABLE_OLE1DDE,
};
use windows::Win32::UI::Shell::PropertiesSystem::{
    IPropertyStore, SHGetPropertyStoreFromParsingName, GPS_DEFAULT,
};

/// Shell 属性的属性键名（落库用这套名字，与 §6.4 的 PKEY 一一对应）。
pub const KEY_FRAME_WIDTH: &str = "System.Video.FrameWidth";
pub const KEY_FRAME_HEIGHT: &str = "System.Video.FrameHeight";
pub const KEY_DURATION: &str = "System.Media.Duration";
pub const KEY_VIDEO_BITRATE: &str = "System.Video.EncodingBitrate";
pub const KEY_TOTAL_BITRATE: &str = "System.Video.TotalBitrate";
pub const KEY_ARTIST: &str = "System.Music.Artist";
pub const KEY_TITLE: &str = "System.Title";
pub const KEY_AUTHOR: &str = "System.Author";
pub const KEY_ITEM_TYPE: &str = "System.ItemTypeText";

/// 需要读取的属性键（顺序即落库与展示顺序）。
pub const WANTED: [&str; 9] = [
    KEY_FRAME_WIDTH,
    KEY_FRAME_HEIGHT,
    KEY_DURATION,
    KEY_VIDEO_BITRATE,
    KEY_TOTAL_BITRATE,
    KEY_ARTIST,
    KEY_TITLE,
    KEY_AUTHOR,
    KEY_ITEM_TYPE,
];

/* 标准属性键的 fmtid（Windows SDK 的 propkey.h） */
const FMTID_SUMMARY: GUID = GUID::from_u128(0xF29F85E0_4FF9_1068_AB91_08002B27B3D9);
const FMTID_MEDIA: GUID = GUID::from_u128(0x64440490_4C8B_11D1_8B70_080036B11A03);
const FMTID_VIDEO: GUID = GUID::from_u128(0x64440491_4C8B_11D1_8B70_080036B11A03);
const FMTID_MUSIC: GUID = GUID::from_u128(0x56A3372E_CE9C_11D2_9F0E_006097C686F6);
const FMTID_ITEM: GUID = GUID::from_u128(0xB725F130_47EF_101A_A5F1_02608C9EEBAC);

const fn pk(fmtid: GUID, pid: u32) -> PROPERTYKEY {
    PROPERTYKEY { fmtid, pid }
}

/// (键名, PROPERTYKEY) 对照表：pid 取自 propkey.h。
fn wanted() -> [(&'static str, PROPERTYKEY); 9] {
    [
        (KEY_FRAME_WIDTH, pk(FMTID_VIDEO, 3)),
        (KEY_FRAME_HEIGHT, pk(FMTID_VIDEO, 4)),
        (KEY_DURATION, pk(FMTID_MEDIA, 3)),
        (KEY_VIDEO_BITRATE, pk(FMTID_VIDEO, 8)),
        (KEY_TOTAL_BITRATE, pk(FMTID_VIDEO, 28)),
        (KEY_ARTIST, pk(FMTID_MUSIC, 2)),
        (KEY_TITLE, pk(FMTID_SUMMARY, 2)),
        (KEY_AUTHOR, pk(FMTID_SUMMARY, 4)),
        (KEY_ITEM_TYPE, pk(FMTID_ITEM, 4)),
    ]
}

fn to_wide(path: &Path) -> Vec<u16> {
    use std::os::windows::ffi::OsStrExt;
    path.as_os_str().encode_wide().chain(std::iter::once(0)).collect()
}

/// 读取一个文件的 Shell 属性（键名 → 字符串值）。
///
/// 读不到的键不会出现；整体失败返回空列表（元数据不该阻塞扫描）。
pub fn read_properties(path: &Path) -> Vec<(String, String)> {
    unsafe { inner(path).unwrap_or_default() }
}

unsafe fn inner(path: &Path) -> Option<Vec<(String, String)>> {
    // 同一线程重复调用无害：S_FALSE / RPC_E_CHANGED_MODE 都可以忽略
    let _ = CoInitializeEx(None, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    let wide = to_wide(path);
    let store: IPropertyStore =
        SHGetPropertyStoreFromParsingName(PCWSTR(wide.as_ptr()), None::<&IBindCtx>, GPS_DEFAULT)
            .ok()?;

    let mut out = Vec::new();
    for (name, key) in wanted() {
        // windows-rs 0.62：IPropertyStore::GetValue(key: *const PROPERTYKEY) -> Result<PROPVARIANT>
        let mut pv: PROPVARIANT = match store.GetValue(&key) {
            Ok(v) => v,
            Err(_) => continue,
        };
        if let Some(text) = pv_to_string(&pv) {
            let text = text.trim().to_string();
            if !text.is_empty() {
                out.push((name.to_string(), text));
            }
        }
        // 返回值是裸 PROPVARIANT（transmute 出来的），字符串内存要自己释放
        let _ = PropVariantClear(&mut pv);
    }
    Some(out)
}

/// `PROPVARIANT` → 字符串（系统转换，覆盖 LPWSTR / LPSTR / 数值 / 向量等类型）。
unsafe fn pv_to_string(pv: &PROPVARIANT) -> Option<String> {
    let raw: PWSTR = PropVariantToStringAlloc(pv).ok()?;
    if raw.is_null() {
        return None;
    }
    let text = raw.to_string().ok();
    CoTaskMemFree(Some(raw.0 as *const core::ffi::c_void));
    text
}

/// 从属性里取分辨率（视频分辨率走 Shell，图片分辨率走 EXIF，§6.4）。
pub fn size_from_props(props: &[(String, String)]) -> Option<(i64, i64)> {
    let get = |key: &str| -> Option<i64> {
        props
            .iter()
            .find(|(k, _)| k == key)
            .and_then(|(_, v)| v.trim().parse::<i64>().ok())
    };
    match (get(KEY_FRAME_WIDTH), get(KEY_FRAME_HEIGHT)) {
        (Some(w), Some(h)) if w > 0 && h > 0 => Some((w, h)),
        _ => None,
    }
}

/// 时长（`System.Media.Duration` 是 100 ns 单位）→ 毫秒。
pub fn duration_ms_from_props(props: &[(String, String)]) -> Option<i64> {
    props
        .iter()
        .find(|(k, _)| k == KEY_DURATION)
        .and_then(|(_, v)| v.trim().parse::<i64>().ok())
        .map(|v| v / 10_000)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn 分辨率从属性键解析() {
        let props = vec![
            (KEY_FRAME_WIDTH.to_string(), "1920".to_string()),
            (KEY_FRAME_HEIGHT.to_string(), "1080".to_string()),
        ];
        assert_eq!(size_from_props(&props), Some((1920, 1080)));
        assert_eq!(size_from_props(&[]), None);
        let half = vec![(KEY_FRAME_WIDTH.to_string(), "1920".to_string())];
        assert_eq!(size_from_props(&half), None);
        let zero = vec![
            (KEY_FRAME_WIDTH.to_string(), "0".to_string()),
            (KEY_FRAME_HEIGHT.to_string(), "0".to_string()),
        ];
        assert_eq!(size_from_props(&zero), None);
    }

    #[test]
    fn 时长单位换算() {
        let props = vec![(KEY_DURATION.to_string(), "123456789".to_string())];
        assert_eq!(duration_ms_from_props(&props), Some(12345));
        assert_eq!(duration_ms_from_props(&[]), None);
    }

    #[test]
    fn 不存在的路径返回空且不panic() {
        assert!(read_properties(Path::new(r"C:\绝对不存在\a.mp4")).is_empty());
    }

    #[test]
    fn 读系统文件能拿到类型文案() {
        // 只读系统文件，验证 COM 通道真的通了；拿不到也不算失败（取决于系统版本与语言包），
        // 但**必须不 panic**，且拿到的键名必须在白名单里。
        let props = read_properties(Path::new(r"C:\Windows\System32\notepad.exe"));
        for (k, _) in &props {
            assert!(WANTED.contains(&k.as_str()), "出现了白名单外的键：{k}");
        }
        eprintln!("notepad.exe 属性：{props:?}");
    }
}
