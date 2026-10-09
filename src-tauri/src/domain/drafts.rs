//! 核心域 · 虚拟变更集（执行版 §7.2，**纯逻辑、无 IO**）。
//!
//! 条文：软件内对文件与文件夹的一切修改动作**都不立即作用于真实文件系统**，而是写进虚拟变更集；
//! 真实文件只在用户显式提交后统一变更。本模块只负责：
//! ① 草稿的数据形状与栈语义（撤销 / 重做），② 名称与路径的预检，③ **投影**（真实状态 + 草稿 →
//! 界面应该显示的样子），④ 冲突检测。落盘（drafts 表）与文件操作分别属基础设施层与 P6。
//!
//! 硬约束：这里没有一行 IO；所有判定都能穷举单测。

use std::collections::HashSet;

/// 草稿的操作类型（对应 §13.3 `drafts.op`）。
#[derive(Debug, Clone, Copy, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub enum DraftOp {
    Rename,
    Move,
    Copy,
    Mkdir,
    Rmdir,
    Trash,
    Retag,
    RewriteRef,
}

impl DraftOp {
    pub fn as_str(self) -> &'static str {
        match self {
            DraftOp::Rename => "rename",
            DraftOp::Move => "move",
            DraftOp::Copy => "copy",
            DraftOp::Mkdir => "mkdir",
            DraftOp::Rmdir => "rmdir",
            DraftOp::Trash => "trash",
            DraftOp::Retag => "retag",
            DraftOp::RewriteRef => "rewrite_ref",
        }
    }

    pub fn parse(s: &str) -> Option<Self> {
        Some(match s {
            "rename" => DraftOp::Rename,
            "move" => DraftOp::Move,
            "copy" => DraftOp::Copy,
            "mkdir" => DraftOp::Mkdir,
            "rmdir" => DraftOp::Rmdir,
            "trash" => DraftOp::Trash,
            "retag" => DraftOp::Retag,
            "rewrite_ref" | "rewriteRef" => DraftOp::RewriteRef,
            _ => return None,
        })
    }

    /// 是否会让源路径消失（投影时标灰）。
    pub fn removes_source(self) -> bool {
        matches!(self, DraftOp::Move | DraftOp::Rmdir | DraftOp::Trash)
    }

    /// 是否会改动源路径的名字/位置（rename / move）。
    pub fn renames(self) -> bool {
        matches!(self, DraftOp::Rename | DraftOp::Move)
    }
}

/// 预检结论（对应 §13.3 `drafts.check_status`）。
#[derive(Debug, Clone, Copy, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub enum CheckStatus {
    Ok,
    /// 目标已存在于真实磁盘，或与另一条草稿撞名。
    Conflict,
    /// 名字非法（非法字符 / 保留名 / 空 / 超长）。
    Illegal,
    /// 路径超长。
    TooLong,
    /// 超出工作根。
    OutOfRoot,
    /// 命中保护区（§7.6）。
    Protected,
    /// 被别的文件引用（§6.3.1）。
    Referenced,
}

/// 一条草稿。
#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Draft {
    /// 单调递增序号（界面按它排序，也用于撤销/重做）。
    pub seq: u64,
    pub op: DraftOp,
    /// 新建类操作为 `None`。
    pub asset_id: Option<i64>,
    /// 源绝对路径（展示与执行用）。
    pub src: String,
    /// 目标绝对路径（rename / move / copy / mkdir 有值）。
    pub dst: Option<String>,
    pub check: CheckStatus,
    pub reason: Option<String>,
}

impl Draft {
    /// 造一条尚未预检的草稿（`seq` 由 [`ChangeSet::add`] 赋）。
    pub fn new(op: DraftOp, src: impl Into<String>, dst: Option<String>) -> Self {
        Draft {
            seq: 0,
            op,
            asset_id: None,
            src: src.into(),
            dst,
            check: CheckStatus::Ok,
            reason: None,
        }
    }
}

/// 变更集：草稿栈 + 重做栈。`Ctrl+Z` / `Ctrl+Y` 只在这两个栈之间搬，**不碰磁盘**（§7.2）。
#[derive(Debug, Default, Clone)]
pub struct ChangeSet {
    drafts: Vec<Draft>,
    redo: Vec<Draft>,
    next_seq: u64,
}

