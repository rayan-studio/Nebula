#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class CodeMapTabView
{
public:
    enum class NodeRole
    {
        Focus,
        Incoming,
        Outgoing,
        SecondaryIncoming,
        SecondaryOutgoing
    };

    void UpdateLayout(HWND hwnd, float left, float top, float right, float bottom);
    void Draw(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd);

    void OnMouseMove(HWND hwnd, POINT clientPoint);
    void OnLeftButtonDown(HWND hwnd, POINT clientPoint);
    void OnLeftButtonUp(HWND hwnd);
    void OnMouseWheel(HWND hwnd, int delta);
    bool IsPointInView(POINT clientPoint) const;

private:
    struct GraphNode
    {
        std::wstring filePath;
        std::wstring key;
        std::wstring title;
        std::wstring subtitle;
        int level = 0;
        NodeRole role = NodeRole::Focus;
        bool hasErrors = false;
        bool hasWarnings = false;
        bool buildClean = false;
        bool buildInProgress = false;
        DWORD buildDurationMs = 0;
        D2D1_RECT_F worldRect = D2D1::RectF(0, 0, 0, 0);
        D2D1_RECT_F screenRect = D2D1::RectF(0, 0, 0, 0);
    };

    struct GraphEdge
    {
        int fromIndex = -1;
        int toIndex = -1;
        bool outgoing = true;
    };

    void RefreshGraphDataIfNeeded(bool forceCurrentFile = false);
    void RefreshGraphData(bool forceCurrentFile = false);
    void RebuildGraphModel();
    void LayoutGraph();
    void FitGraphToViewport();
    void UpdateScreenRects();
    void UpdateFocusHighlights();
    void CenterViewOnNode(int nodeIndex);

    int AddOrUpdateNode(const std::wstring &path, int level, NodeRole role);
    void AddEdge(int fromIndex, int toIndex, bool outgoing);
    int HitTestNode(POINT clientPoint) const;
    int FindNodeIndexByKey(const std::wstring &key) const;

    std::vector<std::wstring> GetNormalizedAdjacency(const std::wstring &path, bool reverse) const;
    std::wstring GetCurrentFileFromWorkspace() const;
    std::wstring GetCurrentProjectRootFromWorkspace() const;
    std::wstring GetDisplayTitle(const std::wstring &path) const;
    std::wstring GetDisplaySubtitle(const std::wstring &path) const;

    static std::wstring NormalizePath(const std::wstring &path);
    static std::wstring MakeCompareKey(const std::wstring &path);
    static int ComputeColumnForPath(const std::wstring &path);
    static NodeRole ComputeRoleForPath(const std::wstring &path);

    D2D1_POINT_2F WorldToScreen(float worldX, float worldY) const;

    D2D1_RECT_F bounds_ = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F graphRect_ = D2D1::RectF(0, 0, 0, 0);
    D2D1_RECT_F emptyStateRect_ = D2D1::RectF(0, 0, 0, 0);

    std::unordered_map<std::wstring, std::vector<std::wstring>> includesByFile_;
    std::unordered_map<std::wstring, std::vector<std::wstring>> reverseIncludes_;
    std::unordered_map<std::wstring, std::wstring> displayPathByKey_;
    std::unordered_map<std::wstring, int> nodeIndexByKey_;
    std::vector<GraphNode> nodes_;
    std::vector<GraphEdge> edges_;

    std::wstring projectRoot_;
    std::wstring focusedFile_;
    std::wstring selectedNodeKey_;
    std::wstring hoveredNodeKey_;
    std::unordered_set<std::wstring> highlightedNodeKeys_;

    float zoom_ = 1.0f;
    float panX_ = 0.0f;
    float panY_ = 0.0f;
    float graphMinX_ = 0.0f;
    float graphMinY_ = 0.0f;
    float graphMaxX_ = 0.0f;
    float graphMaxY_ = 0.0f;
    float currentNodeWidth_ = 180.0f;
    float currentNodeHeight_ = 34.0f;
    float currentRowGap_ = 16.0f;
    float currentColumnGap_ = 72.0f;

    bool manualFocus_ = false;
    bool draggingCanvas_ = false;
    bool layoutDirty_ = true;
    bool graphDataValid_ = false;
    DWORD lastRefreshTick_ = 0;
    std::wstring lastObservedWorkspaceFile_;
    std::wstring lastObservedWorkspaceRoot_;
    bool lastObservedBuildInProgress_ = false;
    bool hasInitializedCamera_ = false;

    POINT lastMousePoint_ = {0, 0};
    POINT dragStartPoint_ = {0, 0};
    float dragStartPanX_ = 0.0f;
    float dragStartPanY_ = 0.0f;
};
