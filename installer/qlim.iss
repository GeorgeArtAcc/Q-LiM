; Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE)
; Inno Setup script for the Windows installer (Windows 7 SP1 and newer, 64-bit).
; Built by:  make -f Makefile.win installer   (runs ISCC.exe under Wine on Linux)

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef DistDir
  #define DistDir "..\build-win\dist"
#endif

[Setup]
AppId={{6E2B7C1A-4F2D-4C9B-9A51-0D5E8B3F7A21}
AppName=Q-LiM
AppVersion={#AppVersion}
AppVerName=Q-LiM {#AppVersion}
AppPublisher=Georgi Stanimirov Ivanov
AppPublisherURL=https://github.com/GeorgeArtAcc/Q-LiM
AppCopyright=Copyright (c) 2026 Georgi Stanimirov Ivanov
DefaultDirName={autopf}\Q-LiM
DefaultGroupName=Q-LiM
DisableProgramGroupPage=yes
LicenseFile={#DistDir}\LICENSE.txt
OutputDir=..\build-win
OutputBaseFilename=Q-LiM-Setup-{#AppVersion}
SetupIconFile=..\assets\icons\qlim.ico
UninstallDisplayIcon={app}\qlim.exe
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=6.1sp1
ChangesAssociations=yes
PrivilegesRequiredOverridesAllowed=dialog

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "bulgarian"; MessagesFile: "compiler:Languages\Bulgarian.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "assoc"; Description: "Open .limproject files with Q-LiM"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#DistDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Q-LiM"; Filename: "{app}\qlim.exe"
Name: "{group}\{cm:UninstallProgram,Q-LiM}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Q-LiM"; Filename: "{app}\qlim.exe"; Tasks: desktopicon

[Registry]
Root: HKA; Subkey: "Software\Classes\.limproject"; ValueType: string; ValueName: ""; ValueData: "QLiM.Project"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\QLiM.Project"; ValueType: string; ValueName: ""; ValueData: "Q-LiM project"; Flags: uninsdeletekey; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\QLiM.Project\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\qlim.exe,1"; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\QLiM.Project\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\qlim.exe"" ""%1"""; Tasks: assoc

[Run]
Filename: "{app}\qlim.exe"; Description: "{cm:LaunchProgram,Q-LiM}"; Flags: nowait postinstall skipifsilent
