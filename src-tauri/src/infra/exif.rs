//! 基础设施层 · 内置轻量 EXIF / 图片尺寸解析（执行版 §6.4）。
//!
//! §6.4 要求：图片的**尺寸、拍摄时间、机型、GPS、方向**走内置轻量解析器（不引入解码库），
//! 无 EXIF 时回退到文件时间（回退由上层负责，这里只报「没读到」）。
//!
//! 覆盖：PNG / JPEG（含 EXIF）/ GIF / BMP / WebP（VP8X、VP8、VP8L）/ TIFF（含 EXIF）。
//! HEIC / AVIF 的尺寸在 P1 **不解析**（ISOBMFF 解析留到需要时；此时返回空值，
//! 由上层用 Shell 属性兜底，界面显示「—」而不是假数据）。
//!
//! 全程只读、只用文件头（默认最多读 1 MB），不做任何写入（§14①）。

use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

use crate::domain::kind;

/// 解析结果；没读到的一律为 `None`（界面显示「—」，绝不编造）。
#[derive(Debug, Clone, Default, PartialEq)]
pub struct ImageInfo {
    pub width: Option<i64>,
    pub height: Option<i64>,
    /// 拍摄时间（Unix 毫秒；EXIF 里是无时区本地时间，按 UTC 解释，只用于排序与展示）。
    pub capture_time: Option<i64>,
    /// 机型（`Make Model`，两者都缺则为 `None`）。
    pub camera: Option<String>,
    pub gps_lat: Option<f64>,
    pub gps_lon: Option<f64>,
    /// EXIF 方向 1–8（§6.4「方向」）。
    pub orientation: Option<i64>,
}

/// 最多读取的文件头字节数（EXIF 与尺寸信息都在头部）。
const HEAD_BYTES: u64 = 1024 * 1024;

/// 读一张图片的元数据（任何失败都返回空结果，不抛错——元数据不该阻塞扫描）。
pub fn read_image_info(path: &Path) -> ImageInfo {
    let name = path.file_name().map(|s| s.to_string_lossy().to_string()).unwrap_or_default();
    let ext = kind::ext_of(&name).to_ascii_lowercase();

    let head = match read_head(path) {
        Ok(h) => h,
        Err(_) => return ImageInfo::default(),
    };

    let mut info = match ext.as_str() {
        "png" => png_size(&head),
        "gif" => gif_size(&head),
        "bmp" => bmp_size(&head),
        "webp" => webp_size(&head),
        "jpg" | "jpeg" | "jpe" | "jfif" => jpeg_info(&head),
        "tif" | "tiff" => tiff_info(&head),
        _ => ImageInfo::default(),
    };

    // JPEG / TIFF 之外的格式（PNG 等）也可能带 EXIF，这里只对 JPEG/TIFF 走 TIFF 解析
    if let Some(off) = find_exif_tiff_offset_if_jpeg(&head, &ext) {
        let exif = parse_tiff(&head[off..]);
        merge(&mut info, exif);
    }
    info
}

fn read_head(path: &Path) -> std::io::Result<Vec<u8>> {
    let mut file = File::open(path)?;
    let len = file.metadata()?.len();
    let take = len.min(HEAD_BYTES) as usize;
    let mut buf = vec![0u8; take];
    let mut read = 0usize;
    while read < take {
        let n = file.read(&mut buf[read..])?;
        if n == 0 {
            break;
        }
        read += n;
    }
    buf.truncate(read);
    Ok(buf)
}

fn merge(base: &mut ImageInfo, extra: ImageInfo) {
    if base.width.is_none() {
        base.width = extra.width;
    }
    if base.height.is_none() {
        base.height = extra.height;
    }
    if base.capture_time.is_none() {
        base.capture_time = extra.capture_time;
    }
    if base.camera.is_none() {
        base.camera = extra.camera;
    }
    if base.gps_lat.is_none() {
        base.gps_lat = extra.gps_lat;
    }
    if base.gps_lon.is_none() {
        base.gps_lon = extra.gps_lon;
    }
    if base.orientation.is_none() {
        base.orientation = extra.orientation;
    }
}

