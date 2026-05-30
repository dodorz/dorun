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
- Managed hotkeys that launch specific commands directly.
- `STARTUP` tasks that run when DoRun starts in startup mode.
- `CRON` tasks for scheduled launches.
- Usage history stored in SQLite with ranking, decay, pruning, recency, and optional fuzzy matching.
- Live config reload support for `DoRun.yaml` and `Command.conf`.

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

Example:

```yaml
VARS:
  EDITOR: 'code.cmd --wait'
  VIEWER: '${EDITOR} --view'
  TERMINAL: 'wt.exe'
  CWD_FLAG: '-d "${CWD}"'
  # Switch to PowerShell:
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

- `STARTUP`
- `CRON`
- `HOTKEY` or `HOTKEYS`

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
