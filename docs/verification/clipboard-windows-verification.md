# DoRun 0.3.1.56 剪贴板历史 — Windows 验证交接文档

生成时间：2026-11-12（Linux 侧静态验证完成，真机行为待验证）
分支：`master` @ `6a16956`，tag `0.3.1.56`
Release：https://github.com/dodorz/dorun/releases/tag/0.3.1.56
（产物 `DoRun-v0.3.1.56-windows-x64.zip` / `-static.zip`）

本文只列**必须在 Windows 上才能确认**的部分。Linux 侧已经做过并通过的验证记在最后一节，供你判断哪些不用重做。

## 快速路线（约 30 分钟，只验"有没有坏"）

按顺序做完这 6 条即可判断版本能不能用；细节分支在后面各节。

| # | 检查 | 通过条件 |
|---|---|---|
| A | 1.1 记事本加入忽略列表后复制 | 历史里**没有**该条，日志有 `ignored process: notepad.exe` |
| B | 1.5 用密码管理器复制密码 | 历史里**没有**该条，日志有 `source opted out` |
| C | 2.1 `CLIPBOARD_RICH_TEXT_AS_PLAIN: true` 后复制 Word 富文本并粘回 | 粘出来**无格式** |
| D | 3.1 复制 3 个文件 → 弹窗粘贴到别的目录 | 真的复制出 3 个文件 |
| E | 3.2 截图 → 弹窗粘贴到画图 | 粘出**图片**，且列表里有**缩略图** |
| F | 4 纯文本复制/粘回、热键循环 | 和旧版一致 |

任一条失败 → 跳到对应小节看详细诊断，并按第 6 节"出问题怎么记录"的格式发我。

## 结果记录表（验完请填这张）

| 节 | 项目 | 结果（通过/失败/部分） | 备注 |
|---|---|---|---|
| 1.1 | 进程忽略基本生效 | | |
| 1.2 | 复制后切窗口 / 源程序关闭 | | |
| 1.2 | UWP 应用取到的进程名 | | 记录日志里的名字 |
| 1.3 | 通配符与大小写 | | |
| 1.4 | 自己的复制不被自己屏蔽 | | |
| 1.5 | 密码管理器 opt-out | | 记录软件名/版本 |
| 2.1 | 富文本压平（全局开关） | | |
| 2.2 | 富文本保留格式（对照） | | |
| 2.3 | Ctrl+Shift+C / Ctrl+T | | 记录哪个键被抢 |
| 2.4 | 图片/文件条目压平 | | |
| 2.5 | 提示行 2/3 行正确 | | |
| 3.1 | 文件粘贴回 Explorer | | |
| 3.2 | 图片粘贴回画图 + 缩略图 | | |
| 3.3 | RTF 粘贴保留格式 | | |
| 3.4 | blob 复用与清理 | | |
| 3.5 | 大文本/大图/200 文件上限 | | |
| 4 | 原有功能回归 | | |
| 5 | 列表 UI 视觉 | | 附截图最佳 |

---

## 0. 验证前的准备

### 0.1 用哪个 exe

| 场景 | 用哪个 |
|---|---|
| 一般功能验证 | `build\StaticRelease\DoRun.exe`（静态 CRT，无 VC++ 依赖） |
| 只想快速跑 | release 里的 `DoRun-v0.3.1.56-windows-x64.zip`（动态 CRT，需要 VC++ 运行库） |

因为要反复改配置，建议用**静态版**，并且从仓库目录（`build.bat StaticRelease x64`）编译，方便随时改代码重编。

### 0.2 配置文件位置（很重要）

`DoRun.yaml` 按这个顺序查找（`src/main.cpp` `FindConfigPath`）：

1. 与 `DoRun.exe` **同目录**的 `DoRun.yaml`
2. `%APPDATA%\DoRun\DoRun.yaml`（Roaming，不是 Local）

两个都可能存在，**同目录优先**。先确认你改的是生效的那一份：

