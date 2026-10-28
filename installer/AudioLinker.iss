; AudioLinker installer (Inno Setup 6.1+). Build it with installer\build.ps1, which
; reads the version from CMakeLists.txt and passes it here:
;   ISCC /DAppVersion=1.2.0 /DExePath=..\build\AudioLinker.exe AudioLinker.iss
;
; Per-user by default (no administrator rights: the app needs none), with the
; usual "install for all users" choice offered by the wizard.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef ExePath
  #define ExePath "..\build\AudioLinker.exe"
#endif

#define AppName "AudioLinker"
#define AppExe "AudioLinker.exe"
#define AppUrl "https://github.com/Crazyshouby/AudioLinker"

[Setup]
; Never change this GUID: it is how an update finds the version it replaces.
AppId={{F021ACAC-2893-4F12-A3B9-BAE1B1D7AB4A}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Valentin Large
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}/issues
AppUpdatesURL={#AppUrl}/releases
VersionInfoVersion={#AppVersion}.0
VersionInfoProductName={#AppName}
VersionInfoProductVersion={#AppVersion}
VersionInfoDescription=Installation de {#AppName}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 10 2004 (build 19041): process-loopback capture and the WebView2 runtime need it.
MinVersion=10.0.19041
LicenseFile=..\LICENSE
SetupIconFile=..\assets\AudioLinker.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
WizardStyle=modern
Compression=lzma2/ultra64
SolidCompression=yes
OutputDir=Output
OutputBaseFilename={#AppName}-Setup-{#AppVersion}
; The running app is stopped by --quit (see [Code]), not by the Restart Manager:
; closing its window only hides it in the tray, and killing it would leave the
; audio default on the virtual cable.
CloseApplications=no
RestartApplications=no
#ifdef SignTool
SignTool=signtool
SignedUninstaller=yes
#endif

[Languages]
Name: "french"; MessagesFile: "compiler:Languages\French.isl"

[Tasks]
Name: "startup"; Description: "Lancer {#AppName} au démarrage de Windows (dans la zone de notification)"; Flags: unchecked
Name: "desktopicon"; Description: "Créer un raccourci sur le &Bureau"; Flags: unchecked

[Files]
Source: "{#ExePath}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "..\THIRD-PARTY-NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
; Same entry the app writes itself (Options > Lancer avec Windows).
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "{#AppName}"; ValueData: """{app}\{#AppExe}"" --tray"; Tasks: startup; Flags: uninsdeletevalue

[Run]
Filename: "{app}\{#AppExe}"; Description: "Lancer {#AppName}"; Flags: nowait postinstall skipifsilent
Filename: "https://vb-audio.com/Cable/"; Description: "Ouvrir la page de téléchargement de VB-CABLE"; Flags: shellexec nowait postinstall skipifsilent unchecked

[Code]
const
  WebView2ClientKey = 'Software\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}';
  WebView2Bootstrapper = 'https://go.microsoft.com/fwlink/p/?LinkId=2124703';
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';

var
  PrereqPage: TOutputMsgWizardPage;
  DownloadPage: TDownloadWizardPage;
  NeedWebView2: Boolean;

function HasWebView2Version(RootKey: Integer; const SubKey: String): Boolean;
var
  Version: String;
begin
  Result := RegQueryStringValue(RootKey, SubKey, 'pv', Version) and (Version <> '') and (Version <> '0.0.0.0');
end;

// The Evergreen runtime registers itself per machine (32-bit view) or per user.
function WebView2Installed: Boolean;
begin
  Result := HasWebView2Version(HKLM32, WebView2ClientKey) or HasWebView2Version(HKLM64, WebView2ClientKey) or
            HasWebView2Version(HKCU, WebView2ClientKey);
end;

function OnDownloadProgress(const Url, FileName: String; const Progress, ProgressMax: Int64): Boolean;
begin
  if Progress = ProgressMax then Log(Format('Téléchargé %s', [FileName]));
  Result := True;
end;

procedure InitializeWizard;
begin
  NeedWebView2 := not WebView2Installed;
  PrereqPage := CreateOutputMsgPage(wpWelcome, 'Avant de commencer', 'Ce dont AudioLinker a besoin',
    'AudioLinker diffuse le son de Windows vers plusieurs sorties à la fois. Il faut :' + #13#10 + #13#10 +
    '  •  Windows 10 (version 2004) ou Windows 11, en 64 bits.' + #13#10 + #13#10 +
    '  •  Le runtime Microsoft Edge WebView2 (affichage de l''interface). ' +
    'Il est déjà présent sur Windows 11 ; sinon cet installateur le télécharge et l''installe.' + #13#10 + #13#10 +
    '  •  Un câble audio virtuel, VB-CABLE (gratuit, de VB-Audio), qui sert de point de capture du son du système. ' +
    'Il s''installe séparément : https://vb-audio.com/Cable/' + #13#10 + #13#10 +
    'Sans VB-CABLE, le mode « Sans câble » (expérimental) des Options permet d''essayer le programme.');
  DownloadPage := CreateDownloadPage(SetupMessage(msgWizardPreparing), SetupMessage(msgPreparingDesc),
    @OnDownloadProgress);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = wpReady) and NeedWebView2 then begin
    DownloadPage.Clear;
    DownloadPage.Add(WebView2Bootstrapper, 'MicrosoftEdgeWebview2Setup.exe', '');
    DownloadPage.Show;
    try
      try
        DownloadPage.Download;
      except
        if DownloadPage.AbortedByUser then
          Result := False
        else begin
          // Not fatal: AudioLinker is installed anyway and tells the user what is missing.
          SuppressibleMsgBox('Le runtime WebView2 n''a pas pu être téléchargé (' + GetExceptionMessage + ').' + #13#10 +
            'AudioLinker sera installé, mais il ne s''affichera qu''après l''installation de WebView2 : ' +
            'https://developer.microsoft.com/microsoft-edge/webview2/', mbError, MB_OK, IDOK);
          NeedWebView2 := False;
        end;
      end;
    finally
      DownloadPage.Hide;
    end;
  end;
end;

// Asks a running instance to quit the way the tray's Quitter does (group
// stopped, Windows' default output restored), and waits for it.
procedure StopRunningApp(const AppDir: String);
var
  Exe: String;
  ResultCode: Integer;
begin
  Exe := AppDir + '\{#AppExe}';
  if FileExists(Exe) then
    Exec(Exe, '--quit', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  StopRunningApp(ExpandConstant('{app}'));
  Result := '';
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Bootstrapper: String;
  ResultCode: Integer;
begin
  if (CurStep = ssPostInstall) and NeedWebView2 then begin
    Bootstrapper := ExpandConstant('{tmp}\MicrosoftEdgeWebview2Setup.exe');
    if FileExists(Bootstrapper) then begin
      WizardForm.StatusLabel.Caption := 'Installation de WebView2...';
      // The runtime installs per machine: ask for the elevation explicitly.
      if not ShellExec('runas', Bootstrapper, '/silent /install', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
        Log(Format('WebView2 : échec du lancement (%d)', [ResultCode]));
    end;
  end;
end;

function InitializeUninstall: Boolean;
begin
  StopRunningApp(ExpandConstant('{app}'));
  Result := True;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then begin
    // Whatever turned it on (this installer or the app's own option).
    RegDeleteValue(HKCU, RunKey, '{#AppName}');
    if not UninstallSilent then begin
      if MsgBox('Supprimer aussi vos réglages (setups, enceintes, raccourcis) et le journal ?' + #13#10 + #13#10 +
                'Choisissez Non pour les garder si vous réinstallez AudioLinker plus tard.',
                mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then begin
        DelTree(ExpandConstant('{userappdata}\{#AppName}'), True, True, True);
        DelTree(ExpandConstant('{localappdata}\{#AppName}'), True, True, True);
      end;
    end;
  end;
end;