fn be_u32(b: &[u8]) -> Option<u32> {
    Some(u32::from_be_bytes([*b.first()?, *b.get(1)?, *b.get(2)?, *b.get(3)?]))
}

/* ── 各容器格式的尺寸 ─────────────────────────────────────────────── */

fn png_size(b: &[u8]) -> ImageInfo {
    if b.len() < 24 || &b[..8] != b"\x89PNG\r\n\x1a\n" || &b[12..16] != b"IHDR" {
        return ImageInfo::default();
    }
    ImageInfo {
        width: be_u32(&b[16..20]).map(|v| v as i64),
        height: be_u32(&b[20..24]).map(|v| v as i64),
        ..Default::default()
    }
}

fn gif_size(b: &[u8]) -> ImageInfo {
    if b.len() < 10 || (&b[..6] != b"GIF87a" && &b[..6] != b"GIF89a") {
        return ImageInfo::default();
    }
    ImageInfo {
        width: Some(u16::from_le_bytes([b[6], b[7]]) as i64),
        height: Some(u16::from_le_bytes([b[8], b[9]]) as i64),
        ..Default::default()
    }
}

fn bmp_size(b: &[u8]) -> ImageInfo {
    if b.len() < 26 || &b[..2] != b"BM" {
        return ImageInfo::default();
    }
    let w = i32::from_le_bytes([b[18], b[19], b[20], b[21]]);
    let h = i32::from_le_bytes([b[22], b[23], b[24], b[25]]);
    ImageInfo {
        width: Some(w.unsigned_abs() as i64),
        height: Some(h.unsigned_abs() as i64),
        ..Default::default()
    }
}

fn webp_size(b: &[u8]) -> ImageInfo {
    if b.len() < 30 || &b[..4] != b"RIFF" || &b[8..12] != b"WEBP" {
        return ImageInfo::default();
    }
    match &b[12..16] {
        b"VP8X" => {
            // 24 位小端（宽高各减一）
            let w = 1 + (b[24] as i64 | (b[25] as i64) << 8 | (b[26] as i64) << 16);
            let h = 1 + (b[27] as i64 | (b[28] as i64) << 8 | (b[29] as i64) << 16);
            ImageInfo {
                width: Some(w),
                height: Some(h),
                ..Default::default()
            }
        }
        b"VP8 " => {
            // 关键帧头：帧标签(3) + 起始码 9D 01 2A 之后是 14 位宽高
            if b.len() < 30 {
                return ImageInfo::default();
            }
            let w = (u16::from_le_bytes([b[26], b[27]]) & 0x3FFF) as i64;
            let h = (u16::from_le_bytes([b[28], b[29]]) & 0x3FFF) as i64;
            ImageInfo {
                width: Some(w),
                height: Some(h),
                ..Default::default()
            }
        }
        b"VP8L" => {
            // 无损：签名 0x2F 后 14 位宽、14 位高
            if b.len() < 25 || b[20] != 0x2F {
                return ImageInfo::default();
            }
            let bits = u32::from_le_bytes([b[21], b[22], b[23], b[24]]);
            let w = (bits & 0x3FFF) as i64 + 1;
            let h = ((bits >> 14) & 0x3FFF) as i64 + 1;
            ImageInfo {
                width: Some(w),
                height: Some(h),
                ..Default::default()
            }
        }
        _ => ImageInfo::default(),
    }
}

/* ── JPEG：段扫描，取 SOF 尺寸与 APP1 里的 EXIF ───────────────────── */

fn find_exif_tiff_offset_if_jpeg(b: &[u8], ext: &str) -> Option<usize> {
    if !matches!(ext, "jpg" | "jpeg" | "jpe" | "jfif") {
        return None;
    }
    jpeg_exif_offset(b)
}

