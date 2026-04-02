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
#include <filesystem>
#include <fstream>
#include <sstream>

// Ont déclare la class
namespace
{
    // Fonction utilitaire pour faire une transition entre deux couleurs a b en fonction de t.
    D2D1_COLOR_F LerpColor(const D2D1_COLOR_F &a, const D2D1_COLOR_F &b, float t)
    {
        t = (std::clamp)(t, 0.0f, 1.0f);
        return D2D1::ColorF(
            a.r + (b.r - a.r) * t,
            a.g + (b.g - a.g) * t,
            a.b + (b.b - a.b) * t,
            a.a + (b.a - a.a) * t);
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

    bridge_.onRequestStarted = [this]() {
        requestInFlight_ = true;
        requestAnimationTick_ = GetTickCount();
        InvalidatePanel();
    };

    bridge_.onTextDelta = [this](const std::wstring &delta) {
        if (streamingMessageIndex_ < 0 || streamingMessageIndex_ >= (int)messages_.size())
            return;

        messages_[(size_t)streamingMessageIndex_].content += delta;
        messages_[(size_t)streamingMessageIndex_].estimatedHeight =
            EstimateMessageHeight(messages_[(size_t)streamingMessageIndex_].content,
                                  MeasureWidthForRole(messages_[(size_t)streamingMessageIndex_].role));
        ScrollMessagesToBottom();
        InvalidatePanel();
    };

    bridge_.onRequestFinished = [this](const std::wstring &sessionId, const std::wstring &fallbackResult) {
        requestInFlight_ = false;
        requestAnimationTick_ = 0;

        if (!sessionId.empty())
            sessionId_ = sessionId;

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
    style.fontSize = UI::InputTheme::kFontSize;
    style.padding = UI::InputTheme::kHorizontalPadding;
    style.paddingLeft = UI::InputTheme::kHorizontalPadding;
    style.paddingRight = UI::InputTheme::kHorizontalPadding;
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
    prompt << L"If you need more repository context, you may use the Read tool only.\n\n";

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
    message.content = text;
    message.estimatedHeight = EstimateMessageHeight(text, MeasureWidthForRole(role));
    messages_.push_back(std::move(message));
    ScrollMessagesToBottom();
}

float ClaudePanel::EstimateMessageHeight(const std::wstring &text, float width) const
{
    const float usableWidth = (std::max)(80.0f, width);
    const float charsPerLine = (std::max)(18.0f, usableWidth / 7.2f);

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

    return 18.0f + (float)lineCount * 17.0f;
}

float ClaudePanel::MeasureWidthForRole(MessageRole role) const
{
    const float baseWidth = (std::max)(80.0f, messagesRect_.right - messagesRect_.left - 28.0f);
    if (role == MessageRole::User)
        return (std::max)(80.0f, baseWidth * 0.74f - 20.0f);
    return baseWidth;
}

float ClaudePanel::ComputeMessagesContentHeight() const
{
    float total = 0.0f;
    for (const ChatMessage &message : messages_)
        total += message.estimatedHeight + 12.0f;
    return total + 16.0f;
}

void ClaudePanel::RecomputeMessageHeights()
{
    for (ChatMessage &message : messages_)
        message.estimatedHeight = EstimateMessageHeight(message.content, MeasureWidthForRole(message.role));
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
    return requestInFlight_;
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
    userMessage.content = rawPrompt;
    userMessage.estimatedHeight = EstimateMessageHeight(rawPrompt, MeasureWidthForRole(userMessage.role));
    messages_.push_back(std::move(userMessage));

    ChatMessage assistantMessage;
    assistantMessage.role = MessageRole::Assistant;
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
    float y = state_.topEdge + state_.titleHeight + 10.0f;

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
        y += 38.0f;
    }
    else
    {
        executableInput_.SetRect(D2D1::RectF(0, 0, 0, 0));
        executableInput_.SetFocused(false);
    }

    const float composerHeight = 94.0f;
    const float composerBottom = state_.bottomEdge - 10.0f;
    const float composerTop = composerBottom - composerHeight;
    const float sendButtonSize = 28.0f;
    const float sendInset = 10.0f;
    const float sendGap = 8.0f;

    promptInput_.SetRect(D2D1::RectF(left, composerTop, right, composerBottom));
    auto &promptStyle = promptInput_.GetStyle();
    promptStyle.paddingRight = sendInset + sendButtonSize + sendGap;
    sendButtonRect_ = D2D1::RectF(right - sendInset - sendButtonSize,
                                  composerBottom - sendInset - sendButtonSize,
                                  right - sendInset,
                                  composerBottom - sendInset);
    messagesRect_ = D2D1::RectF(left, y, right, composerTop - 10.0f);

    RecomputeMessageHeights();
    messagesScrollbar_.UpdateLayout(messagesRect_.left, messagesRect_.top,
                                    messagesRect_.right - messagesRect_.left,
                                    (std::max)(0.0f, messagesRect_.bottom - messagesRect_.top),
                                    ComputeMessagesContentHeight());
}

void ClaudePanel::Draw(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd)
{
    hwnd_ = hwnd;
    if (!visible_ || state_.physicalWidth <= 0)
        return;

    ApplyInputTheme(executableInput_, L"Auto-detect claude.cmd or claude.exe", false);
    ApplyInputTheme(promptInput_, L"Ask Claude about the current file, selection, or project...", true);
    if (sendButtonRect_.right > sendButtonRect_.left)
    {
        auto &promptStyle = promptInput_.GetStyle();
        promptStyle.paddingRight = (sendButtonRect_.right - sendButtonRect_.left) + 18.0f;
    }

    D2D1_RECT_F clipRect = D2D1::RectF(state_.leftEdge, state_.topEdge, state_.rightEdge, state_.bottomEdge);
    ctx->PushAxisAlignedClip(clipRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    DrawBackground(ctx);
    DrawTitle(ctx, dwrite);

    if (ShouldShowExecutableInput())
        executableInput_.Draw(ctx, dwrite);

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

void ClaudePanel::DrawMessages(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite)
{
    IDWriteTextFormat *bodyFormat = nullptr;
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             12.5f, L"en-us", &bodyFormat);

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

    const auto &palette = UI::Theme::GetPalette();
    ctx->CreateSolidColorBrush(palette.explorerRowActive, &userBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.98f), &userTextBrush);
    ctx->CreateSolidColorBrush(UI::Theme::PrimaryText(), &assistantTextBrush);
    ctx->CreateSolidColorBrush(UI::Theme::MutedText(), &systemTextBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.92f, 0.42f, 0.40f, 1.0f), &errorTextBrush);
    ctx->CreateSolidColorBrush(UI::Theme::MutedText(), &mutedBrush);

