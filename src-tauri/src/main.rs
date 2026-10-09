// Prevents additional console window on Windows in release, DO NOT REMOVE!!
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    // 架构强制项 ⑤（执行版 §12.2）：引擎可独立驱动。
    // 带参数启动 = CLI 模式（`luling scan <目录> …`），不带参数 = 图形界面。
    let args: Vec<String> = std::env::args().skip(1).collect();
    if !args.is_empty() {
        std::process::exit(luling_lib::cli::run(&args));
    }
    luling_lib::run();
}
