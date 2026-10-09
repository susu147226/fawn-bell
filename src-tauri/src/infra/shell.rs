//! 基础设施层 · Windows Shell 属性（执行版 §6.4 / §12.3）。
//!
//! §6.4 指定的来源是**Windows Shell 属性系统**：`System.Video.FrameWidth`、`System.Media.Duration`、
//! `System.Video.EncodingBitrate`、`System.Music.Artist`、`System.Title`、`System.Author`、
//! `System.ItemTypeText` 等——零额外依赖、格式覆盖广、复用系统解码器。
//!
//! 本模块当前状态：**接口已冻结、实现待接**（P1 收尾项之一）。
//! - [`read_properties`] 目前返回空列表 → 界面显示「—」，**不编造任何数据**（§12.4⑨）；
//! - [`size_from_props`] 已按最终口径实现，一旦属性读出来即可直接生效；
//! - 调用点是 [`crate::app::metadata::FullMetadata`]，只在「新建 / 变更」的条目上发生（§7.1 增量）。

use std::path::Path;

/// Shell 属性的属性键名（与 §6.4 的 PKEY 一一对应，落库时用这套名字）。
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

/// 读取一个文件的 Shell 属性（键名 → 字符串值）。
///
/// 读不到的键不会出现在结果里；整体失败时返回空列表（元数据不该阻塞扫描）。
pub fn read_properties(_path: &Path) -> Vec<(String, String)> {
    Vec::new()
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
        // 缺一不可
        let half = vec![(KEY_FRAME_WIDTH.to_string(), "1920".to_string())];
        assert_eq!(size_from_props(&half), None);
        // 0 视为无效
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
    fn 当前实现对任意路径都不panic() {
        assert!(read_properties(Path::new(r"C:\不存在\a.mp4")).is_empty());
    }
}
