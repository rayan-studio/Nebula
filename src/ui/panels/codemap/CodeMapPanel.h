#pragma once

#include "ui/panels/Panel.h"
#include "ui/components/scrollbar/Scrollbar.h"

#include <string>
#include <unordered_map>
#include <vector>

class CodeMapPanel : public Panel
{
public:
    CodeMapPanel();

    void Initialize() override;
    void Draw(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd) override;
    void UpdateLayout(HWND hwnd) override;
    void OnMouseMove(HWND hwnd, POINT clientPoint) override;
    void OnLeftButtonDown(HWND hwnd, POINT clientPoint) override;
    void OnLeftButtonUp(HWND hwnd) override;
    void OnMouseWheel(HWND hwnd, int delta) override;

private:
    struct NodeItem
    {
        std::wstring filePath;
        std::wstring title;
        std::wstring subtitle;
        bool hasErrors = false;
        bool hasWarnings = false;
        bool buildClean = false;
        bool buildInProgress = false;
        DWORD buildDurationMs = 0;
        D2D1_RECT_F rect = D2D1::RectF(0, 0, 0, 0);
    };

    void RefreshGraphDataIfNeeded();
    void RefreshGraphData();
    float ComputeContentHeight() const;
    int HitTestNode(POINT clientPoint) const;

    static std::wstring NormalizePath(const std::wstring &path);
    static std::wstring MakeCompareKey(const std::wstring &path);
    static bool IsSamePath(const std::wstring &a, const std::wstring &b);

    std::wstring GetDisplayTitle(const std::wstring &path) const;
    std::wstring GetDisplaySubtitle(const std::wstring &path) const;
    void ApplyBuildStatus(NodeItem &item, bool buildSucceeded) const;
    std::wstring BuildSubtitleWithStatus(const NodeItem &item) const;

    Scrollbar scrollbar_;
    D2D1_RECT_F contentRect_ = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F summaryRect_ = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F focusRect_ = D2D1::RectF(0, 0, 0, 0);

    std::unordered_map<std::wstring, std::vector<std::wstring>> includesByFile_;
    std::wstring projectRoot_;
    std::wstring focusedFile_;
    std::vector<NodeItem> includeNodes_;
    std::vector<NodeItem> incomingNodes_;

    int hoveredNodeIndex_ = -1;
    int selectedNodeIndex_ = -1;
    bool graphDataValid_ = false;
    DWORD lastRefreshTick_ = 0;
    std::wstring lastObservedActiveFile_;

    float contentHeight_ = 0.0f;
    float sectionHeaderHeight_ = 24.0f;
    float nodeHeight_ = 48.0f;
    float nodeGap_ = 8.0f;
};
