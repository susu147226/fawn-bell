//! 基础设施层 · 极简 PNG 编码 与 base64（执行版 §12.4：不引入多余依赖）。
//!
//! 缩略图只需要「把 BGRA 位图变成一个前端能直接显示的图片」。与其为了编码引入图像库
//! （体积、许可证、离线约束三重成本），不如自己写一个**只用 stored（不压缩）deflate 块**
//! 的 PNG 编码器：结构简单、结果完全合法，任何解码器都能读。
//!
//! 代价是文件比压缩过的大一些；缩略图尺寸小（默认 256×256），且按 §10 有容量与 LRU 上限。

/// 把 RGB（每像素 3 字节，行优先）编码成 PNG。
pub fn encode_rgb(width: u32, height: u32, rgb: &[u8]) -> Vec<u8> {
    let stride = width as usize * 3;
    debug_assert_eq!(rgb.len(), stride * height as usize, "RGB 数据长度与尺寸不匹配");

    let mut raw = Vec::with_capacity((stride + 1) * height as usize);
    for y in 0..height as usize {
        raw.push(0u8); // 每行的过滤器字节：0 = None
        let start = y * stride;
        raw.extend_from_slice(&rgb[start..start + stride]);
    }

    let mut out = Vec::new();
    out.extend_from_slice(&[0x89, b'P', b'N', b'G', 0x0D, 0x0A, 0x1A, 0x0A]);
    let mut ihdr = Vec::with_capacity(13);
    ihdr.extend_from_slice(&width.to_be_bytes());
    ihdr.extend_from_slice(&height.to_be_bytes());
    ihdr.extend_from_slice(&[8, 2, 0, 0, 0]); // 位深 8、真彩色 RGB、无隔行
    chunk(&mut out, b"IHDR", &ihdr);
    chunk(&mut out, b"IDAT", &zlib_stored(&raw));
    chunk(&mut out, b"IEND", &[]);
    out
}

/// 把 BGRA（Windows 位图的常见布局）转成 RGB 并编码为 PNG（alpha 直接丢弃：缩略图底为白）。
pub fn encode_bgra(width: u32, height: u32, bgra: &[u8]) -> Vec<u8> {
    let mut rgb = Vec::with_capacity(width as usize * height as usize * 3);
    for px in bgra.chunks_exact(4) {
        rgb.push(px[2]);
        rgb.push(px[1]);
        rgb.push(px[0]);
    }
    encode_rgb(width, height, &rgb)
}

fn chunk(out: &mut Vec<u8>, kind: &[u8; 4], data: &[u8]) {
    out.extend_from_slice(&(data.len() as u32).to_be_bytes());
    out.extend_from_slice(kind);
    out.extend_from_slice(data);
    let mut crc = Crc32::new();
    crc.update(kind);
    crc.update(data);
    out.extend_from_slice(&crc.finish().to_be_bytes());
}

/// zlib 容器 + stored deflate 块（不压缩，但与任何 zlib 解码器兼容）。
fn zlib_stored(data: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(data.len() + 32);
    out.push(0x78); // CMF：deflate、32K 窗口
    out.push(0x01); // FLG：无字典、最低压缩级别
    let mut i = 0usize;
    if data.is_empty() {
        out.push(1);
        out.extend_from_slice(&0u16.to_le_bytes());
        out.extend_from_slice(&0xFFFFu16.to_le_bytes());
    }
    while i < data.len() {
        let n = (data.len() - i).min(65_535);
        let last = i + n >= data.len();
        out.push(if last { 1 } else { 0 });
        out.extend_from_slice(&(n as u16).to_le_bytes());
        out.extend_from_slice(&(!(n as u16)).to_le_bytes());
        out.extend_from_slice(&data[i..i + n]);
        i += n;
    }
    // Adler-32
    let (mut a, mut b) = (1u32, 0u32);
    for &byte in data {
        a = (a + byte as u32) % 65_521;
        b = (b + a) % 65_521;
    }
    out.extend_from_slice(&((b << 16) | a).to_be_bytes());
    out
}

struct Crc32(u32);

impl Crc32 {
    fn new() -> Self {
        Crc32(0xFFFF_FFFF)
    }
    fn update(&mut self, data: &[u8]) {
        for &b in data {
            self.0 ^= b as u32;
            for _ in 0..8 {
                let mask = (self.0 & 1).wrapping_neg();
                self.0 = (self.0 >> 1) ^ (0xEDB8_8320 & mask);
            }
        }
    }
    fn finish(self) -> u32 {
        !self.0
    }
}

const B64: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// 标准 base64（缩略图以 `data:` URL 交给前端显示，避免额外协议注册）。
pub fn base64(data: &[u8]) -> String {
    let mut out = String::with_capacity((data.len() + 2) / 3 * 4);
    for chunk in data.chunks(3) {
        let b0 = chunk[0] as u32;
        let b1 = *chunk.get(1).unwrap_or(&0) as u32;
        let b2 = *chunk.get(2).unwrap_or(&0) as u32;
        let n = (b0 << 16) | (b1 << 8) | b2;
        out.push(B64[(n >> 18) as usize & 63] as char);
        out.push(B64[(n >> 12) as usize & 63] as char);
        out.push(if chunk.len() > 1 {
            B64[(n >> 6) as usize & 63] as char
        } else {
            '='
        });
        out.push(if chunk.len() > 2 {
            B64[n as usize & 63] as char
        } else {
            '='
        });
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn png结构合法() {
        let rgb = vec![255u8, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255]; // 2×2
        let png = encode_rgb(2, 2, &rgb);
        assert_eq!(&png[..8], &[0x89, b'P', b'N', b'G', 0x0D, 0x0A, 0x1A, 0x0A]);
        // 第一块必须是 IHDR，且宽高正确
        assert_eq!(&png[12..16], b"IHDR");
        assert_eq!(u32::from_be_bytes([png[16], png[17], png[18], png[19]]), 2);
        assert_eq!(u32::from_be_bytes([png[20], png[21], png[22], png[23]]), 2);
        assert_eq!(png[24], 8, "位深");
        assert_eq!(png[25], 2, "真彩色");
        assert_eq!(&png[png.len() - 8..png.len() - 4], b"IEND");
    }

    #[test]
    fn crc32标准向量() {
        let mut c = Crc32::new();
        c.update(b"123456789");
        assert_eq!(c.finish(), 0xCBF4_3926, "CRC-32/ISO-HDLC 标准向量");
    }

    #[test]
    fn bgra转rgb() {
        let bgra = vec![3u8, 2, 1, 255]; // B=3 G=2 R=1 A=255
        let rgb = encode_bgra(1, 1, &bgra);
        // IDAT 里能找到 R G B 顺序的三个字节
        let pos = rgb.windows(3).position(|w| w == [1u8, 2, 3]).expect("应有 RGB 三元组");
        assert!(pos > 0);
    }

    #[test]
    fn base64标准向量() {
        assert_eq!(base64(b""), "");
        assert_eq!(base64(b"f"), "Zg==");
        assert_eq!(base64(b"fo"), "Zm8=");
        assert_eq!(base64(b"foo"), "Zm9v");
        assert_eq!(base64(b"foob"), "Zm9vYg==");
        assert_eq!(base64(b"fooba"), "Zm9vYmE=");
        assert_eq!(base64(b"foobar"), "Zm9vYmFy");
    }

    #[test]
    fn 空数据也能编码() {
        let png = encode_rgb(1, 1, &[0, 0, 0]);
        assert!(png.len() > 40);
    }
}
