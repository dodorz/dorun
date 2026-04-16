# AGENTS

## Batch Files

- All `.bat` files must use CRLF line endings. LF-only batch files are treated as broken because they may not execute correctly under `cmd.exe`.

## Config Examples

- Keep example configuration files under `examples\config\`.
- The project must provide and maintain at least these example files:
  - `examples\config\DoRun.example.toml`
  - `examples\config\Command.example.conf`
- When adding or changing configuration fields, commands, hotkey behavior, or file formats, you MUST update the corresponding example files in the same task.
- Example files should stay directly usable after copy/rename, with comments and representative values.

## Version Number

```
MAJOR.MINOR.PATCH.BUILD
```
- 每次提交: BUILD自动+1
- 重大重构或用户指定版本号
- feat/refactor/重大fix: 递增PATCH+1

## Git Commit

```
<类型>(<范围>): <描述>
```
- 类型: feat, fix, docs, style, refactor, perf, test, chore
- 描述用简短英文
- 版本号管理为每次提交后打 tag

## Code Build Requirement

**IMPORTANT**: After completing code based on prompts, you MUST:

1. **立即进行 Build** - 运行相应的编译命令验证代码
2. **自动修复错误** - 根据编译器输出的错误信息，自动修改代码
3. **消除所有警告** - 修复所有警告 (Warning)，直至零警告
4. **迭代验证** - 重复 build-修复循环，直至完全没有错误和警告

### Build 流程

```
写代码 → Build → 有错误/警告? → 是 → 修复 → 重新 Build
                          ↓ 否
                        完成
```

### 注意事项

- 不要等待用户反馈编译结果
- 主动解决所有编译问题
- 确保代码在提交前是可编译的干净状态
