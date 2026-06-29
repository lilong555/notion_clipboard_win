# Notion Clipboard Win

语言：中文 | [English](README.en.md)

Windows 原生剪贴板上传工具。程序常驻后台托盘，按全局热键读取当前剪贴板文本，将 Markdown、代码、LaTeX 公式和常见 HTML 剪贴板内容转换后上传到配置的后端：Notion、本地 Markdown 文件、Obsidian、本地 Git、Webhook、GitHub Gist、GitHub 仓库、语雀或飞书文档。

## 功能亮点

- 后台托盘常驻，默认热键 `Ctrl+Shift+B` 上传当前剪贴板。
- 自定义托盘/窗口图标，鼠标悬停托盘图标时显示 `Notion Clipboard Win`。
- 托盘菜单支持录制新热键，成功后立即重新注册并写回配置。
- 可关闭托盘通知，设置会持久写回 `tray_notifications`。
- 可选开机自动启动，使用当前用户的 Windows Run 注册表项，无需管理员权限。
- 可选使用 `AddClipboardFormatListener` 监听剪贴板事件，不做轮询。
- 自动监听模式支持 debounce 和短时间重复抑制，减少一次复制触发多次上传。
- 先写入持久队列，再由后台线程顺序上传，断网或程序退出后可继续重试。
- 上传后记录 `remote_id` / `remote_url` / `remote_progress`，用于跨后端恢复进度。
- 支持 Notion `Retry-After`、HTTP 短重试和持久队列指数退避。
- 默认限制剪贴板大小、请求间隔、每批 block 数量和请求体大小，降低性能峰值。
- 不提交真实 Notion 或 GitHub token，只提供空配置模板。

## 支持的内容

- 普通文本、标题、段落、分隔线。
- Markdown 列表、任务列表、引用块、代码块。
- Markdown 表格和无分隔行的管道表格会优先上传为 Notion 原生表格；异常复杂时降级为 `plain text` 代码块。
- 被空语言或 `text` 代码围栏包住的表格也会识别为表格，便于处理聊天工具复制出的表格片段。
- 行内公式：`$...$`、`\(...\)`。
- 独立公式：`$$...$$`、`\[...\]`、`equation` / `align` / `gather` 环境。
- 常见 Unicode 数学符号会做保守 LaTeX 修复，例如 `≤`、`≥`、`∫`、`α`。
- 会保护 inline code 和 URL/path 中的 `$`，避免把 `$HOME$`、`$metadata/$value` 误识别成公式。
- 如果剪贴板有 Windows `HTML Format`，会优先抽取 HTML fragment 并轻量转换为 Markdown-like 文本。
- HTML 剪贴板中会尽量识别 KaTeX / MathJax 的 TeX annotation，跳过脚本、样式、视觉辅助和非正文 HTML。

## 项目结构

```text
notion_clipboard_win/
  CMakeLists.txt
  build-release.bat
  config.example.ini
  README.md
  README.en.md
  src/main.cpp
  test/
```

## Notion 准备

1. 在 Notion 创建 integration，拿到 token。
2. 把目标数据库共享给这个 integration。
3. 推荐填写目标 `data_source_id`。如果只有 `database_id`，程序会通过 Notion API 解析第一个 data source。
4. 目标数据源至少需要一个 `title` 类型属性。

本项目按 Notion API `2026-03-11` 版本实现，创建数据库页面时使用 `parent.type = "data_source_id"`。

## 快速开始

复制示例配置：

```powershell
copy config.example.ini notion_clipboard_win.ini
```

填写必要项：

```ini
notion_token=secret_xxx
data_source_id=xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
hotkey=Ctrl+Shift+B
created_time_property_name=创建时间
```

也可以不把 token 写入文件，改用环境变量：

```powershell
setx NOTION_TOKEN "secret_xxx"
setx NOTION_DATA_SOURCE_ID "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
```

构建 Release 版：

```powershell
.\build-release.bat
```

启动后台托盘进程：

```powershell
.\build\Release\notion_clipboard_win.exe --config .\notion_clipboard_win.ini
```

