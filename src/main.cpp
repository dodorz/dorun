#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <imm.h>
#include <powrprof.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <winsqlite/winsqlite3.h>

#include <algorithm>
#include <cstdlib>
#include <array>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "resource.h"

namespace {

struct HotkeyConfig {
    UINT modifiers = MOD_ALT;
    UINT vk = VK_SPACE;
};

struct HistoryConfig {
    bool enabled = true;
    std::wstring databasePath;
    double rankSumLimit = 5000.0;
    double decayFactor = 0.9;
    double pruneBelow = 1.0;
    int maxRows = 5000;
    bool recencyEnabled = true;
    bool fuzzyEnabled = true;
};

struct LauncherAppearanceConfig {
    int cornerRadius = 18;
    int opacity = 245;
    int visibleItemCount = 8;
    std::wstring searchFontFamily;
    std::wstring resultFontFamily;
    int searchFontPointSize = 0;
    int resultFontPointSize = 0;
};

struct EditorConfig {
    std::wstring commandLine;
};

struct HistoryInfo {
    double rank = 0.0;
    int runCount = 0;
    std::wstring lastRunUtc;
};

enum class ItemSourceKind : int {
    ScannedFile = 0,
    CommandConf = 1,
    Synthetic = 2,
};

enum class RunningProcessPolicy {
    Launch,
    Skip,
    Restart,
    Queue,
};

struct LaunchItem {
    std::wstring name;
    std::wstring commandLine;
    std::wstring description;
    std::wstring inlineBatchScript;
    std::wstring workingDirectory;
    std::wstring normalizedName;
    std::wstring normalizedCommandLine;
    std::wstring normalizedCommandBaseName;
    int showCommand = SW_SHOWNORMAL;
    DWORD priorityClass = NORMAL_PRIORITY_CLASS;
    RunningProcessPolicy runningProcessPolicy = RunningProcessPolicy::Launch;
    ItemSourceKind sourceKind = ItemSourceKind::ScannedFile;
    std::wstring itemKey;
};

struct CronFieldSpec {
    bool wildcard = false;
    std::vector<bool> allowed;
};

struct CronTask {
    CronFieldSpec minute;
    CronFieldSpec hour;
    CronFieldSpec dayOfMonth;
    CronFieldSpec month;
    CronFieldSpec weekday;
    LaunchItem item;
    std::wstring lastRunMinuteKey;
    HANDLE runningProcess = nullptr;
    int queuedLaunchCount = 0;
};

struct SearchQueryState {
    std::wstring rawQuery;
    std::wstring normalizedQuery;
    std::vector<std::wstring> normalizedTokens;
    std::vector<size_t> matchedItemIndices;
    bool builtinQuery = false;
    bool valid = false;
};

struct BuiltinCommandDefinition {
    const wchar_t* name;
    const wchar_t* description;
    const wchar_t* token;
};

struct ScanDirectoryConfig {
    std::wstring path;
    std::unordered_set<std::wstring> extensions;
    std::vector<std::wstring> excludedDirectories;
    bool recursive = true;
};

struct AppState {
    HINSTANCE instance = nullptr;
    HWND hostWindow = nullptr;
    HWND launcherWindow = nullptr;
    HWND searchEdit = nullptr;
    HWND resultList = nullptr;
    HWND descriptionText = nullptr;
    WNDPROC editProc = nullptr;
    HFONT searchFont = nullptr;
    HFONT resultFont = nullptr;
    HFONT descriptionFont = nullptr;
    int dpi = USER_DEFAULT_SCREEN_DPI;
    HotkeyConfig hotkey {};
    std::vector<ScanDirectoryConfig> scanDirectories;
    std::vector<LaunchItem> items;
    HistoryConfig historyConfig {};
    LauncherAppearanceConfig launcherAppearance {};
    EditorConfig editorConfig {};
    std::unordered_map<std::wstring, std::wstring> commandVariables;
    std::unordered_map<std::wstring, LaunchItem> commandItemsByName;
    std::unordered_map<std::wstring, HistoryInfo> historyByKey;
    std::vector<CronTask> cronTasks;
    SearchQueryState lastSearch {};
    ULONGLONG lastSearchInputTick = 0;
    bool deferredRefreshArmed = false;
    bool filesystemCachePrimed = false;
    bool filesystemCachePendingRescan = false;
    bool launcherDataLoaded = false;
    bool refreshScheduled = false;
    bool resultRebuildScheduled = false;
    bool pendingItemsRefresh = false;
    bool pendingHistoryRefresh = false;
    ULONGLONG lastItemsRefreshTick = 0;
    ULONGLONG lastHistoryRefreshTick = 0;
};

AppState g_state {};

constexpr wchar_t kHostClassName[] = L"DoRun.HostWindow";
constexpr wchar_t kLauncherClassName[] = L"DoRun.LauncherWindow";
constexpr wchar_t kAppDirectoryName[] = L"DoRun";
constexpr wchar_t kConfigFileName[] = L"DoRun.yaml";
constexpr wchar_t kCommandFileName[] = L"Command.conf";
constexpr UINT_PTR kVisibilityTimerId = 1;
constexpr UINT_PTR kDeferredRefreshTimerId = 2;
constexpr UINT_PTR kCronTimerId = 3;
constexpr UINT kVisibilityTimerIntervalMs = 100;
constexpr UINT kDeferredRefreshCheckIntervalMs = 120;
constexpr UINT kCronTimerIntervalMs = 30 * 1000;
constexpr DWORD kCronRestartTerminateWaitMs = 5000;
constexpr UINT kMessageRefreshLauncherData = WM_APP + 1;
constexpr UINT kMessageRebuildResults = WM_APP + 2;
constexpr ULONGLONG kDeferredRefreshIdleDelayMs = 500;
constexpr ULONGLONG kItemsRefreshIntervalMs = 5ULL * 60ULL * 1000ULL;
constexpr ULONGLONG kHistoryRefreshIntervalMs = 15ULL * 1000ULL;
constexpr wchar_t kBuiltinCommandPrefix[] = L"builtin:";
constexpr wchar_t kFilesystemCacheMagic[] = L"DoRunFilesystemCacheV1";

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif

#ifndef DWMWCP_DEFAULT
#define DWMWCP_DEFAULT 0
#endif

#ifndef DWMWCP_DONOTROUND
#define DWMWCP_DONOTROUND 1
#endif

#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif

#ifndef DWMWCP_ROUNDSMALL
#define DWMWCP_ROUNDSMALL 3
#endif

#ifndef DWMWA_COLOR_NONE
#define DWMWA_COLOR_NONE 0xFFFFFFFE
#endif

constexpr BuiltinCommandDefinition kBuiltinCommands[] = {
    { L"/quit", L"Exit DoRun", L"quit" },
    { L"/reload", L"Reload configuration files", L"reload" },
    { L"/reboot", L"Restart the computer", L"reboot" },
    { L"/poweroff", L"Shut down the computer", L"poweroff" },
    { L"/hibernate", L"Hibernate the computer", L"hibernate" },
    { L"/standby", L"Put the computer to sleep", L"standby" },
    { L"/config", L"Open DoRun.yaml", L"config" },
    { L"/cmdconfig", L"Open Command.conf", L"cmdconfig" },
};

void RebuildResultList();
void RequestResultListRebuild();
void CancelDeferredRefresh();
void RecordSearchInputActivity();
void LoadCronTasks();
std::wstring QuoteCommandLineArgument(std::wstring_view argument);
bool LaunchProcessCommand(
    std::wstring_view commandLineText,
    std::wstring_view workingDirectoryText,
    int showCommand,
    DWORD priorityClass,
    DWORD extraCreationFlags = 0,
    HANDLE* launchedProcess = nullptr);

int Scale(int value) {
    return MulDiv(value, g_state.dpi, USER_DEFAULT_SCREEN_DPI);
}

std::wstring Trim(std::wstring_view text) {
    size_t start = 0;
    while (start < text.size() && iswspace(text[start]) != 0) {
        ++start;
    }
    size_t end = text.size();
    while (end > start && iswspace(text[end - 1]) != 0) {
        --end;
    }
    return std::wstring(text.substr(start, end - start));
}

std::wstring Lowercase(std::wstring_view text) {
    std::wstring value(text);
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
    return value;
}

std::wstring Uppercase(std::wstring_view text) {
    std::wstring value(text);
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towupper(ch));
    });
    return value;
}

bool ContainsInsensitive(std::wstring_view text, std::wstring_view query) {
    if (query.empty()) {
        return true;
    }
    const std::wstring lhs = Lowercase(text);
    const std::wstring rhs = Lowercase(query);
    return lhs.find(rhs) != std::wstring::npos;
}

std::wstring GetModuleDirectory() {
    std::array<wchar_t, MAX_PATH> buffer {};
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path().wstring();
}

std::wstring GetConfigPath(const wchar_t* fileName) {
    return (std::filesystem::path(GetModuleDirectory()) / fileName).wstring();
}

std::wstring GetAppDataDirectory() {
    PWSTR path = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr, &path);
    if (FAILED(result) || path == nullptr) {
        if (path != nullptr) {
            CoTaskMemFree(path);
        }
        return L"";
    }

    const std::filesystem::path appDataPath(path);
    CoTaskMemFree(path);
    return (appDataPath / kAppDirectoryName).wstring();
}

std::wstring GetLocalAppDataDirectory() {
    PWSTR path = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &path);
    if (FAILED(result) || path == nullptr) {
        if (path != nullptr) {
            CoTaskMemFree(path);
        }
        return L"";
    }

    const std::filesystem::path appDataPath(path);
    CoTaskMemFree(path);
    return (appDataPath / kAppDirectoryName).wstring();
}

std::wstring FindConfigPath(const wchar_t* fileName) {
    const std::filesystem::path modulePath = std::filesystem::path(GetModuleDirectory()) / fileName;
    std::error_code error;
    if (std::filesystem::exists(modulePath, error)) {
        return modulePath.wstring();
    }

    const std::wstring appDataDirectory = GetAppDataDirectory();
    if (appDataDirectory.empty()) {
        return modulePath.wstring();
    }

    const std::filesystem::path appDataPath = std::filesystem::path(appDataDirectory) / fileName;
    error.clear();
    if (std::filesystem::exists(appDataPath, error)) {
        return appDataPath.wstring();
    }

    return modulePath.wstring();
}

std::wstring GetWritableConfigPath(const wchar_t* fileName) {
    const std::filesystem::path modulePath = std::filesystem::path(GetModuleDirectory()) / fileName;
    std::error_code error;
    if (std::filesystem::exists(modulePath, error)) {
        return modulePath.wstring();
    }

    const std::wstring appDataDirectory = GetAppDataDirectory();
    if (appDataDirectory.empty()) {
        return modulePath.wstring();
    }

    const std::filesystem::path appDataPath = std::filesystem::path(appDataDirectory);
    std::filesystem::create_directories(appDataPath, error);
    if (error) {
        return modulePath.wstring();
    }

    return (appDataPath / fileName).wstring();
}

