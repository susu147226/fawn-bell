//! 基础设施层 · 库目录解析与迁移（执行版 §10「存储」/ §13.1 / §13.4）。
//!
//! 架构强制项 ⑦：**库目录的解析只在这里发生**，其它模块不得自行拼路径。
//!
//! 落点规则（§13.4 第 3、4 条）：
//! - 默认 `%LOCALAPPDATA%\鹿铃\`；
//! - 设置里可改到**程序目录相对路径** `.\data\`（绿色版形态，随包搬走即带数据）；
//!   由程序目录下的标记文件 [`MARKER_FILE`] 驱动，因为「库在哪里」必须先于「读库里的设置」被知道；
//! - 便携位置不可写时**降级回默认位置并在界面明确提示**（[`Resolution::degraded`]），
//!   **绝不退化到素材目录**（§14 第 13 条）。

use std::fs;
use std::io;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

/// 库目录名（§10：默认位置 `%LOCALAPPDATA%\鹿铃\`）。
pub const APP_DIR_NAME: &str = "鹿铃";
/// 库位置标记文件（放在程序目录，内容是 [`LibraryLocation`]）。
pub const MARKER_FILE: &str = ".luling-library.json";
/// 库主文件名（单一 SQLite 库，§13.1）。
pub const DB_FILE: &str = "luling.db";

/// 库位置的两种形态（§10「存储」）。
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum LibraryLocation {
    /// 应用数据目录：`%LOCALAPPDATA%\鹿铃\`（默认）。
    AppData,
    /// 程序目录相对路径：`<程序目录>\data\`。
    Portable,
}

impl Default for LibraryLocation {
    fn default() -> Self {
        LibraryLocation::AppData
    }
}

impl LibraryLocation {
    pub fn as_str(self) -> &'static str {
        match self {
            LibraryLocation::AppData => "appData",
            LibraryLocation::Portable => "portable",
        }
    }
}

/// 库内各落点。**所有**库内文件的路径都必须从这里取（§12.2 强制项 ⑦）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LibraryLayout {
    /// 库根目录。
    pub root: PathBuf,
    /// SQLite 主库。
    pub db: PathBuf,
    /// 缩略图缓存（§10「缓存」：默认上限 500 MB / 20,000 张）。
    pub thumbs: PathBuf,
    /// 备份包（§13.4 第 2 条：默认保留最近 7 份）。
    pub backups: PathBuf,
    /// 整理报告（§7.8；报告落库目录，不落素材树）。
    pub reports: PathBuf,
    /// 日志（§11 操作日志保留）。
    pub logs: PathBuf,
    /// 提交 journal（§12.2 强制项 ④）。
    pub journal: PathBuf,
}

impl LibraryLayout {
    /// 由库根目录推出一整套落点。
    pub fn for_root(root: impl Into<PathBuf>) -> Self {
        let root = root.into();
        Self {
            db: root.join(DB_FILE),
            thumbs: root.join("thumbnails"),
            backups: root.join("backups"),
            reports: root.join("reports"),
            logs: root.join("logs"),
            journal: root.join("journal"),
            root,
        }
    }

    /// 需要建立的子目录（按建立顺序）。
    pub fn subdirs(&self) -> [&Path; 5] {
        [
            &self.thumbs,
            &self.backups,
            &self.reports,
            &self.logs,
            &self.journal,
        ]
    }
}

/// 解析结果：落点 + 是否发生了降级 + 可写性。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Resolution {
    pub layout: LibraryLayout,
    /// 当前实际采用的位置形态。
    pub location: LibraryLocation,
    /// 用户想要的位置（用于判断是否降级）。
    pub requested: LibraryLocation,
    /// 降级说明；`None` 表示按用户要求落地（§13.4 第 4 条要求把这句话显示给用户）。
    pub degraded: Option<String>,
    /// 程序目录（便携形态的基准）。
    pub program_dir: PathBuf,
}

impl Resolution {
    /// 是否与某个素材根重叠（§14 第 2 条：库目录不得位于素材树内）。
    pub fn overlaps(&self, asset_root: &Path) -> bool {
        crate::domain::guard::is_under(asset_root, &self.layout.root)
    }
}

