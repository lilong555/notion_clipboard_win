# Repository Guidelines

## 语言与风格

- 与用户沟通使用中文。
- 代码注释使用中文，保持简短，只解释不明显的设计意图。
- C++ 代码保持 C++17，优先使用标准库和 Win32/WinHTTP，不引入第三方依赖。
- 稳定用户目标只包含 Notion 和 Obsidian；Webhook、语雀、飞书文档只能在 `NCW_ENABLE_EXPERIMENTAL_TARGETS=true` 下用于开发测试。

## 构建

- Windows/MSVC：
  - `cmake -S . -B build`
  - `cmake --build build --config Release`
- 无控制台后台版：
  - `cmake -S . -B build -DNOTION_CLIPBOARD_WIN_GUI=ON`
  - `cmake --build build --config Release`
- 发版门禁：
  - `powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-installer.ps1 -CheckOnly`
- 开发分支门禁快检：
  - `powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-installer.ps1 -CheckOnly -AllowUnreleased -AllowExistingVersion`
- 生成安装包：
  - `powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-installer.ps1`

## 测试

- 转换回归：`ctest --test-dir build-console -C Release --output-on-failure`
- 通用 dry run：`.\build-console\Release\notion_clipboard_win.exe --dry-run-file .\test\bf.txt`
- Obsidian 调试输出：`.\build-console\Release\notion_clipboard_win.exe --dry-run-obsidian-file .\test\bf.txt .\test\bf.obsidian.md`
- 安装包脚本回归：`powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\test-build-installer.ps1`
- CI：`.github/workflows/ci.yml` 在 Windows 上运行 console 构建、CTest 自测、门禁快检和安装包脚本回归。

## 安全

- 不要提交真实 Notion token、数据库 ID 或本地配置文件。
- `notion_clipboard_win.ini`、日志和队列状态都应保持本地。
