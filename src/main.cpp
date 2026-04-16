#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "resource.h"

namespace {

struct HotkeyConfig {
    UINT modifiers = MOD_ALT;
    UINT vk = VK_SPACE;
};

struct LaunchItem {
    std::wstring name;
    std::wstring commandLine;
    std::wstring workingDirectory;
    int showCommand = SW_SHOWNORMAL;
    DWORD priorityClass = NORMAL_PRIORITY_CLASS;
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
    std::vector<std::wstring> scanDirectories;
    std::unordered_set<std::wstring> executableExtensions;
    std::vector<LaunchItem> items;
};

AppState g_state {};

constexpr wchar_t kHostClassName[] = L"DoRun.HostWindow";
constexpr wchar_t kLauncherClassName[] = L"DoRun.LauncherWindow";
constexpr wchar_t kConfigClassName[] = L"DoRun.ConfigWindow";
constexpr wchar_t kAppDirectoryName[] = L"DoRun";
constexpr wchar_t kTomlFileName[] = L"DoRun.toml";
constexpr wchar_t kCommandFileName[] = L"Command.conf";
constexpr size_t kVisibleItemCount = 8;
constexpr UINT_PTR kVisibilityTimerId = 1;
constexpr UINT kVisibilityTimerIntervalMs = 100;

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

std::optional<UINT> ParseTomlUInt(const std::wstring& content, std::wstring_view key) {
    const std::wstring pattern = std::wstring(key) + L" =";
    const size_t keyPos = content.find(pattern);
    if (keyPos == std::wstring::npos) {
        return std::nullopt;
    }

    size_t valueStart = keyPos + pattern.size();
    while (valueStart < content.size() && iswspace(content[valueStart]) != 0) {
        ++valueStart;
    }

    size_t valueEnd = valueStart;
    while (valueEnd < content.size() && iswdigit(content[valueEnd]) != 0) {
        ++valueEnd;
    }

    if (valueEnd == valueStart) {
        return std::nullopt;
    }

    return static_cast<UINT>(_wtoi(content.substr(valueStart, valueEnd - valueStart).c_str()));
}

void EnsureDefaultHotkey() {
    g_state.hotkey.modifiers = MOD_ALT;
    g_state.hotkey.vk = VK_SPACE;
}

void LoadHotkeyConfig() {
    EnsureDefaultHotkey();

    const std::wstring tomlPath = FindConfigPath(kTomlFileName);
    const std::wstring content = LoadUtf8TextFile(tomlPath);
    if (content.empty()) {
        return;
    }

    if (const std::optional<UINT> modifiers = ParseTomlUInt(content, L"HOTKEY_MODIFIERS")) {
        g_state.hotkey.modifiers = *modifiers;
    }
    if (const std::optional<UINT> vk = ParseTomlUInt(content, L"HOTKEY_VK")) {
        g_state.hotkey.vk = *vk;
    }
    if (g_state.hotkey.vk == 0U) {
        g_state.hotkey.vk = VK_SPACE;
    }
}

void SaveHotkeyConfig() {
    const std::wstring tomlPath = GetWritableConfigPath(kTomlFileName);
    std::wstring content = LoadUtf8TextFile(tomlPath);
    const std::wstring modifiersLine = L"HOTKEY_MODIFIERS = " + std::to_wstring(g_state.hotkey.modifiers);
    const std::wstring vkLine = L"HOTKEY_VK = " + std::to_wstring(g_state.hotkey.vk);

    auto upsertLine = [&](std::wstring_view key, const std::wstring& replacement) {
        const std::wstring pattern = std::wstring(key) + L" =";
        const size_t keyPos = content.find(pattern);
        if (keyPos == std::wstring::npos) {
            if (!content.empty() && content.back() != L'\n') {
                content += L"\n";
            }
            content += replacement;
            content += L"\n";
            return;
        }

        size_t lineEnd = content.find(L'\n', keyPos);
        if (lineEnd == std::wstring::npos) {
            lineEnd = content.size();
        }
        content.replace(keyPos, lineEnd - keyPos, replacement);
    };

    upsertLine(L"HOTKEY_MODIFIERS", modifiersLine);
    upsertLine(L"HOTKEY_VK", vkLine);
    SaveUtf8TextFile(tomlPath, content);
}

void RegisterLauncherHotkey() {
    UnregisterHotKey(g_state.hostWindow, ID_HOTKEY_LAUNCH);
    RegisterHotKey(g_state.hostWindow, ID_HOTKEY_LAUNCH, g_state.hotkey.modifiers, g_state.hotkey.vk);
}

std::vector<std::wstring> ParseQuotedStrings(const std::wstring& text) {
    std::vector<std::wstring> values;
    std::wstring current;
    bool inString = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t ch = text[i];
        if (!inString) {
            if (ch == L'"') {
                inString = true;
                current.clear();
            }
            continue;
        }

        if (ch == L'\\' && i + 1 < text.size()) {
            current.push_back(text[++i]);
            continue;
        }
        if (ch == L'"') {
            values.push_back(current);
            inString = false;
            continue;
        }
        current.push_back(ch);
    }
    return values;
}

