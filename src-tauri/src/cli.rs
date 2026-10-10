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
  luling plan [选项]
  luling archive [选项]

选项：
  --json                以 JSON 输出
  --out <文件>          把结果写入文件（发布版无控制台，脚本请用这个）
  --follow-links        跟随符号链接与 junction（默认否）
  --max-depth <N>       递归深度上限（默认不限）
  --limit <N>           计划预览最多显示多少条（默认 20）
  --exclude <文件名>    逐行剔除该条目（可重复；剔除行不参与编号且不跳号）
  --no-index            只扫描，不写入索引库（默认会写：增量索引 + 内容指纹）
  --keep <档位>         去重保留策略：earliest（默认）/ shortest / manual
  --template <模板>     命名模板（plan 用，默认 date_{seq}）
  --start <0|1>         序号起始值（默认 0；0 与 1 都必须可用）
  --pad <档位>          补零：none（默认）/ 2 / 3 / 1..6（固定位数，超限报警）
  --sep <分隔符>        序号前分隔符：_（默认）/ - / space / none
  --strip <档位>        原名序号剥离：none / digits / under（默认）/ regex
  --folder <文件夹>     只处理该相对文件夹（默认素材根一层）
  --root <路径>         指定素材根（默认取索引里的第一个）
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
    // 命名引擎也要能独立驱动（§12.2 强制项 ⑤）
    if args.first().map(|s| s.as_str()) == Some("plan") {
        return run_plan(&args[1..]);
    }
    // 归档计划也要能独立驱动（§12.2 强制项 ⑤）
    if args.first().map(|s| s.as_str()) == Some("archive") {
        return run_archive(&args[1..]);
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
fn file_name_of(p: &str) -> String {
    std::path::Path::new(p)
        .file_name()
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_default()
}

fn pad_label(p: crate::domain::naming::PadMode) -> String {
    use crate::domain::naming::PadMode;
    match p {
        PadMode::NoPad => "不补零".to_string(),
        PadMode::Min2 => "最少 2 位".to_string(),
        PadMode::Min3 => "最少 3 位".to_string(),
        PadMode::Fixed(n) => format!("固定 {n} 位"),
    }
}

/// `luling plan`（§12.2 强制项 ⑤：命名引擎必须能独立驱动）。
///
/// 只读：读索引 → 出计划 → 打印「旧名 → 新名 + 说明」。**不改名、不碰磁盘**（执行属 P6）。
fn run_plan(args: &[String]) -> i32 {
    use crate::app::naming::{build_plan, detect_cycles, PlanSource};
    use crate::domain::naming::{PadMode, SeqRule, SeqSep, StripRule};

    let mut json = false;
    let mut out_file: Option<PathBuf> = None;
    let mut template = "date_{seq}".to_string();
    let mut rule = SeqRule::default();
    let mut folder = String::new();
    let mut root_arg: Option<String> = None;
    let mut limit: Option<usize> = None;
    let mut excludes: Vec<String> = Vec::new();

    let mut i = 0usize;
    while i < args.len() {
        match args[i].as_str() {
            "--template" => {
                i += 1;
                match args.get(i) {
                    Some(v) => template = v.clone(),
                    None => {
                        err_text("错误：--template 需要一个模板文本（如 date_{seq}）。");
                        return 1;
                    }
                }
            }
            "--start" => {
                i += 1;
                match args.get(i).and_then(|v| v.parse::<u64>().ok()) {
                    Some(v) if v <= 1 => rule.start = v,
                    _ => {
                        err_text("错误：--start 只支持 0 或 1（两个都必须可用）。");
                        return 1;
                    }
                }
            }
            "--pad" => {
                i += 1;
                match args.get(i).map(|s| s.as_str()) {
                    Some("none") => rule.pad = PadMode::NoPad,
                    Some("2") => rule.pad = PadMode::Min2,
                    Some("3") => rule.pad = PadMode::Min3,
                    Some(v) => match v.parse::<u8>() {
                        Ok(n) if (1..=6).contains(&n) => rule.pad = PadMode::Fixed(n),
                        _ => {
                            err_text("错误：--pad 只支持 none / 2 / 3 / 1..6（固定位数）。");
                            return 1;
                        }
                    },
                    None => {
                        err_text("错误：--pad 需要一个值。");
                        return 1;
                    }
                }
            }
            "--sep" => {
                i += 1;
                match args.get(i).map(|s| s.as_str()) {
                    Some("_") => rule.sep = SeqSep::AutoUnderscore,
                    Some("-") => rule.sep = SeqSep::Dash,
                    Some("space") => rule.sep = SeqSep::Space,
                    Some("none") => rule.sep = SeqSep::None,
                    _ => {
                        err_text("错误：--sep 只支持 _ / - / space / none。");
                        return 1;
                    }
                }
            }
            "--strip" => {
                i += 1;
                match args.get(i).map(|s| s.as_str()) {
                    Some("none") => rule.strip = StripRule::None,
                    Some("digits") => rule.strip = StripRule::TrailingDigits,
                    Some("under") => rule.strip = StripRule::TrailingUnderscoreDigits,
                    Some("regex") => rule.strip = StripRule::Regex,
                    _ => {
                        err_text("错误：--strip 只支持 none / digits / under / regex。");
                        return 1;
                    }
                }
            }
            "--folder" => {
                i += 1;
                match args.get(i) {
                    Some(v) => folder = v.clone(),
                    None => {
                        err_text("错误：--folder 需要一个相对素材根的文件夹。");
                        return 1;
                    }
                }
            }
            "--root" => {
                i += 1;
                match args.get(i) {
                    Some(v) => root_arg = Some(v.clone()),
                    None => {
                        err_text("错误：--root 需要一个素材根路径。");
                        return 1;
                    }
                }
            }
            "--limit" => {
                i += 1;
                match args.get(i).and_then(|v| v.parse::<usize>().ok()) {
                    Some(v) if v >= 1 => limit = Some(v),
                    _ => {
                        err_text("错误：--limit 需要一个正整数。");
                        return 1;
                    }
                }
            }
            "--exclude" => {
                i += 1;
                match args.get(i) {
                    Some(v) => excludes.push(v.to_lowercase()),
                    None => {
                        err_text("错误：--exclude 需要一个文件名（可重复）。");
                        return 1;
                    }
                }
            }
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
            other => {
                err_text(&format!("错误：未知参数 `{other}`。用 --help 查看用法。"));
                return 1;
            }
        }
        i += 1;
    }

    let layout = crate::infra::library::layout();
    let conn = match crate::infra::db::open_library(&layout) {
        Ok(c) => c,
        Err(e) => {
            err_text(&format!("打不开索引库：{e}"));
            return 1;
        }
    };
    let roots = match crate::infra::db::scan_roots_all(&conn) {
        Ok(r) => r,
        Err(e) => {
            err_text(&format!("读取素材根失败：{e}"));
            return 1;
        }
    };
    let root = match root_arg.or_else(|| roots.first().cloned()) {
        Some(r) => r,
        None => {
            err_text("还没有扫描过任何素材根：先跑 `luling scan <文件夹>`。");
            return 1;
        }
    };
    let volume = crate::infra::volume::volume_id(std::path::Path::new(&root));
    // 索引里的 rel_path 是**相对卷根**的（§13.4），所以绝对路径要用卷挂载点拼，而不是用素材根拼
    let mp = crate::infra::volume::mount_point(std::path::Path::new(&root))
        .map(|p| p.to_string_lossy().trim_end_matches('\\').to_string())
        .unwrap_or_default();
    let root_rel = crate::infra::volume::rel_path_from_volume(std::path::Path::new(&root));
    let root_rel_prefix = format!("{}\\", root_rel.trim_matches('\\'));
    let rows = match crate::infra::db::assets_of_volume(&conn, &volume) {
        Ok(r) => r,
        Err(e) => {
            err_text(&format!("读取索引失败：{e}"));
            return 1;
        }
    };

    let prefix = if folder.trim().is_empty() {
        String::new()
    } else {
        folder.trim().replace('/', "\\").to_lowercase()
    };
    let root_clean = root.trim_end_matches('\\').to_string();
    let mut sources: Vec<PlanSource> = Vec::new();
    let mut existing: std::collections::HashSet<String> = std::collections::HashSet::new();
    for r in &rows {
        let rel_norm = r.rel_path.replace('/', "\\");
        let rel_lower = rel_norm.to_lowercase();
        // 只处理落在这个素材根之下的条目
        if !rel_lower.starts_with(&root_rel_prefix.to_lowercase()) {
            continue;
        }
        let rest = &rel_norm[root_rel_prefix.len()..];
        let rest_lower = rest.to_lowercase();
        let abs = format!("{mp}\\{rel_norm}");
        existing.insert(abs.to_lowercase());
        let in_folder = if prefix.is_empty() {
            !rest_lower.contains('\\')
        } else {
            rest_lower.starts_with(&format!("{prefix}\\"))
        };
        if !in_folder {
            continue;
        }
        let (stem, ext) = match r.name.rsplit_once('.') {
            Some((s, e)) => (s.to_string(), e.to_string()),
            None => (r.name.clone(), String::new()),
        };
        // 被逐行剔除的条目：不参与编号、也不跳号（§7.3.3）
        let excluded = excludes.iter().any(|x| x == &r.name.to_lowercase());
        sources.push(PlanSource {
            asset_id: r.id,
            abs,
            stem,
            ext,
            capture_time: None,
            mtime: r.mtime,
            ctime: 0,
            camera: None,
            width: None,
            height: None,
            group: None,
            parent: None,
            // 类别名（{kind}）在索引里有，但这一版 CLI 没取；界面侧的计划会带上真实值
            kind: String::new(),
            hash8: None,
            excluded,
        });
    }

    if sources.is_empty() {
        err_text(&format!(
            "`{root_clean}\\{folder}` 里没有被索引到的素材（先扫描该文件夹）。"
        ));
        return 1;
    }

    let mut plan = build_plan(&template, &rule, &sources, &existing);
    let hits = match crate::app::naming::annotate_references(&conn, &mut plan) {
        Ok(h) => h,
        Err(e) => {
            err_text(&format!("引用检查失败：{e}"));
            return 1;
        }
    };
    let cycles = detect_cycles(&plan);
    let (step1, _step2) = crate::app::naming::two_phase(&plan);

    let mut body = String::new();
    body.push_str(&format!("命名计划（模板 {template}）\n"));
    body.push_str(&format!(
        "  素材根 {root_clean} · 文件夹 {} · 参与编号 {} 项 · 剔除 {} 项\n",
        if folder.trim().is_empty() { "（根）" } else { folder.trim() },
        plan.items.iter().filter(|i| !i.excluded).count(),
        plan.items.iter().filter(|i| i.excluded).count()
    ));
    body.push_str(&format!(
        "  序号：起始 {} · {} · 作用于{:?} · 原名剥离{:?}\n",
        rule.start,
        pad_label(rule.pad),
        rule.scope,
        rule.strip
    ));
    if !cycles.is_empty() {
        body.push_str(&format!(
            "  {} 条构成环或大小写伪冲突：提交时会走两阶段改名（先改临时名再改目标名）\n",
            cycles.len()
        ));
    }
    if !hits.is_empty() {
        body.push_str(&format!(
            "  {} 条的新名字被引用命中：默认不改写引用，提交前需要确认\n",
            hits.len()
        ));
    }
    body.push_str(&format!(
        "  预览（共 {} 条，显示前 {} 条）：\n",
        plan.items.len(),
        limit.unwrap_or(20)
    ));
    for item in plan.items.iter().take(limit.unwrap_or(20)) {
        let old = file_name_of(&item.src);
        let new = if item.excluded {
            "（已剔除，不参与编号）".to_string()
        } else {
            file_name_of(&item.dst)
        };
        body.push_str(&format!("    {old} → {new}"));
        if !item.notes.is_empty() {
            body.push_str(&format!("   // {}", item.notes.join("；")));
        }
        body.push('\n');
    }
    body.push_str(&format!(
        "\n说明：本命令只读，不创建/修改/删除素材树里的任何文件；真实改名在提交阶段执行（本次计划里 {} 条会先改临时名再改目标名）。\n",
        step1.len()
    ));

    if json {
        match serde_json::to_string_pretty(&plan) {
            Ok(s) => {
                if let Some(f) = &out_file {
                    if let Err(e) = write_out(f, true, &s) {
                        err_text(&format!("写文件失败：{e}"));
                        return 1;
                    }
                } else {
                    out_text(&s);
                }
            }
            Err(e) => {
                err_text(&format!("序列化失败：{e}"));
                return 1;
            }
        }
    } else if let Some(f) = &out_file {
        if let Err(e) = write_out(f, false, &body) {
            err_text(&format!("写文件失败：{e}"));
            return 1;
        }
    } else {
        out_text(&body);
    }
    0
}

/// `luling archive`（§12.2 强制项 ⑤）：归档计划预览，**只读**——只算目标路径与冲突结论，不搬任何文件。
fn run_archive(args: &[String]) -> i32 {
    use crate::app::archive::{build_plan, ArchiveRequest, ArchiveSource, TargetFacts};
    use crate::domain::archive::ConflictPolicy;
    use crate::domain::naming::SeqRule;
    use std::collections::HashMap;

    let mut json = false;
    let mut out_file: Option<PathBuf> = None;
    let mut root_arg: Option<String> = None;
    let mut folder = String::new();
    let mut target = String::new();
    let mut dir_template = "{kind}/{yyyy}/{mm}".to_string();
    let mut name_template = "{name}_{seq}".to_string();
    let mut policy = ConflictPolicy::Suffix;
    let mut clean_empty_dirs = false;
    let mut limit: Option<usize> = None;

    let mut i = 0usize;
    while i < args.len() {
        match args[i].as_str() {
            "--root" => {
                i += 1;
                match args.get(i) {
                    Some(v) => root_arg = Some(v.clone()),
                    None => {
                        err_text("错误：--root 需要一个素材根路径。");
                        return 1;
                    }
                }
            }
            "--folder" => {
                i += 1;
                folder = args.get(i).cloned().unwrap_or_default();
            }
            "--target" => {
                i += 1;
                match args.get(i) {
                    Some(v) => target = v.clone(),
                    None => {
                        err_text("错误：--target 需要一个目标根目录。");
                        return 1;
                    }
                }
            }
            "--dir-template" => {
                i += 1;
                if let Some(v) = args.get(i) {
                    dir_template = v.clone();
                }
            }
            "--template" => {
                i += 1;
                if let Some(v) = args.get(i) {
                    name_template = v.clone();
                }
            }
            "--policy" => {
                i += 1;
                policy = match args.get(i).map(|s| s.as_str()) {
                    Some("suffix") => ConflictPolicy::Suffix,
                    Some("skip") => ConflictPolicy::Skip,
                    Some("abort") => ConflictPolicy::AbortBatch,
                    _ => {
                        err_text("错误：--policy 只支持 suffix / skip / abort。");
                        return 1;
                    }
                };
            }
            "--clean-empty-dirs" => clean_empty_dirs = true,
            "--limit" => {
                i += 1;
                limit = args.get(i).and_then(|v| v.parse::<usize>().ok());
            }
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
            other => {
                err_text(&format!("错误：未知参数 `{other}`。用 --help 查看用法。"));
                return 1;
            }
        }
        i += 1;
    }
    if target.trim().is_empty() {
        err_text("错误：必须给 --target <目标根目录>。");
        return 1;
    }

    let layout = crate::infra::library::layout();
    let conn = match crate::infra::db::open_library(&layout) {
        Ok(c) => c,
        Err(e) => {
            err_text(&format!("打不开索引库：{e}"));
            return 1;
        }
    };
    let roots = crate::infra::db::scan_roots_all(&conn).unwrap_or_default();
    let root = match root_arg.or_else(|| roots.first().cloned()) {
        Some(r) => r,
        None => {
            err_text("还没有扫描过任何素材根：先跑 `luling scan <文件夹>`。");
            return 1;
        }
    };
    let volume = crate::infra::volume::volume_id(std::path::Path::new(&root));
    let mp = crate::infra::volume::mount_point(std::path::Path::new(&root))
        .map(|p| p.to_string_lossy().trim_end_matches('\\').to_string())
        .unwrap_or_default();
    let root_rel_prefix = format!(
        "{}\\",
        crate::infra::volume::rel_path_from_volume(std::path::Path::new(&root)).trim_matches('\\')
    );
    let rows = match crate::infra::db::assets_of_volume(&conn, &volume) {
        Ok(r) => r,
        Err(e) => {
            err_text(&format!("读取索引失败：{e}"));
            return 1;
        }
    };
    let prefix = folder.trim().replace('/', "\\").to_lowercase();

    let mut sources: Vec<ArchiveSource> = Vec::new();
    let mut siblings: HashMap<String, Vec<String>> = HashMap::new();
    let mut referrers: HashMap<String, Vec<String>> = HashMap::new();
    for r in &rows {
        let rel_norm = r.rel_path.replace('/', "\\");
        if !rel_norm.to_lowercase().starts_with(&root_rel_prefix.to_lowercase()) {
            continue;
        }
        let rest = &rel_norm[root_rel_prefix.len()..];
        let rest_lower = rest.to_lowercase();
        let in_folder = if prefix.is_empty() {
            !rest_lower.contains('\\')
        } else {
            rest_lower.starts_with(&format!("{prefix}\\"))
        };
        if !in_folder {
            continue;
        }
        let abs = format!("{mp}\\{rel_norm}");
        let dir = match abs.rfind('\\') {
            Some(i) => abs[..i].to_string(),
            None => mp.clone(),
        };
        if !siblings.contains_key(&dir) {
            let mut names = Vec::new();
            if let Ok(rd) = std::fs::read_dir(&dir) {
                for e in rd.flatten() {
                    names.push(e.file_name().to_string_lossy().to_string());
                }
            }
            siblings.insert(dir.clone(), names);
        }
        if let Ok(list) = crate::infra::db::refs_for(&conn, &r.name.to_lowercase()) {
            if !list.is_empty() {
                referrers.insert(
                    r.name.to_lowercase(),
                    list.into_iter().map(|(p, _)| p).collect::<Vec<String>>(),
                );
            }
        }
        let (stem, ext) = match r.name.rsplit_once('.') {
            Some((s, e)) => (s.to_string(), e.to_string()),
            None => (r.name.clone(), String::new()),
        };
        // `{kind}` 需要类别名；索引里没取这一列，这里按扩展名给一个稳定口径（与界面分类同义）
        let kind = match ext.to_ascii_lowercase().as_str() {
            "jpg" | "jpeg" | "png" | "gif" | "bmp" | "webp" | "tif" | "tiff" | "heic" | "avif" => "image",
            "mp4" | "mov" | "mkv" | "avi" | "webm" | "wmv" | "flv" => "video",
            "mp3" | "wav" | "flac" | "aac" | "m4a" | "ogg" => "audio",
            "blend" | "blend1" | "blend2" | "max" | "ma" | "mb" | "c4d" | "ztl" | "fbx" | "obj"
            | "stl" | "3ds" => "3d",
            "pdf" | "doc" | "docx" | "xls" | "xlsx" | "ppt" | "pptx" | "txt" | "md" | "csv" => "doc",
            "zip" | "7z" | "rar" | "tar" | "gz" => "archive",
            _ => "other",
        }
        .to_string();
        sources.push(ArchiveSource {
            asset_id: r.id,
            volume_id: r.volume_id.clone(),
            abs,
            stem,
            ext,
            group: None,
            parent: None,
            kind,
            capture_time: None,
            mtime: r.mtime,
            ctime: 0,
            size: r.size,
            excluded: false,
        });
    }
    if sources.is_empty() {
        err_text(&format!("`{root}\\{folder}` 里没有被索引到的素材（先扫描该文件夹）。"));
        return 1;
    }

    let target_volume = crate::infra::volume::volume_id(std::path::Path::new(&target));
    let rule = SeqRule::default();
    // 第一遍：先算出每条的目标路径（冲突结论要等目标盘的事实查回来才能定）
    let empty_targets: TargetFacts = HashMap::new();
    let req1 = ArchiveRequest {
        target_root: &target,
        target_volume: &target_volume,
        dir_template: &dir_template,
        name_template: &name_template,
        rule: &rule,
        policy,
        clean_empty_dirs,
        targets: &empty_targets,
        siblings: &siblings,
        referrers: &referrers,
    };
    let pass1 = build_plan(&req1, &sources);

    // 第二遍：只读 stat 目标盘，把「存在 + 体积 + 修改时间」查进来
    let mut targets: TargetFacts = HashMap::new();
    for it in &pass1.items {
        if it.excluded || it.dst.is_empty() {
            continue;
        }
        if let Ok(md) = std::fs::metadata(&it.dst) {
            let size = md.len() as i64;
            let mtime = md
                .modified()
                .ok()
                .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
                .map(|d| d.as_millis() as i64)
                .unwrap_or(0);
            targets.insert(it.dst.replace('/', "\\").to_lowercase(), (size, mtime));
        }
    }
    let req2 = ArchiveRequest {
        targets: &targets,
        ..req1
    };
    let plan = build_plan(&req2, &sources);

    let mut body = String::new();
    body.push_str(&format!("归档计划（目录模板 {dir_template} · 命名模板 {name_template}）\n"));
    body.push_str(&format!(
        "  素材根 {} · 文件夹 {} · 目标根 {} · 参与 {} 项\n",
        root,
        if folder.trim().is_empty() { "（根）" } else { folder.trim() },
        target,
        plan.items.iter().filter(|i| !i.excluded).count()
    ));
    body.push_str(&format!(
        "  冲突策略 {:?} · 空目录清理 {} · 计划里 {} 条\n",
        plan.policy,
        if clean_empty_dirs { "开启" } else { "关闭（默认）" },
        plan.items.len()
    ));
    for n in &plan.notes {
        body.push_str(&format!("  · {n}\n"));
    }
    if !plan.empty_dirs.is_empty() {
        body.push_str("  归档后会变空的源目录（清理默认关闭）：\n");
        for d in plan.empty_dirs.iter().take(10) {
            body.push_str(&format!("    {d}\n"));
        }
    }
    body.push_str("  预览（旧路径 → 新路径）：\n");
    for it in plan.items.iter().take(limit.unwrap_or(20)) {
        let old = file_name_of(&it.src);
        if it.excluded {
            body.push_str(&format!("    {old} → （已剔除）\n"));
            continue;
        }
        let tag = if it.same_volume { "同盘" } else { "跨盘" };
        body.push_str(&format!("    {old} → {}   [{tag}]", it.dst));
        if !it.notes.is_empty() {
            body.push_str(&format!("   // {}", it.notes.join("；")));
        }
        body.push('\n');
    }
    body.push_str(
        "\n说明：本命令只读——只算目标路径与冲突结论，不移动/复制/删除任何文件；\n真正的搬运（同盘元数据操作、跨盘「复制 → 校验体积与修改时间 → 删除源 → 写 journal」）属 P6 提交执行。\n",
    );

    if json {
        match serde_json::to_string_pretty(&plan) {
            Ok(s) => {
                if let Some(f) = &out_file {
                    if let Err(e) = write_out(f, true, &s) {
                        err_text(&format!("写文件失败：{e}"));
                        return 1;
                    }
                } else {
                    out_text(&s);
                }
            }
            Err(e) => {
                err_text(&format!("序列化失败：{e}"));
                return 1;
            }
        }
    } else if let Some(f) = &out_file {
        if let Err(e) = write_out(f, false, &body) {
            err_text(&format!("写文件失败：{e}"));
            return 1;
        }
    } else {
        out_text(&body);
    }
    0
}

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
