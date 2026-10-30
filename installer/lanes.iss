; SPDX-License-Identifier: GPL-3.0-or-later
;
; Lanes Download Manager installer (Inno Setup 6). Built by
; tools\make-installer.ps1, which stages the program with `cmake --install` and passes:
;   /DAppVersion=0.1.0  /DStageDir=<staged program>  /DOutputDir=<where setup goes>
;   /DSignTool=<name>   optional: sign setup and uninstaller with that ISCC /S tool (CI)
;
; Per-user install by default (no administrator rights, %LOCALAPPDATA%\Programs\Lanes); the
; first page offers "for all users" too. Inno Setup, not MSIX: MSIX virtualizes HKCU, which breaks
; the browser's native-messaging registration.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef StageDir
  #error StageDir is not defined; build with tools\make-installer.ps1
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

[Setup]
AppId={{7C1E5B2A-4D8F-4B6E-9A3C-2F8E1D6B5A90}
AppName=Lanes Download Manager
AppVersion={#AppVersion}
AppVerName=Lanes Download Manager {#AppVersion}
AppPublisher=Lanes contributors
AppPublisherURL=https://github.com/shariful583/dm
AppSupportURL=https://github.com/shariful583/dm/issues
DefaultDirName={autopf}\Lanes
DefaultGroupName=Lanes
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 10 21H2 or newer.
MinVersion=10.0.19044
OutputDir={#OutputDir}
OutputBaseFilename=lanes-setup-{#AppVersion}
SetupIconFile=..\src\gui\resources\lanes.ico
UninstallDisplayIcon={app}\lanes-gui.exe
UninstallDisplayName=Lanes Download Manager
WizardStyle=modern
Compression=lzma2/ultra64
SolidCompression=yes
; Running Lanes (window or background engine) is closed before files are replaced.
CloseApplications=force
RestartApplications=no
ChangesEnvironment=no
LicenseFile=..\LICENSE
#ifdef SignTool
SignTool={#SignTool}
SignedUninstaller=yes
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "startup"; Description: "Start Lanes when I sign in to Windows (in the notification area)"; GroupDescription: "Windows:"
Name: "browser"; Description: "Let the Lanes extension for Chrome and Edge hand downloads to Lanes"; GroupDescription: "Browsers:"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\Lanes"; Filename: "{app}\lanes-gui.exe"; Comment: "Lanes Download Manager"
Name: "{autodesktop}\Lanes"; Filename: "{app}\lanes-gui.exe"; Tasks: desktopicon

[Registry]
; The same value Options → "Start Lanes when I sign in" writes (gui/app_controller.cpp).
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Lanes"; ValueData: """{app}\lanes-gui.exe"" --tray"; Tasks: startup; Flags: uninsdeletevalue

[Run]
; Native-messaging host for Chrome and Edge. Per user: HKCU, as the signed-in user. For all users:
; HKLM, from the elevated installer (the host manifest goes next to the program, which only an
; administrator may write in Program Files).
Filename: "{app}\lanes-bridge.exe"; Parameters: "--register"; Tasks: browser; Check: not IsAdminInstallMode; Flags: runhidden runasoriginaluser; StatusMsg: "Connecting Chrome and Edge..."
Filename: "{app}\lanes-bridge.exe"; Parameters: "--register --machine"; Tasks: browser; Check: IsAdminInstallMode; Flags: runhidden; StatusMsg: "Connecting Chrome and Edge..."
Filename: "{app}\lanes-gui.exe"; Description: "Start Lanes"; Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallRun]
Filename: "{app}\lanes-bridge.exe"; Parameters: "--unregister"; Check: not IsAdminInstallMode; Flags: runhidden; RunOnceId: "UnregisterBridge"
Filename: "{app}\lanes-bridge.exe"; Parameters: "--unregister --machine"; Check: IsAdminInstallMode; Flags: runhidden; RunOnceId: "UnregisterBridgeMachine"
Filename: "{app}\lanes.exe"; Parameters: "engine --stop"; Flags: runhidden; RunOnceId: "StopEngine"

[UninstallDelete]
; Written by lanes-bridge --register, next to the program.
Type: files; Name: "{app}\com.lanes.bridge.json"

[Code]
// The windowless background engine (`lanes engine`) is not a window Restart Manager closes:
// ask it to stop before files are replaced.
procedure StopEngine();
var
  Code: Integer;
begin
  if FileExists(ExpandConstant('{app}\lanes.exe')) then
    Exec(ExpandConstant('{app}\lanes.exe'), 'engine --stop', '', SW_HIDE, ewWaitUntilTerminated, Code);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  StopEngine();
  Result := '';
end;

// Downloads stay where they are; Lanes' own list, settings and updated yt-dlp
// (%LOCALAPPDATA%\Lanes) go only if the user says so.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Data: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'Lanes');
    Data := ExpandConstant('{localappdata}\Lanes');
    if DirExists(Data) and not UninstallSilent then
      if MsgBox('Also remove the download list, settings and queues of Lanes?' + #13#10 +
                '(Downloaded files are not touched.)', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
        DelTree(Data, True, True, True);
  end;
end;
