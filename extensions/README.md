# 社区扩展目录

鹿铃的「设置 › 扩展商店 › 浏览商店」从本文件读取社区扩展列表：

```
https://raw.githubusercontent.com/susu147226/fawn-bell/main/extensions/catalog.json
```

## 格式

```json
{
  "version": 1,
  "extensions": [
    {
      "id": "example-pack",
      "name": "示例扩展",
      "desc": "一句话说明这个扩展做什么。",
      "version": "1.0.0",
      "size": 1048576,
      "url": "https://github.com/<owner>/<repo>/releases/download/v1.0.0/example-pack.zip",
      "sha256": "可选：安装包 SHA-256"
    }
  ]
}
```

- `id` 只能用字母、数字、`-`、`_`、`.`，最长 64 个字符；安装目录为
  `%LOCALAPPDATA%\Luling\extensions\<id>`。
- `url` 必须直接指向一个 zip 安装包；下载后用 Windows 自带的 `tar.exe` 解压到该目录。
- `sha256` 目前只记录在案，校验将在扩展宿主落地时启用。
- 条目缺 `id`、`name` 或 `url` 会被忽略。

## 提交

向本文件添加条目后提交 Pull Request。扩展包本身可以放在自己的 GitHub Release 里，
不需要随本仓库分发。

## 说明

当前版本只负责「浏览目录、下载并解压安装」。原生扩展的运行、权限与沙箱由后续的
扩展宿主负责；在宿主落地前，安装的扩展不会被执行。