```powershell
'.\DoRun.yaml', "$env:APPDATA\DoRun\DoRun.yaml" |
  ForEach-Object {
    if (Test-Path $_) { Get-Item $_ | Select-Object FullName, LastWriteTime }
  }

# 当前实际生效的那一份：
$exe = Join-Path (Split-Path (Get-Process DoRun).Path -Parent) 'DoRun.yaml'
if (Test-Path $exe) { "生效: $exe (exe 同目录优先)" }
elseif (Test-Path "$env:APPDATA\DoRun\DoRun.yaml") { "生效: $env:APPDATA\DoRun\DoRun.yaml" }
else { "未找到 DoRun.yaml" }
```

改完配置不需要重启 DoRun，它 1 秒轮询一次配置时间戳并自动 reload（也可以 `DoRun.exe /reload`）。

### 0.3 打开调试日志（验证屏蔽功能必需）

屏蔽逻辑只写日志、不弹提示，所以**必须开日志才能看到"跳过"的证据**：

```yaml
HOTKEY_DEBUG_LOG: true
```

日志文件：`%LOCALAPPDATA%\DoRun\hotkey-debug.log`

```powershell
Get-Content "$env:LOCALAPPDATA\DoRun\hotkey-debug.log" -Tail 30 -Wait
```

主要会出现的两行（**只有真的跳过时才写**，正常采集不写日志）：

```
Clipboard capture skipped: the source opted out of clipboard history.
Clipboard capture skipped for ignored process: keepassxc.exe
```

日志判读：

| 日志内容 | 含义 | 结论 |
|---|---|---|
| 完全没有 skip 行，且该条目进了历史 | 进程名没取到或被判为不匹配 | 屏蔽没生效，见 1.2/1.3 |
| 完全没有 skip 行，但该条目**没进**历史 | 可能被 `CLIPBOARD_CAPTURE_*` 开关或体积上限挡掉了 | 看 3.5 和配置 |
| `ignored process: <名字>` 且名字与预期不符 | 说明 owner 解析到了别的进程（如 `ApplicationFrameHost.exe`） | 见 1.2 情况 C |
| `source opted out` | 源程序自己声明不要记录 | 正常，符合预期 |

### 0.4 剪贴板历史数据位置

```
%LOCALAPPDATA%\DoRun\clipboard-history.txt   ← 索引，一行一条（第一行是 magic）
%LOCALAPPDATA%\DoRun\clipboard-blobs\        ← RTF/图片 blob，文件名是内容哈希
```

验证前建议**先备份并清空**，避免旧数据干扰：

```powershell
$store = "$env:LOCALAPPDATA\DoRun"
$stamp = Get-Date -Format 'yyyymmddhhmmss'
foreach ($item in 'clipboard-history.txt', 'clipboard-blobs') {
  $path = Join-Path $store $item
  if (Test-Path $path) { Move-Item $path "$path.$stamp.bak" -Force }
}
```

清理注意：DoRun 运行时内存里的历史会在下次保存时写回磁盘，所以**先退出 DoRun（托盘右键退出或 `/kill`）再移动文件**，或者把 `CLIPBOARD_HISTORY_ENABLED: false` 后 reload 让它自己清空。

### 0.5 现场编译（推荐）

```bat
cd <repo>
build.bat StaticRelease x64
:: 产物：build\StaticRelease\DoRun.exe
```

如果只验证不改代码，直接用 release 的 zip 也可以，但改配置后要能 reload（1 秒自动 watch，或 `DoRun.exe /reload`）。

---

## 1. 【必验】按进程屏蔽剪贴板历史

代码位置：
- `GetProcessImageNameForWindow()` — `src/main.cpp:4364`
- `GetClipboardSourceProcessName()` — `src/main.cpp:4395`
- `MatchesProcessPattern()` — `src/main.cpp:4409`
- `IsClipboardSourceIgnored()` — `src/main.cpp:4443`
- `IsClipboardCaptureExcludedBySource()` — `src/main.cpp:4456`
- 采集 `CaptureClipboardEntry()` — `src/main.cpp:4483`
- 判定点 `PollClipboardForChanges()` — `src/main.cpp:4559`

配置：

```yaml
CLIPBOARD_HISTORY_IGNORE_PROCESSES:
  - "notepad.exe"
  - "mspaint*.exe"
```

### 1.1 基本屏蔽（最关键，决定功能是否真的可用）