impl ChangeSet {
    /// 追加一条草稿；**任何新动作都会清空重做栈**（标准撤销语义）。
    pub fn add(&mut self, mut draft: Draft) -> u64 {
        self.next_seq += 1;
        draft.seq = self.next_seq;
        self.drafts.push(draft);
        self.redo.clear();
        self.next_seq
    }

    /// 撤销一条（后进先出），并压入重做栈。
    pub fn undo(&mut self) -> Option<Draft> {
        let d = self.drafts.pop()?;
        self.redo.push(d.clone());
        Some(d)
    }

    /// 重做一条。
    pub fn redo(&mut self) -> Option<Draft> {
        let d = self.redo.pop()?;
        self.drafts.push(d.clone());
        Some(d)
    }

    pub fn all(&self) -> &[Draft] {
        &self.drafts
    }

    pub fn len(&self) -> usize {
        self.drafts.len()
    }

    pub fn is_empty(&self) -> bool {
        self.drafts.is_empty()
    }

    pub fn redo_len(&self) -> usize {
        self.redo.len()
    }

    /// 清空（放弃变更；§7.2 关闭软件时的「放弃变更并关闭」，调用方需二次确认）。
    pub fn clear(&mut self) {
        self.drafts.clear();
        self.redo.clear();
    }

    /// 预检有问题的条数（状态条「待提交 M 项，其中 K 项有问题」用）。
    pub fn problems(&self) -> usize {
        self.drafts
            .iter()
            .filter(|d| d.check != CheckStatus::Ok)
            .count()
    }
}

/// Windows 保留设备名（不区分大小写，带扩展名也算，如 `CON.txt`）。
const RESERVED: [&str; 22] = [
    "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8",
    "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
];

/// 文件名合法性（§7.2 预检：名字非法）。返回 `Some(原因)` 表示非法。
pub fn check_file_name(name: &str) -> Option<String> {
    if name.is_empty() {
        return Some("名字不能为空".into());
    }
    if name == "." || name == ".." {
        return Some("名字不能是 `.` 或 `..`".into());
    }
    if name.chars().count() > 255 {
        return Some("名字超过 255 个字符".into());
    }
    if let Some(c) = name.chars().find(|c| matches!(c, '\\' | '/' | ':' | '*' | '?' | '"' | '<' | '>' | '|')) {
        return Some(format!("名字里不能有 `{c}`"));
    }
    if name.chars().any(|c| (c as u32) < 32) {
        return Some("名字里不能有控制字符".into());
    }
    if name.ends_with('.') || name.ends_with(' ') {
        return Some("名字不能以点或空格结尾".into());
    }
    let stem = name.split('.').next().unwrap_or(name).to_ascii_uppercase();
    if RESERVED.contains(&stem.as_str()) {
        return Some(format!("`{stem}` 是 Windows 保留名"));
    }
    None
}

/// 路径长度上限（§7.2 预检：路径超长）。Windows 常规上限 260（含结尾 NUL 与驱动器前缀）。
pub const MAX_PATH: usize = 259;

/// 路径长度预检；返回 `Some(原因)` 表示超长。
pub fn check_path_len(path: &str) -> Option<String> {
    if path.chars().count() > MAX_PATH {
        return Some(format!("路径长度 {} 超过上限 {MAX_PATH}", path.chars().count()));
    }
    None
}

/// 投影结果：界面应该显示成什么样。
#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Projection {
    /// 折算后的路径（被删除/移走时是最后已知的路径）。
    pub path: String,
    /// 是否已被草稿改动（界面用**斜体 + 橙色小圆点**）。
    pub drafted: bool,
    /// 是否在草稿里被移除（界面标灰）。
    pub removed: bool,
}

/// 投影：把一条真实路径按草稿链折算成「界面显示的样子」。
///
/// 顺序敏感：按 `seq` 升序叠加，rename/move 改写路径，trash/rmdir 标记移除。
pub fn project(path: &str, drafts: &[Draft]) -> Projection {
    let mut ordered: Vec<&Draft> = drafts.iter().collect();
    ordered.sort_by_key(|d| d.seq);

    let mut current = path.to_string();
    let mut drafted = false;
    let mut removed = false;

    for d in ordered {
        if !eq_path(&d.src, &current) {
            continue;
        }
        if d.op.renames() {
            if let Some(dst) = &d.dst {
                current = dst.clone();
                drafted = true;
            }
        } else if d.op.removes_source() {
            removed = true;
            drafted = true;
        }
    }

    Projection {
        path: current,
        drafted,
        removed,
    }
}

