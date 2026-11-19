; Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE)
; ONE Windows installer for every Windows from XP to 11 (since 1.4.1).
;   64-bit Windows 7 and newer -> the 64-bit Q-LiM (build-win/dist, Makefile.win)
;   XP, Vista, every 32-bit Windows -> the 32-bit Q-LiM (build-win32/Q-LiM-<v>-win32, Makefile.win32)
; Needs Inno Setup 5.6.1 (the last one whose installers run on XP; Inno Setup 6 needs Vista).
; Built by:  make -f Makefile.win32 installer   (ISCC.exe of Inno Setup 5.6.1 under Wine)

#ifndef AppVersion
  #define AppVersion "1.4.1"
#endif
#ifndef Dist64
  #define Dist64 "..\build-win\dist"
#endif
#ifndef Dist32
  #define Dist32 "..\build-win32\Q-LiM-" + AppVersion + "-win32"
#endif

[Setup]
AppId={{6E2B7C1A-4F2D-4C9B-9A51-0D5E8B3F7A21}
AppName=Q-LiM
AppVersion={#AppVersion}
AppVerName=Q-LiM {#AppVersion}
AppPublisher=Georgi Stanimirov Ivanov
AppPublisherURL=https://github.com/GeorgeArtAcc/Q-LiM
AppCopyright=Copyright (c) 2026 Georgi Stanimirov Ivanov
DefaultDirName={pf}\Q-LiM
DefaultGroupName=Q-LiM
DisableProgramGroupPage=yes
LicenseFile={#Dist32}\LICENSE.txt
OutputDir=..\build-win32
OutputBaseFilename=Q-LiM-Setup-{#AppVersion}
SetupIconFile=..\assets\icons\qlim.ico
UninstallDisplayIcon={app}\qlim.exe
Compression=lzma2/max
SolidCompression=yes
; 64-bit Windows: the 64-bit program folder (and the 64-bit Q-LiM from Windows 7 on)
ArchitecturesInstallIn64BitMode=x64
MinVersion=5.1
ChangesAssociations=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "assoc"; Description: "Open .limproject files with Q-LiM"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
; 64-bit (Windows 7+ x64): qlim.exe + its DLLs, ASIO, WASAPI ...
Source: "{#Dist64}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Check: Use64
; 32-bit (XP and newer, any Windows): one qlim.exe, everything built in
Source: "{#Dist32}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Check: not Use64

[InstallDelete]
; switching between the two variants (e.g. after an update of Windows): no stale DLLs of the other one
Type: files; Name: "{app}\SDL2.dll"; Check: not Use64
Type: files; Name: "{app}\SDL2_ttf.dll"; Check: not Use64
; 1.4.7: no sndfile.dll in either variant any more (libsndfile is built in)
Type: files; Name: "{app}\sndfile.dll"
Type: files; Name: "{app}\libc++.dll"; Check: not Use64
Type: files; Name: "{app}\libunwind.dll"; Check: not Use64
Type: files; Name: "{app}\libwinpthread-1.dll"; Check: not Use64

[Icons]
Name: "{group}\Q-LiM"; Filename: "{app}\qlim.exe"
Name: "{group}\{cm:UninstallProgram,Q-LiM}"; Filename: "{uninstallexe}"
Name: "{commondesktop}\Q-LiM"; Filename: "{app}\qlim.exe"; Tasks: desktopicon

[Registry]
Root: HKCR; Subkey: ".limproject"; ValueType: string; ValueName: ""; ValueData: "QLiM.Project"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKCR; Subkey: "QLiM.Project"; ValueType: string; ValueName: ""; ValueData: "Q-LiM project"; Flags: uninsdeletekey; Tasks: assoc
Root: HKCR; Subkey: "QLiM.Project\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\qlim.exe,1"; Tasks: assoc
Root: HKCR; Subkey: "QLiM.Project\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\qlim.exe"" ""%1"""; Tasks: assoc

[Run]
Filename: "{app}\qlim.exe"; Description: "{cm:LaunchProgram,Q-LiM}"; Flags: nowait postinstall skipifsilent

[Code]
{ the 64-bit Q-LiM needs 64-bit Windows 7 or newer (6.1) }
function Use64: Boolean;
var
  V: TWindowsVersion;
begin
  GetWindowsVersionEx(V);
  Result := Is64BitInstallMode and ((V.Major > 6) or ((V.Major = 6) and (V.Minor >= 1)));
end;