| 步骤 | 期望 |
|---|---|
| 1. 开日志，配置里加 `notepad.exe`，reload 配置 | — |
| 2. 打开**记事本**，输入任意文字，`Ctrl+A` `Ctrl+C` | 剪贴板内容正常（能粘到别处） |
| 3. 打开剪贴板热键（默认 `Ctrl+``） | **历史里没有这条记事本内容** |
| 4. 查看日志 | 出现 `Clipboard capture skipped for ignored process: notepad.exe` |
| 5. 打开记事本**以外的**程序（例如浏览器）复制文字 | 历史里**有**这条，日志**无** skip 行 |

如果第 3 步历史里仍然出现，是**主要 bug**，请记录日志原文。

### 1.2 `GetClipboardOwner()` 的真实行为（重点怀疑对象）

这是我最没把握的地方，需要你重点看：

- **情况 A（源程序仍在运行）**：记事本窗口没关，复制后立刻按热键 → 应该被屏蔽。
- **情况 B（源程序已关闭）**：记事本复制后**立即关闭**记事本窗口，再等 1 秒按热键。
  代码逻辑：`GetClipboardOwner()` 在 owner 窗口/进程退出后返回 `NULL`，此时**回退到 `GetForegroundWindow()`**。
  期望：切到别的窗口后再按热键，取的进程名是当前前台窗口（不是记事本），所以**不会被屏蔽** → 这条内容进入历史。
  这不是 bug 而是已知取舍；请确认行为是否与上面一致，并记录实际结果。
- **情况 B2（最容易踩的坑）**：在 A 里复制后**立刻** `Alt+Tab` 到别的窗口（比如浏览器），再等 1 秒按热键。
  因为 `GetClipboardOwner()` 这时仍能取到记事本，结果应该是**被屏蔽**。
  但如果 owner 已失效（情况 B），回退拿到的是浏览器 → **不被屏蔽**，内容进了历史。
  也就是说：**"复制后马上切换窗口" 这个时序会影响屏蔽是否生效**。请按这个顺序专门测一遍并记录实际结果（这条决定了要不要改成 "记录上一次所有者" 的缓存策略）。
- **情况 C（UWP 应用）**：从"设置"/"照片"/"计算器"等 UWP 应用复制 → 记录日志里打印出的进程名（可能是 `ApplicationFrameHost.exe` 而非真实 app）。
  因为匹配只针对 `ApplicationFrameHost.exe`，配置里写 `photos.exe` **不会**命中。请记录实际进程名，我会据此决定是否加 UWP 支持。

> 要观察实际取到的进程名，可以临时把日志打印改成总是输出（当前只在 skip 时输出）。若需要我在下一版加一行 `Clipboard capture from: <name>` 的常规日志，告诉我。
>
> **临时自测办法（不改代码）**：把忽略列表设成 `- "*"`，这样**任何**复制都会被跳过并打印进程名。只要复制一次，日志里就会写出 `ignored process: <真实进程名>`——用它来确认所有场景（UWP、提权程序、各种浏览器）取到的名字。测完记得删掉这条。

### 1.3 通配符与大小写

| 配置 | 复制的程序 | 期望 |
|---|---|---|
| `"notepad.exe"` | 记事本 | 屏蔽 |
| `"NOTEPAD.EXE"` | 记事本 | 屏蔽（大小写不敏感） |
| `"note*.exe"` | 记事本 | 屏蔽 |
| `"note?ad.exe"` | 记事本 | 屏蔽 |
| `"C:\Windows\notepad.exe"`（完整路径） | 记事本 | 屏蔽（配置里的路径会自动降级为文件名 `notepad.exe`） |
| `"notepad"`（无 `.exe`） | 记事本 | **不屏蔽**（进程名带 `.exe`，pattern 少后缀就匹配不到） |
| `"*"` | 任何程序 | 全部屏蔽（用于确认通配符真的生效，也是**打印真实进程名**的最快方式） |

### 1.4 与"本程序自己的复制"的隔离

DoRun 自己写剪贴板（例如在启动器里按 `Ctrl+C` 复制命令行）时会主动清空源进程状态，所以**不应被自己的忽略列表影响**。

| 步骤 | 期望 |
|---|---|
| 配置 `- "*"`（什么都屏蔽） | — |
| 在启动器搜索框选中一条命令，按复制命令行的热键 | **仍然出现在剪贴板历史**（自己的写入不受屏蔽影响） |

如果自己被屏蔽了，说明 `CopyTextToClipboard()`（`src/main.cpp:8211`，状态清理在 8230 行）或 `CopyClipboardEntryToClipboard()`（`8299`）里清状态的地方漏了。

### 1.5 源程序主动 opt-out（不需要配置）

| 步骤 | 期望 |
|---|---|
| 用**真实密码管理器**（KeePassXC / 1Password / Bitwarden）复制一个密码 | 历史里没有；日志出现 `the source opted out of clipboard history.` |

KeePassXC 默认开启了 `ExcludeClipboardContentFromMonitorProcessing`（选项：Tools → Settings → Security → "Clipboard" 相关），所以这是最容易复现的一条。
如果密码管理器的内容**进了历史**，那是安全相关的 bug，请立刻记录（包括用的哪个软件、哪个版本）。

### 1.6 屏蔽时的性能/边界

| 检查 | 期望 |
|---|---|
| 反复复制被屏蔽的内容 10 次 | 日志不刷屏（每次复制只写 1 行；sequence number 已消费，不会每 400ms 重试） |
| 屏蔽内容之后立刻复制正常内容 | 正常内容能进历史（不会因为上一条被跳过而"卡住"） |

---

## 2. 【必验】富文本 → 纯文本

代码位置：
- `CopyClipboardEntryToClipboard()` — `src/main.cpp:8299`（`CLIPBOARD_RICH_TEXT_AS_PLAIN` 分支在 8350 行附近）
- `CopyClipboardEntryAsPlainText()` — `src/main.cpp:8390`
- 按键处理 `ClipboardListProc` — `src/main.cpp:5759`

### 2.1 `CLIPBOARD_RICH_TEXT_AS_PLAIN: true`（全局，持久）

```yaml
CLIPBOARD_RICH_TEXT_AS_PLAIN: true
```

| 步骤 | 期望 |
|---|---|
| 1. 在 Word / Windows Live Writer / WordPad 里选中**带格式**的文字（粗体+颜色），`Ctrl+C` | 历史出现 `[RTF]` 条目 |
| 2. 打开弹窗，选中该 `[RTF]` 条目，`Enter` | 弹窗关闭，剪贴板只有纯文本 |
| 3. 粘到 WordPad 或 Word | **格式丢失**（无粗体、无颜色）；粘到记事本正常 |
| 4. 用 PowerShell 检查剪贴板格式 | `Get-Clipboard -Format Text` 有内容；`(Get-Clipboard).Count` 正常，且没有 RTF 格式（见下方脚本） |

检查"RTF 格式是否真的没发布"（PowerShell 5.1+）：

```powershell
Add-Type -AssemblyName System.Windows.Forms
$d = [System.Windows.Forms.Clipboard]::GetDataObject()
$d.GetFormats()   # 期望：不再出现 "Rich Text Format"
```

### 2.2 `CLIPBOARD_RICH_TEXT_AS_PLAIN: false`（默认，保留格式）

同样的步骤，第 3 步粘到 WordPad 应**保留**粗体/颜色，且 `GetFormats()` 里有 `Rich Text Format`。

### 2.3 弹窗内 `Ctrl+Shift+C` / `Ctrl+T`（单次压平）

`CLIPBOARD_RICH_TEXT_AS_PLAIN: false` 的前提下：

| 步骤 | 期望 |
|---|---|
| `[RTF]` 条目上按 `Ctrl+Shift+C` | 只复制纯文本；粘到 WordPad 无格式 |
| `[RTF]` 条目上按 `Ctrl+T` | 同上 |
| `[RTF]` 条目上按 `Ctrl+C` / `Enter` / 双击 | **保留** RTF 格式（与上面形成对照） |

⚠️ 注意 `Ctrl+Shift+C` 在弹窗里是否会被输入法/其他全局热键抢走——这是要验证的点。若被抢，退而用 `Ctrl+T`。

按键实现细节（便于判断是不是 bug）：`ClipboardListProc` 里只有在 `WM_KEYDOWN` 才能看到按键，`Ctrl+T` 走 `controlDown && wParam == 'T'` 分支；`Ctrl+Shift+C` 复用 `wParam == 'C'` 分支并额外检查 `GetKeyState(VK_SHIFT)`。所以：

- 若 `Ctrl+T` 有效但 `Ctrl+Shift+C` 无效 → Shift 状态读取的时序问题（很少见）
- 若两者都无效 → 弹窗自己的 listbox 没拿到 `WM_KEYDOWN`（可能焦点在别的控件上），记录"按了之后光标是否在列表上"

### 2.4 非文本条目压平

| 条目类型 | 按 `Ctrl+T` 后期望 |
|---|---|
| `[IMG]` | 剪贴板得到一行描述文字（如 `Image 1920x1080 ... / Stored as a raw device-independent bitmap...`），**不是**图片 |
| `[FILE]` | 剪贴板得到路径列表文字，**不是**文件 |

### 2.5 提示行（UI 回归）

打开弹窗看底部提示：

```
[cycling line]              ← 有配置热键时才有
Enter / Ctrl+C copy    Ctrl+Shift+C or Ctrl+T copies as plain text
Del remove    Ctrl+Del clear all    Esc close
```

- 无热键（`CLIPBOARD_HOTKEY: none`）时应该是 **2 行**
- 有热键时应该是 **3 行**
- 行数不对会导致底部文字被裁掉（`ClipboardHintLineCount()` — `src/main.cpp:4992`，`LayoutClipboardWindow()` 用它算高度）

---

## 3. 【必验】上一条提交（0.3.1.55）的图片/文件/RTF 功能

这些在 Linux 上完全无法验证，必须真机确认。代码位置都在 `src/main.cpp`。

### 3.1 文件复制 → 粘贴回 Explorer

| 步骤 | 期望 |
|---|---|
| 1. 资源管理器里选中 3 个文件，`Ctrl+C` | 历史出现 `[FILE]` 条目，显示第一个文件名 + `+2 more` |
| 2. 打开剪贴板弹窗，选中该条目，`Enter` | — |
| 3. 切到另一个文件夹，`Ctrl+V` | **真的复制出 3 个文件**（不是粘贴文字路径） |
| 4. 粘到记事本 | 得到 3 行路径文字（这也是刻意的设计） |

`SetClipboardDropFiles()` — `src/main.cpp:8245`。重点怀疑：
- `DROPFILES.fWide = TRUE` 的 Unicode 布局是否正确（`pFiles` 偏移、双 NUL 结尾）
- `Preferred DropEffect = DROPEFFECT_COPY` 是否让 Explorer 执行"复制"而不是"移动"
- 文件夹（不是文件）是否也能粘贴回去

### 3.2 截图/图片 → 粘贴回画图

| 步骤 | 期望 |
|---|---|
| 1. `Win+Shift+S` 截图（或 `PrintScreen`），或画图选区 `Ctrl+C` | 历史出现 `[IMG]` 条目，带**真实缩略图**、显示 `Image WxH` |
| 2. 弹窗里选中，`Enter` | — |
| 3. 粘到画图 / Word | **图片本身**被粘贴（不是描述文字） |
| 4. 粘到记事本 | 得到描述文字（正常，因为记事本不支持图片） |

相关代码：采集 `CaptureClipboardEntry()` (`4483`)、图片尺寸 `ReadClipboardImageSize()` (`4317`)、缩略图 `CreateBitmapFromClipboardDib()` (`5223`) + `LoadClipboardImageBitmap()` (`5287`，带 12 项 LRU) + `DrawClipboardImageThumbnail()` (`5337`)、回填 `CopyClipboardEntryToClipboard()` (`8299`)。

重点怀疑（按风险排序）：
1. **缩略图是否显示**（空白格子 = `CreateDIBitmap` 失败）。若失败，看缩略图是否对 8bpp/调色板图片或负高度（top-down）DIB 才失败——这两条路径我只在单测里覆盖过，没有真机图形栈。
2. **粘贴回去是否是图**（`SetClipboardFormatData(CF_DIBV5 ...)` 是否被画图接受）。如果画图拿到的是空白，试 `CF_DIB`（代码在 `format == CF_DIBV5` 时会额外放一份 40 字节头的 CF_DIB 兼容版，见 `ConvertClipboardDibToBitmapInfoHeader()` `5135`）。
3. **alpha 通道**：CF_DIBV5 带 alpha，画图可能显示黑底——记录实际效果。

### 3.3 富文本 → 保留格式

| 步骤 | 期望 |
|---|---|
| Word 里复制带格式文字 → 弹窗内 `Enter` → 粘到 WordPad | 粗体/颜色/字体**保留** |

RTF blob 回填时是**原样字节流**（没加 NUL，见 `8299` 里的注释）。若 WordPad 粘出来是乱码或空，就是这里的 bug。

### 3.4 存储/迁移/清理

| 检查 | 期望 |
|---|---|
| `clipboard-history.txt` 第一行 | `DoRunClipboardHistoryV2` |
| 复制 2 张不同图 + 1 个文件后看 `clipboard-blobs\` | 有对应的 `<16位十六进制>.dib` / `.rtf` 文件 |
| **同一张图复制两次** | `clipboard-blobs` 里**只有一个**对应 `.dib`（哈希复用） |
| 删除某条图片条目（`Del`）后看目录 | 该 blob 被删除（`PruneClipboardBlobs()` `3913`） |
| `Ctrl+Del` 清空全部 | `clipboard-blobs` 目录变空 |
| `CLIPBOARD_HISTORY_ENABLED: false` 后 reload | 历史清空且 blobs 被清理 |
| **旧版本迁移**：用 0.3.1.54 复制几条文字 → 换成 0.3.1.56 启动 | 旧文字条目仍存在，文件第一行变成新 magic |

迁移做法（不要两个版本同时跑，会抢单实例）：

```powershell
# 1) 用 0.3.1.54 跑，复制 3 条不同文字，退出
# 2) 备份，然后用 0.3.1.56 启动
Get-Content "$env:LOCALAPPDATA\DoRun\clipboard-history.txt" -TotalCount 3
# 期望：第一行是 DoRunClipboardHistoryV2，后 3 行是 kind<TAB>text...
```

注意迁移只检查**第一行是否等于 magic**：如果旧文件里恰好有一行文字就是 `DoRunClipboardHistoryV2`，会被误判成新格式（可忽略，不会崩）。

### 3.5 体积/边界

| 输入 | 期望 |
|---|---|
| 复制一个 >512KB 的纯文本（例如整个网页 HTML 当文本） | 文本被截断/不记录（`kClipboardMaxEntryLength = 512*1024`，`421`），但若同时带图片则图片仍记录 |
| 复制一个 >16MB 的图片（大截图） | 图片不记录（`kClipboardMaxBlobBytes = 16MB`，`422`），且**不会崩** |
| 资源管理器一次复制 300 个文件 | 只记录前 200 个（`kClipboardMaxFileCount = 200`，`423`），UI 不卡 |
| 复制一张 8bpp 索引色图（老 GIF/BMP） | 缩略图正常（调色板路径，`ClipboardDibHeaderBytes()` `5194`） |

---

## 4. 回归（防止改坏原有功能）

| 检查 | 期望 |
|---|---|
| 纯文本复制 → 历史 → 粘回 | 完全不变（这是最主要的使用路径） |
| 热键循环（`Ctrl+`` 连按选条目、松手粘贴） | 仍然工作 |
| `/clipboard` 从启动器打开弹窗 | 正常 |
| 复制命令行热键 | 正常，且出现在剪贴板历史 |
| `CLIPBOARD_HISTORY_ENABLED: false` | 不采集、热键不注册、弹窗不显示 |
| 弹窗焦点丢失后自动关闭 | 正常（约 600ms） |
| 中文/emoji 文本往返 | 不乱码（索引文件是 UTF-8，字段做了转义） |

