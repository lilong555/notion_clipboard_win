# Notion Clipboard Win

Windows 原生剪贴板上传工具：后台托盘常驻，按全局热键读取当前剪贴板文本，并上传到 Notion 数据源里的新页面。

设计目标是高性能和高可靠：

- 后台托盘常驻，默认全局热键 `Ctrl+Shift+B` 触发上传。
- 可选使用 `AddClipboardFormatListener` 事件监听，不轮询剪贴板。
- 自动监听模式下使用 debounce 合并剪贴板事件风暴，默认 750ms 后只读取一次。
- 上传正文时会把 Markdown/LaTeX 转成 Notion blocks，行内公式使用 equation rich text，独立公式使用 equation block。
- 支持标题、列表、任务列表、引用块、代码块、分隔线和 Markdown 表格的保守保留。
- 如果剪贴板包含 Windows `HTML Format`，会优先抽取 HTML fragment 并轻量转换为 Markdown-like 文本。
- 上传工作由单独线程顺序执行，并限制 Notion 请求间隔，减少网络和 API 峰值。
- 每条剪贴板内容先落盘到持久队列，程序崩溃或断网后可继续重试。
- 页面创建成功后会把 `page_id` 写回队列，正文按批追加并记录进度，降低重复创建页面的风险。
- 支持指数退避和 Notion `Retry-After`，失败多次后移动到 `failed` 目录。

## 目录

```text
notion_clipboard_win/
  CMakeLists.txt
  config.example.ini
  src/main.cpp
```

## Notion 准备

1. 在 Notion 创建 integration，拿到 token。
2. 把目标数据库共享给这个 integration。
3. 推荐复制目标 `data_source_id`。如果只有 `database_id`，程序会通过 Notion API 解析第一个 data source。
4. 目标数据源至少需要一个 `title` 类型属性。

本项目按 Notion API `2026-03-11` 版本实现，创建数据库页面时使用 `parent.type = "data_source_id"`。

## 配置

复制示例配置：

```powershell
copy config.example.ini notion_clipboard_win.ini
```

填写：

```ini
notion_token=secret_xxx
data_source_id=xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
hotkey=Ctrl+Shift+B
created_time_property_name=创建时间
```

也可以不写 token 到文件，改用环境变量：

```powershell
setx NOTION_TOKEN "secret_xxx"
setx NOTION_DATA_SOURCE_ID "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
```

## 构建

在 Windows 的 Developer PowerShell 中进入本目录：

```powershell
cmake -S . -B build
cmake --build build --config Release
```

也可以在 Windows 文件资源管理器或 PowerShell 中运行：

```powershell
.\build-release.bat
```

`build-release.bat` 默认构建无控制台窗口的托盘后台版。如果要构建带控制台窗口的调试版：

```powershell
cmake -S . -B build -DNOTION_CLIPBOARD_WIN_GUI=OFF
cmake --build build --config Release
```

本项目是 Windows 原生程序，需要在 Windows/MSVC 或 MinGW 环境中构建。

## 运行

启动后台托盘进程：

```powershell
.\build\Release\notion_clipboard_win.exe --config .\notion_clipboard_win.ini
```

复制文本后按默认热键 `Ctrl+Shift+B`，程序会读取当前剪贴板、写入持久队列，并由后台线程上传。也可以右键托盘图标选择“上传当前剪贴板”。如果希望复制后自动上传，可在配置中设置：

```ini
enable_clipboard_listener=true
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

## 数据落盘

默认状态目录：

```text
%LOCALAPPDATA%\NotionClipboardWin
```

其中：

- `queue/`：等待上传或待重试任务。
- `failed/`：超过重试次数或遇到永久错误的任务。
- `notion-clipboard-win.log`：运行日志。

## 托盘菜单

- 上传当前剪贴板。
- 查看当前热键。
- 临时启用/暂停热键。
- 临时启用/暂停自动监听剪贴板。
- 打开配置、日志和状态目录。
- 退出程序。

## 峰值控制

可在 `notion_clipboard_win.ini` 中调节：

- `hotkey`：全局热键，默认 `Ctrl+Shift+B`。
- `enable_clipboard_listener`：是否复制后自动入队上传，默认关闭。
- `created_time_property_name`：如果存在同名 `date` 属性，创建页面时自动写入任务创建时间；如果是 Notion 内置 `created_time` 类型，则由 Notion 自动填写。
- `debounce_ms`：合并一次复制产生的多次剪贴板事件。
- `duplicate_suppression_ms`：短时间忽略相同内容，避免重复通知造成重复上传。
- `max_clipboard_bytes`：跳过异常大的剪贴板文本，减少内存峰值。
- `min_request_interval_ms`：限制 Notion 请求频率，避免突发写入。
- `append_batch_size`：控制每次追加的 block 数，默认 40，低于 Notion 单请求 100 个 children 的上限；程序还会按约 400KB 请求体安全上限自动再切分。

## Markdown / LaTeX

支持常见输入：

- 行内公式：`$...$`、`\(...\)`。
- 独立公式：`$$...$$`、`\[...\]`、`equation` / `align` / `gather` 环境。
- 常见 Unicode 数学符号会做保守 LaTeX 修复，例如 `≤`、`≥`、`∫`、`α`。
- fenced code block、标题、列表、任务列表、引用块会映射到对应 Notion block。
- Markdown 表格暂时保守上传为 `plain text` 代码块，避免复杂表格在 Notion API 中被错误拆分。

本地 `--self-test` 覆盖普通算法讲解、HTML 空代码块污染、`text`/未知代码语言降级、Markdown/LaTeX、任务列表、引用块、表格保守保留、多行内公式拆分、超长代码块拆分、超长公式降级、货币 `$` 误判、HTML 数字实体和 append 请求体切分。

## 可靠性说明

Notion API 没有通用的写入幂等键。本程序会在页面创建成功后立即记录 `page_id`，正文每批追加成功后记录 `appended_block_count`。如果网络在服务端已经写入但客户端还没收到响应时中断，极端情况下某个追加批次仍可能重复；程序通过小批量追加、较长读取超时和持久进度记录降低这个风险。
