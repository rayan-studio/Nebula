#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <vector>
#include <windows.h>
#include <chrono>
#include "orion/editor/MarkdownViewMode.h"

struct Tab
{
    std::wstring filePath;
    std::wstring displayName;
    bool isDirty = false;
    bool isActive = false;
    bool isMarkdown = false;
    Orion::MarkdownViewMode markdownViewMode = Orion::MarkdownViewMode::Code;
};

class TabBar
{
public:
    TabBar();
    ~TabBar();

    void Draw(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd);
    void UpdateLayout(float left, float top, float right);

    int AddTab(const std::wstring &filePath, const std::wstring &displayName);
    void CloseTab(int index);
    void SetActiveTab(int index);
    int GetActiveTabIndex() const { return activeTabIndex_; }
    const Tab *GetActiveTab() const;
    const Tab *GetTab(int index) const;
    void SetTabDirty(int index, bool dirty);
    bool IsTabDirty(int index) const;
    void SetTabMarkdown(int index, bool isMarkdown);
    void SetTabMarkdownViewMode(int index, Orion::MarkdownViewMode mode);

    int GetTabCount() const { return (int)tabs_.size(); }
    float GetHeight() const { return tabs_.empty() ? 0.0f : (tabHeight_ + 1.0f); }
    void DrawCloseOrDirty(ID2D1RenderTarget *ctx, HWND hwnd, const D2D1_RECT_F &rect, bool hovered, bool dirty) const;

    // Height of the small path bar shown under the tabs
    float pathBarHeight_ = 35.0f;

    // Update tab's file path and display name after Save As
    void UpdateTabPath(int index, const std::wstring &filePath, const std::wstring &displayName);

    // Events
    int OnLeftButtonDown(POINT pt);
    int OnMouseMove(POINT pt);
    int FindTabIndexByFilePath(const std::wstring &filePath) const;
    bool ClearHover();

    static const int TAB_CLICKED_CLOSE = -3;
    static const int TAB_CLICKED_SET_MARKDOWN_VIEW = -4;
    int GetLastCloseRequestIndex() const { return lastCloseRequestIndex_; }
    int GetLastMarkdownViewModeIndex() const { return lastMarkdownViewModeIndex_; }
    Orion::MarkdownViewMode GetLastMarkdownViewMode() const { return lastMarkdownViewMode_; }

private:
    std::vector<std::wstring> mruHistory_; // most-recently-used file paths, front is most recent

private:
    std::vector<Tab> tabs_;
    int activeTabIndex_ = -1;
    int hoveredTabIndex_ = -1;
    int hoveredCloseIndex_ = -1;
    int hoveredMarkdownModeIndex_ = -1;
    Orion::MarkdownViewMode hoveredMarkdownViewMode_ = Orion::MarkdownViewMode::Code;

    float leftEdge_ = 0.0f;
    float topEdge_ = 0.0f;
    float rightEdge_ = 0.0f;
    float tabHeight_ = 35.0f;
    float tabWidth_ = 150.0f;

    // Helpers for close button
    D2D1_RECT_F CloseRectForTab(int index) const;
    bool IsPointInCloseRect(int index, POINT pt) const;
    void DrawCloseButton(ID2D1RenderTarget *ctx, const D2D1_RECT_F &rect, bool hovered) const;
    D2D1_RECT_F MarkdownToolbarRect() const;
    D2D1_RECT_F MarkdownModeRect(Orion::MarkdownViewMode mode) const;
    bool IsPointInMarkdownModeRect(Orion::MarkdownViewMode mode, POINT pt) const;
    void DrawMarkdownModeButton(
        ID2D1RenderTarget *ctx,
        const D2D1_RECT_F &rect,
        Orion::MarkdownViewMode mode,
        bool active,
        bool hovered) const;

    // Last requested close index (UI only) - set when close button clicked
    int lastCloseRequestIndex_ = -1;
    int lastMarkdownViewModeIndex_ = -1;
    Orion::MarkdownViewMode lastMarkdownViewMode_ = Orion::MarkdownViewMode::Code;
};