/// 找出 APP1(Exif) 段中 TIFF 头的偏移。
fn jpeg_exif_offset(b: &[u8]) -> Option<usize> {
    if b.len() < 4 || b[0] != 0xFF || b[1] != 0xD8 {
        return None;
    }
    let mut i = 2usize;
    while i + 4 <= b.len() {
        if b[i] != 0xFF {
            i += 1;
            continue;
        }
        let marker = b[i + 1];
        if marker == 0xD8 || (0xD0..=0xD7).contains(&marker) || marker == 0x01 {
            i += 2;
            continue;
        }
        if marker == 0xDA || marker == 0xD9 {
            return None; // 进入压缩数据，不再有元数据
        }
        let seg_len = u16::from_be_bytes([b[i + 2], b[i + 3]]) as usize;
        if seg_len < 2 || i + 2 + seg_len > b.len() {
            return None;
        }
        if marker == 0xE1 && seg_len >= 8 && &b[i + 4..i + 10] == b"Exif\0\0" {
            return Some(i + 10);
        }
        i += 2 + seg_len;
    }
    None
}

fn jpeg_info(b: &[u8]) -> ImageInfo {
    if b.len() < 4 || b[0] != 0xFF || b[1] != 0xD8 {
        return ImageInfo::default();
    }
    let mut i = 2usize;
    while i + 4 <= b.len() {
        if b[i] != 0xFF {
            i += 1;
            continue;
        }
        let marker = b[i + 1];
        if marker == 0xD8 || (0xD0..=0xD7).contains(&marker) || marker == 0x01 {
            i += 2;
            continue;
        }
        if marker == 0xDA || marker == 0xD9 {
            break;
        }
        let seg_len = u16::from_be_bytes([b[i + 2], b[i + 3]]) as usize;
        if seg_len < 2 || i + 2 + seg_len > b.len() {
            break;
        }
        // SOF0–SOF15（去掉 DHT=C4、JPG=C8、DAC=CC）
        let is_sof = (0xC0..=0xCF).contains(&marker) && !matches!(marker, 0xC4 | 0xC8 | 0xCC);
        if is_sof && seg_len >= 7 {
            let h = u16::from_be_bytes([b[i + 5], b[i + 6]]) as i64;
            let w = u16::from_be_bytes([b[i + 7], b[i + 8]]) as i64;
            return ImageInfo {
                width: Some(w),
                height: Some(h),
                ..Default::default()
            };
        }
        i += 2 + seg_len;
    }
    ImageInfo::default()
}

/* ── TIFF / EXIF ──────────────────────────────────────────────────── */

#[derive(Clone, Copy, PartialEq)]
enum Endian {
    Little,
    Big,
}

struct Tiff<'a> {
    b: &'a [u8],
    e: Endian,
}

impl<'a> Tiff<'a> {
    fn u16(&self, off: usize) -> Option<u16> {
        let s = self.b.get(off..off + 2)?;
        Some(match self.e {
            Endian::Little => u16::from_le_bytes([s[0], s[1]]),
            Endian::Big => u16::from_be_bytes([s[0], s[1]]),
        })
    }
    fn u32(&self, off: usize) -> Option<u32> {
        let s = self.b.get(off..off + 4)?;
        Some(match self.e {
            Endian::Little => u32::from_le_bytes([s[0], s[1], s[2], s[3]]),
            Endian::Big => u32::from_be_bytes([s[0], s[1], s[2], s[3]]),
        })
    }

