# Notion Clipboard Win

语言：中文 | [English](README.en.md)

Notion Clipboard Win 是一个 Windows 原生剪贴板上传工具。它常驻系统托盘，通过热键或剪贴板监听读取内容，将 Markdown、代码块、LaTeX 公式、表格和常见 HTML 剪贴板片段转换后上传到配置的目标后端。

项目当前聚焦 Notion 和 Obsidian 两条稳定写入路径。程序使用 C++17、Win32 和 WinHTTP 实现，不依赖第三方运行时。

## 主要能力

- 系统托盘常驻，默认热键 `Ctrl+Shift+B` 上传当前剪贴板。
- 可选自动监听剪贴板，支持 debounce 和短时间重复抑制。
- 内置本地配置页面，可从托盘菜单打开，预填当前配置、验证配置并输出完整 ini。
- 上传前先写入持久队列，断网、退出或远端限流后可继续重试。
- 记录 `remote_id`、`remote_url` 和 `remote_progress`，支持跨后端断点恢复。
- 支持 Notion `Retry-After`、HTTP 短重试和持久队列指数退避。
- 支持自定义热键、托盘通知、开机自启和状态目录。
- 记录最近上传结果，可查看 Notion URL、Obsidian 文件路径、本地文件链接和 Obsidian URI。
- 支持从托盘菜单打开最近一次 Obsidian 笔记。
- 支持从托盘菜单或配置页面验证当前配置，并输出 Notion/Obsidian 诊断报告。
- 转换器覆盖 Markdown、HTML、公式、表格、代码语言归一化和常见误识别保护。

## 上传目标

`upload_target` 支持单目标，也支持逗号分隔的多目标，例如 `upload_target=notion,obsidian`。多目标上传时，每个目标会进入独立队列任务，便于单独重试和记录远端结果。

| `upload_target` | 说明 | 必要配置 |
| --- | --- | --- |
| `notion` | 创建 Notion 数据源页面并追加正文 block | `notion_token`、`data_source_id` 或 `database_id` |
| `obsidian` | 写入 Obsidian vault 中的 Markdown 笔记 | `obsidian_vault_dir` |

Webhook、语雀和飞书文档会作为未来/实验平台继续打磨；当前配置页不把它们作为稳定入口。

## 支持的内容

- 普通文本、标题、段落、分隔线。
- Markdown 列表、任务列表、引用块、代码块和链接。
- Markdown 表格、无分隔行管道表格，以及被空语言或 `text` 围栏包住的表格。
- 行内公式：`$...$`、`\(...\)`。
- 独立公式：`$$...$$`、`\[...\]`、`equation` / `align` / `gather` 环境。
- KaTeX / MathJax HTML annotation 中的 TeX 公式。
- 常见 Unicode 数学符号的保守 LaTeX 修复。
- inline code、URL、Windows 路径和字面量 `$` 的保护，减少公式误判。

## 项目结构

```text
notion_clipboard_win/
  CMakeLists.txt          CMake 构建入口
  build-release.bat       Release 托盘版构建脚本
  VERSION                 发布版本号的单一来源
  config.example.ini      配置模板，不包含真实凭据
  LICENSE                 MIT 开源许可证
  installer/              Inno Setup 安装包脚本
  scripts/                发布构建脚本
  src/                    C++17 Win32 源码
    main.cpp              托盘、CLI、剪贴板、上传线程
    converter.cpp         Markdown/HTML/LaTeX 转换与自测
    upload_target.cpp     Notion、Obsidian 和实验上传后端
    config_page.cpp       本地 HTML 配置页面
  test/                   转换回归样例
```

## 快速开始

### 使用安装包

从 GitHub Releases 下载 `NotionClipboardWin-0.2.0-Setup.exe`，安装后从开始菜单启动 `Notion Clipboard Win`。安装包不会创建或覆盖你的真实 `notion_clipboard_win.ini`，首次使用仍需要准备配置文件。

### 从源码运行

复制配置模板：

```powershell
copy config.example.ini notion_clipboard_win.ini
```

填写 Notion 最小配置：

```ini
upload_target=notion
notion_token=secret_xxx
data_source_id=xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
hotkey=Ctrl+Shift+B
```

同时写入 Notion 和 Obsidian：

```ini
upload_target=notion,obsidian
notion_token=secret_xxx
data_source_id=xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
obsidian_vault_dir=E:\obsidian\第一个库
obsidian_folder=Inbox/Clipboard
obsidian_tags=algorithm cpp
```

也可以通过环境变量保存敏感配置：

```powershell
setx NOTION_TOKEN "secret_xxx"
setx NOTION_DATA_SOURCE_ID "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
```

构建并启动：

```powershell
.\build-release.bat
.\build\Release\notion_clipboard_win.exe --config .\notion_clipboard_win.ini
```

复制文本后按 `Ctrl+Shift+B` 上传，或右键托盘图标选择“上传当前剪贴板”。写入 Obsidian 成功后，可从托盘菜单选择“打开最近 Obsidian 笔记”直接定位新文件。

## Notion 准备

1. 在 Notion 创建 integration 并获取 token。
2. 将目标数据库共享给该 integration。
3. 推荐填写 `data_source_id`。如果只填写旧版 `database_id`，程序会通过 Notion API 解析第一个 data source。
4. 目标数据源至少需要一个 `title` 类型属性。

本项目按 Notion API `2026-03-11` 实现，创建数据库页面时使用 `parent.type = "data_source_id"`。

## 构建与运行

本项目需要 Windows、CMake，以及 MSVC 或 MinGW。

构建 GUI 托盘版：

```powershell
cmake -S . -B build
cmake --build build --config Release
```

构建带控制台的调试版：

