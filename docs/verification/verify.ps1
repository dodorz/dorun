# =============================================================================
#  DoRun clipboard history verification - automated assertions
#  Companion document: docs\verification\clipboard-windows-verification.md
#
#    powershell.exe -NoProfile -ExecutionPolicy Bypass `
#      -File docs\verification\verify.ps1 -Sections 0,1,2,3,4,5
#
#  How the popup is driven (learned from the earlier probes):
#   * The popup is opened by PostMessage(WM_HOTKEY, ID_HOTKEY_CLIPBOARD=40002)
#     to DoRun's own DoRun.HostWindow found by EnumWindows on its pid.
#   * DoRun treats "hotkey modifiers released" as the end of a cycling
#     sequence and then pastes + hides, so the popup only stays on screen
#     while ALT is physically held: keybd_event(VK_MENU) around every
#     interaction.
#   * Keys go to the popup's ListBox with PostMessage(WM_KEYDOWN); the
#     modifier state DoRun reads with GetKeyState() comes from keybd_event.
#
#  Verdicts print as  RESULT | <id> | <item> | PASS/FAIL/PARTIAL | note
#  and are appended to %LOCALAPPDATA%\DoRun\verification\report3-<stamp>.txt .
# =============================================================================

param(
    [string]$Sections = '0,1,2,3,4,5',
    [switch]$SkipBackup,
    [switch]$NoPrompts
)

$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$Stamp       = (Get-Date -Format 'yyyyMMdd-HHmmss')
$RunDir      = Join-Path $env:LOCALAPPDATA 'DoRun\verification'
$StoreDir    = Join-Path $env:LOCALAPPDATA 'DoRun'
$HistoryPath = Join-Path $StoreDir 'clipboard-history.txt'
$BlobDir     = Join-Path $StoreDir 'clipboard-blobs'
$LogPath     = Join-Path $StoreDir 'hotkey-debug.log'
$ReportPath  = Join-Path $RunDir ("report3-$Stamp.txt")
$LogDumpPath = Join-Path $RunDir ("hotkey-debug3-$Stamp.txt")
$Magic       = 'DoRunClipboardHistoryV2'
$ConfigPath  = 'C:\Users\dodo\Scoop\persist\DoRun\DoRun.yaml'

New-Item -ItemType Directory -Force -Path $RunDir | Out-Null

$script:Lines = New-Object System.Collections.Generic.List[string]
function Out-Line([string]$Text) {
    $line = ('[{0:HH:mm:ss}] {1}' -f (Get-Date), $Text)
    Write-Host $line
    $script:Lines.Add($line)
    ($script:Lines -join "`r`n") | Set-Content -LiteralPath $ReportPath -Encoding UTF8
}
function Section([string]$Title) {
    Out-Line ''; Out-Line ('=' * 78); Out-Line ('## ' + $Title); Out-Line ('=' * 78)
}
function Verdict([string]$Id, [string]$Item, [string]$Result, [string]$Note) {
    Out-Line ('RESULT | {0,-7} | {1,-40} | {2,-7} | {3}' -f $Id, $Item, $Result, $Note)
}
function Pass([string]$Id, [string]$I, [string]$N = '') { Verdict $Id $I 'PASS' $N }
function Fail([string]$Id, [string]$I, [string]$N = '') { Verdict $Id $I 'FAIL' $N }
function Part([string]$Id, [string]$I, [string]$N = '') { Verdict $Id $I 'PARTIAL' $N }
function Ask([string]$Id, [string]$Item, [string]$Question) {
    Out-Line ('ACTION | ' + $Question)
    if ($NoPrompts) { Part $Id $Item 'operator step pending'; return }
    Write-Host ''
    Write-Host ('  >> ' + $Item) -ForegroundColor Yellow
    $a = Read-Host '  p / f / note'
    if ($a -eq 'p') { Pass $Id $Item 'confirmed by operator' }
    elseif ($a -eq 'f') { Fail $Id $Item 'operator reported unexpected behaviour' }
    else { Part $Id $Item ('operator: ' + $a) }
}

# --------------------------------------------------------------------- Win32
if (-not ('W' -as [type])) {
Add-Type -Namespace W -Name U -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string n);
[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr l);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder sb, int max);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder sb, int max);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
[DllImport("user32.dll", EntryPoint="SendMessageW")] public static extern IntPtr SendMsgN(IntPtr h, uint msg, IntPtr w, IntPtr l);
[DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] public static extern IntPtr SendMsgS(IntPtr h, uint msg, IntPtr w, System.Text.StringBuilder l);
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr SendMsgSetText(IntPtr h, string text);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
public delegate bool EnumProc(IntPtr h, IntPtr l);
public struct RECT { public int Left, Top, Right, Bottom; }
'@
}

$WM_HOTKEY   = 0x0312
$WM_KEYDOWN  = 0x0100
$WM_CLOSE    = 0x0010
$VK_RETURN   = 13
$VK_DELETE   = 46
$VK_CONTROL  = 0x11
$VK_SHIFT    = 0x10
$VK_MENU     = 0x12
$ID_HK_LAUNCH     = 40001
$ID_HK_CLIPBOARD  = 40002
$LB_GETCOUNT      = 0x018B
$LB_GETTEXT       = 0x0189
$LB_GETITEMHEIGHT = 0x01A1

function Get-DoRunProc { Get-Process DoRun -ErrorAction SilentlyContinue | Select-Object -First 1 }

function Get-WinMap {
    # FindWindow fails for these class names in this environment (it returns 0
    # with ERROR_INVALID_PARAMETER even though the windows exist), so the
    # windows are located by enumerating all top-level windows.
    $script:WinMap = @{}
    $cb = {
        param($h, $l)
        $sb = New-Object System.Text.StringBuilder 256
        [void][W.U]::GetClassNameW($h, $sb, 256)
        $cls = $sb.ToString()
        if ($cls.StartsWith('DoRun.')) { $script:WinMap[$cls] = $h }
        return $true
    }
    [void][W.U]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:WinMap
}
function Get-HostHandle {
    $m = Get-WinMap
    if ($m.ContainsKey('DoRun.HostWindow')) { return $m['DoRun.HostWindow'] }
    return [IntPtr]::Zero
}
function Get-PopupHandle {
    $m = Get-WinMap
    if ($m.ContainsKey('DoRun.ClipboardWindow')) { return $m['DoRun.ClipboardWindow'] }
    return [IntPtr]::Zero
}
function Test-PopupOpen {
    $h = Get-PopupHandle
    return ($h -ne [IntPtr]::Zero -and [W.U]::IsWindowVisible($h))
}

function Get-Kids {
    $kids = New-Object System.Collections.ArrayList
    $p = Get-PopupHandle
    if ($p -eq [IntPtr]::Zero) { return $kids }
    $cb = {
        param($h, $l)
        $sb = New-Object System.Text.StringBuilder 256
        [void][W.U]::GetClassNameW($h, $sb, 256)
        $cls = $sb.ToString()
        $txt = ''
        if ($cls -eq 'Static') {
            $sb2 = New-Object System.Text.StringBuilder 4096
            [void][W.U]::GetWindowTextW($h, $sb2, 4096)
            $txt = $sb2.ToString()
        }
        [void]$script:KidList.Add([pscustomobject]@{ Cls = $cls; Hwnd = $h; Text = $txt })
        return $true
    }
    $script:KidList = $kids
    [void][W.U]::EnumChildWindows($p, $cb, [IntPtr]::Zero)
    return $kids
}
function Get-ListHandle {
    foreach ($k in (Get-Kids)) { if ($k.Cls -eq 'ListBox') { return $k.Hwnd } }
    return [IntPtr]::Zero
}
function Get-HintText {
    foreach ($k in (Get-Kids)) {
        if ($k.Cls -eq 'Static' -and ($k.Text -match 'Enter' -or $k.Text -match 'Del remove')) { return $k.Text }
    }
    return ''
}
function Get-PreviewText {
    foreach ($k in (Get-Kids)) {
        if ($k.Cls -eq 'Static' -and $k.Text -notmatch 'Enter' -and $k.Text.Length -gt 0) { return $k.Text }
    }
    return ''
}
function Get-ListLabels {
    $labels = @()
    $list = Get-ListHandle
    if ($list -eq [IntPtr]::Zero) { return $labels }
    $count = [int]([W.U]::SendMsgN($list, $LB_GETCOUNT, [IntPtr]::Zero, [IntPtr]::Zero))
    for ($i = 0; $i -lt $count; $i++) {
        $sb = New-Object System.Text.StringBuilder 4096
        [void][W.U]::SendMsgS($list, $LB_GETTEXT, [IntPtr]$i, $sb)
        $labels += $sb.ToString()
    }
    return $labels
}

