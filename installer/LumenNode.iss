[Setup]
AppName=Lumen Node Helper
AppVersion=1.0.0
AppPublisher=Lumen
AppPublisherURL=https://your-official-site.com
DefaultDirName={autopf}\Lumen Node Helper
DefaultGroupName=Lumen Node Helper
OutputBaseFilename=LumenNode-Setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
UninstallDisplayIcon={app}\LumenNodeCompanion.exe
OutputDir=..\LumenNodeOBS\build

[Files]
; Helper EXE
Source: "..\LumenNodeOBS\build\LumenNodeCompanion.exe"; DestDir: "{app}"; Flags: ignoreversion
; OBS 插件 DLL —— 和 EXE 放在同一目录
Source: "..\LumenNodeOBS\build\lumen-node-obs.dll"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Lumen Node Helper"; Filename: "{app}\LumenNodeCompanion.exe"
Name: "{autodesktop}\Lumen Node Helper"; Filename: "{app}\LumenNodeCompanion.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional icons:"

[Run]
Filename: "{app}\LumenNodeCompanion.exe"; Description: "Launch Lumen Node Helper"; Flags: nowait postinstall skipifsilent