    /// 读取一个 IFD 项的数据（自动跟随越界偏移）。
    fn value_bytes(&self, entry_off: usize) -> Option<Vec<u8>> {
        let ty = self.u16(entry_off + 2)?;
        let count = self.u32(entry_off + 4)? as usize;
        let unit = match ty {
            1 | 2 | 6 | 7 => 1usize,
            3 | 8 => 2,
            4 | 9 | 11 => 4,
            5 | 10 | 12 => 8,
            _ => return None,
        };
        let total = unit.checked_mul(count)?;
        if total == 0 || total > 4 * 1024 * 1024 {
            return None;
        }
        if total <= 4 {
            self.b.get(entry_off + 8..entry_off + 8 + total).map(|s| s.to_vec())
        } else {
            let off = self.u32(entry_off + 8)? as usize;
            self.b.get(off..off + total).map(|s| s.to_vec())
        }
    }

    fn ascii(&self, entry_off: usize) -> Option<String> {
        let raw = self.value_bytes(entry_off)?;
        let s: String = raw
            .iter()
            .take_while(|&&c| c != 0)
            .map(|&c| c as char)
            .collect();
        let t = s.trim().to_string();
        if t.is_empty() {
            None
        } else {
            Some(t)
        }
    }

    fn u32_value(&self, entry_off: usize) -> Option<u32> {
        self.u32(entry_off + 8).map(|v| match self.e {
            Endian::Little => v,
            Endian::Big => v,
        })
    }

    /// 三个有理数（GPS 经纬度用）。
    fn rationals3(&self, entry_off: usize) -> Option<[f64; 3]> {
        let raw = self.value_bytes(entry_off)?;
        if raw.len() < 24 {
            return None;
        }
        let mut out = [0f64; 3];
        for (i, slot) in out.iter_mut().enumerate() {
            let base = i * 8;
            let num = match self.e {
                Endian::Little => u32::from_le_bytes([raw[base], raw[base + 1], raw[base + 2], raw[base + 3]]),
                Endian::Big => u32::from_be_bytes([raw[base], raw[base + 1], raw[base + 2], raw[base + 3]]),
            };
            let den = match self.e {
                Endian::Little => u32::from_le_bytes([raw[base + 4], raw[base + 5], raw[base + 6], raw[base + 7]]),
                Endian::Big => u32::from_be_bytes([raw[base + 4], raw[base + 5], raw[base + 6], raw[base + 7]]),
            };
            *slot = if den == 0 { 0.0 } else { num as f64 / den as f64 };
        }
        Some(out)
    }

    /// 遍历一个 IFD，把每个 tag 交给回调。
    fn each_ifd<F: FnMut(u16, usize)>(&self, ifd_off: usize, mut f: F) {
        let Some(count) = self.u16(ifd_off) else { return };
        for i in 0..count as usize {
            let entry = ifd_off + 2 + i * 12;
            let Some(tag) = self.u16(entry) else { return };
            f(tag, entry);
        }
    }
}

