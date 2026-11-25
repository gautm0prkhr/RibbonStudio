#define MyAppName "Ribbon 2.0"
#define MyAppVersion "2.0.0"
#define MyAppPublisher "Ribbon Audio"
#define MyAppExeName "Ribbon.exe"

[Setup]
AppId={{8E9B7D21-6C4A-4A9F-B92D-1F8E5C3A2026}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\Ribbon Studio
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
OutputDir=dist
OutputBaseFilename=RibbonSetup
SetupIconFile=ribbon.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
ChangesAssociations=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "fileassoc"; Description: "Associate .rib project files with Ribbon 2.0"; GroupDescription: "File Associations:"

[Files]
Source: "dist\Ribbon.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "ribbon.ico"; DestDir: "{app}"; Flags: ignoreversion
Source: "README.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\ribbon.ico"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\ribbon.ico"; Tasks: desktopicon

[Registry]
Root: HKA; Subkey: "Software\Classes\.rib"; ValueType: string; ValueName: ""; ValueData: "RibbonProjectFile"; Flags: uninsdeletevalue; Tasks: fileassoc
Root: HKA; Subkey: "Software\Classes\RibbonProjectFile"; ValueType: string; ValueName: ""; ValueData: "Ribbon 2.0 Project"; Flags: uninsdeletekey; Tasks: fileassoc
Root: HKA; Subkey: "Software\Classes\RibbonProjectFile\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\ribbon.ico,0"; Tasks: fileassoc
Root: HKA; Subkey: "Software\Classes\RibbonProjectFile\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Tasks: fileassoc

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