fn env_path(key: &str) -> Option<PathBuf> {
    std::env::var_os(key)
        .map(PathBuf::from)
        .filter(|p| !p.as_os_str().is_empty())
}

/// 库目录的默认落点（§10）。
pub fn default_root() -> PathBuf {
    if let Some(local) = env_path("LOCALAPPDATA") {
        return local.join(APP_DIR_NAME);
    }
    if let Some(profile) = env_path("USERPROFILE") {
        return profile.join("AppData").join("Local").join(APP_DIR_NAME);
    }
    PathBuf::from("data")
}

/// 程序目录（便携形态基准）：可执行文件所在目录，取不到时退回当前工作目录。
pub fn program_dir() -> PathBuf {
    std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(Path::to_path_buf))
        .or_else(|| std::env::current_dir().ok())
        .unwrap_or_else(|| PathBuf::from("."))
}

/// 读程序目录里的位置标记；缺失或不可解析时按默认（应用数据目录）。
pub fn read_marker(program_dir: &Path) -> LibraryLocation {
    let path = program_dir.join(MARKER_FILE);
    let Ok(text) = fs::read_to_string(path) else {
        return LibraryLocation::default();
    };
    match serde_json::from_str::<MarkerFile>(&text) {
        Ok(m) => m.location,
        Err(_) => LibraryLocation::default(),
    }
}

#[derive(Debug, Serialize, Deserialize)]
struct MarkerFile {
    location: LibraryLocation,
}

/// 写位置标记（切换库位置时调用；只写程序目录，不写素材树）。
pub fn write_marker(program_dir: &Path, location: LibraryLocation) -> io::Result<()> {
    let path = program_dir.join(MARKER_FILE);
    let text = serde_json::to_string_pretty(&MarkerFile { location })
        .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?;
    fs::write(&path, text)
}

/// 目标目录是否可写：真建一个探针文件再删掉（比只看权限位可靠）。
pub fn is_writable(dir: &Path) -> bool {
    if fs::create_dir_all(dir).is_err() {
        return false;
    }
    let probe = dir.join(".luling-write-probe");
    match fs::write(&probe, b"probe") {
        Ok(()) => {
            let _ = fs::remove_file(&probe);
            true
        }
        Err(_) => false,
    }
}

/// 解析库位置（可注入环境，便于单测）。
pub fn resolve_with(
    program_dir: &Path,
    app_data_root: PathBuf,
    portable_writable_override: Option<bool>,
) -> Resolution {
    let requested = read_marker(program_dir);
    let portable_root = program_dir.join("data");
    let app_layout = LibraryLayout::for_root(app_data_root);

    match requested {
        LibraryLocation::Portable => {
            let writable = portable_writable_override.unwrap_or_else(|| is_writable(&portable_root));
            if writable {
                Resolution {
                    layout: LibraryLayout::for_root(portable_root),
                    location: LibraryLocation::Portable,
                    requested,
                    degraded: None,
                    program_dir: program_dir.to_path_buf(),
                }
            } else {
                // §13.4 第 4 条：降级到用户目录并明确提示
                Resolution {
                    layout: app_layout,
                    location: LibraryLocation::AppData,
                    requested,
                    degraded: Some(format!(
                        "程序目录不可写（`{}`），库已改到 `{}`；可在设置中指定固定位置。",
                        portable_root.display(),
                        LibraryLayout::for_root(default_root()).root.display()
                    )),
                    program_dir: program_dir.to_path_buf(),
                }
            }
        }
        LibraryLocation::AppData => Resolution {
            layout: app_layout,
            location: LibraryLocation::AppData,
            requested,
            degraded: None,
            program_dir: program_dir.to_path_buf(),
        },
    }
}

/// 解析库位置（真实环境）。
pub fn resolve() -> Resolution {
    resolve_with(&program_dir(), default_root(), None)
}

/// 建齐库目录结构（首次落盘；不碰素材树）。
pub fn ensure(layout: &LibraryLayout) -> io::Result<()> {
    fs::create_dir_all(&layout.root)?;
    for dir in layout.subdirs() {
        fs::create_dir_all(dir)?;
    }
    Ok(())
}

