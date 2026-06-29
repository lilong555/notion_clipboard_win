# Notion Clipboard Win

Language: [中文](README.md) | English

A native Windows clipboard uploader. It runs as a background tray process, reads the current clipboard through a global hotkey, converts Markdown, code, LaTeX formulas, and common HTML clipboard fragments, then uploads them to the configured target: Notion, local Markdown files, Obsidian, local Git, webhook, GitHub Gist, a GitHub repository, Yuque, or Feishu Docs.

## Highlights

- Background tray process with `Ctrl+Shift+B` as the default upload hotkey.
- Custom tray/window icon, with `Notion Clipboard Win` shown when hovering over the tray icon.
- Tray menu hotkey recorder. The new hotkey is registered immediately and persisted to the config file.
- Tray notification toggle. The setting is persisted through `tray_notifications`.
- Optional start with Windows through the current user's Run registry key. No administrator permission is required.
- Optional clipboard event listener through `AddClipboardFormatListener`; no polling.
- Debounce and short duplicate suppression for clipboard event storms.
- Persistent queue first, sequential background upload second.
- Records `remote_id`, `remote_url`, and `remote_progress` so any upload target can resume work.
- Supports Notion `Retry-After`, short HTTP retries, and persistent queue exponential backoff.
- Conservative defaults for clipboard size, request interval, block batch size, and request body size.
- No real Notion or GitHub token is committed. Only an empty config template is provided.

## Supported Content

- Plain text, headings, paragraphs, and dividers.
- Markdown bullet lists, numbered lists, to-do lists, quote blocks, and fenced code blocks.
- Markdown tables and separator-less pipe tables are uploaded as native Notion tables when possible; unusually complex tables fall back to `plain text` code blocks.
- Tables wrapped in empty-language or `text` code fences are also recognized as tables, which helps with table snippets copied from chat tools.
- Inline formulas: `$...$`, `\(...\)`.
- Display formulas: `$$...$$`, `\[...\]`, `equation` / `align` / `gather` environments.
- Conservative LaTeX repair for common Unicode math symbols such as `≤`, `≥`, `∫`, and `α`.
- Protects inline code and URL/path dollar signs, so `$HOME$` and `$metadata/$value` are not misread as formulas.
- If Windows `HTML Format` exists in the clipboard, the app extracts the HTML fragment and converts it into Markdown-like text first.
- HTML conversion tries to detect KaTeX / MathJax TeX annotations and skips scripts, styles, visual helper markup, and non-content HTML.

## Project Layout

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

## Notion Setup

1. Create a Notion integration and get its token.
2. Share the target database with that integration.
3. Prefer configuring the target `data_source_id`. If only `database_id` is provided, the app resolves the first data source through the Notion API.
4. The target data source must contain at least one `title` property.

This project uses Notion API version `2026-03-11` and creates database pages with `parent.type = "data_source_id"`.

## Quick Start

Copy the example config:

```powershell
copy config.example.ini notion_clipboard_win.ini
```

Fill the required fields:

```ini
notion_token=secret_xxx
data_source_id=xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
hotkey=Ctrl+Shift+B
created_time_property_name=Created time
```

You can also keep the token out of the config file by using environment variables:

```powershell
setx NOTION_TOKEN "secret_xxx"
setx NOTION_DATA_SOURCE_ID "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
```

Build the Release binary:

```powershell
.\build-release.bat
```

Start the tray process:

```powershell
.\build\Release\notion_clipboard_win.exe --config .\notion_clipboard_win.ini
```

Copy text and press `Ctrl+Shift+B` to upload it. You can also right-click the tray icon and choose "Upload current clipboard".

## Build

This is a native Windows C++17 / Win32 program. Build it on Windows with MSVC or MinGW.

Using CMake:

```powershell
cmake -S . -B build
cmake --build build --config Release
```

Using the helper script:

```powershell
.\build-release.bat
```

`build-release.bat` builds the no-console tray version by default. To build a console version for debugging:

```powershell
cmake -S . -B build-console -DNOTION_CLIPBOARD_WIN_GUI=OFF
cmake --build build-console --config Release
```

## Usage

Run as a background tray process:

```powershell
.\build\Release\notion_clipboard_win.exe --config .\notion_clipboard_win.ini
```

Upload the current clipboard once:

```powershell
.\build\Release\notion_clipboard_win.exe --once --config .\notion_clipboard_win.ini
```

Validate the Notion configuration:

```powershell
.\build\Release\notion_clipboard_win.exe --validate-config --config .\notion_clipboard_win.ini
```

Show help:

```powershell
.\build\Release\notion_clipboard_win.exe --help
```

Run local conversion regression tests:

```powershell
.\build-console\Release\notion_clipboard_win.exe --self-test
```

Dry-run conversion for a file without using the clipboard or uploading:

```powershell
.\build-console\Release\notion_clipboard_win.exe --dry-run-file .\test\bf.txt
```

## Tray Menu

