// Include du fichier d'en-tête correspondant à la classe ClaudePanel
#include "ClaudePanel.h"

// Inclure les autres fichiers d'en-tête nécessaires pour le fonctionnement de la classe
#include "core/explorer/Explorer.h"
#include "core/window/Window.h"
#include "helpers/window_helpers.h"
#include "ui/components/input/InputTheme.h"
#include "ui/theme/Theme.h"

// Les en tête différentes pour du code shell
#include <shlobj.h>
#include <shellapi.h>

// En-têtes standard pour les fonctionnalités utilisées comme les fichiers, les algorithmes, etc.
#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

// Ont déclare la class
namespace
{
    std::wstring TrimWideLocal(const std::wstring &text)
    {
        size_t start = 0;
        while (start < text.size() && iswspace(text[start]))
            ++start;
        size_t end = text.size();
        while (end > start && iswspace(text[end - 1]))
            --end;
        return text.substr(start, end - start);
    }

    // Fonction utilitaire pour résoudre le chemin des exe de claude.
    std::wstring ExpandEnvPath(const wchar_t *envName, const wchar_t *suffix)
    {
        if (!envName || !suffix)
            return {};

        wchar_t base[MAX_PATH] = {};
        DWORD len = GetEnvironmentVariableW(envName, base, MAX_PATH);
        if (len == 0 || len >= MAX_PATH)
            return {};

        std::filesystem::path path(base);
        path /= suffix;
        std::error_code ec;
        if (std::filesystem::exists(path, ec) && !ec)
            return path.wstring();
        return {};
    }

    // Fonction utilitaire pour détecter le chemin de l'exécutable de claude.
    std::wstring DetectClaudeExecutable()
    {
        static const struct
        {
            const wchar_t *envName;
            const wchar_t *suffix;
        } kKnownPaths[] = {
            {L"USERPROFILE", L".local\\bin\\claude.exe"},
            {L"APPDATA", L"npm\\claude.cmd"},
            {L"APPDATA", L"npm\\claude.exe"},
        };

        for (const auto &candidate : kKnownPaths)
        {
            std::wstring resolved = ExpandEnvPath(candidate.envName, candidate.suffix);
            if (!resolved.empty())
                return resolved;
        }

        static const wchar_t *kCandidates[] = {
            L"claude.exe",
            L"claude.cmd",
            L"claude.bat",
            L"claude"
        };

        wchar_t resolved[MAX_PATH] = {};
        for (const wchar_t *candidate : kCandidates)
        {
            DWORD len = SearchPathW(nullptr, candidate, nullptr, MAX_PATH, resolved, nullptr);
            if (len > 0 && len < MAX_PATH)
                return std::wstring(resolved, resolved + len);
        }

        return {};
    }

    bool IsPointInRect(const D2D1_RECT_F &rect, POINT pt)
    {
        return pt.x >= rect.left && pt.x <= rect.right &&
               pt.y >= rect.top && pt.y <= rect.bottom;
    }

    std::wstring BuildThinkingLabel(DWORD animationTick)
    {
        if (animationTick == 0)
            return L"Thinking...";

        const DWORD elapsed = GetTickCount() - animationTick;
        const int dotCount = (int)((elapsed / 420) % 4);
        std::wstring label = L"Thinking";
        label.append((size_t)dotCount, L'.');
        return label;
    }

    std::wstring Utf8ToWideLocal(const std::string &text)
    {
        if (text.empty())
            return {};

        int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr, 0);
        if (len <= 0)
            len = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
        if (len <= 0)
            return {};

        std::wstring out((size_t)len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), len);
        return out;
    }

    std::string ExtractJsonStringField(const std::string &json, const std::string &key)
    {
        const std::string needle = "\"" + key + "\"";
        size_t pos = json.find(needle);
        if (pos == std::string::npos)
            return {};

        pos = json.find(':', pos + needle.size());
        if (pos == std::string::npos)
            return {};

        pos = json.find('"', pos + 1);
        if (pos == std::string::npos)
            return {};

        std::string out;
        bool escape = false;
        for (size_t i = pos + 1; i < json.size(); ++i)
        {
            char ch = json[i];
            if (escape)
            {
                switch (ch)
                {
                case '"':
                case '\\':
                case '/':
                    out.push_back(ch);
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                default:
                    out.push_back(ch);
                    break;
                }
                escape = false;
                continue;
            }

            if (ch == '\\')
            {
                escape = true;
                continue;
            }

            if (ch == '"')
                break;

            out.push_back(ch);
        }
        return out;
    }

    int ExtractJsonIntField(const std::string &json, const std::string &key, int fallback = 0)
    {
        const std::string needle = "\"" + key + "\"";
        size_t pos = json.find(needle);
        if (pos == std::string::npos)
            return fallback;

        pos = json.find(':', pos + needle.size());
        if (pos == std::string::npos)
            return fallback;

        ++pos;
        while (pos < json.size() && isspace((unsigned char)json[pos]))
            ++pos;

        size_t start = pos;
        while (pos < json.size() && isdigit((unsigned char)json[pos]))
            ++pos;
        if (start == pos)
            return fallback;

        return atoi(json.substr(start, pos - start).c_str());
    }

    std::wstring TruncateText(const std::wstring &text, size_t maxChars)
    {
        if (text.size() <= maxChars)
            return text;
        if (maxChars < 4)
            return text.substr(0, maxChars);
        return text.substr(0, maxChars - 3) + L"...";
    }

    std::wstring FormatIsoTimestampShort(const std::string &timestamp)
    {
        std::wstring wide = Utf8ToWideLocal(timestamp);
        if (wide.empty())
            return {};

        std::replace(wide.begin(), wide.end(), L'T', L' ');
        if (!wide.empty() && wide.back() == L'Z')
            wide.pop_back();
        if (wide.size() >= 16)
            return wide.substr(0, 16);
        return wide;
    }

    std::wstring ExtractUserFacingPrompt(const std::wstring &text)
    {
        const std::wstring marker = L"User request:\n";
        size_t markerPos = text.rfind(marker);
        std::wstring cleaned = markerPos == std::wstring::npos ? text : text.substr(markerPos + marker.size());
        cleaned = TrimWideLocal(cleaned);

        const std::wstring ideMarkerOpen = L"<ide_opened_file>";
        if (cleaned.rfind(ideMarkerOpen, 0) == 0)
        {
            size_t close = cleaned.find(L"</ide_opened_file>");
            if (close != std::wstring::npos)
                cleaned = TrimWideLocal(cleaned.substr(close + 18));
        }

        return cleaned;
    }

    std::wstring SanitizeClaudeProjectName(const std::wstring &root)
    {
        std::wstring out;
        out.reserve(root.size());
        for (wchar_t ch : root)
        {
            if ((ch >= L'a' && ch <= L'z') ||
                (ch >= L'A' && ch <= L'Z') ||
                (ch >= L'0' && ch <= L'9'))
            {
                out.push_back((wchar_t)towlower(ch));
            }
            else
            {
                out.push_back(L'-');
            }
        }
        return out;
    }

    std::wstring ToTitleCaseLocal(std::wstring value)
    {
        bool upperNext = true;
        for (wchar_t &ch : value)
        {
            if (ch == L' ' || ch == L'-' || ch == L'_')
            {
                upperNext = true;
                if (ch == L'_')
                    ch = L' ';
                continue;
            }

            ch = upperNext ? (wchar_t)towupper(ch) : (wchar_t)towlower(ch);
            upperNext = false;
        }
        return value;
    }

    std::wstring FormatAccountValue(const std::wstring &value, const std::wstring &fallback, bool uppercaseClaude = false)
    {
        std::wstring trimmed = TrimWideLocal(value);
        if (trimmed.empty())
            return fallback;

        std::wstring lowered = trimmed;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), towlower);
        if (lowered == L"claude.ai")
            return uppercaseClaude ? L"Claude AI" : L"claude.ai";
        if (lowered == L"pro")
            return L"Claude Pro";
        if (lowered == L"max")
            return L"Claude Max";
        if (lowered == L"free")
            return L"Claude Free";
        return ToTitleCaseLocal(trimmed);
    }

    std::wstring ExtractRateLimitResetText(const std::wstring &text)
    {
        std::wstring cleaned = TrimWideLocal(text);
        size_t marker = cleaned.find(L"resets ");
        if (marker == std::wstring::npos)
            return {};

        std::wstring suffix = TrimWideLocal(cleaned.substr(marker + 7));
        if (suffix.empty())
            return {};
        return L"Resets " + suffix;
    }

    std::wstring FormatRateLimitPercent(int usedPercentage)
    {
        if (usedPercentage < 0)
            return L"Not reported";

        usedPercentage = (std::max)(0, (std::min)(100, usedPercentage));
        return std::to_wstring(usedPercentage) + L"%";
    }

    std::wstring FormatRateLimitPercentCompact(int usedPercentage)
    {
        if (usedPercentage < 0)
            return L"N/A";

        usedPercentage = (std::max)(0, (std::min)(100, usedPercentage));
        return std::to_wstring(usedPercentage) + L"%";
    }

    std::wstring FormatRateLimitStatusCompact(const ClaudeCliBridge::RateLimitWindow &window)
    {
        if (window.usedPercentage >= 0)
            return FormatRateLimitPercentCompact(window.usedPercentage);

        if (!window.available)
            return L"N/A";

        std::wstring status = TrimWideLocal(window.status);
        std::transform(status.begin(), status.end(), status.begin(), towlower);

        std::wstring overageStatus = TrimWideLocal(window.overageStatus);
        std::transform(overageStatus.begin(), overageStatus.end(), overageStatus.begin(), towlower);

        if (window.isUsingOverage)
            return L"Overage";
        if (status == L"allowed")
            return L"Allowed";
        if (status == L"warning")
            return L"Warning";
        if (status == L"limited")
            return L"Limited";
        if (status == L"blocked" || status == L"rejected" || status == L"denied")
            return L"Blocked";
        if (overageStatus == L"available")
            return L"Overage OK";
        if (overageStatus == L"rejected" || overageStatus == L"disabled")
            return L"No overage";

        return L"Live";
    }

    std::wstring BuildRateLimitDetailText(const ClaudeCliBridge::RateLimitWindow &window,
                                          const std::wstring &baseResetText)
    {
        std::wstring detail = baseResetText;

        std::wstring status = TrimWideLocal(window.status);
        std::transform(status.begin(), status.end(), status.begin(), towlower);

        std::wstring overageStatus = TrimWideLocal(window.overageStatus);
        std::transform(overageStatus.begin(), overageStatus.end(), overageStatus.begin(), towlower);

        std::wstring suffix;
        if (window.isUsingOverage)
            suffix = L"Using overage credits";
        else if (status == L"allowed")
            suffix = L"Usage currently allowed";
        else if (status == L"warning")
            suffix = L"Approaching limit";
        else if (status == L"limited")
            suffix = L"Usage limited";
        else if (status == L"blocked" || status == L"rejected" || status == L"denied")
            suffix = L"Usage blocked";
        else if (overageStatus == L"available")
            suffix = L"Overage available";
        else if (overageStatus == L"rejected" || overageStatus == L"disabled")
            suffix = L"Overage unavailable";

        if (suffix.empty())
            return detail;
        if (detail.empty())
            return suffix;
        return detail + L"  •  " + suffix;
    }

    std::wstring FormatRateLimitResetLabel(long long resetUnixSeconds)
    {
        if (resetUnixSeconds <= 0)
            return {};

        std::time_t now = std::time(nullptr);
        long long remaining = resetUnixSeconds - (long long)now;
        if (remaining > 0)
        {
            if (remaining < 3600)
            {
                int minutes = (int)((remaining + 59) / 60);
                return L"Resets in " + std::to_wstring((std::max)(1, minutes)) + L"m";
            }
            if (remaining < 86400)
            {
                int hours = (int)((remaining + 3599) / 3600);
                return L"Resets in " + std::to_wstring((std::max)(1, hours)) + L"h";
            }

            int days = (int)((remaining + 86399) / 86400);
            return L"Resets in " + std::to_wstring((std::max)(1, days)) + L"d";
        }

        __time64_t resetTime = (__time64_t)resetUnixSeconds;
        std::tm tmLocal{};
        if (_localtime64_s(&tmLocal, &resetTime) != 0)
            return {};

        wchar_t buffer[64] = {};
        if (wcsftime(buffer, sizeof(buffer) / sizeof(buffer[0]), L"Resets %a %H:%M", &tmLocal) == 0)
            return {};
        return buffer;
    }

}

