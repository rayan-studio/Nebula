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
    bool RunCommit();
    bool PushCurrentBranch(std::wstring &outError);
    bool BuildDiffViewForChange(const Panels::GitChange &change,
                                std::vector<int> &addedLines,
                                std::vector<int> &deletedLines,
                                GitDiffDecorations::SplitViewData &splitData);

    void DrawChanges(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd);

    bool IsPointInRect(const D2D1_RECT_F &rect, POINT pt) const;
    int HitTestChange(POINT pt) const;
    bool HandleInputClick(HWND hwnd, POINT pt);

    static std::wstring Trim(const std::wstring &s);
    static std::wstring DecodeGitPath(const std::wstring &pathField);
    static std::string WideToUtf8(const std::wstring &text);
    static std::wstring Utf8ToWide(const std::string &text);
    static std::wstring GetLastGitError(const std::wstring &fallback);

    TextInput commitMessageInput_;

    Scrollbar changesScrollbar_;

    D2D1_RECT_F changesRect_ = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F authStatusRect_ = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F infoRect_ = D2D1::RectF(0, 0, 0, 0);

    std::wstring repoRoot_;
    std::wstring lastError_;
    bool isGitRepo_ = false;
    bool libgit2Ready_ = false;

    std::vector<Panels::GitChange> changes_;
    int hoveredChangeIndex_ = -1;
    int selectedChangeIndex_ = -1;

    float changeRowHeight_ = 24.0f;

    bool capturedScrollbar_ = false;
    bool wasVisibleLastLayout_ = false;
    bool hasAutoRefreshed_ = false;
    ULONGLONG lastAutoRefreshTick_ = 0;
};
