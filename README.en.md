# Notion Clipboard Win

Language: [中文](README.md) | English

Notion Clipboard Win is a native Windows clipboard uploader. It runs in the system tray, reads clipboard content through a hotkey or optional clipboard listener, converts Markdown, code blocks, LaTeX formulas, tables, and common HTML clipboard fragments, then uploads the result to the configured target.

The default workflow targets Notion, but the same queue and conversion pipeline can also write local Markdown files, Obsidian vaults, local Git repositories, webhooks, GitHub Gists, GitHub repositories, Yuque, and Feishu Docs. The app is implemented with C++17, Win32, and WinHTTP, with no third-party runtime dependency.

## Features

- Background tray process with `Ctrl+Shift+B` as the default upload hotkey.
- Optional clipboard listener with debounce and short duplicate suppression.
- Local configuration page from the tray menu, prefilled from the active config and able to emit a complete ini file.
- Persistent queue before upload, so network failures, process exits, and rate limits can be retried.
- Stores `remote_id`, `remote_url`, and `remote_progress` for resumable uploads across targets.
- Supports Notion `Retry-After`, short HTTP retries, and persistent queue exponential backoff.
- Configurable hotkey, tray notifications, Windows startup, and state directory.
- Conversion coverage for Markdown, HTML, formulas, tables, code language normalization, and common false-positive protections.

## Upload Targets

| `upload_target` | Description | Required configuration |
| --- | --- | --- |
| `notion` | Create a Notion data source page and append body blocks | `notion_token`, `data_source_id` or `database_id` |
| `markdown_file` | Write a local Markdown file | Optional `markdown_output_dir` |
| `obsidian` | Write into an Obsidian vault | `obsidian_vault_dir` |
| `local_git` | Write into a local Git worktree, optionally committing | `local_git_repo_dir` |
| `webhook` | Send a JSON payload to a custom HTTP endpoint | `webhook_url` |
| `github_gist` | Create a Markdown Gist | `github_token` |
| `github_repo` | Commit a Markdown file through the GitHub Contents API | `github_token`, repository owner/name |
| `yuque` | Create a Markdown document in a Yuque book | `yuque_token`, `yuque_namespace` |
| `feishu_doc` | Create a Feishu document and append text blocks | `feishu_app_id`, `feishu_app_secret` |

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
  LICENSE                 Personal-free, commercial-license terms
  installer/              Inno Setup installer script
  scripts/                Release build scripts
  src/                    C++17 Win32 source
    main.cpp              Tray, CLI, clipboard, upload worker
    converter.cpp         Markdown/HTML/LaTeX conversion and self-tests
    upload_target.cpp     Notion, local files, GitHub, Yuque, Feishu, etc.
    config_page.cpp       Local HTML configuration page
  test/                   Conversion regression samples
```

## Quick Start

### Use The Installer

Download `NotionClipboardWin-0.1.0-Setup.exe` from GitHub Releases, install it, and start `Notion Clipboard Win` from the Start menu. The installer does not create or overwrite your real `notion_clipboard_win.ini`; you still need to prepare a config file before first use.

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

Copy text and press `Ctrl+Shift+B`, or right-click the tray icon and choose "Upload current clipboard".

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
dist\NotionClipboardWin-0.1.0-Setup.exe
dist\NotionClipboardWin-0.1.0-Setup.exe.sha256
```

## Configuration

The recommended path is to open "Configuration page" from the tray menu. The page is prefilled from the active config and can reveal/hide tokens, copy the full ini, or download an ini file.

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

- `markdown_file`: when `markdown_output_dir` is empty, files are written to `%LOCALAPPDATA%\NotionClipboardWin\markdown`.
- `obsidian`: set `obsidian_vault_dir` and optional `obsidian_folder`.
- `local_git`: set `local_git_repo_dir`; with `local_git_auto_commit=true`, the app runs `git add` and `git commit`.
- `webhook`: set `webhook_url`, with optional `webhook_bearer_token`.
- `github_gist`: `github_token` needs Gist write permission.
- `github_repo`: `github_token` needs Contents write permission for the target repository.
- `yuque`: `yuque_namespace` usually looks like `login/repo-slug`.
- `feishu_doc`: uses `feishu_app_id` / `feishu_app_secret` to obtain a tenant token; `feishu_folder_token` is optional.

## Tray Menu

- Upload current clipboard.
- View, enable, pause, or record the hotkey.
- Enable or disable tray notifications.
- Enable or disable start with Windows.
- Temporarily enable or pause automatic clipboard listening.
- Open the configuration page, config file, log file, and state directory.
- Exit.

## Data And Reliability

Default state directory:

```text
%LOCALAPPDATA%\NotionClipboardWin
```

Contents:

- `queue/`: pending or retrying jobs.
- `failed/`: jobs that exceeded retry limits or hit permanent errors.
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

For `v0.1.0`, verify in this order:

```powershell
.\build-console\Release\notion_clipboard_win.exe --self-test
.\build-console\Release\notion_clipboard_win.exe --dry-run-file .\test\bf.txt
.\build-console\Release\notion_clipboard_win.exe --dry-run-file .\test\after.txt
.\scripts\build-installer.ps1
```

Upload the installer and `.sha256` file to the GitHub Release. Before publishing, confirm that no real tokens, runtime configs, logs, queue state, or local state directories are committed.

## License

This project uses a source-available license. It is not licensed under MIT, Apache-2.0, or another OSI-approved open-source license. Personal, educational, research, evaluation, and other non-commercial use is free; commercial use requires written authorization from the copyright holder.

For commercial authorization, contact the copyright holder through the GitHub repository. See [LICENSE](LICENSE) for the full terms.

## Security

- Do not commit real `notion_token`, `github_token`, `yuque_token`, `feishu_app_secret`, webhook tokens, or local ini files.
- Keep `build/`, `build-console/`, logs, queue state, and local state directories out of the repository.
- Before publishing public examples, verify that documentation and test files contain only fake or redacted secrets.
