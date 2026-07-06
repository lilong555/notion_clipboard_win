# Notion Clipboard Win

语言：中文 | [English](README.en.md)

Notion Clipboard Win 是一个 Windows 托盘工具。它可以把你复制的内容一键保存到 Notion 或 Obsidian，适合保存网页内容、题解、代码片段、学习笔记和带公式的 Markdown 文本。

项目当前聚焦两条稳定路径：**Notion** 和 **Obsidian**。程序使用 C++17、Win32 和 WinHTTP 实现，不依赖第三方运行时。

## 主要能力

- 默认热键 `Ctrl+Shift+B` 上传当前剪贴板。
- 支持同时写入 Notion 和 Obsidian。
- 支持 Markdown、HTML、代码块、表格、行内公式和整行公式。
- 内置配置页面，不需要手写配置文件，并可直接录制新的全局热键和测试上传。
- 上传失败会自动进入队列，之后可以继续重试。
- 托盘菜单可以上传当前剪贴板、打开上传中心、查看日志和打开最新 Obsidian 笔记。

## 普通用户教程

### 1. 下载和安装

1. 打开 [GitHub Releases](https://github.com/lilong555/notion_clipboard_win/releases)。
2. 下载最新的 `NotionClipboardWin-0.2.2-Setup.exe`。
3. 双击安装。
4. 从开始菜单启动 `Notion Clipboard Win`。
5. 启动后程序会出现在 Windows 右下角托盘区域。

安装完成并勾选启动程序时，配置页面会自动打开一次。之后程序不会再自动弹出配置页面，需要你从托盘菜单手动打开。

如果看不到托盘图标，点一下任务栏右下角的“上箭头”，Windows 可能把它折叠起来了。

### 2. 打开配置页面

1. 右键托盘图标。
2. 选择“打开配置页面”。
3. 浏览器会打开一个本地配置页面。
4. 配好后点击“应用并重启”。

配置页面只在本机打开，用来生成和保存本地 ini 配置。除安装完成后的首次启动外，程序不会自动打开这个页面。

### 3. 选择保存位置

你可以只选择一个目标，也可以同时选择两个：

- 只保存到 Notion：勾选 `Notion`。
- 只保存到 Obsidian：勾选 `Obsidian`。
- 同时保存到两个地方：同时勾选 `Notion` 和 `Obsidian`。

保存后配置里对应的是：

```ini
upload_target=notion,obsidian
```

### 4. 配置 Notion

如果你要上传到 Notion，需要准备两个信息：

- `Notion Token`
- `Notion 数据源 ID`

获取方式：

1. 打开 Notion 的 integrations 页面，新建一个 integration。
2. 复制它的 secret token，填到配置页面的 `Notion Token`。
3. 打开你的目标数据库。
4. 将这个数据库共享给刚创建的 integration。
5. 复制数据库或 data source 的 ID，填到 `Notion 数据源 ID`。

目标数据库至少需要一个标题属性。其他属性可以不填，正文会作为页面内容追加。

### 5. 配置 Obsidian

如果你要上传到 Obsidian，需要选择一个仓库：

1. 在配置页面找到 `Obsidian 仓库`。
2. 选择已经识别出来的仓库，或手动填写仓库文件夹路径。
3. 在 `Obsidian 子目录` 里填写保存位置，例如 `Clipboard` 或 `Inbox/Clipboard`。
4. 如果文件夹不存在，程序会在写入时自动创建。
5. `Obsidian 标签` 是可选项，可以填写 `algorithm cpp study` 这类标签。

写入 Obsidian 时，文件名会尽量使用内容标题。如果同名文件已经存在，会自动追加编号，避免覆盖旧笔记。

### 6. 测试上传并应用

配置完成后建议按这个顺序检查：

1. 点击配置页面里的“测试上传”。程序会用当前页面里的配置真实写入一条测试内容。
2. 查看自动打开的“上传中心”，确认 Notion 页面链接或 Obsidian 文件路径是否正确。
3. 如果测试失败，按上传中心里的错误提示修正配置后再试一次。
4. 确认无误后，点击“应用并重启”让配置成为正式配置。

“测试上传”会写入一条名为 `Notion Clipboard Win 测试上传` 的内容。它不会把配置保存到 ini；只有点击“应用并重启”才会保存配置。

### 7. 调整热键

默认上传热键是 `Ctrl+Shift+B`。如果这个组合键和其他软件冲突，可以在配置页面修改：

1. 找到“全局热键”。
2. 点击“录制热键”。
3. 按下新的组合键，例如 `Ctrl+Alt+N`。
4. 如果按错了，按 `Esc` 取消后重新录制。
5. 确认“启用全局热键”已勾选。
6. 点击“应用并重启”让新热键生效。

热键输入框只用于显示当前热键，不支持手动输入，避免填入无法识别的组合键。

### 8. 上传一次内容

日常使用只需要三步：

1. 在网页、编辑器、聊天窗口或 PDF 里复制一段内容。
2. 按你配置的上传热键，默认是 `Ctrl+Shift+B`。
3. 等待托盘通知，或在托盘菜单中打开“上传中心”。

你也可以右键托盘图标，选择“上传当前剪贴板”。

### 9. 查看上传结果

上传成功后：

- Notion：上传中心会显示 Notion 页面链接。
- Obsidian：文件会出现在你配置的仓库和子目录中。
- 托盘菜单里的“打开最近 Obsidian 笔记”可以直接打开最新保存的笔记。

如果上传失败，任务会保存在本地队列里，之后会继续重试。上传中心会显示等待重试和最终失败的任务；对最终失败的任务，可以点击“重试此项”单独重试，也可以点击“重试失败任务”批量放回队列。

上传中心是本地快照页面。如果页面已经打开了一段时间，点击“刷新状态”会重新生成最新状态。

## 常见问题

### 配置页面修改后没有生效

请确认点击的是“应用并重启”，不是只复制配置。应用重启后，新配置才会被正在运行的托盘程序读取。

### 配置页面没有自动打开

这是正常行为。除了安装完成并勾选启动程序的那一次，配置页面都需要从托盘菜单手动打开。

### Obsidian 找不到文件夹

先确认 `Obsidian 仓库` 选的是正确的仓库。`Obsidian 子目录` 是仓库内部的子目录，例如 `Clipboard`，不是完整磁盘路径。

### Notion 上传失败

常见原因有：

- token 填错。
- 数据库没有共享给 integration。
- `data_source_id` 填错。
- 目标数据库没有标题属性。

可以先打开配置页面，点击“测试上传”确认是否能真实写入；失败原因会显示在上传中心。

### 代码块没有颜色

Notion 的代码高亮依赖代码块语言。程序会尽量识别 `cpp`、`sql`、`python` 等语言；如果原文没有语言标记，Notion 可能按普通文本显示。

### 公式没有按预期显示

程序支持常见的 `$...$`、`$$...$$`、`\(...\)`、`\[...\]`。如果原文格式很松散，开发调试时可以把问题样例放到 `test/` 后运行 dry run。

## 手动配置示例

如果你不使用配置页面，也可以编辑 `notion_clipboard_win.ini`。普通用户仍建议优先使用配置页面，因为热键录制、Obsidian 仓库选择和测试上传都在页面里更直观。

只上传到 Notion：

```ini
upload_target=notion
notion_token=secret_xxx
data_source_id=xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
```

同时上传到 Notion 和 Obsidian：

```ini
upload_target=notion,obsidian
notion_token=secret_xxx
data_source_id=xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
obsidian_vault_dir=E:\obsidian\MyVault
obsidian_folder=Inbox/Clipboard
obsidian_tags=algorithm cpp
```

完整配置见 [config.example.ini](config.example.ini)。

## 开发者命令

普通用户不需要执行这些命令。只有从源码构建或调试时才需要。

```powershell
cmake -S . -B build
cmake --build build --config Release
```

运行自测：

```powershell
cmake -S . -B build-console -DNOTION_CLIPBOARD_WIN_GUI=OFF
cmake --build build-console --config Release
.\build-console\Release\notion_clipboard_win.exe --self-test
```

转换 dry run：

```powershell
.\build-console\Release\notion_clipboard_win.exe --dry-run-file .\test\bf.txt
```

生成 Obsidian Markdown 调试文件，用来检查公式、引用块和标签 front matter：

```powershell
.\build-console\Release\notion_clipboard_win.exe --dry-run-obsidian-file .\test\bf.txt .\test\bf.obsidian.md
```