- Upload current clipboard.
- Show current hotkey.
- Temporarily enable or pause the hotkey.
- Record a new hotkey: confirm, press a new key combination, or press `Esc` to cancel. A successful recording is persisted and registered immediately.
- Enable or disable tray notifications. The setting is persisted to `tray_notifications`.
- Enable or disable start with Windows. The setting is persisted to `start_with_windows`.
- Temporarily enable or pause automatic clipboard listening.
- Open config, log, and state directory.
- Exit.

## Configuration

Common options:

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
created_time_property_name=Created time
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

Upload targets:

- `upload_target=notion`: default target; uploads clipboard content as a Notion page.
- `upload_target=markdown_file`: non-Notion target; writes clipboard content to a local Markdown file. If `markdown_output_dir` is empty, files go to `%LOCALAPPDATA%\NotionClipboardWin\markdown`.
- `upload_target=obsidian`: writes Markdown files into the vault at `obsidian_vault_dir`; `obsidian_folder` selects a subdirectory.
- `upload_target=local_git`: writes Markdown files into a local Git worktree. When `local_git_auto_commit=true`, the app runs `git add` and `git commit`; local Git must be available and configured with an author identity.
- `upload_target=webhook`: sends a generic JSON payload to `webhook_url`, suitable for n8n, Make, Zapier, Cloudflare Workers, or a self-hosted API. `webhook_bearer_token` is optional and is sent as an `Authorization: Bearer ...` request header.
- `upload_target=github_gist`: uses `github_token` to call the GitHub Gist API and creates one Markdown Gist per upload. A classic token needs the `gist` scope; a fine-grained token needs `Gists` user write permission. `github_gist_public=false` creates a secret gist; `github_gist_filename_prefix` controls the filename prefix.
- `upload_target=github_repo`: uses `github_token` to call the GitHub Contents API and commits each upload as a Markdown file in a repository. A classic token needs `repo` or public-repo access; a fine-grained token needs `Contents` write permission for the target repository. `github_repo_owner` / `github_repo_name` select the repository, `github_repo_branch` is optional, and `github_repo_directory` controls the directory.
- `upload_target=yuque`: uses `yuque_token` to call Yuque Open API v2 and creates Markdown documents in the book identified by `yuque_namespace`, usually `login/repo-slug`.
- `upload_target=feishu_doc`: uses `feishu_app_id` / `feishu_app_secret` to obtain a tenant token, creates a Feishu document, and writes the Markdown as text blocks. If `feishu_folder_token` is empty, Feishu uses the app's default location.
- Future targets such as Obsidian, local Git repositories, Yuque, or Feishu Docs should be added as upload target implementations instead of putting platform-specific logic in clipboard, queue, or conversion code.

Start with Windows:

- `start_with_windows=false`: disabled by default.
- `start_with_windows=true`: when the tray process starts, it writes `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\NotionClipboardWin`.
- The tray menu item updates both the registry entry and the config file.

Peak control:

- `debounce_ms`: merge multiple clipboard events from a single copy operation. Default: `750`.
- `duplicate_suppression_ms`: ignore identical content for a short time window. Default: `3000`.
- `max_clipboard_bytes`: skip unusually large clipboard text. Default: `262144`.
- `min_request_interval_ms`: rate-limit upload-target HTTP requests. Default: `400`.
- `append_batch_size`: blocks per append request. Default: `40`; the app also splits by an approximate 400 KB request body limit.
- `max_retry_attempts`: maximum persistent queue retry attempts.
- `http_retry_attempts`: short retries inside a single HTTP operation.

Created time:

- If `created_time_property_name` points to a Notion `date` property, the app writes the job creation time when creating the page.
- If it points to Notion's built-in `created_time` property, Notion fills it automatically and the app does not write it manually.

## Persistent Data

Default state directory:

```text
%LOCALAPPDATA%\NotionClipboardWin
```

Contents:

- `queue/`: pending or retrying jobs.
- `failed/`: jobs that exceeded retry limits or hit permanent errors.
- `notion-clipboard-win.log`: runtime log.

## Reliability Notes

The Notion API does not provide a general write idempotency key. This app records `remote_id` and `remote_url` immediately after creating a remote resource, and records `remote_progress` after each successful append batch. If the server writes a batch but the client loses the response, that batch may still be duplicated in an extreme case. Small batches, longer read timeouts, and persistent progress records reduce that risk. Older queue files with `page_id`, `page_url`, and `appended_block_count` are still read for compatibility.

## Security Notes

- Do not commit a real `notion_token`, `github_token`, `yuque_token`, `feishu_app_secret`, or webhook bearer token.
- Prefer environment variables for tokens.
- Do not commit `build/`, runtime config files, or local state directories.

## Regression Coverage

Local `--self-test` covers algorithm explanation text, HTML empty code artifacts, HTML list item paragraphs, code language fallback, Markdown/LaTeX, tasks, quotes, standard tables and separator-less pipe tables, many inline equations, long code splitting, long formula fallback, literal dollar signs, inline code dollar signs, URL/path dollar signs, multi-backtick code spans, Windows paths, HTML numeric entities, KaTeX/MathJax, script/style pollution, append request body splitting, and single-value config persistence.
