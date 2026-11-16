# 社区扩展目录

鹿铃的「设置 › 扩展商店 › 浏览商店」读取**签名的目录索引**：

```
extensions/index.json       目录索引（发布者、扩展、版本）
extensions/index.sig.json   RSA-PKCS1-SHA256 签名
```

客户端内置官方发布者公钥，只显示通过校验的目录；校验失败时回退到本地最后一次验证通过的副本，两者都不可用才显示「离线」。

## 工作方式

```
publishers/<id>        发布者，id = 公钥 blob 的 SHA-256
extensions/<id>        扩展的每个版本：包 URL、大小、SHA-256、manifest 哈希、发布者签名
tools/extensions/      校验、验签、签名工具
```

安装时按顺序校验：

1. 目录签名（官方密钥）
2. 包大小与 SHA-256
3. 包内 `manifest.json` 的 SHA-256
4. 发布者对 `manifest.json` 的签名
5. manifest 的 `id` / `version` 与目录一致

首次安装某个发布者的扩展时，客户端会显示发布者名称与指纹，要求用户**显式信任**该发布者。目录签名只证明官方审核过，不等于用户同意第三方代码运行。

## 索引格式

```json
{
  "formatVersion": 1,
  "generated": "2026-11-16",
  "publishers": [
    {
      "id": "<公钥 blob 的 SHA-256>",
      "name": "Luling",
      "publicKey": "<CNG RSAPUBLICBLOB，十六进制>",
      "url": "https://github.com/...",
      "official": true
    }
  ],
  "extensions": [
    {
      "id": "example-pack",
      "publisher": "<publisher id>",
      "name": "示例扩展",
      "summary": "一句话说明。",
      "description": "完整说明。",
      "category": "fileops",
      "license": "MIT",
      "homepage": "https://...",
      "source": "https://...",
      "tags": ["示例"],
      "versions": [
        {
          "version": "1.0.0",
          "released": "2026-11-16",
          "minLuling": "0.5.8",
          "manifestSha256": "<包内 manifest.json 的 SHA-256>",
          "package": {"url": "https://github.com/<owner>/<repo>/releases/download/...", "size": 12345, "sha256": "<zip 的 SHA-256>"},
          "signature": {"url": "https://github.com/.../example-pack-1.0.0.sig.json", "size": 456, "sha256": "<签名文件的 SHA-256>"},
          "notes": "本版说明。"
        }
      ]
    }
  ]
}
```

约束：

- `official: true` 只能用于内置的官方发布者，其他条目会被客户端和工具拒绝。
- `package.url` 必须位于 `https://github.com/`。
- 每个发布者的 `id` 必须等于其 `publicKey` 的 SHA-256。
- `versions` 按新到旧排列，客户端取第一条作为最新版本。

## 发布与审核

见 [CONTRIBUTING.md](CONTRIBUTING.md)。

## 工具

```bash
# 只校验结构与官方发布者
python tools/extensions/catalog.py validate --index extensions/index.json

# 校验签名，并下载每个包和签名文件做完整核对
python tools/extensions/catalog.py verify --index extensions/index.json

# 用官方私钥签名（维护者）
python tools/extensions/catalog.py sign --index extensions/index.json --private <RSAFULLPRIVATEBLOB.hex>
```

签名工具为 `tools/extensions/sign.py`，与客户端 `src/app/store_signature.cpp` 的字节格式一致（域分隔前缀 + RSA-PKCS1-SHA256）。

官方私钥不进入仓库，由维护者离线保管；泄露后需要更换内置公钥并发布客户端更新。
