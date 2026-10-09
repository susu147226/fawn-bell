//! 引擎 CLI（执行版 §12.2 架构强制项 ⑤：引擎可独立驱动）。
//!
//! ```text
//! luling scan <文件夹> [<文件夹>…] [--json] [--out <文件>] [--follow-links] [--max-depth N] [--limit N]
//! luling --help | --version
//! ```
//!
//! - 退出码：`0` 成功 / `1` 参数或运行失败 / `2` 被取消。
//! - **发布构建是 Windows 子系统程序（无控制台）**，因此脚本取结果请用 `--out`；输出一律 UTF-8，
//!   文本结果带 BOM（方便记事本/PowerShell 正确识别），JSON 结果不带 BOM（严格解析器要求）。
//! - CLI 与界面共用同一个 `ScanUseCase`，规则完全一致（同一份核心域与基础设施）。

use std::io::Write;
use std::path::PathBuf;
use std::sync::atomic::AtomicBool;

use crate::app::scan::{run_scan, ScanOptions, ScanOutcome};

const HELP: &str = "\
鹿铃 luling —— 素材命名 / 位置 / 分组工具（引擎命令行）

用法：
  luling scan <文件夹> [<文件夹>…] [选项]
  luling dedupe [选项]
  luling relocate <旧素材根> <新素材根> [选项]
  luling migrate <目标库目录> [选项]

选项：
  --json                以 JSON 输出
  --out <文件>          把结果写入文件（发布版无控制台，脚本请用这个）
  --follow-links        跟随符号链接与 junction（默认否）
  --max-depth <N>       递归深度上限（默认不限）
  --limit <N>           单次扫描文件数上限（默认 200000）
  --no-index            只扫描，不写入索引库（默认会写：增量索引 + 内容指纹）
  --keep <档位>         去重保留策略：earliest（默认）/ shortest / manual
  --apply               重定位：真的改写索引（默认只给计划）
  --include-confirm     重定位：连「待确认」一档也一起改写
  -h, --help            显示本帮助
  -v, --version         显示版本

退出码：0 成功 / 1 失败 / 2 被取消

说明：扫描全程只读，不创建、不修改、不删除素材树里的任何东西。
";

fn out_text(s: &str) {
    let stdout = std::io::stdout();
    let mut lock = stdout.lock();
    let _ = lock.write_all(s.as_bytes());
    let _ = lock.write_all(b"\n");
    let _ = lock.flush();
}

fn err_text(s: &str) {
    let stderr = std::io::stderr();
    let mut lock = stderr.lock();
    let _ = lock.write_all(s.as_bytes());
    let _ = lock.write_all(b"\n");
    let _ = lock.flush();
}

fn human_bytes(n: u64) -> String {
    const UNITS: [&str; 5] = ["B", "KB", "MB", "GB", "TB"];
    let mut v = n as f64;
    let mut i = 0usize;
    while v >= 1024.0 && i < UNITS.len() - 1 {
        v /= 1024.0;
        i += 1;
    }
    if i == 0 {
        format!("{n} B")
    } else {
        format!("{v:.2} {}", UNITS[i])
    }
}

fn group_thousands(n: u64) -> String {
    let s = n.to_string();
    let mut out = String::new();
    for (i, c) in s.chars().enumerate() {
        if i > 0 && (s.len() - i) % 3 == 0 {
            out.push(',');
        }
        out.push(c);
    }
    out
}

fn write_out(path: &PathBuf, json: bool, body: &str) -> Result<(), String> {
    let mut bytes: Vec<u8> = Vec::with_capacity(body.len() + 3);
    if !json {
        bytes.extend_from_slice(&[0xEF, 0xBB, 0xBF]); // UTF-8 BOM
    }
    bytes.extend_from_slice(body.as_bytes());
    std::fs::write(path, bytes).map_err(|e| format!("写入 `{}` 失败：{e}", path.display()))
}

