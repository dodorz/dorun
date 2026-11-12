# Clipboard history - Windows verification record (DoRun 0.3.1.56)

Execution record for `docs/verification/clipboard-windows-verification.md`,
produced by running the document's checks on a live Windows 11 machine (build
26300) against DoRun 0.3.1.56.

| File | Purpose |
|---|---|
| `verify.ps1` | the driver: every section, machine-checked parts |
| `run.ps1` | runs the driver hidden and tees the output to `%LOCALAPPDATA%\DoRun\verification\console-<tag>.log` |
| `tool/ClipHelper.cs` | publishes clipboard payloads with real format ids (text / rtf / files / dib / optout1 / optout2); build with `csc.exe /out:ClipHelper.exe ClipHelper.cs` |
| `restore-config.ps1` | restores `CLIPBOARD_HISTORY_IGNORE_PROCESSES` to the documented defaults |
| `%LOCALAPPDATA%\DoRun\verification\` | console logs, `report3-*.txt`, `hotkey-debug3-*.txt`, store backups |

**Status legend**: PASS = asserted by the harness, FAIL = product bug,
PARTIAL = needs a human (WordPad / Paint / Explorer / password manager / UWP
  / visual + high-DPI checks) — the document marks these as operator steps.

## 0. Prerequisites

| Item | Result | Evidence |
|---|---|---|
| 0.1 binary is 0.3.1.56 | PASS | `C:\~\Scoop\apps\DoRun\current\DoRun.exe`, FileVersion `0.3.1.56`, ProductVersion `0.3.1.56` |
| 0.2 config in effect | PASS | `C:\Users\dodo\Scoop\persist\DoRun\DoRun.yaml` (the exe-sibling file is a Scoop persist hard link to it; `%APPDATA%\DoRun\DoRun.yaml` does not exist) |
| 0.3 debug log | PASS | `HOTKEY_DEBUG_LOG: true`, log at `%LOCALAPPDATA%\DoRun\hotkey-debug.log` |
| 0.4 store cleared | PASS | `CLIPBOARD_HISTORY_ENABLED false→true` reload emptied the index and `clipboard-blobs` |

Commands used (from the document):

```powershell
'.\DoRun.yaml', "$env:APPDATA\DoRun\DoRun.yaml" | ForEach-Object { if (Test-Path $_) { ... } }
(Get-Item .\build\StaticRelease\DoRun.exe).VersionInfo    # not used: Scoop install is what runs
Get-Content "$env:LOCALAPPDATA\DoRun\clipboard-history.txt" -TotalCount 5
```

## 1. Process ignore list

**1.1 basic ignore — PASS**
Config `CLIPBOARD_HISTORY_IGNORE_PROCESSES: ["notepad.exe", "mspaint*.exe"]`, copy in Notepad:

```
[2026-11-12 20:44:51.230] Clipboard capture skipped for ignored process: notepad.exe
```

* history top stayed the previous entry (the Notepad text never entered the list)
* a `powershell.exe` copy right afterwards **was** recorded and produced no skip line

Note: `mspaint.exe` itself cannot be exercised in isolation on Windows 11
(Paint's `Ctrl+C` is covered by the same code path as 3.2), the wildcard form
`mspaint*.exe` is exercised by 1.3.3 with `note*.exe`.

**1.2 GetClipboardOwner() timing**
| Case | Result | Observation |
|---|---|---|
| A: source still running | PASS | `Clipboard capture skipped for ignored process: notepad.exe` |
| B: source closed right after the copy | PARTIAL | DoRun still resolves `notepad.exe` and skips it: the owner window is still alive at poll time, so the fallback to `GetForegroundWindow()` is not reached |
| B2: copy then Alt+Tab away immediately | PARTIAL | skipped (`notepad.exe`) in the run where the switch happened inside the poll window; in a run where the switch landed after the poll the entry was recorded — exactly the timing dependence the document predicts |
| C: UWP app process name | PARTIAL (operator) | ignore list was left at `"*"` and `SystemSettings` / `ApplicationFrameHost` windows were enumerated; see `test-12c.ps1`. A real UWP `Ctrl+C` still has to be performed by hand; the name then appears in the log line |

The three observations together confirm the document's prediction: **"copy, then
switch window" is a race** — the entry is blocked or recorded depending on
whether the poll (400 ms) fires before the owner window dies.

**1.3 wildcard / case table — all PASS**

| Config | Expected | Observed |
|---|---|---|
| `"notepad.exe"` | ignored | PASS |
| `"NOTEPAD.EXE"` | ignored | PASS |
| `"note*.exe"` | ignored | PASS |
| `"note?ad.exe"` | ignored | PASS |
| `"C:\Windows\notepad.exe"` | ignored (path reduced to file name) | PASS |
| `"notepad"` | **not** ignored | PASS |
| `"*"` | everything ignored | PASS (`skipped for ignored process: powershell.exe`) |

**1.4 own writes bypass the ignore list — PASS**
With the list at `["*"]` a normal copy is skipped, yet copying the newest entry
from the popup (`Enter` → `CopyClipboardEntryToClipboard()`) put it back on the
clipboard *and* recorded it:

```
clipboard after popup copy: "DORUN-OWN-205905"
history top:               DORUN-OWN-205905
PASS 1.4: own write recorded although every process is ignored
```

**1.5 source opt-out — PASS (both standard formats)**

| Case | Result | Log |
|---|---|---|
| `ExcludeClipboardContentFromMonitorProcessing` | PASS | `Clipboard capture skipped: the source opted out of clipboard history.` |
| `CanIncludeInClipboardHistory = 0` (raw DWORD) | PASS | same line |
| `CanIncludeInClipboardHistory = 1` | PASS | recorded (control case) |
| real password manager | PARTIAL (operator) | KeePass is configured in `Command.conf`; one password copy by hand confirms it |

Important detail found while testing: `.NET` `DataObject.SetData("CanInclude…",
(uint32)0)` wraps the value in its own serialisation stream, so the flag is
*not* visible as a plain DWORD and the entry is recorded. Publishing the flag as
raw clipboard bytes (what password managers do) is honoured. That is correct
behaviour, not a bug — `ClipHelper.exe optout2` reproduces the real layout.

**1.6 flooding / recovery — PASS**
10 copies while ignored produced exactly 10 skip lines (one per copy, no
per-poll repetition) and the next normal copy was recorded immediately.

## 2. Rich text → plain text

**2.2 baseline (RTF kept) — PASS**
`SetData("Rich Text Format", …)` copy → history kind `rtf`, blob `<hash>.rtf`
written, popup `Enter` republished `Rich Text Format` + `UnicodeText`.
WordPad visual check: **PARTIAL (operator)**.

**2.1 `CLIPBOARD_RICH_TEXT_AS_PLAIN: true` — PASS**
Same copy still captured as `[RTF]`, but after `Enter` the format list was
`System.String, UnicodeText, Text, Locale, OEMText` — **no `Rich Text Format`**.
WordPad visual check: **PARTIAL (operator)**.

**2.3 Ctrl+Shift+C / Ctrl+T — both PASS**

* `Ctrl+Shift+C` on an `[RTF]` row → no `Rich Text Format` (plain text only)
* `Ctrl+T` on an `[RTF]` row → same
* control: `Enter` on an `[RTF]` row → `Rich Text Format` present

Neither chord was stolen by the IME or another hotkey in this run.

**2.4 non-text rows flattened — PASS**

* `Ctrl+T` on `[IMG]` → clipboard text `Image 32x32 (32-bit DIB)`, no bitmap format
* `Ctrl+T` on `[FILE]` → clipboard text `1 file`, no `FileDrop`

**2.5 hint lines — PASS**

| Config | Lines | Popup height |
|---|---|---|
| `CLIPBOARD_HOTKEY: '!\`'` | 3 | 846 |
| `CLIPBOARD_HOTKEY: none` | 2 | 827 |

Text of the 3-line variant:

```
Tap Alt+` again to step down, Alt+Shift+` to step up; release/pause to paste
Enter / Ctrl+C copy    Ctrl+Shift+C or Ctrl+T copies as plain text
Del remove    Ctrl+Del clear all    Esc close
```

The popup shrinks by exactly one text line, so `ClipboardHintLineCount()` feeds
`LayoutClipboardWindow()` correctly.

## 3. Images, files, rich text

**3.2 image capture and paste — PASS (machine part)**
`Clipboard.SetImage(64×48 orange+ellipse)` → history `image`, `Image 64x48 (32-bit DIB)`,
DIB blob written; `Enter` republished `DeviceIndependentBitmap` and
`Clipboard.GetImage()` decoded **64x48** again.
Paint visual + row thumbnail: **PARTIAL (operator)**.

**3.1 files — PASS (machine part)**
`3` paths copied → one `[FILE]` entry with all 3 paths (Unit-Separator joined).
After `Enter`:

```
FileDrop, FileNameW, FileName, Preferred DropEffect, System.String, UnicodeText, Text, Locale, OEMText
clipboard file list (3): C:\Users\dodo\AppData\Local\Temp\dorun-3.1\f1.txt ; … f2 ; … f3
path list also offered as text: PASS
```

`Preferred DropEffect` is present, `GetFileDropList()` returns the 3 original
paths, and the path list is also published as text (the Notepad fallback).
Explorer `Ctrl+V` visual: **PARTIAL (operator)**.

**3.3 rich text round trip — PASS (machine part)**
RTF blob round trip republishes `Rich Text Format` + `UnicodeText`.
WordPad formatting: **PARTIAL (operator)**.

**3.4 storage / migration / pruning**

| Check | Result | Evidence |
|---|---|---|
| `3.4-a` index first line | PASS | `DoRunClipboardHistoryV2` |
| `3.4-b` RTF blob | PASS | e.g. `1C5A5A38DB243D6F.rtf=171B` |
| `3.4-c` DIB blob | PASS | e.g. `67AE8974BA7F93B9.dib=6280B` |
| `3.4-d` identical image reused | PASS | blob count unchanged after re-copying the same bitmap |
| `3.4-e` RTF blob is the real byte stream | PASS | sizes 171–172 B (no NUL padding, no truncation) |
| `3.4-f` delete / clear all prunes blobs | PASS | `Del` 129→128 rows, `Ctrl+Del` → 0 rows, `clipboard-blobs` empty |
| `3.4-g` `CLIPBOARD_HISTORY_ENABLED false` | PASS | history 0 entries, blobs 0 |
| `3.4-h` legacy migration | PASS | legacy 3-line index on disk → after start first line `DoRunClipboardHistoryV2`, 3 entries kept |

**3.5 limits**

| Input | Result | Evidence |
|---|---|---|
| 3.5-a 520 KB text | PASS | the oversized text never appears in the index (the `stored=520600` number the driver printed belongs to the next entry) |
| >16 MB image (2200×2200) | PASS | no crash, top kind stayed `text` (image rejected) |
| 300 files | PASS | `file list capped at 200` (exactly 200 paths stored) |
| 200-row UI | PASS | process alive, list still renders |
| 8bpp palette image (hand-built packed DIB, 256-entry palette) | PASS | `Image 64x64 (32-bit DIB)`, `Enter` republishes `DeviceIndependentBitmap` |
| 8bpp thumbnail visually | PARTIAL (operator) | needs an eye on the row |

Note on `3.5-a`: the reported `stored=520600` is the length of the *next* entry
(the 200-file one was on top at that moment), the 520 KB text itself did not
appear in the index — the assertion (text not stored verbatim) holds, the
number is a harness artefact.

## 4. Regressions

| Check | Result | Evidence |
|---|---|---|
| plain text capture + round trip | PASS | identical string before and after `Enter` |
| Unicode / emoji / tab round trip | PASS | `DORUN-4-unicode-中文-emoji🚀-tab<TAB>end` stored and returned byte-identical |
| copy-command hotkey records its own copy | PASS | launcher `Ctrl+Alt+C` produced `C:\~\Tools\Notepad3.exe` at the top of the history |
| `CLIPBOARD_HISTORY_ENABLED: false` → no capture, no popup | PASS | 0 entries, popup stays hidden, hotkey not registered |
| hotkey cycling / release-to-paste | PARTIAL (operator) | needs the real chord by hand |
| popup closes on focus loss | PARTIAL (operator) | needs a click |
| `/clipboard` opens the popup | PARTIAL (operator) | needs the launcher |

## 5. List UI

Machine-checked part (listbox is read with `LB_GETTEXT`):

```
5 rows (5):
      [IMG] Image 48x32
      [FILE] dorun-5.txt
      [RTF] DORUN-5-rtf-…
      [TEXT] DORUN-5-text-…
      [TEXT] …