---

## 5. 【必验】列表 UI（缩略图 / 徽标 / 预览）视觉回归

代码位置：
- 行标签 `BuildClipboardEntryLabel()` — `src/main.cpp:4672`
- 预览区 `BuildClipboardEntryDescription()` — `src/main.cpp:4706`，`UpdateClipboardPreview()` — `4840`
- 行高 `ClipboardListItemHeight()` — `src/main.cpp:5018`
- 列表刷新 `ReloadClipboardHistory()` — `4860`

| 检查 | 期望 |
|---|---|
| 行首徽标 | `[TEXT]` / `[RTF]` / `[IMG]` / `[FILE]` 四种都能看到 |
| 图片行 | 左侧有**真实缩略图**（不是空白格、不是纯色块），右侧文字是 `Image WxH` |
| 图片行高度 | 比文本行明显更高（`Scale(52)`），且**所有行等高**（不出现高低交错错位） |
| 文件行 | 显示第一个文件名 + `+N more` |
| 混合列表（文本/RTF/图/文件各一条） | 4 行都正确渲染，无重叠、无错位、无残影 |
| 上下滚动 | 缩略图会随滚动正确重绘（LRU 缓存 12 项，`kClipboardThumbnailCacheLimit`） |
| 预览区（图片条目） | `Image WxH (DIB/DIB-V5)` + 一行说明 |
| 预览区（文件条目） | `N files` + 前 6 个完整路径 |
| 预览区（RTF 条目） | `Rich text (RTF)` + 正文前缀 |
| 切换选中项 | 预览区内容立即跟着变，无延迟 |
| 高 DPI（150%/200%） | 缩略图与行高按比例放大，不模糊错位 |

