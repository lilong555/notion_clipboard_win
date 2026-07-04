# Notion Clipboard Win

Language: [中文](README.md) | English

Notion Clipboard Win is a native Windows clipboard uploader. It runs in the system tray, reads clipboard content through a hotkey or optional clipboard listener, converts Markdown, code blocks, LaTeX formulas, tables, and common HTML clipboard fragments, then uploads the result to the configured target.

The project currently focuses on two stable write paths: Notion and Obsidian. The app is implemented with C++17, Win32, and WinHTTP, with no third-party runtime dependency.

## Features

- Background tray process with `Ctrl+Shift+B` as the default upload hotkey.
- Optional clipboard listener with debounce and short duplicate suppression.
- Local configuration page from the tray menu, prefilled from the active config, able to validate settings and emit a complete ini file.
- Persistent queue before upload, so network failures, process exits, and rate limits can be retried.
- Stores `remote_id`, `remote_url`, and `remote_progress` for resumable uploads across targets.
- Supports Notion `Retry-After`, short HTTP retries, and persistent queue exponential backoff.
- Configurable hotkey, tray notifications, Windows startup, and state directory.
- Records recent upload results with Notion URLs, Obsidian file paths, local file links, and Obsidian URIs.
- Can open the latest successful Obsidian note from the tray menu.
- Can validate the active configuration from the tray menu or configuration page and write a Notion/Obsidian diagnostics report.
- Conversion coverage for Markdown, HTML, formulas, tables, code language normalization, and common false-positive protections.

## Upload Targets

`upload_target` accepts a single target or a comma-separated list such as `upload_target=notion,obsidian`. In multi-target mode, each target is queued as an independent job so retries and remote progress stay isolated.

| `upload_target` | Description | Required configuration |
| --- | --- | --- |
| `notion` | Create a Notion data source page and append body blocks | `notion_token`, `data_source_id` or `database_id` |
| `obsidian` | Write Markdown notes into an Obsidian vault | `obsidian_vault_dir` |

Webhook, Yuque, and Feishu Docs remain future/experimental directions. They are not exposed as stable targets in the configuration page.

## Supported Content

- Plain text, headings, paragraphs, and dividers.
- Markdown lists, task lists, quotes, fenced code blocks, and links.
- Markdown tables, separator-less pipe tables, and tables wrapped in empty-language or `text` fences.
- Inline formulas: `$...$`, `\(...\)`.
- Display formulas: `$$...$$`, `\[...\]`, `equation` / `align` / `gather` environments.
- TeX annotations from KaTeX / MathJax HTML.
- Conservative LaTeX repair for common Unicode math symbols.
- Protection for inline code, URLs, Windows paths, and literal dollar signs to reduce formula false positives.

## Project Layout

```text
notion_clipboard_win/
  CMakeLists.txt          CMake entry point
  build-release.bat       Release tray build helper
  VERSION                 Single source of truth for release version
  config.example.ini      Safe config template
  LICENSE                 MIT open-source license
  installer/              Inno Setup installer script
  scripts/                Release build scripts
  src/                    C++17 Win32 source
    main.cpp              Tray, CLI, clipboard, upload worker
    converter.cpp         Markdown/HTML/LaTeX conversion and self-tests
    upload_target.cpp     Notion, Obsidian, and experimental upload targets
    config_page.cpp       Local HTML configuration page
  test/                   Conversion regression samples
```

## Quick Start

### Use The Installer

Download `NotionClipboardWin-0.2.0-Setup.exe` from GitHub Releases, install it, and start `Notion Clipboard Win` from the Start menu. The installer does not create or overwrite your real `notion_clipboard_win.ini`; you still need to prepare a config file before first use.

### Run From Source

Copy the config template:

```powershell
copy config.example.ini notion_clipboard_win.ini
```

Fill the minimum Notion configuration:

```ini
upload_target=notion
notion_token=secret_xxx
data_source_id=xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
hotkey=Ctrl+Shift+B
```

To write to Notion and Obsidian at the same time:

```ini
upload_target=notion,obsidian
notion_token=secret_xxx
data_source_id=xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
obsidian_vault_dir=E:\obsidian\MyVault
obsidian_folder=Inbox/Clipboard
obsidian_tags=algorithm cpp
```

You can keep secrets out of the config file by using environment variables:

```powershell
setx NOTION_TOKEN "secret_xxx"
setx NOTION_DATA_SOURCE_ID "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
```

Build and start:

```powershell
.\build-release.bat
.\build\Release\notion_clipboard_win.exe --config .\notion_clipboard_win.ini
```

Copy text and press `Ctrl+Shift+B`, or right-click the tray icon and choose "Upload current clipboard". After a successful Obsidian write, use "Open latest Obsidian note" from the tray menu to jump to the new file.

## Notion Setup

1. Create a Notion integration and get its token.
2. Share the target database with that integration.
3. Prefer `data_source_id`. If only legacy `database_id` is configured, the app resolves the first data source through the Notion API.
4. The target data source must contain at least one `title` property.

This project uses Notion API version `2026-03-11` and creates database pages with `parent.type = "data_source_id"`.

## Build And Run

Requirements: Windows, CMake, and MSVC or MinGW.

Build the GUI tray binary:

```powershell
cmake -S . -B build
cmake --build build --config Release
```

Build the console binary for debugging:

```powershell
cmake -S . -B build-console -DNOTION_CLIPBOARD_WIN_GUI=OFF
cmake --build build-console --config Release
```