pub fn run(args: &[String]) -> i32 {
    // §12.2 强制项 ⑤：去重、重定位、库迁移都必须能独立驱动
    if args.first().map(|s| s.as_str()) == Some("dedupe") {
        return run_dedupe(&args[1..]);
    }
    if args.first().map(|s| s.as_str()) == Some("relocate") {
        return run_relocate(&args[1..]);
    }
    if args.first().map(|s| s.as_str()) == Some("migrate") {
        return run_migrate(&args[1..]);
    }

    let mut json = false;
    let mut out_file: Option<PathBuf> = None;
    let mut follow_links = false;
    let mut max_depth: Option<usize> = None;
    let mut limit: Option<u64> = None;
    let mut no_index = false;
    let mut roots: Vec<String> = Vec::new();

    let mut i = 0usize;
    while i < args.len() {
        let a = args[i].as_str();
        match a {
            "scan" => {}
            "--json" => json = true,
            "--follow-links" => follow_links = true,
            "--no-index" => no_index = true,
            "--out" => {
                i += 1;
                match args.get(i) {
                    Some(v) => out_file = Some(PathBuf::from(v)),
                    None => {
                        err_text("错误：--out 需要一个文件路径。");
                        return 1;
                    }
                }
            }
            "--max-depth" => {
                i += 1;
                match args.get(i).and_then(|v| v.parse::<usize>().ok()) {
                    Some(v) if v >= 1 => max_depth = Some(v),
                    _ => {
                        err_text("错误：--max-depth 需要一个 ≥1 的整数。");
                        return 1;
                    }
                }
            }
            "--limit" => {
                i += 1;
                match args.get(i).and_then(|v| v.parse::<u64>().ok()) {
                    Some(v) if v >= 1 => limit = Some(v),
                    _ => {
                        err_text("错误：--limit 需要一个 ≥1 的整数。");
                        return 1;
                    }
                }
            }
            "-h" | "--help" => {
                out_text(HELP);
                return 0;
            }
            "-v" | "--version" | "version" => {
                out_text(&format!("鹿铃 luling {}", env!("CARGO_PKG_VERSION")));
                return 0;
            }
            other if other.starts_with("--") => {
                err_text(&format!("错误：未知参数 `{other}`。用 --help 查看用法。"));
                return 1;
            }
            other => roots.push(other.to_string()),
        }
        i += 1;
    }

    if roots.is_empty() {
        err_text("错误：没有指定要扫描的文件夹。用法：luling scan <文件夹> [选项]");
        return 1;
    }

    let library_dir = crate::infra::paths::library_dir();
    let mut summaries: Vec<crate::app::scan::ScanSummary> = Vec::new();
    let mut indexes: Vec<crate::app::index::IndexReport> = Vec::new();
    let mut text = String::new();
    let cancel = AtomicBool::new(false);

    for (n, root) in roots.iter().enumerate() {
        let mut opts = ScanOptions::new(n as u64 + 1, library_dir.clone());
        opts.follow_links = follow_links;
        opts.max_depth = max_depth;
        if let Some(l) = limit {
            opts.file_limit = l;
        }

        let outcome = run_scan(&PathBuf::from(root), &opts, &cancel, |_| {});

        match outcome {
            ScanOutcome::Done(result) => {
                let s = &result.summary;
                text.push_str(&format!("素材根：{}\n", s.root));
                text.push_str(&format!(
                    "  文件夹 {} · 文件 {} · 合计 {} · 用时 {} ms\n",
                    group_thousands(s.dir_count),
                    group_thousands(s.file_count),
                    human_bytes(s.total_bytes),
                    s.elapsed_ms
                ));
                let stats: Vec<String> = s
                    .by_kind
                    .iter()
                    .filter(|k| k.count > 0)
                    .map(|k| {
                        format!(
                            "{} {}（{}）",
                            k.kind.label_zh(),
                            group_thousands(k.count),
                            human_bytes(k.bytes)
                        )
                    })
                    .collect();
                if !stats.is_empty() {
                    text.push_str(&format!("  {}\n", stats.join(" · ")));
                }
                for w in &s.warnings {
                    text.push_str(&format!("  ⚠ {w}\n"));
                }
                if s.truncated {
                    text.push_str("  注意：已达单次扫描文件数上限，结果不完整。\n");
                }

                // §7.1：扫描成功后写索引库（增量判定 + 首尾 64 KB 指纹 + 元数据 + 引用映射）
                if !no_index {
                    let provider = crate::app::metadata::FullMetadata;
                    match crate::app::index::index_scan(
                        &PathBuf::from(&s.root),
                        &result.files,
                        &crate::infra::library::layout(),
                        &cancel,
                        &provider,
                        |_| {},
                    ) {
                        Ok(r) => {
                            text.push_str(&format!(
                                "  索引：新增 {} · 更新 {} · 未变 {} · 缺失 {} · 条目 {} · 引用 {} 行 · 指纹 {} 个\n",
                                r.inserted, r.updated, r.unchanged, r.missing, r.entries, r.refs, r.hashed
                            ));
                            for w in &r.warnings {
                                text.push_str(&format!("  ⚠ {w}\n"));
                            }
                            indexes.push(r);
                        }
                        Err(e) => text.push_str(&format!("  ⚠ 建立索引失败：{e}\n")),
                    }
                }

                summaries.push(s.clone());
            }
            ScanOutcome::Cancelled { .. } => {
                err_text("扫描已被取消。");
                return 2;
            }
            ScanOutcome::Failed(msg) => {
                err_text(&format!("扫描失败：{msg}"));
                return 1;
            }
        }
    }

    let body = if json {
        let payload = serde_json::json!({
            "tool": "luling",
            "version": env!("CARGO_PKG_VERSION"),
            "libraryDir": library_dir.to_string_lossy(),
            "results": summaries,
            "indexes": indexes,
        });
        match serde_json::to_string_pretty(&payload) {
            Ok(s) => s,
            Err(e) => {
                err_text(&format!("序列化失败：{e}"));
                return 1;
            }
        }
    } else {
        text
    };

    match out_file {
        Some(p) => match write_out(&p, json, &body) {
            Ok(()) => {
                out_text(&format!("结果已写入 {}", p.display()));
                0
            }
            Err(e) => {
                err_text(&e);
                1
            }
        },
        None => {
            out_text(&body);
            0
        }
    }
}

