; Build via package.ps1. The managed per-user payload contains the uninstaller
; and repair tools; the plugin itself goes into the chosen System Informer.
#ifndef PackageDir
  #error PackageDir must name the prepared release payload.
#endif
#define ProductName "WSL Tools for System Informer"
#define ProductVersion "0.1.0"

[Setup]
AppId={{1CD91F79-08A2-40C8-B7B1-7C73D98530D4}
AppName={#ProductName}
AppVersion={#ProductVersion}
AppPublisher=David Trapp / Trapp Innovations
DefaultDirName={localappdata}\Programs\WslTools
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableWelcomePage=no
PrivilegesRequired=lowest
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
MinVersion=10.0
WizardStyle=modern
LicenseFile={#PackageDir}\LICENSE
OutputBaseFilename=WslTools-{#ProductVersion}-setup-x64
Compression=lzma2
SolidCompression=yes
CloseApplications=no
RestartApplications=no
UninstallDisplayName={#ProductName}
UninstallDisplayIcon={app}\WslTools.dll
SetupLogging=yes
UsePreviousTasks=no

[Files]
Source: "{#PackageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[INI]
Filename: "{app}\installation.ini"; Section: "Installation"; Key: "SystemInformerDirectory"; String: "{code:GetTargetDirectory}"; Flags: uninsdeleteentry uninsdeletesectionifempty

[UninstallDelete]
Type: files; Name: "{app}\installation.ini"

[Code]
#include "installer-worker.iss"

var
  TargetPage: TInputDirWizardPage;
  CompanionBox: TNewCheckBox;
  PluginPage: TInputOptionWizardPage;
  PluginDetails, PluginPathLabel: TNewStaticText;
  PluginPathEdit: TNewEdit;
  PluginSettingsPath: String;
  PluginSettingsSupported, PluginSettingsNeeded, PluginSettingsInspected: Boolean;
  ExistingTarget, TargetDirectory: String;
  InstallFailed: Boolean;

function CompanionDefault: Boolean;
begin
  Result := ExpandConstant('{param:COMPANIONS|1}') <> '0';
end;

function DistributionSummary: String;
var Names: TArrayOfString; I: Integer; Flags: Cardinal; Name, Key: String;
begin
  Result := '';
  if RegGetSubkeyNames(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Lxss', Names) then
    for I := 0 to GetArrayLength(Names) - 1 do begin
      Key := 'Software\Microsoft\Windows\CurrentVersion\Lxss\' + Names[I];
      { Version is the registry schema. The VM-mode flag identifies WSL 2. }
      if RegQueryDWordValue(HKCU, Key, 'Flags', Flags) and ((Flags and 8) <> 0) and
        RegQueryStringValue(HKCU, Key, 'DistributionName', Name) then begin
        if Result <> '' then Result := Result + ', ';
        Result := Result + Name;
      end;
    end;
  if Result = '' then Result := 'No WSL 2 distributions are registered for this Windows user.'
  else Result := 'WSL 2 distributions: ' + Result;
end;

function DetectSystemInformer: String;
var Candidate, Key: String;
begin
  Result := ExpandConstant('{pf64}\SystemInformer');
  Key := 'Software\Microsoft\Windows\CurrentVersion\Uninstall\SystemInformer';
  if RegQueryStringValue(HKLM64, Key, 'InstallLocation', Candidate) and
    FileExists(AddBackslash(Candidate) + 'SystemInformer.exe') then Result := Candidate
  else if RegQueryStringValue(HKCU, Key, 'InstallLocation', Candidate) and
    FileExists(AddBackslash(Candidate) + 'SystemInformer.exe') then Result := Candidate;
end;

function GetTargetDirectory(Param: String): String;
begin
  Result := TargetDirectory;
end;

procedure InitializeWizard;
var Notice: TNewStaticText; OverridePath: String;
begin
  ExistingTarget := GetPreviousData('SystemInformerDirectory', '');
  TargetDirectory := ExistingTarget;
  if TargetDirectory = '' then TargetDirectory := DetectSystemInformer;
  OverridePath := ExpandConstant('{param:SYSTEMINFORMERDIR|}');
  if OverridePath <> '' then TargetDirectory := OverridePath;
  TargetPage := CreateInputDirPage(wpLicense, 'System Informer installation',
    'Choose the installation to add WSL Tools to.',
    'Select the folder containing SystemInformer.exe. Close that instance before installing.', False, '');
  TargetPage.Add('System Informer folder:');
  TargetPage.Values[0] := TargetDirectory;
  CompanionBox := TNewCheckBox.Create(TargetPage);
  CompanionBox.Parent := TargetPage.Surface;
  CompanionBox.SetBounds(0, ScaleY(108), TargetPage.SurfaceWidth, ScaleY(24));
  CompanionBox.Caption := 'Install companion in all WSL2 distributions';
  CompanionBox.Checked := CompanionDefault;
  Notice := TNewStaticText.Create(TargetPage);
  Notice.Parent := TargetPage.Surface;
  Notice.SetBounds(ScaleX(18), ScaleY(140), TargetPage.SurfaceWidth - ScaleX(18), ScaleY(125));
  Notice.AutoSize := False;
  Notice.WordWrap := True;
  Notice.Caption := 'Includes stopped distributions, which will temporarily be started. ' +
    'Runs as Linux root.' + #13#10#13#10 + DistributionSummary + #13#10#13#10 +
    'Your other System Informer settings will be preserved.';
  PluginPage := CreateInputOptionPage(TargetPage.ID, 'System Informer plugin loading',
    'Allow System Informer to load WSL Tools.',
    'Required for WSL Tools. This enables plugin loading and allows third-party plugin DLLs.',
    False, False);
  PluginPage.Add('Enable third-party plugins in System Informer');
  PluginDetails := TNewStaticText.Create(PluginPage);
  PluginDetails.Parent := PluginPage.Surface;
  PluginDetails.SetBounds(0, ScaleY(75), PluginPage.SurfaceWidth, ScaleY(85));
  PluginDetails.AutoSize := False;
  PluginDetails.WordWrap := True;
  PluginDetails.ShowAccelChar := False;
  PluginPathLabel := TNewStaticText.Create(PluginPage);
  PluginPathLabel.Parent := PluginPage.Surface;
  PluginPathLabel.SetBounds(0, ScaleY(165), PluginPage.SurfaceWidth, ScaleY(18));
  PluginPathLabel.Caption := 'Settings file:';
  PluginPathEdit := TNewEdit.Create(PluginPage);
  PluginPathEdit.Parent := PluginPage.Surface;
  PluginPathEdit.SetBounds(0, ScaleY(185), PluginPage.SurfaceWidth, ScaleY(23));
  PluginPathEdit.ReadOnly := True;
end;

function ValidateTarget: String;
begin
  Result := '';
  TargetDirectory := RemoveBackslashUnlessRoot(Trim(TargetPage.Values[0]));
  if not FileExists(AddBackslash(TargetDirectory) + 'SystemInformer.exe') then
    Result := 'Choose the folder containing SystemInformer.exe.'
  else if (ExistingTarget <> '') and
    (CompareText(RemoveBackslashUnlessRoot(ExistingTarget), TargetDirectory) <> 0) then
    Result := 'WSL Tools is registered for ' + ExistingTarget + '.' + #13#10 +
      'Uninstall it before selecting a different System Informer installation.';
end;

procedure InspectPluginSettings;
var Report, Params, Diagnostic: String; ExitCode: Integer;
begin
  ExtractTemporaryFile('setup-settings.ps1');
  Report := ExpandConstant('{tmp}\plugin-settings.ini');
  DeleteFile(Report);
  Params := '-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File ' +
    NativeQuote(ExpandConstant('{tmp}\setup-settings.ps1')) +
    ' -Mode Inspect -SystemInformerDirectory ' + NativeQuote(TargetDirectory) +
    ' -ResultFile ' + NativeQuote(Report);
  PluginSettingsSupported := False;
  PluginSettingsNeeded := False;
  Diagnostic := 'Could not inspect System Informer settings. You can enable third-party plugins manually after installation.';
  if Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'), Params, '',
    SW_HIDE, ewWaitUntilTerminated, ExitCode) then begin
    if (ExitCode = 0) and FileExists(Report) then begin
      PluginSettingsSupported := GetIniString('Plugins', 'Supported', '0', Report) = '1';
      PluginSettingsNeeded := GetIniString('Plugins', 'NeedsChange', '0', Report) = '1';
    end;
    Diagnostic := GetIniString('Plugins', 'Message', Diagnostic, Report);
  end;
  if PluginSettingsSupported and PluginSettingsNeeded then
    Diagnostic := 'Your other settings and disabled-plugin choices will be preserved.';
  PluginSettingsPath := GetIniString('Plugins', 'Path', '', Report);
  PluginPage.CheckListBox.Enabled := PluginSettingsSupported;
  { Interactive installs opt in by default. Silent installs must explicitly
    request a settings change because there is no checkbox to review. }
  PluginPage.Values[0] := PluginSettingsSupported and PluginSettingsNeeded and
    (ExpandConstant('{param:ENABLETHIRDPARTYPLUGINS|}') <> '0') and
    (not WizardSilent or (ExpandConstant('{param:ENABLETHIRDPARTYPLUGINS|}') = '1'));
  PluginDetails.Caption := Diagnostic;
  PluginPathEdit.Text := PluginSettingsPath;
  PluginPathEdit.Visible := PluginSettingsPath <> '';
  PluginPathLabel.Visible := PluginPathEdit.Visible;
  PluginSettingsInspected := True;
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := (PageID = PluginPage.ID) and PluginSettingsInspected and
    PluginSettingsSupported and not PluginSettingsNeeded;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var Problem: String;
begin
  Result := True;
  if CurPageID = TargetPage.ID then begin
    Problem := ValidateTarget;
    Result := Problem = '';
    if not Result then SuppressibleMsgBox(Problem, mbError, MB_OK, IDOK)
    else InspectPluginSettings;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  { Also validate when silent installation skips the directory page. }
  Result := ValidateTarget;
  if (Result = '') and not PluginSettingsInspected then InspectPluginSettings;
end;

procedure RegisterPreviousData(PreviousDataKey: Integer);
begin
  SetPreviousData(PreviousDataKey, 'SystemInformerDirectory', TargetDirectory);
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := 'System Informer folder:' + NewLine + Space + TargetDirectory + NewLine + NewLine;
  if CompanionBox.Checked then Result := Result + 'Install companion in all WSL2 distributions.'
  else Result := Result + 'Install the Windows plugin only.';
  if PluginPage.Values[0] then
    Result := Result + NewLine + NewLine + 'Enable third-party plugins in System Informer.';
end;

procedure ReportWorkerFailure(Action: String);
begin
  SuppressibleMsgBox(Action + ' did not finish.' + #13#10#13#10 + WorkDiagnostics +
    '' + #13#10#13#10 + 'Completed changes have been kept. Run setup or uninstall again to retry.',
    mbError, MB_OK, IDOK);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then begin
    { Inno has committed its repair payload and uninstall record. External
      updates cannot be rolled back, so retain both if any phase fails. }
    InstallFailed := not RunSetupWorker('Install', 'Windows', TargetDirectory, '');
    if not InstallFailed and PluginPage.Values[0] then
      InstallFailed := not RunSetupWorker('Install', 'PluginSettings', TargetDirectory, PluginSettingsPath);
    if not InstallFailed and CompanionBox.Checked then
      InstallFailed := not RunSetupWorker('Install', 'Companions', TargetDirectory, '');
    if InstallFailed then ReportWorkerFailure('Installation');
  end;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if (CurPageID = wpFinished) and InstallFailed then begin
    WizardForm.FinishedHeadingLabel.Caption := 'Setup needs attention';
    WizardForm.FinishedLabel.Caption := 'Some operations failed or were cancelled. ' +
      'Completed changes and the uninstaller have been kept. Run setup again to retry.';
  end;
end;

function GetCustomSetupExitCode: Integer;
begin
  if InstallFailed then Result := 10 else Result := 0;
end;

function ConfirmCompanionRemoval: Boolean;
var Form: TSetupForm; Box: TNewCheckBox; LabelText: TNewStaticText;
  Button: TNewButton;
begin
  Result := CompanionDefault;
  if UninstallSilent then exit;
  Form := NewSetupForm(520, 260);
  try
    Form.Caption := 'Uninstall WSL Tools for System Informer';
    LabelText := TNewStaticText.Create(Form);
    LabelText.Parent := Form;
    LabelText.SetBounds(ScaleX(12), ScaleY(12), ScaleX(496), ScaleY(46));
    LabelText.AutoSize := False;
    LabelText.WordWrap := True;
    LabelText.Caption := 'Remove WSL Tools from:' + #13#10 + TargetDirectory;
    Box := TNewCheckBox.Create(Form);
    Box.Parent := Form;
    Box.SetBounds(ScaleX(12), ScaleY(70), ScaleX(496), ScaleY(24));
    Box.Caption := 'Remove companion from all WSL2 distributions';
    Box.Checked := Result;
    LabelText := TNewStaticText.Create(Form);
    LabelText.Parent := Form;
    LabelText.SetBounds(ScaleX(30), ScaleY(104), ScaleX(478), ScaleY(106));
    LabelText.AutoSize := False;
    LabelText.WordWrap := True;
    LabelText.Caption := 'Includes stopped distributions, which will temporarily be started. ' +
      'Runs as Linux root.' + #13#10#13#10 + DistributionSummary + #13#10#13#10 +
      'Your settings and other plugins will be kept.';
    Button := TNewButton.Create(Form);
    Button.Parent := Form;
    Button.SetBounds(ScaleX(318), ScaleY(222), ScaleX(90), ScaleY(26));
    Button.Caption := 'Uninstall';
    Button.Default := True;
    Button.ModalResult := mrOk;
    Button := TNewButton.Create(Form);
    Button.Parent := Form;
    Button.SetBounds(ScaleX(418), ScaleY(222), ScaleX(90), ScaleY(26));
    Button.Caption := 'Cancel';
    Button.Cancel := True;
    Button.ModalResult := mrCancel;
    if Form.ShowModal <> mrOk then Abort;
    Result := Box.Checked;
  finally
    Form.Free;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var Companions: Boolean;
begin
  if CurUninstallStep = usUninstall then begin
    TargetDirectory := GetIniString('Installation', 'SystemInformerDirectory', '',
      ExpandConstant('{app}\installation.ini'));
    if TargetDirectory = '' then begin
      SuppressibleMsgBox('The saved System Informer folder is missing. Run setup to repair this installation.', mbError, MB_OK, IDOK);
      Abort;
    end;
    Companions := ConfirmCompanionRemoval;
    { Abort before Inno removes its own files/registration on failure. This
      allows a failed or cancelled companion removal to be retried. }
    if not RunSetupWorker('Uninstall', 'Windows', TargetDirectory, '') then begin
      ReportWorkerFailure('Uninstall');
      Abort;
    end;
    if Companions and not RunSetupWorker('Uninstall', 'Companions', TargetDirectory, '') then begin
      ReportWorkerFailure('Companion removal');
      Abort;
    end;
  end;
end;
