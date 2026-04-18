#include <windows.h>
#include <commctrl.h>
#include <imm.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winsqlite/winsqlite3.h>

#include <algorithm>
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

struct LaunchItem {
    std::wstring name;
    std::wstring commandLine;
    std::wstring workingDirectory;
    int showCommand = SW_SHOWNORMAL;
    DWORD priorityClass = NORMAL_PRIORITY_CLASS;
    ItemSourceKind sourceKind = ItemSourceKind::ScannedFile;
    std::wstring itemKey;
};

struct BuiltinCommandDefinition {
    const wchar_t* name;
    const wchar_t* description;
    const wchar_t* token;
};

struct ScanDirectoryConfig {
    std::wstring path;
    std::unordered_set<std::wstring> extensions;
    bool recursive = true;
};

struct AppState {
    HINSTANCE instance = nullptr;
    HWND hostWindow = nullptr;
    HWND launcherWindow = nullptr;
    HWND searchEdit = nullptr;
    HWND resultList = nullptr;
    HWND configButton = nullptr;
    HWND configWindow = nullptr;
    HWND configHotkey = nullptr;
    HWND configSaveButton = nullptr;
    HWND configCancelButton = nullptr;
    WNDPROC editProc = nullptr;
    HFONT uiFont = nullptr;
    int dpi = USER_DEFAULT_SCREEN_DPI;
    HotkeyConfig hotkey {};
    std::vector<ScanDirectoryConfig> scanDirectories;
    std::vector<LaunchItem> items;
    HistoryConfig historyConfig {};
    std::unordered_map<std::wstring, HistoryInfo> historyByKey;
    bool launcherDataLoaded = false;
    bool refreshScheduled = false;
    bool pendingItemsRefresh = false;
    bool pendingHistoryRefresh = false;
    ULONGLONG lastItemsRefreshTick = 0;
    ULONGLONG lastHistoryRefreshTick = 0;
};

AppState g_state {};

constexpr wchar_t kHostClassName[] = L"DoRun.HostWindow";
constexpr wchar_t kLauncherClassName[] = L"DoRun.LauncherWindow";
constexpr wchar_t kConfigClassName[] = L"DoRun.ConfigWindow";
constexpr wchar_t kAppDirectoryName[] = L"DoRun";
constexpr wchar_t kConfigFileName[] = L"DoRun.yaml";
constexpr wchar_t kCommandFileName[] = L"Command.conf";
constexpr size_t kVisibleItemCount = 8;
constexpr UINT_PTR kVisibilityTimerId = 1;
constexpr UINT kVisibilityTimerIntervalMs = 100;
constexpr UINT kMessageRefreshLauncherData = WM_APP + 1;
constexpr ULONGLONG kItemsRefreshIntervalMs = 5ULL * 60ULL * 1000ULL;
constexpr ULONGLONG kHistoryRefreshIntervalMs = 15ULL * 1000ULL;
constexpr wchar_t kBuiltinCommandPrefix[] = L"builtin:";

constexpr BuiltinCommandDefinition kBuiltinCommands[] = {
    { L"/quit", L"Exit DoRun", L"quit" },
    { L"/reload", L"Reload configuration files", L"reload" },
    { L"/reboot", L"Restart the computer", L"reboot" },
    { L"/poweroff", L"Shut down the computer", L"poweroff" },
    { L"/config", L"Open DoRun.yaml", L"config" },
    { L"/cmdconfig", L"Open Command.conf", L"cmdconfig" },
};

void RebuildResultList();

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

std::wstring NormalizeHistoryField(std::wstring_view text) {
    return Lowercase(Trim(text));
}

std::wstring BuildItemKey(ItemSourceKind sourceKind, std::wstring_view commandLine, std::wstring_view workingDirectory) {
    return std::to_wstring(static_cast<int>(sourceKind)) + L"\n" +
        NormalizeHistoryField(commandLine) + L"\n" +
        NormalizeHistoryField(workingDirectory);
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

int ComputeMatchScore(const LaunchItem& item, std::wstring_view query) {
    if (query.empty()) {
        return 1;
    }

    const std::vector<std::wstring> tokens = SplitQueryTokens(query);
    if (tokens.empty()) {
        return 1;
    }

    int score = 0;
    for (const std::wstring& token : tokens) {
        const int nameScore = GetMatchScoreForText(item.name, token, 100, 90, 75, 60);
        const std::filesystem::path commandPath(item.commandLine);
        const std::wstring commandBaseName = commandPath.stem().wstring();
        const int commandBaseScore = GetMatchScoreForText(commandBaseName, token, 55, 55, 50, 45);
        const int commandScore = GetMatchScoreForText(item.commandLine, token, 40, 40, 40, 25);
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

void ApplyFont(HWND window) {
    if (window != nullptr && g_state.uiFont != nullptr) {
        SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(g_state.uiFont), TRUE);
    }
}

void UpdateUiFont(int dpi) {
    g_state.dpi = dpi;
    if (g_state.uiFont != nullptr) {
        DeleteObject(g_state.uiFont);
        g_state.uiFont = nullptr;
    }

    NONCLIENTMETRICSW metrics {};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, static_cast<UINT>(dpi)) == FALSE) {
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
    }
    g_state.uiFont = CreateFontIndirectW(&metrics.lfMessageFont);
}