ClaudePanel::ClaudePanel()
    : Panel(PanelId::Claude)
{
    config_ = PanelConfig(
        PanelId::Claude,
        L"assets/ressource/icons/claude.svg",
        L"Claude",
        true,
        false,
        5);
    title_ = L"CLAUDE";
    state_.logicalWidth = 360;
    state_.minWidth = 280;
    state_.maxWidth = 720;

    bridge_.onAuthStatus = [this](ClaudeCliBridge::AuthState /*state*/, const std::wstring &detail) {
        authDetail_ = detail;
        InvalidatePanel();
    };

    bridge_.onAuthInfo = [this](const ClaudeCliBridge::AuthInfo &info) {
        accountInfo_ = info;
        InvalidatePanel();
    };

    bridge_.onRequestStarted = [this]() {
        requestInFlight_ = true;
        requestAnimationTick_ = GetTickCount();
        InvalidatePanel();
    };

    bridge_.onTextDelta = [this](const std::wstring &delta) {
        int index = EnsureStreamingAssistantMessage();
        if (index < 0 || index >= (int)messages_.size())
            return;

        messages_[(size_t)index].content += delta;
        messages_[(size_t)index].estimatedHeight =
            EstimateMessageHeight(messages_[(size_t)index].content,
                                  MeasureWidthForRole(messages_[(size_t)index].role));
        ScrollMessagesToBottom();
        InvalidatePanel();
    };

    bridge_.onToolEvent = [this](const ClaudeCliBridge::ToolEvent &event) {
        QueueToolMessage(event);
        InvalidatePanel();
    };

    bridge_.onRateLimitInfo = [this](const ClaudeCliBridge::RateLimitInfo &info) {
        rateLimits_ = info;
        InvalidatePanel();
    };

    bridge_.onRequestFinished = [this](const std::wstring &sessionId, const std::wstring &fallbackResult) {
        requestInFlight_ = false;
        requestAnimationTick_ = 0;

        if (!sessionId.empty())
            sessionId_ = sessionId;

        if (streamingMessageIndex_ < 0 || streamingMessageIndex_ >= (int)messages_.size())
            EnsureStreamingAssistantMessage();

        if (streamingMessageIndex_ >= 0 && streamingMessageIndex_ < (int)messages_.size())
        {
            ChatMessage &message = messages_[(size_t)streamingMessageIndex_];
            if (Trim(message.content).empty() && !fallbackResult.empty())
                message.content = fallbackResult;
            if (Trim(message.content).empty())
                message.content = L"(No textual response received.)";
            message.estimatedHeight = EstimateMessageHeight(message.content,
                MeasureWidthForRole(message.role));
        }

        streamingMessageIndex_ = -1;
        RefreshConversationHistory(true);
        SyncCurrentHistorySelection();
        ScrollMessagesToBottom();
        InvalidatePanel();
    };

    bridge_.onError = [this](const std::wstring &message) {
        requestInFlight_ = false;
        streamingMessageIndex_ = -1;
        requestAnimationTick_ = 0;
        QueueSystemMessage(message, MessageRole::Error);
        InvalidatePanel();
    };
}

ClaudePanel::~ClaudePanel()
{
    bridge_.Cancel();
}

void ClaudePanel::Initialize()
{
    ApplyInputTheme(executableInput_, L"Auto-detect claude.cmd or claude.exe", false);
    ApplyInputTheme(promptInput_, L"Ask Claude about the current file, selection, or project...", true);

    executableInput_.onSubmit = [this]() {
        configuredExecutablePath_ = Trim(executableInput_.GetText());
        SaveConfiguredExecutable();
        RefreshAuthStatus();
    };

    executableInput_.onEscape = [this]() {
        executableInput_.SetFocused(false);
        executableInput_.SetText(configuredExecutablePath_);
        InvalidatePanel();
    };

    promptInput_.onEscape = [this]() {
        promptInput_.SetFocused(false);
        InvalidatePanel();
    };

    LoadConfiguredExecutable();
    ResolveExecutablePath(true);
    RefreshConversationHistory(true);
}

void ClaudePanel::ApplyInputTheme(TextInput &input, const std::wstring &placeholder, bool multiline)
{
    input.SetPlaceholder(placeholder);
    auto &style = input.GetStyle();
    style.useSearchBoxStyle = false;
    style.backgroundColor = UI::InputTheme::Background();
    style.borderColor = UI::InputTheme::Border();
    style.focusBorderColor = UI::InputTheme::FocusBorder();
    style.textColor = UI::InputTheme::Text();
    style.placeholderColor = UI::InputTheme::Placeholder();
    style.selectionColor = UI::InputTheme::Selection();
    style.cursorColor = UI::InputTheme::Caret();
    style.cornerRadius = UI::InputTheme::kCornerRadius;
    style.fontFamily = UI::InputTheme::kFontFamily;
    style.fontSize = multiline ? 12.4f : UI::InputTheme::kFontSize;
    style.padding = multiline ? 5.0f : UI::InputTheme::kHorizontalPadding;
    style.paddingLeft = multiline ? 8.0f : UI::InputTheme::kHorizontalPadding;
    style.paddingRight = multiline ? 8.0f : UI::InputTheme::kHorizontalPadding;
    if (multiline)
        style.cornerRadius = 5.0f;
    style.multiline = multiline;
}

void ClaudePanel::LoadConfiguredExecutable()
{
    configuredExecutablePath_.clear();
    const std::wstring path = GetSettingsPath();
    if (path.empty())
        return;

    std::wifstream in{std::filesystem::path(path)};
    if (!in.is_open())
        return;

    std::getline(in, configuredExecutablePath_);
    configuredExecutablePath_ = Trim(configuredExecutablePath_);
    executableInput_.SetText(configuredExecutablePath_);
}

void ClaudePanel::SaveConfiguredExecutable()
{
    const std::wstring path = GetSettingsPath();
    if (path.empty())
        return;

    std::wofstream out{std::filesystem::path(path), std::ios::trunc};
    if (!out.is_open())
        return;

    out << configuredExecutablePath_;
}

std::wstring ClaudePanel::GetSettingsPath()
{
    PWSTR appDataPath = nullptr;
    std::filesystem::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appDataPath)) && appDataPath)
    {
        out = std::filesystem::path(appDataPath) / L"Nebula";
        CoTaskMemFree(appDataPath);
        std::error_code ec;
        std::filesystem::create_directories(out, ec);
        out /= L"claude-cli.txt";
    }
    return out.wstring();
}

void ClaudePanel::ResolveExecutablePath(bool preferSavedPath)
{
    std::wstring detected = DetectClaudeExecutable();
    std::wstring preferred = Trim(configuredExecutablePath_);

    if (preferSavedPath && !preferred.empty())
    {
        executableInput_.SetText(preferred);
        return;
    }

    if (!detected.empty())
    {
        configuredExecutablePath_ = detected;
        executableInput_.SetText(detected);
        SaveConfiguredExecutable();
    }
    else
    {
        executableInput_.SetText(preferred);
    }
}

void ClaudePanel::RefreshAuthStatus()
{
    configuredExecutablePath_ = Trim(executableInput_.GetText());
    SaveConfiguredExecutable();
    bridge_.CheckAuthStatusAsync(configuredExecutablePath_, ResolveWorkingDirectory());
}

bool ClaudePanel::ShouldShowExecutableInput() const
{
    if (showExecutableInput_)
        return true;

    if (executableInput_.IsFocused())
        return true;

    if (Trim(executableInput_.GetText()).empty())
        return true;

    ClaudeCliBridge::AuthState state = bridge_.GetAuthState();
    return state == ClaudeCliBridge::AuthState::NotConfigured ||
           state == ClaudeCliBridge::AuthState::NeedsLogin ||
           state == ClaudeCliBridge::AuthState::Error;
}

void ClaudePanel::StartLoginFlow()
{
    const std::wstring exePath = Trim(executableInput_.GetText());
    if (exePath.empty())
    {
        QueueSystemMessage(L"Configure or auto-detect the Claude CLI path before starting login.", MessageRole::Error);
        return;
    }

    std::wstring args = L"/K " + ClaudeCliBridge::QuoteArg(ClaudeCliBridge::QuoteArg(exePath) + L" auth login");
    HINSTANCE result = ShellExecuteW(nullptr, L"open", L"cmd.exe", args.c_str(),
                                     ResolveWorkingDirectory().c_str(), SW_SHOWNORMAL);
    if ((INT_PTR)result <= 32)
    {
        QueueSystemMessage(L"Unable to open `claude auth login` in a new terminal window.", MessageRole::Error);
        return;
    }

    QueueSystemMessage(L"Opened `claude auth login` in a separate terminal. Finish the login there, then click Refresh.");
}

std::wstring ClaudePanel::ResolveWorkingDirectory() const
{
    const std::wstring root = Trim(GetExplorerManager().GetState().rootPath);
    if (!root.empty())
        return root;

    wchar_t currentDir[MAX_PATH] = {};
    DWORD len = GetCurrentDirectoryW(MAX_PATH, currentDir);
    if (len == 0 || len >= MAX_PATH)
        return {};
    return std::wstring(currentDir, currentDir + len);
}

std::wstring ClaudePanel::Trim(const std::wstring &text)
{
    size_t start = 0;
    while (start < text.size() && iswspace(text[start]))
        ++start;
    size_t end = text.size();
    while (end > start && iswspace(text[end - 1]))
        --end;
    return text.substr(start, end - start);
}

std::wstring ClaudePanel::JoinLines(const std::vector<std::wstring> &lines, size_t maxChars, bool &outTruncated)
{
    outTruncated = false;
    std::wstring out;
    size_t total = 0;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        const std::wstring &line = lines[i];
        if (!out.empty())
            out.push_back(L'\n');

        if (total + line.size() > maxChars)
        {
            size_t remaining = maxChars > total ? (maxChars - total) : 0;
            out.append(line.substr(0, remaining));
            outTruncated = true;
            break;
        }

        out.append(line);
        total += line.size();
        if (total >= maxChars)
        {
            outTruncated = true;
            break;
        }
    }
    return out;
}

