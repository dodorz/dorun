# 1.4 with a real copied entry: prime the clipboard while the ignore list is
# empty, then block everything ("*") and copy from the popup. DoRun's own write
# must still be recorded (CopyClipboardEntryToClipboard clears the source state).
Add-Type -Namespace W14B -Name K -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder sb, int m);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint f, UIntPtr e);
public delegate bool EnumProc(IntPtr h, IntPtr l);
'@

$cfg = 'C:\Users\dodo\Scoop\persist\DoRun\DoRun.yaml'
$hist = Join-Path $env:LOCALAPPDATA 'DoRun\clipboard-history.txt'
$helper = 'C:\~\Sync\Doc\dorun\verify-run\tool\ClipHelper.exe'

function Put-List([string[]]$v) {
    $lines = @([System.IO.File]::ReadAllLines($cfg, [System.Text.Encoding]::UTF8))
    $idx = -1
    for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -match '^CLIPBOARD_HISTORY_IGNORE_PROCESSES:') { $idx = $i; break } }
    $blk = @('CLIPBOARD_HISTORY_IGNORE_PROCESSES:')
    foreach ($x in $v) { $blk += '  - "' + $x + '"' }
    if ($idx -lt 0) { $lines = @($lines) + $blk }
    else {
        $e = $idx + 1
        while ($e -lt $lines.Count -and $lines[$e] -match '^\s+- ') { $e++ }
        $n = New-Object System.Collections.Generic.List[string]
        for ($i = 0; $i -lt $idx; $i++) { $n.Add($lines[$i]) }
        foreach ($b in $blk) { $n.Add($b) }
        for ($i = $e; $i -lt $lines.Count; $i++) { $n.Add($lines[$i]) }
        $lines = $n.ToArray()
    }
    [System.IO.File]::WriteAllText($cfg, (($lines -join "`r`n") + "`r`n"), (New-Object System.Text.UTF8Encoding($false)))
    Start-Sleep -Milliseconds 2400
}
function Get-Host {
    $script:H = [IntPtr]::Zero
    $cb = { param($x, $l) $sb = New-Object System.Text.StringBuilder 256 ; [void][W14B.K]::GetClassNameW($x, $sb, 256) ; if ($sb.ToString() -eq 'DoRun.HostWindow') { $script:H = $x } ; return $true }
    [void][W14B.K]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:H
}
function Get-Popup {
    $script:P = [IntPtr]::Zero
    $cb = { param($x, $l) $sb = New-Object System.Text.StringBuilder 256 ; [void][W14B.K]::GetClassNameW($x, $sb, 256) ; if ($sb.ToString() -eq 'DoRun.ClipboardWindow') { $script:P = $x } ; return $true }
    [void][W14B.K]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:P
}
function Get-List {
    $script:L = [IntPtr]::Zero
    $cb = { param($x, $l) $sb = New-Object System.Text.StringBuilder 256 ; [void][W14B.K]::GetClassNameW($x, $sb, 256) ; if ($sb.ToString() -eq 'ListBox') { $script:L = $x } ; return $true }
    [void][W14B.K]::EnumChildWindows((Get-Popup), $cb, [IntPtr]::Zero)
    return $script:L
}
function TopText { $a = @(Get-Content $hist -Encoding UTF8) ; if ($a.Count -lt 2) { return '' } ; return ($a[1] -split "`t")[1] }

# 1) prime with an entry that DoRun records normally
Put-List @()
$marker = 'DORUN-OWN-' + (Get-Date -Format 'HHmmss')
& $helper text $marker
Start-Sleep -Milliseconds 1800
Write-Host ('primed top: ' + (TopText))

# 2) block every process, then copy the newest entry from the popup
Put-List @('*')
$host_ = Get-Host
[W14B.K]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 200
[void][W14B.K]::PostMessage($host_, 0x0312, [IntPtr]40002, [IntPtr]::Zero)
Start-Sleep -Milliseconds 900
$list = Get-List
Write-Host ('popup visible=' + [W14B.K]::IsWindowVisible((Get-Popup)))
[void][W14B.K]::PostMessage($list, 0x0100, [IntPtr]13, [IntPtr]::Zero)    # Enter -> copy newest entry
Start-Sleep -Milliseconds 1200
[W14B.K]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 400
[void][W14B.K]::PostMessage((Get-Popup), 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
Start-Sleep -Milliseconds 600

Add-Type -AssemblyName System.Windows.Forms
$clip = [System.Windows.Forms.Clipboard]::GetText()
Write-Host ('clipboard after popup copy: "' + $clip + '"')
Write-Host ('history top: ' + (TopText))
if ($clip -eq $marker -and (TopText) -eq $marker) {
    Write-Host 'PASS 1.4: own write recorded although every process is ignored'
} else {
    Write-Host 'FAIL 1.4'
}

Put-List @()
