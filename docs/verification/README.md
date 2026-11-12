# Clipboard history verification

Materials for verifying DoRun's clipboard history on Windows. The checklist
itself lives in [`clipboard-windows-verification.md`](clipboard-windows-verification.md);
what has actually been measured so far is in
[`clipboard-windows-verification-RESULTS.md`](clipboard-windows-verification-RESULTS.md).

## Running the automated part

```powershell
# from the repository root, hidden run with a console log
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File docs\verification\run.ps1 -Sections 0,1,2,3,4,5 -Tag full

# or interactively, so the operator questions can be answered
powershell.exe -STA -NoProfile -ExecutionPolicy Bypass `
  -File docs\verification\verify.ps1
```

`-STA` is required for `System.Windows.Forms.Clipboard`; `run.ps1` always runs
with `-NoPrompts`, which turns every operator step into a `PARTIAL` verdict.

Output goes to `%LOCALAPPDATA%\DoRun\verification\`:

| File | Contents |
|---|---|
| `console-<tag>.log` | full console output of a run |
| `report3-<stamp>.txt` | the same `RESULT | id | item | PASS/FAIL/PARTIAL | note` lines |
| `hotkey-debug3-<stamp>.txt` | snapshot of `%LOCALAPPDATA%\DoRun\hotkey-debug.log` |
| `store-backup*-<stamp>\` | clipboard store backs before a run rewrote it |

The script rewrites `CLIPBOARD_HISTORY_IGNORE_PROCESSES`,
`CLIPBOARD_RICH_TEXT_AS_PLAIN` and `CLIPBOARD_HISTORY_ENABLED` while it runs;
`restore-config.ps1` puts the ignore list back to the documented defaults.

## Prerequisites

* DoRun 0.3.1.56 must be the running binary — the script aborts otherwise, since
  the document voids every conclusion for another build.
* `HOTKEY_DEBUG_LOG: true` (the script turns it on if it is off).
* The config that is actually in effect is `%APPDATA%\DoRun\DoRun.yaml` or the
  file next to the exe (Scoop installs hard-link the exe-sibling file to
  `persist\DoRun\DoRun.yaml`).

## Clipboard helper

`System.Windows.Forms` cannot publish a DIB, a CF_HDROP or an opt-out flag the
way Explorer / Paint / a password manager does: it wraps custom `byte()` payloads
in its own serialisation stream. `tool/ClipHelper.cs` writes the real clipboard
formats:

```bat
csc.exe /nologo /out:ClipHelper.exe ClipHelper.cs
ClipHelper.exe text "hello"
ClipHelper.exe rtf "plain fallback" payload.rtf
ClipHelper.exe files C:\a.txt C:\b.txt
ClipHelper.exe dib packed-8bpp.dib
ClipHelper.exe optout1 "secret"
ClipHelper.exe optout2 "secret"
ClipHelper.exe formats
```
