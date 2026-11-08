# DoRun

A lightweight Windows launcher for fast keyboard-driven app launching, custom commands, hotkeys, startup tasks, and scheduled jobs.

## GitHub description

Fast Windows launcher with indexed apps, custom commands, hotkeys, startup tasks, cron-style scheduling, and usage-based ranking.

## What it does

`DoRun` is a native Win32 launcher for Windows. It indexes executables and shortcuts, lets you define your own commands, and opens a compact launcher window from a global hotkey.

Main capabilities:

- Global launcher hotkey with fallback from `RegisterHotKey` to a keyboard hook when needed.
- Indexed app search from configured directories.
- Custom commands from `Command.conf`.
- Built-in slash commands such as `/reload`, `/kill`, `/config`, and `/cmdconfig`.
- Optional launcher-scoped hotkey to copy the selected command line to the clipboard.
- Ditto-like clipboard history: one hotkey steps through recent clipboard text and pastes the selection, with a floating list window (`/clipboard`) for browsing.
- Managed hotkeys that launch specific commands directly.
- `STARTUP` tasks that run when DoRun starts in startup mode.
- Named groups that run via `DoRun.exe -run <group>` (e.g. `-run work`).
- `CRON` tasks for scheduled launches.
- Usage history stored in SQLite with ranking, decay, pruning, recency, and optional fuzzy matching.
- Live config reload support for `DoRun.yaml` and `Command.conf`, with an optional periodic forced reload.

## Built-in commands

### Search-box slash commands

Type these in the launcher search box and press Enter:

| Command | Description |
|---|---|
| `/quit` | Exit DoRun |
| `/reload` | Reload both config files |
| `/kill <name>` | Terminate a process by image name |
| `/view <path>` | Open file with `VIEWER` (falls back to `EDITOR`, then shell) |
| `/reboot` | Restart the computer |
| `/poweroff` | Shut down the computer |
| `/hibernate` | Suspend to disk |
| `/standby` | Sleep |
| `/config` | Open `DoRun.yaml` with `EDITOR` |
| `/cmdconfig` | Open `Command.conf` with `EDITOR` |
| `/clipboard` | Open the clipboard history window |

### Copying a command line

While the launcher is open, a configurable hotkey copies the command line of the currently selected result to the clipboard (runtime variables such as `${CMD}` and `${CWD}` are expanded, and alias arguments typed after the alias name in the search box are kept). The status line briefly shows what was copied.

```yaml
# AutoHotkey-like syntax: ^ = Ctrl, ! = Alt, + = Shift, @ = Win.
# Defaults to `^+c` when unset. Set to '' / 0 / none to disable.
COPY_COMMAND_HOTKEY: '^+c'
```

The hotkey is handled only while the launcher window is focused, so it does not take a system-wide shortcut away from other applications.

### Clipboard history

DoRun can keep a Ditto-like history of clipboard **text**. While it is running, every new clipboard text is appended to the history (newest first), including text copied by DoRun itself.

Press the clipboard hotkey at any time to open a small floating window with the history:

