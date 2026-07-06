# Changelog

## 0.2.2 - 2026-07-05

- Add a configuration-page test upload action that writes a real test item to the selected Notion and/or Obsidian targets, then opens recent upload results.
- Limit automatic configuration-page opening to the installer post-install launch; normal startup and missing configuration now only show guidance.
- Clarify hotkey recording and configuration validation guidance in the user documentation.
- Keep the stable product focus on Notion and Obsidian while future-only platforms remain out of the main configuration flow.

## 0.2.1 - 2026-07-04

- Add hotkey recording to the local configuration page, with Esc cancellation and automatic `enable_hotkey=true`.
- Rewrite the Chinese and English README files as non-technical user guides.
- Switch the repository license to MIT in the release line following `v0.2.0`.

## 0.2.0 - 2026-07-04

- Focus the stable product surface on Notion and Obsidian.
- Add multi-target uploads with independent queued jobs, such as `upload_target=notion,obsidian`.
- Promote Obsidian to a first-class target with vault/folder selection, new-folder creation, title-based filenames, conflict suffixes, optional YAML tags, and tray access to the latest note.
- Improve the local configuration page with target checkboxes, Obsidian vault/folder scanning, recent upload results, and apply-and-restart.
- Improve Markdown and formula conversion for Obsidian, including loose bracket math, single-dollar inline math, double-dollar display math, and false-positive protections for code-like identifiers.
- Remove local Git and GitHub upload features from the public configuration surface and documentation.

## 0.1.0 - 2026-06-29

- Initial installer release with tray upload, Notion writing, persistent queueing, configuration, and conversion self-tests.
