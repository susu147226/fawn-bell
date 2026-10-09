//! 核心域 · 素材类型识别（纯逻辑，无 IO）。
//!
//! 执行版 §6.1：五类以**扩展名**为准、辅以文件头嗅探；扩展名清单须可由用户增删（JSON 存应用数据目录）。
//!
//! P0 只实现「按扩展名归类」这一步，清单以内置常量表为准；**可增删清单与文件头嗅探属 P1「五类识别」**，
//! 届时时只把 [`Kind::of_ext`] 内部的查表来源换成库内清单，签名与全部调用方保持不变。

use serde::Serialize;

/// 素材大类。`other` = 未识别（仍可索引 / 改名 / 移动，见 §6.1）。
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize)]
pub enum Kind {
    #[serde(rename = "image")]
    Image,
    #[serde(rename = "video")]
    Video,
    #[serde(rename = "audio")]
    Audio,
    #[serde(rename = "3d")]
    ThreeD,
    #[serde(rename = "doc")]
    Doc,
    #[serde(rename = "other")]
    Other,
}

/// 稳定的展示顺序（顶部类型统计、图表分组一律按此序）。
pub const ALL: [Kind; 6] = [
    Kind::Image,
    Kind::Video,
    Kind::Audio,
    Kind::ThreeD,
    Kind::Doc,
    Kind::Other,
];

impl Kind {
    /// 取小写扩展名（不带点）判类；空扩展名归 `other`。
    pub fn of_ext(ext: &str) -> Kind {
        match ext {
            // —— 图片（§6.1 image，含 RAW 系）——
            "jpg" | "jpeg" | "png" | "webp" | "gif" | "bmp" | "tif" | "tiff" | "avif" | "heic"
            | "heif" | "svg" | "ico" | "psd" | "psb" | "ai" | "eps" | "cr2" | "cr3" | "nef"
            | "arw" | "dng" | "raf" | "orf" | "rw2" => Kind::Image,
            // —— 视频（§6.1 video）——
            "mp4" | "mov" | "mkv" | "avi" | "webm" | "m4v" | "flv" | "wmv" | "mpg" | "mpeg"
            | "ts" | "m2ts" | "r3d" | "braw" => Kind::Video,
            // —— 音频（§6.1 audio）——
            "mp3" | "wav" | "flac" | "aac" | "m4a" | "ogg" | "opus" | "wma" | "aiff" | "mid" => {
                Kind::Audio
            }
            // —— 3D（§6.1 3d）——
            "blend" | "blend1" | "max" | "fbx" | "obj" | "mtl" | "stl" | "gltf" | "glb" | "c4d"
            | "ma" | "mb" | "hip" | "hipnc" | "ztl" | "ztn" | "zpr" | "spp" | "sbsar" | "sbs"
            | "abc" | "usd" | "usda" | "usdc" | "usdz" | "skp" | "3dm" | "step" | "stp" | "iges"
            | "igs" | "dwg" | "dxf" | "f3d" | "sldprt" | "sldasm" | "uasset" | "umap" | "unity"
            | "prefab" => Kind::ThreeD,
            // —— 文档（§6.1 doc）——
            "pdf" | "doc" | "docx" | "xls" | "xlsx" | "ppt" | "pptx" | "txt" | "md" | "rtf"
            | "csv" | "json" | "xml" | "yaml" | "epub" | "mobi" | "one" | "pages" | "numbers"
            | "key" => Kind::Doc,
            _ => Kind::Other,
        }
    }

    /// 稳定的机器可读名（前端与库内一律用它）。
    pub fn as_str(self) -> &'static str {
        match self {
            Kind::Image => "image",
            Kind::Video => "video",
            Kind::Audio => "audio",
            Kind::ThreeD => "3d",
            Kind::Doc => "doc",
            Kind::Other => "other",
        }
    }

    /// 界面展示名（中文）。
    pub fn label_zh(self) -> &'static str {
        match self {
            Kind::Image => "图片",
            Kind::Video => "视频",
            Kind::Audio => "音频",
            Kind::ThreeD => "3D",
            Kind::Doc => "文档",
            Kind::Other => "其他",
        }
    }
}

/// 由文件名取小写扩展名（不含点）。无扩展名返回空串。
///
/// 注意：`a.tar.gz` 只取最后一段 `gz`（§6.1 明确「以扩展名为准」）。
pub fn ext_of(name: &str) -> String {
    match name.rsplit_once('.') {
        // 前导点开头的名字（如 `.gitignore`）不算扩展名
        Some((stem, ext)) if !stem.is_empty() => ext.to_ascii_lowercase(),
        _ => String::new(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn 五类与未识别各归其位() {
        assert_eq!(Kind::of_ext("jpg"), Kind::Image);
        assert_eq!(Kind::of_ext("cr3"), Kind::Image);
        assert_eq!(Kind::of_ext("mkv"), Kind::Video);
        assert_eq!(Kind::of_ext("braw"), Kind::Video);
        assert_eq!(Kind::of_ext("flac"), Kind::Audio);
        assert_eq!(Kind::of_ext("sbsar"), Kind::ThreeD);
        assert_eq!(Kind::of_ext("pptx"), Kind::Doc);
        assert_eq!(Kind::of_ext(""), Kind::Other);
        assert_eq!(Kind::of_ext("xyz"), Kind::Other);
    }

    #[test]
    fn 扩展名解析() {
        assert_eq!(ext_of("DSC_0001.JPG"), "jpg");
        assert_eq!(ext_of("no_ext"), "");
        assert_eq!(ext_of(".gitignore"), "");
        assert_eq!(ext_of("archive.tar.gz"), "gz");
    }

    #[test]
    fn 展示名与机器名稳定() {
        for k in ALL {
            assert!(!k.as_str().is_empty());
            assert!(!k.label_zh().is_empty());
        }
        assert_eq!(Kind::ThreeD.as_str(), "3d");
    }
}