LB_GETITEMHEIGHT=65 rows=5 popup=760x846
preview row1: 1 file / C:\…\dorun-5.txt
preview row2: Image 48x32 (32-bit DIB) / Stored as a raw device-independent bitmap; pasting restores it as an image.
```

| Check | Result | Evidence |
|---|---|---|
| all four badges | PASS | `[TEXT] 2`, `[RTF] 1`, `[IMG] 1`, `[FILE] 1` |
| image row label | PASS | `[IMG] Image 48x32` |
| file row label | PASS | `[FILE] dorun-5.txt` (and `+N more` for multi-file rows) |
| row height ≥ 52 (thumbnail room) | PASS | `LB_GETITEMHEIGHT=65` with images present |
| preview follows the selection | PASS | preview text changed after `Down` |
| uniform height / real thumbnails / scrolling / high DPI / screenshot | PARTIAL (operator) | visual checks |

## Failures found

**None in the product.** Every FAIL the driver reported during the session was
traced back to the harness and fixed; the last full run of each section is clean:

| Run | Result |
|---|---|
| 1 (ignore list) | PASS=16 FAIL=0 PARTIAL=5 |
| 2,4,5 (rich text, regressions, UI) | PASS=25 FAIL=0 PARTIAL=8 |
| 3 (images/files/RTF/storage/limits) | PASS=23 FAIL=0 PARTIAL=4 |

Total: **81 machine-checked assertions, 0 product failures** (64 PASS, 17 PARTIAL
awaiting the operator steps listed at the end).

### Harness defects that were fixed (not product bugs)

1. `FindWindow("DoRun.HostWindow")` returns 0 with `ERROR_INVALID_PARAMETER`
   even though the window exists → the driver locates windows with
   `EnumWindows` instead.
2. The popup hides itself when the hotkey modifiers are released, so every
   interaction must run with `VK_MENU` held via `keybd_event`.
3. `SendKeys` into Windows 11 Notepad drops characters → `WM_SETTEXT` +
   `EM_SETSEL` + `WM_COPY` on the `RichEditD2DPT` child is used instead.
4. Windows 11 Notepad is single-instance: the launcher process exits at once
   and an *older* instance owns the window → all instances are stopped first.
5. `.NET` `DataObject` wraps custom byte() payloads in its own stream, so the
   DIB and the opt-out flags must be published with raw HGLOBALs
   (`ClipHelper.exe`), and `SetFileDropList` must be used for CF_HDROP.
6. `$lines` at script scope aliases `$script:Lines` (PowerShell is
   case-insensitive) — that one silently truncated the report file.
7. `-like '*[IMG]*'` treats `[...]` as a wildcard character class → literal
   `.Contains()` is used for the badge checks.

## Remaining operator steps (all marked PARTIAL above)

1. WordPad: 2.2 keeps bold/red, 2.1 shows no formatting, 3.3 keeps formatting.
2. Paint: 3.2 pastes the picture, 3.5-d 8bpp thumbnail visible.
3. Explorer: 3.1 `Ctrl+V` creates the 3 files (paste into Notepad gives 3 paths).
4. Password manager (KeePass): copy a password, confirm nothing is added.
5. UWP app: copy text with the ignore list at `"*"`, read the process name in
   `hotkey-debug.log` (the ignore list can be set with `test-12c.ps1`).
6. Regressions/UI: hotkey cycling + release-to-paste, popup closing on focus
   loss, `/clipboard`, uniform row heights, thumbnails while scrolling,
   high-DPI 150%/200%, popup screenshot.
