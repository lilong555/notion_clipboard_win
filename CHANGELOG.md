# Changelog

## Unreleased

- Guard release installer builds when the current VERSION tag already exists on a different commit.
- Add a `-CheckOnly` mode to the installer build script for fast release-guard verification.
- Document the release guard and installer packaging commands for contributors.
- Keep Obsidian Markdown debug output out of the user README while documenting it for contributors.
- Add a lightweight regression script for installer release-guard checks.
- Fix release guard git probes so missing version tags are handled by exit code instead of stderr noise.
- Keep the Obsidian Markdown debug command hidden from public `--help` output.
- Replace the configuration page's debug wording for exported ini with backup and migration wording.
- Keep the example configuration focused on the current Notion and Obsidian targets.
- Prefer local Markdown file links over `obsidian://` links in the save records page to avoid vault lookup errors.
- Prefer existing local Markdown files when opening the latest Obsidian note from the tray.

## 0.2.4 - 2026-07-06

- Remove obsolete configuration diagnostics protocol handlers and dead tray command paths after the UI cleanup.
- Keep the configuration page focused on Notion and Obsidian by removing the visible future-platform notice.
- Hide internal configuration-page protocol arguments from the command-line help.
- Hide the legacy configuration validation command from the command-line help while keeping it available for compatibility.
- Add a command-line help regression test to keep internal and future-only options out of the public help surface.

## 0.2.3 - 2026-07-06

- Add a local save records page with recent results, waiting/final-failed queue visibility, refresh, and retry actions for failed items.
- Simplify the tray and configuration-page surface around Notion and Obsidian, with future-only targets removed from the main user flow.
- Use save-oriented wording across the tray menu, configuration page, save records, CLI help, logs, example config, and user documentation.
- Move technical fields and raw ini output behind advanced sections, and keep debug-only Markdown preview and diagnostics out of the default UI.
- Improve Obsidian Markdown output for quoted loose formulas, indexed formulas, and duplicate Markdown titles.
- Avoid hardcoded installer versions in the README download instructions.
- Guard installer builds when `CHANGELOG.md` still has Unreleased entries, with an explicit local-test override.

## 0.2.2 - 2026-07-05

- Add a configuration-page test save action that writes a real test item to the selected Notion and/or Obsidian targets, then opens save records.
- Limit automatic configuration-page opening to the installer post-install launch; normal startup and missing configuration now only show guidance.
- Clarify hotkey recording and configuration validation guidance in the user documentation.
- Keep the stable product focus on Notion and Obsidian while future-only platforms remain out of the main configuration flow.

## 0.2.1 - 2026-07-04

- Add hotkey recording to the local configuration page, with Esc cancellation and automatic `enable_hotkey=true`.
- Rewrite the Chinese and English README files as non-technical user guides.
- Switch the repository license to MIT in the release line following `v0.2.0`.

## 0.2.0 - 2026-07-04

- Focus the stable product surface on Notion and Obsidian.
- Add multi-target saves with independent queued jobs, such as `upload_target=notion,obsidian`.
- Promote Obsidian to a first-class target with vault/folder selection, new-folder creation, title-based filenames, conflict suffixes, optional YAML tags, and tray access to the latest note.
- Improve the local configuration page with target checkboxes, Obsidian vault/folder scanning, save records, and apply-and-restart.
- Improve Markdown and formula conversion for Obsidian, including loose bracket math, single-dollar inline math, double-dollar display math, and false-positive protections for code-like identifiers.
- Remove local Git and GitHub save targets from the public configuration surface and documentation.

## 0.1.0 - 2026-06-29

- Initial installer release with tray save, Notion writing, persistent queueing, configuration, and conversion self-tests.