# ------------------------------------------------------------ popup sessions
# Every manipulation happens while ALT is physically down, otherwise DoRun ends
# the cycling sequence, pastes and hides the popup.
function Open-PopupSession {
    if (Test-PopupOpen) { return $true }
    $host_ = Get-HostHandle
    if ($host_ -eq [IntPtr]::Zero) { Out-Line 'host window not found'; return $false }
    # Hold Alt across the whole session: DoRun ends the cycling sequence (and
    # hides the popup) as soon as the hotkey modifiers are released.
    for ($attempt = 0; $attempt -lt 3; $attempt++) {
        [W.U]::keybd_event($VK_MENU, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 200
        [void][W.U]::PostMessage($host_, $WM_HOTKEY, [IntPtr]$ID_HK_CLIPBOARD, [IntPtr]::Zero)
        for ($i = 0; $i -lt 25; $i++) {
            Start-Sleep -Milliseconds 100
            if (Test-PopupOpen) { return $true }
        }
        [W.U]::keybd_event($VK_MENU, 0, 2, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 400
    }
    Out-Line 'popup did not appear after WM_HOTKEY'
    return $false
}
function Close-PopupSession {
    $p = Get-PopupHandle
    if ($p -ne [IntPtr]::Zero) { [void][W.U]::PostMessage($p, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero) }
    Start-Sleep -Milliseconds 300
    [W.U]::keybd_event($VK_MENU, 0, 2, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 400
}
function Press-ListKey([int]$Vk, [switch]$Ctrl, [switch]$Shift) {
    $list = Get-ListHandle
    if ($list -eq [IntPtr]::Zero) { Out-Line 'listbox not found'; return $false }
    if ($Ctrl) { [W.U]::keybd_event($VK_CONTROL, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 60 }
    if ($Shift) { [W.U]::keybd_event($VK_SHIFT, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 60 }
    [void][W.U]::PostMessage($list, $WM_KEYDOWN, [IntPtr]$Vk, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 60
    if ($Ctrl) { [W.U]::keybd_event($VK_CONTROL, 0, 2, [UIntPtr]::Zero) }
    if ($Shift) { [W.U]::keybd_event($VK_SHIFT, 0, 2, [UIntPtr]::Zero) }
    Start-Sleep -Milliseconds 800
    return $true
}

# ---------------------------------------------------------------- clipboard
function Get-Formats {
    try {
        $d = [System.Windows.Forms.Clipboard]::GetDataObject()
        if ($null -eq $d) { return @() }
        return @($d.GetFormats() | ForEach-Object { [string]$_ })
    } catch { Out-Line ('clipboard read failed: ' + $_.Exception.Message); return @() }
}
function Has-F($Formats, [string]$Name) {
    if ($Name -eq '') { return $false }
    foreach ($f in @($Formats)) { if ($f -eq $Name) { return $true } }
    return $false
}
function Get-ClipText { try { [System.Windows.Forms.Clipboard]::GetText() } catch { '' } }
function Set-ClipText([string]$T) { [System.Windows.Forms.Clipboard]::SetText($T) }
function Set-ClipRtf([string]$Plain, [string]$Rtf) {
    $d = New-Object System.Windows.Forms.DataObject
    $d.SetData([System.Windows.Forms.DataFormats]::UnicodeText, $Plain)
    $d.SetData('Rich Text Format', $Rtf)
    [System.Windows.Forms.Clipboard]::SetDataObject($d, $true)
}
function Set-ClipImage($Bmp) { [System.Windows.Forms.Clipboard]::SetImage($Bmp) }
function Set-ClipFiles([string[]]$Paths) {
    # SetFileDropList publishes CF_HDROP the way Explorer does; a DataObject
    # built with SetData('FileDrop', ...) only works when the producer also
    # offers FileNameW/FileName, which a bare harness copy does not.
    $c = New-Object System.Collections.Specialized.StringCollection
    foreach ($path in $Paths) { [void]$c.Add($path) }
    [System.Windows.Forms.Clipboard]::SetFileDropList($c)
}
function Set-ClipPackedDib([byte[]]$Dib) {
    # System.Windows.Forms serialises a byte() payload into its own stream, so
    # the packed DIB is published as raw clipboard bytes instead.
    Set-RawClipboard8 $Dib
}
# Simulates a password manager that marks its copy as private (1.5 mechanism).
function Set-ClipOptOut([string]$Text) {
    $d = New-Object System.Windows.Forms.DataObject
    $d.SetData([System.Windows.Forms.DataFormats]::UnicodeText, $Text)
    $d.SetData('ExcludeClipboardContentFromMonitorProcessing', (New-Object byte[] 1))
    [System.Windows.Forms.Clipboard]::SetDataObject($d, $true)
}
# Raw HGLOBAL clipboard writers for the opt-out formats. System.Windows.Forms
# wraps the value in its own serialisation stream, which DoRun correctly does
# not read as a DWORD, so the opt-out flags must be published as raw bytes.
Add-Type -Namespace WOpt -Name K -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool OpenClipboard(IntPtr owner);
[DllImport("user32.dll")] public static extern bool CloseClipboard();
[DllImport("user32.dll")] public static extern bool EmptyClipboard();
[DllImport("user32.dll")] public static extern IntPtr SetClipboardData(uint fmt, IntPtr mem);
[DllImport("kernel32.dll")] public static extern IntPtr GlobalAlloc(uint flags, UIntPtr bytes);
[DllImport("kernel32.dll")] public static extern IntPtr GlobalLock(IntPtr mem);
[DllImport("kernel32.dll")] public static extern bool GlobalUnlock(IntPtr mem);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern uint RegisterClipboardFormatW(string name);
'@

function New-GlobalBytes([byte[]]$bytes) {
    $h = [WOpt.K]::GlobalAlloc(0x0042, [UIntPtr]::new([uint64]$bytes.Length))  # GMEM_MOVEABLE|GMEM_ZEROINIT
    $p = [WOpt.K]::GlobalLock($h)
    [System.Runtime.InteropServices.Marshal]::Copy($bytes, 0, $p, $bytes.Length)
    [void][WOpt.K]::GlobalUnlock($h)
    return [IntPtr]$h
}

function Set-RawClipboard([string]$Text, [string]$Fmt, [byte[]]$Value) {
    [void][WOpt.K]::OpenClipboard([IntPtr]::Zero)
    [void][WOpt.K]::EmptyClipboard()
    $textBytes = [System.Text.Encoding]::Unicode.GetBytes($Text + [char]0)
    [void][WOpt.K]::SetClipboardData(13, [IntPtr](New-GlobalBytes $textBytes))   # CF_UNICODETEXT
    if ($Fmt) {
        $fmt = [WOpt.K]::RegisterClipboardFormatW($Fmt)
        [void][WOpt.K]::SetClipboardData($fmt, [IntPtr](New-GlobalBytes $Value))
    }
    [void][WOpt.K]::CloseClipboard()
}

# CF_DIB (format 8) is the identifier DoRun reads besides CF_DIBV5, so the
# packed 8bpp payload is published under that exact format id.
function Set-RawClipboard8([byte[]]$Dib) {
    [void][WOpt.K]::OpenClipboard([IntPtr]::Zero)
    [void][WOpt.K]::EmptyClipboard()
    [void][WOpt.K]::SetClipboardData(8, (New-GlobalBytes $Dib))
    [void][WOpt.K]::CloseClipboard()
}

function Set-ClipOptOutFlag([string]$Text) {
    Set-RawClipboard $Text 'CanIncludeInClipboardHistory' @(0, 0, 0, 0)
}
function Wait-Poll([int]$Ms = 1600) { Start-Sleep -Milliseconds $Ms }

# --------------------------------------------------------- log / store / yaml
function Get-LogCount { if (-not (Test-Path $LogPath)) { return 0 } ; return @(Get-Content -LiteralPath $LogPath -Encoding UTF8).Count }
function Get-LogTail([int]$From) {
    if (-not (Test-Path $LogPath)) { return @() }
    $all = @(Get-Content -LiteralPath $LogPath -Encoding UTF8)
    if ($From -ge $all.Count) { return @() }
    return @($all[$From..($all.Count - 1)])
}
function Get-SkipLines([int]$From) {
    return @(Get-LogTail $From | Where-Object { $_ -match 'Clipboard capture skipped' })
}

function Set-YamlScalar([string]$Key, [string]$Value) {
    $lines = @([System.IO.File]::ReadAllLines($ConfigPath, [System.Text.Encoding]::UTF8))
    $hit = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match ('^' + [regex]::Escape($Key) + ':')) { $lines[$i] = $Key + ': ' + $Value; $hit = $true; break }
    }
    if (-not $hit) { $lines = @($lines) + ($Key + ': ' + $Value) }
    [System.IO.File]::WriteAllText($ConfigPath, (($lines -join "`r`n") + "`r`n"), (New-Object System.Text.UTF8Encoding($false)))
    Wait-Poll 1500
}
function Set-YamlList([string]$Key, [string[]]$Values) {
    $lines = @([System.IO.File]::ReadAllLines($ConfigPath, [System.Text.Encoding]::UTF8))
    $idx = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match ('^' + [regex]::Escape($Key) + ':')) { $idx = $i; break }
    }
    $block = @($Key + ':')
    foreach ($v in $Values) { $block += '  - "' + $v + '"' }
    if ($idx -lt 0) {
        $lines = @($lines) + $block
    } else {
        $end = $idx + 1
        while ($end -lt $lines.Count -and $lines[$end] -match '^\s+- ') { $end++ }
        $new = New-Object System.Collections.Generic.List[string]
        for ($i = 0; $i -lt $idx; $i++) { $new.Add($lines[$i]) }
        foreach ($b in $block) { $new.Add($b) }
        for ($i = $end; $i -lt $lines.Count; $i++) { $new.Add($lines[$i]) }
        $lines = $new.ToArray()
    }
    [System.IO.File]::WriteAllText($ConfigPath, (($lines -join "`r`n") + "`r`n"), (New-Object System.Text.UTF8Encoding($false)))
    Wait-Poll 1500
}
function Get-YamlLine([string]$Key) {
    return (@([System.IO.File]::ReadAllLines($ConfigPath, [System.Text.Encoding]::UTF8) | Where-Object { $_ -match ('^' + [regex]::Escape($Key) + ':') }) -join '; ')
}

function Get-HistRaw { if (-not (Test-Path $HistoryPath)) { return @() } ; return @([System.IO.File]::ReadAllLines($HistoryPath, [System.Text.Encoding]::UTF8)) }
function Get-Entries {
    $all = @(Get-HistRaw)
    if ($all.Count -eq 0) { return @() }
    if ($all[0] -eq $Magic) {
        if ($all.Count -le 1) { return @() }
        return @($all[1..($all.Count - 1)] | Where-Object { $_ -ne '' })
    }
    return @($all | Where-Object { $_ -ne '' })
}
function Get-TopEntry { $e = @(Get-Entries) ; if ($e.Count -eq 0) { return '' } ; return $e[0] }
function Get-TopKind { $t = Get-TopEntry ; if ($t -eq '') { return '' } ; return ($t -split "`t")[0] }
function Get-TopField([int]$N) {
    $t = Get-TopEntry
    if ($t -eq '') { return '' }
    $f = $t -split "`t"
    if ($f.Count -le $N) { return '' }
    return $f[$N]
}
function Get-TopText { return (Get-TopField 1) }
function Unescape-IndexField([string]$Text) {
    $out = New-Object System.Text.StringBuilder
    for ($i = 0; $i -lt $Text.Length; $i++) {
        if ($Text[$i] -eq [char]92 -and ($i + 1) -lt $Text.Length) {
            $n = $Text[$i + 1]
            if ($n -eq 't') { [void]$out.Append([char]9); $i++; continue }
            if ($n -eq 'n') { [void]$out.Append([char]13); [void]$out.Append([char]10); $i++; continue }
            if ($n -eq 'r') { [void]$out.Append([char]13); $i++; continue }
            if ($n -eq [char]92) { [void]$out.Append([char]92); $i++; continue }
        }
        [void]$out.Append($Text[$i])
    }
    return $out.ToString()
}
function Get-CountKind([string]$K) { return @(Get-Entries | Where-Object { ($_ -split "`t")[0] -eq $K }).Count }
function Get-Blobs { if (-not (Test-Path $BlobDir)) { return @() } ; return @(Get-ChildItem -LiteralPath $BlobDir -File -ErrorAction SilentlyContinue) }
function Get-BlobsExt([string]$E) { return @(Get-Blobs | Where-Object { $_.Extension -eq $E }) }

# ---------------------------------------------------------------- payloads
function New-TestRtf([string]$Plain) {
    return ('{\rtf1\ansi\ansicpg1252\deff0{\fonttbl{\f0\fswiss Segoe UI;}}{\colortbl;\red255\green0\blue0;}' +
            '\viewkind4\uc1\pard\f0\fs24\b\cf2 ' + $Plain + '\cf1\b0  plain tail\par}')
}
function New-Packed8bppDib([int]$Wd, [int]$Ht) {
    $stride = ([Math]::Floor(($Wd * 8 + 31) / 32) * 4)
    $pix = $stride * $Ht
    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter($ms)
    $bw.Write([int]40); $bw.Write([int]$Wd); $bw.Write([int]$Ht)
    $bw.Write([int16]1); $bw.Write([int16]8); $bw.Write([int]0); $bw.Write([int]$pix)
    $bw.Write([int]0); $bw.Write([int]0); $bw.Write([int]256); $bw.Write([int]256)
    for ($i = 0; $i -lt 256; $i++) {
        $bw.Write([byte](255 - $i)); $bw.Write([byte]$i); $bw.Write([byte]128); $bw.Write([byte]0)
    }
    $bits = New-Object 'byte[]' $pix
    $cx = $Wd / 2; $cy = $Ht / 2; $rr = $Wd / 3
    for ($r = 0; $r -lt $Ht; $r++) {
        $base = $r * $stride
        for ($c = 0; $c -lt $Wd; $c++) {
            $dx = $c - $cx; $dy = $r - $cy
            if (($dx * $dx + $dy * $dy) -lt ($rr * $rr)) { $bits[$base + $c] = 2 } else { $bits[$base + $c] = 1 }
        }
    }
    $bw.Write($bits); $bw.Flush()
    $data = $ms.ToArray()
    $bw.Dispose(); $ms.Dispose()
    return ,$data
}
function New-Bitmap([int]$Wd, [int]$Ht, [string]$Color) {
    $b = New-Object System.Drawing.Bitmap $Wd, $Ht
    $g = [System.Drawing.Graphics]::FromImage($b)
    $g.Clear([System.Drawing.Color]::FromName($Color))
    $g.Dispose()
    return $b
}
function Get-TempPath([string]$N) { return (Join-Path $env:TEMP $N) }

# Copies the marker text out of Notepad and returns the text that actually
# reached the clipboard, so a harness failure cannot look like a product bug.
#
# Windows 11 ships Notepad as a packaged app: Start-Process notepad.exe spawns a
# launcher that exits, and the real window belongs to a *different* process, so
# the window is located by title instead of by the spawned pid.
function Find-NotepadWindow {
    for ($i = 0; $i -lt 50; $i++) {
        $p = Get-Process notepad -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
        if ($p) { return $p }
        Start-Sleep -Milliseconds 100
    }
    return $null
}

function Set-NotepadTextAndCopy([IntPtr]$Hwnd, [string]$Text) {
    # WM_SETTEXT + EM_SETSEL + WM_COPY: deterministic and focus independent.
    # A lone probe run produced a byte-identical clipboard, while SendKeys
    # dropped characters in the Win11 Notepad editor.
    $script:EditTarget = [IntPtr]::Zero
    $cb = {
        param($x, $l)
        $sb = New-Object System.Text.StringBuilder 256
        [void][W.U]::GetClassNameW($x, $sb, 256)
        if ($sb.ToString() -match '^(RichEdit|Edit|Scintilla)') { $script:EditTarget = $x }
        return $true
    }
    [void][W.U]::EnumChildWindows($Hwnd, $cb, [IntPtr]::Zero)
    $target = $script:EditTarget
    if ($target -eq [IntPtr]::Zero) { return $false }
    [void][W.U]::SendMsgSetText($target, $Text)
    Start-Sleep -Milliseconds 250
    [void][W.U]::SendMsgN($target, 0x00B1, [IntPtr]0, [IntPtr](-1))       # EM_SETSEL 0,-1
    Start-Sleep -Milliseconds 250
    [void][W.U]::SendMsgN($target, 0x0301, [IntPtr]::Zero, [IntPtr]::Zero) # WM_COPY
    Start-Sleep -Milliseconds 900
    return $true
}

function Copy-FromNotepad([string]$Text, [switch]$KeepOpen) {
    # Windows 11 Notepad is a single-instance packaged app: the launcher exits
    # at once and the real window may belong to an older instance, which would
    # silently swallow the text. All instances are stopped first.
    Stop-AllNotepads
    $real = $null
    $clip = ''
    for ($attempt = 0; $attempt -lt 3; $attempt++) {
        if ($null -eq $real) {
            $real = Find-NotepadWindow
            if ($null -eq $real) {
                [void](Start-Process notepad.exe)
                $real = Find-NotepadWindow
            }
        }
        if ($null -eq $real) { Out-Line 'harness: no Notepad window appeared'; return '' }
        [void](Set-NotepadTextAndCopy ([IntPtr]$real.MainWindowHandle) $Text)
        $clip = ''
        try { $clip = [System.Windows.Forms.Clipboard]::GetText() } catch {}
        if ($clip -eq $Text) { break }
        Start-Sleep -Milliseconds 600
    }
    if (-not $KeepOpen) {
        Stop-Process -Id $real.Id -Force -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 700
    }
    return $clip
}

# True when the clipboard text holds the marker; SendKeys can drop characters,
# so the comparison uses the distinctive prefix instead of the full string.
function ClipboardHasMarker([string]$Clip, [string]$Marker) {
    if ($Clip -eq '') { return $false }
    return $Clip.StartsWith($Marker.Substring(0, [Math]::Min(8, $Marker.Length)))
}


$Do = @($Sections -split ',' | ForEach-Object { $_.Trim() })

# =============================================================================
if ($Do -contains '0') {
    Section '0. Prerequisites'
    $proc = Get-DoRunProc
    if ($null -eq $proc) { Out-Line 'ABORT: DoRun is not running.'; return }
    $exe = $proc.Path
    $fv = (Get-Item -LiteralPath $exe).VersionInfo.FileVersion
    Out-Line ('running exe    : ' + $exe)
    Out-Line ('FileVersion    : ' + $fv)
    if ($fv -eq '0.3.1.56') { Pass '0.1' 'binary is 0.3.1.56' $fv } else { Fail '0.1' 'binary is 0.3.1.56' $fv }

    Out-Line ('active config  : ' + $ConfigPath)
    if (Test-Path $ConfigPath) { Pass '0.2' 'config file in effect' $ConfigPath } else { Fail '0.2' 'config file in effect' 'missing' }
    $sib = Join-Path (Split-Path $exe -Parent) 'DoRun.yaml'
    Out-Line ('exe-sibling    : ' + $sib + ' exists=' + (Test-Path $sib) + ' (same file via Scoop persist)')
    Out-Line ('roaming yaml   : ' + (Join-Path $env:APPDATA 'DoRun\DoRun.yaml') + ' exists=' + (Test-Path (Join-Path $env:APPDATA 'DoRun\DoRun.yaml')))

    $dbg = Get-YamlLine 'HOTKEY_DEBUG_LOG'
    Out-Line ('debug log      : ' + $dbg)
    if ($dbg -match 'true|1') { Pass '0.3' 'debug log enabled' $LogPath } else { Set-YamlScalar 'HOTKEY_DEBUG_LOG' 'true' ; Pass '0.3' 'debug log enabled by script' $LogPath }
    Out-Line ('log exists     : ' + (Test-Path $LogPath))
    Out-Line ('history exists : ' + (Test-Path $HistoryPath))
    Out-Line ('blob dir       : ' + (Test-Path $BlobDir))

    $raw = @(Get-HistRaw)
    Out-Line ('history now    : lines=' + $raw.Count + ' text=' + (Get-CountKind 'text') + ' rtf=' + (Get-CountKind 'rtf') +
              ' image=' + (Get-CountKind 'image') + ' files=' + (Get-CountKind 'files'))
    Out-Line ('first line     : ' + $(if ($raw.Count -gt 0) { $raw[0] } else { '(empty)' }))
    if ($raw.Count -gt 0 -and $raw[0] -eq $Magic) { Pass '3.4-a' 'index first line is V2 magic' $Magic }
    elseif ($raw.Count -gt 0) { Fail '3.4-a' 'index first line is V2 magic' $raw[0] }

    if (-not $SkipBackup) {
        $bak = Join-Path $RunDir ("store-backup3-$Stamp")
        New-Item -ItemType Directory -Force -Path $bak | Out-Null
        if (Test-Path $HistoryPath) { Copy-Item $HistoryPath (Join-Path $bak 'clipboard-history.txt') -Force }
        if (Test-Path $BlobDir) { Copy-Item $BlobDir (Join-Path $bak 'clipboard-blobs') -Recurse -Force }
        Out-Line ('store backed up to ' + $bak)
        Set-YamlScalar 'CLIPBOARD_HISTORY_ENABLED' 'false'
        Set-YamlScalar 'CLIPBOARD_HISTORY_ENABLED' 'true'
        Wait-Poll 1200
        $after = @(Get-Entries).Count
        Out-Line ('entries after clear: ' + $after)
        if ($after -eq 0) { Pass '0.4' 'store cleared before testing' '' } else { Part '0.4' 'store cleared before testing' ('still ' + $after + ' entries') }
    } else { Part '0.4' 'store cleared before testing' '-SkipBackup' }
}

# =============================================================================
if ($Do -contains '1') {
    Section '1. Process ignore list'

    # ---- 1.1 -------------------------------------------------------------
    $stamp = Get-Date -Format 'HHmmss'
    $m = 'DORUN11-' + $stamp
    $mark = Get-LogCount
    Set-YamlList 'CLIPBOARD_HISTORY_IGNORE_PROCESSES' @('notepad.exe', 'mspaint*.exe')
    $copied = Copy-FromNotepad $m
    Out-Line ('1.1 clipboard after Ctrl+C = "' + $copied + '"')
    Wait-Poll
    $skip = @(Get-SkipLines $mark)
    if ($skip.Count -eq 0) { Part '1.1' 'notepad copy not recorded' ('inconclusive: the Notepad copy never reached DoRun (clipboard="' + $copied.Substring(0,[Math]::Min(30,$copied.Length)) + '")') }
    elseif ((Get-TopText) -eq $m) { Fail '1.1' 'notepad copy not recorded' 'the ignored text is in the history' }
    else { Pass '1.1' 'notepad copy not recorded' $skip[-1] }
    if ($skip.Count -gt 0) { Pass '1.1' 'skip line in log' $skip[-1] } else { Fail '1.1' 'skip line in log' ('none since line ' + $mark) }

    # 1.1 step 5: a normal app is still recorded
    $m2 = 'DORUN-1.1b-' + (Get-Date -Format 'HHmmss')
    $mark = Get-LogCount
    Set-ClipText $m2
    Wait-Poll
    if ((Get-TopText) -eq $m2) { Pass '1.1' 'normal app copy still recorded' 'powershell.exe copy stored' }
    else { Fail '1.1' 'normal app copy still recorded' ('top="' + (Get-TopText) + '"') }
    $skip = @(Get-SkipLines $mark)
    if ($skip.Count -eq 0) { Pass '1.1' 'no skip line for a normal app' '(normal capture writes nothing)' }
    else { Fail '1.1' 'no skip line for a normal app' $skip[-1] }

    # ---- 1.3 -------------------------------------------------------------
    Out-Line ''; Out-Line '--- 1.3 wildcard and case table ---'
    $cases = @(
        @{ P = 'notepad.exe';            E = 'ignored'; N = 'exact file name' },
        @{ P = 'NOTEPAD.EXE';            E = 'ignored'; N = 'upper case pattern' },
        @{ P = 'note*.exe';              E = 'ignored'; N = 'star wildcard' },
        @{ P = 'note?ad.exe';            E = 'ignored'; N = 'question wildcard' },
        @{ P = 'C:\Windows\notepad.exe'; E = 'ignored'; N = 'full path reduced to file name' },
        @{ P = 'notepad';                E = 'kept';    N = 'pattern without .exe must not match' }
    )
    $n = 0
    foreach ($c in $cases) {
        $n++
        $m = 'DORUN13-' + $n + 'x' + (Get-Date -Format 'HHmmss')
        $mark = Get-LogCount
        Set-YamlList 'CLIPBOARD_HISTORY_IGNORE_PROCESSES' @($c.P)
        $copied = Copy-FromNotepad $m
        Wait-Poll
        $skip = @(Get-SkipLines $mark)
        $id = '1.3.' + $n
        if ($skip.Count -eq 0 -and $c.E -eq 'ignored') { Part $id $c.P ('inconclusive: Notepad copy never reached DoRun (clipboard="' + $copied.Substring(0,[Math]::Min(24,$copied.Length)) + '")'); continue }
        if ($c.E -eq 'ignored' -and $skip.Count -gt 0) { Pass $id $c.P ($c.N + ' -> ignored') }
        elseif ($c.E -eq 'kept' -and $skip.Count -eq 0) { Pass $id $c.P ($c.N + ' -> kept (no skip line)') }
        else { Fail $id $c.P ('expected ' + $c.E + '; skip=' + $skip.Count + ' top="' + (Get-TopText) + '"') }
        if ($skip.Count -gt 0) { Out-Line ('      log: ' + $skip[-1]) }
    }

    # ---- 1.3 last row / 1.2 helper: "*" --------------------------------
    Out-Line ''; Out-Line '--- "*" makes every copy log its resolved process name ---'
    $mark = Get-LogCount
    Set-YamlList 'CLIPBOARD_HISTORY_IGNORE_PROCESSES' @('*')
    Set-ClipText ('DORUN-star-' + (Get-Date -Format 'HHmmss'))
    Wait-Poll
    $skip = @(Get-SkipLines $mark)
    if ($skip.Count -gt 0) { Pass '1.3.7' 'pattern "*" ignores everything' $skip[-1] }
    else { Fail '1.3.7' 'pattern "*" ignores everything' 'no skip line' }

    # ---- 1.4 own writes -------------------------------------------------
    Out-Line ''; Out-Line '--- 1.4 own copy bypasses the ignore list ---'
    $host_ = Get-HostHandle
    if ($host_ -ne [IntPtr]::Zero) {
        [void][W.U]::PostMessage($host_, $WM_HOTKEY, [IntPtr]$ID_HK_LAUNCH, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 1200
        [System.Windows.Forms.SendKeys]::SendWait('notepad')
        Start-Sleep -Milliseconds 900
        [System.Windows.Forms.SendKeys]::SendWait('^%c')
        Start-Sleep -Milliseconds 1200
        [System.Windows.Forms.SendKeys]::SendWait('{ESC}')
        Start-Sleep -Milliseconds 500
    } else { Out-Line '1.4 host window not found' }
    Wait-Poll
    $own = @(Get-Entries | Where-Object { ($_ -split "`t")[0] -eq 'text' -and ($_ -split "`t")[1] -match 'notepad' })
    if ($own.Count -gt 0) { Pass '1.4' 'own copy recorded despite "*"' ($own[0] -split "`t")[1] }
    else { Part '1.4' 'own copy recorded despite "*"' 'no entry; COPY_COMMAND_HOTKEY needs a manual check' }

    # ---- 1.2 A / B / B2 ------------------------------------------------
    Out-Line ''; Out-Line '--- 1.2 GetClipboardOwner() timing ---'
    Set-YamlList 'CLIPBOARD_HISTORY_IGNORE_PROCESSES' @('notepad.exe')

    $mark = Get-LogCount
    $m = 'DORUN12A-' + (Get-Date -Format 'HHmmss')
    $copied = Copy-FromNotepad $m
    Wait-Poll
    $skip = @(Get-SkipLines $mark)
    if ($skip.Count -gt 0 -and (Get-TopText) -ne $m) { Pass '1.2-A' 'case A: source alive -> ignored' $skip[-1] }
    else { Fail '1.2-A' 'case A: source alive -> ignored' ('skip=' + $skip.Count + ' top="' + (Get-TopText) + '"') }

    $m = 'DORUN12B-' + (Get-Date -Format 'HHmmss')
    $mark = Get-LogCount
    $shell = New-Object -ComObject WScript.Shell
    $real = Find-NotepadWindow
    if ($null -eq $real) { $real = Start-Process notepad.exe -PassThru ; Start-Sleep -Milliseconds 4000 }
    [void]$shell.AppActivate($real.Id)
    Start-Sleep -Milliseconds 900
    [System.Windows.Forms.SendKeys]::SendWait('^a')
    Start-Sleep -Milliseconds 150
    [System.Windows.Forms.SendKeys]::SendWait('{DEL}')
    Start-Sleep -Milliseconds 200
    [System.Windows.Forms.SendKeys]::SendWait($m)
    Start-Sleep -Milliseconds 400
    [System.Windows.Forms.SendKeys]::SendWait('^a^c')
    Start-Sleep -Milliseconds 40
    Stop-Process -Id $real.Id -Force -ErrorAction SilentlyContinue
    Wait-Poll 2000
    $skip = @(Get-SkipLines $mark)
    if ($skip.Count -gt 0) { Part '1.2-B' 'case B: source closed -> foreground fallback' ('still ignored: ' + $skip[-1]) }
    elseif ((Get-TopText) -eq $m) { Part '1.2-B' 'case B: source closed -> foreground fallback' 'recorded (foreground window used instead of the dead owner)' }
    else { Part '1.2-B' 'case B: source closed -> foreground fallback' ('neither recorded nor skipped; top="' + (Get-TopText) + '"') }

    $m = 'DORUN12B2-' + (Get-Date -Format 'HHmmss')
    $mark = Get-LogCount
    $real = Find-NotepadWindow
    if ($null -eq $real) { $real = Start-Process notepad.exe -PassThru ; Start-Sleep -Milliseconds 4000 }
    [void]$shell.AppActivate($real.Id)
    Start-Sleep -Milliseconds 900
    [System.Windows.Forms.SendKeys]::SendWait('^a')
    Start-Sleep -Milliseconds 150
    [System.Windows.Forms.SendKeys]::SendWait('{DEL}')
    Start-Sleep -Milliseconds 200
    [System.Windows.Forms.SendKeys]::SendWait($m)
    Start-Sleep -Milliseconds 400
    [System.Windows.Forms.SendKeys]::SendWait('^a^c')
    Start-Sleep -Milliseconds 30
    [System.Windows.Forms.SendKeys]::SendWait('%{TAB}')
    Wait-Poll 2000
    $skip = @(Get-SkipLines $mark)
    if ($skip.Count -gt 0) { Part '1.2-B2' 'case B2: copy then switch window' ('ignored (owner still resolved): ' + $skip[-1]) }
    else { Part '1.2-B2' 'case B2: copy then switch window' ('not ignored; owner already invalid at poll time; top="' + (Get-TopText) + '"') }
    Stop-Process -Id $real.Id -Force -ErrorAction SilentlyContinue

    Ask '1.2-C' 'case C: process name behind a UWP app' `
        'app is still running; with the ignore list at "*" copy text in Settings / Photos / Calculator and read the process name in hotkey-debug.log.'

    # ---- 1.5 -------------------------------------------------------------
    Out-Line ''; Out-Line '--- 1.5 source opt-out ---'
    Set-YamlList 'CLIPBOARD_HISTORY_IGNORE_PROCESSES' @()

    $mark = Get-LogCount
    Set-ClipOptOut ('DORUN-optout-' + (Get-Date -Format 'HHmmss'))
    Wait-Poll
    $skip = @(Get-SkipLines $mark)
    if (@($skip | Where-Object { $_ -match 'opted out' }).Count -gt 0) { Pass '1.5' 'opt-out format 1 is honoured' $skip[-1] }
    else { Fail '1.5' 'opt-out format 1 is honoured' ('no opt-out line; skips=' + ($skip -join ' | ')) }

    $mark = Get-LogCount
    Set-ClipOptOutFlag ('DORUN-optout2-' + (Get-Date -Format 'HHmmss'))
    Wait-Poll
    $skip = @(Get-SkipLines $mark)
    if (@($skip | Where-Object { $_ -match 'opted out' }).Count -gt 0) { Pass '1.5' 'opt-out format 2 (CanIncludeInClipboardHistory=0) is honoured' $skip[-1] }
    else { Fail '1.5' 'opt-out format 2 (CanIncludeInClipboardHistory=0) is honoured' ('no opt-out line') }

    Ask '1.5' 'real password manager (KeePassXC / 1Password / Bitwarden)' `
        'copy one password from the real password manager and confirm nothing new appears in the popup.'

    # ---- 1.6 -------------------------------------------------------------
    Out-Line ''; Out-Line '--- 1.6 flooding and recovery ---'
    Set-YamlList 'CLIPBOARD_HISTORY_IGNORE_PROCESSES' @('powershell.exe', 'pwsh.exe')
    $mark = Get-LogCount
    for ($i = 0; $i -lt 10; $i++) { Set-ClipText ('DORUN-1.6-' + $i) ; Start-Sleep -Milliseconds 520 }
    Wait-Poll
    $skip = @(Get-SkipLines $mark)
    if ($skip.Count -eq 10) { Pass '1.6' 'exactly one skip line per copy' ($skip.Count.ToString() + ' skip lines for 10 copies') }
    elseif ($skip.Count -le 10) { Pass '1.6' 'at most one skip line per copy' ($skip.Count.ToString() + ' skip lines for 10 copies') }
    else { Fail '1.6' 'at most one skip line per copy' ($skip.Count.ToString() + ' skip lines for 10 copies (poll retried)') }

    Set-YamlList 'CLIPBOARD_HISTORY_IGNORE_PROCESSES' @()
    $r = 'DORUN-1.6-reset-' + (Get-Date -Format 'HHmmss')
    Set-ClipText $r
    Wait-Poll
    if ((Get-TopText) -eq $r) { Pass '1.6' 'recovers right after an ignored copy' 'next copy recorded' }
    else { Fail '1.6' 'recovers right after an ignored copy' ('top="' + (Get-TopText) + '"') }
}

# =============================================================================
if ($Do -contains '2') {
    Section '2. Rich text -> plain text'

    # ---- 2.2 baseline -----------------------------------------------------
    Set-YamlScalar 'CLIPBOARD_RICH_TEXT_AS_PLAIN' 'false'
    $p = 'DORUN-RTF22-' + (Get-Date -Format 'HHmmss')
    Set-ClipRtf $p (New-TestRtf $p)
    Wait-Poll
    Out-Line ('2.2 top entry: ' + (Get-TopEntry))
    if ((Get-TopKind) -eq 'rtf') { Pass '2.2' 'rich text captured as [RTF]' (Get-TopText) }
    else { Fail '2.2' 'rich text captured as [RTF]' ('kind=' + (Get-TopKind)) }
    $rtf = Get-BlobsExt '.rtf'
    if ($rtf.Count -gt 0) { Pass '3.4-b' 'RTF blob written' (($rtf | ForEach-Object { $_.Name + '=' + $_.Length + 'B' }) -join ', ') }
    else { Fail '3.4-b' 'RTF blob written' 'no .rtf in clipboard-blobs' }

    if (Open-PopupSession) {
        Out-Line ('2.2 rows: ' + ((Get-ListLabels) -join ' || '))
        [void](Press-ListKey $VK_RETURN)
        $f = Get-Formats
        Out-Line ('2.2 Enter formats: ' + ($f -join ', '))
        if ((Has-F $f 'Rich Text Format') -and (Has-F $f 'UnicodeText')) { Pass '2.2' 'Enter keeps RTF + text' ($f -join ', ') }
        else { Fail '2.2' 'Enter keeps RTF + text' ($f -join ', ') }
    } else { Fail '2.2' 'Enter keeps RTF + text' 'popup did not open' }
    Close-PopupSession
    Ask '2.2' 'WordPad shows bold + red after pasting' 'paste the copied [RTF] entry into WordPad.'

    # ---- 2.1 global flatten ----------------------------------------------
    Set-YamlScalar 'CLIPBOARD_RICH_TEXT_AS_PLAIN' 'true'
    $p = 'DORUN-RTF21-' + (Get-Date -Format 'HHmmss')
    Set-ClipRtf $p (New-TestRtf $p)
    Wait-Poll
    if ((Get-TopKind) -eq 'rtf') { Pass '2.1' 'RTF still captured with the switch on' '' }
    else { Part '2.1' 'RTF still captured with the switch on' ('kind=' + (Get-TopKind)) }
    if (Open-PopupSession) {
        [void](Press-ListKey $VK_RETURN)
        $f = Get-Formats
        Out-Line ('2.1 Enter formats: ' + ($f -join ', '))
        if (-not (Has-F $f 'Rich Text Format') -and (Has-F $f 'UnicodeText')) { Pass '2.1' 'switch removes the RTF format' ($f -join ', ') }
        else { Fail '2.1' 'switch removes the RTF format' ($f -join ', ') }
    } else { Fail '2.1' 'switch removes the RTF format' 'popup did not open' }
    Close-PopupSession
    Ask '2.1' 'WordPad shows no formatting' 'paste into WordPad: no bold, no colour.'
    Set-YamlScalar 'CLIPBOARD_RICH_TEXT_AS_PLAIN' 'false'

    # ---- 2.3 ------------------------------------------------------------
    $p = 'DORUN-RTF23a-' + (Get-Date -Format 'HHmmss')
    Set-ClipRtf $p (New-TestRtf $p)
    Wait-Poll
    if (Open-PopupSession) {
        [void](Press-ListKey 0x43 -Ctrl -Shift)
        $f = Get-Formats
        Out-Line ('2.3 Ctrl+Shift+C formats: ' + ($f -join ', '))
        if (-not (Has-F $f 'Rich Text Format') -and (Has-F $f 'UnicodeText')) { Pass '2.3' 'Ctrl+Shift+C copies as plain text' ($f -join ', ') }
        else { Fail '2.3' 'Ctrl+Shift+C copies as plain text' ($f -join ', ') }
    } else { Fail '2.3' 'Ctrl+Shift+C copies as plain text' 'popup did not open' }
    Close-PopupSession

    $p = 'DORUN-RTF23b-' + (Get-Date -Format 'HHmmss')
    Set-ClipRtf $p (New-TestRtf $p)
    Wait-Poll
    if (Open-PopupSession) {
        [void](Press-ListKey 0x54 -Ctrl)
        $f = Get-Formats
        Out-Line ('2.3 Ctrl+T formats: ' + ($f -join ', '))
        if (-not (Has-F $f 'Rich Text Format') -and (Has-F $f 'UnicodeText')) { Pass '2.3' 'Ctrl+T copies as plain text' ($f -join ', ') }
        else { Fail '2.3' 'Ctrl+T copies as plain text' ($f -join ', ') }
    } else { Fail '2.3' 'Ctrl+T copies as plain text' 'popup did not open' }
    Close-PopupSession

    # ---- 2.4 flatten non-text entries -----------------------------------
    Set-ClipImage (New-Bitmap 32 32 'Cyan')
    Wait-Poll
    if (Open-PopupSession) {
        [void](Press-ListKey 0x54 -Ctrl)
        $f = Get-Formats
        Out-Line ('2.4 [IMG] Ctrl+T formats: ' + ($f -join ', '))
        if (-not (Has-F $f 'DeviceIndependentBitmap') -and -not (Has-F $f 'Bitmap') -and (Has-F $f 'UnicodeText')) {
            Pass '2.4' '[IMG] entry flattens to text' (Get-ClipText)
        } else { Fail '2.4' '[IMG] entry flattens to text' ($f -join ', ') }
    } else { Fail '2.4' '[IMG] entry flattens to text' 'popup did not open' }
    Close-PopupSession

    $file24 = Get-TempPath 'dorun-2.4.txt'
    Set-Content -LiteralPath $file24 -Value 'x'
    Set-ClipFiles @($file24)
    Wait-Poll
    if (Open-PopupSession) {
        [void](Press-ListKey 0x54 -Ctrl)
        $f = Get-Formats
        Out-Line ('2.4 [FILE] Ctrl+T formats: ' + ($f -join ', '))
        if (-not (Has-F $f 'FileDrop') -and (Has-F $f 'UnicodeText')) { Pass '2.4' '[FILE] entry flattens to text' (Get-ClipText) }
        else { Fail '2.4' '[FILE] entry flattens to text' ($f -join ', ') }
    } else { Fail '2.4' '[FILE] entry flattens to text' 'popup did not open' }
    Close-PopupSession

    # ---- 2.5 hint line count -------------------------------------------
    Out-Line ''; Out-Line '--- 2.5 hint line count ---'
    if (Open-PopupSession) {
        $hint = Get-HintText
        $hintLines = @($hint -split "`r`n").Count
        Out-Line ('2.5 hint with hotkey (' + $hintLines + ' line(s)): ' + $hint)
        if ($hintLines -eq 3) { Pass '2.5' 'hint has 3 lines with a hotkey' '' } else { Fail '2.5' 'hint has 3 lines with a hotkey' ($hintLines.ToString() + ' line(s)') }
        $r1 = New-Object W.U+RECT
        [void][W.U]::GetWindowRect((Get-PopupHandle), [ref]$r1)
        $h1 = $r1.Bottom - $r1.Top
        Out-Line ('2.5 popup height with hotkey = ' + $h1)
    } else { Fail '2.5' 'hint has 3 lines with a hotkey' 'popup did not open' }
    Close-PopupSession

    Set-YamlScalar 'CLIPBOARD_HOTKEY' 'none'
    Set-ClipText ('DORUN-2.5-none-' + (Get-Date -Format 'HHmmss'))
    Wait-Poll
    $host_ = Get-HostHandle
    # With CLIPBOARD_HOTKEY: none a synthetic WM_HOTKEY still opens the popup,
    # but the modifier-less cycling sequence ends after ~700 ms and then pastes
    # and hides it again, so the layout is sampled inside that window.
    [void][W.U]::PostMessage($host_, $WM_HOTKEY, [IntPtr]$ID_HK_CLIPBOARD, [IntPtr]::Zero)
    $visible = $false
    for ($i = 0; $i -lt 14; $i++) {
        Start-Sleep -Milliseconds 50
        if (Test-PopupOpen) { $visible = $true; break }
    }
    if ($visible) {
        $hint2 = Get-HintText
        $hintLines2 = @($hint2 -split "`r`n").Count
        Out-Line ('2.5 hint without hotkey (' + $hintLines2 + ' line(s)): ' + $hint2)
        $r2 = New-Object W.U+RECT
        [void][W.U]::GetWindowRect((Get-PopupHandle), [ref]$r2)
        $h2 = $r2.Bottom - $r2.Top
        Out-Line ('2.5 popup height without hotkey = ' + $h2)
        if ($hintLines2 -eq 2) { Pass '2.5' 'hint has 2 lines without a hotkey' '' } else { Fail '2.5' 'hint has 2 lines without a hotkey' ($hintLines2.ToString() + ' line(s)') }
        if ($h2 -lt $h1) { Pass '2.5' 'popup shrinks with the shorter hint' ($h1.ToString() + ' -> ' + $h2) }
        else { Fail '2.5' 'popup shrinks with the shorter hint' ($h1.ToString() + ' -> ' + $h2) }
        Close-PopupSession
    } else { Fail '2.5' 'popup shows the 2-line hint without a hotkey' 'popup not visible' }
    Set-YamlScalar 'CLIPBOARD_HOTKEY' ("'" + '!' + [char]96 + "'")
    Out-Line ('2.5 restored ' + (Get-YamlLine 'CLIPBOARD_HOTKEY'))
}

# =============================================================================
if ($Do -contains '3') {
    Section '3. Images, files, rich text'

    # ---- 3.2 image ------------------------------------------------------
    $img = New-Bitmap 64 48 'Orange'
    $g = [System.Drawing.Graphics]::FromImage($img)
    $g.FillEllipse([System.Drawing.Brushes]::Blue, 4, 4, 40, 30)
    $g.Dispose()
    Set-ClipImage $img
    Wait-Poll
    Out-Line ('3.2 top entry: ' + (Get-TopEntry))
    if ((Get-TopKind) -eq 'image') { Pass '3.2' 'image captured as [IMG]' (Get-TopText) }
    else { Fail '3.2' 'image captured as [IMG]' ('kind=' + (Get-TopKind)) }
    $dib = Get-BlobsExt '.dib'
    if ($dib.Count -gt 0) { Pass '3.4-c' 'DIB blob written' (($dib | ForEach-Object { $_.Name + '=' + $_.Length + 'B' }) -join ', ') }
    else { Fail '3.4-c' 'DIB blob written' 'no .dib blob' }

    if (Open-PopupSession) {
        $labels = Get-ListLabels
        Out-Line ('3.2 rows: ' + ($labels -join ' || '))
        Out-Line ('3.2 preview: ' + ((Get-PreviewText) -replace "`r`n", ' | '))
        [void](Press-ListKey $VK_RETURN)
        $f = Get-Formats
        Out-Line ('3.2 Enter formats: ' + ($f -join ', '))
        if ((Has-F $f 'DeviceIndependentBitmap') -or (Has-F $f 'Bitmap')) { Pass '3.2' 'Enter republishes a bitmap' ($f -join ', ') }
        else { Fail '3.2' 'Enter republishes a bitmap' ($f -join ', ') }
        try {
            $back = [System.Windows.Forms.Clipboard]::GetImage()
            if ($null -ne $back) { Pass '3.2' 'clipboard decodes back to an image' ($back.Width.ToString() + 'x' + $back.Height) }
            else { Fail '3.2' 'clipboard decodes back to an image' 'GetImage returned null' }
        } catch { Fail '3.2' 'clipboard decodes back to an image' $_.Exception.Message }
    } else { Fail '3.2' 'Enter republishes a bitmap' 'popup did not open' }
    Close-PopupSession
    Ask '3.2' 'Paint receives the picture and the row shows a thumbnail' 'paste into Paint; check the [IMG] row thumbnail.'

    # ---- 3.4-d blob reuse ----------------------------------------------
    $before = (Get-BlobsExt '.dib').Count
    Set-ClipImage $img
    Wait-Poll
    $after = (Get-BlobsExt '.dib').Count
    if ($after -eq $before) { Pass '3.4-d' 'identical image reuses one blob' ($after.ToString() + ' .dib files') }
    else { Fail '3.4-d' 'identical image reuses one blob' ('before=' + $before + ' after=' + $after) }

    # ---- 3.1 files ------------------------------------------------------
    $d31 = Get-TempPath 'dorun-3.1'
    New-Item -ItemType Directory -Force -Path $d31 | Out-Null
    $paths = @()
    1..3 | ForEach-Object { $pf = Join-Path $d31 ("f$_.txt") ; Set-Content -LiteralPath $pf -Value "file $_" ; $paths += $pf }
    Set-ClipFiles $paths
    Wait-Poll
    Out-Line ('3.1 top entry: ' + (Get-TopEntry))
    if ((Get-TopKind) -eq 'files') { Pass '3.1' 'file copy captured as [FILE]' '' }
    else { Fail '3.1' 'file copy captured as [FILE]' ('kind=' + (Get-TopKind)) }
    $sep = [char]0x1F
    $stored = @((Get-TopField 2) -split $sep | Where-Object { $_ -ne '' })
    Out-Line ('3.1 stored paths (' + $stored.Count + '): ' + ($stored -join ' ; '))
    if ($stored.Count -eq 3) { Pass '3.1' 'all 3 paths stored' '' } else { Fail '3.1' 'all 3 paths stored' ($stored.Count.ToString()) }

    if (Open-PopupSession) {
        [void](Press-ListKey $VK_RETURN)
        $f = Get-Formats
        Out-Line ('3.1 Enter formats: ' + ($f -join ', '))
        if (Has-F $f 'FileDrop') { Pass '3.1' 'CF_HDROP republished' ($f -join ', ') } else { Fail '3.1' 'CF_HDROP republished' ($f -join ', ') }
        if (Has-F $f 'Preferred DropEffect') { Pass '3.1' 'Preferred DropEffect present' '' } else { Part '3.1' 'Preferred DropEffect present' 'absent from the format list' }
        $drop = @([System.Windows.Forms.Clipboard]::GetFileDropList())
        Out-Line ('3.1 clipboard file list (' + $drop.Count + '): ' + ($drop -join ' ; '))
        if ($drop.Count -eq 3) { Pass '3.1' '3 paths survive the round trip' '' } else { Fail '3.1' '3 paths survive the round trip' ($drop.Count.ToString()) }
        $txt = Get-ClipText
        if (@($txt -split "`r`n").Count -eq 3) { Pass '3.1' 'path list also offered as text' '' }
        else { Part '3.1' 'path list also offered as text' ($txt.Length.ToString() + ' chars') }
    } else { Fail '3.1' 'CF_HDROP republished' 'popup did not open' }
    Close-PopupSession
    Ask '3.1' 'Explorer pastes 3 real files' 'Ctrl+V in another folder creates 3 files; Notepad shows 3 path lines.'

    # ---- 3.3 rich text round trip ---------------------------------------
    $p = 'DORUN-RTF33-' + (Get-Date -Format 'HHmmss')
    Set-ClipRtf $p (New-TestRtf $p)
    Wait-Poll
    $rtf = Get-BlobsExt '.rtf'
    Out-Line ('3.3 RTF blob sizes: ' + (($rtf | ForEach-Object { $_.Length }) -join ','))
    if ($rtf.Count -gt 0 -and @($rtf | Where-Object { $_.Length -lt 20 }).Count -eq 0) { Pass '3.4-e' 'RTF blob holds the real byte stream' '' }
    else { Fail '3.4-e' 'RTF blob holds the real byte stream' (($rtf | ForEach-Object { $_.Name + '=' + $_.Length }) -join ',') }
    if (Open-PopupSession) {
        [void](Press-ListKey $VK_RETURN)
        $f = Get-Formats
        if (Has-F $f 'Rich Text Format') { Pass '3.3' 'RTF republished on paste' ($f -join ', ') } else { Fail '3.3' 'RTF republished on paste' ($f -join ', ') }
    } else { Fail '3.3' 'RTF republished on paste' 'popup did not open' }
    Close-PopupSession
    Ask '3.3' 'WordPad keeps the RTF formatting' 'paste the [RTF] entry: bold red run, no garbage.'

    # ---- 3.4-f / g prune and disable -------------------------------------
    Out-Line ''; Out-Line '--- 3.4 prune on delete / clear all ---'
    Set-ClipText ('DORUN-prune-' + (Get-Date -Format 'HHmmss'))
    Wait-Poll
    if (Open-PopupSession) {
        $n0 = (Get-ListLabels).Count
        [void](Press-ListKey $VK_DELETE)
        $n1 = (Get-ListLabels).Count
        Out-Line ('3.4-f Del: rows ' + $n0 + ' -> ' + $n1)
        if (Test-PopupOpen) {
            [void](Press-ListKey $VK_DELETE -Ctrl)
            Out-Line ('3.4-f Ctrl+Del: rows after clear=' + ((Get-ListLabels).Count) + ' blobs=' + ((Get-Blobs).Count))
        }
    } else { Out-Line '3.4-f popup did not open' }
    Close-PopupSession
    Wait-Poll 800
    if ((Get-Blobs).Count -eq 0 -and (Get-Entries).Count -eq 0) { Pass '3.4-f' 'delete + clear all prune every blob' '' }
    elseif ((Get-Blobs).Count -eq 0) { Pass '3.4-f' 'clear all prunes every blob' ('entries=' + (Get-Entries).Count) }
    else { Part '3.4-f' 'clear all prunes every blob' ((Get-Blobs).Name -join ', ') }

    Set-YamlScalar 'CLIPBOARD_HISTORY_ENABLED' 'false'
    Wait-Poll 800
    if ((Get-Blobs).Count -eq 0 -and (Get-Entries).Count -eq 0) { Pass '3.4-g' 'disable clears history and blobs' '' }
    else { Fail '3.4-g' 'disable clears history and blobs' ('blobs=' + (Get-Blobs).Count + ' entries=' + (Get-Entries).Count) }
    Set-YamlScalar 'CLIPBOARD_HISTORY_ENABLED' 'true'

    # ---- 3.5 limits ------------------------------------------------------
    Out-Line ''; Out-Line '--- 3.5 size and count limits ---'
    $huge = ('A' * 600 + 'B' * 520000)
    Set-ClipText $huge
    Wait-Poll 2000
    $len = (Get-TopText).Length
    Out-Line ('3.5-a 520 KB text -> stored length=' + $len + ' kind=' + (Get-TopKind))
    if ($len -lt 525000) { Pass '3.5-a' 'oversized text is not stored verbatim' ('stored=' + $len) }
    else { Fail '3.5-a' 'oversized text is not stored verbatim' ('stored=' + $len) }

    $big = New-Bitmap 2200 2200 'Green'
    Set-ClipImage $big
    Wait-Poll 2600
    if ((Get-DoRunProc) -ne $null) { Pass '3.5-b' 'oversized image does not crash DoRun' 'process alive' } else { Fail '3.5-b' 'oversized image does not crash DoRun' 'process gone' }
    Out-Line ('3.5-b top kind=' + (Get-TopKind) + ' .dib sizes=' + (((Get-BlobsExt '.dib') | ForEach-Object { $_.Length }) -join ','))
    if ((Get-TopKind) -ne 'image') { Pass '3.5-b' 'oversized image is not recorded' ('top kind=' + (Get-TopKind)) }
    else { Part '3.5-b' 'oversized image is not recorded' 'an image entry is on top' }

    $d35 = Get-TempPath 'dorun-3.5-300'
    New-Item -ItemType Directory -Force -Path $d35 | Out-Null
    $p300 = @()
    1..300 | ForEach-Object { $pf = Join-Path $d35 ("f$_.txt") ; Set-Content -LiteralPath $pf -Value 'x' ; $p300 += $pf }
    Set-ClipFiles $p300
    Wait-Poll 2600
    $n = @((Get-TopField 2) -split $sep | Where-Object { $_ -ne '' }).Count
    Out-Line ('3.5-c 300 files -> stored paths=' + $n + ' kind=' + (Get-TopKind))
    if ($n -eq 200) { Pass '3.5-c' 'file list capped at 200' '' }
    elseif ($n -gt 0 -and $n -lt 200) { Part '3.5-c' 'file list capped at 200' ($n.ToString() + ' paths') }
    else { Fail '3.5-c' 'file list capped at 200' ($n.ToString() + ' paths') }
    if ((Get-DoRunProc) -ne $null) { Pass '3.5-c' 'UI stays responsive with 200 file rows' 'process alive' }

    $dib8 = New-Packed8bppDib 64 64
    Set-ClipPackedDib $dib8
    Wait-Poll
    Out-Line ('3.5-d packed 8bpp DIB ' + $dib8.Length + ' B -> ' + (Get-TopEntry))
    if ((Get-TopKind) -eq 'image') { Pass '3.5-d' '8bpp palette image captured' (Get-TopText) }
    else { Fail '3.5-d' '8bpp palette image captured' ('kind=' + (Get-TopKind)) }
    if (Open-PopupSession) {
        Out-Line ('3.5-d row label: ' + ((Get-ListLabels) -join ' || '))
        [void](Press-ListKey $VK_RETURN)
        $f = Get-Formats
        if ((Has-F $f 'DeviceIndependentBitmap') -or (Has-F $f 'Bitmap')) { Pass '3.5-d' '8bpp image republished' ($f -join ', ') }
        else { Fail '3.5-d' '8bpp image republished' ($f -join ', ') }
    } else { Fail '3.5-d' '8bpp image republished' 'popup did not open' }
    Close-PopupSession
    Ask '3.5-d' '8bpp row shows a real thumbnail' 'the 8bpp entry must show a thumbnail, not an empty cell.'

    # ---- 3.4-h legacy migration ------------------------------------------
    Out-Line ''; Out-Line '--- 3.4-h legacy index migration ---'
    $bak = Join-Path $RunDir ("store-backup-legacy-$Stamp")
    New-Item -ItemType Directory -Force -Path $bak | Out-Null
    if (Test-Path $HistoryPath) { Copy-Item $HistoryPath (Join-Path $bak 'clipboard-history.txt') -Force }
    $dp = Get-DoRunProc
    # Cache the path before stopping: process objects lose .Path afterwards.
    $exePath = $dp.Path
    Stop-Process -Id $dp.Id -Force
    for ($i = 0; $i -lt 20; $i++) {
        if (-not (Get-Process DoRun -ErrorAction SilentlyContinue)) { break }
        Start-Sleep -Milliseconds 200
    }
    Start-Sleep -Milliseconds 800
    $legacy = 'legacy one' + "`r`n" + 'legacy two' + "`r`n" + 'legacy three'
    [System.IO.File]::WriteAllText($HistoryPath, $legacy, (New-Object System.Text.UTF8Encoding($false)))
    Out-Line '3.4-h wrote a legacy 3-line index (no magic line)'
    Start-Process -FilePath $exePath | Out-Null
    # Migration happens on the next save after load, so poll until the magic
    # line appears rather than assuming a fixed delay.
    $raw = @()
    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep -Milliseconds 500
        $raw = @(Get-HistRaw)
        if ($raw.Count -gt 0 -and $raw[0] -eq $Magic) { break }
    }
    Out-Line ('3.4-h first line after restart: ' + $(if ($raw.Count -gt 0) { $raw[0] } else { '(empty)' }))
    Out-Line ('3.4-h entries after migration: ' + @(Get-Entries).Count)
    foreach ($e in @(Get-Entries)) { Out-Line ('        ' + $e) }
    if ($raw.Count -gt 0 -and $raw[0] -eq $Magic -and @(Get-Entries).Count -eq 3) { Pass '3.4-h' 'legacy index migrated in place' 'magic rewritten, 3 entries kept' }
    elseif ($raw.Count -gt 0 -and $raw[0] -eq $Magic) { Part '3.4-h' 'legacy index migrated in place' ('entries=' + @(Get-Entries).Count) }
    else { Fail '3.4-h' 'legacy index migrated in place' ('first line=' + $(if ($raw.Count -gt 0) { $raw[0] } else { '' })) }
}

# =============================================================================
if ($Do -contains '4') {
    Section '4. Regressions'

    $m = 'DORUN-4-text-' + (Get-Date -Format 'HHmmss')
    Set-ClipText $m
    Wait-Poll
    if ((Get-TopText) -eq $m) { Pass '4' 'plain text capture' $m } else { Fail '4' 'plain text capture' ('top="' + (Get-TopText) + '"') }
    if (Open-PopupSession) {
        [void](Press-ListKey $VK_RETURN)
        $back = Get-ClipText
        if ($back -eq $m) { Pass '4' 'plain text round trip' 'clipboard text identical' } else { Fail '4' 'plain text round trip' ('got="' + $back + '"') }
    } else { Fail '4' 'plain text round trip' 'popup did not open' }
    Close-PopupSession

    $u = 'DORUN-4-unicode-中文-emoji🚀-tab' + [char]9 + 'end'
    Set-ClipText $u
    Wait-Poll
    Out-Line ('4 unicode stored: ' + (Get-TopEntry))
    if ((Unescape-IndexField (Get-TopText)) -eq $u) { Pass '4' 'unicode/emoji stored in the index' '' } else { Fail '4' 'unicode/emoji stored in the index' ('stored="' + (Unescape-IndexField (Get-TopText)) + '"') }
    if (Open-PopupSession) {
        [void](Press-ListKey $VK_RETURN)
        $back = Get-ClipText
        if ($back -eq $u) { Pass '4' 'unicode round trip' 'identical' } else { Fail '4' 'unicode round trip' ('got="' + $back + '"') }
    }
    Close-PopupSession

    # own copy through the launcher's copy-command hotkey (Ctrl+Alt+C here)
    $host_ = Get-HostHandle
    [void][W.U]::PostMessage($host_, $WM_HOTKEY, [IntPtr]$ID_HK_LAUNCH, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 1200
    [System.Windows.Forms.SendKeys]::SendWait('notepad')
    Start-Sleep -Milliseconds 900
    [System.Windows.Forms.SendKeys]::SendWait('^%c')
    Start-Sleep -Milliseconds 1200
    [System.Windows.Forms.SendKeys]::SendWait('{ESC}')
    Start-Sleep -Milliseconds 400
    Wait-Poll
    $own = @(Get-Entries | Where-Object { ($_ -split "`t")[0] -eq 'text' -and ($_ -split "`t")[1] -match 'notepad' })
    if ($own.Count -gt 0) { Pass '4' 'copy-command hotkey records its own copy' ($own[0] -split "`t")[1] }
    else { Part '4' 'copy-command hotkey records its own copy' 'not observed' }

    Set-YamlScalar 'CLIPBOARD_HISTORY_ENABLED' 'false'
    Set-ClipText ('DORUN-4-disabled-' + (Get-Date -Format 'HHmmss'))
    Wait-Poll
    $vis = Test-PopupOpen
    Out-Line ('4 disabled: entries=' + @(Get-Entries).Count + ' popupVisible=' + $vis)
    if (@(Get-Entries).Count -eq 0 -and -not $vis) { Pass '4' 'feature off: nothing captured, no popup' '' }
    else { Part '4' 'feature off: nothing captured, no popup' ('entries=' + @(Get-Entries).Count) }
    Set-YamlScalar 'CLIPBOARD_HISTORY_ENABLED' 'true'

    Ask '4' 'hotkey cycling / release-to-paste' 'open with the clipboard hotkey, keep the modifiers down, press again to walk down, release to paste.'
    Ask '4' 'popup closes on focus loss (~1s)' 'click another window while the popup is open.'
    Ask '4' '/clipboard opens the popup' 'launcher -> /clipboard -> Enter.'
}

# =============================================================================
if ($Do -contains '5') {
    Section '5. List UI'
    Set-ClipText ('DORUN-5-text-' + (Get-Date -Format 'HHmmss'))
    Wait-Poll
    $p5 = 'DORUN-5-rtf-' + (Get-Date -Format 'HHmmss')
    Set-ClipRtf $p5 (New-TestRtf $p5)
    Wait-Poll
    Set-ClipImage (New-Bitmap 48 32 'Purple')
    Wait-Poll
    $f5 = Get-TempPath 'dorun-5.txt'
    Set-Content -LiteralPath $f5 -Value 'x'
    Set-ClipFiles @($f5)
    Wait-Poll

    if (Open-PopupSession) {
        $labels = Get-ListLabels
        Out-Line ('5 rows (' + $labels.Count + '):')
        foreach ($l in $labels) { Out-Line ('      ' + $l) }
        $badgeList = @('[TEXT]', '[RTF]', '[IMG]', '[FILE]')
        foreach ($badge in $badgeList) {
            # Literal substring test: '[...]' would be a wildcard character class.
            $n = @($labels | Where-Object { $_.Contains($badge) }).Count
            if ($n -gt 0) { Pass '5' ('row badge ' + $badge) ($n.ToString() + ' row(s)') }
            else { Fail '5' ('row badge ' + $badge) 'absent' }
        }
        $imgRow = @($labels | Where-Object { $_.Contains('[IMG]') })
        if ($imgRow.Count -gt 0 -and $imgRow[0] -match 'Image (\d+)x(\d+)') { Pass '5' 'image row shows "Image WxH"' $imgRow[0] }
        else { Fail '5' 'image row shows "Image WxH"' ($imgRow -join ' | ') }
        $fileRow = @($labels | Where-Object { $_.Contains('[FILE]') })
        $list = Get-ListHandle
        $itemH = [int]([W.U]::SendMsgN($list, $LB_GETITEMHEIGHT, [IntPtr]0, [IntPtr]::Zero))
        $count = [int]([W.U]::SendMsgN($list, $LB_GETCOUNT, [IntPtr]::Zero, [IntPtr]::Zero))
        $r = New-Object W.U+RECT
        [void][W.U]::GetWindowRect((Get-PopupHandle), [ref]$r)
        Out-Line ('5 LB_GETITEMHEIGHT=' + $itemH + ' rows=' + $count + ' popup=' + ($r.Right - $r.Left) + 'x' + ($r.Bottom - $r.Top))
        if ($itemH -ge 52) { Pass '5' 'rows are tall enough for thumbnails (>= 52)' ($itemH.ToString()) }
        else { Fail '5' 'rows are tall enough for thumbnails (>= 52)' ($itemH.ToString()) }
        Out-Line ('5 preview pane: ' + ((Get-PreviewText) -replace "`r`n", ' | '))
        Close-PopupSession

        # preview must follow the selection
        if (Open-PopupSession) {
            $first = Get-PreviewText
            [void](Press-ListKey 0x28)          # VK_DOWN
            $second = Get-PreviewText
            Out-Line ('5 preview row1: ' + $first)
            Out-Line ('5 preview row2: ' + $second)
            if ($second -ne $first) { Pass '5' 'preview follows the selection' '' } else { Fail '5' 'preview follows the selection' 'preview text unchanged' }
            Close-PopupSession
        }
    } else { Fail '5' 'popup opens' 'window did not appear' }

    Ask '5' 'image rows taller, all rows equal height' 'no interleaved heights, no gaps or overlaps.'
    Ask '5' 'thumbnails are real pictures and survive scrolling' 'scroll the list: no blank cells, no tearing.'
    Ask '5' 'high DPI 150% / 200%' 'restart DoRun at that scaling and reopen the popup.'
}

# =============================================================================
Section 'Summary'
$verdicts = @($script:Lines | Where-Object { $_ -match 'RESULT \|' })
$p  = @($verdicts | Where-Object { $_ -match '\| PASS ' }).Count
$fl = @($verdicts | Where-Object { $_ -match '\| FAIL ' }).Count
$pa = @($verdicts | Where-Object { $_ -match '\| PARTIAL ' }).Count
Out-Line ('PASS={0} FAIL={1} PARTIAL={2} TOTAL={3}' -f $p, $fl, $pa, $verdicts.Count)
Out-Line 'FAILURES:'
foreach ($v in @($verdicts | Where-Object { $_ -match '\| FAIL ' })) { Out-Line ('  ' + $v) }
Out-Line 'RESTORED CONFIG:'
foreach ($k in @('CLIPBOARD_HISTORY_ENABLED','CLIPBOARD_HOTKEY','CLIPBOARD_RICH_TEXT_AS_PLAIN','CLIPBOARD_HISTORY_IGNORE_PROCESSES')) {
    Out-Line ('  ' + (Get-YamlLine $k))
}
if (Test-Path $LogPath) { Copy-Item $LogPath $LogDumpPath -Force ; Out-Line ('log copied to ' + $LogDumpPath) }
Out-Line ('report: ' + $ReportPath)

