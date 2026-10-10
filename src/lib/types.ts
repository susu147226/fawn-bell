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
  /** 上次缺失、这次又看见并恢复的条数。 */
  unmissed: number;
  hashed: number;
  /** 本次重建的引用映射行数。 */
  refs: number;
  cancelled: boolean;
  db: string;
  warnings: string[];
}

/** Shell 扩展属性的一条（键名 → 值，§6.4）。 */
export interface MetaProp {
  key: string;
  value: string;
}

/** 索引里的素材元数据（§6.4：图片走 EXIF、其余走 Shell 属性）。 */
export interface AssetMeta {
  width: number | null;
  height: number | null;
  captureTime: number | null;
  camera: string | null;
  gpsLat: number | null;
  gpsLon: number | null;
  orientation: number | null;
  /** 索引后被外部删除/移动（§7.1：元数据仍保留）。 */
  missing: boolean;
  props: MetaProp[];
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

/** 内容去重的一个成员（§7.12）。 */
export interface DuplicateMember {
  assetId: number;
  relPath: string;
  absPath: string | null;
  size: number;
  ctime: number;
  keeper: boolean;
}

export interface DuplicateGroup {
  size: number;
  hashPartial: string;
  members: DuplicateMember[];
  wasteBytes: number;
}

export interface DedupeReport {
  groups: DuplicateGroup[];
  groupCount: number;
  duplicateCount: number;
  keepers: number;
  wasteBytes: number;
  candidates: number;
  policy: 'earliestCreated' | 'shortestPath' | 'manual';
  collectionId: number | null;
  warnings: string[];
  cancelled: boolean;
}

/** 重定位的三档置信度（§13.4）。 */
export type RelocateConfidence = 'high' | 'needsConfirm' | 'unmatched';

export interface RelocateMatch {
  assetId: number;
  confidence: RelocateConfidence;
  why: string;
  oldRelPath: string;
  newRelPath: string | null;
}

export interface RelocatePlan {
  oldRoot: string;
  newRoot: string;
  oldVolume: string;
  newVolume: string;
  matches: RelocateMatch[];
  high: number;
  needsConfirm: number;
  unmatched: number;
}

/** 库位置信息（§13.1 / §13.4）。 */
export interface LibraryInfo {
  root: string;
  db: string;
  thumbs: string;
  backups: string;
  location: 'appData' | 'portable';
  degraded: string | null;
}

/* ── 虚拟变更集（§7.2） ─────────────────────────────────────────── */

export type DraftOp =
  | 'rename'
  | 'move'
  | 'copy'
  | 'mkdir'
  | 'rmdir'
  | 'trash'
  | 'retag'
  | 'rewriteRef';

/** 预检结论（§13.3 drafts.check_status）。 */
export type CheckStatus =
  | 'ok'
  | 'conflict'
  | 'illegal'
  | 'tooLong'
  | 'outOfRoot'
  | 'protected'
  | 'referenced';

export interface Draft {
  seq: number;
  op: DraftOp;
  assetId: number | null;
  src: string;
  dst: string | null;
  check: CheckStatus;
  reason: string | null;
}

export interface DraftList {
  drafts: Draft[];
  count: number;
  problems: number;
  /** 重做栈里还有几条（状态条的重做按钮据此启用）。 */
  redo: number;
}

/** 投影结果（§7.2：真实状态 + 草稿 → 界面显示的样子）。 */
export interface Projection {
  path: string;
  drafted: boolean;
  removed: boolean;
}

/* ── 命名引擎（§7.3） ─────────────────────────────────────────────── */

/** 补零档位（§7.3.3）：不补零（默认）/ 最少 2 位 / 最少 3 位 / 固定 N 位。 */
export type PadMode = 'noPad' | 'min2' | 'min3' | { fixed: number };
export type SeqSep = 'autoUnderscore' | 'dash' | 'space' | 'none';
export type SeqScope = 'currentFolder' | 'currentGroup' | 'selectedSet';
export type StripRule = 'none' | 'trailingDigits' | 'trailingUnderscoreDigits' | 'regex';

export interface SeqRule {
  start: number;
  sep: SeqSep;
  pad: PadMode;
  scope: SeqScope;
  strip: StripRule;
}

/** 必须与核心域 `SeqRule::default()` 对齐：起始 0、不补零、自动 `_`、剥离尾随 `_数字`。 */
export const DEFAULT_SEQ_RULE: SeqRule = {
  start: 0,
  sep: 'autoUnderscore',
  pad: 'noPad',
  scope: 'currentFolder',
  strip: 'trailingUnderscoreDigits',
};

/** 命名预设（§7.3.2）：内置 13 个不可删，自定义可增删改排。 */
export interface Preset {
  id: number;
  baseName: string;
  label: string | null;
  template: string;
  isBuiltin: boolean;
  sortOrder: number;
}

/** 预设套用结果：模板 + **立刻**给出的前三项预览。 */
export interface PresetApply {
  template: string;
  preview: string[];
}

/** 模板渲染结果（notes 里是必须让用户看见的提醒，如固定档超限、截断提示）。 */
export interface Rendered {
  name: string;
  notes: string[];
}

/* ── 分组与保护区（§7.5 / §7.6） ─────────────────────────────────── */

/** 分组：`static` 手工挑选，`smart` 保存规则动态求值。 */
export interface Group {
  id: number;
  name: string;
  kind: string;
  ruleJson: string | null;
  color: string | null;
  sortOrder: number;
  /** 成员数：静态分组是成员表计数；内置「重复内容」由去重用例物化。 */
  memberCount: number;
}

/** 保护区条目：横切安全属性，独立于分组。 */
export interface Protection {
  assetId: number;
  /** auto（提交成功后自动加入）/ manual（手工加入）。 */
  addedBy: string;
  addedAt: number;
  reason: string | null;
}

/** 保护区统计（§7.6：总数 + 当周新增并列显示）。 */
export interface ProtectionStats {
  total: number;
  weekNew: number;
}

/** 「已跳过 N 项（受保护）」明细（§7.6 第 1 点：名称、路径、加入时间与方式）。 */
export interface SkippedProtected {
  assetId: number;
  name: string;
  relPath: string;
  volumeId: string;
  addedBy: string;
  addedAt: number;
  reason: string | null;
}
