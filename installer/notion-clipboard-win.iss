#define AppName "Notion Clipboard Win"
#define AppVersion Trim(FileRead("..\VERSION"))
#define AppPublisher "lilong"
#define AppURL "https://github.com/lilong555/notion_clipboard_win"

[Setup]
AppId={{9F2C139E-4BC9-43A6-93A7-B63A3564A608}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
AppUpdatesURL={#AppURL}/releases
DefaultDirName={localappdata}\Programs\Notion Clipboard Win
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
OutputDir=..\dist
OutputBaseFilename=NotionClipboardWin-{#AppVersion}-Setup
SetupIconFile=..\assets\app.ico
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
UninstallDisplayIcon={app}\notion_clipboard_win.exe
CloseApplications=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "chinesesimp"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Files]
Source: "..\build\Release\notion_clipboard_win.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\config.example.ini"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.en.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\notion_clipboard_win.exe"
Name: "{autoprograms}\{#AppName} README"; Filename: "{app}\README.md"

[Run]
Filename: "{app}\notion_clipboard_win.exe"; Description: "Launch {#AppName}"; Flags: nowait postinstall skipifsilent
