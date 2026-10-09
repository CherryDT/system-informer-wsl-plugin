// Headless PowerShell keeps WSL in the caller's account. Only its Windows file
// helper requests elevation. A timer keeps both installer and uninstaller
// responsive; cancellation is cooperative so WSL upload cleanup can finish.
type
  TWorkerExecuteInfo = record
    cbSize, fMask: LongWord;
    Wnd: HWND;
    Verb, FileName, Parameters, Directory: String;
    Show: Integer;
    Instance: THandle;
    IDList: LongWord;
    ClassName: String;
    ClassKey: THandle;
    HotKey: LongWord;
    Icon, Process: THandle;
  end;

function WorkerShellExecute(var Info: TWorkerExecuteInfo): Boolean;
  external 'ShellExecuteExW@shell32.dll stdcall';
function WorkerExitCode(Process: THandle; var Code: LongWord): Boolean;
  external 'GetExitCodeProcess@kernel32.dll stdcall';
function WorkerCloseHandle(Handle: THandle): Boolean;
  external 'CloseHandle@kernel32.dll stdcall';
function WorkerSetTimer(Wnd: HWND; ID, Interval, Callback: LongWord): LongWord;
  external 'SetTimer@user32.dll stdcall';
function WorkerKillTimer(Wnd: HWND; ID: LongWord): Boolean;
  external 'KillTimer@user32.dll stdcall';

var
  WorkForm: TSetupForm;
  WorkMemo: TNewMemo;
  WorkCancel: TNewButton;
  WorkProcess: THandle;
  WorkTimer, WorkResult: LongWord;
  WorkProgressFile, WorkCancelFile, WorkDiagnostics: String;
  WorkRunning, WorkTickActive: Boolean;

function NewSetupForm(Width, Height: Integer): TSetupForm;
begin
#if VER >= 0x06060000
  Result := CreateCustomForm(ScaleX(Width), ScaleY(Height), False, False);
#else
  Result := CreateCustomForm;
  Result.ClientWidth := ScaleX(Width);
  Result.ClientHeight := ScaleY(Height);
#endif
  Result.Position := poScreenCenter;
end;

