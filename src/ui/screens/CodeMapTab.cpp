#include "CodeMapTab.h"

#include "core/explorer/Explorer.h"
#include "helpers/window_helpers.h"
#include "lsp/LspManager.h"
#include "ui/panels/terminal/TerminalPanel.h"
#include "ui/theme/Theme.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <system_error>

namespace
{
    constexpr UINT kOpenFileMessage = WM_USER + 100;
    constexpr float kMinZoom = 0.45f;
    constexpr float kMaxZoom = 1.8f;

    std::wstring ToLowerCopy(std::wstring value)
    {
        for (wchar_t &ch : value)
        {
            if (ch == L'/')
                ch = L'\\';
            ch = (wchar_t)towlower(ch);
        }
        return value;
    }

    float ClampFloat(float value, float minValue, float maxValue)
    {
        return (std::max)(minValue, (std::min)(value, maxValue));
    }

    bool PointInRect(POINT pt, const D2D1_RECT_F &rect)
    {
        return pt.x >= rect.left && pt.x <= rect.right &&
               pt.y >= rect.top && pt.y <= rect.bottom;
    }

    bool RectIntersects(const D2D1_RECT_F &a, const D2D1_RECT_F &b)
    {
        return a.left < b.right && a.right > b.left &&
               a.top < b.bottom && a.bottom > b.top;
    }

    D2D1_RECT_F ExpandRect(const D2D1_RECT_F &rect, float amount)
    {
        return D2D1::RectF(rect.left - amount, rect.top - amount,
                           rect.right + amount, rect.bottom + amount);
    }

    RECT ToClientRect(const D2D1_RECT_F &rect)
    {
        RECT out{};
        out.left = (LONG)std::floor(rect.left);
        out.top = (LONG)std::floor(rect.top);
        out.right = (LONG)std::ceil(rect.right);
        out.bottom = (LONG)std::ceil(rect.bottom);
        return out;
    }

    bool IsVirtualWorkspacePath(const std::wstring &path)
    {
        return !path.empty() && path.rfind(L"__", 0) == 0;
    }

    uint32_t StableHash(const std::wstring &value)
    {
        uint32_t hash = 2166136261u;
        for (wchar_t ch : value)
        {
            wchar_t c = (ch == L'/') ? L'\\' : (wchar_t)towlower(ch);
            hash ^= (uint32_t)c;
            hash *= 16777619u;
        }
        return hash;
    }

    bool IsHeaderExtension(const std::wstring &ext)
    {
        return ext == L".h" || ext == L".hpp" || ext == L".hh" || ext == L".hxx" || ext == L".inl";
    }

    bool IsSourceExtension(const std::wstring &ext)
    {
        return ext == L".c" || ext == L".cc" || ext == L".cpp" || ext == L".cxx";
    }

    D2D1_RECT_F SnapStrokeRect(const D2D1_RECT_F &rect)
    {
        return D2D1::RectF(
            std::floor(rect.left) + 0.5f,
            std::floor(rect.top) + 0.5f,
            std::floor(rect.right) - 0.5f,
            std::floor(rect.bottom) - 0.5f);
    }

    D2D1_COLOR_F NodeTint(CodeMapTabView::NodeRole role)
    {
        switch (role)
        {
        case CodeMapTabView::NodeRole::Focus:
            return D2D1::ColorF(0.17f, 0.48f, 0.93f, 1.0f);
        case CodeMapTabView::NodeRole::Incoming:
            return D2D1::ColorF(0.18f, 0.69f, 0.46f, 1.0f);
        case CodeMapTabView::NodeRole::Outgoing:
            return D2D1::ColorF(0.20f, 0.57f, 0.93f, 1.0f);
        case CodeMapTabView::NodeRole::SecondaryIncoming:
            return D2D1::ColorF(0.27f, 0.55f, 0.42f, 1.0f);
        case CodeMapTabView::NodeRole::SecondaryOutgoing:
        default:
            return D2D1::ColorF(0.26f, 0.46f, 0.78f, 1.0f);
        }
    }

    std::wstring FormatDurationLabel(DWORD durationMs)
    {
        if (durationMs >= 1000)
        {
            wchar_t buffer[32];
            swprintf_s(buffer, L"%.1fs", (double)durationMs / 1000.0);
            return buffer;
        }
        return std::to_wstring((unsigned long long)durationMs) + L"ms";
    }

    void InvalidateGraphRect(HWND hwnd, const D2D1_RECT_F &rect)
    {
        RECT dirty = ToClientRect(rect);
        InvalidateRect(hwnd, &dirty, FALSE);
    }
}

std::wstring CodeMapTabView::NormalizePath(const std::wstring &path)
{
    if (path.empty())
        return {};

    std::filesystem::path normalized = std::filesystem::path(path).lexically_normal();
    std::wstring value = normalized.wstring();
    for (wchar_t &ch : value)
    {
        if (ch == L'/')
            ch = L'\\';
    }
    return value;
}

std::wstring CodeMapTabView::MakeCompareKey(const std::wstring &path)
{
    return ToLowerCopy(path);
}

int CodeMapTabView::ComputeColumnForPath(const std::wstring &path)
{
    std::filesystem::path fsPath(path);
    std::wstring ext = ToLowerCopy(fsPath.extension().wstring());
    std::wstring bucketSource = fsPath.parent_path().wstring();
    if (bucketSource.empty())
        bucketSource = path;
    int bucket = (int)(StableHash(bucketSource) % 3u);

    if (IsHeaderExtension(ext))
        return -3 + bucket;
    if (IsSourceExtension(ext))
        return 1 + bucket;
    return 0;
}