    ctx->PushAxisAlignedClip(messagesRect_, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    float y = messagesRect_.top + 8.0f - messagesScrollbar_.GetScrollOffset();
    const float contentLeft = messagesRect_.left + 4.0f;
    const float contentRight = messagesRect_.right - 16.0f;
    const float userMaxWidth = (std::max)(180.0f, (messagesRect_.right - messagesRect_.left) * 0.74f);

    for (size_t i = 0; i < messages_.size(); ++i)
    {
        const ChatMessage &message = messages_[i];
        const float height = message.estimatedHeight;
        D2D1_RECT_F bubbleRect = D2D1::RectF(contentLeft, y, contentRight, y + height);

        if (message.role == MessageRole::User)
        {
            bubbleRect.left = (std::max)(contentLeft + 28.0f, contentRight - userMaxWidth);
        }

        if (bubbleRect.bottom < messagesRect_.top)
        {
            y += height + 12.0f;
            continue;
        }
        if (bubbleRect.top > messagesRect_.bottom)
            break;

        std::wstring content = message.content;
        if (message.role == MessageRole::Assistant && content.empty() && requestInFlight_ && (int)i == streamingMessageIndex_)
            content = L"Thinking...";

        if (message.role == MessageRole::User && userBrush)
        {
            ctx->FillRoundedRectangle(D2D1::RoundedRect(bubbleRect, 8.0f, 8.0f), userBrush);
        }

        D2D1_RECT_F bodyRect = D2D1::RectF(bubbleRect.left + 10.0f, bubbleRect.top + 9.0f,
                                           bubbleRect.right - 10.0f, bubbleRect.bottom - 8.0f);
        if (message.role != MessageRole::User)
            bodyRect.left = contentLeft + 2.0f;

        ID2D1SolidColorBrush *bodyBrush = assistantTextBrush;
        if (message.role == MessageRole::User)
            bodyBrush = userTextBrush;
        else if (message.role == MessageRole::System)
            bodyBrush = systemTextBrush ? systemTextBrush : mutedBrush;
        else if (message.role == MessageRole::Error)
            bodyBrush = errorTextBrush;

        if (message.role == MessageRole::Assistant && requestInFlight_ && (int)i == streamingMessageIndex_)
        {
            const DWORD now = GetTickCount();
            const float elapsed = requestAnimationTick_ > 0 ? (float)(now - requestAnimationTick_) : 0.0f;
            const float pulse = 0.5f + 0.5f * std::sinf(elapsed * 0.0105f);
            D2D1_COLOR_F animated = UI::Theme::PrimaryText();
            if (message.content.empty())
            {
                D2D1_COLOR_F thinkingBase = UI::Theme::MutedText();
                D2D1_COLOR_F thinkingAccent = UI::Theme::Accent();
                thinkingAccent.a = 0.95f;
                animated = LerpColor(thinkingBase, thinkingAccent, 0.28f + pulse * 0.60f);
            }
            else
            {
                D2D1_COLOR_F liveAccent = UI::Theme::Accent();
                liveAccent.a = 0.90f;
                animated = LerpColor(animated, liveAccent, 0.10f + pulse * 0.18f);
            }

            if (assistantTextBrush)
            {
                assistantTextBrush->SetColor(animated);
                bodyBrush = assistantTextBrush;
            }
        }

        if (bodyFormat && bodyBrush)
            ctx->DrawTextW(content.c_str(), (UINT32)content.size(), bodyFormat, bodyRect, bodyBrush);

        y += height + 12.0f;
    }

    ctx->PopAxisAlignedClip();
    messagesScrollbar_.Draw(ctx);

    if (mutedBrush)
        mutedBrush->Release();
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
    const bool iconOnly = accent && label.size() == 1;
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