std::wstring ClaudePanel::BuildPromptWithContext(const std::wstring &userPrompt) const
{
    std::wstringstream prompt;
    prompt << L"You are assisting inside the Nebula code editor.\n";
    prompt << L"Keep the answer concise, concrete, and focused on the current coding task.\n";
    prompt << L"You may use Read, LS, Glob, Grep, Bash, Edit, Write, and MultiEdit when needed.\n";
    prompt << L"Prefer reading before editing, and use Bash for concrete commands or file execution when useful.\n\n";

    const std::wstring root = Trim(GetExplorerManager().GetState().rootPath);
    if (!root.empty())
        prompt << L"Project root: " << root << L"\n";

    if (hwnd_)
    {
        if (Window *window = GetWindowFromHwnd(hwnd_))
        {
            if (Orion::Editor *editor = window->GetEditor())
            {
                const std::wstring filePath = Trim(editor->GetFilePath());
                if (!filePath.empty())
                    prompt << L"Active file: " << filePath << L"\n";

                std::wstring selection = Trim(editor->GetSelectionText());
                if (!selection.empty())
                    prompt << L"\nSelected text:\n```text\n" << selection << L"\n```\n";

                bool truncated = false;
                std::wstring fileContents = JoinLines(editor->GetLinesSnapshot(), 12000, truncated);
                if (!fileContents.empty())
                {
                    prompt << L"\nCurrent file contents";
                    if (truncated)
                        prompt << L" (truncated)";
                    prompt << L":\n```text\n" << fileContents << L"\n```\n";
                }
            }
        }
    }

    prompt << L"\nUser request:\n" << userPrompt << L"\n";
    return prompt.str();
}

void ClaudePanel::QueueSystemMessage(const std::wstring &text, MessageRole role)
{
    ChatMessage message;
    message.role = role;
    message.kind = MessageKind::Text;
    message.content = text;
    message.estimatedHeight = EstimateMessageHeight(text, MeasureWidthForRole(role));
    messages_.push_back(std::move(message));
    ScrollMessagesToBottom();
}

void ClaudePanel::QueueToolMessage(const ClaudeCliBridge::ToolEvent &event)
{
    if (streamingMessageIndex_ >= 0 && streamingMessageIndex_ < (int)messages_.size())
    {
        ChatMessage &streaming = messages_[(size_t)streamingMessageIndex_];
        if (streaming.kind == MessageKind::Text && streaming.role == MessageRole::Assistant &&
            Trim(streaming.content).empty())
        {
            messages_.erase(messages_.begin() + streamingMessageIndex_);
        }
        streamingMessageIndex_ = -1;
    }

    ChatMessage message;
    message.role = MessageRole::Tool;
    message.kind = MessageKind::Tool;
    message.title = event.title;
    message.subtitle = event.success ? L"Success" : L"Failed";
    message.content = event.details;
    message.success = event.success;

    const float width = MeasureWidthForRole(message.role);
    float bodyHeight = EstimateWrappedTextHeight(message.content, width - 24.0f, 7.6f, 16.0f);
    message.estimatedHeight = 38.0f + bodyHeight;
    messages_.push_back(std::move(message));
    ScrollMessagesToBottom();
}

int ClaudePanel::EnsureStreamingAssistantMessage()
{
    if (streamingMessageIndex_ >= 0 && streamingMessageIndex_ < (int)messages_.size())
    {
        ChatMessage &message = messages_[(size_t)streamingMessageIndex_];
        if (message.role == MessageRole::Assistant && message.kind == MessageKind::Text)
            return streamingMessageIndex_;
    }

    ChatMessage assistantMessage;
    assistantMessage.role = MessageRole::Assistant;
    assistantMessage.kind = MessageKind::Text;
    assistantMessage.content.clear();
    assistantMessage.estimatedHeight = EstimateMessageHeight(L"Thinking...",
        MeasureWidthForRole(assistantMessage.role));
    messages_.push_back(std::move(assistantMessage));
    streamingMessageIndex_ = (int)messages_.size() - 1;
    return streamingMessageIndex_;
}

std::wstring ClaudePanel::GetDisplayMessageText(size_t index) const
{
    if (index >= messages_.size())
        return {};

    const ChatMessage &message = messages_[index];
    if (message.role == MessageRole::Assistant &&
        requestInFlight_ &&
        (int)index == streamingMessageIndex_ &&
        message.content.empty())
    {
        return BuildThinkingLabel(requestAnimationTick_);
    }

    return message.content;
}

bool ClaudePanel::ShouldUseMarkdownPreview(const ChatMessage &message, const std::wstring &displayText) const
{
    if (message.kind != MessageKind::Text)
        return false;
    if (message.role != MessageRole::Assistant && message.role != MessageRole::System)
        return false;
    return !displayText.empty();
}

void ClaudePanel::RefreshMarkdownPreviews(IDWriteFactory *dwrite)
{
    markdownPreviewCache_.resize(messages_.size());

    for (size_t i = 0; i < messages_.size(); ++i)
    {
        ChatMessage &message = messages_[i];
        const float width = MeasureWidthForRole(message.role);
        const std::wstring displayText = GetDisplayMessageText(i);

        if (message.kind == MessageKind::Tool)
        {
            float bodyHeight = EstimateWrappedTextHeight(message.content, width - 24.0f, 7.6f, 16.0f);
            message.estimatedHeight = 38.0f + bodyHeight;
            markdownPreviewCache_[i] = {};
            continue;
        }

        if (!ShouldUseMarkdownPreview(message, displayText))
        {
            message.estimatedHeight = EstimateMessageHeight(displayText, width);
            markdownPreviewCache_[i] = {};
            continue;
        }

        MarkdownPreviewCacheEntry &entry = markdownPreviewCache_[i];
        if (!entry.editor)
        {
            entry.editor = std::make_unique<Orion::Editor>();
            entry.editor->SetEmbeddedPreviewMode(true);
        }

        if (entry.cachedText != displayText || std::fabs(entry.cachedWidth - width) > 1.0f)
        {
            entry.editor->CreateEmpty();
            entry.editor->SetEmbeddedPreviewMode(true);
            entry.editor->SetTextContent(displayText, false);
            entry.editor->SetMarkdownViewMode(Orion::MarkdownViewMode::Preview);
            entry.cachedText = displayText;
            entry.cachedWidth = width;
            entry.measuredHeight = entry.editor->MeasureMarkdownPreviewHeight(dwrite, width);
        }

        message.estimatedHeight = (std::max)(54.0f, entry.measuredHeight);
    }
}

std::wstring ClaudePanel::ResolveClaudeProjectHistoryDirectory() const
{
    const std::wstring root = Trim(ResolveWorkingDirectory());
    if (root.empty())
        return {};

    const std::wstring userProfile = ExpandEnvPath(L"USERPROFILE", L".claude\\projects");
    if (userProfile.empty())
        return {};

    std::filesystem::path projectsRoot(userProfile);
    std::filesystem::path exact = projectsRoot / SanitizeClaudeProjectName(root);
    std::error_code ec;
    if (std::filesystem::exists(exact, ec) && !ec)
        return exact.wstring();

    std::wstring normalized = SanitizeClaudeProjectName(root);
    for (const auto &entry : std::filesystem::directory_iterator(projectsRoot, ec))
    {
        if (ec || !entry.is_directory())
            continue;
        std::wstring name = entry.path().filename().wstring();
        std::wstring lowered = name;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), towlower);
        if (lowered == normalized)
            return entry.path().wstring();
    }

    return {};
}

void ClaudePanel::RefreshConversationHistory(bool force)
{
    const std::wstring projectDir = ResolveClaudeProjectHistoryDirectory();
    if (!force && projectDir == historyProjectDirectory_)
        return;

    historyProjectDirectory_ = projectDir;
    historyEntries_.clear();
    historyRowRects_.clear();
    hoveredHistoryIndex_ = -1;

    if (projectDir.empty())
    {
        selectedHistoryIndex_ = -1;
        currentConversationUsage_ = {};
        lastKnownRateLimitResetText_.clear();
        return;
    }

    std::error_code ec;
    std::wstring latestRateLimitResetText;
    std::string latestRateLimitTimestamp;
    for (const auto &entry : std::filesystem::directory_iterator(projectDir, ec))
    {
        if (ec || !entry.is_regular_file())
            continue;
        if (entry.path().extension() != L".jsonl")
            continue;

        ConversationHistoryEntry historyEntry;
        historyEntry.sessionId = entry.path().stem().wstring();
        historyEntry.jsonlPath = entry.path().wstring();

        std::ifstream in(entry.path(), std::ios::binary);
        if (!in.is_open())
            continue;

        std::string line;
        std::wstring firstPrompt;
        std::string latestTimestamp;
        while (std::getline(in, line))
        {
            if (line.find("\"timestamp\":\"") != std::string::npos)
            {
                std::string timestamp = ExtractJsonStringField(line, "timestamp");
                if (!timestamp.empty() && timestamp > latestTimestamp)
                    latestTimestamp = timestamp;

                if (line.find("\"error\":\"rate_limit\"") != std::string::npos)
                {
                    std::wstring resetText = ExtractRateLimitResetText(Utf8ToWideLocal(ExtractJsonStringField(line, "text")));
                    if (!resetText.empty() && timestamp >= latestRateLimitTimestamp)
                    {
                        latestRateLimitTimestamp = timestamp;
                        latestRateLimitResetText = resetText;
                    }
                }
            }

            if (line.find("\"type\":\"ai-title\"") != std::string::npos)
            {
                std::wstring title = Trim(Utf8ToWideLocal(ExtractJsonStringField(line, "aiTitle")));
                if (!title.empty())
                    historyEntry.title = title;
            }

            if (line.find("\"type\":\"user\"") != std::string::npos &&
                line.find("\"tool_result\"") == std::string::npos &&
                firstPrompt.empty())
            {
                std::wstring prompt = ExtractUserFacingPrompt(Utf8ToWideLocal(ExtractJsonStringField(line, "content")));
                if (prompt.empty() || prompt == L"type")
                    prompt = ExtractUserFacingPrompt(Utf8ToWideLocal(ExtractJsonStringField(line, "text")));
                if (!prompt.empty())
                    firstPrompt = prompt;
            }

            if (line.find("\"type\":\"assistant\"") != std::string::npos)
            {
                historyEntry.usage.inputTokens += ExtractJsonIntField(line, "input_tokens", 0);
                historyEntry.usage.outputTokens += ExtractJsonIntField(line, "output_tokens", 0);
                if (line.find("\"stop_reason\":\"end_turn\"") != std::string::npos)
                    ++historyEntry.usage.assistantMessages;
                if (line.find("\"type\":\"tool_use\"") != std::string::npos)
                    ++historyEntry.usage.toolCalls;
            }

            if (line.find("\"type\":\"user\"") != std::string::npos &&
                line.find("\"tool_result\"") == std::string::npos)
            {
                ++historyEntry.usage.userMessages;
            }
        }

        historyEntry.sortTimestamp = Utf8ToWideLocal(latestTimestamp);
        historyEntry.usage.lastUpdated = FormatIsoTimestampShort(latestTimestamp);

        if (historyEntry.title.empty())
            historyEntry.title = !firstPrompt.empty() ? TruncateText(firstPrompt, 56) : historyEntry.sessionId;

        if (!firstPrompt.empty() && firstPrompt != historyEntry.title)
            historyEntry.subtitle = TruncateText(firstPrompt, 88);
        else if (!historyEntry.usage.lastUpdated.empty())
            historyEntry.subtitle = historyEntry.usage.lastUpdated;
        else
            historyEntry.subtitle = historyEntry.sessionId;

        historyEntries_.push_back(std::move(historyEntry));
    }

    lastKnownRateLimitResetText_ = latestRateLimitResetText;

    std::sort(historyEntries_.begin(), historyEntries_.end(),
              [](const ConversationHistoryEntry &a, const ConversationHistoryEntry &b) {
                  return a.sortTimestamp > b.sortTimestamp;
              });

    SyncCurrentHistorySelection();
}