```yaml
CLIPBOARD_HISTORY_ENABLED: true     # master switch for the feature
CLIPBOARD_HOTKEY: '^`'              # AutoHotkey-like syntax, default is Ctrl+`
CLIPBOARD_HISTORY_MAX_ROWS: 500     # how many entries to keep
CLIPBOARD_VISIBLE_ITEM_COUNT: 12    # rows visible in the popup
```

In the popup:

| Key | Action |
|---|---|
| `Up` / `Down` | Move the selection; the full text is previewed below the list |
| `Enter`, `Ctrl+C`, double-click | Copy the selected entry back to the clipboard and close |
| `Delete` | Remove the selected entry |
| `Ctrl+Delete` | Clear the whole history |
| `Esc` | Close without copying |

DoRun can also drive the popup with the clipboard hotkey itself, so the whole cycle stays on one chord:

1. **First press** opens the popup with the newest entry selected.
2. **Further presses**, while the hotkey's modifiers (Ctrl/Alt/Shift/Win) stay held, move the selection down one entry each.
3. **Releasing the modifiers** puts the selected entry on the clipboard and pastes it into the window you came from by injecting `Ctrl+V` there.
4. **Shift+hotkey** moves the selection up instead — with the default hotkey (`Ctrl` plus the backtick key), holding Shift as well walks back towards the newest entry. This variant is only registered when Shift is not already part of the base hotkey.

When the hotkey has no modifiers (for example an `F` key such as `F8`), there is no modifier release that could mark the end of the sequence, so consecutive presses with less than ~0.7 s between them are treated as one sequence, and the paste happens once the presses stop.

Walking the list with the hotkey and with `Up` / `Down` are alternatives: pressing a key or clicking in the list abandons the hotkey sequence, so nothing is pasted behind your back.

The popup closes when it loses focus or when `Esc` is pressed. Entries are stored as plain text in `%LOCALAPPDATA%\DoRun\clipboard-history.txt`; pasting returns focus to the window you came from and the injected `Ctrl+V` lands there. Only text formats are captured — images and files are not part of the history. `/clipboard` opens the same popup from the launcher.

In the list, entries are separated by a thin divider line and the selected entry is drawn with a filled highlight, a left accent bar, a bold font, and its rank (`#1`, `#2`, …) on the right.

### Forcing a console window

Prefix a search with `!` to force the selected item to show its console window for that launch, even when the global default hides it (for example `ALIASES: SHOW_WINDOW: 0`). The marker is stripped before matching, so `!ForwardConeSyncthing` matches and ranks `ForwardConeSyncthing` normally, including its usage history. This only affects launches started from the search box; hotkeys, `STARTUP`, and `CRON` entries keep their configured `show_window` value.

### Command-line arguments

| Argument | Description |
|---|---|
| `/startup`, `-startup`, `--startup` | Run the `STARTUP` group, then keep running. |
| `-run <group>` | Run the named group, then keep running. |
| `/reload`, `-reload`, `--reload` | Ask the running DoRun instance to reload both config files. If no instance is running, start DoRun, perform one reload, and exit. |

`DoRun.exe /reload` is the command-line equivalent of the `/reload` slash command. It is convenient for scripts that edit `DoRun.yaml` or `Command.conf` and want the changes applied immediately.

### Periodic auto reload

DoRun watches both config files and reloads them when their timestamp changes. In addition, the whole configuration can be reloaded on a fixed period, which is useful when files are edited through tools that preserve timestamps or when a reload must happen unconditionally:

```yaml
# Reload both config files every N seconds. 0 (the default) disables it.
CONFIG_AUTO_RELOAD_SECONDS: 300
```

Set it to `0` to turn the periodic reload off. The interval is re-read on every reload, so changing the value takes effect without restarting DoRun. A pending periodic reload is postponed for a moment while the launcher is open and the search box is being typed in.

## Configuration files

DoRun uses two main config files:

- `DoRun.yaml` for launcher behavior, indexing, appearance, history, and the main hotkey.
- `Command.conf` for custom commands, managed hotkeys, scheduled tasks, and startup tasks.

Built-in commands can open both files directly from the launcher.

## `DoRun.yaml`

### Custom variables (`VARS`)

Define reusable variables under `VARS`. Command.conf entries reference them as `${NAME}`.

| Variable | Default | Used by | Description |
|---|---|---|---|
| `EDITOR` | `''` | `builtin:config`, `builtin:cmdconfig` | Path to text editor. Falls back to shell default. |
| `VIEWER` | `''` | `/view <path>` | Path to read-only file viewer. Falls back to `EDITOR`, then shell default. Can reference `${EDITOR}`, e.g. `'${EDITOR} --view'`. |
| `TERMINAL` | `'wt.exe'` | Custom commands | Terminal emulator path. Used in wrapper templates. |
| `CWD_FLAG` | `'-d "${CWD}"'` | Custom commands | Terminal's working-directory argument. `${CWD}` stays as a literal until launch time. |

