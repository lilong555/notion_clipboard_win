# Notion Clipboard Win

Language: [中文](README.md) | English

Notion Clipboard Win is a Windows tray app that saves copied content to Notion or Obsidian with one hotkey. It is useful for web clips, solution notes, code snippets, study notes, Markdown text, and formula-heavy content.

The project currently focuses on two stable targets: **Notion** and **Obsidian**. It is implemented with C++17, Win32, and WinHTTP, with no third-party runtime dependency.

## Features

- Upload the current clipboard with the default `Ctrl+Shift+B` hotkey.
- Write to Notion, Obsidian, or both at the same time.
- Convert Markdown, HTML, code blocks, tables, inline formulas, and display formulas.
- Built-in local configuration page, so most users do not need to edit ini files by hand, including hotkey recording, validation, and test upload.
- Failed uploads are queued locally and can be retried later.
- Tray menu for uploading the current clipboard, opening the upload center, viewing logs, and opening the latest Obsidian note.

## User Guide

### 1. Download And Install

1. Open [GitHub Releases](https://github.com/lilong555/notion_clipboard_win/releases).
2. Download the latest `NotionClipboardWin-0.2.2-Setup.exe`.
3. Run the installer.
4. Start `Notion Clipboard Win` from the Start menu.
5. The app appears in the Windows system tray.

If you leave the post-install launch option enabled, the configuration page opens automatically once. After that, the app does not open the configuration page automatically; open it manually from the tray menu.

If you do not see the tray icon, click the small up arrow near the Windows clock. Windows may have hidden it there.

### 2. Open The Configuration Page

1. Right-click the tray icon.
2. Choose "打开配置页面" (open configuration page).
3. A local page opens in your browser.
4. After editing, click "Apply and restart".

The configuration page is local to your computer. It is used to generate and save the local ini configuration. Except for the one post-install launch, the app does not open this page automatically.

### 3. Choose Upload Targets

You can choose one target or both:

- Notion only: check `Notion`.
- Obsidian only: check `Obsidian`.
- Both: check `Notion` and `Obsidian`.

The saved config uses:

```ini
upload_target=notion,obsidian
```

### 4. Configure Notion

To upload to Notion, you need:

- `Notion Token`
- `Data Source ID`

Setup steps:

1. Create a Notion integration and copy its secret token.
2. Paste the token into `Notion Token`.
3. Open your target Notion database.
4. Share the database with the integration.
5. Copy the database or data source ID into `Data Source ID`.

The target database must have at least one title property. Other properties are optional; the clipboard body is appended as page content.

### 5. Configure Obsidian

To upload to Obsidian, choose a vault:

1. Find `Obsidian Vault` on the configuration page.
2. Select a detected vault, or manually enter the vault folder path.
3. Enter a save folder in `Obsidian Folder`, such as `Clipboard` or `Inbox/Clipboard`.
4. Missing folders are created automatically when writing.
5. `Obsidian Tags` is optional. You can enter tags like `algorithm cpp study`.

Obsidian files use the detected content title as the filename when possible. If a file with the same name already exists, the app adds a numeric suffix instead of overwriting it.

### 6. Validate And Test Upload

After editing, check it in this order:

1. Click "Validate config" on the configuration page to check the Notion token, data source, Obsidian vault, and folder.
2. If validation passes, click "Test upload". The app writes one real test item using the current page settings.
3. Read the automatically opened upload center and confirm the Notion page link or Obsidian file path.
4. When everything looks right, click "Apply and restart" to save the configuration.

"Test upload" writes an item named `Notion Clipboard Win 测试上传`. It does not save the configuration to the ini file; only "Apply and restart" saves it.

### 7. Change The Hotkey

The default upload hotkey is `Ctrl+Shift+B`. If it conflicts with another app, change it from the configuration page:

1. Find "Global hotkey".
2. Click "Record hotkey".
3. Press a new key combination, such as `Ctrl+Alt+N`.
4. If you pressed the wrong keys, press `Esc` to cancel and record again.
5. Make sure "Enable global hotkey" is checked.
6. Click "Apply and restart" to activate the new hotkey.

The hotkey field only displays the current hotkey. It is not manually editable, which avoids invalid key combinations.

### 8. Upload Content

Daily use takes three steps:

1. Copy content from a browser, editor, chat window, or PDF.
2. Press your configured upload hotkey. The default is `Ctrl+Shift+B`.
3. Wait for the tray notification, or open the upload center from the tray menu.

You can also right-click the tray icon and choose "Upload current clipboard".

### 9. Check The Result

After a successful upload:

- Notion: the upload center includes the Notion page link.
- Obsidian: the Markdown file appears in the configured vault folder.
- The tray menu can open the latest Obsidian note directly.

If an upload fails, it stays in the local queue and can be retried later. The upload center shows waiting and final-failed tasks; use "Retry this item" for one task or "Retry failed tasks" to move all final-failed tasks back into the queue.

The upload center is a local snapshot page. If it has been open for a while, click "Refresh status" to generate the latest view.

## Troubleshooting

### Configuration Changes Did Not Apply

Make sure you clicked "Apply and restart". Copying or downloading the config does not update the currently running tray process.

### Configuration Page Did Not Open Automatically

This is expected. Except for the one post-install launch, open the configuration page manually from the tray menu.

### Obsidian Folder Is Missing

Check that `Obsidian Vault` points to the correct vault. `Obsidian Folder` is a folder inside the vault, such as `Clipboard`, not a full disk path.

### Notion Upload Fails

Common causes:

- The token is incorrect.
- The database was not shared with the integration.
- `data_source_id` is incorrect.
- The target database has no title property.

Open the configuration page, click "Validate config", then use "Test upload" after validation passes.

### Code Blocks Have No Color

Notion highlighting depends on the code block language. The app tries to detect languages such as `cpp`, `sql`, and `python`; if the source has no language marker, Notion may display it as plain text.

### Formulas Do Not Render As Expected

The app supports common `$...$`, `$$...$$`, `\(...\)`, and `\[...\]` formulas. For unusual source text during development, add the sample to `test/` and run a dry run.

## Manual Configuration Examples

If you do not use the configuration page, edit `notion_clipboard_win.ini`. For most users, the configuration page is still recommended because hotkey recording, Obsidian vault selection, and validation are clearer there.

Notion only:

```ini
upload_target=notion
notion_token=secret_xxx
data_source_id=xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
```

Notion and Obsidian:

```ini
upload_target=notion,obsidian
notion_token=secret_xxx
data_source_id=xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
obsidian_vault_dir=E:\obsidian\MyVault
obsidian_folder=Inbox/Clipboard
obsidian_tags=algorithm cpp
```

See [config.example.ini](config.example.ini) for the full template.

## Developer Commands

Most users do not need these commands. They are only for building from source or debugging.

```powershell
cmake -S . -B build
cmake --build build --config Release
```

Run self-tests:

```powershell
cmake -S . -B build-console -DNOTION_CLIPBOARD_WIN_GUI=OFF
cmake --build build-console --config Release
.\build-console\Release\notion_clipboard_win.exe --self-test
```

Dry-run conversion:

```powershell
.\build-console\Release\notion_clipboard_win.exe --dry-run-file .\test\bf.txt
```

Generate an Obsidian Markdown preview file to inspect formulas, blockquotes, and tag front matter:

```powershell
.\build-console\Release\notion_clipboard_win.exe --dry-run-obsidian-file .\test\bf.txt .\test\bf.obsidian.md
```
