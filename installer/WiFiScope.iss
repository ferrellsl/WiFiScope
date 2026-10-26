; WiFiScope installer (Inno Setup 6): a per-user install - no administrator
; rights - with a Start menu entry, an optional desktop icon and an uninstaller.
;   "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer\WiFiScope.iss
; (after building build\Release\WiFiScope.exe); the setup lands in installer\output.

#define AppVersion "1.1"

[Setup]
AppId={{6B0E3C1D-5A4F-4F2B-9E7A-1D2C3B4A5F60}
AppName=WiFiScope
AppVersion={#AppVersion}
AppVerName=WiFiScope {#AppVersion}
AppPublisher=Stephen Ferrell
AppPublisherURL=https://github.com/ferrellsl/WiFiScope
AppSupportURL=https://github.com/ferrellsl/WiFiScope/issues
DefaultDirName={localappdata}\Programs\WiFiScope
DefaultGroupName=WiFiScope
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
MinVersion=10.0
OutputDir=output
OutputBaseFilename=WiFiScope-{#AppVersion}-setup
SetupIconFile=..\res\WiFiScope.ico
UninstallDisplayIcon={app}\WiFiScope.exe
LicenseFile=..\LICENSE
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "..\build\Release\WiFiScope.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\WiFiScope"; Filename: "{app}\WiFiScope.exe"
Name: "{autodesktop}\WiFiScope"; Filename: "{app}\WiFiScope.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\WiFiScope.exe"; Description: "{cm:LaunchProgram,WiFiScope}"; Flags: nowait postinstall skipifsilent
