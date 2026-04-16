使用 Win32 API 开发一个名为DoRun的简化版类 Raycast 启动器，仅实现应用启动功能，不包含扩展和 AI 功能。

# 核心功能需求

1. **技术栈**：使用C/C++语言，Win32 API开发，MS VS build Tools编译，尽量不引入额外依赖。但要尽可能支持Windows高分屏等新特性
2. **全局热键**：可自定义热键，默认Alt+Enter唤起启动器窗口（全局）；Ctrl+Q退出程序（启动器窗口中）。配置对话框中提供自定义
3. **应用搜索**：支持搜索自定义目录下的可执行文件（默认包括$PATHEXT中的全部扩展名）和快捷方式（.lnk）
4. **自定义命令**：支持配置自定义命令及其执行参数
5. **键盘导航**：
   - 上下键选择项目
   - 回车键启动选中项
   - ESC 键隐藏窗口
6. **实时过滤**：输入关键词实时过滤显示结果
7. **鼠标点击**：支持鼠标直接点击启动选中项

# 配置文件格式

## dorun.toml - 扫描目录配置

```
DIR = [
    "C:\Windows\System32",
    "C:\Program Files"
]
```

程序会扫描该目录下的所有 .exe 和 .lnk 文件。

## command.conf - 自定义命令配置

格式：`命令名:命令行:运行路径:窗口状态:进程优先级`

参数说明：
- **命令名**：显示在启动器中的名称
- **命令行**：要执行的完整命令（支持带引号的路径）
- **运行路径**：工作目录（可为空）
- **窗口状态**(默认：SW_SHOWNORMAL)：
  - 0 = SW_HIDE（隐藏）
  - 1 = SW_SHOWNORMAL（正常显示）
  - 3 = SW_MAXIMIZE（最大化）
  - 7 = SW_SHOWMINNOACTIVE（最小化）
  - 默认：SW_SHOWNORMAL
- **进程优先级**(默认：NORMAL_PRIORITY_CLASS)：
  - 32 = NORMAL_PRIORITY_CLASS（普通）
  - 64 = IDLE_PRIORITY_CLASS（低）
  - 128 = HIGH_PRIORITY_CLASS（高）
  - 默认：NORMAL_PRIORITY_CLASS

示例：
```
edt:C:\Tool\Notepad3.exe
cmd:"cmd.exe /s /k C:/Tool/Scoop/apps/clink/current/clink_x64.exe inject --quiet"
calc:"C:\Program Files\Calculator\calc.exe"::1:32
```

# 实现要点

## 窗口创建
- 使用 `WS_POPUP | WS_BORDER` 创建无标题栏的弹窗
- `WS_EX_TOPMOST | WS_EX_TOOLWINDOW` 保持窗口置顶且不显示在任务栏
- 窗口居中显示在屏幕上方 1/4 位置

## 热键注册
- 使用 `RegisterHotKey` 注册热键
- 在 `WM_HOTKEY` 消息中显示窗口并设置焦点到搜索框

## 搜索过滤
- 监听搜索框的 `EN_CHANGE` 事件
- 使用不区分大小写的字符串匹配（`stristr` 函数）
- 实时更新过滤结果并重绘窗口

## 启动应用
- 支持工作目录、窗口状态、进程优先级控制

## 键盘处理
- 在 `WM_KEYDOWN` 中处理：
  - VK_UP / VK_DOWN：移动选择索引
  - VK_RETURN：启动选中项
  - VK_ESCAPE：隐藏窗口

## 绘制列表
- 默认显示 8 项
- 选中项使用高亮背景色
- 支持HiDPI

# 编译

使用MSBuild从命令行编译

# 版本管理

版本号格式：`MAJOR.MINOR.PATCH.BUILD`
- 每次提交：BUILD 自动 +1
- feat/refactor/重大 fix：递增 PATCH+1