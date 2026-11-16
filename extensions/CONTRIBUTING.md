# 发布与审核扩展

鹿铃的扩展目录由官方签名后分发。作者在**自己的仓库**发布包，向本仓库提交目录条目；维护者审核后用离线私钥签名目录。

## 作者流程

### 1. 准备扩展包

zip 根目录必须包含 `manifest.json`（插件清单），其余文件按 manifest 的 `main` 引用：

```
example-pack-1.0.0.zip
├── manifest.json
└── example.dll
```

`manifest.json` 至少包含：

```json
{
  "schema": 2,
  "id": "example-pack",
  "name": "示例扩展",
  "version": "1.0.0",
  "api_version": 1,
  "main": "example.dll",
  "capabilities": ["ui.command"]
}
```

### 2. 生成发布者密钥（首次）

```bash
python tools/extensions/sign.py generate \
    --private example-pack.private.hex \
    --public example-pack.public.hex
```

输出的 `publisher` 即发布者 id（公钥 blob 的 SHA-256）。私钥必须离线保管，**不要**提交到任何仓库；丢失后无法为已有扩展发布新版本，泄露后需要更换密钥并重新提交一个发布者条目。

### 3. 签名 manifest

```bash
python tools/extensions/sign.py sign \
    --private example-pack.private.hex \
    --domain manifest \
    --input manifest.json \
    --output example-pack-1.0.0.sig.json
```

签名覆盖 `Luling.Extension.Manifest.Signature\0v1\0` + `manifest.json` 的原始字节。`manifest.json` 一旦改动（包括空格和换行）都必须重新签名。

### 4. 发布到自己的 GitHub Release

把 `example-pack-1.0.0.zip` 与 `example-pack-1.0.0.sig.json` 上传到你的仓库 Release，记下两个下载 URL。

### 5. 生成目录条目

```bash
python tools/extensions/catalog.py entry \
    --package example-pack-1.0.0.zip \
    --signature example-pack-1.0.0.sig.json \
    --package-url https://github.com/<owner>/<repo>/releases/download/v1.0.0/example-pack-1.0.0.zip \
    --signature-url https://github.com/<owner>/<repo>/releases/download/v1.0.0/example-pack-1.0.0.sig.json \
    --version 1.0.0
```

输出的 JSON 直接放进 `extensions/index.json` 对应扩展的 `versions` 数组（最新版本放第一条）。

### 6. 提交 Pull Request

PR 需要修改两处：

- `extensions/index.json`：新增 `publishers` 条目（首次发布）和/或扩展条目；更新 `generated` 日期。
- 不要修改 `extensions/index.sig.json`，它由维护者签名。

CI 会运行 `catalog.py validate` 检查结构与官方发布者；合并后维护者重新签名。

## 维护者流程

1. 审核 PR：确认包来源可追溯、行为与说明一致、不静默联网上传数据、写文件经过鹿铃的文件操作。
2. 合并前在本地完整核验：

   ```bash
   python tools/extensions/catalog.py verify --index extensions/index.json
   ```

   该命令下载每个包与签名文件，核对大小、SHA-256、manifest 哈希、发布者签名与 manifest id/version。

3. 签目录（私钥离线保管）：

   ```bash
   python tools/extensions/catalog.py sign \
       --index extensions/index.json \
       --private <RSAFULLPRIVATEBLOB.hex>
   ```

4. 提交 `index.sig.json` 并推送。CI 在 `main` 上运行完整 `verify`，签名不一致会失败。

## 安全

- 原生扩展在独立宿主进程里运行，但仍以当前用户权限访问文件；能力授权在首次运行前由用户确认。
- 目录签名证明官方审核过条目；发布者签名证明包来自该发布者。两者都不是用户对发布者的信任，客户端首次安装时另行弹窗确认。
- 发现问题的版本应从目录移除并重新签名；已安装的副本会在客户端下次刷新时提示停用。
