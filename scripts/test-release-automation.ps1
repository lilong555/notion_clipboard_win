param(
    [string]$PowerShellExe
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$SourceScript = Join-Path $Root "scripts\prepare-release.ps1"
$WorkflowPath = Join-Path $Root ".github\workflows\release.yml"
if (-not (Test-Path -LiteralPath $SourceScript)) {
    throw "prepare-release.ps1 was not found."
}
if (-not (Test-Path -LiteralPath $WorkflowPath)) {
    throw "release.yml was not found."
}

if (-not $PowerShellExe) {
    $CurrentProcess = Get-Process -Id $PID
    $PowerShellExe = $CurrentProcess.Path
    if (-not $PowerShellExe -or -not (Test-Path -LiteralPath $PowerShellExe)) {
        $PowerShellExe = "powershell"
    }
}

$TempRoot = Join-Path ([IO.Path]::GetTempPath()) ("notion-clipboard-release-tests-" + [Guid]::NewGuid().ToString("N"))

function New-Fixture {
    param(
        [string]$Name,
        [string]$Version,
        [string]$Changelog
    )

    $FixtureRoot = Join-Path $TempRoot $Name
    New-Item -ItemType Directory -Force -Path (Join-Path $FixtureRoot "scripts") | Out-Null
    Copy-Item -LiteralPath $SourceScript -Destination (Join-Path $FixtureRoot "scripts\prepare-release.ps1")
    Set-Content -Encoding ASCII -Path (Join-Path $FixtureRoot "VERSION") -Value $Version
    Set-Content -Encoding UTF8 -Path (Join-Path $FixtureRoot "CHANGELOG.md") -Value $Changelog
    return $FixtureRoot
}

function Invoke-PrepareRelease {
    param(
        [string]$FixtureRoot,
        [string]$Tag,
        [switch]$WriteNotes
    )

    $Script = Join-Path $FixtureRoot "scripts\prepare-release.ps1"
    $OutputPath = [IO.Path]::GetTempFileName()
    $ErrorPath = [IO.Path]::GetTempFileName()
    $NotesPath = Join-Path $FixtureRoot "release-notes.md"
    $Arguments = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $Script, "-Tag", $Tag)
    if ($WriteNotes) {
        $Arguments += @("-NotesOutput", $NotesPath)
    }

    try {
        $Process = Start-Process -FilePath $PowerShellExe `
            -ArgumentList $Arguments `
            -RedirectStandardOutput $OutputPath `
            -RedirectStandardError $ErrorPath `
            -WindowStyle Hidden `
            -Wait `
            -PassThru
        $Output = ((Get-Content -Raw -ErrorAction SilentlyContinue $OutputPath) +
            (Get-Content -Raw -ErrorAction SilentlyContinue $ErrorPath))
    }
    finally {
        Remove-Item -LiteralPath $OutputPath, $ErrorPath -Force -ErrorAction SilentlyContinue
    }

    return [pscustomobject]@{
        ExitCode = $Process.ExitCode
        Output = $Output
        NotesPath = $NotesPath
    }
}

function Assert-Success {
    param([string]$Name, [object]$Result)
    if ($Result.ExitCode -ne 0) {
        throw "$Name failed unexpectedly.`n$($Result.Output)"
    }
    Write-Host "[PASS] $Name"
}

function Assert-FailureContains {
    param([string]$Name, [object]$Result, [string]$Expected)
    if ($Result.ExitCode -eq 0 -or $Result.Output -notlike "*$Expected*") {
        throw "$Name did not fail with expected message '$Expected'.`nExitCode: $($Result.ExitCode)`n$($Result.Output)"
    }
    Write-Host "[PASS] $Name"
}

$ReleasedChangelog = @"
# Changelog

## Unreleased

## 9.9.9 - 2099-01-01

- Shipped the verified installer.

## 9.9.8 - 2098-01-01

- Previous release.
"@

New-Item -ItemType Directory -Force -Path $TempRoot | Out-Null
try {
    $ValidFixture = New-Fixture -Name "valid" -Version "9.9.9" -Changelog $ReleasedChangelog
    $ValidResult = Invoke-PrepareRelease -FixtureRoot $ValidFixture -Tag "v9.9.9" -WriteNotes
    Assert-Success "matching tag writes release notes" $ValidResult
    $Notes = (Get-Content -Raw -LiteralPath $ValidResult.NotesPath).Trim()
    if ($Notes -ne "## 9.9.9 - 2099-01-01`n`n- Shipped the verified installer." -and
        $Notes -ne "## 9.9.9 - 2099-01-01`r`n`r`n- Shipped the verified installer.") {
        throw "release notes did not contain only the current version section.`n$Notes"
    }
    Write-Host "[PASS] release notes contain only the current version"

    Assert-FailureContains "mismatched tag is rejected" `
        (Invoke-PrepareRelease -FixtureRoot $ValidFixture -Tag "v9.9.8") `
        "does not match VERSION"

    $PendingFixture = New-Fixture -Name "pending" -Version "9.9.9" -Changelog ($ReleasedChangelog -replace
        "## Unreleased\r?\n", "## Unreleased`r`n`r`n- Pending change.`r`n")
    Assert-FailureContains "unreleased entries are rejected" `
        (Invoke-PrepareRelease -FixtureRoot $PendingFixture -Tag "v9.9.9") `
        "still has Unreleased entries"

    $MissingFixture = New-Fixture -Name "missing" -Version "9.9.9" -Changelog ($ReleasedChangelog -replace
        "9\.9\.9", "9.9.7")
    Assert-FailureContains "missing release section is rejected" `
        (Invoke-PrepareRelease -FixtureRoot $MissingFixture -Tag "v9.9.9") `
        "must contain exactly one release section"

    $Workflow = Get-Content -Raw -LiteralPath $WorkflowPath
    foreach ($Required in @(
            "tags:",
            "scripts/prepare-release.ps1",
            "scripts/build-installer.ps1",
            "actions/upload-artifact@v7",
            "Get-FileHash -Algorithm SHA256",
            "gh release create",
            "gh release upload"
        )) {
        if ($Workflow -notlike "*$Required*") {
            throw "release.yml is missing required release step: $Required"
        }
    }
    Write-Host "[PASS] release workflow keeps build, artifact, and publishing steps"
}
finally {
    Remove-Item -LiteralPath $TempRoot -Recurse -Force -ErrorAction SilentlyContinue
}
