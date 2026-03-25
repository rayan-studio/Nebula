#pragma once

#include "ui/panels/Panel.h"
#include "ui/components/input/TextInput.h"
#include "ui/components/scrollbar/Scrollbar.h"
#include "ui/panels/git/GitDiffDecorations.h"

#include <string>
#include <vector>

namespace Panels
{
    struct GitChange
    {
        wchar_t indexStatus = L' ';
        wchar_t worktreeStatus = L' ';
        unsigned int statusFlags = 0;
        std::wstring path;
    };
}

class GitPanel : public Panel
{
public:
    GitPanel();
    ~GitPanel() override;

    void Initialize() override;
    void Draw(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd) override;
    void UpdateLayout(HWND hwnd) override;
    void OnMouseMove(HWND hwnd, POINT clientPoint) override;
    void OnLeftButtonDown(HWND hwnd, POINT clientPoint) override;
    void OnLeftButtonUp(HWND hwnd) override;
    void OnMouseWheel(HWND hwnd, int delta) override;
    void OnChar(wchar_t ch) override;
    void OnKeyDown(WPARAM key) override;

    bool IsInputFocused() const;
    void UnfocusInputs();

private:
    void ApplyInputTheme(TextInput &input, const std::wstring &placeholder, const std::wstring &icon = L"");
    void SyncRepoPathFromExplorer();
    void RefreshStatus();
    void RefreshBranchInfo();
    bool FetchFromRemote();
    bool PullFromRemote();
    bool RunCommit();
    bool RunCommit(bool pushAfter);
    bool PushCurrentBranch(std::wstring &outError);
    bool RunPushOnly();
    bool ExecuteQuickAction(int actionIndex);
    const wchar_t *GetQuickActionLabel(int actionIndex) const;
    int HitTestQuickActionMenuItem(POINT pt) const;
    int HitTestChange(POINT pt) const;
    int HitTestSection(POINT pt) const;
    bool BuildDiffViewForChange(const Panels::GitChange &change,
                                std::vector<int> &addedLines,
                                std::vector<int> &deletedLines,
                                GitDiffDecorations::SplitViewData &splitData);

    void DrawBranchBar(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd);
    void DrawChanges(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd);
    void DrawQuickActions(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd);

    bool IsPointInRect(const D2D1_RECT_F &rect, POINT pt) const;
    bool HandleInputClick(HWND hwnd, POINT pt);

    static std::wstring Trim(const std::wstring &s);
    static std::wstring DecodeGitPath(const std::wstring &pathField);
    static std::string WideToUtf8(const std::wstring &text);
    static std::wstring Utf8ToWide(const std::string &text);
    static std::wstring GetLastGitError(const std::wstring &fallback);

    TextInput commitMessageInput_;
    Scrollbar changesScrollbar_;

    D2D1_RECT_F changesRect_         = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F branchBarRect_       = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F authStatusRect_      = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F infoRect_            = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F quickActionPrimaryRect_ = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F quickActionToggleRect_  = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F quickActionMenuRect_    = D2D1::RectF(0, 0, 0, 0);

    // Branch toolbar buttons
    D2D1_RECT_F pullBtnRect_    = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F refreshBtnRect_ = D2D1::RectF(0, 0, 0, 0);
    bool pullBtnHovered_    = false;
    bool refreshBtnHovered_ = false;

    std::wstring repoRoot_;
    std::wstring lastError_;
    bool isGitRepo_     = false;
    bool libgit2Ready_  = false;

    // Branch info
    std::wstring currentBranch_;
    int aheadCount_  = 0;
    int behindCount_ = 0;

    std::vector<Panels::GitChange> changes_;

    // Categorized indices into changes_ (computed at refresh time)
    std::vector<int> conflictIndices_;
    std::vector<int> stagedIndices_;
    std::vector<int> changesIndices_;

    // Section collapse state (0=Conflicts, 1=Staged, 2=Changes)
    bool sectionCollapsed_[3] = {false, false, false};
    int hoveredSectionIndex_  = -1;

    int hoveredChangeIndex_  = -1;
    int selectedChangeIndex_ = -1;

    float changeRowHeight_ = 24.0f;

    static constexpr float kSectionHeaderH = 22.0f;
    static constexpr float kBranchBarH     = 36.0f;

    bool capturedScrollbar_        = false;
    bool quickActionPrimaryHovered_ = false;
    bool quickActionToggleHovered_  = false;
    bool quickActionMenuOpen_       = false;
    int  quickActionHoveredIndex_   = -1;
    int  quickActionPrimaryIndex_   = 0;
    bool wasVisibleLastLayout_  = false;
    bool hasAutoRefreshed_      = false;
    ULONGLONG lastAutoRefreshTick_ = 0;
};