void ClaudePanel::SyncCurrentHistorySelection()
{
    selectedHistoryIndex_ = -1;
    currentConversationUsage_ = {};

    for (size_t i = 0; i < historyEntries_.size(); ++i)
    {
        if (historyEntries_[i].sessionId == sessionId_)
        {
            selectedHistoryIndex_ = (int)i;
            currentConversationUsage_ = historyEntries_[i].usage;
            return;
        }
    }

    if (selectedHistoryIndex_ < 0 && !historyEntries_.empty() && sessionId_.empty())
        currentConversationUsage_ = historyEntries_.front().usage;
}

bool ClaudePanel::LoadConversationFromHistoryIndex(size_t index)
{
    if (index >= historyEntries_.size())
        return false;

    const ConversationHistoryEntry &entry = historyEntries_[index];
    std::ifstream in(std::filesystem::path(entry.jsonlPath), std::ios::binary);
    if (!in.is_open())
        return false;

    std::vector<ChatMessage> loadedMessages;
    std::string line;
    while (std::getline(in, line))
    {
        if (line.find("\"type\":\"user\"") != std::string::npos &&
            line.find("\"tool_result\"") == std::string::npos)
        {
            std::wstring prompt = ExtractUserFacingPrompt(Utf8ToWideLocal(ExtractJsonStringField(line, "content")));
            if (prompt.empty() || prompt == L"type")
                prompt = ExtractUserFacingPrompt(Utf8ToWideLocal(ExtractJsonStringField(line, "text")));
            if (prompt.empty())
                continue;

            ChatMessage message;
            message.role = MessageRole::User;
            message.kind = MessageKind::Text;
            message.content = prompt;
            loadedMessages.push_back(std::move(message));
            continue;
        }

        if (line.find("\"type\":\"assistant\"") != std::string::npos &&
            line.find("\"stop_reason\":\"end_turn\"") != std::string::npos)
        {
            std::wstring response = Trim(Utf8ToWideLocal(ExtractJsonStringField(line, "text")));
            if (response.empty())
                continue;

            ChatMessage message;
            message.role = MessageRole::Assistant;
            message.kind = MessageKind::Text;
            message.content = response;
            loadedMessages.push_back(std::move(message));
        }
    }

    messages_ = std::move(loadedMessages);
    markdownPreviewCache_.clear();
    sessionId_ = entry.sessionId;
    selectedHistoryIndex_ = (int)index;
    currentConversationUsage_ = entry.usage;
    showHistory_ = false;
    hoveredHistoryIndex_ = -1;
    RecomputeMessageHeights();
    ScrollMessagesToBottom();
    InvalidatePanel();
    return true;
}

std::wstring ClaudePanel::CurrentConversationTitle() const
{
    if (selectedHistoryIndex_ >= 0 && selectedHistoryIndex_ < (int)historyEntries_.size())
        return historyEntries_[(size_t)selectedHistoryIndex_].title;
    if (!historyEntries_.empty())
        return historyEntries_.front().title;
    return sessionId_.empty() ? L"New conversation" : sessionId_;
}

float ClaudePanel::EstimateWrappedTextHeight(const std::wstring &text, float width, float charsPerLineDivisor, float lineHeight)
{
    const float usableWidth = (std::max)(80.0f, width);
    const float charsPerLine = (std::max)(16.0f, usableWidth / charsPerLineDivisor);

    size_t lineCount = 1;
    size_t currentRun = 0;
    for (wchar_t ch : text)
    {
        if (ch == L'\n')
        {
            ++lineCount;
            currentRun = 0;
            continue;
        }

        ++currentRun;
        if ((float)currentRun >= charsPerLine)
        {
            ++lineCount;
            currentRun = 0;
        }
    }

    return (float)lineCount * lineHeight;
}

float ClaudePanel::EstimateMessageHeight(const std::wstring &text, float width) const
{
    return 10.0f + EstimateWrappedTextHeight(text, width, 7.8f, 15.5f);
}

float ClaudePanel::MeasureWidthForRole(MessageRole role) const
{
    const float baseWidth = (std::max)(80.0f, messagesRect_.right - messagesRect_.left - 12.0f);
    if (role == MessageRole::User)
        return (std::max)(92.0f, baseWidth * 0.64f - 12.0f);
    if (role == MessageRole::Tool)
        return (std::max)(120.0f, baseWidth - 6.0f);
    return (std::max)(140.0f, baseWidth * 0.86f);
}

float ClaudePanel::ComputeMessagesContentHeight() const
{
    float total = 0.0f;
    for (const ChatMessage &message : messages_)
        total += message.estimatedHeight + 4.0f;
    return total + 8.0f;
}

float ClaudePanel::ComputeHistoryContentHeight() const
{
    if (historyEntries_.empty())
        return 92.0f;
    return 12.0f + (float)historyEntries_.size() * 64.0f;
}

float ClaudePanel::ComputeScrollContentHeight() const
{
    return showHistory_ ? ComputeHistoryContentHeight() : ComputeMessagesContentHeight();
}

void ClaudePanel::RecomputeMessageHeights()
{
    for (size_t i = 0; i < messages_.size(); ++i)
    {
        ChatMessage &message = messages_[i];
        const float width = MeasureWidthForRole(message.role);
        if (message.kind == MessageKind::Tool)
        {
            float bodyHeight = EstimateWrappedTextHeight(message.content, width - 18.0f, 7.8f, 14.5f);
            message.estimatedHeight = 28.0f + bodyHeight;
        }
        else
        {
            message.estimatedHeight = EstimateMessageHeight(GetDisplayMessageText(i), width);
        }
    }
}

void ClaudePanel::ScrollMessagesToBottom()
{
    float contentHeight = ComputeMessagesContentHeight();
    float viewport = (std::max)(0.0f, messagesRect_.bottom - messagesRect_.top);
    messagesScrollbar_.SetScrollOffset((std::max)(0.0f, contentHeight - viewport));
}

void ClaudePanel::InvalidatePanel() const
{
    if (hwnd_)
        InvalidateRect(hwnd_, nullptr, FALSE);
}

bool ClaudePanel::UpdateUiAnimation()
{
    return requestInFlight_ &&
           streamingMessageIndex_ >= 0 &&
           streamingMessageIndex_ < (int)messages_.size() &&
           messages_[(size_t)streamingMessageIndex_].content.empty();
}

void ClaudePanel::SubmitPrompt()
{
    if (requestInFlight_ || bridge_.IsBusy())
        return;

    const std::wstring rawPrompt = Trim(promptInput_.GetText());
    if (rawPrompt.empty())
        return;

    ChatMessage userMessage;
    userMessage.role = MessageRole::User;
    userMessage.kind = MessageKind::Text;
    userMessage.content = rawPrompt;
    userMessage.estimatedHeight = EstimateMessageHeight(rawPrompt, MeasureWidthForRole(userMessage.role));
    messages_.push_back(std::move(userMessage));

    ChatMessage assistantMessage;
    assistantMessage.role = MessageRole::Assistant;
    assistantMessage.kind = MessageKind::Text;
    assistantMessage.content = L"";
    assistantMessage.estimatedHeight = EstimateMessageHeight(L"Thinking...",
        MeasureWidthForRole(assistantMessage.role));
    messages_.push_back(std::move(assistantMessage));
    streamingMessageIndex_ = (int)messages_.size() - 1;

    promptInput_.SetText(L"");
    ScrollMessagesToBottom();

    ClaudeCliBridge::RequestOptions options;
    options.executablePath = Trim(executableInput_.GetText());
    options.workingDirectory = ResolveWorkingDirectory();
    options.resumeSessionId = sessionId_;
    options.prompt = BuildPromptWithContext(rawPrompt);

    if (!bridge_.StartRequest(options))
    {
        requestInFlight_ = false;
        streamingMessageIndex_ = -1;
        QueueSystemMessage(L"A Claude request is already running.", MessageRole::Error);
    }

    InvalidatePanel();
}

bool ClaudePanel::CanSubmitPrompt() const
{
    return !requestInFlight_ &&
           !bridge_.IsBusy() &&
           !Trim(promptInput_.GetText()).empty() &&
           !Trim(executableInput_.GetText()).empty();
}

void ClaudePanel::UpdateLayout(HWND hwnd)
{
    hwnd_ = hwnd;

    UINT dpi = win32_get_dpi_for_window(hwnd);
    float scale = dpi / 96.0f;
    RECT clientRect;
    GetClientRect(hwnd, &clientRect);
    RECT tbRect = win32_titlebar_rect(hwnd);
    int footerHeight = win32_dpi_scale(28, dpi);

    state_.physicalWidth = static_cast<int>(state_.logicalWidth * scale);
    state_.rightEdge = static_cast<float>(clientRect.right);
    state_.leftEdge = state_.rightEdge - static_cast<float>(state_.physicalWidth);
    state_.topEdge = static_cast<float>(tbRect.bottom);
    state_.bottomEdge = static_cast<float>(clientRect.bottom - footerHeight);

    if (!visible_)
    {
        state_.physicalWidth = 0;
        state_.leftEdge = state_.rightEdge;
        return;
    }

    const float left = state_.leftEdge + state_.leftPadding;
    const float right = state_.rightEdge - state_.leftPadding;
    float y = state_.topEdge + state_.titleHeight + 6.0f;

    headerRect_ = D2D1::RectF(0, 0, 0, 0);
    detectButtonRect_ = D2D1::RectF(0, 0, 0, 0);
    loginButtonRect_ = D2D1::RectF(0, 0, 0, 0);
    pathButtonRect_ = D2D1::RectF(0, 0, 0, 0);
    detectButtonHovered_ = false;
    loginButtonHovered_ = false;
    pathButtonHovered_ = false;

    if (ShouldShowExecutableInput())
    {
        executableInput_.SetRect(D2D1::RectF(left, y, right, y + 30.0f));
        y += 34.0f;
    }
    else
    {
        executableInput_.SetRect(D2D1::RectF(0, 0, 0, 0));
        executableInput_.SetFocused(false);
    }

    const float toolbarHeight = 24.0f;
    const float toolbarButtonSize = 22.0f;
    toolbarRect_ = D2D1::RectF(left, y, right, y + toolbarHeight);
    usageButtonRect_ = D2D1::RectF(right - toolbarButtonSize, y + 1.0f, right, y + 1.0f + toolbarButtonSize);
    historyButtonRect_ = D2D1::RectF(usageButtonRect_.left - 6.0f - toolbarButtonSize, y + 1.0f,
                                     usageButtonRect_.left - 6.0f, y + 1.0f + toolbarButtonSize);
    y += toolbarHeight + 6.0f;

    const float composerHeight = 54.0f;
    const float composerBottom = state_.bottomEdge - 6.0f;
    const float composerTop = composerBottom - composerHeight;
    const float sendButtonSize = 22.0f;
    const float sendInset = 6.0f;
    const float sendGap = 4.0f;

    promptInput_.SetRect(D2D1::RectF(left, composerTop, right, composerBottom));
    auto &promptStyle = promptInput_.GetStyle();
    promptStyle.paddingRight = sendInset + sendButtonSize + sendGap;
    sendButtonRect_ = D2D1::RectF(right - sendInset - sendButtonSize,
                                  composerBottom - sendInset - sendButtonSize,
                                  right - sendInset,
                                  composerBottom - sendInset);
    messagesRect_ = D2D1::RectF(left, y, right, composerTop - 6.0f);
    historyRowRects_.assign(historyEntries_.size(), D2D1::RectF(0, 0, 0, 0));
    usageLinkRect_ = D2D1::RectF(0, 0, 0, 0);

    RecomputeMessageHeights();
    messagesScrollbar_.UpdateLayout(messagesRect_.left, messagesRect_.top,
                                    messagesRect_.right - messagesRect_.left,
                                    (std::max)(0.0f, messagesRect_.bottom - messagesRect_.top),
                                    ComputeScrollContentHeight());
}