void LayoutLauncher() {
    RECT workArea {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const int width = Scale(720);
    const int margin = Scale(12);
    const int buttonWidth = Scale(96);
    const int controlHeight = Scale(32);
    const int listHeight = Scale(30) * static_cast<int>(kVisibleItemCount);
    const int height = margin * 3 + controlHeight + listHeight;
    const int x = workArea.left + ((workArea.right - workArea.left) - width) / 2;
    const int y = workArea.top + ((workArea.bottom - workArea.top) - height) / 4;

    SetWindowPos(g_state.launcherWindow, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
    MoveWindow(g_state.searchEdit, margin, margin, width - margin * 3 - buttonWidth, controlHeight, TRUE);
    MoveWindow(g_state.configButton, width - margin - buttonWidth, margin, buttonWidth, controlHeight, TRUE);
    MoveWindow(g_state.resultList, margin, margin * 2 + controlHeight, width - margin * 2, listHeight, TRUE);
}

void LayoutConfig(HWND window) {
    RECT anchor {};
    GetWindowRect(g_state.launcherWindow, &anchor);
    const int width = Scale(340);
    const int height = Scale(160);
    const int margin = Scale(16);
    const int controlHeight = Scale(28);
    SetWindowPos(window, HWND_TOPMOST, anchor.left + Scale(40), anchor.top + Scale(40), width, height, SWP_SHOWWINDOW);
    MoveWindow(g_state.configHotkey, margin, margin + Scale(28), width - margin * 2, controlHeight, TRUE);
    MoveWindow(g_state.configSaveButton, width - margin * 2 - Scale(176), height - margin - controlHeight, Scale(80), controlHeight, TRUE);
    MoveWindow(g_state.configCancelButton, width - margin - Scale(80), height - margin - controlHeight, Scale(80), controlHeight, TRUE);
}

void HideLauncher() {
    if (g_state.launcherWindow != nullptr) {
        KillTimer(g_state.launcherWindow, kVisibilityTimerId);
    }
    ShowWindow(g_state.launcherWindow, SW_HIDE);
}

bool IsChildOrSameWindow(HWND parent, HWND candidate) {
    return parent != nullptr && candidate != nullptr && (parent == candidate || IsChild(parent, candidate) != FALSE);
}

bool IsLauncherRelatedWindow(HWND window) {
    return IsChildOrSameWindow(g_state.launcherWindow, window) || IsChildOrSameWindow(g_state.configWindow, window);
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

void SaveHotkeyConfig() {
    const std::wstring configPath = GetWritableConfigPath(kConfigFileName);
    std::wstring content = LoadUtf8TextFile(configPath);
    const std::wstring modifiersLine = L"HOTKEY_MODIFIERS: " + std::to_wstring(g_state.hotkey.modifiers);
    const std::wstring vkLine = L"HOTKEY_VK: " + std::to_wstring(g_state.hotkey.vk);

    auto upsertLine = [&](std::wstring_view key, const std::wstring& replacement) {
        std::wstringstream stream(content);
        std::wstring line;
        std::wstring rebuilt;
        bool replaced = false;

        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == L'\r') {
                line.pop_back();
            }

            std::wstring remainder;
            if (!replaced && MatchYamlKey(Trim(line), key, &remainder)) {
                rebuilt += replacement;
                rebuilt += L"\n";
                replaced = true;
                continue;
            }

            rebuilt += line;
            rebuilt += L"\n";
        }

        if (!replaced) {
            if (!rebuilt.empty() && rebuilt.back() != L'\n') {
                rebuilt += L"\n";
            }
            rebuilt += replacement;
            rebuilt += L"\n";
        }
        content = std::move(rebuilt);
    };

    upsertLine(L"HOTKEY_MODIFIERS", modifiersLine);
    upsertLine(L"HOTKEY_VK", vkLine);
    SaveUtf8TextFile(configPath, content);
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

bool RunShellCommand(const wchar_t* file, const wchar_t* parameters, int showCommand = SW_HIDE) {
    const HINSTANCE result = ShellExecuteW(g_state.launcherWindow, L"open", file, parameters, nullptr, showCommand);
    return reinterpret_cast<INT_PTR>(result) > 32;
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

void LoadCommandItems() {
    std::wifstream stream(FindConfigPath(kCommandFileName));
    if (!stream.is_open()) {
        return;
    }
    stream.imbue(std::locale(".UTF-8"));

    std::wstring line;
    while (std::getline(stream, line)) {
        line = Trim(line);
        if (IsCommandCommentLine(line)) {
            continue;
        }

        const std::vector<std::wstring> fields = SplitCommandFields(line);
        if (fields.size() < 2) {
            continue;
        }

        LaunchItem item {};
        item.name = UnescapeCommandField(StripOuterQuotes(fields[0]));
        item.commandLine = UnescapeCommandField(StripOuterQuotes(fields[1]));
        item.sourceKind = ItemSourceKind::CommandConf;
        if (fields.size() > 2) {
            item.workingDirectory = UnescapeCommandField(StripOuterQuotes(fields[2]));
        }
        if (fields.size() > 3 && !fields[3].empty()) {
            item.showCommand = _wtoi(fields[3].c_str());
        }
        if (fields.size() > 4 && !fields[4].empty()) {
            item.priorityClass = static_cast<DWORD>(_wtoi(fields[4].c_str()));
        }
        item.itemKey = BuildItemKey(item.sourceKind, item.commandLine, item.workingDirectory);
        g_state.items.push_back(std::move(item));
    }
}

void LoadBuiltinCommandItems() {
    for (const BuiltinCommandDefinition& command : kBuiltinCommands) {
        LaunchItem item {};
        item.name = std::wstring(command.name) + L" - " + command.description;
        item.commandLine = std::wstring(kBuiltinCommandPrefix) + command.token;
        item.sourceKind = ItemSourceKind::Synthetic;
        item.itemKey = BuildItemKey(item.sourceKind, item.commandLine, item.workingDirectory);
        g_state.items.push_back(std::move(item));
    }
}

void LoadFilesystemItems() {
    std::unordered_set<std::wstring> seen;
    for (const LaunchItem& item : g_state.items) {
        seen.insert(Lowercase(item.name + L"|" + item.commandLine));
    }

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
            item.sourceKind = ItemSourceKind::ScannedFile;
            item.itemKey = BuildItemKey(item.sourceKind, item.commandLine, item.workingDirectory);
            const std::wstring key = Lowercase(item.name + L"|" + item.commandLine);
            if (seen.insert(key).second) {
                g_state.items.push_back(std::move(item));
            }
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
            if (!it->is_regular_file(error)) {
                continue;
            }
            appendItem(it->path());
        }
    }

    std::sort(g_state.items.begin(), g_state.items.end(), [](const LaunchItem& lhs, const LaunchItem& rhs) {
        return _wcsicmp(lhs.name.c_str(), rhs.name.c_str()) < 0;
    });
}

void ReloadItems() {
    const std::wstring content = LoadUtf8TextFile(FindConfigPath(kConfigFileName));
    g_state.items.clear();
    LoadScanDirectories(content);
    LoadCommandItems();
    LoadFilesystemItems();
    LoadBuiltinCommandItems();
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

void RefreshLauncherDataIfNeeded(bool forceItems, bool forceHistory) {
    const ULONGLONG now = GetTickCount64();
    const bool shouldRefreshItems = forceItems ||
        !g_state.launcherDataLoaded ||
        (now - g_state.lastItemsRefreshTick) >= kItemsRefreshIntervalMs;
    const bool shouldRefreshHistory = forceHistory ||
        !g_state.launcherDataLoaded ||
        (now - g_state.lastHistoryRefreshTick) >= kHistoryRefreshIntervalMs;

    bool rebuiltItems = false;
    if (shouldRefreshItems) {
        ReloadItems();
        g_state.lastItemsRefreshTick = now;
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
    SendMessageW(g_state.resultList, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_state.resultList, LB_RESETCONTENT, 0, 0);

    std::vector<RankedResult> rankedResults;
    rankedResults.reserve(g_state.items.size());

    for (size_t index = 0; index < g_state.items.size(); ++index) {
        const LaunchItem& item = g_state.items[index];
        if (builtinQuery && item.sourceKind != ItemSourceKind::Synthetic) {
            continue;
        }
        if (!builtinQuery && item.sourceKind == ItemSourceKind::Synthetic) {
            continue;
        }
        const int matchScore = ComputeMatchScore(item, query);
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
            RegisterLauncherHotkey();
            RequestLauncherDataRefresh(true, true);
            HideLauncher();
            success = true;
        } else if (command == L"builtin:reboot") {
            success = RunShellCommand(L"shutdown.exe", L"/r /t 0");
            if (success) {
                HideLauncher();
            }
        } else if (command == L"builtin:poweroff") {
            success = RunShellCommand(L"shutdown.exe", L"/s /t 0");
            if (success) {
                HideLauncher();
            }
        } else if (command == L"builtin:config") {
            const std::wstring path = GetWritableConfigPath(kConfigFileName);
            success = EnsureFileExists(path) && OpenPathInShell(path);
            if (success) {
                HideLauncher();
            }
        } else if (command == L"builtin:cmdconfig") {
            const std::wstring path = GetWritableConfigPath(kCommandFileName);
            success = EnsureFileExists(path) && OpenPathInShell(path);
            if (success) {
                HideLauncher();
            }
        }
        return success;
    }

    SetEnglishImeForWindow(g_state.searchEdit);
    std::wstring commandLine = ExpandEnvironmentVariables(item.commandLine);
    std::wstring workingDirectoryBuffer = ExpandEnvironmentVariables(item.workingDirectory);
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startupInfo {};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESHOWWINDOW;
    startupInfo.wShowWindow = static_cast<WORD>(item.showCommand);

    PROCESS_INFORMATION processInfo {};
    const wchar_t* workingDirectory = workingDirectoryBuffer.empty() ? nullptr : workingDirectoryBuffer.c_str();
    const DWORD creationFlags = CREATE_UNICODE_ENVIRONMENT | item.priorityClass;
    const BOOL ok = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, creationFlags, nullptr, workingDirectory, &startupInfo, &processInfo);
    if (ok == FALSE) {
        const HINSTANCE result = ShellExecuteW(g_state.launcherWindow, L"open", commandLine.c_str(), nullptr, workingDirectory, item.showCommand);
        if (reinterpret_cast<INT_PTR>(result) <= 32) {
            return false;
        }
    } else {
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
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
}

LRESULT CALLBACK SearchEditProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_SETFOCUS) {
        SetEnglishImeForWindow(window);
    }
    if (message == WM_KEYDOWN) {
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

void PopulateHotkeyControl() {
    BYTE modifiers = 0;
    if ((g_state.hotkey.modifiers & MOD_ALT) != 0U) {
        modifiers |= HOTKEYF_ALT;
    }
    if ((g_state.hotkey.modifiers & MOD_CONTROL) != 0U) {
        modifiers |= HOTKEYF_CONTROL;
    }
    if ((g_state.hotkey.modifiers & MOD_SHIFT) != 0U) {
        modifiers |= HOTKEYF_SHIFT;
    }
    SendMessageW(g_state.configHotkey, HKM_SETHOTKEY, MAKEWORD(g_state.hotkey.vk, modifiers), 0);
}

void ReadHotkeyControl() {
    const DWORD hotkey = static_cast<DWORD>(SendMessageW(g_state.configHotkey, HKM_GETHOTKEY, 0, 0));
    const UINT vk = LOBYTE(hotkey);
    const UINT flags = HIBYTE(hotkey);
    UINT modifiers = 0;
    if ((flags & HOTKEYF_ALT) != 0U) {
        modifiers |= MOD_ALT;
    }
    if ((flags & HOTKEYF_CONTROL) != 0U) {
        modifiers |= MOD_CONTROL;
    }
    if ((flags & HOTKEYF_SHIFT) != 0U) {
        modifiers |= MOD_SHIFT;
    }
    if (vk != 0U) {
        g_state.hotkey.vk = vk;
        g_state.hotkey.modifiers = modifiers;
    }
}

void ShowLauncher() {
    LayoutLauncher();
    ShowWindow(g_state.launcherWindow, SW_SHOW);
    SetForegroundWindow(g_state.launcherWindow);
    SetFocus(g_state.searchEdit);
    SetEnglishImeForWindow(g_state.searchEdit);
    SendMessageW(g_state.searchEdit, EM_SETSEL, 0, -1);
    StartVisibilityTimer();
    RequestLauncherDataRefresh(false, false);
}

void ShowConfigWindow() {
    if (g_state.configWindow != nullptr) {
        ShowWindow(g_state.configWindow, SW_SHOWNORMAL);
        SetForegroundWindow(g_state.configWindow);
        return;
    }

    g_state.configWindow = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        kConfigClassName,
        L"DoRun Settings",
        WS_CAPTION | WS_POPUP | WS_SYSMENU,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        g_state.launcherWindow,
        nullptr,
        g_state.instance,
        nullptr);
}

LRESULT CALLBACK ConfigWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        HWND label = CreateWindowExW(0, L"STATIC", L"Launcher Hotkey", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window, nullptr, g_state.instance, nullptr);
        g_state.configHotkey = CreateWindowExW(WS_EX_CLIENTEDGE, HOTKEY_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_HOTKEY_LAUNCH)), g_state.instance, nullptr);
        g_state.configSaveButton = CreateWindowExW(0, L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_SAVE)), g_state.instance, nullptr);
        g_state.configCancelButton = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_CANCEL)), g_state.instance, nullptr);
        ApplyFont(label);
        ApplyFont(g_state.configHotkey);
        ApplyFont(g_state.configSaveButton);
        ApplyFont(g_state.configCancelButton);
        PopulateHotkeyControl();
        LayoutConfig(window);
        return 0;
    }
    case WM_DPICHANGED:
        UpdateUiFont(HIWORD(wParam));
        EnumChildWindows(window, [](HWND child, LPARAM) -> BOOL {
            ApplyFont(child);
            return TRUE;
        }, 0);
        LayoutConfig(window);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_BTN_SAVE) {
            ReadHotkeyControl();
            SaveHotkeyConfig();
            RegisterLauncherHotkey();
            DestroyWindow(window);
            return 0;
        }
        if (LOWORD(wParam) == IDC_BTN_CANCEL) {
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        g_state.configWindow = nullptr;
        g_state.configHotkey = nullptr;
        g_state.configSaveButton = nullptr;
        g_state.configCancelButton = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK LauncherWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        g_state.searchEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SEARCH)), g_state.instance, nullptr);
        g_state.resultList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | WS_VSCROLL, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_RESULTS)), g_state.instance, nullptr);
        g_state.configButton = CreateWindowExW(0, L"BUTTON", L"Config", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CONFIG)), g_state.instance, nullptr);
        ApplyFont(g_state.searchEdit);
        ApplyFont(g_state.resultList);
        ApplyFont(g_state.configButton);
        g_state.editProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_state.searchEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(SearchEditProc)));
        LayoutLauncher();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_SEARCH && HIWORD(wParam) == EN_CHANGE) {
            RebuildResultList();
            return 0;
        }
        if (LOWORD(wParam) == IDC_RESULTS && HIWORD(wParam) == LBN_DBLCLK) {
            LaunchSelectedItem();
            return 0;
        }
        if (LOWORD(wParam) == IDC_CONFIG && HIWORD(wParam) == BN_CLICKED) {
            ShowConfigWindow();
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
        break;
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE && reinterpret_cast<HWND>(lParam) != g_state.configWindow) {
            HideLauncher();
        }
        break;
    case WM_DPICHANGED:
        UpdateUiFont(HIWORD(wParam));
        ApplyFont(g_state.searchEdit);
        ApplyFont(g_state.resultList);
        ApplyFont(g_state.configButton);
        LayoutLauncher();
        return 0;
    case WM_CLOSE:
        HideLauncher();
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
    if (message == kMessageRefreshLauncherData) {
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
    if (RegisterWindowClass(kConfigClassName, ConfigWindowProc, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1)) == 0) {
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
        WS_POPUP | WS_BORDER,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        nullptr,
        nullptr,
        g_state.instance,
        nullptr);

    return g_state.launcherWindow != nullptr;
}

void InitializeDpi() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    UpdateUiFont(static_cast<int>(GetDpiForSystem()));
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    g_state.instance = instance;

    INITCOMMONCONTROLSEX commonControls {};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_WIN95_CLASSES;
    InitCommonControlsEx(&commonControls);

    InitializeDpi();
    LoadHotkeyConfig();
    LoadHistoryConfig();

    if (!InitializeWindows()) {
        return 1;
    }

    RegisterLauncherHotkey();
    RequestLauncherDataRefresh(true, true);

    MSG message {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    UnregisterHotKey(g_state.hostWindow, ID_HOTKEY_LAUNCH);
    if (g_state.uiFont != nullptr) {
        DeleteObject(g_state.uiFont);
        g_state.uiFont = nullptr;
    }
    return static_cast<int>(message.wParam);
}
