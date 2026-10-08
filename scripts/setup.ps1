# Start through setup.cmd/uninstall.cmd so Windows PowerShell uses an STA thread.
[CmdletBinding()]
param([ValidateSet('Install', 'Uninstall')] [string] $Mode = 'Install')
. (Join-Path $PSScriptRoot 'setup-common.ps1')
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[Windows.Forms.Application]::EnableVisualStyles()

$Form = New-Object Windows.Forms.Form
$Form.Text = 'WSL Tools setup'
$Form.ClientSize = New-Object Drawing.Size(660, 520)
$Form.MinimumSize = New-Object Drawing.Size(676, 559)
$Form.StartPosition = 'CenterScreen'
$Form.Font = [Drawing.SystemFonts]::MessageBoxFont
$Form.AutoScaleMode = 'Dpi'

$ModeLabel = New-Object Windows.Forms.Label
$ModeLabel.Text = 'Action:'
$ModeLabel.SetBounds(16, 19, 70, 22)
$ModeBox = New-Object Windows.Forms.ComboBox
$ModeBox.DropDownStyle = 'DropDownList'
$ModeBox.Items.AddRange(@('Install / update', 'Uninstall'))
$ModeBox.SetBounds(90, 16, 190, 24)
$ModeBox.SelectedIndex = if ($Mode -eq 'Install') { 0 } else { 1 }

$DirectoryLabel = New-Object Windows.Forms.Label
$DirectoryLabel.Text = 'System Informer folder:'
$DirectoryLabel.SetBounds(16, 53, 350, 22)
$Directory = New-Object Windows.Forms.TextBox
$Directory.Text = Find-SystemInformerDirectory
$Directory.SetBounds(16, 77, 534, 24)
$Directory.Anchor = 'Top, Left, Right'
$Browse = New-Object Windows.Forms.Button
$Browse.Text = 'Browse...'
$Browse.SetBounds(558, 76, 86, 25)
$Browse.Anchor = 'Top, Right'
$Browse.Add_Click({
    $Picker = New-Object Windows.Forms.FolderBrowserDialog
    $Picker.Description = 'Choose the folder containing SystemInformer.exe'
    $Picker.SelectedPath = $Directory.Text
    $Picker.ShowNewFolderButton = $false
    if ($Picker.ShowDialog($Form) -eq 'OK') { $Directory.Text = $Picker.SelectedPath }
    $Picker.Dispose()
})

$Companions = New-Object Windows.Forms.CheckBox
$Companions.SetBounds(16, 116, 628, 25)
$Companions.Anchor = 'Top, Left, Right'
$Companions.Checked = $true
$Notice = New-Object Windows.Forms.Label
$Notice.Text = 'Includes stopped distributions, which will be started and left running. Runs as Linux root.'
$Notice.SetBounds(34, 145, 610, 36)
$Notice.Anchor = 'Top, Left, Right'
$DistroLabel = New-Object Windows.Forms.Label
$DistroLabel.SetBounds(16, 183, 628, 45)
$DistroLabel.Anchor = 'Top, Left, Right'
# Inventory belongs to the caller, not to a possible alternate UAC account.
$Distros = @(Get-Wsl2Distributions)
$DistroLabel.Text = if ($Distros.Count) { 'WSL 2 distributions: ' + ($Distros -join ', ') }
    else { 'No WSL 2 distributions are registered for this Windows user.' }

$Instructions = New-Object Windows.Forms.Label
$Instructions.Text = 'Close System Informer before continuing. Your settings will be preserved. Windows may ask for administrator permission to change files in Program Files.'
$Instructions.SetBounds(16, 231, 628, 45)
$Instructions.Anchor = 'Top, Left, Right'
$Log = New-Object Windows.Forms.TextBox
$Log.Multiline = $true
$Log.ReadOnly = $true
$Log.ScrollBars = 'Vertical'
$Log.SetBounds(16, 284, 628, 180)
$Log.Anchor = 'Top, Bottom, Left, Right'
$Log.BackColor = [Drawing.SystemColors]::Window