复制文本后按默认热键 `Ctrl+Shift+B` 上传。也可以右键托盘图标选择“上传当前剪贴板”。

## 构建

本项目是 Windows 原生 C++17 / Win32 程序，需要在 Windows 的 MSVC 或 MinGW 环境中构建。

使用 CMake：

```powershell
cmake -S . -B build
cmake --build build --config Release
```

使用构建脚本：

```powershell
.\build-release.bat
```

`build-release.bat` 默认构建无控制台窗口的托盘后台版。如果要构建带控制台窗口的调试版：

```powershell
cmake -S . -B build-console -DNOTION_CLIPBOARD_WIN_GUI=OFF
cmake --build build-console --config Release
```

## 使用方式

后台托盘：

```powershell
.\build\Release\notion_clipboard_win.exe --config .\notion_clipboard_win.ini
```

只上传当前剪贴板一次：

```powershell
.\build\Release\notion_clipboard_win.exe --once --config .\notion_clipboard_win.ini
```

验证 Notion 配置：

```powershell
.\build\Release\notion_clipboard_win.exe --validate-config --config .\notion_clipboard_win.ini
```

查看参数：

```powershell
.\build\Release\notion_clipboard_win.exe --help
```

运行本地转换回归测试：

```powershell
.\build-console\Release\notion_clipboard_win.exe --self-test
```

对指定文件做转换统计，不依赖剪贴板，也不会上传：

```powershell
.\build-console\Release\notion_clipboard_win.exe --dry-run-file .\test\bf.txt
```

## 托盘菜单

- 上传当前剪贴板。
- 查看当前热键。
- 临时启用或暂停热键。
- 录制新热键：确认后按新的组合键，`Esc` 取消；成功后会写回配置并立即重新注册。
- 启用或关闭托盘通知，设置会写回 `tray_notifications`。
- 启用或关闭开机自动启动，设置会写回 `start_with_windows`。
- 临时启用或暂停自动监听剪贴板。
- 打开配置、日志和状态目录。
- 退出程序。

## 配置项

常用配置：

```ini
upload_target=notion
notion_token=
webhook_url=
webhook_bearer_token=
github_token=
github_gist_public=false
github_gist_filename_prefix=clipboard
github_repo_owner=
github_repo_name=
github_repo_branch=
github_repo_directory=clipboard
github_repo_filename_prefix=clipboard
yuque_token=
yuque_namespace=
yuque_slug_prefix=clipboard
feishu_app_id=
feishu_app_secret=
feishu_folder_token=
data_source_id=
database_id=
title_property_name=
content_property_name=
created_time_property_name=创建时间
markdown_output_dir=
obsidian_vault_dir=
obsidian_folder=Clipboard
obsidian_filename_prefix=clipboard
local_git_repo_dir=
local_git_directory=clipboard
local_git_filename_prefix=clipboard
local_git_auto_commit=false
hotkey=Ctrl+Shift+B
enable_hotkey=true
enable_clipboard_listener=false
tray_notifications=true
start_with_windows=false
```

上传后端：