CodeMapTabView::NodeRole CodeMapTabView::ComputeRoleForPath(const std::wstring &path)
{
    std::wstring ext = ToLowerCopy(std::filesystem::path(path).extension().wstring());
    if (IsHeaderExtension(ext))
        return NodeRole::Incoming;
    if (IsSourceExtension(ext))
        return NodeRole::Outgoing;
    return NodeRole::SecondaryOutgoing;
}

std::wstring CodeMapTabView::GetCurrentFileFromWorkspace() const
{
    std::wstring activePath = GetExplorerManager().GetState().activePath;
    if (IsVirtualWorkspacePath(activePath))
        return {};
    return NormalizePath(activePath);
}

std::wstring CodeMapTabView::GetCurrentProjectRootFromWorkspace() const
{
    return NormalizePath(GetExplorerManager().GetState().rootPath);
}

std::wstring CodeMapTabView::GetDisplayTitle(const std::wstring &path) const
{
    if (path.empty())
        return L"(unknown)";
    return std::filesystem::path(path).filename().wstring();
}

std::wstring CodeMapTabView::GetDisplaySubtitle(const std::wstring &path) const
{
    if (path.empty())
        return {};

    std::filesystem::path full(path);
    std::error_code ec;
    if (!projectRoot_.empty())
    {
        std::filesystem::path rel = std::filesystem::relative(full, projectRoot_, ec);
        if (!ec)
        {
            std::wstring value = rel.parent_path().wstring();
            return value == L"." ? L"/" : value;
        }
    }

    std::wstring parent = full.parent_path().wstring();
    return parent.empty() ? L"/" : parent;
}

std::vector<std::wstring> CodeMapTabView::GetNormalizedAdjacency(const std::wstring &path, bool reverse) const
{
    const auto &mapRef = reverse ? reverseIncludes_ : includesByFile_;
    std::wstring key = MakeCompareKey(NormalizePath(path));
    auto it = mapRef.find(key);
    if (it == mapRef.end())
        return {};
    return it->second;
}

void CodeMapTabView::RefreshGraphDataIfNeeded(bool forceCurrentFile)
{
    std::wstring currentFile = GetCurrentFileFromWorkspace();
    std::wstring currentKey = MakeCompareKey(currentFile);
    std::wstring currentRoot = GetCurrentProjectRootFromWorkspace();
    std::wstring currentRootKey = MakeCompareKey(currentRoot);
    DWORD now = GetTickCount();
    bool buildInProgress = GetTerminalPanel().IsBuildInProgress();

    bool workspaceFileChanged = currentKey != lastObservedWorkspaceFile_;
    bool workspaceRootChanged = currentRootKey != lastObservedWorkspaceRoot_;
    bool buildStateChanged = buildInProgress != lastObservedBuildInProgress_;
    DWORD refreshInterval = buildInProgress ? 120 : 750;
    bool periodicRefresh = !graphDataValid_ || (now - lastRefreshTick_) >= refreshInterval;
    if (!forceCurrentFile && !workspaceFileChanged && !workspaceRootChanged && !buildStateChanged && !periodicRefresh)
        return;

    if (workspaceRootChanged)
    {
        manualFocus_ = false;
        focusedFile_.clear();
        selectedNodeKey_.clear();
        hoveredNodeKey_.clear();
        highlightedNodeKeys_.clear();
        hasInitializedCamera_ = false;
        layoutDirty_ = true;
    }

    RefreshGraphData(forceCurrentFile);
    lastObservedWorkspaceFile_ = currentKey;
    lastObservedWorkspaceRoot_ = currentRootKey;
    lastObservedBuildInProgress_ = buildInProgress;
    lastRefreshTick_ = now;
    graphDataValid_ = true;
}

int CodeMapTabView::AddOrUpdateNode(const std::wstring &path, int level, NodeRole role)
{
    std::wstring normalized = NormalizePath(path);
    if (normalized.empty())
        return -1;

    std::wstring key = MakeCompareKey(normalized);
    auto it = nodeIndexByKey_.find(key);
    if (it != nodeIndexByKey_.end())
    {
        return it->second;
    }

    GraphNode node;
    node.filePath = normalized;
    node.key = key;
    node.title = GetDisplayTitle(normalized);
    node.subtitle = GetDisplaySubtitle(normalized);
    node.level = level;
    node.role = role;

    int index = (int)nodes_.size();
    nodes_.push_back(std::move(node));
    nodeIndexByKey_[key] = index;
    return index;
}

void CodeMapTabView::AddEdge(int fromIndex, int toIndex, bool outgoing)
{
    if (fromIndex < 0 || toIndex < 0 || fromIndex == toIndex)
        return;

    for (const GraphEdge &edge : edges_)
    {
        if (edge.fromIndex == fromIndex && edge.toIndex == toIndex)
            return;
    }

    GraphEdge edge;
    edge.fromIndex = fromIndex;
    edge.toIndex = toIndex;
    edge.outgoing = outgoing;
    edges_.push_back(edge);
}