/// `luling dedupe`：内容去重（§7.12）。
///
/// 只读素材、只写库：判定结果同时落进「重复内容」智能集合；真正的清理动作
/// （只进回收站）属于 P7 文件操作矩阵，这里不做任何删除。
fn run_dedupe(args: &[String]) -> i32 {
    use crate::domain::dedupe::KeepPolicy;

    let mut json = false;
    let mut out_file: Option<PathBuf> = None;
    let mut policy = KeepPolicy::default();

    let mut i = 0usize;
    while i < args.len() {
        match args[i].as_str() {
            "--json" => json = true,
            "--out" => {
                i += 1;
                match args.get(i) {
                    Some(v) => out_file = Some(PathBuf::from(v)),
                    None => {
                        err_text("错误：--out 需要一个文件路径。");
                        return 1;
                    }
                }
            }
            "--keep" => {
                i += 1;
                match args.get(i).and_then(|v| KeepPolicy::parse(v)) {
                    Some(p) => policy = p,
                    None => {
                        err_text("错误：--keep 只支持 earliest / shortest / manual。");
                        return 1;
                    }
                }
            }
            "-h" | "--help" => {
                out_text(HELP);
                return 0;
            }
            other => {
                err_text(&format!("错误：未知参数 `{other}`。用 --help 查看用法。"));
                return 1;
            }
        }
        i += 1;
    }

    let cancel = AtomicBool::new(false);
    let layout = crate::infra::library::layout();
    let report = match crate::app::dedupe::run(&layout, policy, true, &cancel) {
        Ok(r) => r,
        Err(e) => {
            err_text(&format!("内容去重失败：{e}"));
            return 1;
        }
    };

    let body = if json {
        match serde_json::to_string_pretty(&report) {
            Ok(s) => s,
            Err(e) => {
                err_text(&format!("序列化失败：{e}"));
                return 1;
            }
        }
    } else {
        let mut t = String::new();
        t.push_str(&format!(
            "内容去重（保留策略：{}）· 库 {}\n",
            report.policy.as_str(),
            layout.db.display()
        ));
        t.push_str(&format!(
            "  候选 {} 项 · 重复组 {} 组 · 重复项 {} 项 · 保留 {} 项 · 可清理 {}\n",
            group_thousands(report.candidates),
            group_thousands(report.group_count),
            group_thousands(report.duplicate_count),
            group_thousands(report.keepers),
            human_bytes(report.waste_bytes.max(0) as u64)
        ));
        for (n, g) in report.groups.iter().enumerate() {
            t.push_str(&format!(
                "  组 {}（{} · {}）：\n",
                n + 1,
                human_bytes(g.size.max(0) as u64),
                g.hash_partial
            ));
            for m in &g.members {
                t.push_str(&format!(
                    "    {} {}\n",
                    if m.keeper { "保留" } else { "重复" },
                    m.abs_path.clone().unwrap_or_else(|| m.rel_path.clone())
                ));
            }
        }
        for w in &report.warnings {
            t.push_str(&format!("  ⚠ {w}\n"));
        }
        t
    };

    match out_file {
        Some(p) => match write_out(&p, json, &body) {
            Ok(()) => {
                out_text(&format!("结果已写入 {}", p.display()));
                0
            }
            Err(e) => {
                err_text(&e);
                1
            }
        },
        None => {
            out_text(&body);
            0
        }
    }
}

