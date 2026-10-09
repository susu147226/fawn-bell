//! 基础设施层 · 内容指纹（执行版 §6.4 / §7.12「两级指纹」的第一级）。
//!
//! §7.12：**扫描时**对每个文件只读取**首尾各 64 KB** 算分段哈希入库，默认执行；
//! 只有分段哈希相同的候选组才继续做「全长内容比对」（第二级，见 `app::dedupe`）。
//! 这样整盘扫描不会为了去重而全量读盘。
//!
//! 指纹串形如 `b3p1:<首段16位>-<尾段16位>`；同体积不同内容几乎不可能同指纹，
//! 而最终判定仍是**逐字节全长比对**，指纹只做候选筛选。

use std::fs::File;
use std::io::{self, Read, Seek, SeekFrom};
use std::path::Path;

/// 分段大小：64 KB（§6.4 / §7.12 明文规定，不可配置）。
pub const CHUNK: u64 = 64 * 1024;

/// 指纹版本前缀：算法换了要让旧指纹失效并重算。
pub const PREFIX: &str = "b3p1";

fn hex8(bytes: &[u8]) -> String {
    bytes.iter().take(8).map(|b| format!("{b:02x}")).collect()
}

/// 计算一个文件的首尾分段哈希。
///
/// - 文件 ≤ 64 KB：首段即全文，尾段同值（此时指纹等价于全文哈希）；
/// - 文件 > 64 KB：尾段从 `size - 64 KB` 起读 64 KB。
pub fn partial_hash(path: &Path) -> io::Result<String> {
    let mut file = File::open(path)?;
    let size = file.metadata()?.len();

    let head_len = CHUNK.min(size);
    let mut head = vec![0u8; head_len as usize];
    file.read_exact(&mut head)?;
    let head_sum = blake3::hash(&head);

    let tail_sum = if size <= CHUNK {
        head_sum
    } else {
        file.seek(SeekFrom::Start(size - CHUNK))?;
        let mut tail = vec![0u8; CHUNK as usize];
        file.read_exact(&mut tail)?;
        blake3::hash(&tail)
    };

    Ok(format!(
        "{PREFIX}:{}-{}",
        hex8(head_sum.as_bytes()),
        hex8(tail_sum.as_bytes())
    ))
}

/// 指纹算法是否与服务端一致（旧库升级时用来判断要不要重算）。
pub fn is_current(fingerprint: &str) -> bool {
    fingerprint.starts_with(&format!("{PREFIX}:"))
}

/// 第二级：逐字节比较两个文件是否完全相同（§7.12 的最终判定）。
///
/// 不做整文件哈希，避免为了比对再读两遍盘：分块读、边读边比，遇到差异立即返回。
pub fn same_content(a: &Path, b: &Path) -> io::Result<bool> {
    let (mut fa, mut fb) = (File::open(a)?, File::open(b)?);
    let (la, lb) = (fa.metadata()?.len(), fb.metadata()?.len());
    if la != lb {
        return Ok(false);
    }
    let mut buf_a = vec![0u8; 256 * 1024];
    let mut buf_b = vec![0u8; 256 * 1024];
    loop {
        let n = fa.read(&mut buf_a)?;
        if n == 0 {
            return Ok(true);
        }
        fb.read_exact(&mut buf_b[..n])?;
        if buf_a[..n] != buf_b[..n] {
            return Ok(false);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::path::PathBuf;

    fn fixture(name: &str) -> PathBuf {
        let dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("target")
            .join("luling-tests")
            .join("hash")
            .join(name);
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).expect("建测试目录");
        dir
    }

    fn write(path: &Path, bytes: &[u8]) {
        fs::write(path, bytes).expect("写文件");
    }

    #[test]
    fn 小文件首尾同值且确定() {
        let dir = fixture("小文件");
        let p = dir.join("a.bin");
        write(&p, b"hello luling");
        let h1 = partial_hash(&p).unwrap();
        let h2 = partial_hash(&p).unwrap();
        assert_eq!(h1, h2);
        assert!(h1.starts_with("b3p1:"));
        let (head, tail) = h1.trim_start_matches("b3p1:").split_once('-').unwrap();
        assert_eq!(head, tail, "≤64KB 的文件首尾段应是同一段");
        assert_eq!(head.len(), 16);
    }

    #[test]
    fn 大于64KB时首尾分段都参与指纹() {
        let dir = fixture("大文件");
        // 三倍分段：首段 0..64K、中段 64K..128K（不参与指纹）、尾段 128K..192K
        let mut bytes = vec![7u8; (CHUNK * 3) as usize];
        let p1 = dir.join("a.bin");
        write(&p1, &bytes);
        let h1 = partial_hash(&p1).unwrap();

        // 只改中段（不影响首尾 64 KB）→ 指纹不变（这正是两级指纹要的效果）
        bytes[(CHUNK + 100) as usize] = 9;
        let p2 = dir.join("b.bin");
        write(&p2, &bytes);
        assert_eq!(h1, partial_hash(&p2).unwrap());

        // 改尾部 → 指纹变化
        let mut tail = vec![7u8; (CHUNK * 3) as usize];
        let last = tail.len() - 5;
        tail[last] = 1;
        let p3 = dir.join("c.bin");
        write(&p3, &tail);
        assert_ne!(h1, partial_hash(&p3).unwrap());
    }

    #[test]
    fn 同体积不同内容指纹不同() {
        let dir = fixture("同体积");
        let a = dir.join("a.bin");
        let b = dir.join("b.bin");
        write(&a, b"AAAAAAAA");
        write(&b, b"AAAAAAAB");
        assert_eq!(fs::metadata(&a).unwrap().len(), fs::metadata(&b).unwrap().len());
        assert_ne!(partial_hash(&a).unwrap(), partial_hash(&b).unwrap());
    }

    #[test]
    fn 空文件也能算指纹() {
        let dir = fixture("空文件");
        let p = dir.join("empty.bin");
        write(&p, b"");
        let h = partial_hash(&p).unwrap();
        assert!(h.starts_with("b3p1:"));
    }

    #[test]
    fn 指纹版本可判定() {
        assert!(is_current("b3p1:0011223344556677-8899aabbccddeeff"));
        assert!(!is_current("md5:deadbeef"));
    }

    #[test]
    fn 全长比对只在内容一致时为真() {
        let dir = fixture("全长比对");
        let a = dir.join("a.bin");
        let b = dir.join("b.bin");
        let c = dir.join("c.bin");
        let big = vec![3u8; 300 * 1024];
        write(&a, &big);
        write(&b, &big);
        let mut other = big.clone();
        other[250 * 1024] = 4; // 差异落在中段（首尾指纹看不出来）
        write(&c, &other);

        assert!(same_content(&a, &b).unwrap());
        assert!(!same_content(&a, &c).unwrap(), "中段不同必须被判为不同");

        // 体积不同直接 false
        write(&c, b"short");
        assert!(!same_content(&a, &c).unwrap());
    }
}
