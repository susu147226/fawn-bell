# 鹿铃（luling）

**就地整理素材的命名 / 位置 / 分组工具**——所有操作先进虚拟变更集，确认后才落到真实文件。

- 作者：云舒眠眠
- 许可证：**源码可见的专有软件**，不适用 MIT License（沿用作者既有私有许可证：允许个人非商业获取 / 编译 / 运行 / 本地修改；禁商业使用、禁再分发、禁移除署名、禁更换许可证、现状提供）
- 仓库：`git@github.com:susu147226/fawn-bell.git`

---

## 当前状态

| 项 | 状态 |
| --- | --- |
| 开发需求文档 | **执行版已出**（`docs/鹿铃-开发需求文档-执行版.md`）——**唯一权威文本**，可直接交给 AI 执行；共 18 章、48.0 人日 ±5、22 条验收 |
| 历史留档 | 需求文档 v1.2 与定稿版已收进 `docs/archive/`——仅供追溯，与执行版冲突时**以执行版为准** |
| UI 设计稿 | **8 张已出**（静态渲染图，默认 3200×1800；03 因左栏四组规则加高为 3200×2800，见 `docs/mockups/shots/`） |
| 可交互原型 | 8 个 HTML + 共用 `tokens.css`，可离线打开 |
| 图标资产 | **已就位（最终版）**：作者提供的 `assets/icon/鹿铃.ico`（7 帧 16→256，PNG 压缩 32 位带 alpha，自带奶白圆角方砖底），已抽出 `assets/icon/png/` 全尺寸 PNG 与 Tauri v2 命名集 |
| 代码 | **P0 骨架已完成**（3.0 人日）：Tauri v2 外壳、目录树、素材列表（虚拟滚动）、扫描与进度 / 取消、文件夹视图 · 混排列表；**扫描全程只读**，结果仅存内存，尚未接索引库 |

### P0 已完成的范围（对照执行版第十五章）

- 桌面外壳：1280×800（最小 960×600）单窗口，三栏布局（左栏目录树 / 中栏素材列表 / 右栏详情），左右栏可拖拽调宽并记住宽度，浅色 / 深色 / 跟随系统三档主题。
- 扫描：Rust 侧多线程遍历，进度事件节流 80 ms，可随时取消；只读，不创建 / 不修改 / 不删除素材树里任何东西。
- 素材列表：`@tanstack/react-virtual` 虚拟滚动，自然序（`Intl.Collator` numeric）排序，类型图标 + 类型 / 项数·大小 / 修改时间列。
- 独立命令行入口：`luling scan <文件夹> [--json] [--out <文件>]`，供脚本与自检使用。
- **尚未做**（P1 起）：索引库与增量扫描、缩略图、内容去重、变更集、命名引擎、真实文件操作。

---

## 文档索引

现行文档（本目录内）：

- [鹿铃-开发需求文档-执行版.md](docs/鹿铃-开发需求文档-执行版.md)——**唯一权威文本（执行版）**，交付 AI 直接执行；第一章「基本原则」为最高约束，第十七章为唯一的待确认清单
- [鹿铃-UI设计稿说明.md](docs/鹿铃-UI设计稿说明.md)——稿子清单、每张稿看什么、令牌体系与 Tailwind 对应、评审要点

设计稿渲染图（`docs/mockups/shots/`）：

- [01-main-light.png](docs/mockups/shots/01-main-light.png)——主界面 · 浅色 · 网格 · 三态标记
- [02-main-dark.png](docs/mockups/shots/02-main-dark.png)——主界面 · 深色 · 列表 · 变更集摘要
- [03-rename.png](docs/mockups/shots/03-rename.png)——批量重命名 · 命名模板预设 · 序号规则（不补零 / 自适应）· 字符级 diff（画布 1600×1400，30 行示例）
- [04-changeset-close.png](docs/mockups/shots/04-changeset-close.png)——变更集抽屉 + 关闭前提醒弹窗
- [05-appearance.png](docs/mockups/shots/05-appearance.png)——外观自定义设置页
- [06-protection.png](docs/mockups/shots/06-protection.png)——保护区视图与规则说明
- [07-folder-list.png](docs/mockups/shots/07-folder-list.png)——**文件夹视图 · 混排列表（默认视图）**
- [08-folder-cards.png](docs/mockups/shots/08-folder-cards.png)——**文件夹视图 · 卡片墙 + 多选批量面板**