std::unordered_set<std::wstring> LoadExecutableExtensions() {
    std::unordered_set<std::wstring> extensions;
    std::array<wchar_t, 32767> buffer {};
    const DWORD length = GetEnvironmentVariableW(L"PATHEXT", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size()) {
        std::wstringstream stream(buffer.data());
        std::wstring token;
        while (std::getline(stream, token, L';')) {
            token = Lowercase(Trim(token));
            if (!token.empty() && token.front() != L'.') {
                token.insert(token.begin(), L'.');
            }
            if (!token.empty()) {
                extensions.insert(token);
            }
        }
    }
    extensions.insert(L".exe");
    extensions.insert(L".lnk");
    return extensions;
}

void EnsureDefaultDirectories() {
    if (!g_state.scanDirectories.empty()) {
        return;
    }
    g_state.scanDirectories = {
        L"C:\\Windows\\System32",
        L"C:\\Program Files",
        L"C:\\Program Files (x86)",
    };
}

void LoadScanDirectories() {
    g_state.scanDirectories.clear();
    const std::wstring content = LoadUtf8TextFile(FindConfigPath(kTomlFileName));
    if (content.empty()) {
        EnsureDefaultDirectories();
        return;
    }
    const size_t dirPos = content.find(L"DIR");
    const size_t start = content.find(L'[', dirPos);
    const size_t end = content.find(L']', start);
    if (dirPos == std::wstring::npos || start == std::wstring::npos || end == std::wstring::npos || end <= start) {
        EnsureDefaultDirectories();
        return;
    }
    g_state.scanDirectories = ParseQuotedStrings(content.substr(start + 1, end - start - 1));
    EnsureDefaultDirectories();
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
        if (fields.size() > 2) {
            item.workingDirectory = UnescapeCommandField(StripOuterQuotes(fields[2]));
        }
        if (fields.size() > 3 && !fields[3].empty()) {
            item.showCommand = _wtoi(fields[3].c_str());
        }
        if (fields.size() > 4 && !fields[4].empty()) {
            item.priorityClass = static_cast<DWORD>(_wtoi(fields[4].c_str()));
        }
        g_state.items.push_back(std::move(item));
    }
}

void LoadFilesystemItems() {
    std::unordered_set<std::wstring> seen;
    for (const LaunchItem& item : g_state.items) {
        seen.insert(Lowercase(item.name + L"|" + item.commandLine));
    }

    for (const std::wstring& directory : g_state.scanDirectories) {
        std::error_code error;
        if (!std::filesystem::exists(directory, error)) {
            continue;
        }

        for (std::filesystem::recursive_directory_iterator it(directory, std::filesystem::directory_options::skip_permission_denied, error), end; it != end; it.increment(error)) {
            if (error) {
                error.clear();
                continue;
            }
            if (!it->is_regular_file(error)) {
                continue;
            }

            const std::filesystem::path path = it->path();
            const std::wstring extension = Lowercase(path.extension().wstring());
            if (g_state.executableExtensions.find(extension) == g_state.executableExtensions.end()) {
                continue;
            }

            LaunchItem item {};
            item.name = path.stem().wstring();
            item.commandLine = path.wstring();
            item.workingDirectory = path.parent_path().wstring();
            const std::wstring key = Lowercase(item.name + L"|" + item.commandLine);
            if (seen.insert(key).second) {
                g_state.items.push_back(std::move(item));
            }
        }
    }

    std::sort(g_state.items.begin(), g_state.items.end(), [](const LaunchItem& lhs, const LaunchItem& rhs) {
        return _wcsicmp(lhs.name.c_str(), rhs.name.c_str()) < 0;
    });
}

void ReloadItems() {
    g_state.executableExtensions = LoadExecutableExtensions();
    g_state.items.clear();
    LoadScanDirectories();
    LoadCommandItems();
    LoadFilesystemItems();
}

std::wstring GetControlText(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring text(static_cast<size_t>(length), L'\0');
    GetWindowTextW(window, text.data(), length + 1);
    return text;
}

void RebuildResultList() {
    const std::wstring query = Trim(GetControlText(g_state.searchEdit));
    SendMessageW(g_state.resultList, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_state.resultList, LB_RESETCONTENT, 0, 0);

    for (size_t index = 0; index < g_state.items.size(); ++index) {
        const LaunchItem& item = g_state.items[index];
        if (!ContainsInsensitive(item.name, query) && !ContainsInsensitive(item.commandLine, query)) {
            continue;
        }
        const LRESULT inserted = SendMessageW(g_state.resultList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.name.c_str()));
        if (inserted >= 0) {
            SendMessageW(g_state.resultList, LB_SETITEMDATA, static_cast<WPARAM>(inserted), static_cast<LPARAM>(index));
        }
    }

    if (SendMessageW(g_state.resultList, LB_GETCOUNT, 0, 0) > 0) {
        SendMessageW(g_state.resultList, LB_SETCURSEL, 0, 0);
    }
    SendMessageW(g_state.resultList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_state.resultList, nullptr, TRUE);
}

bool LaunchItemByIndex(size_t index) {
    if (index >= g_state.items.size()) {
        return false;
    }

    const LaunchItem& item = g_state.items[index];
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
    ReloadItems();
    RebuildResultList();
    LayoutLauncher();
    ShowWindow(g_state.launcherWindow, SW_SHOW);
    SetForegroundWindow(g_state.launcherWindow);
    SetFocus(g_state.searchEdit);
    SendMessageW(g_state.searchEdit, EM_SETSEL, 0, -1);
    StartVisibilityTimer();
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

    if (!InitializeWindows()) {
        return 1;
    }

    RegisterLauncherHotkey();

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
