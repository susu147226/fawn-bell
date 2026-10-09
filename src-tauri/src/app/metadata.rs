//! 元数据编排（执行版 §6.4）。
//!
//! §6.4 的两条来源各有其位：
//! - **图片**走内置轻量 EXIF 解析器（[`crate::infra::exif`]）：尺寸、拍摄时间、机型、GPS、方向；
//! - **视频 / 音频 / 文档**走 Windows Shell 属性系统（[`crate::infra::shell`]）：
//!   时长、分辨率、码率、艺术家、标题、作者等，零额外解码依赖。
//!
//! 只有「新建 / 变更」的条目才会走到这里（增量判定在 [`crate::app::index`]），
//! 因此重复扫描不会反复读这些文件（§7.1）。

use std::path::Path;

use crate::app::index::Metadata;
use crate::domain::kind::Kind;
use crate::infra::db::AssetRecord;
use crate::infra::{exif, shell};

/// 真实元数据提供者：图片用 EXIF，其余用 Shell 属性。
pub struct FullMetadata;

impl Metadata for FullMetadata {
    fn fill(
        &self,
        abs: &Path,
        kind: Kind,
        rec: &mut AssetRecord,
        extra: &mut Vec<(String, String)>,
    ) {
        match kind {
            Kind::Image => {
                let info = exif::read_image_info(abs);
                rec.width = info.width;
                rec.height = info.height;
                rec.capture_time = info.capture_time;
                rec.camera = info.camera;
                rec.gps_lat = info.gps_lat;
                rec.gps_lon = info.gps_lon;
                rec.orientation = info.orientation;
                // 尺寸在 EXIF 里读不到时（如 HEIC/AVIF），退回 Shell 属性兜底
                if rec.width.is_none() || rec.height.is_none() {
                    let props = shell::read_properties(abs);
                    if let Some((w, h)) = shell::size_from_props(&props) {
                        rec.width = rec.width.or(Some(w));
                        rec.height = rec.height.or(Some(h));
                    }
                }
            }
            _ => {
                let props = shell::read_properties(abs);
                if let Some((w, h)) = shell::size_from_props(&props) {
                    rec.width = Some(w);
                    rec.height = Some(h);
                }
                for (k, v) in props {
                    extra.push((k, v));
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::path::PathBuf;

    fn dir(name: &str) -> PathBuf {
        let d = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("target")
            .join("luling-tests")
            .join("metadata")
            .join(name);
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn 图片走exif填尺寸与方向() {
        let d = dir("image");
        let p = d.join("a.png");
        let mut bytes = b"\x89PNG\r\n\x1a\n".to_vec();
        bytes.extend_from_slice(&13u32.to_be_bytes());
        bytes.extend_from_slice(b"IHDR");
        bytes.extend_from_slice(&640u32.to_be_bytes());
        bytes.extend_from_slice(&480u32.to_be_bytes());
        bytes.extend_from_slice(&[8, 6, 0, 0, 0]);
        fs::write(&p, bytes).unwrap();

        let mut rec = AssetRecord::default();
        let mut extra = Vec::new();
        FullMetadata.fill(&p, Kind::Image, &mut rec, &mut extra);
        assert_eq!((rec.width, rec.height), (Some(640), Some(480)));
        assert!(extra.is_empty(), "图片不写 Shell 扩展属性");
    }

    #[test]
    fn 非图片走shell且不panic() {
        let d = dir("other");
        let p = d.join("a.bin");
        fs::write(&p, b"whatever").unwrap();
        let mut rec = AssetRecord::default();
        let mut extra = Vec::new();
        // 不强求读到属性（取决于系统），只要不 panic、不写脏数据
        FullMetadata.fill(&p, Kind::Doc, &mut rec, &mut extra);
    }
}