- `upload_target=notion`：默认后端，按当前配置上传为 Notion 页面。
- `upload_target=markdown_file`：非 Notion 后端，直接把剪贴板内容写入本地 Markdown 文件。`markdown_output_dir` 留空时使用 `%LOCALAPPDATA%\NotionClipboardWin\markdown`。
- `upload_target=obsidian`：写入 `obsidian_vault_dir` 指向的 Obsidian vault，可用 `obsidian_folder` 指定子目录。
- `upload_target=local_git`：写入本地 Git 工作区。`local_git_auto_commit=true` 时会执行 `git add` 和 `git commit`，需要本机 `git` 可用且仓库已配置提交身份。
- `upload_target=webhook`：向 `webhook_url` 发送通用 JSON payload，适合 n8n、Make、Zapier、Cloudflare Worker、自建 API 等中转。`webhook_bearer_token` 可选，设置后会作为 `Authorization: Bearer ...` 请求头发送。
- `upload_target=github_gist`：用 `github_token` 调用 GitHub Gist API，每次上传创建一个 Markdown Gist。classic token 需要 `gist` scope；fine-grained token 需要 `Gists` user write 权限。`github_gist_public=false` 时创建 secret gist；`github_gist_filename_prefix` 控制文件名前缀。
- `upload_target=github_repo`：用 `github_token` 调用 GitHub Contents API，把每次上传提交为仓库中的 Markdown 文件。classic token 需要 `repo` 或 public repo 对应权限；fine-grained token 需要目标仓库 `Contents` write 权限。`github_repo_owner` / `github_repo_name` 指定仓库，`github_repo_branch` 留空时使用默认分支，`github_repo_directory` 控制目录。
- `upload_target=yuque`：用 `yuque_token` 调用语雀 Open API v2，在 `yuque_namespace` 指向的知识库中创建 Markdown 文档。`yuque_namespace` 通常形如 `login/repo-slug`。
- `upload_target=feishu_doc`：用 `feishu_app_id` / `feishu_app_secret` 获取 tenant token，创建飞书文档并写入 Markdown 文本块。`feishu_folder_token` 留空时使用应用默认位置。
- 后续接入 Obsidian、本地 Git 仓库、语雀、飞书文档等平台时，应新增上传后端实现，而不是把平台逻辑写进剪贴板、队列或转换器代码。

开机自动启动：

- `start_with_windows=false`：默认关闭。
- `start_with_windows=true`：启动托盘进程时写入 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\NotionClipboardWin`。
- 托盘菜单中的“开机自动启动”会同时更新注册表和配置文件。

峰值控制：

- `debounce_ms`：合并一次复制产生的多次剪贴板事件，默认 `750`。
- `duplicate_suppression_ms`：短时间忽略相同内容，默认 `3000`。
- `max_clipboard_bytes`：跳过异常大的剪贴板文本，默认 `262144`。
- `min_request_interval_ms`：限制上传后端 HTTP 请求频率，默认 `400`。
- `append_batch_size`：每次追加的 block 数，默认 `40`，程序还会按约 400KB 请求体安全上限自动切分。
- `max_retry_attempts`：持久队列最大重试次数。
- `http_retry_attempts`：单个 HTTP 操作内部的短重试次数。

创建时间：

- 如果 `created_time_property_name` 对应 Notion `date` 属性，程序会在创建页面时自动写入任务创建时间。
- 如果它是 Notion 内置 `created_time` 类型，则由 Notion 自动填写，程序不会手动写。

## 数据落盘

默认状态目录：

```text
%LOCALAPPDATA%\NotionClipboardWin
```

其中：

- `queue/`：等待上传或待重试任务。
- `failed/`：超过重试次数或遇到永久错误的任务。
- `notion-clipboard-win.log`：运行日志。

## 可靠性说明

Notion API 没有通用写入幂等键。本程序会在远端资源创建成功后立即记录 `remote_id` / `remote_url`，并在每批追加成功后记录 `remote_progress`。如果网络在服务端已经写入但客户端还没收到响应时中断，极端情况下某个追加批次仍可能重复；程序通过小批量追加、较长读取超时和持久进度记录降低这个风险。旧队列文件中的 `page_id`、`page_url` 和 `appended_block_count` 会自动兼容读取。

## 安全说明

- 不要把真实 `notion_token`、`github_token`、`yuque_token`、`feishu_app_secret` 或 webhook bearer token 提交到仓库。
- 推荐使用环境变量保存 token。
- `build/`、运行配置和本地状态目录不应提交。

## 回归测试覆盖

本地 `--self-test` 覆盖普通算法讲解、HTML 空代码块污染、HTML 列表项段落、代码语言降级、Markdown/LaTeX、任务列表、引用块、标准表格和无分隔行管道表格、多行内公式拆分、超长代码块拆分、超长公式降级、货币 `$` 误判、inline code 中的 `$`、URL/path 中的 `$`、多反引号 code span、Windows 路径、HTML 数字实体、KaTeX/MathJax、script/style 污染、append 请求体切分和配置单项写回。