void ClaudePanel::Draw(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd)
{
    hwnd_ = hwnd;
    if (!visible_ || state_.physicalWidth <= 0)
        return;

    ApplyInputTheme(executableInput_, L"Auto-detect claude.cmd or claude.exe", false);
    ApplyInputTheme(promptInput_, L"Ask Claude about the current file, selection, or project...", true);
    RefreshConversationHistory(false);
    if (sendButtonRect_.right > sendButtonRect_.left)
    {
        auto &promptStyle = promptInput_.GetStyle();
        promptStyle.paddingRight = (sendButtonRect_.right - sendButtonRect_.left) + 18.0f;
    }

    RefreshMarkdownPreviews(dwrite);
    messagesScrollbar_.UpdateLayout(messagesRect_.left, messagesRect_.top,
                                    messagesRect_.right - messagesRect_.left,
                                    (std::max)(0.0f, messagesRect_.bottom - messagesRect_.top),
                                    ComputeScrollContentHeight());

    D2D1_RECT_F clipRect = D2D1::RectF(state_.leftEdge, state_.topEdge, state_.rightEdge, state_.bottomEdge);
    ctx->PushAxisAlignedClip(clipRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    DrawBackground(ctx);
    DrawTitle(ctx, dwrite);

    if (ShouldShowExecutableInput())
        executableInput_.Draw(ctx, dwrite);

    DrawToolbar(ctx, dwrite);
    DrawMessages(ctx, dwrite);
    DrawComposer(ctx, dwrite);

    ctx->PopAxisAlignedClip();
    DrawRightBorder(ctx);

    if (initialAuthRefreshPending_)
    {
        initialAuthRefreshPending_ = false;
        RefreshAuthStatus();
    }
}

void ClaudePanel::DrawHeader(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite)
{
    ID2D1SolidColorBrush *lineBrush = nullptr;
    ID2D1SolidColorBrush *textBrush = nullptr;
    ID2D1SolidColorBrush *mutedBrush = nullptr;
    ID2D1SolidColorBrush *dotBrush = nullptr;
    ctx->CreateSolidColorBrush(UI::Theme::ChromeBorder(), &lineBrush);
    ctx->CreateSolidColorBrush(UI::Theme::PrimaryText(), &textBrush);
    ctx->CreateSolidColorBrush(UI::Theme::MutedText(), &mutedBrush);

    D2D1_COLOR_F dot = UI::Theme::MutedText();
    std::wstring statusText;
    switch (bridge_.GetAuthState())
    {
    case ClaudeCliBridge::AuthState::Ready:
        dot = D2D1::ColorF(0.24f, 0.73f, 0.45f, 1.0f);
        statusText = L"Connected";
        break;
    case ClaudeCliBridge::AuthState::NeedsLogin:
        dot = D2D1::ColorF(0.92f, 0.69f, 0.18f, 1.0f);
        statusText = L"Login required";
        break;
    case ClaudeCliBridge::AuthState::Checking:
        dot = UI::Theme::Accent();
        statusText = L"Checking";
        break;
    case ClaudeCliBridge::AuthState::Error:
        dot = D2D1::ColorF(0.84f, 0.32f, 0.30f, 1.0f);
        statusText = L"Unavailable";
        break;
    default:
        dot = UI::Theme::MutedText();
        statusText = L"Not configured";
        break;
    }
    ctx->CreateSolidColorBrush(dot, &dotBrush);

    IDWriteTextFormat *labelFormat = nullptr;
    IDWriteTextFormat *statusFormat = nullptr;
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             12.5f, L"en-us", &labelFormat);
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             11.0f, L"en-us", &statusFormat);

    if (lineBrush)
    {
        ctx->DrawLine(D2D1::Point2F(headerRect_.left, headerRect_.bottom + 3.0f),
                      D2D1::Point2F(headerRect_.right, headerRect_.bottom + 3.0f),
                      lineBrush, 0.8f);
    }

    if (dotBrush)
        ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(headerRect_.left + 5.0f, headerRect_.top + 14.0f), 3.5f, 3.5f), dotBrush);

    if (labelFormat && textBrush)
        ctx->DrawTextW(L"Claude", 6, labelFormat,
                       D2D1::RectF(headerRect_.left + 14.0f, headerRect_.top,
                                   detectButtonRect_.left - 12.0f, headerRect_.top + 18.0f),
                       textBrush);
    if (statusFormat && mutedBrush)
        ctx->DrawTextW(statusText.c_str(), (UINT32)statusText.size(), statusFormat,
                       D2D1::RectF(headerRect_.left + 14.0f, headerRect_.top + 14.0f,
                                   detectButtonRect_.left - 12.0f, headerRect_.bottom),
                       mutedBrush);

    const bool hasExe = !Trim(executableInput_.GetText()).empty();
    const bool ready = bridge_.GetAuthState() == ClaudeCliBridge::AuthState::Ready;
    const std::wstring actionLabel = !hasExe ? L"Detect"
                                     : ready ? L"Refresh"
                                     : bridge_.GetAuthState() == ClaudeCliBridge::AuthState::NeedsLogin ? L"Login"
                                     : L"Refresh";
    DrawButton(ctx, dwrite, detectButtonRect_, actionLabel, detectButtonHovered_, false, true);
    DrawButton(ctx, dwrite, loginButtonRect_, L"Path", loginButtonHovered_, false, true);

    const bool pathOpen = ShouldShowExecutableInput();
    DrawButton(ctx, dwrite, pathButtonRect_, pathOpen ? L"Hide" : L"Path", pathButtonHovered_, false, true);

    if (statusFormat)
        statusFormat->Release();
    if (labelFormat)
        labelFormat->Release();
    if (dotBrush)
        dotBrush->Release();
    if (mutedBrush)
        mutedBrush->Release();
    if (textBrush)
        textBrush->Release();
    if (lineBrush)
        lineBrush->Release();
}

void ClaudePanel::DrawToolbar(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite)
{
    if (toolbarRect_.right <= toolbarRect_.left)
        return;

    ID2D1SolidColorBrush *mutedBrush = nullptr;
    ID2D1SolidColorBrush *textBrush = nullptr;
    ID2D1SolidColorBrush *lineBrush = nullptr;
    ctx->CreateSolidColorBrush(UI::Theme::MutedText(), &mutedBrush);
    ctx->CreateSolidColorBrush(UI::Theme::PrimaryText(), &textBrush);
    ctx->CreateSolidColorBrush(UI::Theme::ChromeBorder(), &lineBrush);

    IDWriteTextFormat *titleFormat = nullptr;
    IDWriteTextFormat *metaFormat = nullptr;
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             11.5f, L"en-us", &titleFormat);
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             9.5f, L"en-us", &metaFormat);

    const int totalMessages = currentConversationUsage_.userMessages + currentConversationUsage_.assistantMessages;
    std::wstring title = TruncateText(CurrentConversationTitle(), 38);
    std::wstring meta = sessionId_.empty() ? L"Current session"
                                           : (currentConversationUsage_.lastUpdated.empty()
                                                  ? sessionId_
                                                  : currentConversationUsage_.lastUpdated);
    if (totalMessages > 0 || currentConversationUsage_.toolCalls > 0)
    {
        meta += L"  •  ";
        meta += std::to_wstring(totalMessages) + L" msg";
        if (currentConversationUsage_.toolCalls > 0)
            meta += L"  •  " + std::to_wstring(currentConversationUsage_.toolCalls) + L" tools";
    }
    else if (meta == L"Current session" && !Trim(authDetail_).empty())
    {
        meta = TruncateText(authDetail_, 42);
    }

    if (titleFormat && textBrush)
        ctx->DrawTextW(title.c_str(), (UINT32)title.size(), titleFormat,
                       D2D1::RectF(toolbarRect_.left, toolbarRect_.top,
                                   historyButtonRect_.left - 10.0f, toolbarRect_.top + 14.0f),
                       textBrush);
    if (metaFormat && mutedBrush)
        ctx->DrawTextW(meta.c_str(), (UINT32)meta.size(), metaFormat,
                       D2D1::RectF(toolbarRect_.left, toolbarRect_.top + 11.0f,
                                   historyButtonRect_.left - 12.0f, toolbarRect_.bottom),
                       mutedBrush);
    if (lineBrush)
        ctx->DrawLine(D2D1::Point2F(toolbarRect_.left, toolbarRect_.bottom + 2.0f),
                      D2D1::Point2F(toolbarRect_.right, toolbarRect_.bottom + 2.0f),
                      lineBrush, 0.8f);

    DrawButton(ctx, dwrite, historyButtonRect_, L"\uE81C", historyButtonHovered_, showHistory_, true);
    DrawButton(ctx, dwrite, usageButtonRect_, L"\uE9D2", usageButtonHovered_, showUsageOverlay_, true);

    if (metaFormat)
        metaFormat->Release();
    if (titleFormat)
        titleFormat->Release();
    if (lineBrush)
        lineBrush->Release();
    if (textBrush)
        textBrush->Release();
    if (mutedBrush)
        mutedBrush->Release();
}