void CodeMapTabView::RebuildGraphModel()
{
    nodes_.clear();
    edges_.clear();
    nodeIndexByKey_.clear();

    std::vector<std::wstring> allPaths;
    allPaths.reserve(includesByFile_.size() * 2);

    for (const auto &entry : displayPathByKey_)
    {
        if (!entry.second.empty())
            allPaths.push_back(entry.second);
    }

    for (const auto &entry : includesByFile_)
    {
        for (const std::wstring &includePath : entry.second)
        {
            if (!includePath.empty())
                allPaths.push_back(includePath);
        }
    }

    std::sort(allPaths.begin(), allPaths.end());
    allPaths.erase(std::unique(allPaths.begin(), allPaths.end()), allPaths.end());

    for (const std::wstring &path : allPaths)
        AddOrUpdateNode(path, ComputeColumnForPath(path), ComputeRoleForPath(path));

    bool buildSucceeded = GetTerminalPanel().DidLastBuildSucceed();
    for (GraphNode &node : nodes_)
    {
        std::vector<Lsp::Diagnostic> diagnostics = Lsp::LspManager::Instance().GetDiagnostics(node.filePath);
        for (const auto &diag : diagnostics)
        {
            if (diag.severity == Lsp::DiagnosticSeverity::Error)
                node.hasErrors = true;
            else if (diag.severity == Lsp::DiagnosticSeverity::Warning)
                node.hasWarnings = true;
        }

        int buildSeverity = GetTerminalPanel().GetBuildIssueSeverity(node.filePath);
        if (buildSeverity >= 2)
            node.hasErrors = true;
        else if (buildSeverity == 1)
            node.hasWarnings = true;

        auto buildProgress = GetTerminalPanel().GetBuildFileProgress(node.filePath);
        node.buildInProgress = buildProgress.inProgress;
        node.buildDurationMs = buildProgress.durationMs;

        if (buildProgress.completed && buildSucceeded && !node.hasErrors && !node.hasWarnings)
            node.buildClean = true;
    }

    for (const auto &entry : includesByFile_)
    {
        int fromIndex = FindNodeIndexByKey(entry.first);
        if (fromIndex < 0)
            continue;

        for (const std::wstring &includePath : entry.second)
        {
            int toIndex = FindNodeIndexByKey(MakeCompareKey(includePath));
            AddEdge(fromIndex, toIndex, true);
        }
    }

    UpdateFocusHighlights();
}

void CodeMapTabView::LayoutGraph()
{
    graphMinX_ = 0.0f;
    graphMinY_ = 0.0f;
    graphMaxX_ = 0.0f;
    graphMaxY_ = 0.0f;

    if (nodes_.empty())
        return;

    if (nodes_.size() > 140)
    {
        currentNodeWidth_ = 168.0f;
        currentNodeHeight_ = 24.0f;
        currentRowGap_ = 8.0f;
        currentColumnGap_ = 56.0f;
    }
    else if (nodes_.size() > 80)
    {
        currentNodeWidth_ = 180.0f;
        currentNodeHeight_ = 28.0f;
        currentRowGap_ = 10.0f;
        currentColumnGap_ = 62.0f;
    }
    else if (nodes_.size() > 40)
    {
        currentNodeWidth_ = 192.0f;
        currentNodeHeight_ = 30.0f;
        currentRowGap_ = 12.0f;
        currentColumnGap_ = 68.0f;
    }
    else
    {
        currentNodeWidth_ = 180.0f;
        currentNodeHeight_ = 34.0f;
        currentRowGap_ = 16.0f;
        currentColumnGap_ = 72.0f;
    }

    std::unordered_map<int, std::vector<int>> columns;
    for (size_t i = 0; i < nodes_.size(); ++i)
        columns[nodes_[i].level].push_back((int)i);

    std::vector<int> levelOrder;
    levelOrder.reserve(columns.size());
    for (const auto &entry : columns)
        levelOrder.push_back(entry.first);
    std::sort(levelOrder.begin(), levelOrder.end());

    bool firstNode = true;
    for (int level : levelOrder)
    {
        auto it = columns.find(level);
        if (it == columns.end())
            continue;

        std::vector<int> &indices = it->second;
        std::sort(indices.begin(), indices.end(), [&](int a, int b) {
            const GraphNode &na = nodes_[(size_t)a];
            const GraphNode &nb = nodes_[(size_t)b];
            if (na.subtitle != nb.subtitle)
                return na.subtitle < nb.subtitle;
            return na.title < nb.title;
        });

        float totalHeight = (float)indices.size() * currentNodeHeight_;
        if (indices.size() > 1)
            totalHeight += (float)(indices.size() - 1) * currentRowGap_;
        float startY = -totalHeight * 0.5f;
        float x = (float)level * (currentNodeWidth_ + currentColumnGap_);

        for (size_t order = 0; order < indices.size(); ++order)
        {
            GraphNode &node = nodes_[(size_t)indices[order]];
            float top = startY + (float)order * (currentNodeHeight_ + currentRowGap_);
            node.worldRect = D2D1::RectF(x - currentNodeWidth_ * 0.5f,
                                         top,
                                         x + currentNodeWidth_ * 0.5f,
                                         top + currentNodeHeight_);

            if (firstNode || node.worldRect.left < graphMinX_)
                graphMinX_ = node.worldRect.left;
            if (firstNode || node.worldRect.top < graphMinY_)
                graphMinY_ = node.worldRect.top;
            if (firstNode || node.worldRect.right > graphMaxX_)
                graphMaxX_ = node.worldRect.right;
            if (firstNode || node.worldRect.bottom > graphMaxY_)
                graphMaxY_ = node.worldRect.bottom;
            firstNode = false;
        }
    }
}

