#include "CodeMapPanel.h"

#include "core/explorer/Explorer.h"
#include "helpers/window_helpers.h"
#include "lsp/LspManager.h"
#include "ui/panels/terminal/TerminalPanel.h"
#include "ui/theme/Theme.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <system_error>

namespace
{
    constexpr float kSummaryHeight = 18.0f;
    constexpr float kCurrentCardHeight = 62.0f;
    constexpr float kSectionTopGap = 14.0f;
    constexpr float kNodeIndent = 18.0f;
    constexpr float kNodeDotRadius = 3.5f;

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

    bool IsVirtualWorkspacePath(const std::wstring &path)
    {
        return !path.empty() && path.rfind(L"__", 0) == 0;
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
}

CodeMapPanel::CodeMapPanel()
    : Panel(PanelId::CodeMap)
{
    config_ = PanelConfig(
        PanelId::CodeMap,
        L"assets/ressource/icons/architecture.svg",
        L"Code Map",
        true,
        false,
        3);
    title_ = L"CODE MAP";
    state_.logicalWidth = 340;
    state_.minWidth = 240;
    state_.maxWidth = 680;
}

void CodeMapPanel::Initialize()
{
    RefreshGraphData();
}

std::wstring CodeMapPanel::NormalizePath(const std::wstring &path)
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

std::wstring CodeMapPanel::MakeCompareKey(const std::wstring &path)
{
    return ToLowerCopy(path);
}

bool CodeMapPanel::IsSamePath(const std::wstring &a, const std::wstring &b)
{
    return MakeCompareKey(NormalizePath(a)) == MakeCompareKey(NormalizePath(b));
}

std::wstring CodeMapPanel::GetDisplayTitle(const std::wstring &path) const
{
    if (path.empty())
        return L"(unknown)";
    return std::filesystem::path(path).filename().wstring();
}

std::wstring CodeMapPanel::GetDisplaySubtitle(const std::wstring &path) const
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

void CodeMapPanel::ApplyBuildStatus(NodeItem &item, bool buildSucceeded) const
{
    if (item.filePath.empty())
        return;

    std::vector<Lsp::Diagnostic> diagnostics = Lsp::LspManager::Instance().GetDiagnostics(item.filePath);
    for (const auto &diag : diagnostics)
    {
        if (diag.severity == Lsp::DiagnosticSeverity::Error)
            item.hasErrors = true;
        else if (diag.severity == Lsp::DiagnosticSeverity::Warning)
            item.hasWarnings = true;
    }

    int buildSeverity = GetTerminalPanel().GetBuildIssueSeverity(item.filePath);
    if (buildSeverity >= 2)
        item.hasErrors = true;
    else if (buildSeverity == 1)
        item.hasWarnings = true;

    auto buildProgress = GetTerminalPanel().GetBuildFileProgress(item.filePath);
    item.buildInProgress = buildProgress.inProgress;
    item.buildDurationMs = buildProgress.durationMs;

    if (buildProgress.completed && buildSucceeded && !item.hasErrors && !item.hasWarnings)
        item.buildClean = true;
}

std::wstring CodeMapPanel::BuildSubtitleWithStatus(const NodeItem &item) const
{
    std::wstring subtitle = item.subtitle;
    std::wstring status;

    if (item.buildInProgress)
        status = L"building...";
    else if (item.buildDurationMs > 0)
        status = FormatDurationLabel(item.buildDurationMs);

    if (status.empty())
        return subtitle;
    if (subtitle.empty() || subtitle == L"/")
        return status;
    return status + L"  |  " + subtitle;
}

void CodeMapPanel::RefreshGraphData()
{
    includesByFile_ = Lsp::LspManager::Instance().GetFileIncludes();
    projectRoot_ = NormalizePath(Lsp::LspManager::Instance().GetProjectRoot());

    std::wstring rawActivePath = GetExplorerManager().GetState().activePath;
    std::wstring activePath = IsVirtualWorkspacePath(rawActivePath) ? std::wstring() : NormalizePath(rawActivePath);
    if (!activePath.empty())
        focusedFile_ = activePath;

    if (focusedFile_.empty() && !includesByFile_.empty())
        focusedFile_ = includesByFile_.begin()->first;

    includeNodes_.clear();
    incomingNodes_.clear();

    if (focusedFile_.empty())
        return;

    bool buildSucceeded = GetTerminalPanel().DidLastBuildSucceed();

    auto addNode = [&](std::vector<NodeItem> &target, const std::wstring &path)
    {
        NodeItem item;
        item.filePath = path;
        item.title = GetDisplayTitle(path);
        item.subtitle = GetDisplaySubtitle(path);
        ApplyBuildStatus(item, buildSucceeded);
        target.push_back(std::move(item));
    };

    auto it = includesByFile_.find(focusedFile_);
    if (it != includesByFile_.end())
    {
        for (const std::wstring &inc : it->second)
            addNode(includeNodes_, inc);
    }

    const std::wstring focusedKey = MakeCompareKey(focusedFile_);
    for (const auto &entry : includesByFile_)
    {
        for (const std::wstring &inc : entry.second)
        {
            if (MakeCompareKey(inc) == focusedKey)
            {
                addNode(incomingNodes_, entry.first);
                break;
            }
        }
    }

    auto sortNodes = [](std::vector<NodeItem> &nodes)
    {
        std::sort(nodes.begin(), nodes.end(), [](const NodeItem &a, const NodeItem &b) {
            if (a.title != b.title)
                return a.title < b.title;
            return a.filePath < b.filePath;
        });
    };

    sortNodes(includeNodes_);
    sortNodes(incomingNodes_);
}

void CodeMapPanel::RefreshGraphDataIfNeeded()
{
    if (!visible_)
        return;

    std::wstring rawActivePath = GetExplorerManager().GetState().activePath;
    std::wstring activePath = IsVirtualWorkspacePath(rawActivePath) ? std::wstring() : NormalizePath(rawActivePath);
    std::wstring activeKey = MakeCompareKey(activePath);
    DWORD now = GetTickCount();
    bool activeChanged = activeKey != lastObservedActiveFile_;
    bool periodicRefresh = !graphDataValid_ || (now - lastRefreshTick_) >= 1000;
    if (!activeChanged && !periodicRefresh)
        return;

    RefreshGraphData();
    lastObservedActiveFile_ = activeKey;
    lastRefreshTick_ = now;
    graphDataValid_ = true;
}

float CodeMapPanel::ComputeContentHeight() const
{
    float h = 0.0f;
    h += kSummaryHeight + 8.0f;
    h += kCurrentCardHeight;
    h += kSectionTopGap;
    h += sectionHeaderHeight_;
    h += includeNodes_.empty() ? nodeHeight_ : (float)includeNodes_.size() * (nodeHeight_ + nodeGap_) - nodeGap_;
    h += kSectionTopGap;
    h += sectionHeaderHeight_;
    h += incomingNodes_.empty() ? nodeHeight_ : (float)incomingNodes_.size() * (nodeHeight_ + nodeGap_) - nodeGap_;
    h += 24.0f;
    return h;
}

void CodeMapPanel::UpdateLayout(HWND hwnd)
{
    UINT dpi = win32_get_dpi_for_window(hwnd);
    int sidebarWidth = win32_dpi_scale(52, dpi);
    UpdateBaseLayout(hwnd, static_cast<float>(sidebarWidth));

    const float contentLeft = state_.leftEdge + state_.leftPadding;
    const float contentRight = state_.rightEdge - state_.leftPadding;
    const float contentTop = state_.topEdge + state_.titleHeight + 8.0f;
    const float contentBottom = state_.bottomEdge - 6.0f;
    contentRect_ = D2D1::RectF(contentLeft, contentTop, contentRight, contentBottom);

    RefreshGraphDataIfNeeded();
    contentHeight_ = ComputeContentHeight();
    scrollbar_.UpdateLayout(contentRect_.left, contentRect_.top,
                            contentRect_.right - contentRect_.left,
                            contentRect_.bottom - contentRect_.top,
                            contentHeight_);
}

int CodeMapPanel::HitTestNode(POINT clientPoint) const
{
    auto hitNodes = [&](const std::vector<NodeItem> &nodes, int baseIndex) -> int
    {
        for (size_t i = 0; i < nodes.size(); ++i)
        {
            const auto &rect = nodes[i].rect;
            if (clientPoint.x >= rect.left && clientPoint.x <= rect.right &&
                clientPoint.y >= rect.top && clientPoint.y <= rect.bottom)
            {
                return baseIndex + (int)i;
            }
        }
        return -1;
    };

    int hit = hitNodes(includeNodes_, 0);
    if (hit >= 0)
        return hit;
    return hitNodes(incomingNodes_, (int)includeNodes_.size());
}

void CodeMapPanel::Draw(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd)
{
    if (!visible_ || state_.physicalWidth <= 0)
        return;
    (void)hwnd;
    contentHeight_ = ComputeContentHeight();
    scrollbar_.UpdateLayout(contentRect_.left, contentRect_.top,
                            contentRect_.right - contentRect_.left,
                            contentRect_.bottom - contentRect_.top,
                            contentHeight_);

    ctx->PushAxisAlignedClip(
        D2D1::RectF(state_.leftEdge, state_.topEdge, state_.rightEdge, state_.bottomEdge),
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    DrawBackground(ctx);
    DrawTitle(ctx, dwrite);

    IDWriteTextFormat *titleFmt = nullptr;
    IDWriteTextFormat *subFmt = nullptr;
    IDWriteTextFormat *sectionFmt = nullptr;
    dwrite->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             13.0f, L"en-us", &titleFmt);
    dwrite->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             11.0f, L"en-us", &subFmt);
    dwrite->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             11.0f, L"en-us", &sectionFmt);
    if (titleFmt)
    {
        titleFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        titleFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        titleFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    if (subFmt)
    {
        subFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        subFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        subFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    if (sectionFmt)
    {
        sectionFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        sectionFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        sectionFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    ID2D1SolidColorBrush *textBrush = nullptr;
    ID2D1SolidColorBrush *mutedBrush = nullptr;
    ID2D1SolidColorBrush *cardBrush = nullptr;
    ID2D1SolidColorBrush *hoverBrush = nullptr;
    ID2D1SolidColorBrush *borderBrush = nullptr;
    ID2D1SolidColorBrush *accentBrush = nullptr;
    ID2D1SolidColorBrush *includeBrush = nullptr;
    ID2D1SolidColorBrush *incomingBrush = nullptr;
    ID2D1SolidColorBrush *emptyBrush = nullptr;

    const UI::Theme::Palette &palette = UI::Theme::GetPalette();
    ctx->CreateSolidColorBrush(UI::Theme::PrimaryText(), &textBrush);
    ctx->CreateSolidColorBrush(UI::Theme::MutedText(), &mutedBrush);
    ctx->CreateSolidColorBrush(palette.inputBackground, &cardBrush);
    ctx->CreateSolidColorBrush(palette.explorerRowHover, &hoverBrush);
    ctx->CreateSolidColorBrush(UI::Theme::ChromeBorder(), &borderBrush);
    ctx->CreateSolidColorBrush(UI::Theme::Accent(), &accentBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.33f, 0.62f, 0.95f, 1.0f), &includeBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.38f, 0.78f, 0.56f, 1.0f), &incomingBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.58f, 0.60f, 0.64f, 1.0f), &emptyBrush);

    D2D1_RECT_F clip = contentRect_;
    ctx->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    float y = contentRect_.top - scrollbar_.GetScrollOffset();
    summaryRect_ = D2D1::RectF(contentRect_.left, y, contentRect_.right, y + kSummaryHeight);

    std::wstring summary;
    if (!projectRoot_.empty())
    {
        summary = std::to_wstring((unsigned long long)includesByFile_.size()) + L" indexed files";
        if (!focusedFile_.empty())
        {
            summary += L"  |  " + std::to_wstring((unsigned long long)includeNodes_.size()) + L" includes";
            summary += L"  |  " + std::to_wstring((unsigned long long)incomingNodes_.size()) + L" incoming";
        }
    }
    else
    {
        summary = L"Open a project to build a simple include map";
    }
    if (subFmt && mutedBrush)
        ctx->DrawTextW(summary.c_str(), (UINT32)summary.size(), subFmt, summaryRect_, mutedBrush);

    y += kSummaryHeight + 8.0f;
    focusRect_ = D2D1::RectF(contentRect_.left, y, contentRect_.right - (scrollbar_.IsVisible() ? 14.0f : 0.0f), y + kCurrentCardHeight);

    if (cardBrush)
        ctx->FillRoundedRectangle(D2D1::RoundedRect(focusRect_, 8.0f, 8.0f), cardBrush);
    if (borderBrush)
        ctx->DrawRoundedRectangle(D2D1::RoundedRect(focusRect_, 8.0f, 8.0f), borderBrush, 1.0f);
    if (accentBrush)
        ctx->FillRectangle(D2D1::RectF(focusRect_.left, focusRect_.top, focusRect_.left + 4.0f, focusRect_.bottom), accentBrush);

    std::wstring focusTitle = focusedFile_.empty() ? L"No active file" : GetDisplayTitle(focusedFile_);
    NodeItem focusedItem;
    if (!focusedFile_.empty())
    {
        focusedItem.filePath = focusedFile_;
        focusedItem.title = focusTitle;
        focusedItem.subtitle = GetDisplaySubtitle(focusedFile_);
        ApplyBuildStatus(focusedItem, GetTerminalPanel().DidLastBuildSucceed());
    }

    std::wstring focusSubtitle = focusedFile_.empty()
        ? L"Select a project file to explore includes"
        : BuildSubtitleWithStatus(focusedItem);
    D2D1_RECT_F focusTitleRect = D2D1::RectF(focusRect_.left + 12.0f, focusRect_.top + 10.0f, focusRect_.right - 12.0f, focusRect_.top + 30.0f);
    D2D1_RECT_F focusSubRect = D2D1::RectF(focusRect_.left + 12.0f, focusRect_.top + 31.0f, focusRect_.right - 12.0f, focusRect_.bottom - 8.0f);
    if (titleFmt && textBrush)
        ctx->DrawTextW(focusTitle.c_str(), (UINT32)focusTitle.size(), titleFmt, focusTitleRect, textBrush);
    if (subFmt && mutedBrush)
        ctx->DrawTextW(focusSubtitle.c_str(), (UINT32)focusSubtitle.size(), subFmt, focusSubRect, mutedBrush);

    y += kCurrentCardHeight + kSectionTopGap;

    auto drawSection = [&](const wchar_t *label,
                           std::vector<NodeItem> &nodes,
                           ID2D1SolidColorBrush *lineBrush,
                           int indexBase)
    {
        D2D1_RECT_F sectionRect = D2D1::RectF(contentRect_.left, y, contentRect_.right, y + sectionHeaderHeight_);
        if (sectionFmt && mutedBrush)
            ctx->DrawTextW(label, (UINT32)wcslen(label), sectionFmt, sectionRect, mutedBrush);
        y += sectionHeaderHeight_;

        float nodeRight = contentRect_.right - (scrollbar_.IsVisible() ? 14.0f : 0.0f);
        if (nodes.empty())
        {
            D2D1_RECT_F emptyRect = D2D1::RectF(contentRect_.left + 4.0f, y, nodeRight, y + nodeHeight_);
            if (emptyBrush && subFmt)
                ctx->DrawTextW(L"No relationships", 16, subFmt, emptyRect, emptyBrush);
            y += nodeHeight_ + kSectionTopGap;
            return;
        }

        const float stemX = contentRect_.left + kNodeIndent;
        const float cardLeft = stemX + 12.0f;
        const float centerX = (focusRect_.left + focusRect_.right) * 0.5f;
        const float centerY = focusRect_.bottom;

        for (size_t i = 0; i < nodes.size(); ++i)
        {
            NodeItem &node = nodes[i];
            float nodeTop = y;
            float nodeBottom = y + nodeHeight_;
            float dotY = nodeTop + nodeHeight_ * 0.5f;

            node.rect = D2D1::RectF(cardLeft, nodeTop, nodeRight, nodeBottom);

            if (lineBrush)
            {
                ctx->DrawLine(D2D1::Point2F(centerX, centerY),
                              D2D1::Point2F(stemX, dotY),
                              lineBrush,
                              1.0f);
                ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(stemX, dotY), kNodeDotRadius, kNodeDotRadius), lineBrush);
            }

            bool hovered = (hoveredNodeIndex_ == indexBase + (int)i);
            bool selected = (selectedNodeIndex_ == indexBase + (int)i);
            ID2D1SolidColorBrush *bg = (hovered || selected) ? hoverBrush : cardBrush;

            if (bg)
                ctx->FillRoundedRectangle(D2D1::RoundedRect(node.rect, 8.0f, 8.0f), bg);
            if (borderBrush)
                ctx->DrawRoundedRectangle(D2D1::RoundedRect(node.rect, 8.0f, 8.0f), borderBrush, 1.0f);

            D2D1_RECT_F titleRect = D2D1::RectF(node.rect.left + 10.0f, node.rect.top + 6.0f, node.rect.right - 10.0f, node.rect.top + 24.0f);
            D2D1_RECT_F subRect = D2D1::RectF(node.rect.left + 10.0f, node.rect.top + 23.0f, node.rect.right - 10.0f, node.rect.bottom - 6.0f);
            std::wstring subtitle = BuildSubtitleWithStatus(node);
            if (subFmt && textBrush)
                ctx->DrawTextW(node.title.c_str(), (UINT32)node.title.size(), subFmt, titleRect, textBrush);
            if (subFmt && mutedBrush)
                ctx->DrawTextW(subtitle.c_str(), (UINT32)subtitle.size(), subFmt, subRect, mutedBrush);

            y += nodeHeight_ + nodeGap_;
        }

        y -= nodeGap_;
        y += kSectionTopGap;
    };

    drawSection(L"Includes", includeNodes_, includeBrush, 0);
    drawSection(L"Included By", incomingNodes_, incomingBrush, (int)includeNodes_.size());

    ctx->PopAxisAlignedClip();
    scrollbar_.Draw(ctx);
    DrawRightBorder(ctx);
    ctx->PopAxisAlignedClip();

    if (titleFmt)
        titleFmt->Release();
    if (subFmt)
        subFmt->Release();
    if (sectionFmt)
        sectionFmt->Release();
    if (textBrush)
        textBrush->Release();
    if (mutedBrush)
        mutedBrush->Release();
    if (cardBrush)
        cardBrush->Release();
    if (hoverBrush)
        hoverBrush->Release();
    if (borderBrush)
        borderBrush->Release();
    if (accentBrush)
        accentBrush->Release();
    if (includeBrush)
        includeBrush->Release();
    if (incomingBrush)
        incomingBrush->Release();
    if (emptyBrush)
        emptyBrush->Release();
}