历史版本（存放在 `docs/archive/` 与工作区根目录 `../`，仅作追溯，一律不再更新）：

- [docs/archive/鹿铃-开发需求文档-定稿.md](docs/archive/鹿铃-开发需求文档-定稿.md)——执行版的前一版
- [docs/archive/鹿铃-开发需求文档-v1.2.md](docs/archive/鹿铃-开发需求文档-v1.2.md)——含全部决策记录（D1–D33）
- [docs/archive/鹿铃-UI设计稿说明-v0.1.md](docs/archive/鹿铃-UI设计稿说明-v0.1.md)——含逐次修订补记
- 更早：[可行性文档 v0.1](../素材整理工具-可行性文档-v0.1.md)、[开发需求文档 v1.0](../素材整理工具-开发需求文档-v1.0.md)、[开发需求文档 v1.1](../素材整理工具-开发需求文档-v1.1.md)

**现行权威文本只有《鹿铃-开发需求文档-执行版.md》**，其余全部为留档。

---

## 技术栈

- **Tauri v2**（Rust 后端 + 系统 WebView 前端，安装包小、可长期驻留）；Rust 侧 `tauri 2.12.2`、`tauri-plugin-dialog 2.8.1`、`serde` / `serde_json 1`。
- **前端**：React 19.3.0 + TypeScript 6.0.3 + Vite 8.3.4（`@vitejs/plugin-react 6.1.2`）。
- **令牌化 CSS**：**`src/styles/tokens.css` 是唯一视觉真源**（唯一允许出现字面色值与时长的文件），组件类只允许引用 `var(--*)`，禁止写死色值 / 圆角 / 间距 / 时长；缺失令牌先在执行版第九章登记再全项目使用。设计稿件 `docs/mockups/tokens.css` 属历史稿子，其命名已在 `src/styles/tokens.css` 头部注释里登记收敛映射。
- **实际引入的第三方依赖**（均本地打包，无 CDN / 无网络字体）：`@tanstack/react-virtual 3.14.13`（虚拟滚动）、`lucide-react 1.53.0`（图标）、`@tauri-apps/api 2.12.2`、`@tauri-apps/plugin-dialog 2.8.1`。
- **已批准但尚未引入**（不引入也不违规，须遵守执行版 §12.4 落地约束）：Tailwind CSS v4、motion（备选 animate.css）、clsx / tailwind-merge、Floating UI、Pragmatic drag and drop 或 SortableJS、Fontsource。引入时：主题层必须映射到同一批 `var(--*)`，不得引入语义重复的第二套变量名；动画时长统一覆盖到 ≤250 ms；原生控件观感必须与第八节一致。**明确不引入**：daisyUI、Bootstrap / Bulma / Element Plus / Ant Design、GSAP、一切 CDN 与图标字体。
- **两条自动门禁**（`pnpm build` 内嵌，命中即报错）：`scripts/check-tokens.mjs`（检索源码与编译产物的十六进制色值、颜色函数、像素时长字面量、未登记令牌、远程地址；按 §12.4 第 2 条）、`scripts/license-audit.mjs`（逐版本打开随包 LICENSE 核对许可证，按 §12.4 第 5 条）。

## 两条硬约束