void CodeMapTabView::FitGraphToViewport()
{
    if (nodes_.empty())
    {
        zoom_ = 1.0f;
        panX_ = 0.0f;
        panY_ = 0.0f;
        return;
    }

    float graphWidth = graphMaxX_ - graphMinX_;
    float graphHeight = graphMaxY_ - graphMinY_;
    float viewportWidth = (graphRect_.right - graphRect_.left) - 96.0f;
    float viewportHeight = (graphRect_.bottom - graphRect_.top) - 72.0f;

    if (graphWidth <= 0.0f || graphHeight <= 0.0f || viewportWidth <= 0.0f || viewportHeight <= 0.0f)
        return;

    float fitScaleX = viewportWidth / graphWidth;
    float fitScaleY = viewportHeight / graphHeight;
    zoom_ = ClampFloat((std::min)(fitScaleX, fitScaleY), kMinZoom, 1.0f);

    float graphCenterX = (graphMinX_ + graphMaxX_) * 0.5f;
    float graphCenterY = (graphMinY_ + graphMaxY_) * 0.5f;
    panX_ = -graphCenterX * zoom_;
    panY_ = -graphCenterY * zoom_;
}

D2D1_POINT_2F CodeMapTabView::WorldToScreen(float worldX, float worldY) const
{
    float centerX = (graphRect_.left + graphRect_.right) * 0.5f;
    float centerY = (graphRect_.top + graphRect_.bottom) * 0.5f;
    return D2D1::Point2F(centerX + panX_ + (worldX * zoom_),
                         centerY + panY_ + (worldY * zoom_));
}

void CodeMapTabView::UpdateScreenRects()
{
    for (GraphNode &node : nodes_)
    {
        D2D1_POINT_2F topLeft = WorldToScreen(node.worldRect.left, node.worldRect.top);
        D2D1_POINT_2F bottomRight = WorldToScreen(node.worldRect.right, node.worldRect.bottom);
        node.screenRect = D2D1::RectF(topLeft.x, topLeft.y, bottomRight.x, bottomRight.y);
    }
}

int CodeMapTabView::FindNodeIndexByKey(const std::wstring &key) const
{
    auto it = nodeIndexByKey_.find(key);
    if (it == nodeIndexByKey_.end())
        return -1;
    return it->second;
}

void CodeMapTabView::UpdateFocusHighlights()
{
    highlightedNodeKeys_.clear();
    if (focusedFile_.empty())
        return;

    std::wstring focusKey = MakeCompareKey(focusedFile_);
    highlightedNodeKeys_.insert(focusKey);

    auto incoming = GetNormalizedAdjacency(focusedFile_, true);
    auto outgoing = GetNormalizedAdjacency(focusedFile_, false);
    for (const std::wstring &path : incoming)
        highlightedNodeKeys_.insert(MakeCompareKey(path));
    for (const std::wstring &path : outgoing)
        highlightedNodeKeys_.insert(MakeCompareKey(path));
}

void CodeMapTabView::CenterViewOnNode(int nodeIndex)
{
    if (nodeIndex < 0 || nodeIndex >= (int)nodes_.size())
        return;

    const GraphNode &node = nodes_[(size_t)nodeIndex];
    float centerWorldX = (node.worldRect.left + node.worldRect.right) * 0.5f;
    float centerWorldY = (node.worldRect.top + node.worldRect.bottom) * 0.5f;
    panX_ = -centerWorldX * zoom_;
    panY_ = -centerWorldY * zoom_;
    UpdateScreenRects();
}