void ClaudePanel::DrawHistoryList(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite)
{
    ID2D1SolidColorBrush *cardBrush = nullptr;
    ID2D1SolidColorBrush *borderBrush = nullptr;
    ID2D1SolidColorBrush *textBrush = nullptr;
    ID2D1SolidColorBrush *mutedBrush = nullptr;
    ID2D1SolidColorBrush *accentBrush = nullptr;
    const auto &palette = UI::Theme::GetPalette();
    ctx->CreateSolidColorBrush(palette.inputBackground, &cardBrush);
    ctx->CreateSolidColorBrush(UI::Theme::ChromeBorder(), &borderBrush);
    ctx->CreateSolidColorBrush(UI::Theme::PrimaryText(), &textBrush);
    ctx->CreateSolidColorBrush(UI::Theme::MutedText(), &mutedBrush);
    ctx->CreateSolidColorBrush(UI::Theme::Accent(), &accentBrush);

    IDWriteTextFormat *titleFormat = nullptr;
    IDWriteTextFormat *metaFormat = nullptr;
    IDWriteTextFormat *emptyFormat = nullptr;
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             12.0f, L"en-us", &titleFormat);
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             10.5f, L"en-us", &metaFormat);
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             11.0f, L"en-us", &emptyFormat);
    for (IDWriteTextFormat *format : {titleFormat, metaFormat, emptyFormat})
    {
        if (!format)
            continue;
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }

    historyRowRects_.assign(historyEntries_.size(), D2D1::RectF(0, 0, 0, 0));

    ctx->PushAxisAlignedClip(messagesRect_, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    float y = messagesRect_.top + 6.0f - messagesScrollbar_.GetScrollOffset();

    if (historyEntries_.empty())
    {
        if (emptyFormat && mutedBrush)
            ctx->DrawTextW(L"No conversation history found for this project.", 45, emptyFormat,
                           D2D1::RectF(messagesRect_.left + 4.0f, y + 8.0f, messagesRect_.right - 8.0f, y + 56.0f),
                           mutedBrush);
        ctx->PopAxisAlignedClip();
    }
    else
    {
        for (size_t i = 0; i < historyEntries_.size(); ++i)
        {
            const ConversationHistoryEntry &entry = historyEntries_[i];
            D2D1_RECT_F rowRect = D2D1::RectF(messagesRect_.left + 2.0f, y, messagesRect_.right - 12.0f, y + 56.0f);
            historyRowRects_[i] = rowRect;

            if (rowRect.bottom >= messagesRect_.top && rowRect.top <= messagesRect_.bottom)
            {
                D2D1_COLOR_F fillColor = palette.inputBackground;
                if ((int)i == selectedHistoryIndex_)
                    fillColor = palette.explorerRowActive;
                else if ((int)i == hoveredHistoryIndex_)
                    fillColor = palette.explorerRowHover;

                ID2D1SolidColorBrush *rowBrush = nullptr;
                ctx->CreateSolidColorBrush(fillColor, &rowBrush);
                if (rowBrush)
                {
                    ctx->FillRoundedRectangle(D2D1::RoundedRect(rowRect, 8.0f, 8.0f), rowBrush);
                    rowBrush->Release();
                }
                if (borderBrush)
                    ctx->DrawRoundedRectangle(D2D1::RoundedRect(rowRect, 8.0f, 8.0f), borderBrush, 1.0f);

                const int totalMessages = entry.usage.userMessages + entry.usage.assistantMessages;
                std::wstring stats = entry.usage.lastUpdated.empty() ? entry.sessionId : entry.usage.lastUpdated;
                if (totalMessages > 0 || entry.usage.toolCalls > 0)
                {
                    stats += L"  •  ";
                    stats += std::to_wstring(totalMessages) + L" msg";
                    if (entry.usage.toolCalls > 0)
                        stats += L"  •  " + std::to_wstring(entry.usage.toolCalls) + L" tools";
                }

                if (titleFormat && textBrush)
                    ctx->DrawTextW(entry.title.c_str(), (UINT32)entry.title.size(), titleFormat,
                                   D2D1::RectF(rowRect.left + 12.0f, rowRect.top + 7.0f, rowRect.right - 12.0f, rowRect.top + 23.0f),
                                   textBrush);
                if (metaFormat && mutedBrush)
                    ctx->DrawTextW(entry.subtitle.c_str(), (UINT32)entry.subtitle.size(), metaFormat,
                                   D2D1::RectF(rowRect.left + 12.0f, rowRect.top + 25.0f, rowRect.right - 12.0f, rowRect.top + 39.0f),
                                   mutedBrush);
                if (metaFormat && mutedBrush)
                    ctx->DrawTextW(stats.c_str(), (UINT32)stats.size(), metaFormat,
                                   D2D1::RectF(rowRect.left + 12.0f, rowRect.bottom - 19.0f, rowRect.right - 12.0f, rowRect.bottom - 4.0f),
                                   mutedBrush);
            }

            y += 64.0f;
        }
        ctx->PopAxisAlignedClip();
    }

    messagesScrollbar_.Draw(ctx);

    if (emptyFormat)
        emptyFormat->Release();
    if (metaFormat)
        metaFormat->Release();
    if (titleFormat)
        titleFormat->Release();
    if (accentBrush)
        accentBrush->Release();
    if (mutedBrush)
        mutedBrush->Release();
    if (textBrush)
        textBrush->Release();
    if (borderBrush)
        borderBrush->Release();
    if (cardBrush)
        cardBrush->Release();
}