fn parse_tiff(b: &[u8]) -> ImageInfo {
    if b.len() < 8 {
        return ImageInfo::default();
    }
    let e = match &b[..2] {
        b"II" => Endian::Little,
        b"MM" => Endian::Big,
        _ => return ImageInfo::default(),
    };
    let t = Tiff { b, e };
    if t.u16(2) != Some(42) {
        return ImageInfo::default();
    }
    let Some(ifd0) = t.u32(4) else {
        return ImageInfo::default();
    };

    let mut info = ImageInfo::default();
    let mut make: Option<String> = None;
    let mut model: Option<String> = None;
    let mut exif_ifd: Option<usize> = None;
    let mut gps_ifd: Option<usize> = None;

    t.each_ifd(ifd0 as usize, |tag, entry| match tag {
        0x0100 => info.width = t.u32_value(entry).map(|v| v as i64),
        0x0101 => info.height = t.u32_value(entry).map(|v| v as i64),
        0x0112 => info.orientation = t.u32_value(entry).map(|v| v as i64),
        0x010F => make = t.ascii(entry),
        0x0110 => model = t.ascii(entry),
        0x8769 => exif_ifd = t.u32_value(entry).map(|v| v as usize),
        0x8825 => gps_ifd = t.u32_value(entry).map(|v| v as usize),
        _ => {}
    });

    if let Some(off) = exif_ifd {
        t.each_ifd(off, |tag, entry| {
            if tag == 0x9003 {
                info.capture_time = t.ascii(entry).and_then(|s| parse_exif_datetime(&s));
            }
        });
    }

    if let Some(off) = gps_ifd {
        let mut lat_ref = 'N';
        let mut lon_ref = 'E';
        let mut lat: Option<[f64; 3]> = None;
        let mut lon: Option<[f64; 3]> = None;
        t.each_ifd(off, |tag, entry| match tag {
            0x0001 => {
                if let Some(s) = t.ascii(entry) {
                    lat_ref = s.chars().next().unwrap_or('N').to_ascii_uppercase();
                }
            }
            0x0002 => lat = t.rationals3(entry),
            0x0003 => {
                if let Some(s) = t.ascii(entry) {
                    lon_ref = s.chars().next().unwrap_or('E').to_ascii_uppercase();
                }
            }
            0x0004 => lon = t.rationals3(entry),
            _ => {}
        });
        if let Some(d) = lat {
            let v = d[0] + d[1] / 60.0 + d[2] / 3600.0;
            info.gps_lat = Some(if lat_ref == 'S' { -v } else { v });
        }
        if let Some(d) = lon {
            let v = d[0] + d[1] / 60.0 + d[2] / 3600.0;
            info.gps_lon = Some(if lon_ref == 'W' { -v } else { v });
        }
    }

    info.camera = match (make, model) {
        (Some(a), Some(b)) => {
            if b.starts_with(a.as_str()) {
                Some(b)
            } else {
                Some(format!("{a} {b}"))
            }
        }
        (Some(a), None) => Some(a),
        (None, Some(b)) => Some(b),
        (None, None) => None,
    };
    info
}

fn tiff_info(b: &[u8]) -> ImageInfo {
    parse_tiff(b)
}

/// `YYYY:MM:DD HH:MM:SS` → Unix 毫秒（按 UTC 解释）。
pub fn parse_exif_datetime(s: &str) -> Option<i64> {
    let bytes = s.as_bytes();
    if bytes.len() < 19 {
        return None;
    }
    let num = |a: usize, b: usize| -> Option<i64> { s.get(a..b)?.parse::<i64>().ok() };
    let (y, mo, d) = (num(0, 4)?, num(5, 7)?, num(8, 10)?);
    let (h, mi, sec) = (num(11, 13)?, num(14, 16)?, num(17, 19)?);
    if !(1..=12).contains(&mo) || !(1..=31).contains(&d) || h > 23 || mi > 59 || sec > 60 {
        return None;
    }
    let days = days_from_civil(y, mo, d);
    let secs = days * 86_400 + h * 3600 + mi * 60 + sec;
    Some(secs * 1000)
}

/// Howard Hinnant 的 civil→days 算法（纯整数，无时区/闰秒坑）。
fn days_from_civil(y: i64, m: i64, d: i64) -> i64 {
    let y = if m <= 2 { y - 1 } else { y };
    let era = if y >= 0 { y } else { y - 399 } / 400;
    let yoe = y - era * 400;
    let mp = (m + 9) % 12;
    let doy = (153 * mp + 2) / 5 + d - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    era * 146_097 + doe - 719_468
}

/// 供 `app::metadata` 判断要不要尝试解析 EXIF。
pub fn is_image_like(path: &Path) -> bool {
    let name = path.file_name().map(|s| s.to_string_lossy().to_string()).unwrap_or_default();
    matches!(
        kind::ext_of(&name).to_ascii_lowercase().as_str(),
        "png" | "jpg" | "jpeg" | "jpe" | "jfif" | "gif" | "bmp" | "webp" | "tif" | "tiff"
    )
}