Optional alias DSL integration uses the `aliasc` compiler:

```yaml
ALIASES:
  FILE: 'C:\~\.config\alias'
  # Optional. Defaults to C:\~\Scoop\apps\aliasc\current\aliasc.exe,
  # then falls back to `scoopx aliasc`.
  COMPILER: 'C:\~\Scoop\apps\aliasc\current\aliasc.exe'
```

When `ALIASES` is configured, DoRun invokes `aliasc` to compile the source for PowerShell, CMD, and POSIX shells. It imports the names reported by `aliasc list --shell powershell` into the launcher; selecting one runs the corresponding generated PowerShell function. The compiler also includes the adjacent `alias.local`, plus a `ShortcutMap.yaml` resolved by aliasc itself: `%USERPROFILE%\.config\ShortcutMap.yaml` when it exists, otherwise a `ShortcutMap.yaml` beside the alias source. Pass a different file with `SHORTCUT_MAP`. Alias items launch through `powershell.exe`, so they show a console window by default; set `SHOW_WINDOW: 0` under `ALIASES` to hide it (value is a `SW_*` constant, default `1`).

`ShortcutMap.yaml` is an alias source in its own right, so `FILE` and `SHORTCUT_MAP` are independent. `aliasc` expands the map even when the configured alias file is missing (it only reports `source ... does not exist` on stderr), and DoRun follows that behaviour: a stale or renamed `FILE` no longer disables the import, and `ALIASES` with only `SHORTCUT_MAP` set is valid. Keep at least one of the two configured; with neither, `ALIASES` stays inert. Changes to `FILE`, `alias.local`, `SHORTCUT_MAP`, and both autodetected `ShortcutMap.yaml` locations invalidate the import cache, so the launcher reimports automatically.

The launcher preview shows each alias's definition body when the installed `aliasc` supports `list --format tsv` (the body is the same single-line form printed by `aliasc`'s `alias-search`). Set `SHOW_DEFINITION: 0` under `ALIASES` to keep the generic `aliasc PowerShell alias: <name>` text instead. Bodies are shown verbatim, so they may expose secrets or private paths on shared screens.

Example:

```yaml
VARS:
  EDITOR: 'code.cmd --wait'
  VIEWER: '${EDITOR} --view'
  TERMINAL: 'wt.exe'
  CWD_FLAG: '-d "${CWD}"'
ALIASES:
  FILE: 'C:\~\.config\alias'
  COMPILER: 'C:\~\Scoop\apps\aliasc\current\aliasc.exe'
  # Optional. 0 = SW_HIDE (no console window for alias items), 1 = SW_SHOWNORMAL.
  # SHOW_WINDOW: 0
  # Optional. Show each alias definition body in the preview (default 1).
  # SHOW_DEFINITION: 0
  # Optional. Override the ShortcutMap.yaml aliasc reads (aliasc defaults to
  # %USERPROFILE%\.config\ShortcutMap.yaml, then the source's directory).
  # FILE and SHORTCUT_MAP are independent; either one alone is enough.
  # SHORTCUT_MAP: 'C:\~\.config\ShortcutMap.yaml'
  # Or use the fallback command:
  # COMPILER: 'scoopx aliasc'
  # TERMINAL: 'pwsh.exe'
  # CWD_FLAG: '-WorkingDirectory "${CWD}"'
```

### Runtime variables

These expand at **launch time** (after config-load-time `${VARS}` expansion):

| Variable | Description |
|---|---|
| `${CMD}` | The item's full `commandLine` |
| `${CWD}` | The item's `workingDirectory` |
| `${CWD_FLAG}` | From `VARS.CWD_FLAG`, cascades `${CWD}` through load→runtime |

Use in wrapper templates in Command.conf:

```
term_wrapper:"${TERMINAL}" ${CWD_FLAG} cmd /k "${CMD}"
```