/// `luling relocate`：重新定位素材树（§13.4）。默认只出计划，`--apply` 才改写索引。
fn run_relocate(args: &[String]) -> i32 {
    use crate::app::library::Confidence;

    let mut json = false;
    let mut out_file: Option<PathBuf> = None;
    let mut apply_now = false;
    let mut include_confirm = false;
    let mut roots: Vec<String> = Vec::new();

    let mut i = 0usize;
    while i < args.len() {
        match args[i].as_str() {
            "--json" => json = true,
            "--apply" => apply_now = true,
            "--include-confirm" => include_confirm = true,
            "--out" => {
                i += 1;
                match args.get(i) {
                    Some(v) => out_file = Some(PathBuf::from(v)),
                    None => {
                        err_text("错误：--out 需要一个文件路径。");
                        return 1;
                    }
                }
            }
            "-h" | "--help" => {
                out_text(HELP);
                return 0;
            }
            other if other.starts_with("--") => {
                err_text(&format!("错误：未知参数 `{other}`。用 --help 查看用法。"));
                return 1;
            }
            other => roots.push(other.to_string()),
        }
        i += 1;
    }

    if roots.len() != 2 {
        err_text("用法：luling relocate <旧素材根> <新素材根> [--apply] [--include-confirm]");
        return 1;
    }

    let layout = crate::infra::library::layout();
    let (plan, changed) = match crate::app::library::run(
        &layout,
        &PathBuf::from(&roots[0]),
        &PathBuf::from(&roots[1]),
        apply_now,
        include_confirm,
    ) {
        Ok(v) => v,
        Err(e) => {
            err_text(&format!("重定位失败：{e}"));
            return 1;
        }
    };

    let body = if json {
        match serde_json::to_string_pretty(&serde_json::json!({ "plan": plan, "changed": changed })) {
            Ok(s) => s,
            Err(e) => {
                err_text(&format!("序列化失败：{e}"));
                return 1;
            }
        }
    } else {
        let mut t = String::new();
        t.push_str("重新定位素材树\n");
        t.push_str(&format!("  原根：{}（卷 {}）\n", plan.old_root, plan.old_volume));
        t.push_str(&format!("  新根：{}（卷 {}）\n", plan.new_root, plan.new_volume));
        t.push_str(&format!(
            "  高置信 {} · 待确认 {} · 无法匹配 {}｜本次改写 {} 条\n",
            group_thousands(plan.high),
            group_thousands(plan.needs_confirm),
            group_thousands(plan.unmatched),
            group_thousands(changed as u64)
        ));
        for m in plan.matches.iter().take(40) {
            let tag = match m.confidence {
                Confidence::High => "高置信",
                Confidence::NeedsConfirm => "待确认",
                Confidence::Unmatched => "无法匹配",
            };
            match &m.new_rel_path {
                Some(new_rel) => t.push_str(&format!("  [{tag}] {} → {}\n", m.old_rel_path, new_rel)),
                None => t.push_str(&format!("  [{tag}] {} —— {}\n", m.old_rel_path, m.why)),
            }
        }
        if plan.matches.len() > 40 {
            t.push_str(&format!("  …… 另有 {} 条（用 --json 看全部）\n", plan.matches.len() - 40));
        }
        if !apply_now {
            t.push_str("  这是计划：没有改写任何索引；加 --apply 才会改写（待确认一档需再加 --include-confirm）。\n");
        }
        t
    };

    match out_file {
        Some(p) => match write_out(&p, json, &body) {
            Ok(()) => {
                out_text(&format!("结果已写入 {}", p.display()));
                0
            }
            Err(e) => {
                err_text(&e);
                1
            }
        },
        None => {
            out_text(&body);
            0
        }
    }
}