/// 读文件末尾（给以后可能用到的实现留口；当前未使用，保持 API 收敛不用）。
#[allow(dead_code)]
fn read_tail(path: &Path, bytes: u64) -> std::io::Result<Vec<u8>> {
    let mut file = File::open(path)?;
    let len = file.metadata()?.len();
    let take = len.min(bytes);
    file.seek(SeekFrom::Start(len - take))?;
    let mut buf = vec![0u8; take as usize];
    file.read_exact(&mut buf)?;
    Ok(buf)
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
            .join("exif")
            .join(name);
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    /// 造一个最小 PNG 头。
    fn png(w: u32, h: u32) -> Vec<u8> {
        let mut v = Vec::new();
        v.extend_from_slice(b"\x89PNG\r\n\x1a\n");
        v.extend_from_slice(&13u32.to_be_bytes());
        v.extend_from_slice(b"IHDR");
        v.extend_from_slice(&w.to_be_bytes());
        v.extend_from_slice(&h.to_be_bytes());
        v.extend_from_slice(&[8, 6, 0, 0, 0]);
        v
    }

    /// 造一个最小 JPEG 头（SOI + SOF0 + SOS）。
    fn jpeg(w: u16, h: u16) -> Vec<u8> {
        let mut v = vec![0xFF, 0xD8];
        v.extend_from_slice(&[0xFF, 0xC0, 0x00, 0x11, 0x08]);
        v.extend_from_slice(&h.to_be_bytes());
        v.extend_from_slice(&w.to_be_bytes());
        v.extend_from_slice(&[0x03, 0x01, 0x11, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01]);
        v.extend_from_slice(&[0xFF, 0xDA, 0x00, 0x02]);
        v
    }

    /// 造一个最小 TIFF/EXIF（含 Make/Model/Orientation/DateTimeOriginal/GPS）。
    fn tiff_with_exif() -> Vec<u8> {
        // 小端；布局：头(8) + IFD0 + ExifIFD + GPSIFD + 字符串与有理数数据区
        let mut b: Vec<u8> = Vec::new();
        b.extend_from_slice(b"II");
        b.extend_from_slice(&42u16.to_le_bytes());
        b.extend_from_slice(&8u32.to_le_bytes()); // IFD0 偏移

        // 预留：IFD0 有 7 项 → 2 + 7*12 + 4 字节
        let ifd0_off = 8usize;
        let ifd0_len = 2 + 7 * 12 + 4;
        let exif_off = ifd0_off + ifd0_len; // ExifIFD
        let exif_len = 2 + 1 * 12 + 4;
        let gps_off = exif_off + exif_len; // GPS IFD
        let gps_len = 2 + 4 * 12 + 4;
        let data_off = gps_off + gps_len; // 数据区

        let make = b"LULING\0";
        let model = b"LULING X1\0";
        let dt = b"2024:05:06 07:08:09\0";
        let make_off = data_off;
        let model_off = make_off + make.len();
        let dt_off = model_off + model.len();
        let lat_off = dt_off + dt.len();
        let lon_off = lat_off + 24;
        let lat_ref_off = lon_off + 24;
        let lon_ref_off = lat_ref_off + 2;

        // IFD0
        b.extend_from_slice(&7u16.to_le_bytes());
        let mut entry = |b: &mut Vec<u8>, tag: u16, ty: u16, count: u32, value: u32| {
            b.extend_from_slice(&tag.to_le_bytes());
            b.extend_from_slice(&ty.to_le_bytes());
            b.extend_from_slice(&count.to_le_bytes());
            b.extend_from_slice(&value.to_le_bytes());
        };
        entry(&mut b, 0x010F, 2, make.len() as u32, make_off as u32); // Make
        entry(&mut b, 0x0110, 2, model.len() as u32, model_off as u32); // Model
        entry(&mut b, 0x0112, 3, 1, 6); // Orientation = 6
        entry(&mut b, 0x0100, 4, 1, 4000); // ImageWidth
        entry(&mut b, 0x0101, 4, 1, 3000); // ImageLength
        entry(&mut b, 0x8769, 4, 1, exif_off as u32); // ExifIFD
        entry(&mut b, 0x8825, 4, 1, gps_off as u32); // GPS IFD
        b.extend_from_slice(&0u32.to_le_bytes()); // 下一个 IFD = 0

        // ExifIFD：DateTimeOriginal
        b.extend_from_slice(&1u16.to_le_bytes());
        entry(&mut b, 0x9003, 2, dt.len() as u32, dt_off as u32);
        b.extend_from_slice(&0u32.to_le_bytes());

        // GPS IFD：纬度 ref + 纬度 + 经度 ref + 经度
        let rat = |b: &mut Vec<u8>, off: usize, vals: [(u32, u32); 3]| {
            while b.len() < off {
                b.push(0);
            }
            for (n, d) in vals {
                b.extend_from_slice(&n.to_le_bytes());
                b.extend_from_slice(&d.to_le_bytes());
            }
        };
        b.extend_from_slice(&4u16.to_le_bytes());
        entry(&mut b, 0x0001, 2, 2, lat_ref_off as u32);
        entry(&mut b, 0x0002, 5, 3, lat_off as u32);
        entry(&mut b, 0x0003, 2, 2, lon_ref_off as u32);
        entry(&mut b, 0x0004, 5, 3, lon_off as u32);
        b.extend_from_slice(&0u32.to_le_bytes());

        // 数据区
        b.extend_from_slice(make);
        b.extend_from_slice(model);
        b.extend_from_slice(dt);
        rat(&mut b, lat_off, [(31, 1), (12, 1), (0, 1)]);
        rat(&mut b, lon_off, [(121, 1), (30, 1), (0, 1)]);
        while b.len() < lat_ref_off {
            b.push(0);
        }
        b.extend_from_slice(b"N\0");
        b.extend_from_slice(b"E\0");
        b
    }

    fn jpeg_with_exif() -> Vec<u8> {
        let tiff = tiff_with_exif();
        let mut v = vec![0xFF, 0xD8];
        let seg_len = (tiff.len() + 2 + 6) as u16;
        v.extend_from_slice(&[0xFF, 0xE1]);
        v.extend_from_slice(&seg_len.to_be_bytes());
        v.extend_from_slice(b"Exif\0\0");
        v.extend_from_slice(&tiff);
        // 再放一个 SOF0
        v.extend_from_slice(&[0xFF, 0xC0, 0x00, 0x11, 0x08]);
        v.extend_from_slice(&3000u16.to_be_bytes());
        v.extend_from_slice(&4000u16.to_be_bytes());
        v.extend_from_slice(&[0x03, 0x01, 0x11, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01]);
        v.extend_from_slice(&[0xFF, 0xDA, 0x00, 0x02]);
        v
    }

    #[test]
    fn png尺寸() {
        let d = dir("png");
        let p = d.join("a.png");
        fs::write(&p, png(1920, 1080)).unwrap();
        let info = read_image_info(&p);
        assert_eq!((info.width, info.height), (Some(1920), Some(1080)));
        assert_eq!(info.capture_time, None);
    }

    #[test]
    fn jpeg尺寸() {
        let d = dir("jpeg");
        let p = d.join("a.jpg");
        fs::write(&p, jpeg(640, 480)).unwrap();
        let info = read_image_info(&p);
        assert_eq!((info.width, info.height), (Some(640), Some(480)));
    }

    #[test]
    fn jpeg里的exif全字段() {
        let d = dir("jpg-exif");
        let p = d.join("a.jpg");
        fs::write(&p, jpeg_with_exif()).unwrap();
        let info = read_image_info(&p);
        assert_eq!((info.width, info.height), (Some(4000), Some(3000)));
        assert_eq!(info.orientation, Some(6));
        assert_eq!(info.camera.as_deref(), Some("LULING X1"));
        // 2024-05-06 07:08:09 UTC
        assert_eq!(info.capture_time, Some(1714979289000));
        let lat = info.gps_lat.unwrap();
        let lon = info.gps_lon.unwrap();
        assert!((lat - (31.0 + 12.0 / 60.0)).abs() < 1e-6, "纬度={lat}");
        assert!((lon - (121.0 + 30.0 / 60.0)).abs() < 1e-6, "经度={lon}");
    }

    #[test]
    fn tiff直接解析() {
        let d = dir("tiff");
        let p = d.join("a.tiff");
        fs::write(&p, tiff_with_exif()).unwrap();
        let info = read_image_info(&p);
        assert_eq!(info.orientation, Some(6));
        assert_eq!((info.width, info.height), (Some(4000), Some(3000)));
    }

    #[test]
    fn gif_bmp_webp尺寸() {
        let d = dir("others");
        let gif = d.join("a.gif");
        let mut g = b"GIF89a".to_vec();
        g.extend_from_slice(&320u16.to_le_bytes());
        g.extend_from_slice(&240u16.to_le_bytes());
        fs::write(&gif, &g).unwrap();
        assert_eq!(
            (read_image_info(&gif).width, read_image_info(&gif).height),
            (Some(320), Some(240))
        );

        let bmp = d.join("a.bmp");
        let mut bm = b"BM".to_vec();
        bm.resize(18, 0);
        bm.extend_from_slice(&800i32.to_le_bytes());
        bm.extend_from_slice(&600i32.to_le_bytes());
        fs::write(&bmp, &bm).unwrap();
        assert_eq!(
            (read_image_info(&bmp).width, read_image_info(&bmp).height),
            (Some(800), Some(600))
        );

        let webp = d.join("a.webp");
        let mut w = b"RIFF".to_vec();
        w.extend_from_slice(&0u32.to_le_bytes());
        w.extend_from_slice(b"WEBP");
        w.extend_from_slice(b"VP8X");
        w.extend_from_slice(&10u32.to_le_bytes());
        w.extend_from_slice(&[0, 0, 0, 0]); // flags(1) + reserved(3)
        // 宽 1024-1、高 768-1（24 位小端）
        w.extend_from_slice(&[0xFF, 0x03, 0x00]);
        w.extend_from_slice(&[0xFF, 0x02, 0x00]);
        fs::write(&webp, &w).unwrap();
        let info = read_image_info(&webp);
        assert_eq!((info.width, info.height), (Some(1024), Some(768)));
    }

    #[test]
    fn 坏文件与未知格式不panic() {
        let d = dir("broken");
        let p = d.join("a.jpg");
        fs::write(&p, b"\xFF\xD8garbage").unwrap();
        let info = read_image_info(&p);
        assert_eq!(info, ImageInfo::default());

        let q = d.join("a.heic");
        fs::write(&q, vec![0u8; 64]).unwrap();
        assert_eq!(read_image_info(&q), ImageInfo::default());
        assert!(!is_image_like(&q) || is_image_like(&q)); // 不断言语义，只要求不 panic
    }

    #[test]
    fn 时间解析与历法() {
        assert_eq!(parse_exif_datetime("1970:01:01 00:00:00"), Some(0));
        assert_eq!(parse_exif_datetime("2024:05:06 07:08:09"), Some(1714979289000));
        assert_eq!(parse_exif_datetime("2024:13:01 00:00:00"), None);
        assert_eq!(parse_exif_datetime("bad"), None);
    }

    #[test]
    fn 图片类扩展名判定() {
        assert!(is_image_like(Path::new("a.PNG")));
        assert!(is_image_like(Path::new("a.jpeg")));
        assert!(!is_image_like(Path::new("a.mp4")));
    }
}
