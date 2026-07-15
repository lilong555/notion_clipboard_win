# Notion Clipboard Win

[![CI](https://github.com/lilong555/notion_clipboard_win/actions/workflows/ci.yml/badge.svg)](https://github.com/lilong555/notion_clipboard_win/actions/workflows/ci.yml)

Language: [中文](README.md) | English

Notion Clipboard Win is a Windows tray app that saves copied content to Notion or Obsidian with one hotkey. It is useful for web clips, solution notes, code snippets, study notes, Markdown text, and formula-heavy content.

The project currently focuses on two stable targets: **Notion** and **Obsidian**. It is implemented with C++17, Win32, and WinHTTP, with no third-party runtime dependency.

## Features

- Save the current clipboard with the default `Ctrl+Shift+B` hotkey.
- Write to Notion, Obsidian, or both at the same time.
- Convert Markdown, HTML, code blocks, tables, inline formulas, and display formulas.
- Built-in local configuration page, so most users do not need to edit ini files by hand, including hotkey recording and test save.
- Failed saves are queued locally and can be retried later.
- Tray menu for saving the current clipboard, opening save records, viewing logs, and opening the latest Obsidian note.

## User Guide

### 1. Download And Install

1. Open [GitHub Releases](https://github.com/lilong555/notion_clipboard_win/releases).
2. Download the latest `NotionClipboardWin-version-Setup.exe` installer.
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

### 3. Choose Save Locations

You can choose one target or both:

- Notion only: check `Notion`.
- Obsidian only: check `Obsidian`.
- Both: check `Notion` and `Obsidian`.

The saved config uses:

```ini
upload_target=notion,obsidian
```

### 4. Configure Notion

To save to Notion, you need:

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

To save to Obsidian, choose a vault:

1. Find `Obsidian Vault` on the configuration page.
2. Select a detected vault, or manually enter the vault folder path.
3. Enter a save folder in `Obsidian Folder`, such as `Clipboard` or `Inbox/Clipboard`.
4. Missing folders are created automatically when writing.
5. `Obsidian Tags` is optional. You can enter tags like `algorithm cpp study`.

If you just created a vault or folder and it is not listed yet, click "重新扫描 Obsidian" on the configuration page. It rescans using the current unsaved vault path on the page and does not write the ini configuration.

Obsidian files use the detected content title as the filename when possible. If a file with the same name already exists, the app adds a numeric suffix instead of overwriting it.

### 6. Test Save And Apply

After editing, check it in this order:

1. Click "Test save" on the configuration page. The app writes one real test item using the current page settings.
2. Read the automatically opened save records page and confirm the Notion page link or Obsidian file path.
3. If the test fails, fix the settings using the save records error and try again.
4. When everything looks right, click "Apply and restart" to save the configuration.

"Test save" writes an item named `Notion Clipboard Win 测试保存`. It does not save the configuration to the ini file; only "Apply and restart" saves it.

### 7. Change The Hotkey

The default save hotkey is `Ctrl+Shift+B`. If it conflicts with another app, change it from the configuration page:

1. Find "Global hotkey".
2. Click "Record hotkey".
3. Press a new key combination, such as `Ctrl+Alt+N`.
4. If you pressed the wrong keys, press `Esc` to cancel and record again.
5. Make sure "Enable global hotkey" is checked.
6. Click "Apply and restart" to activate the new hotkey.

The hotkey field only displays the current hotkey. It is not manually editable, which avoids invalid key combinations.

### 8. Save Content

Daily use takes three steps:

1. Copy content from a browser, editor, chat window, or PDF.
2. Press your configured save hotkey. The default is `Ctrl+Shift+B`.
3. Wait for the tray notification, or open save records from the tray menu.

You can also right-click the tray icon and choose "保存当前剪贴板" (save current clipboard).

### 9. Check The Result

After a successful save:

- Notion: save records include the Notion page link.
- Obsidian: the Markdown file appears in the configured vault folder.
- The tray menu can open the latest Obsidian note directly.

If a save fails, it stays in the local queue and can be retried later. Save records show waiting and final-failed tasks; use "Retry this item" for one task or "Retry failed tasks" to move all final-failed tasks back into the queue.

Save records are shown as a local snapshot page. If it has been open for a while, click "Refresh status" to generate the latest view.

## Troubleshooting

### Configuration Changes Did Not Apply

Make sure you clicked "Apply and restart". Copying or downloading the config does not update the currently running tray process.

### Configuration Page Did Not Open Automatically

This is expected. Except for the one post-install launch, open the configuration page manually from the tray menu.

### Obsidian Folder Is Missing

Check that `Obsidian Vault` points to the correct vault. `Obsidian Folder` is a folder inside the vault, such as `Clipboard`, not a full disk path.

If you just created the folder on disk, return to the configuration page and click "重新扫描 Obsidian". Rescan only refreshes the list; click "Apply and restart" afterward when the settings are correct.

### Notion Save Fails

Common causes:

- The token is incorrect.
- The database was not shared with the integration.
- `data_source_id` is incorrect.
- The target database has no title property.

Open the configuration page and click "Test save" to confirm a real write. Save records show the failure reason if it does not work.

### Code Blocks Have No Color

Notion highlighting depends on the code block language. The app tries to detect languages such as `cpp`, `sql`, and `python`; if the source has no language marker, Notion may display it as plain text.

### Formulas Do Not Render As Expected

The app supports common `$...$`, `$$...$$`, `\(...\)`, and `\[...\]` formulas. If the saved result looks wrong, reproduce it with "Test save" on the configuration page; save records show the target and failure reason. When reporting an issue, include the source text and whether the target is Notion or Obsidian.

## Reporting Issues

Use the Bug report form in GitHub Issues when possible. Include the app version, Windows version, save target, smallest reproducible source text, and the save records error. The app version is shown at the top of the configuration page and in the tray tooltip. Do not include Notion tokens, database IDs, or private note content.

## Manual Configuration Examples

If you do not use the configuration page, edit `notion_clipboard_win.ini`. For most users, the configuration page is still recommended because hotkey recording, Obsidian vault selection, and test save are clearer there.

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

Run the full test suite:

```powershell
cmake -S . -B build-console -DNOTION_CLIPBOARD_WIN_GUI=OFF
cmake --build build-console --config Release
ctest --test-dir build-console -C Release --output-on-failure
```

CTest runs the built-in self-test, Notion and Obsidian long-form conversions of `test/bf.txt`, and structural checks on the Obsidian output. CI also runs release-guard and installer-guard checks; before submitting changes, run at least the CTest command above.

Check release guards before packaging:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-installer.ps1 -CheckOnly
```

This only checks release gates and does not build the installer. It fails if `CHANGELOG.md` still has `Unreleased` entries, or if the `VERSION` tag already exists on a different commit.

For normal development branches, use this to check that the script itself still works:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-installer.ps1 -CheckOnly -AllowUnreleased -AllowExistingVersion
```

Build the installer:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-installer.ps1
```

The script builds the console app and runs the full CTest suite before building the GUI app and writing the installer plus `.sha256` file to `dist/`. Any failed build, test, or packaging command stops the script immediately. Inno Setup must be installed locally.

For a public release, update `VERSION`, move the pending `CHANGELOG.md` entries into the matching version section, commit, and push the matching `vX.Y.Z` tag. GitHub Actions rebuilds and tests the installer, retains the workflow artifact, and creates the GitHub Release from that changelog section. A mismatched tag, version, or changelog stops publication.

Dry-run conversion:

```powershell
.\build-console\Release\notion_clipboard_win.exe --dry-run-file .\test\bf.txt
```