**已知风险点**：如果缩略图是空白，说明 `CreateDIBitmap` 失败。请记录是**所有图片**都空白，还是只有某些（8bpp / top-down 负高度 / DIBV5 with alpha）空白——这两条路径只有单测覆盖。

---

## 6. 出问题怎么记录

请按这个格式记，我据此定位：

```
场景：1.1 基本屏蔽
配置：CLIPBOARD_HISTORY_IGNORE_PROCESSES: ["notepad.exe"]
操作：记事本 Ctrl+C → Ctrl+` 打开弹窗
实际：历史第一条是记事本内容
期望：不应出现
日志：
  <粘贴 hotkey-debug.log 相关几行>
版本：0.3.1.56 / static
```

如果日志里**完全没有** skip 行，说明进程名没取到（`GetProcessImageNameForWindow` 返回空），那是 1.2 的情况 C 或权限问题。请先做这个判别：

1. 把忽略列表临时设成 `- "*"` 再复制一次
2. 看日志里有没有 `ignored process: <名字>`

- **有名字** → 进程名能取到，问题在 pattern 匹配（看 1.3 那张表：是不是少了 `.exe`、是不是 `?`/`*` 写错）
- **还是没名字** → 进程名取不到，请同时贴出：
  - 复制的程序名与版本
  - `Get-Process <名字> | Select-Object Id, ProcessName, Path` 输出
  - 该程序是否是**提权（管理员）**启动的
  - `hotkey-debug.log` 里该时间段的**全部**行（不只是一行）

---

## 7. Linux 侧已完成的验证（不必重做）

这些是静态/单元级的，逻辑本身已验证，真机上只需要确认"调用 Windows API 的那一层"。

1. **完整编译**：`mingw-w64 g++ -std=c++20 -Wall -Wextra` 编译整个 `main.cpp` 通过，无新增告警（仅剩改动前就有的 unused/missing-initializer）。
2. **抽真实函数跑单测**（不是复刻，直接 `#include` 源码片段）：
   - `MatchesProcessPattern`：15 例（大小写、`*`/`?`、前后缀、空 pattern、不匹配）全过
   - YAML：`CLIPBOARD_HISTORY_IGNORE_PROCESSES` 列表（引号/路径/无引号）解析正确；`CLIPBOARD_HISTORY` 前缀键不会误匹配；缺失键返回空数组
   - `examples/config/dorun.example.yaml` 真实解析 → 3 条忽略项 + `richAsPlain=false` 正确
