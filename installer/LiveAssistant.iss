; 灵犀 安装程序脚本
; 使用 Inno Setup Compiler 6.x 编译

#define MyAppName "灵犀.视频云"
#include "version.iss"
#define MyAppPublisher "linxiqidian"
#define MyAppURL "https://www.lxi-tech.com"
#define MyAppExeName "灵犀.视频云.exe"

[Setup]
; 基本设置
AppId={{YOUR-GUID-HERE-1234-567890ABCDEF}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={userappdata}\{#MyAppName}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
UsePreviousAppDir=yes
UsePreviousGroup=yes
UsePreviousTasks=yes

; 输出设置
OutputDir=..\installer\output
OutputBaseFilename=灵犀.视频云-{#MyAppVersion}-安装包
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

; 图标和外观
SetupIconFile=..\resources\logo_new.ico
UninstallDisplayIcon={app}\{#MyAppExeName}

; 权限要求
PrivilegesRequired=admin
ArchitecturesInstallIn64BitMode=x64compatible
CloseApplications=force
RestartApplications=no
SetupLogging=yes

; 版本信息
VersionInfoVersion={#MyAppVersion}
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription={#MyAppName} 直播助手
VersionInfoTextVersion={#MyAppVersion}
VersionInfoCopyright=Copyright (C) 2026

; 其他选项
DisableWelcomePage=no
DisableDirPage=no
DisableProgramGroupPage=no

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; 打包前请先完整编译一次 Release（CMake 会对 LiveAssistant POST_BUILD 执行 windeployqt，
; 将 Qt/QML/Quick/WebEngine 插件与依赖拷到输出目录）。此处整目录拷贝，避免手工列举 Qt DLL 遗漏导致「仅标题栏」。
#define BuildReleaseDir "..\build_vs2019\Release"
Source: "{#BuildReleaseDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: "*.pdb,*.ilk,*.exp,*.lib,*.obj"

; XMagic 资源
Source: "..\third_party\xmagic\res\*"; DestDir: "{app}\xmagic\res"; Flags: ignoreversion recursesubdirs

; 资源文件
Source: "..\resources\*"; DestDir: "{app}\resources"; Flags: ignoreversion recursesubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}\logs"
Type: filesandordirs; Name: "{app}\cache"
Type: filesandordirs; Name: "{app}\temp"