void ClaudePanel::DrawUsageOverlay(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite)
{
    const float availableWidth = (std::max)(280.0f, messagesRect_.right - messagesRect_.left);
    const float availableHeight = (std::max)(220.0f, messagesRect_.bottom - messagesRect_.top);
    const float cardWidth = (std::min)(360.0f, availableWidth - 20.0f);
    const float cardHeight = (std::min)(292.0f, availableHeight - 18.0f);
    const float cardLeft = (availableWidth >= 340.0f)
                               ? (messagesRect_.right - cardWidth - 8.0f)
                               : (messagesRect_.left + ((availableWidth - cardWidth) * 0.5f));
    const float cardTop = messagesRect_.top + 8.0f;
    const D2D1_RECT_F shadowRect = D2D1::RectF(
        cardLeft + 2.0f,
        cardTop + 5.0f,
        cardLeft + cardWidth + 2.0f,
        cardTop + cardHeight + 5.0f);
    const D2D1_RECT_F cardRect = D2D1::RectF(
        cardLeft,
        cardTop,
        cardLeft + cardWidth,
        cardTop + cardHeight);
    usageOverlayRect_ = cardRect;

    const auto &palette = UI::Theme::GetPalette();
    const D2D1_COLOR_F accent = UI::Theme::Accent();
    ID2D1SolidColorBrush *bgBrush = nullptr;
    ID2D1SolidColorBrush *borderBrush = nullptr;
    ID2D1SolidColorBrush *titleBrush = nullptr;
    ID2D1SolidColorBrush *mutedBrush = nullptr;
    ID2D1SolidColorBrush *accentBrush = nullptr;
    ID2D1SolidColorBrush *barBgBrush = nullptr;
    ID2D1SolidColorBrush *shadowBrush = nullptr;
    ID2D1SolidColorBrush *closeBrush = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f, 0.10f), &shadowBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(palette.inputBackground.r, palette.inputBackground.g, palette.inputBackground.b, 0.985f), &bgBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(palette.inputBorder.r, palette.inputBorder.g, palette.inputBorder.b, 0.95f), &borderBrush);
    ctx->CreateSolidColorBrush(UI::Theme::PrimaryText(), &titleBrush);
    ctx->CreateSolidColorBrush(UI::Theme::MutedText(), &mutedBrush);
    ctx->CreateSolidColorBrush(accent, &accentBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.14f), &barBgBrush);
    ctx->CreateSolidColorBrush(usageCloseButtonHovered_ ? UI::Theme::PrimaryText() : UI::Theme::MutedText(), &closeBrush);

    IDWriteTextFormat *titleFormat = nullptr;
    IDWriteTextFormat *bodyFormat = nullptr;
    IDWriteTextFormat *labelFormat = nullptr;
    IDWriteTextFormat *valueFormat = nullptr;
    IDWriteTextFormat *trailingValueFormat = nullptr;
    IDWriteTextFormat *percentFormat = nullptr;
    IDWriteTextFormat *closeFormat = nullptr;
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             13.5f, L"en-us", &titleFormat);
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             10.5f, L"en-us", &bodyFormat);
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             10.0f, L"en-us", &labelFormat);
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             10.5f, L"en-us", &valueFormat);
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             10.5f, L"en-us", &trailingValueFormat);
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             11.0f, L"en-us", &percentFormat);
    dwrite->CreateTextFormat(L"Segoe Fluent Icons", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             12.0f, L"en-us", &closeFormat);

    if (valueFormat)
        valueFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (trailingValueFormat)
    {
        trailingValueFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        trailingValueFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    if (percentFormat)
    {
        percentFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        percentFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    if (shadowBrush)
        ctx->FillRoundedRectangle(D2D1::RoundedRect(shadowRect, 9.0f, 9.0f), shadowBrush);
    if (bgBrush)
        ctx->FillRoundedRectangle(D2D1::RoundedRect(cardRect, 8.0f, 8.0f), bgBrush);
    if (borderBrush)
        ctx->DrawRoundedRectangle(D2D1::RoundedRect(cardRect, 8.0f, 8.0f), borderBrush, 1.0f);

    const float left = cardRect.left + 14.0f;
    const float right = cardRect.right - 14.0f;
    const float rowSplit = left + 108.0f;
    float y = cardRect.top + 14.0f;
    usageCloseButtonRect_ = D2D1::RectF(cardRect.right - 28.0f, cardRect.top + 10.0f, cardRect.right - 10.0f, cardRect.top + 26.0f);

    if (titleFormat && titleBrush)
        ctx->DrawTextW(L"Account & Usage", 15, titleFormat,
                       D2D1::RectF(left, y, right - 28.0f, y + 20.0f), titleBrush);
    if (closeFormat && closeBrush)
        ctx->DrawTextW(L"\uE711", 1, closeFormat, usageCloseButtonRect_, closeBrush);
    y += 34.0f;

    auto drawSectionLabel = [&](const wchar_t *label) {
        if (labelFormat && mutedBrush)
            ctx->DrawTextW(label, (UINT32)wcslen(label), labelFormat,
                           D2D1::RectF(left, y, right, y + 16.0f), mutedBrush);
        y += 18.0f;
    };

    auto drawRow = [&](const wchar_t *label, const std::wstring &value) {
        const std::wstring clippedValue = TruncateText(value, 40);
        if (bodyFormat && mutedBrush)
            ctx->DrawTextW(label, (UINT32)wcslen(label), bodyFormat,
                           D2D1::RectF(left, y, rowSplit - 12.0f, y + 18.0f), mutedBrush);
        if (trailingValueFormat && titleBrush)
            ctx->DrawTextW(clippedValue.c_str(), (UINT32)clippedValue.size(), trailingValueFormat,
                           D2D1::RectF(rowSplit, y, right, y + 18.0f), titleBrush);
        y += 22.0f;
    };

    drawSectionLabel(L"ACCOUNT");
    drawRow(L"Auth method", FormatAccountValue(accountInfo_.authMethod, L"Unavailable", true));
    drawRow(L"Email", accountInfo_.email.empty() ? L"Unavailable" : accountInfo_.email);
    drawRow(L"Organization", accountInfo_.orgName.empty() ? L"Unavailable" : accountInfo_.orgName);
    drawRow(L"Plan", FormatAccountValue(accountInfo_.subscriptionType, L"Unavailable"));

    y += 4.0f;
    drawSectionLabel(L"USAGE");

    const int totalMessages = currentConversationUsage_.userMessages + currentConversationUsage_.assistantMessages;
    const int totalTokens = currentConversationUsage_.inputTokens + currentConversationUsage_.outputTokens;
    const std::wstring sessionStats = std::to_wstring(totalMessages) + L" msg  " +
                                      std::to_wstring(currentConversationUsage_.toolCalls) + L" tools  " +
                                      std::to_wstring(totalTokens) + L" tokens";

    auto drawUsageBar = [&](const std::wstring &label, const ClaudeCliBridge::RateLimitWindow &window,
                            const std::wstring &fallbackResetText) {
        std::wstring percent = FormatRateLimitStatusCompact(window);
        std::wstring resetText = FormatRateLimitResetLabel(window.resetsAtUnix);
        if (resetText.empty())
            resetText = fallbackResetText;
        if (resetText.empty())
            resetText = window.available ? L"Waiting for Claude usage data" : L"No live data yet";
        if (window.usedPercentage < 0 && window.available)
            resetText += L"  •  Percent tracked on claude.ai";

        const float top = y;
        if (bodyFormat && titleBrush)
            ctx->DrawTextW(label.c_str(), (UINT32)label.size(), bodyFormat,
                           D2D1::RectF(left, top, right - 64.0f, top + 18.0f), titleBrush);
        if (percentFormat && titleBrush)
            ctx->DrawTextW(percent.c_str(), (UINT32)percent.size(), percentFormat,
                           D2D1::RectF(right - 64.0f, top, right, top + 18.0f), titleBrush);

        const float progressTop = top + 21.0f;
        const D2D1_RECT_F barRect = D2D1::RectF(left, progressTop, right, progressTop + 7.0f);
        if (barBgBrush)
            ctx->FillRoundedRectangle(D2D1::RoundedRect(barRect, 3.5f, 3.5f), barBgBrush);

        const float fillRatio = window.usedPercentage < 0 ? 0.0f : ((std::max)(0, (std::min)(100, window.usedPercentage)) / 100.0f);
        const float fillWidth = (barRect.right - barRect.left) * fillRatio;
        if (accentBrush && fillWidth > 0.0f)
            ctx->FillRoundedRectangle(D2D1::RoundedRect(
                D2D1::RectF(barRect.left, barRect.top, barRect.left + fillWidth, barRect.bottom), 3.5f, 3.5f), accentBrush);

        if (bodyFormat && mutedBrush)
            ctx->DrawTextW(resetText.c_str(), (UINT32)resetText.size(), bodyFormat,
                           D2D1::RectF(left, progressTop + 11.0f, right, progressTop + 28.0f), mutedBrush);
        y += 50.0f;
    };

    const std::wstring sessionResetFallback = !rateLimits_.fiveHour.available ? lastKnownRateLimitResetText_ : std::wstring();
    drawUsageBar(L"Session (5hr)", rateLimits_.fiveHour, sessionResetFallback);
    drawUsageBar(L"Weekly (7 day)", rateLimits_.sevenDay, L"Reported by Claude Code when available");

    const float footerTop = cardRect.bottom - 46.0f;
    if (bodyFormat && mutedBrush)
        ctx->DrawTextW(sessionStats.c_str(), (UINT32)sessionStats.size(), bodyFormat,
                       D2D1::RectF(left, footerTop, right, footerTop + 16.0f), mutedBrush);

    usageLinkRect_ = D2D1::RectF(left, cardRect.bottom - 24.0f, left + 176.0f, cardRect.bottom - 8.0f);
    if (bodyFormat && (usageLinkHovered_ ? accentBrush : mutedBrush))
        ctx->DrawTextW(L"Manage usage on claude.ai", 25, bodyFormat, usageLinkRect_,
                       usageLinkHovered_ ? accentBrush : mutedBrush);

    if (closeFormat)
        closeFormat->Release();
    if (percentFormat)
        percentFormat->Release();
    if (trailingValueFormat)
        trailingValueFormat->Release();
    if (valueFormat)
        valueFormat->Release();
    if (labelFormat)
        labelFormat->Release();
    if (bodyFormat)
        bodyFormat->Release();
    if (titleFormat)
        titleFormat->Release();
    if (closeBrush)
        closeBrush->Release();
    if (shadowBrush)
        shadowBrush->Release();
    if (barBgBrush)
        barBgBrush->Release();
    if (accentBrush)
        accentBrush->Release();
    if (mutedBrush)
        mutedBrush->Release();
    if (titleBrush)
        titleBrush->Release();
    if (borderBrush)
        borderBrush->Release();
    if (bgBrush)
        bgBrush->Release();
}

void ClaudePanel::DrawMessages(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite)
{
    if (showHistory_)
    {
        DrawHistoryList(ctx, dwrite);
        if (showUsageOverlay_)
            DrawUsageOverlay(ctx, dwrite);
        return;
    }

    IDWriteTextFormat *bodyFormat = nullptr;
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             11.5f, L"en-us", &bodyFormat);

    if (bodyFormat)
    {
        bodyFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        bodyFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }

    ID2D1SolidColorBrush *userBrush = nullptr;
    ID2D1SolidColorBrush *assistantTextBrush = nullptr;
    ID2D1SolidColorBrush *systemTextBrush = nullptr;
    ID2D1SolidColorBrush *errorTextBrush = nullptr;
    ID2D1SolidColorBrush *userTextBrush = nullptr;
    ID2D1SolidColorBrush *mutedBrush = nullptr;
    ID2D1SolidColorBrush *toolCardBrush = nullptr;
    ID2D1SolidColorBrush *toolBorderBrush = nullptr;
    ID2D1SolidColorBrush *toolTitleBrush = nullptr;
    ID2D1SolidColorBrush *toolBodyBrush = nullptr;
    ID2D1SolidColorBrush *toolStatusBrush = nullptr;
    const auto &palette = UI::Theme::GetPalette();
    ctx->CreateSolidColorBrush(palette.explorerRowActive, &userBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.98f), &userTextBrush);
    ctx->CreateSolidColorBrush(UI::Theme::PrimaryText(), &assistantTextBrush);
    ctx->CreateSolidColorBrush(UI::Theme::MutedText(), &systemTextBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.92f, 0.42f, 0.40f, 1.0f), &errorTextBrush);
    ctx->CreateSolidColorBrush(UI::Theme::MutedText(), &mutedBrush);
    ctx->CreateSolidColorBrush(palette.inputBackground, &toolCardBrush);
    ctx->CreateSolidColorBrush(UI::Theme::ChromeBorder(), &toolBorderBrush);
    ctx->CreateSolidColorBrush(UI::Theme::MutedText(), &toolTitleBrush);
    ctx->CreateSolidColorBrush(UI::Theme::PrimaryText(), &toolBodyBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.60f, 0.82f, 0.62f, 1.0f), &toolStatusBrush);

    IDWriteTextFormat *toolTitleFormat = nullptr;
    IDWriteTextFormat *toolBodyFormat = nullptr;
    IDWriteTextFormat *toolStatusFormat = nullptr;
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             10.5f, L"en-us", &toolTitleFormat);
    dwrite->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             11.0f, L"en-us", &toolBodyFormat);
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             10.0f, L"en-us", &toolStatusFormat);
    for (IDWriteTextFormat *format : {toolTitleFormat, toolBodyFormat, toolStatusFormat})
    {
        if (!format)
            continue;
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }

    ctx->PushAxisAlignedClip(messagesRect_, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    float y = messagesRect_.top + 2.0f - messagesScrollbar_.GetScrollOffset();
    const float contentLeft = messagesRect_.left;
    const float contentRight = messagesRect_.right - 10.0f;
    const float userMaxWidth = (std::max)(170.0f, (messagesRect_.right - messagesRect_.left) * 0.64f);
    const float assistantMaxWidth = (std::max)(220.0f, (messagesRect_.right - messagesRect_.left) * 0.86f);

    for (size_t i = 0; i < messages_.size(); ++i)
    {
        const ChatMessage &message = messages_[i];
        const float height = message.estimatedHeight;
        D2D1_RECT_F bubbleRect = D2D1::RectF(contentLeft, y, contentRight, y + height);

        if (message.role == MessageRole::User)
        {
            bubbleRect.left = (std::max)(contentLeft + 16.0f, contentRight - userMaxWidth);
        }
        else if (message.kind != MessageKind::Tool)
        {
            bubbleRect.right = (std::min)(contentRight, contentLeft + assistantMaxWidth);
        }

        if (bubbleRect.bottom < messagesRect_.top)
        {
            y += height + 4.0f;
            continue;
        }
        if (bubbleRect.top > messagesRect_.bottom)
            break;

        const std::wstring content = GetDisplayMessageText(i);

        if (message.kind == MessageKind::Tool)
        {
            D2D1_RECT_F cardRect = D2D1::RectF(contentLeft, y, contentRight, y + height);
            if (toolCardBrush)
                ctx->FillRoundedRectangle(D2D1::RoundedRect(cardRect, 8.0f, 8.0f), toolCardBrush);
            if (toolBorderBrush)
                ctx->DrawRoundedRectangle(D2D1::RoundedRect(cardRect, 8.0f, 8.0f), toolBorderBrush, 1.0f);

            D2D1_RECT_F titleRect = D2D1::RectF(cardRect.left + 12.0f, cardRect.top + 8.0f,
                                                cardRect.right - 72.0f, cardRect.top + 24.0f);
            D2D1_RECT_F statusRect = D2D1::RectF(cardRect.right - 64.0f, cardRect.top + 8.0f,
                                                 cardRect.right - 12.0f, cardRect.top + 24.0f);
            D2D1_RECT_F bodyRect = D2D1::RectF(cardRect.left + 12.0f, cardRect.top + 26.0f,
                                               cardRect.right - 12.0f, cardRect.bottom - 10.0f);

            if (toolTitleFormat && toolTitleBrush)
                ctx->DrawTextW(message.title.c_str(), (UINT32)message.title.size(), toolTitleFormat, titleRect, toolTitleBrush);
            if (toolStatusFormat && toolStatusBrush && !message.subtitle.empty())
                ctx->DrawTextW(message.subtitle.c_str(), (UINT32)message.subtitle.size(), toolStatusFormat, statusRect, toolStatusBrush);
            if (toolBodyFormat && toolBodyBrush && !content.empty())
                ctx->DrawTextW(content.c_str(), (UINT32)content.size(), toolBodyFormat, bodyRect, toolBodyBrush);

            y += height + 4.0f;
            continue;
        }

        if (message.role == MessageRole::User && userBrush)
        {
            ctx->FillRoundedRectangle(D2D1::RoundedRect(bubbleRect, 6.0f, 6.0f), userBrush);
        }

        if (ShouldUseMarkdownPreview(message, content) &&
            i < markdownPreviewCache_.size() &&
            markdownPreviewCache_[i].editor)
        {
            const float previewInset = message.role == MessageRole::User ? 8.0f : 2.0f;
            markdownPreviewCache_[i].editor->UpdateLayout(
                hwnd_,
                bubbleRect.left + previewInset,
                bubbleRect.top + 2.0f,
                bubbleRect.right - previewInset,
                bubbleRect.bottom - 2.0f);
            markdownPreviewCache_[i].editor->Draw(ctx, dwrite);

            y += height + 4.0f;
            continue;
        }

        D2D1_RECT_F bodyRect = D2D1::RectF(bubbleRect.left + 8.0f, bubbleRect.top + 4.0f,
                                           bubbleRect.right - 8.0f, bubbleRect.bottom - 4.0f);
        if (message.role != MessageRole::User)
            bodyRect.left = bubbleRect.left + 1.0f;

        ID2D1SolidColorBrush *bodyBrush = assistantTextBrush;
        if (message.role == MessageRole::User)
            bodyBrush = userTextBrush;
        else if (message.role == MessageRole::System)
            bodyBrush = systemTextBrush ? systemTextBrush : mutedBrush;
        else if (message.role == MessageRole::Error)
            bodyBrush = errorTextBrush;

        if (bodyFormat && bodyBrush)
            ctx->DrawTextW(content.c_str(), (UINT32)content.size(), bodyFormat, bodyRect, bodyBrush);

        y += height + 4.0f;
    }

    ctx->PopAxisAlignedClip();
    messagesScrollbar_.Draw(ctx);
    if (showUsageOverlay_)
        DrawUsageOverlay(ctx, dwrite);

    if (mutedBrush)
        mutedBrush->Release();
    if (toolStatusBrush)
        toolStatusBrush->Release();
    if (toolBodyBrush)
        toolBodyBrush->Release();
    if (toolTitleBrush)
        toolTitleBrush->Release();
    if (toolBorderBrush)
        toolBorderBrush->Release();
    if (toolCardBrush)
        toolCardBrush->Release();
    if (userTextBrush)
        userTextBrush->Release();
    if (errorTextBrush)
        errorTextBrush->Release();
    if (systemTextBrush)
        systemTextBrush->Release();
    if (assistantTextBrush)
        assistantTextBrush->Release();
    if (userBrush)
        userBrush->Release();
    if (bodyFormat)
        bodyFormat->Release();
    if (toolStatusFormat)
        toolStatusFormat->Release();
    if (toolBodyFormat)
        toolBodyFormat->Release();
    if (toolTitleFormat)
        toolTitleFormat->Release();
}