void CodeMapTabView::RefreshGraphData(bool forceCurrentFile)
{
    includesByFile_.clear();
    reverseIncludes_.clear();
    displayPathByKey_.clear();
    std::wstring firstDiscoveredFile;
    std::wstring previousProjectRoot = projectRoot_;

    auto rawIncludes = Lsp::LspManager::Instance().GetFileIncludes();
    for (const auto &entry : rawIncludes)
    {
        std::wstring normalizedFile = NormalizePath(entry.first);
        if (normalizedFile.empty())
            continue;
        if (firstDiscoveredFile.empty())
            firstDiscoveredFile = normalizedFile;
        std::wstring fileKey = MakeCompareKey(normalizedFile);
        displayPathByKey_[fileKey] = normalizedFile;
        std::vector<std::wstring> normalizedList;
        normalizedList.reserve(entry.second.size());

        for (const std::wstring &includePath : entry.second)
        {
            std::wstring normalized = NormalizePath(includePath);
            if (normalized.empty())
                continue;

            normalizedList.push_back(normalized);
            reverseIncludes_[MakeCompareKey(normalized)].push_back(normalizedFile);
        }

        std::sort(normalizedList.begin(), normalizedList.end());
        normalizedList.erase(std::unique(normalizedList.begin(), normalizedList.end()), normalizedList.end());
        includesByFile_[fileKey] = std::move(normalizedList);
    }

    for (auto &entry : reverseIncludes_)
    {
        std::sort(entry.second.begin(), entry.second.end());
        entry.second.erase(std::unique(entry.second.begin(), entry.second.end()), entry.second.end());
    }

    projectRoot_ = NormalizePath(Lsp::LspManager::Instance().GetProjectRoot());
    bool projectSwitched = MakeCompareKey(previousProjectRoot) != MakeCompareKey(projectRoot_);
    if (projectSwitched)
    {
        manualFocus_ = false;
        focusedFile_.clear();
        selectedNodeKey_.clear();
        hoveredNodeKey_.clear();
        highlightedNodeKeys_.clear();
        hasInitializedCamera_ = false;
        layoutDirty_ = true;
    }

    std::wstring currentFile = GetCurrentFileFromWorkspace();
    if (forceCurrentFile)
    {
        manualFocus_ = false;
        if (!currentFile.empty())
            focusedFile_ = currentFile;
    }
    else if (!manualFocus_ && !currentFile.empty())
    {
        focusedFile_ = currentFile;
    }

    if (focusedFile_.empty())
    {
        if (!currentFile.empty())
            focusedFile_ = currentFile;
        else if (!firstDiscoveredFile.empty())
            focusedFile_ = firstDiscoveredFile;
    }

    focusedFile_ = NormalizePath(focusedFile_);
    selectedNodeKey_ = MakeCompareKey(focusedFile_);

    RebuildGraphModel();
    LayoutGraph();
    if (layoutDirty_)
    {
        int focusIndex = FindNodeIndexByKey(selectedNodeKey_);
        if (focusIndex >= 0)
        {
            if (!hasInitializedCamera_)
            {
                zoom_ = nodes_.size() > 120 ? 0.92f : 1.0f;
                hasInitializedCamera_ = true;
            }
            CenterViewOnNode(focusIndex);
        }
        else
        {
            FitGraphToViewport();
        }
        layoutDirty_ = false;
    }
    UpdateScreenRects();
}

void CodeMapTabView::UpdateLayout(HWND hwnd, float left, float top, float right, float bottom)
{
    bounds_ = D2D1::RectF(left, top, right, bottom);
    (void)hwnd;
    graphRect_ = bounds_;
    emptyStateRect_ = graphRect_;

    RefreshGraphDataIfNeeded(false);
}

int CodeMapTabView::HitTestNode(POINT clientPoint) const
{
    D2D1_RECT_F hitViewport = ExpandRect(graphRect_, 8.0f);
    for (int i = (int)nodes_.size() - 1; i >= 0; --i)
    {
        if (!RectIntersects(nodes_[(size_t)i].screenRect, hitViewport))
            continue;
        if (PointInRect(clientPoint, nodes_[(size_t)i].screenRect))
            return i;
    }
    return -1;
}

bool CodeMapTabView::IsPointInView(POINT clientPoint) const
{
    return PointInRect(clientPoint, bounds_);
}