1. **零网络请求**——无 CDN、无网络字体，断网可用；外观设置同样不得触发任何网络请求。
2. **素材树零写入**——软件对用户素材树只读，改名 / 移动等只能在用户确认后按变更集执行；库、缓存、日志**全部**写在 `%LOCALAPPDATA%\鹿铃\`。

---

## 开发与构建

前置：Node.js + pnpm、Rust 工具链（edition 2021）。本项目使用 **pnpm 11**（corepack 提供，实测 `pnpm -v` → `11.22.0`）。首次拉取后：

```powershell
pnpm install
```

> **`pnpm-workspace.yaml` 是必须的，不要删**：pnpm 11 默认启用供应链检查 `minimumReleaseAge`（距发布不足 24 小时的包不许安装），会拦住锁文件里刚发布的依赖，导致 `pnpm install` 以及 `tauri dev` 前自动执行的依赖检查失败（`ERR_PNPM_MINIMUM_RELEASE_AGE_VIOLATION`）。该项目文件把这条设为 `0`。注意 pnpm 11 **不再从 `.npmrc` 读取 pnpm 自身的设置**，所以这条只能写在这里（`minimum-release-age=0` 写进 `.npmrc` 实测无效）。
>
> 另：若 node_modules 是用别的 pnpm 大版本装的，pnpm 会提示「The modules directory … will be removed and reinstalled from scratch」，在终端里答 Yes 即可（它只是重建 `node_modules/`，不动源码与锁文件）。

| 命令 | 作用 |
| --- | --- |
| `pnpm dev` | 只起前端 Vite 开发服务器（浏览器里看界面，无 Rust 后端） |
| `pnpm tauri dev` | 起完整桌面应用（前端 + Rust，推荐） |
| `pnpm build` | **发布构建**：源码令牌门禁 → `tsc` → `vite build` → 产物令牌门禁 |
| `pnpm build:web` | 跳过门禁的纯前端构建（排查用） |
| `pnpm check:tokens` | 单独跑令牌门禁（源码 + 产物） |
| `pnpm check:licenses` | 单独跑依赖许可证逐版本核对 |
| `pnpm tauri build` | 出安装包（**P9 前不要出正式安装包**，见执行版 §15） |

> **如果 PowerShell 报「无法加载文件 …`pnpm.ps1`，因为在此系统上禁止运行脚本」（执行策略 Restricted）**——这不是项目问题，是 Windows 默认执行策略禁止运行 `pnpm.ps1`。任选一种解法：
> 1. 用不受执行策略限制的 `pnpm.cmd`：`pnpm.cmd tauri dev`；
> 2. 在**命令提示符（cmd）**里执行：`pnpm tauri dev`；
> 3. 一次性放开当前用户的脚本执行：`Set-ExecutionPolicy -Scope CurrentUser RemoteSigned`（无需管理员）。
>
> `src-tauri/tauri.conf.json` 里的 `beforeDevCommand` / `beforeBuildCommand` 写的是 **`pnpm.cmd`**，所以 Tauri 自己拉起前端（`vite` / 发布构建）时不会受该策略影响。

Rust 侧自测与命令行：

```powershell
cd src-tauri
cargo test --lib          # 19 个单元测试（忽略规则 / 类型判定 / 汇总 / 遍历 / 守卫）
cargo build --bin luling
.\target\debug\luling.exe scan <素材文件夹> --json --out result.json
```

`luling scan` 的退出码：`0` 成功、`1` 失败、`2` 被取消；扫描全程只读（发布版无控制台，脚本请用 `--out` 收结果）。

---

## 许可与致谢

本软件为**源码可见的专有软件**：允许个人非商业获取、编译、运行与本地修改；**禁止商业使用、禁止再分发、禁止移除署名、禁止更换许可证**；按「现状」提供，不承担任何担保。Copyright © 2026 云舒眠眠。

> `LICENSE` 文件待补齐：按执行版第五章要求，仓库必须随附作者既有的私有许可证全文（仅替换项目名 / slug / 仓库地址）。该文本由作者提供，到位前本仓库**不得发布**。

第三方依赖的许可证**逐版本**按其随包 `LICENSE` 文件核对（执行版 §12.4 第 5 条：禁止凭记忆判定）。下表由 `node scripts/license-audit.mjs --readme` 生成，**空行不得发布**：

<!-- licenses:start -->

| 依赖 | 用途 | 版本 | 自带 LICENSE 文件 | 许可证 | 是否允许本项目分发 | 核对人 / 日期 |
| --- | --- | --- | --- | --- | --- | --- |
| `@tanstack/react-virtual` | 虚拟滚动（§12.4 已批准 TanStack Virtual） | 3.14.13 | LICENSE | MIT | 是 | license-audit.mjs / 2026-10-09 |
| `@tanstack/virtual-core` | 虚拟滚动内核 | 3.17.11 | LICENSE | MIT | 是 | license-audit.mjs / 2026-10-09 |
| `@tauri-apps/api` | 前后端 IPC（invoke / event） | 2.12.2 | LICENSE-APACHE-2.0 | Apache-2.0 | 是 | license-audit.mjs / 2026-10-09 |
| `@tauri-apps/plugin-dialog` | 选择素材文件夹对话框 | 2.8.1 | LICENSE.spdx | MIT OR Apache-2.0 | 是 | license-audit.mjs / 2026-10-09 |
| `lucide-react` | 图标（§12.4 已批准 Lucide） | 1.53.0 | LICENSE | ISC | 是 | license-audit.mjs / 2026-10-09 |
| `react` | UI 框架（§12.1 选型） | 19.3.0 | LICENSE | MIT | 是 | license-audit.mjs / 2026-10-09 |
| `react-dom` | UI 渲染 | 19.3.0 | LICENSE | MIT | 是 | license-audit.mjs / 2026-10-09 |
| `scheduler` | React 运行时依赖 | 0.28.0 | LICENSE | MIT | 是 | license-audit.mjs / 2026-10-09 |
| `@tauri-apps/cli` | Tauri 打包 / 开发命令（构建期） | 2.12.1 | LICENSE-APACHE-2.0 | Apache-2.0 | 是 | license-audit.mjs / 2026-10-09 |
| `@types/react` | React 类型定义（构建期） | 19.3.0 | LICENSE | MIT | 是 | license-audit.mjs / 2026-10-09 |
| `@types/react-dom` | ReactDOM 类型定义（构建期） | 19.3.0 | LICENSE | MIT | 是 | license-audit.mjs / 2026-10-09 |
| `@vitejs/plugin-react` | Vite React 插件（构建期） | 6.1.2 | LICENSE | MIT | 是 | license-audit.mjs / 2026-10-09 |
| `typescript` | TypeScript 编译（构建期） | 6.0.3 | LICENSE.txt | Apache-2.0 | 是 | license-audit.mjs / 2026-10-09 |
| `vite` | 构建工具（构建期，不进产物） | 8.3.4 | LICENSE.md | MIT | 是 | license-audit.mjs / 2026-10-09 |

核对备注：

- `@tauri-apps/plugin-dialog@2.8.1`：LICENSE 正文未匹配到已知许可证模板，已按元数据登记

<!-- licenses:end -->

---

## 工作量与下一步

- 全量工作量估算：**48.0 人日 ±5**（详见执行版第十五章）；**P0 已完成（3.0 人日）**。
- **下一步：P1「索引与元数据」8.0 人日** —— 扫描结果落 SQLite 索引库、增量扫描、库目录解析与重新定位向导、缩略图、元数据读取、内容去重（+2.5）、样式体系搭建与第三方样式库重置收敛（+0.5）。
- 第一段实施目标仍是 **P0–P3 约 22.0 人日**：完成 P3 后即可「安全批量改名」自用；P6 后可放心动真实素材；P8 后达到美观可自定义；P9 后可分发。

---

## 目录结构

```text
鹿铃/
├─ README.md                      # 本文件
├─ .gitignore
├─ index.html                     # 前端入口（zh-CN / data-theme）
├─ package.json / pnpm-lock.yaml  # 前端依赖与脚本
├─ pnpm-workspace.yaml            # pnpm 设置：minimumReleaseAge: 0（原因见文件内注释）
├─ tsconfig.json / tsconfig.node.json / vite.config.mts
├─ assets/
│  └─ icon/                       # 作者提供的 鹿铃.ico + 抽出的多尺寸 PNG（见目录内 README）
├─ scripts/
│  ├─ check-tokens.mjs            # 令牌门禁：源码与产物里的色值 / 时长 / 未登记令牌 / 远程地址
│  ├─ license-audit.mjs           # 依赖许可证逐版本核对（--readme 写回本文件表格）
│  ├─ verify-exec-doc.js          # 执行版文档结构自检（章号连续 / 引用可解析 / 工作量相加）
│  └─ audit-exec-refs.js          # 逐条列出「第X章 / 第X节」引用解析到的章标题
├─ src/                           # 前端（React + TypeScript）
│  ├─ main.tsx                    # 入口：装载三个样式表 + <App/>
│  ├─ App.tsx                     # 外壳与状态编排（扫描 / 选择 / 视图 / 主题 / 栏宽）
│  ├─ components/                 # Toolbar / Sidebar / Breadcrumb / TypeStats / ScanBanner /
│  │                              # AssetList / DetailPanel / StatusBar / EmptyState / Notice
│  ├─ lib/
│  │  ├─ types.ts                 # 与 Rust 侧逐字对齐的数据契约
│  │  ├─ ipc.ts                   # 命令与事件封装（app_info / scan_start / scan_cancel / …）
│  │  ├─ folders.ts               # 扫描结果 → 目录 / 文件索引（建表时排好自然序）
│  │  ├─ rows.ts                  # 列表行模型（文件夹行 / 文件行 / 折叠提示行）
│  │  ├─ format.ts                # 大小 / 数量 / 时长 / 时间格式化
│  │  ├─ icons.tsx                # 素材类型 → Lucide 图标
│  │  ├─ useResizable.ts          # 栏宽拖拽 + 记忆
│  │  └─ useTheme.ts              # 浅色 / 深色 / 跟随系统
│  └─ styles/
│     ├─ tokens.css               # 唯一视觉真源（令牌定义，唯一允许写字面值的文件）
│     ├─ base.css                 # 最小重置（不改动原生控件外观）
│     └─ app.css                  # 组件类（全部走 var(--*)）
├─ src-tauri/                     # Rust 后端
│  ├─ Cargo.toml / Cargo.lock / build.rs / tauri.conf.json
│  ├─ capabilities/default.json   # 窗口权限
│  ├─ icons/                      # Tauri 打包图标（含作者图标的命名集）
│  └─ src/
│     ├─ main.rs                  # 有参数走命令行，无参数起窗口
│     ├─ lib.rs                   # 装配与命令注册
│     ├─ cli.rs                   # `luling scan` 命令行入口
│     ├─ ipc.rs                   # scan_start / scan_cancel / scan_result / scan_snapshot + 进度事件
│     ├─ app/scan.rs              # 扫描编排（进度节流 / 取消 / ETA / 大目录与排除项统计）
│     ├─ domain/                  # kind（类型判定）/ ignore（忽略规则）/ aggregate（汇总）/ guard（守卫）
│     └─ infra/walker.rs          # 只读遍历（不跟随符号链接、不读云占位内容）
└─ docs/
   ├─ 鹿铃-开发需求文档-执行版.md    # 唯一权威文本（执行版）
   ├─ 鹿铃-UI设计稿说明.md          # 稿子索引与评审说明
   ├─ archive/                     # 历史留档（不再更新）
   │  ├─ 鹿铃-开发需求文档-定稿.md
   │  ├─ 鹿铃-开发需求文档-v1.2.md
   │  └─ 鹿铃-UI设计稿说明-v0.1.md
   └─ mockups/                     # 可交互原型 + 渲染 / 自检脚本
      ├─ 01-main-light.html         # 8 张稿子：浅色 / 深色 / 重命名 /
      ├─ 02-main-dark.html          #   变更集关闭 / 外观 / 保护区 /
      ├─ 03-rename.html             #   文件夹视图（列表） / 文件夹视图（卡片）
      ├─ 04-changeset-close.html
      ├─ 05-appearance.html
      ├─ 06-protection.html
      ├─ 07-folder-list.html
      ├─ 08-folder-cards.html
      ├─ tokens.css                 # 设计稿用的历史令牌（现行真源见 src/styles/tokens.css）
      ├─ shot.ps1                   # 无头 Chrome 渲染脚本
      ├─ static-check.js            # 静态检查（04 / 05 / 06）
      ├─ static-check-123.js        # 静态检查（01 / 02 / 03）
      ├─ folder-check.js            # 07/08 专用静态检查
      ├─ folder-classcheck.js       # 07/08 类定义完整性核对
      ├─ audit-rename03-height.js   # 03 左栏高度实测审计
      ├─ audit-detail-height.js     # 右栏详情纵向几何审计
      ├─ measure-geometry.js        # 浏览器探针实测（DOM 尺寸）
      └─ shots/                     # 渲染图 + 自检报告
         ├─ 01-main-light.png … 08-folder-cards.png   # 8 张设计稿（01/02/04–08 为 3200×1800，03 为 3200×2800）
         ├─ selfcheck.ps1           # 结构 / 令牌纪律自检
         ├─ static-check.txt        # 自检报告（开发自用，非交付物）
         ├─ static-check-123.txt    # 01 / 02 / 03 自检报告
         ├─ folder-check.txt        # 07/08 自检报告
         └─ rename-03-height.txt    # 03 高度实测审计输出
```

> 渲染设计稿须在 `docs/mockups/` 目录下执行：先 `cd 鹿铃\docs\mockups`，再
> `powershell -NoProfile -ExecutionPolicy Bypass -File .\shot.ps1 -Html .\03-rename.html -Png .\shots\03-rename.png`