```powershell
cmake -S . -B build-console -DNOTION_CLIPBOARD_WIN_GUI=OFF
cmake --build build-console --config Release
```

常用命令：

```powershell
.\build\Release\notion_clipboard_win.exe --config .\notion_clipboard_win.ini
.\build\Release\notion_clipboard_win.exe --once --config .\notion_clipboard_win.ini
.\build\Release\notion_clipboard_win.exe --validate-config --config .\notion_clipboard_win.ini
.\build\Release\notion_clipboard_win.exe --help
```

生成安装包需要 Inno Setup 6：

```powershell
winget install JRSoftware.InnoSetup
.\scripts\build-installer.ps1
```

脚本会构建 console 版、运行 `--self-test`、构建 GUI Release 版，然后生成：

```text
dist\NotionClipboardWin-0.2.0-Setup.exe
dist\NotionClipboardWin-0.2.0-Setup.exe.sha256
```

## 配置

推荐先从托盘菜单打开“配置页面”。页面会预填当前配置，支持同时选择 Notion 和 Obsidian、自动列出 Obsidian vault 和子目录、显示写入位置预览、验证当前输出配置、查看诊断与最近上传结果、显示/隐藏 token、复制完整 ini、下载 ini，以及“应用并重启”写回当前配置文件。

核心配置示例：

```ini
upload_target=notion
notion_token=
data_source_id=
database_id=
title_property_name=
content_property_name=
created_time_property_name=创建时间
hotkey=Ctrl+Shift+B
enable_hotkey=true
enable_clipboard_listener=false
tray_notifications=true
start_with_windows=false
```

完整字段见 [config.example.ini](config.example.ini)。

### 目标配置

- `notion`：设置 `notion_token`，并优先填写 `data_source_id`。
- `obsidian`：设置 `obsidian_vault_dir` 和可选 `obsidian_folder`；配置页可选择已有子目录，也可输入新目录，写入时会自动创建。可选 `obsidian_tags` 会写入 YAML frontmatter 的 `tags` 字段，多个标签用逗号、分号或空格分隔。文件名直接使用剪贴板标题，同名时自动追加编号。
- Webhook、语雀和飞书文档：保留为未来/实验方向，暂不作为当前稳定配置路径。

## 托盘菜单

- 上传当前剪贴板。
- 查看、启用、暂停或录制热键。
- 启用或关闭托盘通知。
- 启用或关闭开机自动启动。
- 临时启用或暂停剪贴板自动监听。
- 验证当前配置，打开配置页面、配置文件、配置诊断、最近上传结果、最近 Obsidian 笔记、日志和状态目录。
- 退出程序。

## 数据与可靠性

默认状态目录：

```text
%LOCALAPPDATA%\NotionClipboardWin
```

目录内容：

- `queue/`：等待上传或待重试任务。
- `failed/`：超过重试次数或遇到永久错误的任务。
- `config-diagnostics.md`：最近一次配置诊断，包含 Notion/Obsidian 的可用性和错误信息。
- `recent-upload-results.md`：最近上传结果，包含 Notion URL、Notion page id、Obsidian 文件路径、本地文件 URI、Obsidian URI 或失败错误。
- `last-obsidian-upload.ini`：最近一次 Obsidian 成功写入的位置，用于托盘菜单“打开最近 Obsidian 笔记”。
- `notion-clipboard-win.log`：运行日志。

Notion API 没有通用写入幂等键。本程序会在远端资源创建成功后立即记录 `remote_id` / `remote_url`，并在每批追加成功后记录 `remote_progress`。如果服务端已写入但客户端没有收到响应，极端情况下仍可能出现重复追加；较小批次、较长读取超时和持久进度记录可以降低风险。

旧队列文件中的 `page_id`、`page_url` 和 `appended_block_count` 会自动兼容读取。

## 开发与验证

运行本地自测：

```powershell
.\build-console\Release\notion_clipboard_win.exe --self-test
```

对指定文件做转换统计，不读取剪贴板，也不会上传：

```powershell
.\build-console\Release\notion_clipboard_win.exe --dry-run-file .\test\bf.txt
```

自测覆盖算法讲解文本、HTML 清理、Markdown/LaTeX、表格、公式拆分、代码块拆分、字面量 `$`、inline code、URL/path、KaTeX/MathJax、请求切分、配置写回和各上传后端 payload。

贡献代码时请保持：

- C++17，MSVC `/W4 /permissive- /utf-8` 下无新增警告。
- 转换逻辑尽量保持平台无关，不在 `converter.cpp` 引入 Win32 依赖。
- 新增上传平台时实现 `UploadTarget`，不要把平台逻辑塞进剪贴板或队列模块。
- 修改转换规则时补充 `--self-test` 用例，必要时更新 `test/` 样例。

## 发布流程

发布 `v0.2.0` 时按以下顺序验证：

```powershell
.\build-console\Release\notion_clipboard_win.exe --self-test
.\build-console\Release\notion_clipboard_win.exe --dry-run-file .\test\bf.txt
.\build-console\Release\notion_clipboard_win.exe --dry-run-file .\test\after.txt
.\scripts\build-installer.ps1
```

在 GitHub Release 中上传安装包和 `.sha256` 文件。发布前确认没有提交真实 token、运行配置、日志、队列状态或本地状态目录。

## 许可证

本项目采用 MIT License。你可以自由使用、复制、修改、分发、再授权和销售本软件，前提是保留版权声明和许可声明。完整条款见 [LICENSE](LICENSE)。

## 安全

- 不要提交真实 `notion_token` 或本地 ini。
- `build/`、`build-console/`、运行日志、队列状态和本地状态目录不应进入仓库。
- 开源发布前请确认示例文档和测试文件只包含假 token 或脱敏数据。