/// 库迁移结果（§13.4 第 3 条：迁移后校验条目数与可写性）。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct MigrateReport {
    pub source: PathBuf,
    pub target: PathBuf,
    pub files: usize,
    pub bytes: u64,
    /// 目标库里读到的素材条目数（用于「条目数校验」）。
    pub entries: i64,
    pub writable: bool,
}

/// 统计一棵树里的文件数与字节数。
fn tree_stat(root: &Path) -> io::Result<(usize, u64)> {
    let mut files = 0usize;
    let mut bytes = 0u64;
    let mut stack = vec![root.to_path_buf()];
    while let Some(dir) = stack.pop() {
        let Ok(entries) = fs::read_dir(&dir) else {
            continue;
        };
        for entry in entries.flatten() {
            let path = entry.path();
            let Ok(meta) = entry.metadata() else { continue };
            if meta.is_dir() {
                stack.push(path);
            } else if meta.is_file() {
                files += 1;
                bytes += meta.len();
            }
        }
    }
    Ok((files, bytes))
}

/// 把库整体搬到另一个根目录（§13.4 第 3 条）：先复制、校验、**保留旧目录**，由用户确认后再删。
pub fn migrate(from: &LibraryLayout, to_root: &Path) -> io::Result<MigrateReport> {
    let to = LibraryLayout::for_root(to_root);
    ensure(&to)?;

    let mut stack = vec![from.root.clone()];
    let mut files = 0usize;
    let mut bytes = 0u64;
    while let Some(dir) = stack.pop() {
        let entries = match fs::read_dir(&dir) {
            Ok(e) => e,
            Err(_) => continue,
        };
        for entry in entries.flatten() {
            let path = entry.path();
            let rel = path
                .strip_prefix(&from.root)
                .map(Path::to_path_buf)
                .unwrap_or_else(|_| PathBuf::from(entry.file_name()));
            let meta = match entry.metadata() {
                Ok(m) => m,
                Err(_) => continue,
            };
            if meta.is_dir() {
                fs::create_dir_all(to.root.join(&rel))?;
                stack.push(path);
            } else if meta.is_file() {
                // SQLite 的 -wal/-shm 一并搬，避免新库里丢掉未落盘的页
                fs::copy(&path, to.root.join(&rel))?;
                files += 1;
                bytes += meta.len();
            }
        }
    }

    let (expect_files, _) = tree_stat(&from.root)?;
    if files < expect_files {
        return Err(io::Error::new(
            io::ErrorKind::Other,
            format!("迁移不完整：源库 {expect_files} 个文件，只搬到 {files} 个"),
        ));
    }

    let entries = count_assets(&to.db).unwrap_or(-1);
    let writable = is_writable(&to.root);

    Ok(MigrateReport {
        source: from.root.clone(),
        target: to.root.clone(),
        files,
        bytes,
        entries,
        writable,
    })
}

/// 只读地取一个库文件里的素材条目数（迁移校验用；库不存在或不是库返回 Err）。
pub fn count_assets(db: &Path) -> Result<i64, String> {
    if !db.is_file() {
        return Err(format!("目标库里没有 {}", db.display()));
    }
    let conn = rusqlite::Connection::open_with_flags(
        db,
        rusqlite::OpenFlags::SQLITE_OPEN_READ_ONLY,
    )
    .map_err(|e| e.to_string())?;
    conn.query_row("SELECT COUNT(*) FROM assets", [], |row| row.get(0))
        .map_err(|e| e.to_string())
}

/// 缓存解析结果：进程内多次取库路径不应反复探磁盘。
pub fn cached() -> &'static Resolution {
    use std::sync::OnceLock;
    static CACHE: OnceLock<Resolution> = OnceLock::new();
    CACHE.get_or_init(resolve)
}

/// 库根目录（等价于 §13.1 的「应用数据目录」）。
pub fn root() -> PathBuf {
    cached().layout.root.clone()
}