3. **索引格式往返**：`SaveClipboardEntriesToFile` 的行格式 ↔ `LoadClipboardEntriesFromFile` 解析，文件条目与图片条目（含 64 位哈希、尺寸）往返一致。
4. **旧格式迁移**：纯文本旧索引（含 `\t`/`\n`/`\\` 转义）解析正确，迁移后重写为新 magic。
5. **转义**：`EscapeCacheField`/`UnescapeCacheField` 对制表符、换行、回车、反斜杠、中文，以及"反斜杠后面跟字母 t"的字面量（不能被误当成转义）往返一致。
6. **DIB 头解析** `ClipboardDibHeaderBytes`：32/24/8bpp、256/8 色表、`BI_BITFIELDS`、坏 `biSize`、零宽 → 6 例全对。
7. **V5→40 字节重打包** `ConvertClipboardDibToBitmapInfoHeader`：头+mask+像素总长 68/68 正确。
8. **哈希**：5000 个载荷无相邻碰撞；blob 名固定 16 位十六进制。
9. **预览文本截断** `BuildClipboardPreviewText`：短文本/超长(512KB)/纯空白 三种输入都正确且不扫全文。

---

## 8. 如果需要我下一步做的事（按验证结果挑）

- 1.2 情况 C 确认后 → 加 UWP 真实进程名解析（`ApplicationFrameHost` → 子进程）
- 1.1 失败且日志无输出 → 加"常规采集日志"确认取到的进程名
- 3.2 缩略图空白 → 需要真机 DIB 样本，我会加 DIB 解析失败时回退到"仅显示尺寸"并写日志
- 若要按**窗口标题**屏蔽（例如只屏蔽某浏览器的某些页面），需要新配置键，告诉我即可

