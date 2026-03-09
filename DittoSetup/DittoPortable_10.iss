#define MyAppName "Ditto Portable"
#define MyAppVersion GetFileVersion("..\Release64\Ditto.exe")
#define MyAppVerName MyAppName + " " + MyAppVersion
#define MyAppPublisher "Scott Brogden"
#define MyAppSupportURL "ditto-cp.sourceforge.net"
#define MyAppCopyrighEndYear GetDateTimeString('yyyy','','')

[Setup]
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppVerName}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppSupportURL}
AppSupportURL={#MyAppSupportURL}
AppUpdatesURL={#MyAppSupportURL}
WizardStyle=modern
VersionInfoDescription={#MyAppName} extractor
VersionInfoVersion={#MyAppVersion}
VersionInfoProductName={#MyAppName}
VersionInfoProductVersion={#MyAppVersion}
AppCopyright={#MyAppPublisher} {#MyAppCopyrighEndYear}
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
CreateUninstallRegKey=no
Uninstallable=no
UsePreviousAppDir=no
UsePreviousTasks=no
DisableProgramGroupPage=yes
DisableReadyPage=yes
DirExistsWarning=no
DefaultDirName={userdocs}\DittoPortable
OutputDir=output
OutputBaseFilename=DittoPortable
Compression=lzma2
SolidCompression=yes
SetupLogging=yes

[Languages]
Name: English; MessagesFile: compiler:Default.isl
Name: Czech; MessagesFile: compiler:Languages\Czech.isl
Name: Danish; MessagesFile: compiler:Languages\Danish.isl
Name: Dutch; MessagesFile: compiler:Languages\Dutch.isl
Name: Finnish; MessagesFile: compiler:Languages\Finnish.isl
Name: French; MessagesFile: compiler:Languages\French.isl
Name: Deutsch; MessagesFile: compiler:Languages\German.isl
Name: Hebrew; MessagesFile: compiler:Languages\Hebrew.isl
Name: Italiano; MessagesFile: compiler:Languages\Italian.isl
Name: Japanese; MessagesFile: compiler:Languages\Japanese.isl
Name: Polski; MessagesFile: compiler:Languages\Polish.isl
Name: Portuguese; MessagesFile: compiler:Languages\Portuguese.isl
Name: Russian; MessagesFile: compiler:Languages\Russian.isl
Name: Slovak; MessagesFile: compiler:Languages\Slovak.isl
Name: Slovenian; MessagesFile: compiler:Languages\Slovenian.isl
Name: Spanish; MessagesFile: compiler:Languages\Spanish.isl
Name: Turkish; MessagesFile: compiler:Languages\Turkish.isl
Name: Ukrainian; MessagesFile: compiler:Languages\Ukrainian.isl
Name: ChineseSimplified; MessagesFile: ChineseSimplified.isl
Name: ChineseTraditional; MessagesFile: ChineseTraditional.isl
Name: Croatian; MessagesFile: Croatian.isl
Name: Greek; MessagesFile: Greek.isl
Name: Hungarian; MessagesFile: Hungarian.isl
Name: Korean; MessagesFile: Korean.isl
Name: Romanian; MessagesFile: Romanian.isl
Name: Swedish; MessagesFile: Swedish.isl

[Files]
Source: ..\Release64\Ditto.exe; DestDir: {app}; DestName: Ditto.exe; Flags: ignoreversion
Source: ..\Release64\ICU_Loader.dll; DestDir: {app}; Flags: ignoreversion
Source: ..\Release64\Addins\*.dll; DestDir: {app}\Addins; Flags: ignoreversion
Source: C:\Windows\sysnative\vcruntime140.dll; DestDir: {app}; Flags: ignoreversion
Source: C:\Windows\sysnative\vcruntime140_1.dll; DestDir: {app}; Flags: ignoreversion
Source: C:\Windows\sysnative\msvcp140.dll; DestDir: {app}; Flags: ignoreversion
Source: C:\Windows\sysnative\mfc140u.dll; DestDir: {app}; Flags: ignoreversion
Source: ..\Changes.txt; DestDir: {app}; Flags: ignoreversion
Source: portable; DestDir: {app}; Flags: ignoreversion
Source: ..\Debug\Language\*; DestDir: {app}\Language; Flags: ignoreversion
Source: ..\Debug\Themes\*; DestDir: {app}\Themes; Flags: ignoreversion

[Run]
Filename: {app}\Ditto.exe; Description: Launch Ditto; Flags: nowait postinstall skipifsilent