std::wstring LoadUtf8TextFile(const std::wstring& path) {
    std::wifstream stream(path);
    if (!stream.is_open()) {
        return L"";
    }

    stream.imbue(std::locale(".UTF-8"));
    std::wstringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

bool SaveUtf8TextFile(const std::wstring& path, const std::wstring& content) {
    std::wofstream stream(path, std::ios::trunc);
    if (!stream.is_open()) {
        return false;
    }

    stream.imbue(std::locale(".UTF-8"));
    stream << content;
    return stream.good();
}

struct YamlLine {
    int indent = 0;
    std::wstring text;
};

std::wstring StripYamlComment(std::wstring_view line) {
    bool inSingleQuote = false;
    bool inDoubleQuote = false;
    std::wstring result;
    result.reserve(line.size());
    for (size_t index = 0; index < line.size(); ++index) {
        const wchar_t ch = line[index];
        if (ch == L'\'' && !inDoubleQuote) {
            inSingleQuote = !inSingleQuote;
            result.push_back(ch);
            continue;
        }
        if (ch == L'"' && !inSingleQuote) {
            inDoubleQuote = !inDoubleQuote;
            result.push_back(ch);
            continue;
        }
        if (ch == L'#' && !inSingleQuote && !inDoubleQuote) {
            break;
        }
        result.push_back(ch);
    }
    return result;
}

std::wstring RTrim(std::wstring value) {
    while (!value.empty() && iswspace(value.back()) != 0) {
        value.pop_back();
    }
    return value;
}

std::vector<YamlLine> ParseYamlLines(const std::wstring& content) {
    std::vector<YamlLine> lines;
    std::wstringstream stream(content);
    std::wstring rawLine;
    while (std::getline(stream, rawLine)) {
        if (!rawLine.empty() && rawLine.back() == L'\r') {
            rawLine.pop_back();
        }

        const std::wstring withoutComments = RTrim(StripYamlComment(rawLine));
        size_t offset = 0;
        while (offset < withoutComments.size() && withoutComments[offset] == L' ') {
            ++offset;
        }

        const std::wstring trimmed = withoutComments.substr(offset);
        if (trimmed.empty()) {
            continue;
        }

        lines.push_back({ static_cast<int>(offset), trimmed });
    }
    return lines;
}

bool MatchYamlKey(const std::wstring& text, std::wstring_view key, std::wstring* remainder) {
    if (!text.starts_with(key)) {
        return false;
    }
    const size_t keyLength = key.size();
    if (text.size() <= keyLength || text[keyLength] != L':') {
        return false;
    }
    if (remainder != nullptr) {
        *remainder = Trim(text.substr(keyLength + 1));
    }
    return true;
}

std::optional<std::wstring> ExtractYamlChildBlock(const std::wstring& content, std::wstring_view key) {
    const std::vector<YamlLine> lines = ParseYamlLines(content);
    for (size_t index = 0; index < lines.size(); ++index) {
        if (lines[index].indent != 0) {
            continue;
        }

        std::wstring remainder;
        if (!MatchYamlKey(lines[index].text, key, &remainder) || !remainder.empty()) {
            continue;
        }

        std::vector<YamlLine> children;
        int minIndent = (std::numeric_limits<int>::max)();
        for (size_t childIndex = index + 1; childIndex < lines.size(); ++childIndex) {
            if (lines[childIndex].indent <= lines[index].indent) {
                break;
            }
            children.push_back(lines[childIndex]);
            minIndent = std::min(minIndent, lines[childIndex].indent);
        }

        if (children.empty()) {
            return std::nullopt;
        }

        std::wstring block;
        for (const YamlLine& line : children) {
            block.append(static_cast<size_t>(line.indent - minIndent), L' ');
            block += line.text;
            block += L"\n";
        }
        return block;
    }
    return std::nullopt;
}

std::optional<std::wstring> ParseYamlRawValue(const std::wstring& content, std::wstring_view key) {
    const std::vector<YamlLine> lines = ParseYamlLines(content);
    for (const YamlLine& line : lines) {
        if (line.indent != 0) {
            continue;
        }

        std::wstring remainder;
        if (!MatchYamlKey(line.text, key, &remainder) || remainder.empty()) {
            continue;
        }
        return remainder;
    }
    return std::nullopt;
}

std::vector<std::wstring> ParseYamlSequenceMapBlocks(const std::wstring& content, std::wstring_view key) {
    std::vector<std::wstring> blocks;
    const std::optional<std::wstring> block = ExtractYamlChildBlock(content, key);
    if (!block) {
        return blocks;
    }

    const std::vector<YamlLine> lines = ParseYamlLines(*block);
    std::wstring currentBlock;
    for (const YamlLine& line : lines) {
        if (line.indent == 0 && line.text.starts_with(L"- ")) {
            if (!currentBlock.empty()) {
                blocks.push_back(currentBlock);
                currentBlock.clear();
            }
            currentBlock += line.text.substr(2);
            currentBlock += L"\n";
            continue;
        }

        if (currentBlock.empty()) {
            continue;
        }
        currentBlock.append(static_cast<size_t>(line.indent), L' ');
        currentBlock += line.text;
        currentBlock += L"\n";
    }

    if (!currentBlock.empty()) {
        blocks.push_back(currentBlock);
    }
    return blocks;
}

std::optional<UINT> ParseYamlUInt(const std::wstring& content, std::wstring_view key) {
    const std::optional<std::wstring> rawValue = ParseYamlRawValue(content, key);
    if (!rawValue || rawValue->empty()) {
        return std::nullopt;
    }
    return static_cast<UINT>(_wtoi(rawValue->c_str()));
}

std::optional<int> ParseYamlInt(const std::wstring& content, std::wstring_view key) {
    const std::optional<std::wstring> rawValue = ParseYamlRawValue(content, key);
    if (!rawValue || rawValue->empty()) {
        return std::nullopt;
    }
    return _wtoi(rawValue->c_str());
}

std::optional<double> ParseYamlDouble(const std::wstring& content, std::wstring_view key) {
    const std::optional<std::wstring> rawValue = ParseYamlRawValue(content, key);
    if (!rawValue || rawValue->empty()) {
        return std::nullopt;
    }
    return _wtof(rawValue->c_str());
}

std::optional<std::wstring> ParseYamlString(const std::wstring& content, std::wstring_view key) {
    const std::optional<std::wstring> rawValue = ParseYamlRawValue(content, key);
    if (!rawValue) {
        return std::nullopt;
    }
    std::wstring value = *rawValue;
    if (value.size() >= 2 &&
        ((value.front() == L'"' && value.back() == L'"') || (value.front() == L'\'' && value.back() == L'\''))) {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

std::wstring ParseYamlScalarValue(std::wstring value) {
    value = Trim(std::move(value));
    if (value.size() >= 2 &&
        ((value.front() == L'"' && value.back() == L'"') || (value.front() == L'\'' && value.back() == L'\''))) {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

std::unordered_map<std::wstring, std::wstring> ParseYamlRootScalarMap(const std::wstring& content) {
    std::unordered_map<std::wstring, std::wstring> values;
    for (const YamlLine& line : ParseYamlLines(content)) {
        if (line.indent != 0) {
            continue;
        }

        const size_t separator = line.text.find(L':');
        if (separator == std::wstring::npos) {
            continue;
        }

        std::wstring key = Uppercase(Trim(line.text.substr(0, separator)));
        if (key.empty()) {
            continue;
        }

        const std::wstring rawValue = Trim(line.text.substr(separator + 1));
        if (rawValue.empty()) {
            continue;
        }

        std::wstring value = ParseYamlScalarValue(rawValue);
        values.insert_or_assign(std::move(key), std::move(value));
    }
    return values;
}

std::unordered_map<std::wstring, std::wstring> ParseYamlScalarMapBlock(const std::wstring& content, std::wstring_view key) {
    const std::optional<std::wstring> block = ExtractYamlChildBlock(content, key);
    if (!block) {
        return {};
    }
    return ParseYamlRootScalarMap(*block);
}

std::vector<std::wstring> ParseYamlStringArray(const std::wstring& content, std::wstring_view key) {
    std::vector<std::wstring> values;
    const std::optional<std::wstring> block = ExtractYamlChildBlock(content, key);
    if (!block) {
        return values;
    }

    for (const YamlLine& line : ParseYamlLines(*block)) {
        if (line.indent != 0 || !line.text.starts_with(L"- ")) {
            continue;
        }
        std::wstring value = Trim(line.text.substr(2));
        if (value.size() >= 2 &&
            ((value.front() == L'"' && value.back() == L'"') || (value.front() == L'\'' && value.back() == L'\''))) {
            value = value.substr(1, value.size() - 2);
        }
        values.push_back(std::move(value));
    }
    return values;
}

std::optional<bool> ParseYamlBool(const std::wstring& content, std::wstring_view key) {
    const std::optional<std::wstring> rawValue = ParseYamlRawValue(content, key);
    if (!rawValue || rawValue->empty()) {
        return std::nullopt;
    }

    const std::wstring value = Lowercase(Trim(*rawValue));
    if (value == L"1" || value == L"true") {
        return true;
    }
    if (value == L"0" || value == L"false") {
        return false;
    }
    return std::nullopt;
}

std::wstring ExpandEnvironmentVariables(std::wstring_view text) {
    if (text.empty()) {
        return L"";
    }

    const DWORD required = ExpandEnvironmentStringsW(std::wstring(text).c_str(), nullptr, 0);
    if (required == 0) {
        return std::wstring(text);
    }

    std::wstring expanded(static_cast<size_t>(required), L'\0');
    const DWORD written = ExpandEnvironmentStringsW(std::wstring(text).c_str(), expanded.data(), required);
    if (written == 0) {
        return std::wstring(text);
    }

    expanded.resize(wcsnlen_s(expanded.c_str(), expanded.size()));
    return expanded;
}

std::wstring ExpandCommandVariables(std::wstring_view text) {
    if (text.empty()) {
        return L"";
    }

    std::wstring expanded;
    expanded.reserve(text.size());

    for (size_t index = 0; index < text.size(); ++index) {
        if (text[index] != L'$' || (index + 1) >= text.size() || text[index + 1] != L'{') {
            expanded.push_back(text[index]);
            continue;
        }

        const size_t end = text.find(L'}', index + 2);
        if (end == std::wstring_view::npos) {
            expanded.push_back(text[index]);
            continue;
        }

        const std::wstring key = Uppercase(Trim(text.substr(index + 2, end - (index + 2))));
        if (const auto it = g_state.commandVariables.find(key); it != g_state.commandVariables.end()) {
            expanded += it->second;
        } else {
            expanded.append(text.substr(index, end - index + 1));
        }
        index = end;
    }

    return expanded;
}

std::wstring NormalizeHistoryField(std::wstring_view text) {
    return Lowercase(Trim(text));
}

std::wstring NormalizeCommandNameKey(std::wstring_view text) {
    return Lowercase(Trim(text));
}

std::wstring BuildItemKey(ItemSourceKind sourceKind, std::wstring_view commandLine, std::wstring_view workingDirectory) {
    return std::to_wstring(static_cast<int>(sourceKind)) + L"\n" +
        NormalizeHistoryField(commandLine) + L"\n" +
        NormalizeHistoryField(workingDirectory);
}

std::wstring BuildCommandIdentity(const LaunchItem& item) {
    if (!item.inlineBatchScript.empty()) {
        return L"inline-batch\n" + item.inlineBatchScript;
    }
    return item.commandLine;
}

std::wstring GetDefaultHistoryDatabasePath() {
    const std::wstring localAppDataDirectory = GetLocalAppDataDirectory();
    if (localAppDataDirectory.empty()) {
        return (std::filesystem::path(GetModuleDirectory()) / L"history.db").wstring();
    }
    return (std::filesystem::path(localAppDataDirectory) / L"history.db").wstring();
}

std::wstring GetHistoryDatabasePath() {
    if (!g_state.historyConfig.databasePath.empty()) {
        return ExpandEnvironmentVariables(g_state.historyConfig.databasePath);
    }
    return GetDefaultHistoryDatabasePath();
}

std::wstring GetFilesystemCachePath() {
    const std::wstring localAppDataDirectory = GetLocalAppDataDirectory();
    if (localAppDataDirectory.empty()) {
        return (std::filesystem::path(GetModuleDirectory()) / L"filesystem-cache.txt").wstring();
    }
    return (std::filesystem::path(localAppDataDirectory) / L"filesystem-cache.txt").wstring();
}

std::wstring EscapeCacheField(std::wstring_view value) {
    std::wstring escaped;
    escaped.reserve(value.size());
    for (const wchar_t ch : value) {
        switch (ch) {
        case L'\\':
            escaped += L"\\\\";
            break;
        case L'\t':
            escaped += L"\\t";
            break;
        case L'\n':
            escaped += L"\\n";
            break;
        case L'\r':
            escaped += L"\\r";
            break;
        default:
            escaped.push_back(ch);
            break;
        }
    }
    return escaped;
}

std::wstring UnescapeCacheField(std::wstring_view value) {
    std::wstring unescaped;
    unescaped.reserve(value.size());
    bool escaped = false;
    for (const wchar_t ch : value) {
        if (!escaped) {
            if (ch == L'\\') {
                escaped = true;
            } else {
                unescaped.push_back(ch);
            }
            continue;
        }

        switch (ch) {
        case L't':
            unescaped.push_back(L'\t');
            break;
        case L'n':
            unescaped.push_back(L'\n');
            break;
        case L'r':
            unescaped.push_back(L'\r');
            break;
        case L'\\':
            unescaped.push_back(L'\\');
            break;
        default:
            unescaped.push_back(L'\\');
            unescaped.push_back(ch);
            break;
        }
        escaped = false;
    }
    if (escaped) {
        unescaped.push_back(L'\\');
    }
    return unescaped;
}

std::wstring NormalizePathForComparison(const std::filesystem::path& path) {
    std::wstring normalized = path.lexically_normal().wstring();
    while (normalized.size() > 3 && (normalized.back() == L'\\' || normalized.back() == L'/')) {
        normalized.pop_back();
    }
    return Lowercase(std::move(normalized));
}

std::wstring ResolveExcludedDirectoryPath(const std::wstring& root, const std::wstring& excludedDirectory) {
    std::filesystem::path path(Trim(excludedDirectory));
    if (path.empty()) {
        return L"";
    }
    if (path.is_relative()) {
        path = std::filesystem::path(root) / path;
    }
    return NormalizePathForComparison(path);
}

bool IsPathUnderDirectory(const std::wstring& normalizedPath, const std::wstring& normalizedDirectory) {
    if (normalizedPath == normalizedDirectory) {
        return true;
    }
    if (normalizedPath.size() <= normalizedDirectory.size() || !normalizedPath.starts_with(normalizedDirectory)) {
        return false;
    }
    const wchar_t separator = normalizedPath[normalizedDirectory.size()];
    return separator == L'\\' || separator == L'/';
}

bool IsExcludedScanPath(const ScanDirectoryConfig& directory, const std::filesystem::path& path) {
    if (directory.excludedDirectories.empty()) {
        return false;
    }

    const std::wstring normalizedPath = NormalizePathForComparison(path);
    for (const std::wstring& excludedDirectory : directory.excludedDirectories) {
        if (IsPathUnderDirectory(normalizedPath, excludedDirectory)) {
            return true;
        }
    }
    return false;
}

std::wstring BuildScanDirectorySignature() {
    std::vector<std::wstring> parts;
    parts.reserve(g_state.scanDirectories.size());
    for (const ScanDirectoryConfig& directory : g_state.scanDirectories) {
        std::vector<std::wstring> extensions(directory.extensions.begin(), directory.extensions.end());
        std::sort(extensions.begin(), extensions.end());
        std::wstring part = directory.path + L"|" + (directory.recursive ? L"1" : L"0");
        for (const std::wstring& extension : extensions) {
            part += L"|";
            part += extension;
        }
        std::vector<std::wstring> excludedDirectories = directory.excludedDirectories;
        std::sort(excludedDirectories.begin(), excludedDirectories.end());
        for (const std::wstring& excludedDirectory : excludedDirectories) {
            part += L"|!";
            part += excludedDirectory;
        }
        parts.push_back(std::move(part));
    }
    std::sort(parts.begin(), parts.end());

    std::wstring signature = kFilesystemCacheMagic;
    for (const std::wstring& part : parts) {
        signature += L"\n";
        signature += part;
    }
    return signature;
}

bool IsSubsequenceMatch(std::wstring_view text, std::wstring_view query) {
    if (query.empty()) {
        return true;
    }

    size_t queryIndex = 0;
    for (const wchar_t ch : text) {
        if (towlower(ch) == towlower(query[queryIndex])) {
            ++queryIndex;
            if (queryIndex == query.size()) {
                return true;
            }
        }
    }
    return false;
}

std::vector<std::wstring> SplitQueryTokens(std::wstring_view query) {
    std::vector<std::wstring> tokens;
    std::wstringstream stream { std::wstring(query) };
    std::wstring token;
    while (stream >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

int GetMatchScoreForText(std::wstring_view haystack, std::wstring_view needle, int exactScore, int prefixScore, int substringScore, int subsequenceScore) {
    if (needle.empty()) {
        return 1;
    }

    const std::wstring normalizedHaystack = Lowercase(haystack);
    const std::wstring normalizedNeedle = Lowercase(needle);
    if (normalizedHaystack == normalizedNeedle) {
        return exactScore;
    }
    if (normalizedHaystack.starts_with(normalizedNeedle)) {
        return prefixScore;
    }
    if (normalizedHaystack.find(normalizedNeedle) != std::wstring::npos) {
        return substringScore;
    }
    if (g_state.historyConfig.fuzzyEnabled && IsSubsequenceMatch(normalizedHaystack, normalizedNeedle)) {
        return subsequenceScore;
    }
    return -1;
}

int GetMatchScoreForNormalizedText(std::wstring_view normalizedHaystack, std::wstring_view normalizedNeedle, int exactScore, int prefixScore, int substringScore, int subsequenceScore) {
    if (normalizedNeedle.empty()) {
        return 1;
    }

    if (normalizedHaystack == normalizedNeedle) {
        return exactScore;
    }
    if (normalizedHaystack.starts_with(normalizedNeedle)) {
        return prefixScore;
    }
    if (normalizedHaystack.find(normalizedNeedle) != std::wstring::npos) {
        return substringScore;
    }
    if (g_state.historyConfig.fuzzyEnabled && IsSubsequenceMatch(normalizedHaystack, normalizedNeedle)) {
        return subsequenceScore;
    }
    return -1;
}

void PopulateSearchFields(LaunchItem& item) {
    item.normalizedName = Lowercase(item.name);
    item.normalizedCommandLine = Lowercase(item.commandLine);

    std::filesystem::path commandPath(item.commandLine);
    item.normalizedCommandBaseName = Lowercase(commandPath.stem().wstring());
}

int ComputeMatchScore(const LaunchItem& item, const std::vector<std::wstring>& normalizedTokens) {
    if (normalizedTokens.empty()) {
        return 1;
    }

    int score = 0;
    for (const std::wstring& normalizedToken : normalizedTokens) {
        const int nameScore = GetMatchScoreForNormalizedText(item.normalizedName, normalizedToken, 100, 90, 75, 60);
        const int commandBaseScore = GetMatchScoreForNormalizedText(item.normalizedCommandBaseName, normalizedToken, 55, 55, 50, 45);
        const int commandScore = GetMatchScoreForNormalizedText(item.normalizedCommandLine, normalizedToken, 40, 40, 40, 25);
        const int bestScore = std::max({nameScore, commandBaseScore, commandScore});
        if (bestScore < 0) {
            return -1;
        }
        score += bestScore;
    }

    return score;
}

std::wstring GetCurrentUtcTimestamp() {
    SYSTEMTIME utcNow {};
    GetSystemTime(&utcNow);

    wchar_t buffer[32] {};
    swprintf_s(
        buffer,
        L"%04u-%02u-%02uT%02u:%02u:%02uZ",
        utcNow.wYear,
        utcNow.wMonth,
        utcNow.wDay,
        utcNow.wHour,
        utcNow.wMinute,
        utcNow.wSecond);
    return buffer;
}

ULONGLONG SystemTimeToUnixLikeTicks(const SYSTEMTIME& systemTime) {
    FILETIME fileTime {};
    if (SystemTimeToFileTime(&systemTime, &fileTime) == FALSE) {
        return 0;
    }

    ULARGE_INTEGER value {};
    value.LowPart = fileTime.dwLowDateTime;
    value.HighPart = fileTime.dwHighDateTime;
    return value.QuadPart;
}

std::optional<SYSTEMTIME> ParseUtcTimestamp(std::wstring_view value) {
    SYSTEMTIME timestamp {};
    if (swscanf_s(
            std::wstring(value).c_str(),
            L"%hu-%hu-%huT%hu:%hu:%huZ",
            &timestamp.wYear,
            &timestamp.wMonth,
            &timestamp.wDay,
            &timestamp.wHour,
            &timestamp.wMinute,
            &timestamp.wSecond) != 6) {
        return std::nullopt;
    }
    return timestamp;
}

int ComputeRecencyBonus(const std::wstring& lastRunUtc) {
    if (!g_state.historyConfig.recencyEnabled || lastRunUtc.empty()) {
        return 0;
    }

    const std::optional<SYSTEMTIME> lastRun = ParseUtcTimestamp(lastRunUtc);
    if (!lastRun) {
        return 0;
    }

    SYSTEMTIME nowUtc {};
    GetSystemTime(&nowUtc);
    const ULONGLONG nowTicks = SystemTimeToUnixLikeTicks(nowUtc);
    const ULONGLONG lastRunTicks = SystemTimeToUnixLikeTicks(*lastRun);
    if (nowTicks <= lastRunTicks) {
        return 30;
    }

    const double hoursSinceLastRun = static_cast<double>(nowTicks - lastRunTicks) / 10000000.0 / 3600.0;
    if (hoursSinceLastRun <= 24.0) {
        return 30;
    }
    if (hoursSinceLastRun <= 24.0 * 7.0) {
        return 20;
    }
    if (hoursSinceLastRun <= 24.0 * 30.0) {
        return 10;
    }
    if (hoursSinceLastRun <= 24.0 * 90.0) {
        return 5;
    }
    return 0;
}

HFONT CreateConfiguredUiFont(int dpi, std::wstring_view fontFamily, int pointSize) {
    NONCLIENTMETRICSW metrics {};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, static_cast<UINT>(dpi)) == FALSE) {
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
    }

    if (!fontFamily.empty()) {
        wcsncpy_s(metrics.lfMessageFont.lfFaceName, fontFamily.data(), _TRUNCATE);
    }

    if (pointSize > 0) {
        HDC screenDc = GetDC(nullptr);
        const int logPixelsY = screenDc != nullptr ? GetDeviceCaps(screenDc, LOGPIXELSY) : dpi;
        if (screenDc != nullptr) {
            ReleaseDC(nullptr, screenDc);
        }
        metrics.lfMessageFont.lfHeight = -MulDiv(pointSize, logPixelsY, 72);
    }

    return CreateFontIndirectW(&metrics.lfMessageFont);
}

void ApplyFont(HWND window, HFONT font) {
    if (window != nullptr && font != nullptr) {
        SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

int MeasureFontTextHeight(HFONT font) {
    if (font == nullptr) {
        return Scale(18);
    }

    HDC screenDc = GetDC(nullptr);
    if (screenDc == nullptr) {
        return Scale(18);
    }

    const HGDIOBJ oldFont = SelectObject(screenDc, font);
    TEXTMETRICW textMetric {};
    const BOOL ok = GetTextMetricsW(screenDc, &textMetric);
    SelectObject(screenDc, oldFont);
    ReleaseDC(nullptr, screenDc);

    if (ok == FALSE) {
        return Scale(18);
    }
    return textMetric.tmHeight;
}

void UpdateUiFont(int dpi) {
    g_state.dpi = dpi;
    if (g_state.searchFont != nullptr) {
        DeleteObject(g_state.searchFont);
        g_state.searchFont = nullptr;
    }
    if (g_state.resultFont != nullptr) {
        DeleteObject(g_state.resultFont);
        g_state.resultFont = nullptr;
    }
    if (g_state.descriptionFont != nullptr) {
        DeleteObject(g_state.descriptionFont);
        g_state.descriptionFont = nullptr;
    }

    g_state.searchFont = CreateConfiguredUiFont(dpi, g_state.launcherAppearance.searchFontFamily, g_state.launcherAppearance.searchFontPointSize);
    g_state.resultFont = CreateConfiguredUiFont(dpi, g_state.launcherAppearance.resultFontFamily, g_state.launcherAppearance.resultFontPointSize);
    const int descriptionPointSize = g_state.launcherAppearance.resultFontPointSize > 0
        ? std::max(6, g_state.launcherAppearance.resultFontPointSize - 2)
        : 8;
    g_state.descriptionFont = CreateConfiguredUiFont(dpi, g_state.launcherAppearance.resultFontFamily, descriptionPointSize);
}

bool TryApplyNativeRoundedCorners() {
    if (g_state.launcherWindow == nullptr) {
        return false;
    }

    const DWM_WINDOW_CORNER_PREFERENCE preference = static_cast<DWM_WINDOW_CORNER_PREFERENCE>(
        g_state.launcherAppearance.cornerRadius <= 0
            ? DWMWCP_DONOTROUND
            : (g_state.launcherAppearance.cornerRadius <= 12 ? DWMWCP_ROUNDSMALL : DWMWCP_ROUND));
    const HRESULT cornerResult = DwmSetWindowAttribute(
        g_state.launcherWindow,
        DWMWA_WINDOW_CORNER_PREFERENCE,
        &preference,
        sizeof(preference));
    if (FAILED(cornerResult)) {
        return false;
    }

    const COLORREF borderColor = DWMWA_COLOR_NONE;
    DwmSetWindowAttribute(
        g_state.launcherWindow,
        DWMWA_BORDER_COLOR,
        &borderColor,
        sizeof(borderColor));
    return true;
}

void ApplyLauncherAppearance() {
    if (g_state.launcherWindow == nullptr) {
        return;
    }

    LONG_PTR exStyle = GetWindowLongPtrW(g_state.launcherWindow, GWL_EXSTYLE);
    if (g_state.launcherAppearance.opacity < 255) {
        exStyle |= WS_EX_LAYERED;
        SetWindowLongPtrW(g_state.launcherWindow, GWL_EXSTYLE, exStyle);
        SetLayeredWindowAttributes(g_state.launcherWindow, 0, static_cast<BYTE>(g_state.launcherAppearance.opacity), LWA_ALPHA);
    } else {
        exStyle &= ~static_cast<LONG_PTR>(WS_EX_LAYERED);
        SetWindowLongPtrW(g_state.launcherWindow, GWL_EXSTYLE, exStyle);
    }

    if (TryApplyNativeRoundedCorners()) {
        SetWindowRgn(g_state.launcherWindow, nullptr, TRUE);
        return;
    }

    if (g_state.launcherAppearance.cornerRadius <= 0) {
        SetWindowRgn(g_state.launcherWindow, nullptr, TRUE);
        return;
    }

    RECT windowRect {};
    if (GetWindowRect(g_state.launcherWindow, &windowRect) == FALSE) {
        return;
    }

    const int width = windowRect.right - windowRect.left;
    const int height = windowRect.bottom - windowRect.top;
    if (width <= 0 || height <= 0) {
        return;
    }

    const int diameter = std::max(2, std::min(g_state.launcherAppearance.cornerRadius * 2, std::min(width, height)));
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, diameter, diameter);
    if (region == nullptr) {
        return;
    }

    if (SetWindowRgn(g_state.launcherWindow, region, TRUE) == 0) {
        DeleteObject(region);
    }
}

void LayoutLauncher() {
    RECT workArea {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const int width = Scale(720);
    const int margin = Scale(12);
    const int controlHeight = std::max(Scale(32), MeasureFontTextHeight(g_state.searchFont) + Scale(12));
    const int descriptionHeight = std::max(Scale(18), MeasureFontTextHeight(g_state.descriptionFont) + Scale(4));
    int listItemHeight = Scale(30);
    if (g_state.resultList != nullptr) {
        const LRESULT measuredHeight = SendMessageW(g_state.resultList, LB_GETITEMHEIGHT, 0, 0);
        if (measuredHeight > 0) {
            listItemHeight = static_cast<int>(measuredHeight);
        }
    }
    const int listHeight = listItemHeight * g_state.launcherAppearance.visibleItemCount;
    const int height = margin * 4 + controlHeight + listHeight + descriptionHeight;
    const int x = workArea.left + ((workArea.right - workArea.left) - width) / 2;
    const int y = workArea.top + ((workArea.bottom - workArea.top) - height) / 4;

    SetWindowPos(g_state.launcherWindow, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
    MoveWindow(g_state.searchEdit, margin, margin, width - margin * 2, controlHeight, TRUE);
    MoveWindow(g_state.resultList, margin, margin * 2 + controlHeight, width - margin * 2, listHeight, TRUE);
    MoveWindow(g_state.descriptionText, margin, margin * 3 + controlHeight + listHeight, width - margin * 2, descriptionHeight, TRUE);
    ApplyLauncherAppearance();
}

void HideLauncher() {
    if (g_state.launcherWindow != nullptr) {
        KillTimer(g_state.launcherWindow, kVisibilityTimerId);
    }
    CancelDeferredRefresh();
    ShowWindow(g_state.launcherWindow, SW_HIDE);
}

bool IsChildOrSameWindow(HWND parent, HWND candidate) {
    return parent != nullptr && candidate != nullptr && (parent == candidate || IsChild(parent, candidate) != FALSE);
}

bool IsLauncherRelatedWindow(HWND window) {
    return IsChildOrSameWindow(g_state.launcherWindow, window);
}

void StartVisibilityTimer() {
    if (g_state.launcherWindow != nullptr) {
        SetTimer(g_state.launcherWindow, kVisibilityTimerId, kVisibilityTimerIntervalMs, nullptr);
    }
}

HKL GetEnglishKeyboardLayout() {
    HKL englishLayout = LoadKeyboardLayoutW(L"00000409", KLF_ACTIVATE | KLF_SUBSTITUTE_OK);
    if (englishLayout == nullptr) {
        englishLayout = LoadKeyboardLayoutW(L"00000409", KLF_SUBSTITUTE_OK);
    }
    return englishLayout;
}

void SetEnglishImeForWindow(HWND window) {
    const HKL englishLayout = GetEnglishKeyboardLayout();
    if (englishLayout != nullptr) {
        ActivateKeyboardLayout(englishLayout, 0);
        if (window != nullptr) {
            SendMessageW(window, WM_INPUTLANGCHANGEREQUEST, 0, reinterpret_cast<LPARAM>(englishLayout));
        }
        if (g_state.launcherWindow != nullptr && g_state.launcherWindow != window) {
            SendMessageW(g_state.launcherWindow, WM_INPUTLANGCHANGEREQUEST, 0, reinterpret_cast<LPARAM>(englishLayout));
        }
    }

    if (window == nullptr) {
        return;
    }

    HIMC inputContext = ImmGetContext(window);
    if (inputContext == nullptr) {
        return;
    }

    ImmSetOpenStatus(inputContext, FALSE);
    ImmReleaseContext(window, inputContext);
}

void EnsureDefaultHotkey() {
    g_state.hotkey.modifiers = MOD_ALT;
    g_state.hotkey.vk = VK_SPACE;
}

void LoadHotkeyConfig() {
    EnsureDefaultHotkey();

    const std::wstring configPath = FindConfigPath(kConfigFileName);
    const std::wstring content = LoadUtf8TextFile(configPath);
    if (content.empty()) {
        return;
    }

    if (const std::optional<UINT> modifiers = ParseYamlUInt(content, L"HOTKEY_MODIFIERS")) {
        g_state.hotkey.modifiers = *modifiers;
    }
    if (const std::optional<UINT> vk = ParseYamlUInt(content, L"HOTKEY_VK")) {
        g_state.hotkey.vk = *vk;
    }
    if (g_state.hotkey.vk == 0U) {
        g_state.hotkey.vk = VK_SPACE;
    }
}

void LoadHistoryConfig() {
    g_state.historyConfig = HistoryConfig {};

    const std::wstring configPath = FindConfigPath(kConfigFileName);
    const std::wstring content = LoadUtf8TextFile(configPath);
    if (content.empty()) {
        return;
    }

    if (const std::optional<UINT> enabled = ParseYamlUInt(content, L"HISTORY_ENABLED")) {
        g_state.historyConfig.enabled = *enabled != 0U;
    }
    if (const std::optional<std::wstring> databasePath = ParseYamlString(content, L"HISTORY_DB_PATH")) {
        g_state.historyConfig.databasePath = *databasePath;
    }
    if (const std::optional<double> rankSumLimit = ParseYamlDouble(content, L"HISTORY_RANK_SUM_LIMIT")) {
        g_state.historyConfig.rankSumLimit = std::max(1.0, *rankSumLimit);
    }
    if (const std::optional<double> decayFactor = ParseYamlDouble(content, L"HISTORY_DECAY_FACTOR")) {
        g_state.historyConfig.decayFactor = std::clamp(*decayFactor, 0.01, 0.999);
    }
    if (const std::optional<double> pruneBelow = ParseYamlDouble(content, L"HISTORY_PRUNE_BELOW")) {
        g_state.historyConfig.pruneBelow = std::max(0.0, *pruneBelow);
    }
    if (const std::optional<int> maxRows = ParseYamlInt(content, L"HISTORY_MAX_ROWS")) {
        g_state.historyConfig.maxRows = std::max(1, *maxRows);
    }
    if (const std::optional<UINT> recencyEnabled = ParseYamlUInt(content, L"HISTORY_RECENCY_ENABLED")) {
        g_state.historyConfig.recencyEnabled = *recencyEnabled != 0U;
    }
    if (const std::optional<UINT> fuzzyEnabled = ParseYamlUInt(content, L"HISTORY_FUZZY_ENABLED")) {
        g_state.historyConfig.fuzzyEnabled = *fuzzyEnabled != 0U;
    }
}

void LoadLauncherAppearanceConfig() {
    g_state.launcherAppearance = LauncherAppearanceConfig {};

    const std::wstring configPath = FindConfigPath(kConfigFileName);
    const std::wstring content = LoadUtf8TextFile(configPath);
    if (content.empty()) {
        return;
    }

    if (const std::optional<int> cornerRadius = ParseYamlInt(content, L"LAUNCHER_CORNER_RADIUS")) {
        g_state.launcherAppearance.cornerRadius = std::clamp(*cornerRadius, 0, 96);
    }
    if (const std::optional<int> opacity = ParseYamlInt(content, L"LAUNCHER_OPACITY")) {
        g_state.launcherAppearance.opacity = std::clamp(*opacity, 32, 255);
    }
    if (const std::optional<int> visibleItemCount = ParseYamlInt(content, L"LAUNCHER_VISIBLE_ITEM_COUNT")) {
        g_state.launcherAppearance.visibleItemCount = std::clamp(*visibleItemCount, 1, 20);
    }
    if (const std::optional<std::wstring> searchFontFamily = ParseYamlString(content, L"LAUNCHER_SEARCH_FONT_FAMILY")) {
        g_state.launcherAppearance.searchFontFamily = Trim(*searchFontFamily);
    }
    if (const std::optional<std::wstring> resultFontFamily = ParseYamlString(content, L"LAUNCHER_RESULT_FONT_FAMILY")) {
        g_state.launcherAppearance.resultFontFamily = Trim(*resultFontFamily);
    }
    if (const std::optional<int> searchFontPointSize = ParseYamlInt(content, L"LAUNCHER_SEARCH_FONT_SIZE")) {
        g_state.launcherAppearance.searchFontPointSize = std::clamp(*searchFontPointSize, 0, 72);
    }
    if (const std::optional<int> resultFontPointSize = ParseYamlInt(content, L"LAUNCHER_RESULT_FONT_SIZE")) {
        g_state.launcherAppearance.resultFontPointSize = std::clamp(*resultFontPointSize, 0, 72);
    }
}

void LoadCommandVariables() {
    g_state.editorConfig = EditorConfig {};
    g_state.commandVariables.clear();

    const std::wstring configPath = FindConfigPath(kConfigFileName);
    const std::wstring content = LoadUtf8TextFile(configPath);
    if (content.empty()) {
        return;
    }

    g_state.commandVariables = ParseYamlRootScalarMap(content);
    for (auto& [key, value] : ParseYamlScalarMapBlock(content, L"VARS")) {
        g_state.commandVariables.insert_or_assign(std::move(key), std::move(value));
    }
    if (const auto it = g_state.commandVariables.find(L"EDITOR"); it != g_state.commandVariables.end()) {
        g_state.editorConfig.commandLine = Trim(it->second);
    }
}

void RegisterLauncherHotkey() {
    UnregisterHotKey(g_state.hostWindow, ID_HOTKEY_LAUNCH);
    RegisterHotKey(g_state.hostWindow, ID_HOTKEY_LAUNCH, g_state.hotkey.modifiers, g_state.hotkey.vk);
}

std::wstring NormalizeExtension(std::wstring value) {
    value = Lowercase(Trim(value));
    if (!value.empty() && value.front() != L'.') {
        value.insert(value.begin(), L'.');
    }
    return value;
}

std::unordered_set<std::wstring> LoadExecutableExtensionsFromPathExt() {
    std::unordered_set<std::wstring> extensions;
    std::array<wchar_t, 32767> buffer {};
    const DWORD length = GetEnvironmentVariableW(L"PATHEXT", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size()) {
        std::wstringstream stream(buffer.data());
        std::wstring token;
        while (std::getline(stream, token, L';')) {
            token = NormalizeExtension(token);
            if (!token.empty()) {
                extensions.insert(token);
            }
        }
    }
    return extensions;
}

std::unordered_set<std::wstring> BuildExecutableExtensions(bool includePathExt, const std::vector<std::wstring>& configuredExtensions) {
    std::unordered_set<std::wstring> extensions = includePathExt
        ? LoadExecutableExtensionsFromPathExt()
        : std::unordered_set<std::wstring> {};

    for (std::wstring extension : configuredExtensions) {
        extension = NormalizeExtension(std::move(extension));
        if (!extension.empty()) {
            extensions.insert(extension);
        }
    }
    return extensions;
}

std::unordered_set<std::wstring> LoadExecutableExtensions(const std::wstring& content) {
    bool includePathExt = true;
    if (!content.empty()) {
        if (const std::optional<bool> configured = ParseYamlBool(content, L"INDEX_INCLUDE_PATHEXT")) {
            includePathExt = *configured;
        }
    }

    std::vector<std::wstring> configuredExtensions = content.empty()
        ? std::vector<std::wstring> {}
        : ParseYamlStringArray(content, L"INDEX_EXTENSIONS");
    if (configuredExtensions.empty()) {
        configuredExtensions = { L".exe", L".lnk" };
    }

    return BuildExecutableExtensions(includePathExt, configuredExtensions);
}

void EnsureDefaultDirectories() {
    if (!g_state.scanDirectories.empty()) {
        return;
    }
    g_state.scanDirectories = {
        { L"C:\\Windows\\System32" },
        { L"C:\\Program Files" },
        { L"C:\\Program Files (x86)" },
    };
}

void LoadScanDirectories(const std::wstring& content) {
    g_state.scanDirectories.clear();
    const std::unordered_set<std::wstring> globalExtensions = LoadExecutableExtensions(content);
    bool globalRecursive = true;
    if (!content.empty()) {
        if (const std::optional<bool> configured = ParseYamlBool(content, L"INDEX_RECURSIVE")) {
            globalRecursive = *configured;
        }
    }

    auto applyDefaults = [&](ScanDirectoryConfig& config) {
        config.extensions = globalExtensions;
        config.recursive = globalRecursive;
    };

    if (content.empty()) {
        EnsureDefaultDirectories();
        for (ScanDirectoryConfig& directory : g_state.scanDirectories) {
            applyDefaults(directory);
        }
        return;
    }

    for (const std::wstring& block : ParseYamlSequenceMapBlocks(content, L"DIR")) {
        const std::optional<std::wstring> path = ParseYamlString(block, L"PATH");
        if (!path || path->empty()) {
            continue;
        }

        ScanDirectoryConfig directory {};
        directory.path = *path;
        applyDefaults(directory);
        if (const std::optional<bool> recursive = ParseYamlBool(block, L"INDEX_RECURSIVE")) {
            directory.recursive = *recursive;
        }
        const std::vector<std::wstring> directoryExtensions = ParseYamlStringArray(block, L"INDEX_EXTENSIONS");
        const std::optional<bool> includePathExt = ParseYamlBool(block, L"INDEX_INCLUDE_PATHEXT");
        if (includePathExt || !directoryExtensions.empty()) {
            std::vector<std::wstring> effectiveExtensions = directoryExtensions;
            if (effectiveExtensions.empty()) {
                effectiveExtensions = { L".exe", L".lnk" };
            }
            directory.extensions = BuildExecutableExtensions(includePathExt.value_or(true), effectiveExtensions);
        }
        for (const std::wstring& excludedDirectory : ParseYamlStringArray(block, L"INDEX_EXCLUDE_DIRS")) {
            std::wstring normalizedExcludedDirectory = ResolveExcludedDirectoryPath(directory.path, excludedDirectory);
            if (!normalizedExcludedDirectory.empty()) {
                directory.excludedDirectories.push_back(std::move(normalizedExcludedDirectory));
            }
        }
        g_state.scanDirectories.push_back(std::move(directory));
    }

    if (g_state.scanDirectories.empty()) {
        EnsureDefaultDirectories();
        for (ScanDirectoryConfig& directory : g_state.scanDirectories) {
            applyDefaults(directory);
        }
    }
}

bool OpenHistoryDatabase(sqlite3** database) {
    if (database == nullptr) {
        return false;
    }
    *database = nullptr;

    const std::wstring databasePath = GetHistoryDatabasePath();
    std::error_code error;
    const std::filesystem::path databaseDirectory = std::filesystem::path(databasePath).parent_path();
    if (!databaseDirectory.empty()) {
        std::filesystem::create_directories(databaseDirectory, error);
        if (error) {
            return false;
        }
    }

    if (sqlite3_open16(databasePath.c_str(), database) != SQLITE_OK || *database == nullptr) {
        if (*database != nullptr) {
            sqlite3_close(*database);
            *database = nullptr;
        }
        return false;
    }

    const char* initSql =
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=NORMAL;"
        "PRAGMA foreign_keys=ON;"
        "CREATE TABLE IF NOT EXISTS launch_history ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    item_key TEXT NOT NULL UNIQUE,"
        "    display_name TEXT NOT NULL,"
        "    command_line TEXT NOT NULL,"
        "    working_directory TEXT NOT NULL DEFAULT '',"
        "    source_kind INTEGER NOT NULL DEFAULT 0,"
        "    run_count INTEGER NOT NULL DEFAULT 0,"
        "    rank REAL NOT NULL DEFAULT 0,"
        "    last_run_utc TEXT NOT NULL DEFAULT '',"
        "    created_utc TEXT NOT NULL DEFAULT '',"
        "    updated_utc TEXT NOT NULL DEFAULT ''"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_launch_history_rank ON launch_history(rank DESC);"
        "CREATE INDEX IF NOT EXISTS idx_launch_history_last_run ON launch_history(last_run_utc DESC);"
        "CREATE INDEX IF NOT EXISTS idx_launch_history_display_name ON launch_history(display_name);";

    if (sqlite3_exec(*database, initSql, nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_close(*database);
        *database = nullptr;
        return false;
    }

    return true;
}

bool EnsureParentDirectoryForFile(const std::wstring& path) {
    std::error_code error;
    const std::filesystem::path directory = std::filesystem::path(path).parent_path();
    if (directory.empty()) {
        return true;
    }
    std::filesystem::create_directories(directory, error);
    return !error;
}

bool EnsureFileExists(const std::wstring& path) {
    std::error_code error;
    if (std::filesystem::exists(path, error)) {
        return true;
    }
    if (!EnsureParentDirectoryForFile(path)) {
        return false;
    }
    return SaveUtf8TextFile(path, L"");
}

bool OpenPathInShell(const std::wstring& path) {
    const HINSTANCE result = ShellExecuteW(g_state.launcherWindow, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

std::wstring ResolveShortcutCommandLine(const std::wstring& path) {
    IShellLinkW* shellLink = nullptr;
    HRESULT result = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shellLink));
    if (FAILED(result) || shellLink == nullptr) {
        return L"";
    }

    IPersistFile* persistFile = nullptr;
    result = shellLink->QueryInterface(IID_PPV_ARGS(&persistFile));
    if (FAILED(result) || persistFile == nullptr) {
        shellLink->Release();
        return L"";
    }

    std::wstring commandLine;
    result = persistFile->Load(path.c_str(), STGM_READ);
    if (SUCCEEDED(result)) {
        std::array<wchar_t, INFOTIPSIZE> targetPath {};
        std::array<wchar_t, INFOTIPSIZE> arguments {};
        WIN32_FIND_DATAW findData {};
        if (SUCCEEDED(shellLink->GetPath(targetPath.data(), static_cast<int>(targetPath.size()), &findData, SLGP_UNCPRIORITY)) && targetPath[0] != L'\0') {
            commandLine = targetPath.data();
            if (SUCCEEDED(shellLink->GetArguments(arguments.data(), static_cast<int>(arguments.size()))) && arguments[0] != L'\0') {
                commandLine += L" ";
                commandLine += arguments.data();
            }
        }
    }

    persistFile->Release();
    shellLink->Release();
    return commandLine;
}

std::wstring GetTempBatchDirectory() {
    const std::wstring localAppDataDirectory = GetLocalAppDataDirectory();
    if (!localAppDataDirectory.empty()) {
        return (std::filesystem::path(localAppDataDirectory) / L"temp").wstring();
    }

    std::array<wchar_t, MAX_PATH> buffer {};
    const DWORD length = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
    if (length == 0 || length >= buffer.size()) {
        return GetModuleDirectory();
    }
    return std::wstring(buffer.data(), length);
}

std::wstring MakeInlineBatchFilePath() {
    std::error_code error;
    const std::filesystem::path directory = GetTempBatchDirectory();
    std::filesystem::create_directories(directory, error);

    const DWORD processId = GetCurrentProcessId();
    const ULONGLONG tickCount = GetTickCount64();
    const std::wstring fileName = L"DoRun-inline-" + std::to_wstring(processId) + L"-" + std::to_wstring(tickCount) + L".bat";
    return (directory / fileName).wstring();
}

bool LaunchInlineBatchScript(const LaunchItem& item, HANDLE* launchedProcess = nullptr) {
    const std::wstring batchPath = MakeInlineBatchFilePath();
    std::wstring script = ExpandCommandVariables(item.inlineBatchScript);
    if (!script.ends_with(L"\n")) {
        script += L"\n";
    }
    if (!SaveUtf8TextFile(batchPath, script)) {
        return false;
    }

    const std::wstring commandLine = L"cmd.exe /c " + QuoteCommandLineArgument(batchPath);
    const DWORD extraCreationFlags = item.showCommand == SW_HIDE ? CREATE_NO_WINDOW : 0;
    return LaunchProcessCommand(commandLine, item.workingDirectory, item.showCommand, item.priorityClass, extraCreationFlags, launchedProcess);
}

bool RunShellCommand(const wchar_t* file, const wchar_t* parameters, int showCommand = SW_HIDE) {
    const HINSTANCE result = ShellExecuteW(g_state.launcherWindow, L"open", file, parameters, nullptr, showCommand);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

bool SuspendSystem(bool hibernate) {
    return SetSuspendState(hibernate ? TRUE : FALSE, FALSE, FALSE) != FALSE;
}

std::wstring QuoteCommandLineArgument(std::wstring_view argument) {
    std::wstring quoted;
    quoted.reserve(argument.size() + 2);
    quoted.push_back(L'"');
    quoted.append(argument);
    quoted.push_back(L'"');
    return quoted;
}

bool LaunchProcessCommand(
    std::wstring_view commandLineText,
    std::wstring_view workingDirectoryText,
    int showCommand,
    DWORD priorityClass,
    DWORD extraCreationFlags,
    HANDLE* launchedProcess) {
    if (launchedProcess != nullptr) {
        *launchedProcess = nullptr;
    }

    std::wstring commandLine = ExpandEnvironmentVariables(commandLineText);
    std::wstring workingDirectoryBuffer = ExpandEnvironmentVariables(workingDirectoryText);
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startupInfo {};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESHOWWINDOW;
    startupInfo.wShowWindow = static_cast<WORD>(showCommand);

    PROCESS_INFORMATION processInfo {};
    const wchar_t* workingDirectory = workingDirectoryBuffer.empty() ? nullptr : workingDirectoryBuffer.c_str();
    const DWORD creationFlags = CREATE_UNICODE_ENVIRONMENT | priorityClass | extraCreationFlags;
    const BOOL ok = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, creationFlags, nullptr, workingDirectory, &startupInfo, &processInfo);
    if (ok == FALSE) {
        SHELLEXECUTEINFOW executeInfo {};
        executeInfo.cbSize = sizeof(executeInfo);
        executeInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
        executeInfo.hwnd = g_state.launcherWindow;
        executeInfo.lpVerb = L"open";
        executeInfo.lpFile = commandLine.c_str();
        executeInfo.lpDirectory = workingDirectory;
        executeInfo.nShow = showCommand;
        if (ShellExecuteExW(&executeInfo) == FALSE || reinterpret_cast<INT_PTR>(executeInfo.hInstApp) <= 32) {
            return false;
        }
        if (launchedProcess != nullptr) {
            *launchedProcess = executeInfo.hProcess;
        } else if (executeInfo.hProcess != nullptr) {
            CloseHandle(executeInfo.hProcess);
        }
        return true;
    }

    CloseHandle(processInfo.hThread);
    if (launchedProcess != nullptr) {
        *launchedProcess = processInfo.hProcess;
    } else {
        CloseHandle(processInfo.hProcess);
    }
    return true;
}

bool OpenPathWithConfiguredEditor(const std::wstring& path) {
    if (g_state.editorConfig.commandLine.empty()) {
        return OpenPathInShell(path);
    }

    const std::wstring commandLine = g_state.editorConfig.commandLine + L" " + QuoteCommandLineArgument(path);
    return LaunchProcessCommand(commandLine, L"", SW_SHOWNORMAL, NORMAL_PRIORITY_CLASS);
}

bool ExecuteSql(sqlite3* database, const char* sql) {
    return database != nullptr && sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

bool PrepareStatement(sqlite3* database, const wchar_t* sql, sqlite3_stmt** statement) {
    if (database == nullptr || statement == nullptr) {
        return false;
    }
    *statement = nullptr;
    return sqlite3_prepare16_v2(database, sql, -1, statement, nullptr) == SQLITE_OK && *statement != nullptr;
}

void FinalizeStatement(sqlite3_stmt* statement) {
    if (statement != nullptr) {
        sqlite3_finalize(statement);
    }
}

bool BindText(sqlite3_stmt* statement, int index, const std::wstring& value) {
    return sqlite3_bind_text16(statement, index, value.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK;
}

void LoadHistoryCache() {
    g_state.historyByKey.clear();
    if (!g_state.historyConfig.enabled) {
        return;
    }

    sqlite3* database = nullptr;
    if (!OpenHistoryDatabase(&database)) {
        return;
    }

    sqlite3_stmt* statement = nullptr;
    const wchar_t* sql = L"SELECT item_key, rank, run_count, last_run_utc FROM launch_history;";
    if (!PrepareStatement(database, sql, &statement)) {
        sqlite3_close(database);
        return;
    }

    while (sqlite3_step(statement) == SQLITE_ROW) {
        const void* itemKeyText = sqlite3_column_text16(statement, 0);
        const void* lastRunText = sqlite3_column_text16(statement, 3);

        HistoryInfo info {};
        info.rank = sqlite3_column_double(statement, 1);
        info.runCount = sqlite3_column_int(statement, 2);
        if (lastRunText != nullptr) {
            info.lastRunUtc = static_cast<const wchar_t*>(lastRunText);
        }

        if (itemKeyText != nullptr) {
            g_state.historyByKey.emplace(static_cast<const wchar_t*>(itemKeyText), std::move(info));
        }
    }

    FinalizeStatement(statement);
    sqlite3_close(database);
}

std::vector<std::wstring> SplitCommandFields(std::wstring_view line) {
    std::vector<std::wstring> fields;
    std::wstring current;
    bool inQuotes = false;
    bool escaped = false;
    auto isEscapableChar = [](wchar_t ch) {
        return ch == L'\\' || ch == L':' || ch == L'"' || ch == L';' || ch == L'#';
    };
    auto isSchemeName = [](std::wstring_view value) {
        return !value.empty() && std::all_of(value.begin(), value.end(), [](wchar_t ch) { return iswalpha(ch) != 0; });
    };

    for (size_t index = 0; index < line.size(); ++index) {
        const wchar_t ch = line[index];
        if (escaped) {
            if (isEscapableChar(ch)) {
                current.push_back(ch);
            } else {
                current.push_back(L'\\');
                current.push_back(ch);
            }
            escaped = false;
            continue;
        }
        if (ch == L'\\') {
            escaped = true;
            continue;
        }
        if (ch == L'"') {
            inQuotes = !inQuotes;
            current.push_back(ch);
        } else if (ch == L':' && !inQuotes) {
            const bool isDriveLetter = !fields.empty() && current.size() == 1 && iswalpha(current[0]) != 0 &&
                index + 1 < line.size() && (line[index + 1] == L'\\' || line[index + 1] == L'/');
            const bool isUriScheme = !fields.empty() && isSchemeName(current) &&
                ((index + 2 < line.size() && line[index + 1] == L'/' && line[index + 2] == L'/') ||
                 (current == L"shell"));
            if (isDriveLetter || isUriScheme) {
                current.push_back(ch);
                continue;
            }
            fields.push_back(Trim(current));
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    if (escaped) {
        current.push_back(L'\\');
    }
    fields.push_back(Trim(current));
    return fields;
}

bool IsCommandCommentLine(std::wstring_view line) {
    if (line.empty()) {
        return true;
    }
    if (line.front() == L'"') {
        return false;
    }
    if (line.size() >= 2 && line.front() == L'\\' && (line[1] == L';' || line[1] == L'#')) {
        return false;
    }
    return line.front() == L'#' || line.front() == L';';
}

std::wstring UnescapeCommandField(std::wstring_view text) {
    std::wstring value;
    value.reserve(text.size());
    bool escaped = false;
    auto isEscapableChar = [](wchar_t ch) {
        return ch == L'\\' || ch == L':' || ch == L'"' || ch == L';' || ch == L'#';
    };
    for (const wchar_t ch : text) {
        if (escaped) {
            if (isEscapableChar(ch)) {
                value.push_back(ch);
            } else {
                value.push_back(L'\\');
                value.push_back(ch);
            }
            escaped = false;
            continue;
        }
        if (ch == L'\\') {
            escaped = true;
            continue;
        }
        value.push_back(ch);
    }
    if (escaped) {
        value.push_back(L'\\');
    }
    return value;
}

std::wstring StripOuterQuotes(std::wstring value) {
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

bool IsCommandBlockStart(std::wstring_view text, std::wstring_view blockName) {
    if (!text.starts_with(blockName)) {
        return false;
    }
    const size_t suffixIndex = blockName.size();
    if (text.size() == suffixIndex + 1 && text[suffixIndex] == L'{') {
        return true;
    }
    return text.size() == suffixIndex + 2 && text[suffixIndex] == L' ' && text[suffixIndex + 1] == L'{';
}

void SkipCommandBlock(std::wistream& stream) {
    std::wstring line;
    int depth = 1;
    while (std::getline(stream, line)) {
        const std::wstring trimmedLine = Trim(line);
        if (IsCommandCommentLine(trimmedLine)) {
            continue;
        }
        if (trimmedLine == L"}") {
            --depth;
            if (depth <= 0) {
                return;
            }
            continue;
        }
        if (trimmedLine == L"{" || trimmedLine.ends_with(L"{")) {
            ++depth;
        }
    }
}

std::vector<std::wstring> ReadNestedCommandBlock(std::wistream& stream) {
    std::vector<std::wstring> lines;
    std::wstring line;
    int depth = 1;
    while (std::getline(stream, line)) {
        const std::wstring trimmedLine = Trim(line);
        if (IsCommandCommentLine(trimmedLine)) {
            lines.push_back(line);
            continue;
        }
        if (trimmedLine == L"}") {
            --depth;
            if (depth <= 0) {
                break;
            }
            lines.push_back(line);
            continue;
        }

        if (trimmedLine == L"{" || trimmedLine.ends_with(L"{")) {
            ++depth;
        }
        lines.push_back(line);
    }
    return lines;
}

bool StripTrailingCommandPropertyBlockStart(std::wstring* line) {
    if (line == nullptr) {
        return false;
    }

    std::wstring trimmedLine = Trim(*line);
    if (trimmedLine.empty() || trimmedLine == L"{") {
        return false;
    }
    if (!trimmedLine.ends_with(L"{")) {
        return false;
    }

    trimmedLine.pop_back();
    *line = Trim(trimmedLine);
    return true;
}

std::optional<std::pair<std::wstring, std::wstring>> ParseCommandPropertyLine(std::wstring_view line) {
    const std::wstring trimmedLine = Trim(line);
    const size_t colonIndex = trimmedLine.find(L':');
    const size_t equalsIndex = trimmedLine.find(L'=');
    size_t separatorIndex = std::wstring::npos;
    if (colonIndex == std::wstring::npos) {
        separatorIndex = equalsIndex;
    } else if (equalsIndex == std::wstring::npos) {
        separatorIndex = colonIndex;
    } else {
        separatorIndex = (std::min)(colonIndex, equalsIndex);
    }

    if (separatorIndex == std::wstring::npos || separatorIndex == 0) {
        return std::nullopt;
    }

    std::wstring key = Lowercase(Trim(trimmedLine.substr(0, separatorIndex)));
    std::wstring value = Trim(trimmedLine.substr(separatorIndex + 1));
    if (key.empty()) {
        return std::nullopt;
    }
    return std::make_pair(std::move(key), std::move(value));
}

std::optional<RunningProcessPolicy> ParseRunningProcessPolicy(std::wstring_view text) {
    const std::wstring value = Lowercase(Trim(text));
    if (value == L"launch" || value == L"start" || value == L"normal" || value == L"parallel") {
        return RunningProcessPolicy::Launch;
    }
    if (value == L"skip" || value == L"ignore" || value == L"do_not_start" || value == L"no_start") {
        return RunningProcessPolicy::Skip;
    }
    if (value == L"restart" || value == L"terminate" || value == L"terminate_previous" || value == L"kill_previous") {
        return RunningProcessPolicy::Restart;
    }
    if (value == L"queue" || value == L"wait" || value == L"wait_previous") {
        return RunningProcessPolicy::Queue;
    }
    return std::nullopt;
}

std::wstring DecodeCommandConfigValue(std::wstring_view value) {
    return ExpandCommandVariables(UnescapeCommandField(StripOuterQuotes(std::wstring(value))));
}

bool TryApplyCommandItemReference(LaunchItem& item, std::wstring_view commandName) {
    const std::wstring resolvedName = DecodeCommandConfigValue(commandName);
    const std::wstring key = NormalizeCommandNameKey(resolvedName);
    if (key.empty()) {
        return false;
    }

    const auto it = g_state.commandItemsByName.find(key);
    if (it == g_state.commandItemsByName.end()) {
        return false;
    }

    item = it->second;
    return true;
}

void ApplyCommandProperty(LaunchItem& item, const std::wstring& key, const std::wstring& rawValue) {
    const std::wstring value = DecodeCommandConfigValue(rawValue);
    if (key == L"command_line" || key == L"command") {
        if (!TryApplyCommandItemReference(item, rawValue)) {
            item.commandLine = value;
            item.description = item.commandLine;
        }
    } else if (key == L"working_directory" || key == L"cwd") {
        item.workingDirectory = value;
    } else if (key == L"show_window") {
        if (!value.empty()) {
            item.showCommand = _wtoi(value.c_str());
        }
    } else if (key == L"priority_class") {
        if (!value.empty()) {
            item.priorityClass = static_cast<DWORD>(_wtoi(value.c_str()));
        }
    } else if (key == L"on_running" || key == L"running_policy" || key == L"previous_process") {
        if (const std::optional<RunningProcessPolicy> policy = ParseRunningProcessPolicy(value)) {
            item.runningProcessPolicy = *policy;
        }
    }
}

void ApplyCommandPropertyBlock(LaunchItem& item, const std::vector<std::wstring>& lines, size_t* index) {
    if (index == nullptr) {
        return;
    }

    while (*index < lines.size()) {
        const std::wstring propertyLine = Trim(lines[*index]);
        ++(*index);
        if (propertyLine == L"}") {
            return;
        }
        if (IsCommandCommentLine(propertyLine)) {
            continue;
        }

        const std::optional<std::pair<std::wstring, std::wstring>> property = ParseCommandPropertyLine(propertyLine);
        if (!property) {
            continue;
        }
        ApplyCommandProperty(item, property->first, property->second);
    }
}

std::vector<std::wstring> SplitStartupCommandFields(std::wstring_view line) {
    std::vector<std::wstring> fields;
    std::wstring current;
    bool inQuotes = false;
    bool escaped = false;
    auto isEscapableChar = [](wchar_t ch) {
        return ch == L'\\' || ch == L':' || ch == L'"' || ch == L';' || ch == L'#';
    };
    auto isSchemeName = [](std::wstring_view value) {
        return !value.empty() && std::all_of(value.begin(), value.end(), [](wchar_t ch) { return iswalpha(ch) != 0; });
    };

    for (size_t index = 0; index < line.size(); ++index) {
        const wchar_t ch = line[index];
        if (escaped) {
            if (isEscapableChar(ch)) {
                current.push_back(ch);
            } else {
                current.push_back(L'\\');
                current.push_back(ch);
            }
            escaped = false;
            continue;
        }
        if (ch == L'\\') {
            escaped = true;
            continue;
        }
        if (ch == L'"') {
            inQuotes = !inQuotes;
            current.push_back(ch);
        } else if (ch == L':' && !inQuotes) {
            const bool isDriveLetter = current.size() == 1 && iswalpha(current[0]) != 0 &&
                index + 1 < line.size() && (line[index + 1] == L'\\' || line[index + 1] == L'/');
            const bool isUriScheme = isSchemeName(current) &&
                ((index + 2 < line.size() && line[index + 1] == L'/' && line[index + 2] == L'/') ||
                 (current == L"shell"));
            if (isDriveLetter || isUriScheme) {
                current.push_back(ch);
                continue;
            }
            fields.push_back(Trim(current));
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    if (escaped) {
        current.push_back(L'\\');
    }
    fields.push_back(Trim(current));
    return fields;
}

bool ApplyCommandOptionsFromFields(LaunchItem& item, const std::vector<std::wstring>& fields, size_t commandFieldIndex) {
    if (commandFieldIndex >= fields.size()) {
        return false;
    }

    if (!TryApplyCommandItemReference(item, fields[commandFieldIndex])) {
        item.commandLine = DecodeCommandConfigValue(fields[commandFieldIndex]);
        item.description = item.commandLine;
    }
    const size_t workingDirectoryFieldIndex = commandFieldIndex + 1;
    if (fields.size() > workingDirectoryFieldIndex) {
        item.workingDirectory = DecodeCommandConfigValue(fields[workingDirectoryFieldIndex]);
    }
    const size_t showCommandFieldIndex = commandFieldIndex + 2;
    if (fields.size() > showCommandFieldIndex && !fields[showCommandFieldIndex].empty()) {
        item.showCommand = _wtoi(fields[showCommandFieldIndex].c_str());
    }
    const size_t priorityClassFieldIndex = commandFieldIndex + 3;
    if (fields.size() > priorityClassFieldIndex && !fields[priorityClassFieldIndex].empty()) {
        item.priorityClass = static_cast<DWORD>(_wtoi(fields[priorityClassFieldIndex].c_str()));
    }
    return !item.commandLine.empty();
}

struct CronLineParts {
    std::wstring minute;
    std::wstring hour;
    std::wstring dayOfMonth;
    std::wstring month;
    std::wstring weekday;
    std::wstring command;
};

std::optional<int> ParseCronInteger(std::wstring_view text) {
    std::wstring value = Trim(text);
    if (value.empty()) {
        return std::nullopt;
    }

    wchar_t* end = nullptr;
    const long parsed = wcstol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != L'\0') {
        return std::nullopt;
    }
    if (parsed < (std::numeric_limits<int>::min)() || parsed > (std::numeric_limits<int>::max)()) {
        return std::nullopt;
    }
    return static_cast<int>(parsed);
}

std::wstring NormalizeCronMinuteKey(const SYSTEMTIME& time) {
    wchar_t buffer[32] {};
    swprintf_s(
        buffer,
        L"%04u%02u%02u%02u%02u",
        time.wYear,
        time.wMonth,
        time.wDay,
        time.wHour,
        time.wMinute);
    return buffer;
}

std::optional<CronLineParts> ParseCronLineParts(std::wstring_view line) {
    CronLineParts parts {};
    size_t position = 0;
    auto readToken = [&](std::wstring* target) -> bool {
        while (position < line.size() && iswspace(line[position]) != 0) {
            ++position;
        }
        if (position >= line.size()) {
            return false;
        }
        const size_t start = position;
        while (position < line.size() && iswspace(line[position]) == 0) {
            ++position;
        }
        *target = std::wstring(line.substr(start, position - start));
        return true;
    };

    if (!readToken(&parts.minute) ||
        !readToken(&parts.hour) ||
        !readToken(&parts.dayOfMonth) ||
        !readToken(&parts.month) ||
        !readToken(&parts.weekday)) {
        return std::nullopt;
    }

    while (position < line.size() && iswspace(line[position]) != 0) {
        ++position;
    }
    if (position >= line.size()) {
        return std::nullopt;
    }

    parts.command = Trim(line.substr(position));
    if (parts.command.empty()) {
        return std::nullopt;
    }
    return parts;
}

bool TrySetCronValue(std::vector<bool>& allowed, int minValue, int maxValue, int value) {
    if (value < minValue || value > maxValue) {
        return false;
    }
    if (value >= static_cast<int>(allowed.size())) {
        return false;
    }
    allowed[static_cast<size_t>(value)] = true;
    return true;
}

bool ParseCronFieldSpec(std::wstring_view text, int minValue, int maxValue, bool sundaySevenAlias, CronFieldSpec* spec) {
    if (spec == nullptr) {
        return false;
    }

    spec->wildcard = false;
    spec->allowed.assign(static_cast<size_t>(maxValue + 1), false);

    const std::wstring value = Trim(text);
    if (value.empty()) {
        return false;
    }

    size_t segmentStart = 0;
    auto normalizeValue = [&](int rawValue) -> std::optional<int> {
        if (sundaySevenAlias && rawValue == 7) {
            return 0;
        }
        if (rawValue < minValue || rawValue > maxValue) {
            return std::nullopt;
        }
        return rawValue;
    };

    while (segmentStart <= value.size()) {
        const size_t segmentEnd = value.find(L',', segmentStart);
        const std::wstring segment = Trim(value.substr(segmentStart, segmentEnd == std::wstring::npos ? std::wstring::npos : segmentEnd - segmentStart));
        if (segment.empty()) {
            return false;
        }

        if (segment == L"*" || segment == L"?") {
            spec->wildcard = true;
            spec->allowed.assign(static_cast<size_t>(maxValue + 1), true);
            return true;
        }

        const size_t slashIndex = segment.find(L'/');
        const std::wstring basePart = Trim(segment.substr(0, slashIndex));
        const std::wstring stepPart = slashIndex == std::wstring::npos ? L"" : Trim(segment.substr(slashIndex + 1));

        int step = 1;
        if (!stepPart.empty()) {
            const std::optional<int> parsedStep = ParseCronInteger(stepPart);
            if (!parsedStep || *parsedStep <= 0) {
                return false;
            }
            step = *parsedStep;
        }

        int rangeStart = minValue;
        int rangeEnd = maxValue;
        if (basePart != L"*") {
            const size_t dashIndex = basePart.find(L'-');
            if (dashIndex == std::wstring::npos) {
                const std::optional<int> parsedValue = ParseCronInteger(basePart);
                if (!parsedValue) {
                    return false;
                }
                const std::optional<int> normalized = normalizeValue(*parsedValue);
                if (!normalized) {
                    return false;
                }
                rangeStart = *normalized;
                rangeEnd = maxValue;
            } else {
                const std::wstring startPart = Trim(basePart.substr(0, dashIndex));
                const std::wstring endPart = Trim(basePart.substr(dashIndex + 1));
                const std::optional<int> parsedStart = ParseCronInteger(startPart);
                const std::optional<int> parsedEnd = ParseCronInteger(endPart);
                if (!parsedStart || !parsedEnd) {
                    return false;
                }
                if (sundaySevenAlias && (*parsedStart == 7 || *parsedEnd == 7)) {
                    return false;
                }
                rangeStart = *parsedStart;
                rangeEnd = *parsedEnd;
                if (rangeStart < minValue || rangeStart > maxValue || rangeEnd < minValue || rangeEnd > maxValue || rangeStart > rangeEnd) {
                    return false;
                }
            }
        }

        for (int valueIndex = rangeStart; valueIndex <= rangeEnd; valueIndex += step) {
            if (!TrySetCronValue(spec->allowed, minValue, maxValue, valueIndex)) {
                return false;
            }
        }

        if (segmentEnd == std::wstring::npos) {
            break;
        }
        segmentStart = segmentEnd + 1;
    }

    return true;
}

bool CronFieldMatches(const CronFieldSpec& spec, int value) {
    if (spec.wildcard) {
        return true;
    }
    if (value < 0 || value >= static_cast<int>(spec.allowed.size())) {
        return false;
    }
    return spec.allowed[static_cast<size_t>(value)];
}

bool CronTaskMatchesTime(const CronTask& task, const SYSTEMTIME& time) {
    const int weekday = static_cast<int>(time.wDayOfWeek);
    const bool minuteMatches = CronFieldMatches(task.minute, static_cast<int>(time.wMinute));
    const bool hourMatches = CronFieldMatches(task.hour, static_cast<int>(time.wHour));
    const bool monthMatches = CronFieldMatches(task.month, static_cast<int>(time.wMonth));
    const bool dayOfMonthMatches = CronFieldMatches(task.dayOfMonth, static_cast<int>(time.wDay));
    const bool weekdayMatches = CronFieldMatches(task.weekday, weekday);

    const bool dayOfMonthWildcard = task.dayOfMonth.wildcard;
    const bool weekdayWildcard = task.weekday.wildcard;
    bool dayMatches = false;
    if (dayOfMonthWildcard && weekdayWildcard) {
        dayMatches = true;
    } else if (dayOfMonthWildcard) {
        dayMatches = weekdayMatches;
    } else if (weekdayWildcard) {
        dayMatches = dayOfMonthMatches;
    } else {
        dayMatches = dayOfMonthMatches || weekdayMatches;
    }

    return minuteMatches && hourMatches && monthMatches && dayMatches;
}

void CloseProcessHandle(HANDLE* process) {
    if (process != nullptr && *process != nullptr) {
        CloseHandle(*process);
        *process = nullptr;
    }
}

bool IsProcessStillRunning(HANDLE process) {
    return process != nullptr && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
}

void RefreshCronTaskProcessState(CronTask& task) {
    if (task.runningProcess != nullptr && !IsProcessStillRunning(task.runningProcess)) {
        CloseProcessHandle(&task.runningProcess);
    }
}

void CloseCronTaskProcessHandles() {
    for (CronTask& task : g_state.cronTasks) {
        CloseProcessHandle(&task.runningProcess);
        task.queuedLaunchCount = 0;
    }
}

bool LaunchCronTask(CronTask& task) {
    HANDLE launchedProcess = nullptr;
    const bool shouldTrackProcess = task.item.runningProcessPolicy != RunningProcessPolicy::Launch;
    HANDLE* launchedProcessTarget = shouldTrackProcess ? &launchedProcess : nullptr;
    bool launched = false;
    if (task.item.inlineBatchScript.empty()) {
        launched = LaunchProcessCommand(
            task.item.commandLine,
            task.item.workingDirectory,
            task.item.showCommand,
            task.item.priorityClass,
            0,
            launchedProcessTarget);
    } else {
        launched = LaunchInlineBatchScript(task.item, launchedProcessTarget);
    }
    if (!launched) {
        CloseProcessHandle(&launchedProcess);
        return false;
    }

    if (shouldTrackProcess) {
        CloseProcessHandle(&task.runningProcess);
        task.runningProcess = launchedProcess;
    }
    return true;
}

void RunQueuedCronLaunches(CronTask& task) {
    while (task.queuedLaunchCount > 0) {
        RefreshCronTaskProcessState(task);
        if (IsProcessStillRunning(task.runningProcess)) {
            return;
        }

        --task.queuedLaunchCount;
        if (!LaunchCronTask(task)) {
            task.queuedLaunchCount = 0;
            return;
        }
    }
}

void RequestCronTaskLaunch(CronTask& task) {
    RefreshCronTaskProcessState(task);
    if (!IsProcessStillRunning(task.runningProcess)) {
        LaunchCronTask(task);
        return;
    }

    if (task.item.runningProcessPolicy == RunningProcessPolicy::Launch) {
        LaunchCronTask(task);
    } else if (task.item.runningProcessPolicy == RunningProcessPolicy::Skip) {
        return;
    } else if (task.item.runningProcessPolicy == RunningProcessPolicy::Restart) {
        TerminateProcess(task.runningProcess, 1);
        WaitForSingleObject(task.runningProcess, kCronRestartTerminateWaitMs);
        CloseProcessHandle(&task.runningProcess);
        LaunchCronTask(task);
    } else if (task.item.runningProcessPolicy == RunningProcessPolicy::Queue) {
        ++task.queuedLaunchCount;
    }
}

void LoadCommandItems(bool appendToResultItems = true) {
    g_state.commandItemsByName.clear();
    std::wifstream stream(FindConfigPath(kCommandFileName));
    if (!stream.is_open()) {
        return;
    }
    stream.imbue(std::locale(".UTF-8"));

    std::wstring line;
    while (std::getline(stream, line)) {
        const std::wstring trimmedLine = Trim(line);
        if (IsCommandCommentLine(trimmedLine)) {
            continue;
        }
        if (IsCommandBlockStart(trimmedLine, L"STARTUP")) {
            SkipCommandBlock(stream);
            continue;
        }
        if (IsCommandBlockStart(trimmedLine, L"CRON")) {
            SkipCommandBlock(stream);
            continue;
        }

        std::vector<std::wstring> fields = SplitCommandFields(trimmedLine);
        if (fields.size() < 2) {
            continue;
        }

        LaunchItem item {};
        item.name = ExpandCommandVariables(UnescapeCommandField(StripOuterQuotes(fields[0])));
        item.sourceKind = ItemSourceKind::CommandConf;

        bool startsInlineBatch = false;
        if (fields[1].empty() && trimmedLine.ends_with(L"{") && !fields.empty()) {
            if (fields.back() == L"{") {
                fields.pop_back();
                startsInlineBatch = true;
            } else if (!fields.back().empty() && fields.back().back() == L'{') {
                fields.back().pop_back();
                fields.back() = Trim(fields.back());
                startsInlineBatch = true;
            }
        }

        if (startsInlineBatch) {
            if (fields.size() > 2 && !fields[2].empty()) {
                item.workingDirectory = ExpandCommandVariables(UnescapeCommandField(StripOuterQuotes(fields[2])));
            }
            if (fields.size() > 3 && !fields[3].empty()) {
                item.showCommand = _wtoi(fields[3].c_str());
            }
            if (fields.size() > 4 && !fields[4].empty()) {
                item.priorityClass = static_cast<DWORD>(_wtoi(fields[4].c_str()));
            }

            std::wstring script;
            bool closed = false;
            while (std::getline(stream, line)) {
                if (Trim(line) == L"}") {
                    closed = true;
                    break;
                }
                script += line;
                script += L"\n";
            }
            if (!closed) {
                continue;
            }
            item.inlineBatchScript = std::move(script);
            item.description = item.inlineBatchScript;
        } else {
            if (!ApplyCommandOptionsFromFields(item, fields, 1)) {
                continue;
            }
        }

        PopulateSearchFields(item);
        item.itemKey = BuildItemKey(item.sourceKind, BuildCommandIdentity(item), item.workingDirectory);
        const std::wstring nameKey = NormalizeCommandNameKey(item.name);
        if (!nameKey.empty()) {
            g_state.commandItemsByName.insert_or_assign(nameKey, item);
        }
        if (appendToResultItems) {
            g_state.items.push_back(std::move(item));
        }
    }
}

void LoadCronTasks() {
    CloseCronTaskProcessHandles();
    g_state.cronTasks.clear();

    std::wifstream stream(FindConfigPath(kCommandFileName));
    if (!stream.is_open()) {
        return;
    }
    stream.imbue(std::locale(".UTF-8"));

    std::wstring line;
    while (std::getline(stream, line)) {
        const std::wstring trimmedLine = Trim(line);
        if (IsCommandCommentLine(trimmedLine) || !IsCommandBlockStart(trimmedLine, L"CRON")) {
            continue;
        }

        const std::vector<std::wstring> cronLines = ReadNestedCommandBlock(stream);
        for (size_t index = 0; index < cronLines.size(); ++index) {
            std::wstring cronLine = Trim(cronLines[index]);
            if (IsCommandCommentLine(cronLine)) {
                continue;
            }

            const bool hasPropertyBlock = StripTrailingCommandPropertyBlockStart(&cronLine);
            const std::optional<CronLineParts> parsedLine = ParseCronLineParts(cronLine);
            if (!parsedLine) {
                continue;
            }

            CronTask task {};
            if (!ParseCronFieldSpec(parsedLine->minute, 0, 59, false, &task.minute) ||
                !ParseCronFieldSpec(parsedLine->hour, 0, 23, false, &task.hour) ||
                !ParseCronFieldSpec(parsedLine->dayOfMonth, 1, 31, false, &task.dayOfMonth) ||
                !ParseCronFieldSpec(parsedLine->month, 1, 12, false, &task.month) ||
                !ParseCronFieldSpec(parsedLine->weekday, 0, 7, true, &task.weekday)) {
                continue;
            }

            const std::vector<std::wstring> commandFields = SplitStartupCommandFields(parsedLine->command);
            if (!ApplyCommandOptionsFromFields(task.item, commandFields, 0)) {
                continue;
            }
            if (hasPropertyBlock) {
                ++index;
                ApplyCommandPropertyBlock(task.item, cronLines, &index);
            } else if (index + 1 < cronLines.size() && Trim(cronLines[index + 1]) == L"{") {
                index += 2;
                ApplyCommandPropertyBlock(task.item, cronLines, &index);
            }

            g_state.cronTasks.push_back(std::move(task));
        }
    }
}

void RunStartupCommands() {
    std::wifstream stream(FindConfigPath(kCommandFileName));
    if (!stream.is_open()) {
        return;
    }
    stream.imbue(std::locale(".UTF-8"));

    std::wstring line;
    while (std::getline(stream, line)) {
        const std::wstring trimmedLine = Trim(line);
        if (IsCommandCommentLine(trimmedLine) || !IsCommandBlockStart(trimmedLine, L"STARTUP")) {
            continue;
        }

        const std::vector<std::wstring> startupLines = ReadNestedCommandBlock(stream);
        for (size_t index = 0; index < startupLines.size(); ++index) {
            std::wstring startupLine = Trim(startupLines[index]);
            if (IsCommandCommentLine(startupLine)) {
                continue;
            }

            const bool hasPropertyBlock = StripTrailingCommandPropertyBlockStart(&startupLine);
            LaunchItem item {};
            const std::vector<std::wstring> fields = SplitStartupCommandFields(startupLine);
            if (!ApplyCommandOptionsFromFields(item, fields, 0)) {
                continue;
            }
            if (hasPropertyBlock) {
                ++index;
                ApplyCommandPropertyBlock(item, startupLines, &index);
            } else if (index + 1 < startupLines.size() && Trim(startupLines[index + 1]) == L"{") {
                index += 2;
                ApplyCommandPropertyBlock(item, startupLines, &index);
            }

            if (item.inlineBatchScript.empty()) {
                LaunchProcessCommand(item.commandLine, item.workingDirectory, item.showCommand, item.priorityClass);
            } else {
                LaunchInlineBatchScript(item);
            }
        }
    }
}

void RunCronTasks() {
    if (g_state.cronTasks.empty()) {
        return;
    }

    SYSTEMTIME localTime {};
    GetLocalTime(&localTime);
    const std::wstring minuteKey = NormalizeCronMinuteKey(localTime);

    for (CronTask& task : g_state.cronTasks) {
        RunQueuedCronLaunches(task);
        if (!CronTaskMatchesTime(task, localTime)) {
            continue;
        }
        if (task.lastRunMinuteKey == minuteKey) {
            continue;
        }

        RequestCronTaskLaunch(task);
        task.lastRunMinuteKey = minuteKey;
    }
}

void StartCronTimer() {
    if (g_state.hostWindow != nullptr) {
        SetTimer(g_state.hostWindow, kCronTimerId, kCronTimerIntervalMs, nullptr);
    }
}

bool ShouldRunStartupCommands() {
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments == nullptr) {
        return false;
    }

    bool shouldRun = false;
    for (int index = 1; index < argumentCount; ++index) {
        std::wstring_view argument(arguments[index]);
        if (argument == L"/startup" || argument == L"-startup" || argument == L"--startup") {
            shouldRun = true;
            break;
        }
    }

    LocalFree(arguments);
    return shouldRun;
}

void LoadBuiltinCommandItems() {
    for (const BuiltinCommandDefinition& command : kBuiltinCommands) {
        LaunchItem item {};
        item.name = std::wstring(command.name) + L" - " + command.description;
        item.commandLine = std::wstring(kBuiltinCommandPrefix) + command.token;
        item.description = command.description;
        item.sourceKind = ItemSourceKind::Synthetic;
        PopulateSearchFields(item);
        item.itemKey = BuildItemKey(item.sourceKind, item.commandLine, item.workingDirectory);
        g_state.items.push_back(std::move(item));
    }
}

void AppendFilesystemItemWithDedup(
    std::vector<LaunchItem>& target,
    std::unordered_set<std::wstring>& seen,
    LaunchItem item) {
    const std::wstring key = Lowercase(item.name + L"|" + item.commandLine);
    if (!seen.insert(key).second) {
        return;
    }
    target.push_back(std::move(item));
}

bool SaveFilesystemCache(const std::vector<LaunchItem>& scannedItems) {
    std::wstring content = BuildScanDirectorySignature();
    content += L"\n";
    for (const LaunchItem& item : scannedItems) {
        content += EscapeCacheField(item.name);
        content += L"\t";
        content += EscapeCacheField(item.commandLine);
        content += L"\t";
        content += EscapeCacheField(item.description);
        content += L"\t";
        content += EscapeCacheField(item.workingDirectory);
        content += L"\n";
    }
    std::error_code error;
    const std::filesystem::path cachePath = GetFilesystemCachePath();
    const std::filesystem::path directory = cachePath.parent_path();
    if (!directory.empty()) {
        std::filesystem::create_directories(directory, error);
        if (error) {
            return false;
        }
    }
    return SaveUtf8TextFile(cachePath.wstring(), content);
}

bool LoadFilesystemItemsFromCache(std::vector<LaunchItem>& destination, std::unordered_set<std::wstring>& seen) {
    const std::wstring content = LoadUtf8TextFile(GetFilesystemCachePath());
    if (content.empty()) {
        return false;
    }

    std::wstringstream stream(content);
    std::wstring line;
    if (!std::getline(stream, line)) {
        return false;
    }
    if (!line.empty() && line.back() == L'\r') {
        line.pop_back();
    }
    if (line != kFilesystemCacheMagic) {
        return false;
    }

    std::wstring expectedSignature = BuildScanDirectorySignature();
    std::wstring actualSignature = line;
    const size_t expectedSignatureLineCount = static_cast<size_t>(1 + g_state.scanDirectories.size());
    for (size_t index = 1; index < expectedSignatureLineCount; ++index) {
        if (!std::getline(stream, line)) {
            return false;
        }
        if (!line.empty() && line.back() == L'\r') {
            line.pop_back();
        }
        actualSignature += L"\n";
        actualSignature += line;
    }
    if (actualSignature != expectedSignature) {
        return false;
    }

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == L'\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }

        std::vector<std::wstring> fields;
        std::wstring current;
        bool escaped = false;
        for (const wchar_t ch : line) {
            if (!escaped && ch == L'\t') {
                fields.push_back(std::move(current));
                current.clear();
                continue;
            }
            if (ch == L'\\' && !escaped) {
                escaped = true;
                current.push_back(ch);
                continue;
            }
            escaped = false;
            current.push_back(ch);
        }
        fields.push_back(std::move(current));
        if (fields.size() != 4) {
            continue;
        }

        LaunchItem item {};
        item.name = UnescapeCacheField(fields[0]);
        item.commandLine = UnescapeCacheField(fields[1]);
        item.description = UnescapeCacheField(fields[2]);
        item.workingDirectory = UnescapeCacheField(fields[3]);
        item.sourceKind = ItemSourceKind::ScannedFile;
        PopulateSearchFields(item);
        item.itemKey = BuildItemKey(item.sourceKind, item.commandLine, item.workingDirectory);
        AppendFilesystemItemWithDedup(destination, seen, std::move(item));
    }

    return true;
}

void ScanFilesystemItems(std::vector<LaunchItem>& destination, std::unordered_set<std::wstring>& seen) {
    std::vector<LaunchItem> scannedItems;

    for (const ScanDirectoryConfig& directory : g_state.scanDirectories) {
        std::error_code error;
        if (!std::filesystem::exists(directory.path, error)) {
            continue;
        }

        auto appendItem = [&](const std::filesystem::path& path) {
            const std::wstring extension = Lowercase(path.extension().wstring());
            if (directory.extensions.find(extension) == directory.extensions.end()) {
                return;
            }

            LaunchItem item {};
            item.name = path.stem().wstring();
            item.commandLine = path.wstring();
            item.workingDirectory = path.parent_path().wstring();
            item.description = extension == L".lnk" ? ResolveShortcutCommandLine(item.commandLine) : item.commandLine;
            if (item.description.empty()) {
                item.description = item.commandLine;
            }
            item.sourceKind = ItemSourceKind::ScannedFile;
            PopulateSearchFields(item);
            item.itemKey = BuildItemKey(item.sourceKind, item.commandLine, item.workingDirectory);
            AppendFilesystemItemWithDedup(scannedItems, seen, std::move(item));
        };

        if (!directory.recursive) {
            for (std::filesystem::directory_iterator it(directory.path, std::filesystem::directory_options::skip_permission_denied, error), end; it != end; it.increment(error)) {
                if (error) {
                    error.clear();
                    continue;
                }
                if (!it->is_regular_file(error)) {
                    continue;
                }
                appendItem(it->path());
            }
            continue;
        }

        for (std::filesystem::recursive_directory_iterator it(directory.path, std::filesystem::directory_options::skip_permission_denied, error), end; it != end; it.increment(error)) {
            if (error) {
                error.clear();
                continue;
            }
            if (it->is_directory(error)) {
                if (IsExcludedScanPath(directory, it->path())) {
                    it.disable_recursion_pending();
                }
                continue;
            }
            if (!it->is_regular_file(error)) {
                continue;
            }
            if (IsExcludedScanPath(directory, it->path())) {
                continue;
            }
            appendItem(it->path());
        }
    }

    SaveFilesystemCache(scannedItems);
    for (LaunchItem& item : scannedItems) {
        destination.push_back(std::move(item));
    }
}

bool LoadFilesystemItems(bool allowCache) {
    std::unordered_set<std::wstring> seen;
    for (const LaunchItem& item : g_state.items) {
        seen.insert(Lowercase(item.name + L"|" + item.commandLine));
    }

    if (allowCache && LoadFilesystemItemsFromCache(g_state.items, seen)) {
        std::sort(g_state.items.begin(), g_state.items.end(), [](const LaunchItem& lhs, const LaunchItem& rhs) {
            return _wcsicmp(lhs.name.c_str(), rhs.name.c_str()) < 0;
        });
        return true;
    }

    ScanFilesystemItems(g_state.items, seen);
    std::sort(g_state.items.begin(), g_state.items.end(), [](const LaunchItem& lhs, const LaunchItem& rhs) {
        return _wcsicmp(lhs.name.c_str(), rhs.name.c_str()) < 0;
    });
    return false;
}

bool ReloadItems(bool allowFilesystemCache) {
    const std::wstring content = LoadUtf8TextFile(FindConfigPath(kConfigFileName));
    g_state.items.clear();
    g_state.lastSearch = SearchQueryState {};
    LoadScanDirectories(content);
    LoadCommandItems();
    LoadCronTasks();
    const bool loadedFilesystemCache = LoadFilesystemItems(allowFilesystemCache);
    LoadBuiltinCommandItems();
    return loadedFilesystemCache;
}

void RequestLauncherDataRefresh(bool refreshItems, bool refreshHistory) {
    g_state.pendingItemsRefresh = g_state.pendingItemsRefresh || refreshItems;
    g_state.pendingHistoryRefresh = g_state.pendingHistoryRefresh || refreshHistory;
    if (g_state.refreshScheduled || g_state.hostWindow == nullptr) {
        return;
    }
    g_state.refreshScheduled = true;
    PostMessageW(g_state.hostWindow, kMessageRefreshLauncherData, 0, 0);
}

void UpdateSelectedItemDescription() {
    if (g_state.descriptionText == nullptr || g_state.resultList == nullptr) {
        return;
    }

    const LRESULT selected = SendMessageW(g_state.resultList, LB_GETCURSEL, 0, 0);
    if (selected == LB_ERR) {
        SetWindowTextW(g_state.descriptionText, L"");
        return;
    }

    const LRESULT itemIndex = SendMessageW(g_state.resultList, LB_GETITEMDATA, static_cast<WPARAM>(selected), 0);
    if (itemIndex == LB_ERR || static_cast<size_t>(itemIndex) >= g_state.items.size()) {
        SetWindowTextW(g_state.descriptionText, L"");
        return;
    }

    SetWindowTextW(g_state.descriptionText, g_state.items[static_cast<size_t>(itemIndex)].description.c_str());
}

void RequestResultListRebuild() {
    if (g_state.resultRebuildScheduled || g_state.launcherWindow == nullptr) {
        return;
    }
    g_state.resultRebuildScheduled = true;
    PostMessageW(g_state.launcherWindow, kMessageRebuildResults, 0, 0);
}

void ArmDeferredRefresh() {
    if (g_state.launcherWindow == nullptr) {
        return;
    }
    g_state.deferredRefreshArmed = true;
    SetTimer(g_state.launcherWindow, kDeferredRefreshTimerId, kDeferredRefreshCheckIntervalMs, nullptr);
}

void CancelDeferredRefresh() {
    if (g_state.launcherWindow != nullptr) {
        KillTimer(g_state.launcherWindow, kDeferredRefreshTimerId);
    }
    g_state.deferredRefreshArmed = false;
}

void RecordSearchInputActivity() {
    g_state.lastSearchInputTick = GetTickCount64();
}

bool ShouldDeferRefreshForActiveTyping() {
    if (g_state.launcherWindow == nullptr || IsWindowVisible(g_state.launcherWindow) == FALSE) {
        return false;
    }
    if (g_state.lastSearchInputTick == 0) {
        return false;
    }
    const ULONGLONG now = GetTickCount64();
    return (now - g_state.lastSearchInputTick) < kDeferredRefreshIdleDelayMs;
}

void RefreshLauncherDataIfNeeded(bool forceItems, bool forceHistory) {
    const ULONGLONG now = GetTickCount64();
    const bool shouldRefreshItems = forceItems ||
        !g_state.launcherDataLoaded ||
        (now - g_state.lastItemsRefreshTick) >= kItemsRefreshIntervalMs;
    const bool shouldRefreshHistory = forceHistory ||
        !g_state.launcherDataLoaded ||
        (now - g_state.lastHistoryRefreshTick) >= kHistoryRefreshIntervalMs;

    bool rebuiltItems = false;
    bool loadedFilesystemCache = false;
    if (shouldRefreshItems) {
        loadedFilesystemCache = ReloadItems(!g_state.filesystemCachePrimed);
        if (loadedFilesystemCache) {
            g_state.filesystemCachePrimed = true;
            g_state.filesystemCachePendingRescan = true;
            g_state.lastItemsRefreshTick = 0;
        } else {
            g_state.filesystemCachePrimed = true;
            g_state.filesystemCachePendingRescan = false;
            g_state.lastItemsRefreshTick = now;
        }
        rebuiltItems = true;
    }
    if (shouldRefreshHistory) {
        LoadHistoryCache();
        g_state.lastHistoryRefreshTick = now;
    }

    if (rebuiltItems || shouldRefreshHistory || !g_state.launcherDataLoaded) {
        RebuildResultList();
    }
    g_state.launcherDataLoaded = true;

    if (loadedFilesystemCache && g_state.filesystemCachePendingRescan) {
        RequestLauncherDataRefresh(true, false);
    }
}

std::wstring GetControlText(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring text(static_cast<size_t>(length), L'\0');
    GetWindowTextW(window, text.data(), length + 1);
    return text;
}

struct RankedResult {
    size_t itemIndex = 0;
    int matchScore = 0;
    double rank = 0.0;
    int recencyBonus = 0;
    double finalScore = 0.0;
    std::wstring lastRunUtc;
};

void RebuildResultList() {
    const std::wstring query = Trim(GetControlText(g_state.searchEdit));
    const bool builtinQuery = !query.empty() && query.front() == L'/';
    const std::wstring normalizedQuery = Lowercase(query);
    std::vector<std::wstring> normalizedTokens = SplitQueryTokens(normalizedQuery);

    SendMessageW(g_state.resultList, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_state.resultList, LB_RESETCONTENT, 0, 0);

    std::vector<RankedResult> rankedResults;
    rankedResults.reserve(g_state.items.size());
    std::vector<size_t> candidateIndices;
    const bool canReuseLastMatches =
        g_state.lastSearch.valid &&
        g_state.lastSearch.builtinQuery == builtinQuery &&
        !g_state.lastSearch.normalizedQuery.empty() &&
        normalizedQuery.starts_with(g_state.lastSearch.normalizedQuery);

    if (canReuseLastMatches) {
        candidateIndices = g_state.lastSearch.matchedItemIndices;
    } else {
        candidateIndices.reserve(g_state.items.size());
        for (size_t index = 0; index < g_state.items.size(); ++index) {
            candidateIndices.push_back(index);
        }
    }

    std::vector<size_t> matchedItemIndices;
    matchedItemIndices.reserve(candidateIndices.size());

    for (const size_t index : candidateIndices) {
        const LaunchItem& item = g_state.items[index];
        if (builtinQuery && item.sourceKind != ItemSourceKind::Synthetic) {
            continue;
        }
        if (!builtinQuery && item.sourceKind == ItemSourceKind::Synthetic) {
            continue;
        }
        const int matchScore = ComputeMatchScore(item, normalizedTokens);
        if (matchScore < 0) {
            continue;
        }

        RankedResult result {};
        result.itemIndex = index;
        result.matchScore = matchScore;
        if (item.sourceKind != ItemSourceKind::Synthetic) {
            if (const auto it = g_state.historyByKey.find(item.itemKey); it != g_state.historyByKey.end()) {
                result.rank = it->second.rank;
                result.lastRunUtc = it->second.lastRunUtc;
                result.recencyBonus = ComputeRecencyBonus(it->second.lastRunUtc);
            }
        }
        result.finalScore = static_cast<double>(matchScore) * 1000.0 + result.rank * 20.0 + static_cast<double>(result.recencyBonus);
        rankedResults.push_back(std::move(result));
        matchedItemIndices.push_back(index);
    }

    std::sort(rankedResults.begin(), rankedResults.end(), [](const RankedResult& lhs, const RankedResult& rhs) {
        if (lhs.finalScore != rhs.finalScore) {
            return lhs.finalScore > rhs.finalScore;
        }
        if (lhs.matchScore != rhs.matchScore) {
            return lhs.matchScore > rhs.matchScore;
        }
        if (lhs.rank != rhs.rank) {
            return lhs.rank > rhs.rank;
        }
        if (lhs.lastRunUtc != rhs.lastRunUtc) {
            return lhs.lastRunUtc > rhs.lastRunUtc;
        }
        const LaunchItem& lhsItem = g_state.items[lhs.itemIndex];
        const LaunchItem& rhsItem = g_state.items[rhs.itemIndex];
        return _wcsicmp(lhsItem.name.c_str(), rhsItem.name.c_str()) < 0;
    });

    for (const RankedResult& result : rankedResults) {
        const LaunchItem& item = g_state.items[result.itemIndex];
        const LRESULT inserted = SendMessageW(g_state.resultList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.name.c_str()));
        if (inserted >= 0) {
            SendMessageW(g_state.resultList, LB_SETITEMDATA, static_cast<WPARAM>(inserted), static_cast<LPARAM>(result.itemIndex));
        }
    }

    if (SendMessageW(g_state.resultList, LB_GETCOUNT, 0, 0) > 0) {
        SendMessageW(g_state.resultList, LB_SETCURSEL, 0, 0);
    }
    SendMessageW(g_state.resultList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_state.resultList, nullptr, TRUE);

    g_state.lastSearch.rawQuery = query;
    g_state.lastSearch.normalizedQuery = std::move(normalizedQuery);
    g_state.lastSearch.normalizedTokens = std::move(normalizedTokens);
    g_state.lastSearch.matchedItemIndices = std::move(matchedItemIndices);
    g_state.lastSearch.builtinQuery = builtinQuery;
    g_state.lastSearch.valid = true;
    UpdateSelectedItemDescription();
}

void RecordSuccessfulLaunch(const LaunchItem& item) {
    if (item.sourceKind == ItemSourceKind::Synthetic || !g_state.historyConfig.enabled) {
        return;
    }

    sqlite3* database = nullptr;
    if (!OpenHistoryDatabase(&database)) {
        return;
    }

    const std::wstring nowUtc = GetCurrentUtcTimestamp();
    bool success = ExecuteSql(database, "BEGIN IMMEDIATE TRANSACTION;");

    sqlite3_stmt* upsertStatement = nullptr;
    const wchar_t* upsertSql =
        L"INSERT INTO launch_history ("
        L"    item_key, display_name, command_line, working_directory, source_kind,"
        L"    run_count, rank, last_run_utc, created_utc, updated_utc"
        L") VALUES ("
        L"    ?, ?, ?, ?, ?,"
        L"    1, 1.0, ?, ?, ?"
        L") ON CONFLICT(item_key) DO UPDATE SET "
        L"    display_name = excluded.display_name,"
        L"    command_line = excluded.command_line,"
        L"    working_directory = excluded.working_directory,"
        L"    source_kind = excluded.source_kind,"
        L"    run_count = launch_history.run_count + 1,"
        L"    rank = launch_history.rank + 1.0,"
        L"    last_run_utc = excluded.last_run_utc,"
        L"    updated_utc = excluded.updated_utc;";

    if (success && PrepareStatement(database, upsertSql, &upsertStatement)) {
        success = BindText(upsertStatement, 1, item.itemKey) &&
            BindText(upsertStatement, 2, item.name) &&
            BindText(upsertStatement, 3, item.commandLine) &&
            BindText(upsertStatement, 4, item.workingDirectory) &&
            sqlite3_bind_int(upsertStatement, 5, static_cast<int>(item.sourceKind)) == SQLITE_OK &&
            BindText(upsertStatement, 6, nowUtc) &&
            BindText(upsertStatement, 7, nowUtc) &&
            BindText(upsertStatement, 8, nowUtc) &&
            sqlite3_step(upsertStatement) == SQLITE_DONE;
    } else {
        success = false;
    }
    FinalizeStatement(upsertStatement);

    double totalRank = 0.0;
    sqlite3_stmt* sumStatement = nullptr;
    if (success && PrepareStatement(database, L"SELECT COALESCE(SUM(rank), 0) FROM launch_history;", &sumStatement)) {
        if (sqlite3_step(sumStatement) == SQLITE_ROW) {
            totalRank = sqlite3_column_double(sumStatement, 0);
        } else {
            success = false;
        }
    } else if (success) {
        success = false;
    }
    FinalizeStatement(sumStatement);

    if (success && totalRank > g_state.historyConfig.rankSumLimit) {
        sqlite3_stmt* decayStatement = nullptr;
        if (PrepareStatement(database, L"UPDATE launch_history SET rank = rank * ?, updated_utc = ?;", &decayStatement)) {
            success = sqlite3_bind_double(decayStatement, 1, g_state.historyConfig.decayFactor) == SQLITE_OK &&
                BindText(decayStatement, 2, nowUtc) &&
                sqlite3_step(decayStatement) == SQLITE_DONE;
        } else {
            success = false;
        }
        FinalizeStatement(decayStatement);
    }

    sqlite3_stmt* pruneStatement = nullptr;
    if (success && PrepareStatement(database, L"DELETE FROM launch_history WHERE rank < ?;", &pruneStatement)) {
        success = sqlite3_bind_double(pruneStatement, 1, g_state.historyConfig.pruneBelow) == SQLITE_OK &&
            sqlite3_step(pruneStatement) == SQLITE_DONE;
    } else if (success) {
        success = false;
    }
    FinalizeStatement(pruneStatement);

    sqlite3_stmt* trimStatement = nullptr;
    if (success && PrepareStatement(database,
            L"DELETE FROM launch_history WHERE id IN ("
            L"    SELECT id FROM launch_history "
            L"    ORDER BY rank DESC, last_run_utc DESC "
            L"    LIMIT -1 OFFSET ?"
            L");",
            &trimStatement)) {
        success = sqlite3_bind_int(trimStatement, 1, g_state.historyConfig.maxRows) == SQLITE_OK &&
            sqlite3_step(trimStatement) == SQLITE_DONE;
    } else if (success) {
        success = false;
    }
    FinalizeStatement(trimStatement);

    ExecuteSql(database, success ? "COMMIT;" : "ROLLBACK;");
    sqlite3_close(database);

    if (success) {
        HistoryInfo& info = g_state.historyByKey[item.itemKey];
        info.rank += 1.0;
        info.runCount += 1;
        info.lastRunUtc = nowUtc;
    }
}

bool LaunchItemByIndex(size_t index) {
    if (index >= g_state.items.size()) {
        return false;
    }

    const LaunchItem& item = g_state.items[index];
    if (item.sourceKind == ItemSourceKind::Synthetic) {
        const std::wstring command = Lowercase(item.commandLine);
        bool success = false;
        if (command == L"builtin:quit") {
            PostMessageW(g_state.hostWindow, WM_CLOSE, 0, 0);
            success = true;
        } else if (command == L"builtin:reload") {
            LoadHotkeyConfig();
            LoadHistoryConfig();
            LoadLauncherAppearanceConfig();
            LoadCommandVariables();
            LoadCommandItems(false);
            LoadCronTasks();
            UpdateUiFont(g_state.dpi);
            RegisterLauncherHotkey();
            ApplyLauncherAppearance();
            ApplyFont(g_state.searchEdit, g_state.searchFont);
            ApplyFont(g_state.resultList, g_state.resultFont);
            ApplyFont(g_state.descriptionText, g_state.descriptionFont);
            LayoutLauncher();
            RequestLauncherDataRefresh(true, true);
            HideLauncher();
            success = true;
        } else if (command == L"builtin:reboot") {
            const int response = MessageBoxW(
                g_state.launcherWindow,
                L"Restart the computer now?",
                L"Confirm Reboot",
                MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
            if (response == IDYES) {
                success = RunShellCommand(L"shutdown.exe", L"/r /t 0");
            }
            if (success) {
                HideLauncher();
            }
        } else if (command == L"builtin:poweroff") {
            const int response = MessageBoxW(
                g_state.launcherWindow,
                L"Shut down the computer now?",
                L"Confirm Shutdown",
                MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
            if (response == IDYES) {
                success = RunShellCommand(L"shutdown.exe", L"/s /t 0");
            }
            if (success) {
                HideLauncher();
            }
        } else if (command == L"builtin:hibernate") {
            success = SuspendSystem(true);
            if (success) {
                HideLauncher();
            }
        } else if (command == L"builtin:standby") {
            success = SuspendSystem(false);
            if (success) {
                HideLauncher();
            }
        } else if (command == L"builtin:config") {
            const std::wstring path = GetWritableConfigPath(kConfigFileName);
            success = EnsureFileExists(path) && OpenPathWithConfiguredEditor(path);
            if (success) {
                HideLauncher();
            }
        } else if (command == L"builtin:cmdconfig") {
            const std::wstring path = GetWritableConfigPath(kCommandFileName);
            success = EnsureFileExists(path) && OpenPathWithConfiguredEditor(path);
            if (success) {
                HideLauncher();
            }
        }
        return success;
    }

    SetEnglishImeForWindow(g_state.searchEdit);
    const bool launched = item.inlineBatchScript.empty()
        ? LaunchProcessCommand(item.commandLine, item.workingDirectory, item.showCommand, item.priorityClass)
        : LaunchInlineBatchScript(item);
    if (!launched) {
        return false;
    }

    RecordSuccessfulLaunch(item);
    HideLauncher();
    return true;
}

void LaunchSelectedItem() {
    const LRESULT selected = SendMessageW(g_state.resultList, LB_GETCURSEL, 0, 0);
    if (selected == LB_ERR) {
        return;
    }
    const LRESULT itemIndex = SendMessageW(g_state.resultList, LB_GETITEMDATA, static_cast<WPARAM>(selected), 0);
    if (itemIndex == LB_ERR) {
        return;
    }
    LaunchItemByIndex(static_cast<size_t>(itemIndex));
}

void MoveSelection(int delta) {
    const LRESULT count = SendMessageW(g_state.resultList, LB_GETCOUNT, 0, 0);
    if (count <= 0) {
        return;
    }

    LRESULT selected = SendMessageW(g_state.resultList, LB_GETCURSEL, 0, 0);
    if (selected == LB_ERR) {
        selected = 0;
    }
    selected += delta;
    if (selected < 0) {
        selected = 0;
    }
    if (selected >= count) {
        selected = count - 1;
    }
    SendMessageW(g_state.resultList, LB_SETCURSEL, static_cast<WPARAM>(selected), 0);
    UpdateSelectedItemDescription();
}

LRESULT CALLBACK SearchEditProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_SETFOCUS) {
        SetEnglishImeForWindow(window);
    }
    if (message == WM_KEYDOWN) {
        RecordSearchInputActivity();
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && wParam == 'Q') {
            PostQuitMessage(0);
            return 0;
        }
        if (wParam == VK_UP) {
            MoveSelection(-1);
            return 0;
        }
        if (wParam == VK_DOWN) {
            MoveSelection(1);
            return 0;
        }
        if (wParam == VK_RETURN) {
            LaunchSelectedItem();
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            HideLauncher();
            return 0;
        }
    }
    return CallWindowProcW(g_state.editProc, window, message, wParam, lParam);
}

void ShowLauncher() {
    LayoutLauncher();
    ShowWindow(g_state.launcherWindow, SW_SHOW);
    SetForegroundWindow(g_state.launcherWindow);
    SetFocus(g_state.searchEdit);
    SetEnglishImeForWindow(g_state.searchEdit);
    SendMessageW(g_state.searchEdit, EM_SETSEL, 0, -1);
    RecordSearchInputActivity();
    StartVisibilityTimer();
    ArmDeferredRefresh();
}

LRESULT CALLBACK LauncherWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        g_state.searchEdit = CreateWindowExW(0, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SEARCH)), g_state.instance, nullptr);
        g_state.resultList = CreateWindowExW(0, L"LISTBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_RESULTS)), g_state.instance, nullptr);
        g_state.descriptionText = CreateWindowExW(0, L"STATIC", nullptr, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DESCRIPTION)), g_state.instance, nullptr);
        ApplyFont(g_state.searchEdit, g_state.searchFont);
        ApplyFont(g_state.resultList, g_state.resultFont);
        ApplyFont(g_state.descriptionText, g_state.descriptionFont);
        g_state.editProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_state.searchEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(SearchEditProc)));
        LayoutLauncher();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_SEARCH && HIWORD(wParam) == EN_CHANGE) {
            RecordSearchInputActivity();
            RequestResultListRebuild();
            return 0;
        }
        if (LOWORD(wParam) == IDC_RESULTS && HIWORD(wParam) == LBN_DBLCLK) {
            LaunchSelectedItem();
            return 0;
        }
        if (LOWORD(wParam) == IDC_RESULTS && HIWORD(wParam) == LBN_SELCHANGE) {
            UpdateSelectedItemDescription();
            return 0;
        }
        break;
    case WM_TIMER:
        if (wParam == kVisibilityTimerId) {
            const HWND foregroundWindow = GetForegroundWindow();
            if (foregroundWindow != nullptr && !IsLauncherRelatedWindow(foregroundWindow)) {
                HideLauncher();
            }
            return 0;
        }
        if (wParam == kDeferredRefreshTimerId) {
            if (!g_state.deferredRefreshArmed || !IsWindowVisible(g_state.launcherWindow)) {
                CancelDeferredRefresh();
                return 0;
            }
            const ULONGLONG now = GetTickCount64();
            if ((now - g_state.lastSearchInputTick) < kDeferredRefreshIdleDelayMs) {
                return 0;
            }
            CancelDeferredRefresh();
            RequestLauncherDataRefresh(false, false);
            return 0;
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
            HideLauncher();
        }
        break;
    case WM_DPICHANGED:
        UpdateUiFont(HIWORD(wParam));
        ApplyFont(g_state.searchEdit, g_state.searchFont);
        ApplyFont(g_state.resultList, g_state.resultFont);
        ApplyFont(g_state.descriptionText, g_state.descriptionFont);
        LayoutLauncher();
        return 0;
    case WM_CLOSE:
        HideLauncher();
        return 0;
    case kMessageRebuildResults:
        g_state.resultRebuildScheduled = false;
        RebuildResultList();
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK HostWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_HOTKEY && wParam == ID_HOTKEY_LAUNCH) {
        ShowLauncher();
        return 0;
    }
    if (message == WM_TIMER && wParam == kCronTimerId) {
        RunCronTasks();
        return 0;
    }
    if (message == kMessageRefreshLauncherData) {
        if (ShouldDeferRefreshForActiveTyping()) {
            g_state.refreshScheduled = false;
            ArmDeferredRefresh();
            return 0;
        }
        const bool refreshItems = g_state.pendingItemsRefresh;
        const bool refreshHistory = g_state.pendingHistoryRefresh;
        g_state.pendingItemsRefresh = false;
        g_state.pendingHistoryRefresh = false;
        g_state.refreshScheduled = false;
        RefreshLauncherDataIfNeeded(refreshItems, refreshHistory);
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

ATOM RegisterWindowClass(const wchar_t* name, WNDPROC proc, HBRUSH brush) {
    WNDCLASSEXW wc {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = g_state.instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = brush;
    wc.lpszClassName = name;
    wc.hIconSm = wc.hIcon;
    return RegisterClassExW(&wc);
}

bool InitializeWindows() {
    if (RegisterWindowClass(kHostClassName, HostWindowProc, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1)) == 0) {
        return false;
    }
    if (RegisterWindowClass(kLauncherClassName, LauncherWindowProc, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1)) == 0) {
        return false;
    }

    g_state.hostWindow = CreateWindowExW(0, kHostClassName, L"DoRunHost", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g_state.instance, nullptr);
    if (g_state.hostWindow == nullptr) {
        return false;
    }

    g_state.launcherWindow = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kLauncherClassName,
        L"DoRun",
        WS_POPUP,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        nullptr,
        nullptr,
        g_state.instance,
        nullptr);

    ApplyLauncherAppearance();
    return g_state.launcherWindow != nullptr;
}

void InitializeDpi() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    UpdateUiFont(static_cast<int>(GetDpiForSystem()));
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    g_state.instance = instance;
    const HRESULT comInitResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool comInitialized = SUCCEEDED(comInitResult);

    INITCOMMONCONTROLSEX commonControls {};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_WIN95_CLASSES;
    InitCommonControlsEx(&commonControls);

    LoadHotkeyConfig();
    LoadHistoryConfig();
    LoadLauncherAppearanceConfig();
    InitializeDpi();
    LoadCommandVariables();
    LoadCommandItems(false);
    LoadCronTasks();

    if (!InitializeWindows()) {
        if (comInitialized) {
            CoUninitialize();
        }
        return 1;
    }

    RegisterLauncherHotkey();
    if (ShouldRunStartupCommands()) {
        RunStartupCommands();
    }
    RunCronTasks();
    StartCronTimer();
    RequestLauncherDataRefresh(true, true);

    MSG message {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    UnregisterHotKey(g_state.hostWindow, ID_HOTKEY_LAUNCH);
    CloseCronTaskProcessHandles();
    if (g_state.searchFont != nullptr) {
        DeleteObject(g_state.searchFont);
        g_state.searchFont = nullptr;
    }
    if (g_state.resultFont != nullptr) {
        DeleteObject(g_state.resultFont);
        g_state.resultFont = nullptr;
    }
    if (g_state.descriptionFont != nullptr) {
        DeleteObject(g_state.descriptionFont);
        g_state.descriptionFont = nullptr;
    }
    if (comInitialized) {
        CoUninitialize();
    }
    return static_cast<int>(message.wParam);
}