$StartButton = New-Object Windows.Forms.Button
$StartButton.SetBounds(458, 480, 90, 26)
$StartButton.Anchor = 'Bottom, Right'
$CloseButton = New-Object Windows.Forms.Button
$CloseButton.Text = 'Close'
$CloseButton.SetBounds(554, 480, 90, 26)
$CloseButton.Anchor = 'Bottom, Right'
$Form.AcceptButton = $StartButton
$Form.CancelButton = $CloseButton
$State = @{ Worker = $null; Pending = $null; Queue = $null; Cancellation = $null; Running = $false }
$UpdateMode = {
    if ($ModeBox.SelectedIndex -eq 0) {
        $Companions.Text = 'Install the Linux companion in all WSL 2 distributions'
        $StartButton.Text = 'Install'
    } else {
        $Companions.Text = 'Remove the Linux companion from all WSL 2 distributions'
        $StartButton.Text = 'Uninstall'
    }
}
$ModeBox.Add_SelectedIndexChanged($UpdateMode)
& $UpdateMode

$Timer = New-Object Windows.Forms.Timer
$Timer.Interval = 100
$Timer.Add_Tick({
    if (-not $State.Running) { return }
    $Line = ''
    while ($State.Queue.TryDequeue([ref] $Line)) { $Log.AppendText($Line + [Environment]::NewLine) }
    if (-not $State.Pending.IsCompleted) { return }
    try { [void] $State.Worker.EndInvoke($State.Pending) }
    catch { $Log.AppendText($_.Exception.Message + [Environment]::NewLine) }
    foreach ($Problem in $State.Worker.Streams.Error) { $Log.AppendText($Problem.ToString() + [Environment]::NewLine) }
    $State.Worker.Dispose()
    $State.Cancellation.Dispose()
    $State.Running = $false
    $Timer.Stop()
    foreach ($Control in @($ModeBox, $Directory, $Browse, $Companions, $StartButton, $CloseButton)) { $Control.Enabled = $true }
    $CloseButton.Text = 'Close'
})
$StartButton.Add_Click({
    $Log.Clear()
    if (-not (Test-Path -LiteralPath (Join-Path $Directory.Text 'SystemInformer.exe'))) {
        $Log.Text = 'Choose the folder containing SystemInformer.exe.'
        return
    }
    $SelectedMode = if ($ModeBox.SelectedIndex -eq 0) { 'Install' } else { 'Uninstall' }
    $State.Queue = New-Object 'Collections.Concurrent.ConcurrentQueue[string]'
    $State.Cancellation = New-Object Threading.ManualResetEventSlim($false)
    $State.Worker = [PowerShell]::Create()
    # A separate runspace keeps the dialog responsive during UAC, WSL startup,
    # upload and removal. UI controls are touched only by the UI timer.
    [void] $State.Worker.AddScript({
        param($Common, $Mode, $Directory, $Companions, $Distros, $Queue, $Cancellation)
        . $Common
        Invoke-Setup $Mode $Directory '' $Companions $Distros $Queue $Cancellation
    }).AddArgument((Join-Path $PSScriptRoot 'setup-common.ps1')).AddArgument($SelectedMode).
        AddArgument($Directory.Text).AddArgument($Companions.Checked).AddArgument($Distros).
        AddArgument($State.Queue).AddArgument($State.Cancellation)
    $State.Pending = $State.Worker.BeginInvoke()
    $State.Running = $true
    foreach ($Control in @($ModeBox, $Directory, $Browse, $Companions, $StartButton)) { $Control.Enabled = $false }
    $CloseButton.Text = 'Cancel'
    $Timer.Start()
})
$CancelWork = {
    $State.Cancellation.Set()
    $CloseButton.Enabled = $false
    $Log.AppendText('Cancelling after the current Windows file operation; completed changes will be kept.' + [Environment]::NewLine)
}
$CloseButton.Add_Click({ if ($State.Running) { & $CancelWork } else { $Form.Close() } })
$Form.Add_FormClosing({
    param($Sender, $Event)
    if ($State.Running) { $Event.Cancel = $true; & $CancelWork }
})
$Form.Controls.AddRange(@($ModeLabel, $ModeBox, $DirectoryLabel, $Directory, $Browse,
    $Companions, $Notice, $DistroLabel, $Instructions, $Log, $StartButton, $CloseButton))
try { [void] $Form.ShowDialog() }
finally { $Timer.Dispose(); $Form.Dispose() }
