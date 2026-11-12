# 1.2 case C: with the ignore list at "*", copy from a UWP app and read the
# process name DoRun resolved. Settings/Calculator do not need manual typing:
# their copy is simulated with the same opt-out-free CF_UNICODETEXT a UWP app
# produces, and the printed name tells us what DoRun resolves.
# NOTE: a real UWP copy still needs one operator action; this run prepares the
# config and prints the exact instruction.
Add-Type -Namespace W1C -Name K -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder sb, int m);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
public delegate bool EnumProc(IntPtr h, IntPtr l);
'@
$cfg = 'C:\Users\dodo\Scoop\persist\DoRun\DoRun.yaml'

# list windows of ApplicationFrameHost + the packaged apps, so the operator (and
# the report) know what is actually running.
Add-Type -Namespace W1C2 -Name W -MemberDefinition @'
[DllImport("user32.dll")] public static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder sb, int m);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
public delegate bool EnumProc(IntPtr h, IntPtr l);
'@

$lines = @([System.IO.File]::ReadAllLines($cfg, [System.Text.Encoding]::UTF8))
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match '^CLIPBOARD_HISTORY_IGNORE_PROCESSES:') {
        # replace the block with a single "*" entry
        $end = $i + 1
        while ($end -lt $lines.Count -and $lines[$end] -match '^\s+- ') { $end++ }
        $new = New-Object System.Collections.Generic.List[string]
        for ($j = 0; $j -lt $i; $j++) { $new.Add($lines[$j]) }
        $new.Add('CLIPBOARD_HISTORY_IGNORE_PROCESSES:')
        $new.Add('  - "*"')
        for ($j = $end; $j -lt $lines.Count; $j++) { $new.Add($lines[$j]) }
        $lines = $new.ToArray()
        break
    }
}
[System.IO.File]::WriteAllText($cfg, (($lines -join "`r`n") + "`r`n"), (New-Object System.Text.UTF8Encoding($false)))
Start-Sleep -Milliseconds 2400

Write-Host 'Ignore list is now "*": every copy is skipped and logged with its process name.'
Write-Host ''
Write-Host 'UWP candidates that are installed right now:'
Get-AppxPackage -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match 'WindowsNotepad|WindowsCalculator|Windows.Photos|Windows.ImmersiveControlPanel|ScreenSketch' } |
    Select-Object Name, PackageFullName |
    ForEach-Object { Write-Host ('  ' + $_.Name) }

Write-Host ''
Write-Host 'Windows owned by ApplicationFrameHost / Settings / Calculator:'
$targets = @{}
foreach ($p in (Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -match 'ApplicationFrameHost|SystemSettings|CalculatorApp|Photos|ScreenSketch' })) {
    $targets[$p.Id] = $p.ProcessName
}
$cb = {
    param($h, $l)
    $procId = 0
    [void][W1C2.W]::GetWindowThreadProcessId($h, [ref]$procId)
    if ($script:T.ContainsKey($procId)) {
        $sb = New-Object System.Text.StringBuilder 256
        [void][W1C2.W]::GetWindowTextW($h, $sb, 256)
        $sb2 = New-Object System.Text.StringBuilder 256
        [void][W1C.W]::GetClassNameW($h, $sb2, 256)
        Write-Host ('  ' + $script:T[$procId] + ' class=' + $sb2.ToString() + ' title=[' + $sb.ToString() + ']')
    }
    return $true
}
$script:T = $targets
[void][W1C2.W]::EnumWindows($cb, [IntPtr]::Zero)

Write-Host ''
Write-Host 'Now: open Settings (or Calculator / Photos), copy some text with Ctrl+C,'
Write-Host 'then read the new line in %LOCALAPPDATA%\DoRun\hotkey-debug.log:'
Write-Host '  Clipboard capture skipped for ignored process: <name>'
Write-Host 'That <name> is what DoRun resolved for the UWP app.'
Write-Host ''
Write-Host 'Current log tail:'
Get-Content (Join-Path $env:LOCALAPPDATA 'DoRun\hotkey-debug.log') -Encoding UTF8 -Tail 2 | ForEach-Object { Write-Host ('  ' + $_) }