void CodeMapPanel::OnMouseMove(HWND hwnd, POINT clientPoint)
{
    if (HandleResizeMouseMove(hwnd, clientPoint))
        return;

    bool changed = false;
    if (scrollbar_.OnMouseMove(clientPoint))
        changed = true;

    int hovered = HitTestNode(clientPoint);
    if (hovered != hoveredNodeIndex_)
    {
        hoveredNodeIndex_ = hovered;
        changed = true;
    }

    if (changed)
        InvalidateRect(hwnd, nullptr, FALSE);
}

void CodeMapPanel::OnLeftButtonDown(HWND hwnd, POINT clientPoint)
{
    if (HandleResizeLeftButtonDown(hwnd, clientPoint))
        return;

    if (scrollbar_.OnLeftButtonDown(clientPoint))
    {
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    int hit = HitTestNode(clientPoint);
    if (hit < 0)
        return;

    selectedNodeIndex_ = hit;
    const NodeItem *node = nullptr;
    if (hit < (int)includeNodes_.size())
        node = &includeNodes_[(size_t)hit];
    else
    {
        int incomingIndex = hit - (int)includeNodes_.size();
        if (incomingIndex >= 0 && incomingIndex < (int)incomingNodes_.size())
            node = &incomingNodes_[(size_t)incomingIndex];
    }

    if (node && !node->filePath.empty())
    {
        auto *heapPath = new std::wstring(node->filePath);
        PostMessageW(hwnd, WM_USER + 100, 0, (LPARAM)heapPath);
    }

    InvalidateRect(hwnd, nullptr, FALSE);
}

void CodeMapPanel::OnLeftButtonUp(HWND hwnd)
{
    if (HandleResizeLeftButtonUp(hwnd))
        return;

    if (scrollbar_.OnLeftButtonUp())
    {
        ReleaseCapture();
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

void CodeMapPanel::OnMouseWheel(HWND hwnd, int delta)
{
    if (scrollbar_.OnMouseWheel(delta))
        InvalidateRect(hwnd, nullptr, FALSE);
}
