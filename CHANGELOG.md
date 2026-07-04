# Changelog

## 0.2.0 - 2026-07-04

- Focus the stable product surface on Notion and Obsidian.
- Add multi-target uploads with independent queued jobs, such as `upload_target=notion,obsidian`.
- Promote Obsidian to a first-class target with vault/folder selection, new-folder creation, title-based filenames, conflict suffixes, optional YAML tags, and tray access to the latest note.
- Improve the local configuration page with target checkboxes, Obsidian vault/folder scanning, output preview, validation, diagnostics, recent upload results, and apply-and-restart.
- Improve Markdown and formula conversion for Obsidian, including loose bracket math, single-dollar inline math, double-dollar display math, and false-positive protections for code-like identifiers.
- Remove local Git and GitHub upload features from the public configuration surface and documentation.

## 0.1.0 - 2026-06-29

- Initial installer release with tray upload, Notion writing, persistent queueing, configuration, and conversion self-tests.