### Supported settings

- `HOTKEY_MODIFIERS`
- `HOTKEY_VK`
- `HOTKEY_DEBUG_LOG`
- `COPY_COMMAND_HOTKEY`
- `CONFIG_AUTO_RELOAD_SECONDS`
- `CLIPBOARD_HISTORY_ENABLED`
- `CLIPBOARD_HOTKEY`
- `CLIPBOARD_HISTORY_MAX_ROWS`
- `CLIPBOARD_VISIBLE_ITEM_COUNT`
- `HISTORY_ENABLED`
- `HISTORY_DB_PATH`
- `HISTORY_RANK_SUM_LIMIT`
- `HISTORY_DECAY_FACTOR`
- `HISTORY_PRUNE_BELOW`
- `HISTORY_MAX_ROWS`
- `HISTORY_RECENCY_ENABLED`
- `HISTORY_FUZZY_ENABLED`
- `LAUNCHER_CORNER_RADIUS`
- `LAUNCHER_OPACITY`
- `LAUNCHER_VISIBLE_ITEM_COUNT`
- `LAUNCHER_SEARCH_FONT_FAMILY`
- `LAUNCHER_RESULT_FONT_FAMILY`
- `LAUNCHER_SEARCH_FONT_SIZE`
- `LAUNCHER_RESULT_FONT_SIZE`
- `LAUNCHER_BG_COLOR`
- `LAUNCHER_BORDER_COLOR`
- `LAUNCHER_TEXT_COLOR`
- `LAUNCHER_SUBTLE_TEXT_COLOR`
- `LAUNCHER_ACCENT_COLOR`
- `LAUNCHER_SEARCH_BG_COLOR`

Color settings use `#RRGGBB` or `RRGGBB`.
- `INDEX_RECURSIVE`
- `INDEX_INCLUDE_PATHEXT`
- `INDEX_EXTENSIONS`
- `DIR` blocks with:
  - `PATH`
  - `INDEX_RECURSIVE`
  - `INDEX_INCLUDE_PATHEXT`
  - `INDEX_EXTENSIONS`
  - `INDEX_EXCLUDE_DIRS`

If no scan directories are configured, DoRun defaults to indexing:

- `C:\Windows\System32`
- `C:\Program Files`
- `C:\Program Files (x86)`

Default indexed file types are `.exe` and `.lnk`, with optional `PATHEXT` support.

## `Command.conf`

`Command.conf` supports several command blocks:

- `STARTUP` — run with `-startup` or `-run STARTUP`
- `CRON`
- `HOTKEY` or `HOTKEYS`
- Named groups (e.g. `WORK{...}`) — run with `-run WORK`

Commands support launch options such as:

- working directory
- show mode
- priority class
- running-process policy
- inline batch scripts

Running-process policies include:

- launch normally
- skip when already running
- restart existing process
- queue launches
- queue once

## Build

This project targets Windows and builds with MSBuild.

Local build:

```bat
build.bat
```

Specific configuration examples:

```bat
build.bat Release x64
build.bat Debug x64
build.bat StaticRelease x64
```

The build script:

- locates `MSBuild.exe`
- stops a running `DoRun.exe` before building
- builds one or more configurations from `DoRun.sln`

## GitHub Actions

The GitHub Actions build workflow:

- supports manual runs via `workflow_dispatch`
- accepts version tags with or without a leading `v`
- only auto-builds pushed version tags when `major.minor.patch` changes
- skips auto-builds for build-only tag bumps
- publishes a GitHub Release for manual versioned runs and for pushed tags whose `major.minor.patch` changed
- packages artifacts as `DoRun-<version>-windows-x64.zip`

## Versioning

DoRun uses a four-part version number:

```text
major.minor.patch.build
```

Release tags use the `v<version>` format, for example:

```text
v0.2.1.27
```

See `VERSIONING.md` for the project rules.

## Notes

This is a Windows-specific native launcher focused on keyboard-first workflows and local automation.