/// Windows 路径比较：反斜杠统一 + 大小写不敏感。
pub fn eq_path(a: &str, b: &str) -> bool {
    a.replace('/', "\\").to_lowercase() == b.replace('/', "\\").to_lowercase()
}

/// 冲突检测（§7.2 实时预检）：
/// ① 目标在真实磁盘上已存在；② 两条草稿的目标撞名。返回 `(seq, 结论, 原因)`。
pub fn detect_conflicts(drafts: &[Draft], existing: &HashSet<String>) -> Vec<(u64, CheckStatus, String)> {
    let existing_lower: HashSet<String> = existing.iter().map(|p| p.replace('/', "\\").to_lowercase()).collect();
    let mut planned: HashSet<String> = HashSet::new();
    let mut out = Vec::new();

    let mut ordered: Vec<&Draft> = drafts.iter().collect();
    ordered.sort_by_key(|d| d.seq);

    for d in ordered {
        let Some(dst) = &d.dst else { continue };
        let key = dst.replace('/', "\\").to_lowercase();

        // 自己改自己（同名）不算冲突
        if eq_path(dst, &d.src) {
            continue;
        }
        if existing_lower.contains(&key) {
            out.push((d.seq, CheckStatus::Conflict, format!("`{dst}` 在磁盘上已存在")));
            continue;
        }
        if !planned.insert(key) {
            out.push((d.seq, CheckStatus::Conflict, format!("`{dst}` 与另一条草稿的目标相同")));
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn cs() -> ChangeSet {
        ChangeSet::default()
    }

    #[test]
    fn 名字校验的各类非法情形() {
        assert!(check_file_name("").is_some(), "空名");
        assert!(check_file_name(".").is_some());
        assert!(check_file_name("..").is_some());
        assert!(check_file_name("a\\b").is_some(), "反斜杠");
        assert!(check_file_name("a/b").is_some(), "斜杠");
        assert!(check_file_name("a:b").is_some(), "冒号");
        assert!(check_file_name("a?b").is_some());
        assert!(check_file_name("a\u{1}b").is_some(), "控制字符");
        assert!(check_file_name("abc.").is_some(), "点结尾");
        assert!(check_file_name("abc ").is_some(), "空格结尾");
        assert!(check_file_name("CON").is_some(), "保留名");
        assert!(check_file_name("con.txt").is_some(), "带扩展名的保留名");
        assert!(check_file_name(&"x".repeat(256)).is_some(), "超长");

        assert_eq!(check_file_name("date_0.png"), None);
        assert_eq!(check_file_name("中文 名称.jpg"), None);
        assert_eq!(check_file_name("NUL_备份"), None, "只是以保留名开头，不算保留名");
    }

    #[test]
    fn 路径长度校验() {
        assert_eq!(check_path_len(&format!(r"D:\{}", "a".repeat(200))), None);
        assert!(check_path_len(&format!(r"D:\{}", "a".repeat(300))).is_some());
    }

    #[test]
    fn 撤销重做只在草稿内搬且新动作清空重做栈() {
        let mut c = cs();
        assert!(c.is_empty());
        c.add(Draft::new(DraftOp::Rename, r"D:\a.png", Some(r"D:\b.png".into())));
        c.add(Draft::new(DraftOp::Move, r"D:\c.png", Some(r"D:\子\c.png".into())));
        assert_eq!(c.len(), 2);

        let undone = c.undo().expect("能撤销");
        assert_eq!(undone.op, DraftOp::Move);
        assert_eq!(c.len(), 1);
        assert_eq!(c.redo_len(), 1);

        let redone = c.redo().expect("能重做");
        assert_eq!(redone.op, DraftOp::Move);
        assert_eq!(c.len(), 2);

        // 撤销后再来一个新动作 → 重做栈被清空（标准语义）
        c.undo();
        c.add(Draft::new(DraftOp::Mkdir, r"D:\新目录", None));
        assert_eq!(c.redo_len(), 0);
        assert_eq!(c.len(), 2);

        // 序号单调
        assert_eq!(c.all().iter().map(|d| d.seq).collect::<Vec<_>>(), vec![1, 3]);
    }

    #[test]
    fn 投影把重命名与移动串起来() {
        let mut c = cs();
        c.add(Draft::new(DraftOp::Rename, r"D:\素材\a.png", Some(r"D:\素材\b.png".into())));
        c.add(Draft::new(DraftOp::Move, r"D:\素材\b.png", Some(r"D:\素材\子\b.png".into())));

        let p = project(r"D:\素材\a.png", c.all());
        assert_eq!(p.path, r"D:\素材\子\b.png");
        assert!(p.drafted);
        assert!(!p.removed);

        // 没被草稿碰到的路径保持原样
        let q = project(r"D:\素材\z.png", c.all());
        assert_eq!(q.path, r"D:\素材\z.png");
        assert!(!q.drafted);
    }

    #[test]
    fn 投影把移走与移入回收站标为已移除() {
        let mut c = cs();
        c.add(Draft::new(DraftOp::Trash, r"D:\素材\a.png", None));
        let p = project(r"D:\素材\a.png", c.all());
        assert!(p.removed && p.drafted);
        assert_eq!(p.path, r"D:\素材\a.png", "被删除时路径保持最后已知值");
    }

    #[test]
    fn 复制不改动源路径() {
        let mut c = cs();
        c.add(Draft::new(DraftOp::Copy, r"D:\素材\a.png", Some(r"D:\备份\a.png".into())));
        let p = project(r"D:\素材\a.png", c.all());
        assert_eq!(p.path, r"D:\素材\a.png");
        assert!(!p.drafted, "复制不会让源变草稿态");
    }

    #[test]
    fn 冲突检测覆盖磁盘已存在与草稿互相撞名() {
        let mut c = cs();
        c.add(Draft::new(DraftOp::Rename, r"D:\a.png", Some(r"D:\b.png".into())));
        c.add(Draft::new(DraftOp::Rename, r"D:\c.png", Some(r"D:\b.png".into())));
        c.add(Draft::new(DraftOp::Rename, r"D:\d.png", Some(r"D:\e.png".into())));

        let mut existing = HashSet::new();
        existing.insert(r"D:\e.png".to_string()); // 磁盘上已经有个 e.png

        let issues = detect_conflicts(c.all(), &existing);
        assert_eq!(issues.len(), 2);
        // 先报草稿之间的撞名（seq=2 与 seq=1 都指向 b.png）
        assert_eq!(issues[0].0, 2);
        assert!(issues[0].2.contains("另一条草稿"));
        // 再报与磁盘撞名（seq=3 的 e.png 在磁盘上已存在）
        assert_eq!(issues[1].0, 3);
        assert!(issues[1].2.contains("磁盘上已存在"));
    }

    #[test]
    fn 大小写不敏感地比较路径() {
        assert!(eq_path(r"D:\A.PNG", r"d:/a.png"));
        let mut c = cs();
        c.add(Draft::new(DraftOp::Rename, r"D:\a.png", Some(r"D:\SUB\B.PNG".into())));
        let mut existing = HashSet::new();
        existing.insert(r"d:\sub\b.png".to_string());
        let issues = detect_conflicts(c.all(), &existing);
        assert_eq!(issues.len(), 1, "大小写不同也算冲突");
    }

    #[test]
    fn 同名改自己不算冲突() {
        let mut c = cs();
        c.add(Draft::new(DraftOp::Rename, r"D:\a.png", Some(r"D:\a.png".into())));
        assert!(detect_conflicts(c.all(), &HashSet::new()).is_empty());
    }

    #[test]
    fn 有问题条数统计() {
        let mut c = cs();
        c.add(Draft::new(DraftOp::Rename, r"D:\a.png", Some(r"D:\b.png".into())));
        let mut bad = Draft::new(DraftOp::Rename, r"D:\c.png", Some(r"D:\d.png".into()));
        bad.check = CheckStatus::Conflict;
        c.add(bad);
        assert_eq!(c.problems(), 1);
        assert_eq!(c.len(), 2);
    }

    #[test]
    fn 操作类型字符串往返() {
        for op in [
            DraftOp::Rename,
            DraftOp::Move,
            DraftOp::Copy,
            DraftOp::Mkdir,
            DraftOp::Rmdir,
            DraftOp::Trash,
            DraftOp::Retag,
            DraftOp::RewriteRef,
        ] {
            assert_eq!(DraftOp::parse(op.as_str()), Some(op), "{}", op.as_str());
        }
        assert_eq!(DraftOp::parse("nope"), None);
        assert!(DraftOp::Move.removes_source());
        assert!(!DraftOp::Copy.removes_source());
        assert!(DraftOp::Rename.renames());
    }
}
