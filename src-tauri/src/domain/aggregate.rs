//! 核心域 · 目录聚合（纯逻辑，无 IO）。
//!
//! §8.5 要求文件夹行**无需进入即可见**六项信息，其中「项数 / 总大小 / 整理进度」必须是聚合值，
//! 不得为渲染每行实时统计磁盘。P0 无索引库，聚合在扫描结果之上一次算清（P1 起改由 SQL 聚合）。

use std::collections::HashMap;

use super::kind::{Kind, ALL};

/// 扫描得到的目录（每个被访问到的目录一条，含空目录）。
#[derive(Debug, Clone)]
pub struct DirMeta {
    /// 相对工作根的路径，用 `/` 分隔；工作根自身为空串。
    pub rel_path: String,
    pub mtime_ms: i64,
    pub cloud: bool,
}

/// 扫描得到的文件。
#[derive(Debug, Clone, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct FileMeta {
    pub rel_path: String,
    pub name: String,
    pub kind: Kind,
    pub size: u64,
    pub mtime_ms: i64,
    /// 创建时间。§7.1 的增量判定看「体积 + 修改时间 + 创建时间」三项，缺一不可。
    pub ctime_ms: i64,
    pub cloud: bool,
}

/// 目录聚合行（供中栏文件夹行与左栏目录树使用）。
#[derive(Debug, Clone, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DirAgg {
    pub rel_path: String,
    pub name: String,
    /// 直接子文件数（§8.5 ①：子文件夹另行汇总）。
    pub direct_files: u32,
    /// 直接子文件夹数。
    pub direct_dirs: u32,
    /// 后代文件总数（含直接子文件）。
    pub total_files: u32,
    /// 后代总字节数。
    pub total_bytes: u64,
    pub mtime_ms: i64,
    pub cloud: bool,
    /// 目录内各类型分布（按 [`ALL`] 顺序稳定输出，含 0 项）。
    pub kind_counts: Vec<KindCount>,
}

#[derive(Debug, Clone, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct KindCount {
    pub kind: Kind,
    pub count: u32,
    pub bytes: u64,
}

/// 取父目录相对路径；顶层项的父目录是工作根（空串）。
pub fn parent_of(rel_path: &str) -> &str {
    match rel_path.rfind('/') {
        Some(i) => &rel_path[..i],
        None => "",
    }
}

/// 取末段名。
pub fn leaf_of(rel_path: &str) -> &str {
    match rel_path.rfind('/') {
        Some(i) => &rel_path[i + 1..],
        None => rel_path,
    }
}

/// 由「工作根 + 全部目录 + 全部文件」算出每个目录的聚合行。
///
/// 输出顺序：工作根在前，其余按输入目录顺序（前端排序自理）。
pub fn build_dirs(root: &DirMeta, dirs: &[DirMeta], files: &[FileMeta]) -> Vec<DirAgg> {
    fn push_dir<'a>(
        out: &mut Vec<DirAgg>,
        index: &mut HashMap<&'a str, usize>,
        meta: &'a DirMeta,
    ) {
        let i = out.len();
        out.push(DirAgg {
            rel_path: meta.rel_path.clone(),
            name: if meta.rel_path.is_empty() {
                String::new()
            } else {
                leaf_of(&meta.rel_path).to_string()
            },
            direct_files: 0,
            direct_dirs: 0,
            total_files: 0,
            total_bytes: 0,
            mtime_ms: meta.mtime_ms,
            cloud: meta.cloud,
            kind_counts: ALL
                .iter()
                .map(|k| KindCount {
                    kind: *k,
                    count: 0,
                    bytes: 0,
                })
                .collect(),
        });
        index.insert(meta.rel_path.as_str(), i);
    }

    let mut out: Vec<DirAgg> = Vec::with_capacity(dirs.len() + 1);
    let mut index: HashMap<&str, usize> = HashMap::with_capacity(dirs.len() + 1);

    push_dir(&mut out, &mut index, root);
    for d in dirs {
        push_dir(&mut out, &mut index, d);
    }

    // 直接子项计数（含子文件夹数）
    for d in dirs {
        if let Some(&p) = index.get(parent_of(&d.rel_path)) {
            out[p].direct_dirs += 1;
        }
    }
    for f in files {
        if let Some(&p) = index.get(parent_of(&f.rel_path)) {
            out[p].direct_files += 1;
        }
    }

    // 后代汇总：每个文件向所有祖先（含工作根）累计计数、字节与类型分布
    for f in files {
        let mut cur = parent_of(&f.rel_path);
        loop {
            if let Some(&i) = index.get(cur) {
                out[i].total_files += 1;
                out[i].total_bytes += f.size;
                if let Some(kc) = out[i]
                    .kind_counts
                    .iter_mut()
                    .find(|kc| kc.kind == f.kind)
                {
                    kc.count += 1;
                    kc.bytes += f.size;
                }
            }
            if cur.is_empty() {
                break;
            }
            cur = parent_of(cur);
        }
    }

    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn dir(rel: &str) -> DirMeta {
        DirMeta {
            rel_path: rel.to_string(),
            mtime_ms: 0,
            cloud: false,
        }
    }

    fn file(rel: &str, size: u64, kind: Kind) -> FileMeta {
        FileMeta {
            rel_path: rel.to_string(),
            name: leaf_of(rel).to_string(),
            kind,
            size,
            mtime_ms: 0,
            ctime_ms: 0,
            cloud: false,
        }
    }

    #[test]
    fn 路径取段() {
        assert_eq!(parent_of("a"), "");
        assert_eq!(parent_of("a/b"), "a");
        assert_eq!(parent_of("a/b/c"), "a/b");
        assert_eq!(leaf_of("a/b/c.jpg"), "c.jpg");
        assert_eq!(leaf_of("c.jpg"), "c.jpg");
    }

    #[test]
    fn 直接项与后代汇总分开() {
        let root = dir("");
        let dirs = vec![dir("子"), dir("子/孙")];
        let files = vec![
            file("顶层.jpg", 10, Kind::Image),
            file("子/b.mkv", 20, Kind::Video),
            file("子/孙/c.wav", 30, Kind::Audio),
        ];
        let aggs = build_dirs(&root, &dirs, &files);
        let at = |rel: &str| aggs.iter().find(|a| a.rel_path == rel).unwrap();

        assert_eq!(at("").direct_files, 1);
        assert_eq!(at("").direct_dirs, 1);
        assert_eq!(at("").total_files, 3);
        assert_eq!(at("").total_bytes, 60);
        assert_eq!(at("子").direct_files, 1);
        assert_eq!(at("子").direct_dirs, 1);
        assert_eq!(at("子").total_files, 2);
        assert_eq!(at("子").total_bytes, 50);
        assert_eq!(at("子/孙").total_files, 1);
        assert_eq!(at("子/孙").total_bytes, 30);

        let img = at("").kind_counts.iter().find(|k| k.kind == Kind::Image).unwrap();
        assert_eq!(img.count, 1);
        assert_eq!(img.bytes, 10);
        assert_eq!(at("").kind_counts.len(), 6);
    }

    #[test]
    fn 空目录只算直接子文件夹() {
        let root = dir("");
        let dirs = vec![dir("空A"), dir("空B")];
        let aggs = build_dirs(&root, &dirs, &[]);
        assert_eq!(aggs.len(), 3);
        assert_eq!(aggs[0].direct_dirs, 2);
        assert_eq!(aggs[0].total_files, 0);
        assert_eq!(aggs[1].name, "空A");
        assert_eq!(aggs[0].name, "");
    }
}