function NativeQuote(Value: String): String;
begin
  { All arguments are data for -File, never PowerShell expressions. Windows
    filenames cannot contain quotes; double a trailing slash before the quote. }
  if (Length(Value) > 0) and (Value[Length(Value)] = '\') then
    Value := Value + '\';
  Result := '"' + Value + '"';
end;

procedure CancelWorker(Sender: TObject);
begin
  if not WorkRunning then exit;
  SaveStringToFile(WorkCancelFile, 'cancel', False);
  WorkCancel.Enabled := False;
  WorkCancel.Caption := 'Cancelling...';
end;

procedure WorkerCloseQuery(Sender: TObject; var CanClose: Boolean);
begin
  CanClose := not WorkRunning;
  if not CanClose then CancelWorker(Sender);
end;

procedure ReadWorkerProgress;
var Lines: TStringList;
begin
  if not FileExists(WorkProgressFile) then exit;
  Lines := TStringList.Create;
  try
    { The worker writes UTF-16 with a BOM. Reading may race an append. Keep the
      previous display if a read fails; retry on the next timer tick. }
    try
      Lines.LoadFromFile(WorkProgressFile);
      if WorkMemo.Text <> Lines.Text then begin
        WorkMemo.Lines.Assign(Lines);
        SendMessage(WorkMemo.Handle, $00B1, Length(WorkMemo.Text), Length(WorkMemo.Text));
        SendMessage(WorkMemo.Handle, $00B7, 0, 0);
      end;
    except
      Log('Waiting for setup progress file.');
    end;
  finally
    Lines.Free;
  end;
end;

procedure WorkerTick(Wnd: HWND; Msg, TimerID, Time: LongWord);
var Code: LongWord;
begin
  if WorkTickActive or not WorkRunning then exit;
  WorkTickActive := True;
  try
    ReadWorkerProgress;
    if not WorkerExitCode(WorkProcess, Code) then Code := 1;
    if Code <> 259 then begin
      ReadWorkerProgress;
      WorkResult := Code;
      WorkRunning := False;
      WorkForm.Close;
    end;
  finally
    WorkTickActive := False;
  end;
end;

function RunSetupWorker(Mode, Phase, Target: String): Boolean;
var Info: TWorkerExecuteInfo; Params: String; I: Integer; Silent: Boolean;
begin
  Result := False;
  if IsUninstaller then Silent := UninstallSilent else Silent := WizardSilent;
  WorkDiagnostics := '';
  WorkResult := 1;
  WorkProgressFile := ExpandConstant('{tmp}\wsl-tools-' + Phase + '.log');
  WorkCancelFile := ExpandConstant('{tmp}\wsl-tools-' + Phase + '.cancel');
  DeleteFile(WorkProgressFile);
  DeleteFile(WorkCancelFile);
  WorkForm := NewSetupForm(560, 320);
  try
    WorkForm.Caption := Mode + ' WSL Tools for System Informer';
    WorkForm.OnCloseQuery := @WorkerCloseQuery;
    WorkMemo := TNewMemo.Create(WorkForm);
    WorkMemo.Parent := WorkForm;
    WorkMemo.SetBounds(ScaleX(12), ScaleY(12), ScaleX(536), ScaleY(260));
    WorkMemo.ReadOnly := True;
    WorkMemo.ScrollBars := ssVertical;
    WorkMemo.WordWrap := True;
    WorkMemo.Text := 'Preparing ' + Lowercase(Phase) + '...';
    WorkCancel := TNewButton.Create(WorkForm);
    WorkCancel.Parent := WorkForm;
    WorkCancel.SetBounds(ScaleX(458), ScaleY(282), ScaleX(90), ScaleY(26));
    WorkCancel.Caption := 'Cancel';
    WorkCancel.Cancel := True;
    WorkCancel.OnClick := @CancelWorker;
    Params := '-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File ' +
      NativeQuote(ExpandConstant('{app}\setup-worker.ps1')) +
      ' -Mode ' + Mode + ' -Phase ' + Phase +
      ' -SystemInformerDirectory ' + NativeQuote(Target) +
      ' -DistributionDirectory ' + NativeQuote(ExpandConstant('{app}')) +
      ' -ProgressFile ' + NativeQuote(WorkProgressFile) +
      ' -CancelFile ' + NativeQuote(WorkCancelFile);
    Info.cbSize := SizeOf(Info);
    Info.fMask := $00000040 or $00000100; { NOCLOSEPROCESS | NOASYNC }
    Info.Wnd := WorkForm.Handle;
    Info.Verb := 'open';
    Info.FileName := ExpandConstant('{sysnative}\WindowsPowerShell\v1.0\powershell.exe');
    Info.Parameters := Params;
    Info.Show := SW_HIDE;
    if not WorkerShellExecute(Info) then begin
      WorkDiagnostics := 'Could not start Windows PowerShell: ' + SysErrorMessage(DLLGetLastError);
      exit;
    end;
    WorkProcess := Info.Process;
    WorkRunning := True;
    try
      if Silent then begin
        while WorkRunning do begin
          WorkerTick(0, 0, 0, 0);
          if WorkRunning then Sleep(100);
        end;
      end else begin
        WorkTimer := WorkerSetTimer(0, 0, 100, CreateCallback(@WorkerTick));
        if WorkTimer = 0 then begin
          CancelWorker(WorkCancel);
          while WorkRunning do begin
            WorkerTick(0, 0, 0, 0);
            if WorkRunning then Sleep(100);
          end;
          RaiseException('Could not create the setup progress timer.');
        end;
        try
          WorkForm.ShowModal;
        finally
          WorkerKillTimer(0, WorkTimer);
        end;
      end;
      WorkDiagnostics := WorkMemo.Text;
      for I := 0 to WorkMemo.Lines.Count - 1 do Log(WorkMemo.Lines[I]);
      Result := WorkResult = 0;
    finally
      WorkRunning := False;
      WorkerCloseHandle(WorkProcess);
    end;
  finally
    WorkForm.Free;
  end;
end;