/// 库 layout（进程内缓存）。
pub fn layout() -> LibraryLayout {
    cached().layout.clone()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fixture(name: &str) -> PathBuf {
        let dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("target")
            .join("luling-tests")
            .join(name);
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).expect("建测试目录");
        dir
    }

    #[test]
    fn 默认解析到应用数据目录() {
        let program = fixture("lib-default-program");
        let appdata = fixture("lib-default-appdata");
        let r = resolve_with(&program, appdata.clone(), None);
        assert_eq!(r.location, LibraryLocation::AppData);
        assert_eq!(r.requested, LibraryLocation::AppData);
        assert_eq!(r.degraded, None);
        assert_eq!(r.layout.root, appdata);
        assert_eq!(r.layout.db, appdata.join(DB_FILE));
    }

    #[test]
    fn 标记为便携且可写时落到程序目录的data() {
        let program = fixture("lib-portable-program");
        let appdata = fixture("lib-portable-appdata");
        write_marker(&program, LibraryLocation::Portable).expect("写标记");
        assert_eq!(read_marker(&program), LibraryLocation::Portable);
        let r = resolve_with(&program, appdata.clone(), Some(true));
        assert_eq!(r.location, LibraryLocation::Portable);
        assert_eq!(r.layout.root, program.join("data"));
        assert_eq!(r.degraded, None);
    }

    #[test]
    fn 便携位置不可写时降级并给出提示() {
        let program = fixture("lib-degrade-program");
        let appdata = fixture("lib-degrade-appdata");
        write_marker(&program, LibraryLocation::Portable).expect("写标记");
        let r = resolve_with(&program, appdata.clone(), Some(false));
        assert_eq!(r.location, LibraryLocation::AppData);
        assert_eq!(r.requested, LibraryLocation::Portable);
        assert_eq!(r.layout.root, appdata);
        assert!(r.degraded.as_deref().unwrap_or("").contains("程序目录不可写"));
    }

    #[test]
    fn 库落在素材根内可被识别() {
        let program = fixture("lib-overlap-program");
        let appdata = fixture("lib-overlap-appdata");
        let r = resolve_with(&program, appdata.clone(), None);
        // 库就在素材根里
        assert!(r.overlaps(&appdata));
        // 素材根在库的子目录里：属于 §14② 的另一半，由 guard::check_root 负责拒绝，
        // 这里只管「库位于素材树内」这一个方向，不能误判为重叠。
        assert!(!r.overlaps(&appdata.join("子目录")));
        // 与库同名前缀但不在其下 → 不算重叠（§14② 的边界）
        assert!(!r.overlaps(&PathBuf::from(format!("{}备份", appdata.display()))));
    }

    #[test]
    fn 建齐子目录() {
        let root = fixture("lib-ensure-root");
        let layout = LibraryLayout::for_root(root.join("库"));
        ensure(&layout).expect("建目录");
        for dir in layout.subdirs() {
            assert!(dir.is_dir(), "缺少子目录 {}", dir.display());
        }
    }

    #[test]
    fn 迁移复制全部文件并在目标库读到条目数() {
        let src_root = fixture("lib-migrate-src");
        let dst_root = fixture("lib-migrate-dst");
        let src = LibraryLayout::for_root(src_root.join("库"));
        // 建一个**真库**并写入 1 条素材，迁移后应能读到同样的条目数
        {
            let conn = super::super::db::open_library(&src).expect("建源库");
            super::super::db::upsert_asset(
                &conn,
                &super::super::db::AssetRecord {
                    volume_id: "v".into(),
                    rel_path: "a.png".into(),
                    name: "a.png".into(),
                    kind: "image".into(),
                    size: 1,
                    ..Default::default()
                },
            )
            .expect("写素材");
        }
        fs::write(src.thumbs.join("a.thumb"), b"0123456789").expect("写缩略图");
        fs::write(src.backups.join("b.zip"), b"01234").expect("写备份");

        let report = migrate(&src, &dst_root.join("库")).expect("迁移");
        assert!(report.files >= 1, "至少搬了库文件");
        assert!(report.bytes > 0);
        assert_eq!(report.entries, 1, "目标库里应能读到迁移过来的条目数");
        assert!(report.writable);
        // 旧目录保留（§13.4：保留旧目录直至用户确认删除）
        assert!(src.db.is_file());
        assert!(dst_root.join("库").join("thumbnails").join("a.thumb").is_file());
        assert!(dst_root.join("库").join("backups").join("b.zip").is_file());
    }
}