Common commands:

```powershell
.\build\Release\notion_clipboard_win.exe --config .\notion_clipboard_win.ini
.\build\Release\notion_clipboard_win.exe --once --config .\notion_clipboard_win.ini
.\build\Release\notion_clipboard_win.exe --validate-config --config .\notion_clipboard_win.ini
.\build\Release\notion_clipboard_win.exe --help
```

Building the installer requires Inno Setup 6:

```powershell
winget install JRSoftware.InnoSetup
.\scripts\build-installer.ps1
```

The script builds the console binary, runs `--self-test`, builds the GUI Release binary, then produces:

```text
dist\NotionClipboardWin-0.2.0-Setup.exe
dist\NotionClipboardWin-0.2.0-Setup.exe.sha256
```

## Configuration

The recommended path is to open "Configuration page" from the tray menu. The page is prefilled from the active config, can select Notion and Obsidian at the same time, list registered Obsidian vaults and folders, preview the final Obsidian write location, validate the current output config, open diagnostics and recent upload results, reveal/hide tokens, copy or download the full ini, and apply changes by writing the active config file and restarting the tray app.

Core options:

```ini
upload_target=notion
notion_token=
data_source_id=
database_id=
title_property_name=
content_property_name=
created_time_property_name=Created time
hotkey=Ctrl+Shift+B
enable_hotkey=true
enable_clipboard_listener=false
tray_notifications=true
start_with_windows=false
```

See [config.example.ini](config.example.ini) for the full template.

### Target Configuration

- `notion`: set `notion_token` and prefer `data_source_id`.
- `obsidian`: set `obsidian_vault_dir` and optional `obsidian_folder`; the configuration page can select an existing folder or accept a new folder, which is created when writing. Optional `obsidian_tags` are written to the YAML frontmatter `tags` field, separated by commas, semicolons, or whitespace. Filenames use the clipboard title directly, with a numeric suffix on conflicts.
- Webhook, Yuque, and Feishu Docs: retained as future/experimental directions, not as the current stable configuration path.

## Tray Menu

- Upload current clipboard.
- View, enable, pause, or record the hotkey.
- Enable or disable tray notifications.
- Enable or disable start with Windows.
- Temporarily enable or pause automatic clipboard listening.
- Validate the active configuration, and open the configuration page, config file, diagnostics report, recent upload results, latest Obsidian note, log file, and state directory.
- Exit.

## Data And Reliability

Default state directory:

```text
%LOCALAPPDATA%\NotionClipboardWin
```

Contents:

- `queue/`: pending or retrying jobs.
- `failed/`: jobs that exceeded retry limits or hit permanent errors.
- `config-diagnostics.md`: the latest configuration diagnostics with Notion/Obsidian availability and errors.
- `recent-upload-results.md`: recent upload results with Notion URLs, Notion page ids, Obsidian file paths, local file URIs, Obsidian URIs, or failure errors.
- `last-obsidian-upload.ini`: the latest successful Obsidian write location, used by "Open latest Obsidian note" in the tray menu.
- `notion-clipboard-win.log`: runtime log.

The Notion API does not provide a general write idempotency key. The app records `remote_id` / `remote_url` immediately after creating a remote resource, and records `remote_progress` after each successful append batch. If the server writes a batch but the client loses the response, a duplicate append is still possible in an extreme case; smaller batches, longer read timeouts, and persisted progress reduce the risk.

Older queue files with `page_id`, `page_url`, and `appended_block_count` are still read for compatibility.

## Development

Run local self-tests:

```powershell
.\build-console\Release\notion_clipboard_win.exe --self-test
```

Dry-run conversion for a file without reading the clipboard or uploading:

```powershell
.\build-console\Release\notion_clipboard_win.exe --dry-run-file .\test\bf.txt
```

Self-tests cover algorithm explanation text, HTML cleanup, Markdown/LaTeX, tables, formula splitting, code splitting, literal dollar signs, inline code, URL/path protection, KaTeX/MathJax, request splitting, config persistence, and upload target payloads.

Contribution expectations:

- Use C++17 and keep MSVC `/W4 /permissive- /utf-8` builds warning-free.
- Keep conversion logic platform-independent; do not add Win32 dependencies to `converter.cpp`.
- Add new platforms through `UploadTarget` implementations instead of clipboard or queue code.
- Add `--self-test` coverage when changing conversion rules, and update `test/` samples when useful.

## Release Process

For `v0.2.0`, verify in this order:

```powershell
.\build-console\Release\notion_clipboard_win.exe --self-test
.\build-console\Release\notion_clipboard_win.exe --dry-run-file .\test\bf.txt
.\build-console\Release\notion_clipboard_win.exe --dry-run-file .\test\after.txt
.\scripts\build-installer.ps1
```

Upload the installer and `.sha256` file to the GitHub Release. Before publishing, confirm that no real tokens, runtime configs, logs, queue state, or local state directories are committed.

## License

This project is licensed under the MIT License. You may use, copy, modify, distribute, sublicense, and sell copies of the software as long as the copyright and license notices are preserved. See [LICENSE](LICENSE) for the full terms.

## Security

- Do not commit real `notion_token` values or local ini files.
- Keep `build/`, `build-console/`, logs, queue state, and local state directories out of the repository.
- Before publishing public examples, verify that documentation and test files contain only fake or redacted secrets.