/// `luling migrate`：把库整体搬到另一个目录（§13.4 第 3 条）。
///
/// 只复制与校验，**保留旧目录**（由用户确认后再自行删除）；目标若是约定位置，顺带改写位置标记。
fn run_migrate(args: &[String]) -> i32 {
    let mut json = false;
    let mut out_file: Option<PathBuf> = None;
    let mut target: Option<String> = None;

    let mut i = 0usize;
    while i < args.len() {
        match args[i].as_str() {
            "--json" => json = true,
            "--out" => {
                i += 1;
                match args.get(i) {
                    Some(v) => out_file = Some(PathBuf::from(v)),
                    None => {
                        err_text("错误：--out 需要一个文件路径。");
                        return 1;
                    }
                }
            }
            "-h" | "--help" => {
                out_text(HELP);
                return 0;
            }
            other if other.starts_with("--") => {
                err_text(&format!("错误：未知参数 `{other}`。用 --help 查看用法。"));
                return 1;
            }
            other => target = Some(other.to_string()),
        }
        i += 1;
    }

    let Some(target) = target else {
        err_text("用法：luling migrate <目标库目录>（例如 D:\\鹿铃数据 或程序目录下的 data）");
        return 1;
    };
    let to_root = PathBuf::from(&target);

    let from = crate::infra::library::layout();
    let report = match crate::infra::library::migrate(&from, &to_root) {
        Ok(r) => r,
        Err(e) => {
            err_text(&format!("库迁移失败：{e}"));
            return 1;
        }
    };

    // 目标是「程序目录\data」或默认位置时，改写位置标记，让下次启动就落到新库
    let program = crate::infra::library::program_dir();
    let portable_root = program.join("data");
    let app_root = crate::infra::library::default_root();
    let norm = crate::domain::guard::norm;
    let mut switched: Option<&'static str> = None;
    if norm(&to_root) == norm(&portable_root) {
        if crate::infra::library::write_marker(&program, crate::infra::library::LibraryLocation::Portable).is_ok() {
            switched = Some("portable");
        }
    } else if norm(&to_root) == norm(&app_root) {
        if crate::infra::library::write_marker(&program, crate::infra::library::LibraryLocation::AppData).is_ok() {
            switched = Some("appData");
        }
    }

    let body = if json {
        match serde_json::to_string_pretty(&serde_json::json!({
            "source": report.source.to_string_lossy(),
            "target": report.target.to_string_lossy(),
            "files": report.files,
            "bytes": report.bytes,
            "entries": report.entries,
            "writable": report.writable,
            "switched": switched,
        })) {
            Ok(s) => s,
            Err(e) => {
                err_text(&format!("序列化失败：{e}"));
                return 1;
            }
        }
    } else {
        let mut t = String::new();
        t.push_str("库迁移\n");
        t.push_str(&format!("  源：{}\n", report.source.display()));
        t.push_str(&format!("  目标：{}\n", report.target.display()));
        t.push_str(&format!(
            "  文件 {} 个 · {} · 目标库条目 {} 条 · 可写 {}\n",
            group_thousands(report.files as u64),
            human_bytes(report.bytes),
            report.entries,
            if report.writable { "是" } else { "否" }
        ));
        match switched {
            Some(which) => t.push_str(&format!("  库位置已切换为 {which}（标记文件写在程序目录）。\n")),
            None => t.push_str("  这是自定义目录：库位置标记未改动，请在设置里指定该位置。\n"),
        }
        t.push_str("  旧目录保留未删（确认无误后可自行删除）。\n");
        t
    };

    match out_file {
        Some(p) => match write_out(&p, json, &body) {
            Ok(()) => {
                out_text(&format!("结果已写入 {}", p.display()));
                0
            }
            Err(e) => {
                err_text(&e);
                1
            }
        },
        None => {
            out_text(&body);
            0
        }
    }
}
