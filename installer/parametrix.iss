; One-click Windows installer for Parametrix (Inno Setup 6 or newer).
;
; Build the plugin first, then:
;   cmake --install build_x64 --prefix release --config RelWithDebInfo
;   iscc /DAppVersion=0.1.0 /DSourceDir=release\obs-parametrix /DOutputDir=release installer\parametrix.iss
;
; It installs into OBS's per-machine plugin folder, which ordinary users can write to,
; so there is no administrator prompt and nothing to choose.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\release\obs-parametrix"
#endif
#ifndef OutputDir
  #define OutputDir "..\release"
#endif

[Setup]
; Never change AppId: it is how an update finds the previous install.
AppId={{6F0C1F0B-3B7B-4C5E-9A52-7B1D7E0A9C41}
AppName=Parametrix for OBS Studio
AppVersion={#AppVersion}
AppPublisher=Parametrix contributors
AppPublisherURL=https://github.com/meyyy-git/obs-parametrix
AppSupportURL=https://github.com/meyyy-git/obs-parametrix/issues
DefaultDirName={commonappdata}\obs-studio\plugins\obs-parametrix
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableWelcomePage=yes
DisableReadyPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; If OBS is open it holds the plugin file; Setup offers to close it first.
CloseApplications=yes
RestartApplications=no
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
OutputDir={#OutputDir}
OutputBaseFilename=obs-parametrix-{#AppVersion}-windows-x64-Installer
UninstallDisplayName=Parametrix for OBS Studio
VersionInfoVersion={#AppVersion}

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
#if FileExists(CompilerPath + "Languages\Indonesian.isl")
Name: "id"; MessagesFile: "compiler:Languages\Indonesian.isl"
#endif

[InstallDelete]
; Factory presets are replaced as a set, so renamed or removed ones don't linger.
; Presets you saved yourself live in OBS's config folder and are not touched.
Type: filesandordirs; Name: "{app}\data\presets"
Type: filesandordirs; Name: "{app}\data\module-presets"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Excludes: "*.pdb"; Flags: recursesubdirs ignoreversion

[Messages]
en.FinishedHeadingLabel=Parametrix is installed
en.FinishedLabelNoIcons=Start OBS Studio, open Filters on an audio source, click + under Audio Filters and choose Parametrix.
en.FinishedLabel=Start OBS Studio, open Filters on an audio source, click + under Audio Filters and choose Parametrix.
#if FileExists(CompilerPath + "Languages\Indonesian.isl")
id.FinishedHeadingLabel=Parametrix sudah terpasang
id.FinishedLabelNoIcons=Buka OBS Studio, buka Filters pada source audio, klik + di Audio Filters, lalu pilih Parametrix.
id.FinishedLabel=Buka OBS Studio, buka Filters pada source audio, klik + di Audio Filters, lalu pilih Parametrix.
#endif
