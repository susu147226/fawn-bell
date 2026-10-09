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

选项：
  --json                以 JSON 输出扫描摘要
  --out <文件>          把结果写入文件（发布版无控制台，脚本请用这个）
  --follow-links        跟随符号链接与 junction（默认否）
  --max-depth <N>       递归深度上限（默认不限）
  --limit <N>           单次扫描文件数上限（默认 200000）
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
    let mut json = false;
    let mut out_file: Option<PathBuf> = None;
    let mut follow_links = false;
    let mut max_depth: Option<usize> = None;
    let mut limit: Option<u64> = None;
    let mut roots: Vec<String> = Vec::new();

    let mut i = 0usize;
    while i < args.len() {
        let a = args[i].as_str();
        match a {
            "scan" => {}
            "--json" => json = true,
            "--follow-links" => follow_links = true,
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
