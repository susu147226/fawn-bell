//! 基础设施层 · 只读目录遍历（执行版 §6.2 / §7.1 / §10）。
//!
//! 硬性约定：
//! - **只读**：全流程只调用 `read_dir` / `metadata`，不创建、不修改、不删除任何东西（§7.1、§14①）。
//! - **默认不跟随符号链接与 junction**（§6.2、§14⑤）：靠 `FILE_ATTRIBUTE_REPARSE_POINT` 判定，
//!   开启跟随时用规范化路径去重，避免自环。
//! - **可取消**：每处理一个目录项检查一次取消标志，取消即返回（§7.1）。
//! - **深路径**：Rust 标准库在 Windows 上对超长路径内部走 verbatim 前缀，无需自行拼 `\\?\`。
//! - P0 不跟随链接时不做任何递归深度保护以外的限制；`max_depth` 为 §10 的可配项（默认不限）。

use std::collections::HashSet;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::UNIX_EPOCH;

use crate::domain::aggregate::{DirMeta, FileMeta};
use crate::domain::guard::norm;
use crate::domain::{ignore, kind};

#[cfg(windows)]
const FILE_ATTRIBUTE_HIDDEN: u32 = 0x2;
#[cfg(windows)]
const FILE_ATTRIBUTE_REPARSE_POINT: u32 = 0x400;
#[cfg(windows)]
const FILE_ATTRIBUTE_OFFLINE: u32 = 0x1000;
#[cfg(windows)]
const FILE_ATTRIBUTE_RECALL_ON_OPEN: u32 = 0x40000;
#[cfg(windows)]
const FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS: u32 = 0x400000;

#[derive(Debug, Clone)]
pub struct WalkOptions {
    /// §10 默认「否」；开启时用规范化路径去重防自环。
    pub follow_links: bool,
    /// §10 默认「不限」。
    pub max_depth: Option<usize>,
    /// 强制排除的子树（§6.2 应用数据目录）。
    pub excluded: Vec<PathBuf>,
    /// §10 单次扫描文件数上限（默认 200,000）。
    pub file_limit: u64,
}

impl Default for WalkOptions {
    fn default() -> Self {
        Self {
            follow_links: false,
            max_depth: None,
            excluded: Vec::new(),
            file_limit: 200_000,
        }
    }
}

/// 遍历过程中的事件（由 [`walk`] 流入调用方的 sink）。
pub enum WalkItem {
    /// 发现并准备处理一个目录。
    DirEntered { meta: DirMeta, depth: usize },
    /// 一个目录的直接子项已数清（用于进度与 ETA 的「已完成目录」计数）。
    DirFinished {
        rel_path: String,
        direct_files: u32,
        direct_dirs: u32,
    },
    File(FileMeta),
}

#[derive(Debug, Default)]
pub struct WalkOutcome {
    pub file_count: u64,
    pub dir_count: u64,
    pub total_bytes: u64,
    pub cancelled: bool,
    /// 达到单次扫描文件数上限而提前结束（§10）。
    pub truncated: bool,
    pub warnings: Vec<String>,
    /// 「单目录条目数预警」（§10，默认 5,000）：(目录, 直接条目数)
    pub big_dirs: Vec<(String, u32)>,
}

fn attrs_of(md: &fs::Metadata) -> u32 {
    #[cfg(windows)]
    {
        use std::os::windows::fs::MetadataExt;
        md.file_attributes()
    }
    #[cfg(not(windows))]
    {
        let _ = md;
        0
    }
}

fn mtime_ms(md: &fs::Metadata) -> i64 {
    md.modified()
        .ok()
        .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
        .map(|d| d.as_millis() as i64)
        .unwrap_or(0)
}

/// 创建时间（§7.1 增量判定三项之一；取不到记 0，此时退化为「体积 + 修改时间」判定）。
fn ctime_ms(md: &fs::Metadata) -> i64 {
    md.created()
        .ok()
        .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
        .map(|d| d.as_millis() as i64)
        .unwrap_or(0)
}

fn is_cloud(attrs: u32) -> bool {
    attrs
        & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)
        != 0
}

/// 目录条目数预警阈值（§10「单目录条目数预警 5,000」）。
pub const BIG_DIR_THRESHOLD: u32 = 5_000;

