param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$Tag,
    [string]$NotesOutput
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$VersionPath = Join-Path $Root "VERSION"
$ChangelogPath = Join-Path $Root "CHANGELOG.md"

if (-not (Test-Path -LiteralPath $VersionPath)) {
    throw "VERSION was not found."
}
if (-not (Test-Path -LiteralPath $ChangelogPath)) {
    throw "CHANGELOG.md was not found."
}

$Version = (Get-Content -Raw -LiteralPath $VersionPath).Trim()
if ($Version -notmatch '^\d+\.\d+\.\d+$') {
    throw "VERSION must use x.y.z format. Current value: $Version"
}

$ExpectedTag = "v$Version"
if (-not [string]::Equals($Tag, $ExpectedTag, [StringComparison]::Ordinal)) {
    throw "Release tag $Tag does not match VERSION $Version. Expected tag: $ExpectedTag"
}

$Changelog = Get-Content -Raw -LiteralPath $ChangelogPath
$Unreleased = [regex]::Match($Changelog, '(?ms)^## Unreleased[ \t]*(?:\r?\n)+(.*?)(?=^##\s|\z)')
if ($Unreleased.Success -and $Unreleased.Groups[1].Value -match '(?m)^\s*-\s+\S') {
    throw "CHANGELOG.md still has Unreleased entries. Move them to the $Version release section before tagging."
}

$VersionPattern = [regex]::Escape($Version)
$ReleasePattern = "(?ms)^##[ \t]+$VersionPattern[ \t]+-[ \t]+\d{4}-\d{2}-\d{2}[ \t]*\r?\n.*?(?=^##[ \t]+|\z)"
$ReleaseSections = [regex]::Matches($Changelog, $ReleasePattern)
if ($ReleaseSections.Count -ne 1) {
    throw "CHANGELOG.md must contain exactly one release section named '## $Version - YYYY-MM-DD'."
}

$ReleaseNotes = $ReleaseSections[0].Value.Trim()
if ($ReleaseNotes -notmatch '(?m)^\s*-\s+\S') {
    throw "The CHANGELOG.md section for $Version must contain at least one release note."
}

if ($NotesOutput) {
    $NotesPath = [IO.Path]::GetFullPath($NotesOutput)
    $NotesDirectory = Split-Path -Parent $NotesPath
    if ($NotesDirectory) {
        New-Item -ItemType Directory -Force -Path $NotesDirectory | Out-Null
    }
    [IO.File]::WriteAllText($NotesPath, $ReleaseNotes + "`n", [Text.UTF8Encoding]::new($false))
    Write-Host "Release notes written to $NotesPath"
}

Write-Host "Release metadata validated for $ExpectedTag."