void ClaudePanel::DrawComposer(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite)
{
    promptInput_.Draw(ctx, dwrite);
    DrawButton(ctx, dwrite, sendButtonRect_, L"\uE724",
               sendButtonHovered_, true, CanSubmitPrompt());
}

void ClaudePanel::DrawButton(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite,
                             const D2D1_RECT_F &rect, const std::wstring &label,
                             bool hovered, bool accent, bool enabled)
{
    D2D1_COLOR_F fill = accent ? UI::Theme::Accent() : UI::Theme::GetPalette().explorerRowHover;
    if (!enabled)
        fill = accent ? UI::Theme::GetPalette().inputBorder : UI::Theme::GetPalette().explorerRowHover;
    else if (hovered)
        fill.a = (std::min)(1.0f, fill.a + 0.12f);

    ID2D1SolidColorBrush *fillBrush = nullptr;
    ID2D1SolidColorBrush *textBrush = nullptr;
    ctx->CreateSolidColorBrush(fill, &fillBrush);
    D2D1_COLOR_F labelColor = accent ? D2D1::ColorF(1.f, 1.f, 1.f, enabled ? 1.0f : 0.55f)
                                     : D2D1::ColorF(UI::Theme::PrimaryText().r, UI::Theme::PrimaryText().g, UI::Theme::PrimaryText().b, enabled ? 1.0f : 0.6f);
    ctx->CreateSolidColorBrush(labelColor, &textBrush);

    IDWriteTextFormat *format = nullptr;
    const bool iconOnly = label.size() == 1;
    const bool fluentIcon = iconOnly && !label.empty() && label[0] >= 0xE700 && label[0] <= 0xF8FF;
    dwrite->CreateTextFormat(fluentIcon ? L"Segoe Fluent Icons" : iconOnly ? L"Segoe UI Symbol" : L"Segoe UI", nullptr,
                             iconOnly ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_SEMI_BOLD,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             iconOnly ? 13.5f : 11.5f, L"en-us", &format);
    if (format)
    {
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    if (fillBrush)
        ctx->FillRoundedRectangle(D2D1::RoundedRect(rect, 6.0f, 6.0f), fillBrush);
    if (format && textBrush)
        ctx->DrawTextW(label.c_str(), (UINT32)label.size(), format, rect, textBrush);

    if (format)
        format->Release();
    if (textBrush)
        textBrush->Release();
    if (fillBrush)
        fillBrush->Release();
}

void ClaudePanel::OnMouseMove(HWND hwnd, POINT clientPoint)
{
    HandleResizeMouseMove(hwnd, clientPoint);
    if (state_.isResizing || state_.isHoveringResizeZone)
        return;

    bool changed = false;
    if (executableInput_.OnMouseMove(hwnd, clientPoint))
        changed = true;
    if (promptInput_.OnMouseMove(hwnd, clientPoint))
        changed = true;
    if (messagesScrollbar_.OnMouseMove(clientPoint))
        changed = true;
    bool historyHovered = IsPointInRect(historyButtonRect_, clientPoint);
    if (historyHovered != historyButtonHovered_)
    {
        historyButtonHovered_ = historyHovered;
        changed = true;
    }
    bool usageHovered = IsPointInRect(usageButtonRect_, clientPoint);
    if (usageHovered != usageButtonHovered_)
    {
        usageButtonHovered_ = usageHovered;
        changed = true;
    }
    bool usageLinkHovered = showUsageOverlay_ && IsPointInRect(usageLinkRect_, clientPoint);
    if (usageLinkHovered != usageLinkHovered_)
    {
        usageLinkHovered_ = usageLinkHovered;
        changed = true;
    }
    bool usageCloseHovered = showUsageOverlay_ && IsPointInRect(usageCloseButtonRect_, clientPoint);
    if (usageCloseHovered != usageCloseButtonHovered_)
    {
        usageCloseButtonHovered_ = usageCloseHovered;
        changed = true;
    }
    int hoveredHistory = -1;
    if (showHistory_)
    {
        for (size_t i = 0; i < historyRowRects_.size(); ++i)
        {
            if (IsPointInRect(historyRowRects_[i], clientPoint))
            {
                hoveredHistory = (int)i;
                break;
            }
        }
    }
    if (hoveredHistory != hoveredHistoryIndex_)
    {
        hoveredHistoryIndex_ = hoveredHistory;
        changed = true;
    }
    bool sendHovered = IsPointInRect(sendButtonRect_, clientPoint);
    if (sendHovered != sendButtonHovered_)
    {
        sendButtonHovered_ = sendHovered;
        changed = true;
    }

    if (changed)
        InvalidateRect(hwnd, nullptr, FALSE);
}

void ClaudePanel::OnLeftButtonDown(HWND hwnd, POINT clientPoint)
{
    if (HandleResizeLeftButtonDown(hwnd, clientPoint))
        return;

    if (messagesScrollbar_.OnLeftButtonDown(clientPoint))
    {
        SetCapture(hwnd);
        return;
    }

    if (IsPointInRect(historyButtonRect_, clientPoint))
    {
        showHistory_ = !showHistory_;
        showUsageOverlay_ = false;
        hoveredHistoryIndex_ = -1;
        if (showHistory_)
            messagesScrollbar_.SetScrollOffset(0.0f);
        messagesScrollbar_.UpdateLayout(messagesRect_.left, messagesRect_.top,
                                        messagesRect_.right - messagesRect_.left,
                                        (std::max)(0.0f, messagesRect_.bottom - messagesRect_.top),
                                        ComputeScrollContentHeight());
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    if (IsPointInRect(usageButtonRect_, clientPoint))
    {
        showUsageOverlay_ = !showUsageOverlay_;
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    if (showUsageOverlay_ && IsPointInRect(usageCloseButtonRect_, clientPoint))
    {
        showUsageOverlay_ = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    if (showUsageOverlay_ && IsPointInRect(usageLinkRect_, clientPoint))
    {
        ShellExecuteW(hwnd, L"open", L"https://claude.ai/settings", nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }

    if (showUsageOverlay_ && !IsPointInRect(usageOverlayRect_, clientPoint))
    {
        showUsageOverlay_ = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    if (showHistory_ && hoveredHistoryIndex_ >= 0)
    {
        if (requestInFlight_ || bridge_.IsBusy())
            return;
        LoadConversationFromHistoryIndex((size_t)hoveredHistoryIndex_);
        messagesScrollbar_.UpdateLayout(messagesRect_.left, messagesRect_.top,
                                        messagesRect_.right - messagesRect_.left,
                                        (std::max)(0.0f, messagesRect_.bottom - messagesRect_.top),
                                        ComputeScrollContentHeight());
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    if (executableInput_.HitTest(clientPoint))
    {
        promptInput_.SetFocused(false);
        executableInput_.OnLeftButtonDown(hwnd, clientPoint);
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    if (IsPointInRect(sendButtonRect_, clientPoint))
    {
        executableInput_.SetFocused(false);
        promptInput_.SetFocused(false);
        if (CanSubmitPrompt())
            SubmitPrompt();
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    if (promptInput_.HitTest(clientPoint))
    {
        executableInput_.SetFocused(false);
        promptInput_.OnLeftButtonDown(hwnd, clientPoint);
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    executableInput_.SetFocused(false);
    promptInput_.SetFocused(false);
}

void ClaudePanel::OnLeftButtonUp(HWND hwnd)
{
    if (HandleResizeLeftButtonUp(hwnd))
        return;

    messagesScrollbar_.OnLeftButtonUp();
    POINT pt{};
    GetCursorPos(&pt);
    ScreenToClient(hwnd, &pt);
    executableInput_.OnLeftButtonUp(hwnd, pt);
    promptInput_.OnLeftButtonUp(hwnd, pt);
}

void ClaudePanel::OnMouseWheel(HWND hwnd, int delta)
{
    if (messagesScrollbar_.OnMouseWheel(delta))
        InvalidateRect(hwnd, nullptr, FALSE);
}

void ClaudePanel::OnChar(wchar_t ch)
{
    if (executableInput_.IsFocused())
    {
        executableInput_.OnChar(ch);
        InvalidatePanel();
        return;
    }

    if (promptInput_.IsFocused())
    {
        promptInput_.OnChar(ch);
        InvalidatePanel();
    }
}

void ClaudePanel::OnKeyDown(WPARAM key)
{
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

    if (key == VK_ESCAPE)
    {
        if (showUsageOverlay_ || showHistory_)
        {
            showUsageOverlay_ = false;
            showHistory_ = false;
            InvalidatePanel();
            return;
        }
    }

    if (key == VK_TAB)
    {
        const bool pathFocused = executableInput_.IsFocused();
        executableInput_.SetFocused(!pathFocused);
        promptInput_.SetFocused(pathFocused);
        InvalidatePanel();
        return;
    }

    if (promptInput_.IsFocused() && ctrl && key == VK_RETURN)
    {
        SubmitPrompt();
        return;
    }

    if (executableInput_.IsFocused())
    {
        executableInput_.OnKeyDown(key);
        InvalidatePanel();
        return;
    }

    if (promptInput_.IsFocused())
    {
        promptInput_.OnKeyDown(key);
        InvalidatePanel();
    }
}
