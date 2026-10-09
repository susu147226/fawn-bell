# 图标资产

本目录存放应用图标。**图标由作者提供**（2026-10 上传的 `鹿铃图标设计.ico`，为最终采用版本），下方为落地记录。

## 文件

| 文件 | 说明 |
| --- | --- |
| `鹿铃.ico` | **作者提供的最终图标**（172404 字节，7 帧，全部为 PNG 压缩的 32 位带 alpha），未做任何改动 |
| `png/icon-16.png` … `png/icon-256.png` | 从原始 ICO 中**逐帧原样抽出**（未重采样、未重新编码）：16 / 24 / 32 / 48 / 64 / 128 / 256 |
| `png/32x32.png`、`png/128x128.png`、`png/128x128@2x.png`、`png/icon.png` | 按 **Tauri v2** 约定命名的同一批帧（分别取自 32 / 128 / 256 / 256），接入时直接复制到 `src-tauri/icons/` |
| `png/icon.ico` | 原始 ICO 的副本，接入时复制为 `src-tauri/icons/icon.ico` |
| `png/_icon-legibility-compare.png` | 浅色 / 深色背景、16–48px 实际像素 + 32px 四倍放大的可见性校样（**开发自用，`.gitignore` 已排除 `png/_*`**） |
| `png/_icon-v1-upload.ico` | 作者先前上传的第一版图标（透明底白鹿），仅留档，不参与构建 |

原始 ICO 的帧清单（`ICONDIR` 实测）：

| 序 | 尺寸 | 位深 | 字节数 | 存储形式 |
| --- | --- | --- | --- | --- |
| 0 | 16×16 | 32 | 912 | PNG |
| 1 | 24×24 | 32 | 1790 | PNG |
| 2 | 32×32 | 32 | 2949 | PNG |
| 3 | 48×48 | 32 | 6011 | PNG |
| 4 | 64×64 | 32 | 9690 | PNG |
| 5 | 128×128 | 32 | 32945 | PNG |
| 6 | 256×256 | 32 | 117989 | PNG |

透明底自检：`icon-256.png` 角点 alpha=0 / 中心 alpha=254，`icon-16.png` 角点 alpha=1 / 中心 alpha=254 —— 圆角方砖外为透明，砖内为实色奶白，**无白底残留**。

**关于浅色背景的可见性（已复核，无需额外处理）**：这一版的图标自带奶白圆角方砖底（小鹿 + 绿鹿角 + 彩点），在浅色与深色背景上 16–48px 都能立住（见 `png/_icon-legibility-compare.png`）。因此**不需要**为小尺寸另垫青绿底——先前讨论过的 A / B / C 三方案作废。

## 接入 Tauri 时需要做的事（P0 落地时执行）

1. 把 `png/32x32.png`、`png/128x128.png`、`png/128x128@2x.png`、`png/icon.png`、`png/icon.ico` 复制到 `src-tauri/icons/`（本项目仅发布 Windows，**不需要** macOS 的 `icon.icns`）。
2. `tauri.conf.json` 的 `bundle.icon` 按上表列出这四个 PNG 与 `icon.ico`。
3. 托盘图标、窗口标题栏图标复用 `icon.ico`（Windows 会按 DPI 从 ICO 里挑合适帧），不再另做一套。
