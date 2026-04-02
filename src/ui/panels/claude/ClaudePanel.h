#pragma once

#include "ui/components/input/TextInput.h"
#include "ui/components/scrollbar/Scrollbar.h"
#include "ui/panels/Panel.h"
#include "ClaudeCliBridge.h"
#include "orion/editor/Editor.h"

#include <memory>
#include <string>
#include <vector>

class ClaudePanel : public Panel
{
public:
    ClaudePanel();
    ~ClaudePanel() override;

    void Initialize() override;
    void Draw(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd) override;
    void UpdateLayout(HWND hwnd) override;
    void OnMouseMove(HWND hwnd, POINT clientPoint) override;
    void OnLeftButtonDown(HWND hwnd, POINT clientPoint) override;
    void OnLeftButtonUp(HWND hwnd) override;
    void OnMouseWheel(HWND hwnd, int delta) override;
    void OnChar(wchar_t ch) override;
    void OnKeyDown(WPARAM key) override;
    bool IsDockedRight() const override { return true; }
    bool UpdateUiAnimation();

    bool IsInputFocused() const { return executableInput_.IsFocused() || promptInput_.IsFocused(); }

private:
    enum class MessageRole
    {
        User,
        Assistant,
        Tool,
        System,
        Error,
    };

    enum class MessageKind
    {
        Text,
        Tool,
    };

    struct ChatMessage
    {
        MessageRole role = MessageRole::System;
        MessageKind kind = MessageKind::Text;
        std::wstring title;
        std::wstring subtitle;
        std::wstring content;
        float estimatedHeight = 0.0f;
        bool success = true;
    };

    struct MarkdownPreviewCacheEntry
    {
        std::unique_ptr<Orion::Editor> editor;
        std::wstring cachedText;
        float cachedWidth = 0.0f;
        float measuredHeight = 0.0f;
    };

    struct ConversationUsageStats
    {
        int userMessages = 0;
        int assistantMessages = 0;
        int toolCalls = 0;
        int inputTokens = 0;
        int outputTokens = 0;
        std::wstring lastUpdated;
    };

    struct ConversationHistoryEntry
    {
        std::wstring sessionId;
        std::wstring title;
        std::wstring subtitle;
        std::wstring jsonlPath;
        std::wstring sortTimestamp;
        ConversationUsageStats usage;
    };

    void ApplyInputTheme(TextInput &input, const std::wstring &placeholder, bool multiline);
    void DrawHeader(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite);
    void DrawToolbar(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite);
    void DrawMessages(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite);
    void DrawHistoryList(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite);
    void DrawUsageOverlay(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite);
    void DrawComposer(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite);
    void DrawButton(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite,
                    const D2D1_RECT_F &rect, const std::wstring &label,
                    bool hovered, bool accent, bool enabled = true);
    float EstimateMessageHeight(const std::wstring &text, float width) const;
    float MeasureWidthForRole(MessageRole role) const;
    float ComputeMessagesContentHeight() const;
    float ComputeHistoryContentHeight() const;
    float ComputeScrollContentHeight() const;
    void RecomputeMessageHeights();
    void ScrollMessagesToBottom();
    void InvalidatePanel() const;

    void QueueSystemMessage(const std::wstring &text, MessageRole role = MessageRole::System);
    void QueueToolMessage(const ClaudeCliBridge::ToolEvent &event);
    int EnsureStreamingAssistantMessage();
    std::wstring GetDisplayMessageText(size_t index) const;
    bool ShouldUseMarkdownPreview(const ChatMessage &message, const std::wstring &displayText) const;
    void RefreshMarkdownPreviews(IDWriteFactory *dwrite);
    void RefreshConversationHistory(bool force);
    bool LoadConversationFromHistoryIndex(size_t index);
    void SyncCurrentHistorySelection();
    std::wstring ResolveClaudeProjectHistoryDirectory() const;
    std::wstring CurrentConversationTitle() const;
    void SaveConfiguredExecutable();
    void LoadConfiguredExecutable();
    void ResolveExecutablePath(bool preferSavedPath);
    void RefreshAuthStatus();
    void StartLoginFlow();
    void SubmitPrompt();
    bool CanSubmitPrompt() const;
    bool ShouldShowExecutableInput() const;
    std::wstring BuildPromptWithContext(const std::wstring &userPrompt) const;
    std::wstring ResolveWorkingDirectory() const;
    static std::wstring GetSettingsPath();
    static std::wstring JoinLines(const std::vector<std::wstring> &lines, size_t maxChars, bool &outTruncated);
    static std::wstring Trim(const std::wstring &text);
    static float EstimateWrappedTextHeight(const std::wstring &text, float width, float charsPerLineDivisor, float lineHeight);

protected:
    bool IsResizeHandleOnLeft() const override { return true; }

private:
    ClaudeCliBridge bridge_;
    TextInput executableInput_;
    TextInput promptInput_;
    Scrollbar messagesScrollbar_;

    std::vector<ChatMessage> messages_;
    std::vector<MarkdownPreviewCacheEntry> markdownPreviewCache_;
    std::wstring configuredExecutablePath_;
    std::wstring sessionId_;
    std::wstring authDetail_;
    HWND hwnd_ = nullptr;

    D2D1_RECT_F headerRect_ = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F detectButtonRect_ = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F loginButtonRect_ = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F pathButtonRect_ = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F toolbarRect_ = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F historyButtonRect_ = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F usageButtonRect_ = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F usageLinkRect_ = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F sendButtonRect_ = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F messagesRect_ = D2D1::RectF(0, 0, 0, 0);

    bool detectButtonHovered_ = false;
    bool loginButtonHovered_ = false;
    bool pathButtonHovered_ = false;
    bool historyButtonHovered_ = false;
    bool usageButtonHovered_ = false;
    bool usageLinkHovered_ = false;
    bool sendButtonHovered_ = false;
    bool initialAuthRefreshPending_ = true;
    bool requestInFlight_ = false;
    bool showExecutableInput_ = false;
    bool showHistory_ = false;
    bool showUsageOverlay_ = false;
    int streamingMessageIndex_ = -1;
    int hoveredHistoryIndex_ = -1;
    int selectedHistoryIndex_ = -1;
    DWORD requestAnimationTick_ = 0;

    ClaudeCliBridge::AuthInfo accountInfo_;
    ConversationUsageStats currentConversationUsage_;
    std::vector<ConversationHistoryEntry> historyEntries_;
    std::vector<D2D1_RECT_F> historyRowRects_;
    std::wstring historyProjectDirectory_;
};
