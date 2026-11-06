#define MyAppName "OBS Record Source"
#define MyAppVersion "0.2.0"
#define MyAppPublisher "Blansi93"
#define MyAppURL "https://github.com/Blansi93/obs-independent-source-record"

[Setup]
AppId={{8E3A3E1C-4C8F-4A5D-9F1B-2C7A1D9E6B42}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
DefaultDirName={commonappdata}\obs-studio\plugins\obs-record-source
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=obs-record-source-setup-0.2.0
Compression=lzma
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\obs-record-source.dll
LicenseFile=..\LICENSE

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"

[Files]
Source: "..\build\stage\obs-record-source\obs-record-source.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\stage\obs-record-source\data\*"; DestDir: "{app}\data"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Run]
Filename: "{win}\explorer.exe"; Parameters: "{app}"; Description: "Open installation folder"; Flags: postinstall shellexec skipifsilent

[Code]
function InitializeSetup(): Boolean;
begin
  Result := True;
end;
