param(
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$AppVersion = (Get-Content -Raw (Join-Path $Root "VERSION")).Trim()
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
    throw "未找到 cmake。请安装 CMake，或安装带 C++ 工具的 Visual Studio。"
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
    & $CMakePath -S . -B build-console -DNOTION_CLIPBOARD_WIN_GUI=OFF
    & $CMakePath --build build-console --config $Configuration
    & ".\build-console\$Configuration\notion_clipboard_win.exe" --self-test

    $ReleaseExe = Join-Path $Root "build\$Configuration\notion_clipboard_win.exe"
    $RunningRelease = Get-CimInstance Win32_Process -Filter "name = 'notion_clipboard_win.exe'" |
        Where-Object { $_.ExecutablePath -and ((Resolve-Path $_.ExecutablePath).Path -eq (Resolve-Path $ReleaseExe -ErrorAction SilentlyContinue).Path) }
    if ($RunningRelease) {
        throw "正在运行 build\$Configuration\notion_clipboard_win.exe，请先从托盘退出后再构建安装包。"
    }

    & $CMakePath -S . -B build -DNOTION_CLIPBOARD_WIN_GUI=ON
    & $CMakePath --build build --config $Configuration

    if (-not $IsccPath) {
        Write-Host "未找到 Inno Setup 编译器 ISCC.exe。"
        Write-Host "可使用以下命令安装：winget install JRSoftware.InnoSetup"
        throw "缺少 Inno Setup，无法生成安装包。"
    }

    New-Item -ItemType Directory -Force -Path ".\dist" | Out-Null
    & $IsccPath "/DAppVersion=$AppVersion" ".\installer\notion-clipboard-win.iss"

    $Installer = ".\dist\NotionClipboardWin-$AppVersion-Setup.exe"
    if (-not (Test-Path $Installer)) {
        throw "安装包未生成：$Installer"
    }

    $Hash = Get-FileHash -Algorithm SHA256 $Installer
    "$($Hash.Hash)  $(Split-Path $Installer -Leaf)" | Set-Content -Encoding ASCII "$Installer.sha256"
    Write-Host "已生成安装包：$Installer"
    Write-Host "SHA256：$($Hash.Hash)"
}
finally {
    Pop-Location
}
