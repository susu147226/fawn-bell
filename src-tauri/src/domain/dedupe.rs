//! 核心域 · 重复内容分组与保留策略（执行版 §7.12 / §16⑰；**纯逻辑，无 IO**）。
//!
//! §7.12 的判定口径：
//! 1. 先按「**体积 + 首尾各 64 KB 分段哈希**」筛出候选组（第一级指纹，扫描时已入库）；
//! 2. 候选组内再比**全长内容**（第二级，见 [`crate::app::dedupe`]，这里只负责分组与策略）；
//! 3. 仅体积相同而哈希不同的一律不算重复。
//!
//! 保留策略（§7.12）：默认「保留最早创建的」，可切「保留路径最短」，或由用户手工指定。

/// 参与去重的候选条目（来自索引库的投影，不含 IO）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Candidate {
    pub asset_id: i64,
    /// 卷标识（`vol:<序列号>` / `mnt:<挂载点>`）：还原绝对路径与跨卷区分都要用。
    pub volume_id: String,
    pub rel_path: String,
    pub size: i64,
    /// 首尾各 64 KB 分段哈希（形如 `b3p1:aaaabbbb-ccccdddd`）。
    pub hash_partial: String,
    /// 创建时间（毫秒）；缺失记 0。
    pub ctime: i64,
}

/// 每个重复组保留哪一项。
#[derive(Debug, Clone, Copy, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub enum KeepPolicy {
    /// 默认档：保留最早创建的（创建时间相同则路径短者优先，再按路径字典序，保证确定性）。
    EarliestCreated,
    /// 保留路径最短的。
    ShortestPath,
    /// 由用户手工指定，核心域不替用户选。
    Manual,
}

impl Default for KeepPolicy {
    fn default() -> Self {
        KeepPolicy::EarliestCreated
    }
}

impl KeepPolicy {
    pub fn as_str(self) -> &'static str {
        match self {
            KeepPolicy::EarliestCreated => "earliestCreated",
            KeepPolicy::ShortestPath => "shortestPath",
            KeepPolicy::Manual => "manual",
        }
    }

    pub fn parse(s: &str) -> Option<Self> {
        match s {
            "earliest" | "earliestCreated" => Some(KeepPolicy::EarliestCreated),
            "shortest" | "shortestPath" => Some(KeepPolicy::ShortestPath),
            "manual" => Some(KeepPolicy::Manual),
            _ => None,
        }
    }
}

/// 按「体积 + 分段哈希」分组，**只返回成员数 > 1 的组**（索引下标指向入参）。
///
/// 顺序确定性：先按体积升序，再按指纹字典序；组内保持入参顺序。
pub fn group_by_fingerprint(items: &[Candidate]) -> Vec<Vec<usize>> {
    let mut order: Vec<usize> = (0..items.len()).collect();
    order.sort_by(|&a, &b| {
        let (x, y) = (&items[a], &items[b]);
        x.size
            .cmp(&y.size)
            .then_with(|| x.hash_partial.cmp(&y.hash_partial))
            .then_with(|| a.cmp(&b))
    });

    let mut groups: Vec<Vec<usize>> = Vec::new();
    let mut current: Vec<usize> = Vec::new();
    let mut key: Option<(i64, &str)> = None;
    for idx in order {
        let item = &items[idx];
        let k = (item.size, item.hash_partial.as_str());
        match key {
            Some(prev) if prev == k => current.push(idx),
            _ => {
                if current.len() > 1 {
                    groups.push(std::mem::take(&mut current));
                } else {
                    current.clear();
                }
                key = Some(k);
                current.push(idx);
            }
        }
    }
    if current.len() > 1 {
        groups.push(current);
    }
    groups
}

/// 在组内选出保留项（返回组内下标）；[`KeepPolicy::Manual`] 返回 `None`。
pub fn pick_keeper(group: &[Candidate], policy: KeepPolicy) -> Option<usize> {
    match policy {
        KeepPolicy::Manual => None,
        KeepPolicy::EarliestCreated => group
            .iter()
            .enumerate()
            .min_by(|(_, a), (_, b)| {
                a.ctime
                    .cmp(&b.ctime)
                    .then_with(|| a.rel_path.len().cmp(&b.rel_path.len()))
                    .then_with(|| a.rel_path.cmp(&b.rel_path))
            })
            .map(|(i, _)| i),
        KeepPolicy::ShortestPath => group
            .iter()
            .enumerate()
            .min_by(|(_, a), (_, b)| {
                a.rel_path
                    .len()
                    .cmp(&b.rel_path.len())
                    .then_with(|| a.rel_path.cmp(&b.rel_path))
            })
            .map(|(i, _)| i),
    }
}