---

## 附 1：造测试数据（图片/RTF/文件）

```powershell
# 造一张 8bpp 索引色 BMP 并复制到剪贴板（测试调色板路径）
Add-Type -AssemblyName System.Drawing, System.Windows.Forms
$bmp = New-Object System.Drawing.Bitmap 64, 64, ([System.Drawing.Imaging.PixelFormat]::Format8bppIndexed)
$pal = $bmp.Palette
for ($i = 0; $i -lt 256; $i++) { $pal.Entries[$i] = [System.Drawing.Color]::FromArgb(255, $i, 255 - $i, 128) }
$bmp.Palette = $pal
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.Clear([System.Drawing.Color]::Red)
$g.FillEllipse([System.Drawing.Brushes]::Blue, 5, 5, 54, 54)
$g.Dispose()
[System.Windows.Forms.Clipboard]::SetImage($bmp)
"已复制 8bpp 图"

# 造一张大图（>16MB 未压缩，测试体积上限）
$big = New-Object System.Drawing.Bitmap 2200, 2200   # 32bpp ≈ 19MB
$g2 = [System.Drawing.Graphics]::FromImage($big)
$g2.Clear([System.Drawing.Color]::Green); $g2.Dispose()
[System.Windows.Forms.Clipboard]::SetImage($big)
"已复制大图（应超出 16MB 上限）"

# 造 300 个文件，测试 CF_HDROP 上限 200
$dir = "$env:TEMP\ctest"; New-Item -ItemType Directory -Force $dir | Out-Null
1..300 | ForEach-Object { Set-Content "$dir\f$_.txt" "x" }
explorer.exe /select,"$dir\f1.txt"   # 或者手动 Ctrl+A Ctrl+C
```

