param(
    [string]$Configuration = "Release",
    [switch]$AllowUnreleased,
    [switch]$AllowExistingVersion,
    [switch]$CheckOnly
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$AppVersion = (Get-Content -Raw (Join-Path $Root "VERSION")).Trim()

function Invoke-NativeForOutput {
    param(
        [string]$Command,
        [string[]]$Arguments
    )

    $PreviousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $Output = & $Command @Arguments 2>$null
        return [pscustomobject]@{
            ExitCode = $LASTEXITCODE
            Output = $Output
        }
    }
    finally {
        $ErrorActionPreference = $PreviousErrorActionPreference
    }
}

function Invoke-CheckedNative {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Command,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    & $Command @Arguments
    $ExitCode = $LASTEXITCODE
    if ($ExitCode -ne 0) {
        throw "$Description failed with exit code $ExitCode."
    }
}

function Test-ReleaseGuards {
    if ($AppVersion -notmatch '^\d+\.\d+\.\d+$') {
        throw "VERSION must use x.y.z format. Current value: $AppVersion"
    }

    $ChangelogPath = Join-Path $Root "CHANGELOG.md"
    if ((Test-Path $ChangelogPath) -and -not $AllowUnreleased) {
        $Changelog = Get-Content -Raw $ChangelogPath
        $Unreleased = [regex]::Match($Changelog, '(?ms)^## Unreleased[ \t]*(?:\r?\n)+(.*?)(?=^##\s|\z)')
        if ($Unreleased.Success -and $Unreleased.Groups[1].Value -match '(?m)^\s*-\s+\S') {
            throw "CHANGELOG.md still has Unreleased entries. Update VERSION and move those entries to a release section before building a release installer, or pass -AllowUnreleased for a local test installer."
        }
    }

    $GitCommand = Get-Command git -ErrorAction SilentlyContinue
    if ($GitCommand -and -not $AllowExistingVersion) {
        Push-Location $Root
        try {
            $VersionTag = "v$AppVersion"
            $HeadResult = Invoke-NativeForOutput $GitCommand.Source @("rev-parse", "HEAD")
            $TagResult = Invoke-NativeForOutput $GitCommand.Source @("rev-parse", "--verify", "$VersionTag^{commit}")
            if ($HeadResult.ExitCode -eq 0 -and $TagResult.ExitCode -eq 0) {
                $HeadCommit = ($HeadResult.Output | Select-Object -First 1).Trim()
                $TagCommit = ($TagResult.Output | Select-Object -First 1).Trim()
                if ($HeadCommit -and $TagCommit -and $HeadCommit -ne $TagCommit) {
                    throw "Tag $VersionTag already exists but does not point to HEAD. Bump VERSION before building a release installer, or pass -AllowExistingVersion for a local test installer."
                }
            }
        }
        finally {
            Pop-Location
        }
    }
}

Test-ReleaseGuards

if ($CheckOnly) {
    Write-Host "Release guards passed for version $AppVersion."
    exit 0
}

$CMakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
$CMakePath = if ($CMakeCommand) { $CMakeCommand.Source } else { $null }

if (-not $CMakePath) {
    $VsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $VsWhere) {
        $VsInstall = & $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($VsInstall) {
            $Candidate = Join-Path $VsInstall "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
            if (Test-Path $Candidate) {
                $CMakePath = $Candidate
            }
        }
    }
}

if (-not $CMakePath) {
    throw "cmake was not found. Install CMake or Visual Studio with C++ tools."
}

$CTestPath = Join-Path (Split-Path -Parent $CMakePath) "ctest.exe"
if (-not (Test-Path -LiteralPath $CTestPath)) {
    throw "ctest.exe was not found next to cmake: $CTestPath"
}

$IsccCommand = Get-Command ISCC.exe -ErrorAction SilentlyContinue
$IsccPath = if ($IsccCommand) { $IsccCommand.Source } else { $null }
if (-not $IsccPath) {
    $InnoCandidates = @(
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles}\Inno Setup 6\ISCC.exe"
    )
    foreach ($Candidate in $InnoCandidates) {
        if (Test-Path $Candidate) {
            $IsccPath = $Candidate
            break
        }
    }
}

Push-Location $Root
try {
    Invoke-CheckedNative -Command $CMakePath `
        -Arguments @("-S", ".", "-B", "build-console", "-DNOTION_CLIPBOARD_WIN_GUI=OFF") `
        -Description "Console build configuration"
    Invoke-CheckedNative -Command $CMakePath `
        -Arguments @("--build", "build-console", "--config", $Configuration) `
        -Description "Console build"
    Invoke-CheckedNative -Command $CTestPath `
        -Arguments @("--test-dir", "build-console", "-C", $Configuration, "--output-on-failure") `
        -Description "CTest suite"

    $ReleaseExe = Join-Path $Root "build\$Configuration\notion_clipboard_win.exe"
    $RunningRelease = Get-CimInstance Win32_Process -Filter "name = 'notion_clipboard_win.exe'" |
        Where-Object { $_.ExecutablePath -and ((Resolve-Path $_.ExecutablePath).Path -eq (Resolve-Path $ReleaseExe -ErrorAction SilentlyContinue).Path) }
    if ($RunningRelease) {
        throw "build\$Configuration\notion_clipboard_win.exe is running. Exit it from the tray before building the installer."
    }

    Invoke-CheckedNative -Command $CMakePath `
        -Arguments @("-S", ".", "-B", "build", "-DNOTION_CLIPBOARD_WIN_GUI=ON") `
        -Description "GUI build configuration"
    Invoke-CheckedNative -Command $CMakePath `
        -Arguments @("--build", "build", "--config", $Configuration) `
        -Description "GUI build"

    if (-not $IsccPath) {
        Write-Host "Inno Setup compiler ISCC.exe was not found."
        Write-Host "Install it with: winget install JRSoftware.InnoSetup"
        throw "Inno Setup is required to build the installer."
    }

    New-Item -ItemType Directory -Force -Path ".\dist" | Out-Null
    Invoke-CheckedNative -Command $IsccPath `
        -Arguments @("/DAppVersion=$AppVersion", ".\installer\notion-clipboard-win.iss") `
        -Description "Installer compilation"

    $Installer = ".\dist\NotionClipboardWin-$AppVersion-Setup.exe"
    if (-not (Test-Path $Installer)) {
        throw "Installer was not generated: $Installer"
    }

    $Hash = Get-FileHash -Algorithm SHA256 $Installer
    "$($Hash.Hash)  $(Split-Path $Installer -Leaf)" | Set-Content -Encoding ASCII "$Installer.sha256"
    Write-Host "Generated installer: $Installer"
    Write-Host "SHA256: $($Hash.Hash)"
}
finally {
    Pop-Location
}