/// 一组重复内容里，除保留项外白白占用的字节数。
pub fn waste_bytes(group: &[Candidate], keeper: Option<usize>) -> i64 {
    if group.len() < 2 {
        return 0;
    }
    let per = group.first().map(|c| c.size).unwrap_or(0);
    let cleared = if keeper.is_some() { 1 } else { 0 };
    per * (group.len() as i64 - cleared)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn c(id: i64, path: &str, size: i64, hash: &str, ctime: i64) -> Candidate {
        Candidate {
            asset_id: id,
            volume_id: "vol:TEST".to_string(),
            rel_path: path.to_string(),
            size,
            hash_partial: hash.to_string(),
            ctime,
        }
    }

    #[test]
    fn 同体积同指纹才成组() {
        let items = vec![
            c(1, r"a\x.jpg", 100, "b3p1:aa-bb", 10),
            c(2, r"a\y.jpg", 100, "b3p1:aa-bb", 20),
            // 同体积不同指纹 → 不是重复（§7.12 明文）
            c(3, r"a\z.jpg", 100, "b3p1:cc-dd", 30),
            // 指纹相同但体积不同 → 不成组
            c(4, r"a\w.jpg", 200, "b3p1:aa-bb", 40),
            // 只有一个成员 → 不成组
            c(5, r"a\solo.jpg", 300, "b3p1:ee-ff", 50),
        ];
        let groups = group_by_fingerprint(&items);
        assert_eq!(groups.len(), 1);
        let mut ids: Vec<i64> = groups[0].iter().map(|&i| items[i].asset_id).collect();
        ids.sort();
        assert_eq!(ids, vec![1, 2]);
    }

    #[test]
    fn 默认保留最早创建() {
        let g = vec![
            c(1, r"very\long\path\x.jpg", 100, "h", 500),
            c(2, r"b.jpg", 100, "h", 300),
            c(3, r"c.jpg", 100, "h", 400),
        ];
        assert_eq!(pick_keeper(&g, KeepPolicy::EarliestCreated), Some(1));
    }

    #[test]
    fn 创建时间相同时按路径短再字典序() {
        let g = vec![
            c(1, r"aa\bb\cc.jpg", 100, "h", 100),
            c(2, r"z.jpg", 100, "h", 100),
            c(3, r"a.jpg", 100, "h", 100),
        ];
        // 同为 ctime=100：路径最短的是 z.jpg(5) 与 a.jpg(5) → 字典序 a.jpg 胜
        assert_eq!(pick_keeper(&g, KeepPolicy::EarliestCreated), Some(2));
    }

    #[test]
    fn 最短路径策略与手工策略() {
        let g = vec![
            c(1, r"aa\bb\cc.jpg", 100, "h", 100),
            c(2, r"z.jpg", 100, "h", 900),
        ];
        assert_eq!(pick_keeper(&g, KeepPolicy::ShortestPath), Some(1));
        assert_eq!(pick_keeper(&g, KeepPolicy::Manual), None);
    }

    #[test]
    fn 浪费字节数按未保留项累计() {
        let g = vec![
            c(1, "a", 100, "h", 1),
            c(2, "b", 100, "h", 2),
            c(3, "c", 100, "h", 3),
        ];
        assert_eq!(waste_bytes(&g, Some(0)), 200);
        assert_eq!(waste_bytes(&g, None), 300);
        assert_eq!(waste_bytes(&g[..1], Some(0)), 0);
    }

    #[test]
    fn 策略字符串往返() {
        assert_eq!(KeepPolicy::parse("earliest"), Some(KeepPolicy::EarliestCreated));
        assert_eq!(KeepPolicy::parse("shortest"), Some(KeepPolicy::ShortestPath));
        assert_eq!(KeepPolicy::parse("manual"), Some(KeepPolicy::Manual));
        assert_eq!(KeepPolicy::parse("whatever"), None);
        assert_eq!(KeepPolicy::default().as_str(), "earliestCreated");
    }
}