RTF、文件复制没有 PowerShell 的简单写法，用 Word / WordPad / 资源管理器手动操作即可。

---

## 附：常用 PowerShell 片段

```powershell
# 看当前剪贴板有哪些格式
Add-Type -AssemblyName System.Windows.Forms
[System.Windows.Forms.Clipboard]::GetDataObject().GetFormats()

# 看索引文件
Get-Content "$env:LOCALAPPDATA\DoRun\clipboard-history.txt" -TotalCount 5

# 看 blob 目录
Get-ChildItem "$env:LOCALAPPDATA\DoRun\clipboard-blobs" |
  Select-Object Name, Length, LastWriteTime

# 跟踪日志
Get-Content "$env:LOCALAPPDATA\DoRun\hotkey-debug.log" -Tail 20 -Wait

# 确认 DoRun 在跑（单实例 mutex）
Get-Process DoRun -ErrorAction SilentlyContinue | Select-Object Id, Path, StartTime

# 确认在这份检查通过的配置里，忽略列表真的读进去了
Select-String -Path "$env:APPDATA\DoRun\DoRun.yaml", ".\DoRun.yaml" `
  -Pattern 'CLIPBOARD_' -ErrorAction SilentlyContinue

# 已编译 exe 的版本号（应与 tag 0.3.1.56 一致）
(Get-Item .\build\StaticRelease\DoRun.exe).VersionInfo |
  Select-Object FileVersion, ProductVersion
```

> 最后一条很重要：如果 `FileVersion` 不是 `0.3.1.56`，说明跑的不是这次改的二进制，所有验证结论都作废。
