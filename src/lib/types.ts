/**
 * 后端 DTO 的 TypeScript 镜像（与 `src-tauri/src/app/scan.rs`、`src-tauri/src/ipc.rs` 的
 * `#[serde(rename_all = "camelCase")]` 一一对应）。改动 Rust 侧字段时这里必须同步。
 */

export type Kind = 'image' | 'video' | 'audio' | '3d' | 'doc' | 'other';

/** 稳定展示顺序（与 Rust `domain::kind::ALL` 一致）。 */
export const KINDS: Kind[] = ['image', 'video', 'audio', '3d', 'doc', 'other'];

export const KIND_LABEL: Record<Kind, string> = {
  image: '图片',
  video: '视频',
  audio: '音频',
  '3d': '3D',
  doc: '文档',
  other: '其他',
};

export interface AppInfo {
  name: string;
  version: string;
  identifier: string;
  author: string;
  copyright: string;
  libraryDir: string;
}

export interface KindStat {
  kind: Kind;
  count: number;
  bytes: number;
}

export interface KindCount {
  kind: Kind;
  count: number;
  bytes: number;
}

export interface BigDir {
  relPath: string;
  entries: number;
}

export interface ScanSummary {
  scanId: number;
  root: string;
  dirCount: number;
  fileCount: number;
  totalBytes: number;
  elapsedMs: number;
  byKind: KindStat[];
  warnings: string[];
  truncated: boolean;
  followedLinks: boolean;
  bigDirs: BigDir[];
  excluded: string[];
  libraryDir: string;
}

/** 文件夹行（库内聚合值，绝不为了渲染而实时统计磁盘——§8.5 性能条款）。 */
export interface DirAgg {
  /** 相对素材根的路径，`/` 分隔；素材根本身为空串。 */
  relPath: string;
  name: string;
  directFiles: number;
  directDirs: number;
  totalFiles: number;
  totalBytes: number;
  mtimeMs: number;
  cloud: boolean;
  kindCounts: KindCount[];
}

export interface FileRow {
  relPath: string;
  name: string;
  kind: Kind;
  size: number;
  mtimeMs: number;
  cloud: boolean;
}

export interface ScanResult {
  summary: ScanSummary;
  dirs: DirAgg[];
  files: FileRow[];
}

export interface ScanProgress {
  scanId: number;
  files: number;
  dirsDone: number;
  dirsPending: number;
  bytes: number;
  current: string;
  elapsedMs: number;
  etaMs: number | null;
  phase: string;
}

/** 索引结果（§7.1；P1 起每次扫描成功后由后端返回）。 */
export interface IndexReport {
  volumeId: string;
  /** 库内现有条目总数。 */
  entries: number;
  inserted: number;
  updated: number;
  unchanged: number;
  /** 本次没再出现、被标记为缺失的条数。 */
  missing: number;
  hashed: number;
  /** 本次重建的引用映射行数。 */
  refs: number;
  cancelled: boolean;
  db: string;
  warnings: string[];
}

export interface ScanDone {
  scanId: number;
  status: 'done' | 'cancelled' | 'failed';
  summary: ScanSummary | null;
  message: string | null;
  /** 索引结果；索引写库失败时为 null（见 indexError）。 */
  index: IndexReport | null;
  indexError: string | null;
}

export interface ScanSnapshot {
  running: number | null;
  progress: ScanProgress | null;
  hasResult: boolean;
}

/** 四档视图（§8.3：Ctrl+1 文件夹（默认）/ Ctrl+2 网格 / Ctrl+3 列表 / Ctrl+4 详情）。 */
export type ViewMode = 'folders' | 'grid' | 'list' | 'detail';

export const VIEW_LABEL: Record<ViewMode, string> = {
  folders: '文件夹',
  grid: '网格',
  list: '列表',
  detail: '详情',
};