void CodeMapTabView::Draw(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd)
{
    (void)hwnd;
    if (!ctx || !dwrite)
        return;

    IDWriteTextFormat *bodyFmt = nullptr;
    IDWriteTextFormat *smallFmt = nullptr;

    dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.5f, L"fr-FR", &bodyFmt);
    dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 9.75f, L"fr-FR", &smallFmt);

    if (bodyFmt)
    {
        bodyFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        bodyFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (smallFmt)
    {
        smallFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        smallFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    IDWriteInlineObject *ellipsisSign = nullptr;
    if (dwrite && bodyFmt)
    {
        dwrite->CreateEllipsisTrimmingSign(bodyFmt, &ellipsisSign);
        if (ellipsisSign)
        {
            DWRITE_TRIMMING trimming = {};
            trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
            bodyFmt->SetTrimming(&trimming, ellipsisSign);
            if (smallFmt)
                smallFmt->SetTrimming(&trimming, ellipsisSign);
        }
    }

    const UI::Theme::Palette &palette = UI::Theme::GetPalette();
    ID2D1SolidColorBrush *textBrush = nullptr;
    ID2D1SolidColorBrush *mutedBrush = nullptr;
    ID2D1SolidColorBrush *cardBorderBrush = nullptr;
    ID2D1SolidColorBrush *edgeOutgoingBrush = nullptr;
    ID2D1SolidColorBrush *edgeIncomingBrush = nullptr;
    ID2D1SolidColorBrush *edgeOutgoingMutedBrush = nullptr;
    ID2D1SolidColorBrush *edgeIncomingMutedBrush = nullptr;
    ID2D1SolidColorBrush *gridBrush = nullptr;
    ID2D1SolidColorBrush *selectionBrush = nullptr;

    ctx->CreateSolidColorBrush(UI::Theme::PrimaryText(), &textBrush);
    ctx->CreateSolidColorBrush(UI::Theme::MutedText(), &mutedBrush);
    ctx->CreateSolidColorBrush(UI::Theme::ChromeBorder(), &cardBorderBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.20f, 0.57f, 0.93f, 0.76f), &edgeOutgoingBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.18f, 0.69f, 0.46f, 0.74f), &edgeIncomingBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.20f, 0.57f, 0.93f, 0.16f), &edgeOutgoingMutedBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.18f, 0.69f, 0.46f, 0.15f), &edgeIncomingMutedBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.32f, 0.35f, 0.40f, 0.10f), &gridBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.24f, 0.55f, 0.98f, 0.22f), &selectionBrush);
    ID2D1SolidColorBrush *nodeFillBrush = nullptr;
    ID2D1SolidColorBrush *nodeTintBrush = nullptr;
    ctx->CreateSolidColorBrush(palette.inputBackground, &nodeFillBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &nodeTintBrush);

    ctx->PushAxisAlignedClip(bounds_, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    ctx->PushAxisAlignedClip(graphRect_, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    D2D1_RECT_F visibleGraphRect = ExpandRect(graphRect_, 48.0f);
    std::vector<uint8_t> visibleNodeMask(nodes_.size(), 0);
    for (size_t i = 0; i < nodes_.size(); ++i)
    {
        const GraphNode &node = nodes_[i];
        bool visible = RectIntersects(node.screenRect, visibleGraphRect);
        if (!visible)
        {
            bool important = (node.key == selectedNodeKey_) ||
                             (node.key == hoveredNodeKey_) ||
                             (highlightedNodeKeys_.count(node.key) != 0);
            visible = important;
        }
        visibleNodeMask[i] = visible ? 1u : 0u;
    }

    if (gridBrush && zoom_ >= 0.70f)
    {
        float centerX = (graphRect_.left + graphRect_.right) * 0.5f + panX_;
        float centerY = (graphRect_.top + graphRect_.bottom) * 0.5f + panY_;
        for (int i = -8; i <= 8; ++i)
        {
            float x = centerX + ((float)i * 120.0f * zoom_);
            ctx->DrawLine(D2D1::Point2F(x, graphRect_.top), D2D1::Point2F(x, graphRect_.bottom), gridBrush, 1.0f);
            float y = centerY + ((float)i * 80.0f * zoom_);
            ctx->DrawLine(D2D1::Point2F(graphRect_.left, y), D2D1::Point2F(graphRect_.right, y), gridBrush, 1.0f);
        }
    }

    if (nodes_.empty())
    {
        if (bodyFmt && mutedBrush)
        {
            const wchar_t *emptyMessage = L"Aucune relation #include disponible pour le moment.";
            ctx->DrawTextW(emptyMessage, (UINT32)wcslen(emptyMessage), bodyFmt, emptyStateRect_, mutedBrush);
        }
    }
    else
    {
        for (const GraphEdge &edge : edges_)
        {
            if (edge.fromIndex < 0 || edge.toIndex < 0 ||
                edge.fromIndex >= (int)nodes_.size() || edge.toIndex >= (int)nodes_.size())
                continue;

            if (!visibleNodeMask[(size_t)edge.fromIndex] && !visibleNodeMask[(size_t)edge.toIndex])
                continue;

            const GraphNode &from = nodes_[(size_t)edge.fromIndex];
            const GraphNode &to = nodes_[(size_t)edge.toIndex];
            bool edgeHovered = (!hoveredNodeKey_.empty() &&
                                (from.key == hoveredNodeKey_ || to.key == hoveredNodeKey_));
            bool edgeContext = edgeHovered;

            if (!edgeContext && zoom_ < 1.28f)
                continue;

            D2D1_RECT_F edgeBounds = D2D1::RectF(
                (std::min)(from.screenRect.left, to.screenRect.left),
                (std::min)(from.screenRect.top, to.screenRect.top),
                (std::max)(from.screenRect.right, to.screenRect.right),
                (std::max)(from.screenRect.bottom, to.screenRect.bottom));
            if (!RectIntersects(edgeBounds, visibleGraphRect))
                continue;

            bool leftToRight = from.screenRect.left <= to.screenRect.left;
            float startX = leftToRight ? from.screenRect.right : from.screenRect.left;
            float endX = leftToRight ? to.screenRect.left : to.screenRect.right;
            float startY = (from.screenRect.top + from.screenRect.bottom) * 0.5f;
            float endY = (to.screenRect.top + to.screenRect.bottom) * 0.5f;
            float horizontalSpan = std::abs(endX - startX);
            float controlOffset = (std::max)(26.0f, horizontalSpan * 0.35f);
            float cp1X = leftToRight ? startX + controlOffset : startX - controlOffset;
            float cp2X = leftToRight ? endX - controlOffset : endX + controlOffset;

            ID2D1SolidColorBrush *brush = nullptr;
            if (edgeContext)
                brush = edge.outgoing ? edgeOutgoingBrush : edgeIncomingBrush;
            else
                brush = edge.outgoing ? edgeOutgoingMutedBrush : edgeIncomingMutedBrush;
            if (!brush)
                continue;

            float thickness = edgeContext ? 1.45f : 0.6f;

            ID2D1Factory *factory = nullptr;
            ctx->GetFactory(&factory);
            ID2D1PathGeometry *path = nullptr;
            ID2D1GeometrySink *sink = nullptr;
            if (factory &&
                SUCCEEDED(factory->CreatePathGeometry(&path)) && path &&
                SUCCEEDED(path->Open(&sink)) && sink)
            {
                sink->BeginFigure(D2D1::Point2F(startX, startY), D2D1_FIGURE_BEGIN_HOLLOW);
                D2D1_BEZIER_SEGMENT curve = {};
                curve.point1 = D2D1::Point2F(cp1X, startY);
                curve.point2 = D2D1::Point2F(cp2X, endY);
                curve.point3 = D2D1::Point2F(endX, endY);
                sink->AddBezier(curve);
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->Close();
                ctx->DrawGeometry(path, brush, thickness);
            }
            if (sink)
                sink->Release();
            if (path)
                path->Release();
            if (factory)
                factory->Release();
        }

        for (const GraphNode &node : nodes_)
        {
            size_t nodeIndex = (size_t)(&node - nodes_.data());
            if (!visibleNodeMask[nodeIndex])
                continue;

            bool hovered = (node.key == hoveredNodeKey_);
            bool selected = (node.key == selectedNodeKey_);
            bool highlighted = highlightedNodeKeys_.count(node.key) != 0;
            if (selected && selectionBrush)
                ctx->FillRectangle(node.screenRect, selectionBrush);
            if (nodeFillBrush)
            {
                D2D1_COLOR_F fillColor = palette.inputBackground;
                fillColor.a = 0.95f;
                if (node.hasErrors)
                {
                    fillColor.r = 0.33f;
                    fillColor.g = 0.16f;
                    fillColor.b = 0.16f;
                    fillColor.a = 0.92f;
                }
                else if (node.hasWarnings)
                {
                    fillColor.r = 0.33f;
                    fillColor.g = 0.26f;
                    fillColor.b = 0.13f;
                    fillColor.a = 0.92f;
                }
                else if (node.buildInProgress)
                {
                    fillColor.r = 0.15f;
                    fillColor.g = 0.24f;
                    fillColor.b = 0.34f;
                    fillColor.a = 0.92f;
                }
                else if (node.buildClean)
                {
                    fillColor.r = 0.14f;
                    fillColor.g = 0.26f;
                    fillColor.b = 0.18f;
                    fillColor.a = 0.92f;
                }
                nodeFillBrush->SetColor(fillColor);
                ctx->FillRectangle(node.screenRect, nodeFillBrush);
            }
            if (cardBorderBrush)
            {
                D2D1_COLOR_F borderColor = UI::Theme::ChromeBorder();
                if (node.hasErrors)
                    borderColor = D2D1::ColorF(0.91f, 0.29f, 0.27f, 1.0f);
                else if (node.hasWarnings)
                    borderColor = D2D1::ColorF(0.88f, 0.67f, 0.22f, 1.0f);
                else if (node.buildInProgress)
                    borderColor = D2D1::ColorF(0.36f, 0.68f, 0.96f, 1.0f);
                else if (node.buildClean)
                    borderColor = D2D1::ColorF(0.29f, 0.76f, 0.41f, 1.0f);
                cardBorderBrush->SetColor(borderColor);
                ctx->DrawRectangle(SnapStrokeRect(node.screenRect), cardBorderBrush, (hovered || selected || highlighted || node.hasErrors) ? 1.6f : 1.0f);
            }
            if (nodeTintBrush)
            {
                if (node.hasErrors)
                    nodeTintBrush->SetColor(D2D1::ColorF(0.91f, 0.29f, 0.27f, 1.0f));
                else if (node.hasWarnings)
                    nodeTintBrush->SetColor(D2D1::ColorF(0.88f, 0.67f, 0.22f, 1.0f));
                else if (node.buildInProgress)
                    nodeTintBrush->SetColor(D2D1::ColorF(0.36f, 0.68f, 0.96f, 1.0f));
                else if (node.buildClean)
                    nodeTintBrush->SetColor(D2D1::ColorF(0.29f, 0.76f, 0.41f, 1.0f));
                else
                    nodeTintBrush->SetColor(NodeTint(node.role));
                float accentWidth = currentNodeWidth_ > 120.0f ? 4.0f : 2.0f;
                ctx->FillRectangle(D2D1::RectF(node.screenRect.left, node.screenRect.top, node.screenRect.left + accentWidth, node.screenRect.bottom), nodeTintBrush);
            }

            float screenWidth = node.screenRect.right - node.screenRect.left;
            float screenHeight = node.screenRect.bottom - node.screenRect.top;
            float padX = (std::max)(1.5f, (std::min)(10.0f, screenWidth * 0.05f));
            float padY = (std::max)(1.0f, (std::min)(6.0f, screenHeight * 0.14f));
            bool importantNode = selected || hovered || highlighted;
            bool hasBuildOverlay = node.buildInProgress || node.buildDurationMs > 0;

            bool showTitle = screenWidth >= 22.0f && screenHeight >= 8.0f &&
                             (zoom_ >= 0.45f || importantNode || nodes_.size() <= 50);
            bool showSubtitle = screenWidth >= 120.0f && screenHeight >= 34.0f &&
                                (zoom_ >= 1.05f || selected || hovered || hasBuildOverlay);

            D2D1_RECT_F innerRect = D2D1::RectF(
                node.screenRect.left + padX,
                node.screenRect.top + padY,
                node.screenRect.right - padX,
                node.screenRect.bottom - padY);

            std::wstring subtitleText = node.subtitle;
            if (node.buildInProgress)
                subtitleText = L"building...";
            else if (node.buildDurationMs > 0)
            {
                std::wstring durationLabel = FormatDurationLabel(node.buildDurationMs);
                if (subtitleText.empty() || subtitleText == L"/")
                    subtitleText = durationLabel;
                else
                    subtitleText += L"  •  " + durationLabel;
            }

            IDWriteTextFormat *titleFmt = (screenWidth < 96.0f || screenHeight < 22.0f || zoom_ < 0.9f) ? smallFmt : bodyFmt;
            if (showTitle && titleFmt && textBrush)
            {
                ctx->PushAxisAlignedClip(innerRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                D2D1_RECT_F titleRect = innerRect;
                if (showSubtitle)
                {
                    titleRect.bottom = titleRect.top + (screenHeight * 0.45f);
                }
                ctx->DrawTextW(node.title.c_str(), (UINT32)node.title.size(), titleFmt, titleRect, textBrush);
                ctx->PopAxisAlignedClip();
            }
            if (showSubtitle && smallFmt && mutedBrush)
            {
                ctx->PushAxisAlignedClip(innerRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                D2D1_RECT_F subtitleRect = innerRect;
                subtitleRect.top = innerRect.top + (screenHeight * 0.42f);
                ctx->DrawTextW(subtitleText.c_str(), (UINT32)subtitleText.size(), smallFmt, subtitleRect, mutedBrush);
                ctx->PopAxisAlignedClip();
            }

        }
    }

    ctx->PopAxisAlignedClip();
    ctx->PopAxisAlignedClip();

    if (bodyFmt)
        bodyFmt->Release();
    if (smallFmt)
        smallFmt->Release();
    if (ellipsisSign)
        ellipsisSign->Release();
    if (textBrush)
        textBrush->Release();
    if (mutedBrush)
        mutedBrush->Release();
    if (cardBorderBrush)
        cardBorderBrush->Release();
    if (edgeOutgoingBrush)
        edgeOutgoingBrush->Release();
    if (edgeIncomingBrush)
        edgeIncomingBrush->Release();
    if (edgeOutgoingMutedBrush)
        edgeOutgoingMutedBrush->Release();
    if (edgeIncomingMutedBrush)
        edgeIncomingMutedBrush->Release();
    if (gridBrush)
        gridBrush->Release();
    if (selectionBrush)
        selectionBrush->Release();
    if (nodeFillBrush)
        nodeFillBrush->Release();
    if (nodeTintBrush)
        nodeTintBrush->Release();
}

void CodeMapTabView::OnMouseMove(HWND hwnd, POINT clientPoint)
{
    lastMousePoint_ = clientPoint;

    if (draggingCanvas_)
    {
        panX_ = dragStartPanX_ + (float)(clientPoint.x - dragStartPoint_.x);
        panY_ = dragStartPanY_ + (float)(clientPoint.y - dragStartPoint_.y);
        UpdateScreenRects();
        InvalidateGraphRect(hwnd, graphRect_);
        return;
    }

    if (!PointInRect(clientPoint, graphRect_))
    {
        if (!hoveredNodeKey_.empty())
        {
            hoveredNodeKey_.clear();
            InvalidateGraphRect(hwnd, graphRect_);
        }
        return;
    }

    bool changed = false;
    int nodeIndex = HitTestNode(clientPoint);
    std::wstring newHovered = nodeIndex >= 0 ? nodes_[(size_t)nodeIndex].key : L"";
    if (newHovered != hoveredNodeKey_)
    {
        hoveredNodeKey_ = std::move(newHovered);
        changed = true;
    }

    if (changed)
        InvalidateGraphRect(hwnd, graphRect_);
}

void CodeMapTabView::OnLeftButtonDown(HWND hwnd, POINT clientPoint)
{
    lastMousePoint_ = clientPoint;

    int nodeIndex = HitTestNode(clientPoint);
    if (nodeIndex >= 0)
    {
        const GraphNode &node = nodes_[(size_t)nodeIndex];
        manualFocus_ = true;
        focusedFile_ = node.filePath;
        selectedNodeKey_ = node.key;
        UpdateFocusHighlights();
        auto *heapPath = new std::wstring(node.filePath);
        PostMessageW(hwnd, kOpenFileMessage, (WPARAM)-1, (LPARAM)heapPath);
        return;
    }

    if (PointInRect(clientPoint, graphRect_))
    {
        draggingCanvas_ = true;
        dragStartPoint_ = clientPoint;
        dragStartPanX_ = panX_;
        dragStartPanY_ = panY_;
        SetCapture(hwnd);
    }
}

void CodeMapTabView::OnLeftButtonUp(HWND hwnd)
{
    if (draggingCanvas_)
    {
        draggingCanvas_ = false;
        if (GetCapture() == hwnd)
            ReleaseCapture();
        InvalidateGraphRect(hwnd, graphRect_);
    }
}

void CodeMapTabView::OnMouseWheel(HWND hwnd, int delta)
{
    if (!PointInRect(lastMousePoint_, graphRect_))
        return;

    float oldZoom = zoom_;
    float factor = delta > 0 ? 1.12f : (1.0f / 1.12f);
    zoom_ = ClampFloat(zoom_ * factor, kMinZoom, kMaxZoom);
    if (std::abs(zoom_ - oldZoom) < 0.001f)
        return;

    float centerX = (graphRect_.left + graphRect_.right) * 0.5f;
    float centerY = (graphRect_.top + graphRect_.bottom) * 0.5f;
    float relX = (float)lastMousePoint_.x - centerX - panX_;
    float relY = (float)lastMousePoint_.y - centerY - panY_;
    if (oldZoom > 0.0f)
    {
        panX_ -= relX * ((zoom_ / oldZoom) - 1.0f);
        panY_ -= relY * ((zoom_ / oldZoom) - 1.0f);
    }

    UpdateScreenRects();
    InvalidateGraphRect(hwnd, graphRect_);
}
