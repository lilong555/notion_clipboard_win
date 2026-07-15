param(
    [string]$PowerShellExe
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$SourceScript = Join-Path $Root "scripts\build-installer.ps1"
if (-not (Test-Path $SourceScript)) {
    throw "build-installer.ps1 was not found."
}
$SourceContent = Get-Content -Raw $SourceScript

if (-not $PowerShellExe) {
    $CurrentProcess = Get-Process -Id $PID
    $PowerShellExe = $CurrentProcess.Path
    if (-not $PowerShellExe -or -not (Test-Path $PowerShellExe)) {
        $PowerShellExe = "powershell"
    }
}

$TempRoot = Join-Path ([IO.Path]::GetTempPath()) ("notion-clipboard-installer-tests-" + [Guid]::NewGuid().ToString("N"))
$ReleasedChangelog = @"
# Changelog

## 9.9.9 - 2099-01-01

- Released.
"@
$UnreleasedChangelog = @"
# Changelog

## Unreleased

- Pending release note.

## 9.9.8 - 2099-01-01

- Previous release.
"@

function New-Fixture {
    param(
        [string]$Name,
        [string]$Changelog
    )

    $FixtureRoot = Join-Path $TempRoot $Name
    New-Item -ItemType Directory -Force -Path (Join-Path $FixtureRoot "scripts") | Out-Null
    Copy-Item -LiteralPath $SourceScript -Destination (Join-Path $FixtureRoot "scripts\build-installer.ps1")
    Set-Content -Encoding ASCII -Path (Join-Path $FixtureRoot "VERSION") -Value "9.9.9"
    Set-Content -Encoding UTF8 -Path (Join-Path $FixtureRoot "CHANGELOG.md") -Value $Changelog
    return $FixtureRoot
}

function Invoke-InstallerCheck {
    param(
        [string]$FixtureRoot,
        [string[]]$Arguments
    )

    $Script = Join-Path $FixtureRoot "scripts\build-installer.ps1"
    $OutputPath = [IO.Path]::GetTempFileName()
    $ErrorPath = [IO.Path]::GetTempFileName()
    $Process = $null
    try {
        $Process = Start-Process -FilePath $PowerShellExe `
            -ArgumentList (@("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $Script) + $Arguments) `
            -RedirectStandardOutput $OutputPath `
            -RedirectStandardError $ErrorPath `
            -WindowStyle Hidden `
            -Wait `
            -PassThru
        $Output = ((Get-Content -Raw -ErrorAction SilentlyContinue $OutputPath) + `
            (Get-Content -Raw -ErrorAction SilentlyContinue $ErrorPath))
    }
    finally {
        if (Test-Path $OutputPath) {
            Remove-Item -LiteralPath $OutputPath -Force
        }
        if (Test-Path $ErrorPath) {
            Remove-Item -LiteralPath $ErrorPath -Force
        }
    }

    return [pscustomobject]@{
        ExitCode = $Process.ExitCode
        Output = $Output
    }
}

function Assert-Success {
    param(
        [string]$Name,
        [object]$Result
    )

    if ($Result.ExitCode -ne 0) {
        throw "$Name failed unexpectedly.`n$($Result.Output)"
    }
    Write-Host "[PASS] $Name"
}

function Assert-FailureContains {
    param(
        [string]$Name,
        [object]$Result,
        [string]$Expected
    )

    if ($Result.ExitCode -eq 0 -or $Result.Output -notlike "*$Expected*") {
        throw "$Name did not fail with expected message '$Expected'.`nExitCode: $($Result.ExitCode)`n$($Result.Output)"
    }
    Write-Host "[PASS] $Name"
}

function Invoke-Git {
    param(
        [string]$FixtureRoot,
        [string[]]$Arguments
    )

    Push-Location $FixtureRoot
    try {
        $PreviousErrorActionPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        $Output = & git @Arguments 2>&1
        $Status = $LASTEXITCODE
        $ErrorActionPreference = $PreviousErrorActionPreference
        if ($Status -ne 0) {
            throw "git $($Arguments -join ' ') failed.`n$($Output | Out-String)"
        }
    }
    finally {
        $ErrorActionPreference = "Stop"
        Pop-Location
    }
}

function Initialize-GitRepo {
    param([string]$FixtureRoot)

    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        return
    }

    Invoke-Git $FixtureRoot @("init", "--quiet")
    Invoke-Git $FixtureRoot @("config", "user.email", "test@example.invalid")
    Invoke-Git $FixtureRoot @("config", "user.name", "Installer Guard Test")
    Invoke-Git $FixtureRoot @("config", "commit.gpgsign", "false")
    Invoke-Git $FixtureRoot @("config", "core.autocrlf", "false")
    Invoke-Git $FixtureRoot @("add", ".")
    Invoke-Git $FixtureRoot @("commit", "--quiet", "-m", "Initial fixture")
}

New-Item -ItemType Directory -Force -Path $TempRoot | Out-Null
try {
    if ($SourceContent -notmatch 'Invoke-CheckedNative\s+-Command\s+\$CTestPath' -or
        $SourceContent -notmatch '"--test-dir"\s*,\s*"build-console"' -or
        $SourceContent -notmatch '"--output-on-failure"') {
        throw "build-installer.ps1 must run the complete CTest suite before packaging."
    }
    Write-Host "[PASS] installer build runs CTest"

    if ($SourceContent -match '--self-test') {
        throw "build-installer.ps1 must not bypass CTest by running only --self-test."
    }
    Write-Host "[PASS] installer build does not bypass CTest"

    $CheckedCMakeCalls = [regex]::Matches(
        $SourceContent,
        'Invoke-CheckedNative\s+-Command\s+\$CMakePath'
    ).Count
    if ($CheckedCMakeCalls -lt 4 -or
        $SourceContent -notmatch 'Invoke-CheckedNative\s+-Command\s+\$IsccPath') {
        throw "build-installer.ps1 must check native build and installer command exit codes."
    }
    Write-Host "[PASS] native build command failures are checked"

    $CleanFixture = New-Fixture -Name "clean" -Changelog $ReleasedChangelog
    Initialize-GitRepo $CleanFixture
    Assert-Success "check-only accepts released changelog" (Invoke-InstallerCheck $CleanFixture @("-CheckOnly"))

    $UnreleasedFixture = New-Fixture -Name "unreleased" -Changelog $UnreleasedChangelog
    Initialize-GitRepo $UnreleasedFixture
    Assert-FailureContains "check-only blocks unreleased changelog" `
        (Invoke-InstallerCheck $UnreleasedFixture @("-CheckOnly")) `
        "CHANGELOG.md still has Unreleased entries"
    Assert-Success "check-only allows unreleased changelog when requested" `
        (Invoke-InstallerCheck $UnreleasedFixture @("-CheckOnly", "-AllowUnreleased"))

    if (Get-Command git -ErrorAction SilentlyContinue) {
        $TagFixture = New-Fixture -Name "tag-conflict" -Changelog $ReleasedChangelog
        Initialize-GitRepo $TagFixture
        Invoke-Git $TagFixture @("tag", "v9.9.9")
        Set-Content -Encoding ASCII -Path (Join-Path $TagFixture "marker.txt") -Value "move head"
        Invoke-Git $TagFixture @("add", "marker.txt")
        Invoke-Git $TagFixture @("commit", "--quiet", "-m", "Move head")

        Assert-FailureContains "check-only blocks existing version tag on another commit" `
            (Invoke-InstallerCheck $TagFixture @("-CheckOnly")) `
            "Tag v9.9.9 already exists but does not point to HEAD"
        Assert-Success "check-only allows existing version tag when requested" `
            (Invoke-InstallerCheck $TagFixture @("-CheckOnly", "-AllowExistingVersion"))
    }
    else {
        Write-Host "[SKIP] git tag conflict checks require git in PATH"
    }
}
finally {
    if (Test-Path $TempRoot) {
        Remove-Item -LiteralPath $TempRoot -Recurse -Force
    }
}
