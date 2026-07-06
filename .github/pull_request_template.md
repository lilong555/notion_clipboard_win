## Summary

-

## Verification

- [ ] `ctest --test-dir build-console -C Release --output-on-failure`
- [ ] `powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-installer.ps1 -CheckOnly -AllowUnreleased -AllowExistingVersion`
- [ ] `powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\test-build-installer.ps1`

## Notes

- Config or migration impact:
- Screenshots or save-record examples, if UI behavior changed:
- Sensitive data checked: no tokens, database IDs, private notes, logs, or local config files are included.