/// 递归遍历 `root`（不含 root 自身之外的任何写入）。
pub fn walk<F>(
    root: &Path,
    opts: &WalkOptions,
    cancel: &AtomicBool,
    mut sink: F,
) -> io::Result<WalkOutcome>
where
    F: FnMut(WalkItem),
{
    let root_md = fs::metadata(root)?;
    if !root_md.is_dir() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "所选路径不是文件夹",
        ));
    }

    let root_meta = DirMeta {
        rel_path: String::new(),
        mtime_ms: mtime_ms(&root_md),
        cloud: is_cloud(attrs_of(&root_md)),
    };
    sink(WalkItem::DirEntered {
        meta: root_meta.clone(),
        depth: 0,
    });

    let mut out = WalkOutcome::default();
    let mut stack: Vec<(PathBuf, String, usize)> = vec![(root.to_path_buf(), String::new(), 0)];
    let mut visited: HashSet<String> = HashSet::new();
    if opts.follow_links {
        visited.insert(norm(root));
    }

    while let Some((dir_path, dir_rel, depth)) = stack.pop() {
        if cancel.load(Ordering::Relaxed) {
            out.cancelled = true;
            break;
        }

        let reader = match fs::read_dir(&dir_path) {
            Ok(r) => r,
            Err(e) => {
                out.warnings.push(format!(
                    "无法读取文件夹 {}：{}",
                    dir_path.to_string_lossy(),
                    e
                ));
                sink(WalkItem::DirFinished {
                    rel_path: dir_rel,
                    direct_files: 0,
                    direct_dirs: 0,
                });
                continue;
            }
        };

        let mut direct_files: u32 = 0;
        let mut direct_dirs: u32 = 0;
        let mut hit_limit = false;

        for entry in reader {
            if cancel.load(Ordering::Relaxed) {
                out.cancelled = true;
                break;
            }
            let entry = match entry {
                Ok(e) => e,
                Err(e) => {
                    out.warnings.push(format!("读取条目失败：{e}"));
                    continue;
                }
            };

            let name = entry.file_name().to_string_lossy().to_string();
            let md = match entry.metadata() {
                Ok(m) => m,
                Err(_) => match fs::symlink_metadata(entry.path()) {
                    Ok(m) => m,
                    Err(e) => {
                        out.warnings.push(format!("读取元数据失败 {}：{}", name, e));
                        continue;
                    }
                },
            };

            let attrs = attrs_of(&md);
            if ignore::is_ignored(&name, attrs & FILE_ATTRIBUTE_HIDDEN != 0) {
                continue;
            }
            let reparse = attrs & FILE_ATTRIBUTE_REPARSE_POINT != 0;
            if reparse && !opts.follow_links {
                // §6.2 / §14⑤：默认不跟随符号链接与 junction
                continue;
            }
            let target = if reparse {
                fs::metadata(entry.path()).unwrap_or(md)
            } else {
                md
            };

            let child_rel = if dir_rel.is_empty() {
                name.clone()
            } else {
                format!("{dir_rel}/{name}")
            };
            let cloud = is_cloud(attrs);

            if target.is_dir() {
                if opts
                    .excluded
                    .iter()
                    .any(|ex| norm(ex) == norm(&entry.path()))
                {
                    // §6.2：排除应用数据目录，不可关闭
                    continue;
                }
                if reparse {
                    let key = fs::canonicalize(entry.path())
                        .map(|p| norm(&p))
                        .unwrap_or_else(|_| norm(&entry.path()));
                    if !visited.insert(key) {
                        continue;
                    }
                }
                direct_dirs += 1;
                out.dir_count += 1;
                sink(WalkItem::DirEntered {
                    meta: DirMeta {
                        rel_path: child_rel.clone(),
                        mtime_ms: mtime_ms(&target),
                        cloud,
                    },
                    depth: depth + 1,
                });
                let too_deep = opts.max_depth.map(|m| depth + 1 > m).unwrap_or(false);
                if too_deep {
                    // 只登记这一层，不再下钻
                    sink(WalkItem::DirFinished {
                        rel_path: child_rel,
                        direct_files: 0,
                        direct_dirs: 0,
                    });
                } else {
                    stack.push((entry.path(), child_rel, depth + 1));
                }
            } else if target.is_file() {
                if out.file_count >= opts.file_limit {
                    out.truncated = true;
                    hit_limit = true;
                    break;
                }
                let size = target.len();
                direct_files += 1;
                out.file_count += 1;
                out.total_bytes += size;
                sink(WalkItem::File(FileMeta {
                    name: name.clone(),
                    kind: kind::Kind::of_ext(&kind::ext_of(&name)),
                    rel_path: child_rel,
                    size,
                    mtime_ms: mtime_ms(&target),
                    ctime_ms: ctime_ms(&target),
                    cloud,
                }));
            }
        }

        if direct_files + direct_dirs >= BIG_DIR_THRESHOLD {
            out.big_dirs
                .push((dir_rel.clone(), direct_files + direct_dirs));
        }
        sink(WalkItem::DirFinished {
            rel_path: dir_rel,
            direct_files,
            direct_dirs,
        });

        if hit_limit {
            out.warnings.push(format!(
                "已达单次扫描文件数上限 {}，本次扫描提前结束（执行版 §10）。",
                opts.file_limit
            ));
            break;
        }
        if out.cancelled {
            break;
        }
    }

    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// 测试夹具根目录：放在 crate 自己的 `target/luling-tests/` 下。
    ///
    /// 不用 `std::env::temp_dir()`：本机受管环境里 cargo 的测试子进程对 `%TEMP%`
    /// 的写入会被拒绝（`Os { code: 5, kind: PermissionDenied }`，而 pwsh 自身可写），
    /// 夹具放 target 下既保证可写，也绝不触碰任何素材树（§14①）。
    fn fixture_root(name: &str) -> PathBuf {
        let base = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("target")
            .join("luling-tests")
            .join(name);
        let _ = fs::remove_dir_all(&base);
        base
    }

    #[test]
    fn 遍历临时目录并忽略默认集() {
        let base = fixture_root("walk-basic");
        fs::create_dir_all(base.join("子目录")).unwrap();
        fs::write(base.join("a.jpg"), b"12345").unwrap();
        fs::write(base.join("b.mkv"), b"1234").unwrap();
        fs::write(base.join("Thumbs.db"), b"x").unwrap();
        fs::write(base.join(".hidden"), b"x").unwrap();
        fs::write(base.join("t.tmp"), b"x").unwrap();
        fs::write(base.join("子目录").join("c.flac"), b"123456").unwrap();

        let cancel = AtomicBool::new(false);
        let mut dirs = Vec::new();
        let mut files = Vec::new();
        let outcome = walk(&base, &WalkOptions::default(), &cancel, |item| match item {
            WalkItem::DirEntered { meta, .. } => dirs.push(meta),
            WalkItem::File(f) => files.push(f),
            WalkItem::DirFinished { .. } => {}
        })
        .unwrap();

        assert!(!outcome.cancelled);
        assert_eq!(outcome.file_count, 3);
        assert_eq!(outcome.total_bytes, 5 + 4 + 6);
        assert_eq!(dirs.len(), 2, "根 + 子目录");
        let mut names: Vec<String> = files.iter().map(|f| f.name.clone()).collect();
        names.sort();
        assert_eq!(names, vec!["a.jpg", "b.mkv", "c.flac"]);
        assert!(files
            .iter()
            .any(|f| f.rel_path == "子目录/c.flac" && f.kind == kind::Kind::Audio));

        let _ = fs::remove_dir_all(&base);
    }

    #[test]
    fn 取消立刻返回() {
        let base = fixture_root("walk-cancel");
        fs::create_dir_all(&base).unwrap();
        fs::write(base.join("a.txt"), b"x").unwrap();

        let cancel = AtomicBool::new(true);
        let outcome = walk(&base, &WalkOptions::default(), &cancel, |_| {}).unwrap();
        assert!(outcome.cancelled);
        let _ = fs::remove_dir_all(&base);
    }

    #[test]
    fn 排除子树() {
        let base = fixture_root("walk-exclude");
        fs::create_dir_all(base.join("保留")).unwrap();
        fs::create_dir_all(base.join("排除")).unwrap();
        fs::write(base.join("保留").join("keep.jpg"), b"1").unwrap();
        fs::write(base.join("排除").join("drop.jpg"), b"1").unwrap();

        let opts = WalkOptions {
            excluded: vec![base.join("排除")],
            ..Default::default()
        };
        let cancel = AtomicBool::new(false);
        let mut files = Vec::new();
        walk(&base, &opts, &cancel, |item| {
            if let WalkItem::File(f) = item {
                files.push(f);
            }
        })
        .unwrap();
        assert_eq!(files.len(), 1);
        assert_eq!(files[0].rel_path, "保留/keep.jpg");
        let _ = fs::remove_dir_all(&base);
    }
}
