#include "GitPanel.h"
#include "GitDiffDecorations.h"

#include "core/explorer/Explorer.h"
#include "helpers/window_helpers.h"
#include "ui/components/input/InputTheme.h"
#include "utils/auth/GitHubAuth.h"

#include <git2.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace
{
std::mutex g_libgit2Mutex;
int g_libgit2RefCount = 0;

char StatusCharIndex(git_status_t status)
{
    if (status & GIT_STATUS_CONFLICTED)
        return 'U';
    if (status & GIT_STATUS_INDEX_NEW)
        return 'A';
    if (status & GIT_STATUS_INDEX_MODIFIED)
        return 'M';
    if (status & GIT_STATUS_INDEX_DELETED)
        return 'D';
    if (status & GIT_STATUS_INDEX_RENAMED)
        return 'R';
    if (status & GIT_STATUS_INDEX_TYPECHANGE)
        return 'T';
    return ' ';
}

char StatusCharWorktree(git_status_t status)
{
    if (status & GIT_STATUS_CONFLICTED)
        return 'U';
    if (status & GIT_STATUS_WT_NEW)
        return '?';
    if (status & GIT_STATUS_WT_MODIFIED)
        return 'M';
    if (status & GIT_STATUS_WT_DELETED)
        return 'D';
    if (status & GIT_STATUS_WT_RENAMED)
        return 'R';
    if (status & GIT_STATUS_WT_TYPECHANGE)
        return 'T';
    if (status & GIT_STATUS_WT_UNREADABLE)
        return '!';
    return ' ';
}

struct DiffCollectContext
{
    std::vector<int> *addedLines = nullptr;
    std::vector<int> *deletedLines = nullptr;
    GitDiffDecorations::SplitViewData *splitData = nullptr;
    std::deque<std::wstring> pendingDeleted;
};

struct PushCredentialContext
{
    std::string token;
};

int AcquirePushCredentials(git_credential **out,
                           const char * /*url*/,
                           const char * /*username_from_url*/,
                           unsigned int /*allowed_types*/,
                           void *payload)
{
    if (!out)
        return -1;

    PushCredentialContext *ctx = static_cast<PushCredentialContext *>(payload);
    if (!ctx || ctx->token.empty())
        return -1;
    return git_credential_userpass_plaintext_new(out, "x-access-token", ctx->token.c_str());
}

std::wstring Utf8BytesToWide(const char *bytes, size_t len)
{
    if (!bytes || len == 0)
        return {};

    int wideLen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, (int)len, nullptr, 0);
    if (wideLen > 0)
    {
        std::wstring out((size_t)wideLen, L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, (int)len, out.data(), wideLen);
        return out;
    }

    wideLen = MultiByteToWideChar(CP_UTF8, 0, bytes, (int)len, nullptr, 0);
    if (wideLen > 0)
    {
        std::wstring out((size_t)wideLen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, bytes, (int)len, out.data(), wideLen);
        return out;
    }

    wideLen = MultiByteToWideChar(CP_ACP, 0, bytes, (int)len, nullptr, 0);
    if (wideLen <= 0)
        return {};
    std::wstring out((size_t)wideLen, L'\0');
    MultiByteToWideChar(CP_ACP, 0, bytes, (int)len, out.data(), wideLen);
    return out;
}

std::wstring StripPatchLineEndings(const std::wstring &line)
{
    size_t end = line.size();
    while (end > 0 && (line[end - 1] == L'\r' || line[end - 1] == L'\n'))
        --end;
    return line.substr(0, end);
}

void FlushPendingDeletedRows(DiffCollectContext &ctx)
{
    if (!ctx.splitData)
    {
        ctx.pendingDeleted.clear();
        return;
    }

    while (!ctx.pendingDeleted.empty())
    {
        GitDiffDecorations::SplitRow row;
        row.leftText = std::move(ctx.pendingDeleted.front());
        row.hasLeft = true;
        row.hasRight = false;
        row.leftDeleted = true;
        ctx.pendingDeleted.pop_front();
        ctx.splitData->rows.push_back(std::move(row));
    }
}

int CollectDiffLineNumbers(const git_diff_delta *, const git_diff_hunk *, const git_diff_line *line, void *payload)
{
    if (!line || !payload)
        return 0;
    DiffCollectContext *ctx = static_cast<DiffCollectContext *>(payload);
    if (!ctx)
        return 0;

    if (line->origin == GIT_DIFF_LINE_ADDITION && line->new_lineno > 0 && ctx->addedLines)
    {
        ctx->addedLines->push_back((int)line->new_lineno - 1);
    }
    else if (line->origin == GIT_DIFF_LINE_DELETION && ctx->deletedLines)
    {
        int lineNum = -1;
        if (line->new_lineno > 0)
            lineNum = (int)line->new_lineno - 1;
        else if (line->old_lineno > 0)
            lineNum = (int)line->old_lineno - 1;
        if (lineNum >= 0)
            ctx->deletedLines->push_back(lineNum);
    }

    if (ctx->splitData)
    {
        const char origin = line->origin;
        const bool isAddition = (origin == GIT_DIFF_LINE_ADDITION);
        const bool isDeletion = (origin == GIT_DIFF_LINE_DELETION);
        const bool isContext = (origin == GIT_DIFF_LINE_CONTEXT);

        if (isAddition || isDeletion || isContext)
        {
            std::wstring text = StripPatchLineEndings(Utf8BytesToWide(line->content, line->content_len));

            if (isDeletion)
            {
                ctx->pendingDeleted.push_back(std::move(text));
            }
            else if (isAddition)
            {
                GitDiffDecorations::SplitRow row;
                if (!ctx->pendingDeleted.empty())
                {
                    row.leftText = std::move(ctx->pendingDeleted.front());
                    ctx->pendingDeleted.pop_front();
                    row.hasLeft = true;
                    row.leftDeleted = true;
                }
                row.hasRight = true;
                row.rightText = std::move(text);
                row.rightAdded = true;
                ctx->splitData->rows.push_back(std::move(row));
            }
            else
            {
                FlushPendingDeletedRows(*ctx);
                GitDiffDecorations::SplitRow row;
                row.leftText = text;
                row.rightText = std::move(text);
                row.hasLeft = true;
                row.hasRight = true;
                ctx->splitData->rows.push_back(std::move(row));
            }
        }
        else
        {
            FlushPendingDeletedRows(*ctx);
        }
    }

    return 0;
}

void SortUnique(std::vector<int> &values)
{
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}
}

GitPanel::GitPanel()
    : Panel(PanelId::Git)
{
    config_ = PanelConfig(
        PanelId::Git,
        L"assets/ressource/icons/git.svg",
        L"Source Control",
        true,
        false,
        2);
    title_ = L"SOURCE CONTROL";

    ApplyInputTheme(commitMessageInput_, L"Commit message");

    commitMessageInput_.onSubmit = [this]()
    {
        ExecuteQuickAction(quickActionPrimaryIndex_);
    };

    commitMessageInput_.onEscape = [this]()
    {
        commitMessageInput_.SetFocused(false);
    };

    {
        std::lock_guard<std::mutex> lock(g_libgit2Mutex);
        if (g_libgit2RefCount == 0)
        {
            int rc = git_libgit2_init();
            libgit2Ready_ = (rc >= 0);
        }
        else
        {
            libgit2Ready_ = true;
        }
        if (libgit2Ready_)
            ++g_libgit2RefCount;
    }

    if (!libgit2Ready_)
        lastError_ = L"libgit2 initialization failed.";
}

GitPanel::~GitPanel()
{
    std::lock_guard<std::mutex> lock(g_libgit2Mutex);
    if (libgit2Ready_ && g_libgit2RefCount > 0)
    {
        --g_libgit2RefCount;
        if (g_libgit2RefCount == 0)
            git_libgit2_shutdown();
    }
}

void GitPanel::Initialize()
{
    SyncRepoPathFromExplorer();
    RefreshStatus();
}

void GitPanel::ApplyInputTheme(TextInput &input, const std::wstring &placeholder, const std::wstring &icon)
{
    input.SetPlaceholder(placeholder);
    if (!icon.empty())
        input.SetIcon(icon);

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
}

void GitPanel::SyncRepoPathFromExplorer()
{
    if (!Trim(repoRoot_).empty())
        return;

    const std::wstring root = Trim(GetExplorerManager().GetState().rootPath);
    if (!root.empty())
        repoRoot_ = root;
}

void GitPanel::UpdateLayout(HWND hwnd)
{
    UINT dpi = win32_get_dpi_for_window(hwnd);
    int sidebarWidth = win32_dpi_scale(52, dpi);
    UpdateBaseLayout(hwnd, static_cast<float>(sidebarWidth));

    if (!visible_)
    {
        state_.physicalWidth = 0;
        state_.rightEdge = state_.leftEdge;
        wasVisibleLastLayout_ = false;
        return;
    }

    ULONGLONG now = GetTickCount64();
    bool becameVisible = !wasVisibleLastLayout_;
    bool periodicRefresh = hasAutoRefreshed_ && !commitMessageInput_.IsFocused() &&
                           (now - lastAutoRefreshTick_ >= 1200);
    if (becameVisible || !hasAutoRefreshed_ || periodicRefresh)
    {
        RefreshStatus();
        hasAutoRefreshed_ = true;
        lastAutoRefreshTick_ = now;
    }
    wasVisibleLastLayout_ = true;

    SyncRepoPathFromExplorer();

    float x0 = state_.leftEdge + state_.leftPadding;
    float x1 = state_.rightEdge - state_.leftPadding;
    float y = state_.topEdge + state_.titleHeight + 8.0f;
    const float inputH = 30.0f;
    const float gap = 8.0f;

    const float actionPrimaryW = 112.0f;
    const float actionToggleW = 24.0f;
    const float actionGap = 8.0f;
    const float minInputW = 170.0f;
    const bool showQuickActions = ((x1 - x0) >= (actionPrimaryW + actionToggleW + actionGap + minInputW));

    if (showQuickActions)
    {
        quickActionToggleRect_ = D2D1::RectF(x1 - actionToggleW, y, x1, y + inputH);
        quickActionPrimaryRect_ = D2D1::RectF(quickActionToggleRect_.left - actionPrimaryW, y,
                                              quickActionToggleRect_.left - 2.0f, y + inputH);
        float inputRight = quickActionPrimaryRect_.left - actionGap;
        commitMessageInput_.SetRect(D2D1::RectF(x0, y, inputRight, y + inputH));

        const float menuItemH = inputH;
        const float menuW = actionPrimaryW + actionToggleW + 2.0f;
        const float menuTop = y + inputH + 2.0f;
        quickActionMenuRect_ = D2D1::RectF(
            quickActionPrimaryRect_.left,
            menuTop,
            quickActionPrimaryRect_.left + menuW,
            menuTop + menuItemH * 4.0f);
    }
    else
    {
        commitMessageInput_.SetRect(D2D1::RectF(x0, y, x1, y + inputH));
        quickActionPrimaryRect_ = D2D1::RectF(0, 0, 0, 0);
        quickActionToggleRect_ = D2D1::RectF(0, 0, 0, 0);
        quickActionMenuRect_ = D2D1::RectF(0, 0, 0, 0);
        quickActionMenuOpen_ = false;
        quickActionHoveredIndex_ = -1;
    }
    y += inputH + gap;

    authStatusRect_ = D2D1::RectF(0, 0, 0, 0);
    if (!lastError_.empty())
    {
        infoRect_ = D2D1::RectF(x0, y, x1, y + 18.0f);
        y += 20.0f;
    }
    else
    {
        infoRect_ = D2D1::RectF(0, 0, 0, 0);
    }

    float changesBottom = state_.bottomEdge - 4.0f;
    if (changesBottom < y + 120.0f)
        changesBottom = y + 120.0f;
    changesRect_ = D2D1::RectF(x0, y, x1, changesBottom);

    float changesViewport = (changesRect_.bottom - changesRect_.top);
    if (changesViewport < 0.0f)
        changesViewport = 0.0f;
    float changesContent = static_cast<float>(changes_.size()) * changeRowHeight_;
    changesScrollbar_.UpdateLayout(changesRect_.left, changesRect_.top,
                                   changesRect_.right - changesRect_.left, changesViewport, changesContent);
}

void GitPanel::Draw(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd)
{
    if (!visible_ || state_.physicalWidth <= 0)
        return;

    D2D1_RECT_F clipRect = D2D1::RectF(state_.leftEdge, state_.topEdge, state_.rightEdge, state_.bottomEdge);
    ctx->PushAxisAlignedClip(clipRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    Panel::DrawBackground(ctx);
    Panel::DrawTitle(ctx, dwrite);

    commitMessageInput_.Draw(ctx, dwrite);

    if (!lastError_.empty() && infoRect_.right > infoRect_.left)
    {
        IDWriteTextFormat *metaFmt = nullptr;
        dwrite->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                 DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &metaFmt);
        if (metaFmt)
        {
            metaFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            metaFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            metaFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }

        ID2D1SolidColorBrush *warnBrush = nullptr;
        ctx->CreateSolidColorBrush(D2D1::ColorF(0.90f, 0.42f, 0.42f), &warnBrush);
        if (metaFmt && warnBrush)
            ctx->DrawTextW(lastError_.c_str(), (UINT32)lastError_.size(), metaFmt, infoRect_, warnBrush);

        if (metaFmt)
            metaFmt->Release();
        if (warnBrush)
            warnBrush->Release();
    }

    DrawChanges(ctx, dwrite, hwnd);
    changesScrollbar_.Draw(ctx);
    DrawQuickActions(ctx, dwrite);

    Panel::DrawRightBorder(ctx);
    ctx->PopAxisAlignedClip();

}

void GitPanel::DrawQuickActions(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite)
{
    if (quickActionPrimaryRect_.right <= quickActionPrimaryRect_.left ||
        quickActionToggleRect_.right <= quickActionToggleRect_.left)
        return;

    ID2D1SolidColorBrush *primaryBrush = nullptr;
    ID2D1SolidColorBrush *toggleBrush = nullptr;
    ID2D1SolidColorBrush *menuBrush = nullptr;
    ID2D1SolidColorBrush *menuHoverBrush = nullptr;
    ID2D1SolidColorBrush *menuSelectedBrush = nullptr;
    ID2D1SolidColorBrush *borderBrush = nullptr;
    ID2D1SolidColorBrush *textBrush = nullptr;
    ID2D1SolidColorBrush *chevronBrush = nullptr;

    ctx->CreateSolidColorBrush(quickActionPrimaryHovered_ ? D2D1::ColorF(0.24f, 0.53f, 0.92f, 0.96f)
                                                           : D2D1::ColorF(0.20f, 0.46f, 0.82f, 0.90f),
                               &primaryBrush);
    ctx->CreateSolidColorBrush(quickActionToggleHovered_ ? D2D1::ColorF(0.22f, 0.48f, 0.85f, 0.96f)
                                                          : D2D1::ColorF(0.18f, 0.42f, 0.75f, 0.90f),
                               &toggleBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.10f, 0.10f), &menuBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.17f, 0.17f, 0.17f), &menuHoverBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.20f, 0.45f, 0.80f, 0.26f), &menuSelectedBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.24f, 0.24f, 0.24f), &borderBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.95f, 0.95f, 0.95f), &textBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.90f, 0.90f, 0.90f), &chevronBrush);

    if (primaryBrush)
        ctx->FillRoundedRectangle(D2D1::RoundedRect(quickActionPrimaryRect_, 6.0f, 6.0f), primaryBrush);
    if (toggleBrush)
        ctx->FillRoundedRectangle(D2D1::RoundedRect(quickActionToggleRect_, 6.0f, 6.0f), toggleBrush);
    if (borderBrush)
    {
        ctx->DrawRoundedRectangle(D2D1::RoundedRect(quickActionPrimaryRect_, 6.0f, 6.0f), borderBrush, 1.0f);
        ctx->DrawRoundedRectangle(D2D1::RoundedRect(quickActionToggleRect_, 6.0f, 6.0f), borderBrush, 1.0f);
    }

    IDWriteTextFormat *buttonFmt = nullptr;
    IDWriteTextFormat *menuFmt = nullptr;
    dwrite->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                             DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &buttonFmt);
    dwrite->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                             DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &menuFmt);

    if (buttonFmt)
    {
        buttonFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        buttonFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        buttonFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    if (menuFmt)
    {
        menuFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        menuFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        menuFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    if (buttonFmt && textBrush)
    {
        const wchar_t *label = GetQuickActionLabel(quickActionPrimaryIndex_);
        ctx->DrawTextW(label, (UINT32)wcslen(label), buttonFmt, quickActionPrimaryRect_, textBrush);
    }
    if (buttonFmt && chevronBrush)
    {
        const wchar_t *chevron = L"\u25BE";
        ctx->DrawTextW(chevron, 1, buttonFmt, quickActionToggleRect_, chevronBrush);
    }

    if (quickActionMenuOpen_ &&
        quickActionMenuRect_.right > quickActionMenuRect_.left &&
        quickActionMenuRect_.bottom > quickActionMenuRect_.top)
    {
        if (menuBrush)
            ctx->FillRoundedRectangle(D2D1::RoundedRect(quickActionMenuRect_, 6.0f, 6.0f), menuBrush);
        if (borderBrush)
            ctx->DrawRoundedRectangle(D2D1::RoundedRect(quickActionMenuRect_, 6.0f, 6.0f), borderBrush, 1.0f);

        const float rowH = (quickActionMenuRect_.bottom - quickActionMenuRect_.top) / 4.0f;
        for (int i = 0; i < 4; ++i)
        {
            D2D1_RECT_F rowRect = D2D1::RectF(
                quickActionMenuRect_.left,
                quickActionMenuRect_.top + rowH * (float)i,
                quickActionMenuRect_.right,
                quickActionMenuRect_.top + rowH * (float)(i + 1));

            if (i == quickActionPrimaryIndex_ && menuSelectedBrush)
                ctx->FillRectangle(rowRect, menuSelectedBrush);
            if (i == quickActionHoveredIndex_ && menuHoverBrush)
                ctx->FillRectangle(rowRect, menuHoverBrush);

            if (menuFmt && textBrush)
            {
                D2D1_RECT_F textRect = D2D1::RectF(rowRect.left + 10.0f, rowRect.top, rowRect.right - 8.0f, rowRect.bottom);
                const wchar_t *itemLabel = GetQuickActionLabel(i);
                ctx->DrawTextW(itemLabel, (UINT32)wcslen(itemLabel), menuFmt, textRect, textBrush);
            }
        }
    }

    if (buttonFmt)
        buttonFmt->Release();
    if (menuFmt)
        menuFmt->Release();
    if (primaryBrush)
        primaryBrush->Release();
    if (toggleBrush)
        toggleBrush->Release();
    if (menuBrush)
        menuBrush->Release();
    if (menuHoverBrush)
        menuHoverBrush->Release();
    if (menuSelectedBrush)
        menuSelectedBrush->Release();
    if (borderBrush)
        borderBrush->Release();
    if (textBrush)
        textBrush->Release();
    if (chevronBrush)
        chevronBrush->Release();
}

void GitPanel::DrawChanges(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd)
{
    ID2D1SolidColorBrush *pathBrush = nullptr;
    ID2D1SolidColorBrush *hoverBrush = nullptr;
    ID2D1SolidColorBrush *selectedBrush = nullptr;
    ID2D1SolidColorBrush *statusBrush = nullptr;

    ctx->CreateSolidColorBrush(D2D1::ColorF(0.84f, 0.84f, 0.84f), &pathBrush);
    // Match Explorer hover/active row tones.
    ctx->CreateSolidColorBrush(D2D1::ColorF(30.0f / 255.0f, 30.0f / 255.0f, 30.0f / 255.0f, 1.0f), &hoverBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.18f, 0.25f, 1.0f), &selectedBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.42f, 0.78f, 0.38f), &statusBrush);
    IDWriteTextFormat *rowFmt = nullptr;
    dwrite->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                             DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &rowFmt);
    if (rowFmt)
    {
        rowFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        rowFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        rowFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    D2D1_RECT_F listClip = D2D1::RectF(changesRect_.left, changesRect_.top, changesRect_.right, changesRect_.bottom);
    ctx->PushAxisAlignedClip(listClip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    const float insetX = 4.0f;
    const float insetY = 0.0f;
    const float corner = UI::InputTheme::kCornerRadius;
    const float iconPx = (float)win32_dpi_scale((int)GetExplorerManager().GetState().iconSize, win32_get_dpi_for_window(hwnd));

    auto drawRoundedFill = [&](const D2D1_RECT_F &r, ID2D1Brush *brush)
    {
        if (!brush)
            return;
        D2D1_RECT_F rr = D2D1::RectF(std::round(r.left), std::round(r.top), std::round(r.right), std::round(r.bottom));
        D2D1_ANTIALIAS_MODE oldAA = ctx->GetAntialiasMode();
        ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        ctx->FillRoundedRectangle(D2D1::RoundedRect(rr, corner, corner), brush);
        ctx->SetAntialiasMode(oldAA);
    };

    float y = listClip.top - changesScrollbar_.GetScrollOffset();
    const float rowRightInset = changesScrollbar_.IsVisible() ? 16.0f : 4.0f;
    for (size_t i = 0; i < changes_.size(); ++i)
    {
        D2D1_RECT_F rowRect = D2D1::RectF(changesRect_.left, y, changesRect_.right - rowRightInset, y + changeRowHeight_);
        if (rowRect.bottom < listClip.top)
        {
            y += changeRowHeight_;
            continue;
        }
        if (rowRect.top > listClip.bottom)
            break;

        D2D1_RECT_F fillRect = D2D1::RectF(rowRect.left + insetX, std::round(rowRect.top) + insetY,
                                           rowRect.right - insetX, std::round(rowRect.bottom) - insetY);
        if ((int)i == selectedChangeIndex_ && selectedBrush)
            drawRoundedFill(fillRect, selectedBrush);
        else if ((int)i == hoveredChangeIndex_ && hoverBrush)
            drawRoundedFill(fillRect, hoverBrush);

        const Panels::GitChange &c = changes_[i];

        std::filesystem::path relPath(c.path);
        ExplorerItem iconItem;
        iconItem.isDirectory = false;
        iconItem.extension = relPath.extension().string();
        iconItem.name = relPath.filename().wstring();
        iconItem.fullPath = c.path;
        ID2D1Bitmap *icon = GetExplorerManager().GetIconForItemPublic(ctx, iconItem, hwnd);
        if (icon)
        {
            float iconY = std::round(rowRect.top + (rowRect.bottom - rowRect.top - iconPx) * 0.5f);
            D2D1_RECT_F iconRect = D2D1::RectF(
                std::round(rowRect.left + insetX + state_.leftPadding),
                iconY,
                std::round(rowRect.left + insetX + state_.leftPadding + iconPx),
                iconY + iconPx);
            ctx->DrawBitmap(icon, iconRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        }

        std::wstring statusToken;
        statusToken.push_back(c.indexStatus);
        statusToken.push_back(c.worktreeStatus);
        if (statusBrush)
        {
            D2D1_COLOR_F color = D2D1::ColorF(0.42f, 0.78f, 0.38f);
            if (c.indexStatus == L'D' || c.worktreeStatus == L'D')
                color = D2D1::ColorF(0.92f, 0.37f, 0.37f);
            else if (c.indexStatus == L'?' || c.worktreeStatus == L'?')
                color = D2D1::ColorF(0.73f, 0.84f, 0.40f);
            else if (c.indexStatus == L'M' || c.worktreeStatus == L'M')
                color = D2D1::ColorF(0.39f, 0.67f, 0.93f);
            statusBrush->SetColor(color);
        }

        D2D1_RECT_F statusRect = D2D1::RectF(rowRect.right - 38.0f, rowRect.top, rowRect.right - 8.0f, rowRect.bottom);
        if (rowFmt && statusBrush)
            ctx->DrawTextW(statusToken.c_str(), (UINT32)statusToken.size(), rowFmt, statusRect, statusBrush);

        D2D1_RECT_F pathRect = D2D1::RectF(rowRect.left + insetX + state_.leftPadding + iconPx + 6.0f, rowRect.top,
                                           statusRect.left - 6.0f, rowRect.bottom);
        if (rowFmt && pathBrush)
            ctx->DrawTextW(c.path.c_str(), (UINT32)c.path.size(), rowFmt, pathRect, pathBrush);

        y += changeRowHeight_;
    }

    if (changes_.empty() && rowFmt && pathBrush)
    {
        D2D1_RECT_F emptyRect = D2D1::RectF(changesRect_.left + 8.0f, listClip.top + 8.0f, changesRect_.right - 8.0f, listClip.top + 28.0f);
        const wchar_t *msg = isGitRepo_ ? L"No pending changes." : L"Open a git repository to see changes.";
        ctx->DrawTextW(msg, (UINT32)wcslen(msg), rowFmt, emptyRect, pathBrush);
    }

    ctx->PopAxisAlignedClip();

    if (rowFmt)
        rowFmt->Release();
    if (pathBrush)
        pathBrush->Release();
    if (hoverBrush)
        hoverBrush->Release();
    if (selectedBrush)
        selectedBrush->Release();
    if (statusBrush)
        statusBrush->Release();
}

void GitPanel::OnMouseMove(HWND hwnd, POINT clientPoint)
{
    bool changed = HandleResizeMouseMove(hwnd, clientPoint);
    if (state_.isResizing)
        return;
    if (state_.isHoveringResizeZone)
        return;

    bool prevPrimaryHover = quickActionPrimaryHovered_;
    bool prevToggleHover = quickActionToggleHovered_;
    int prevQuickActionHover = quickActionHoveredIndex_;

    quickActionPrimaryHovered_ = IsPointInRect(quickActionPrimaryRect_, clientPoint);
    quickActionToggleHovered_ = IsPointInRect(quickActionToggleRect_, clientPoint);
    quickActionHoveredIndex_ = quickActionMenuOpen_ ? HitTestQuickActionMenuItem(clientPoint) : -1;

    if (prevPrimaryHover != quickActionPrimaryHovered_ ||
        prevToggleHover != quickActionToggleHovered_ ||
        prevQuickActionHover != quickActionHoveredIndex_)
        changed = true;

    if (commitMessageInput_.OnMouseMove(hwnd, clientPoint))
        changed = true;

    if (changesScrollbar_.OnMouseMove(clientPoint))
        changed = true;

    int prevHover = hoveredChangeIndex_;
    if (quickActionMenuOpen_ ||
        changesScrollbar_.IsHoveringThumb() ||
        changesScrollbar_.IsHoveringTrack() ||
        changesScrollbar_.IsDragging())
        hoveredChangeIndex_ = -1;
    else
        hoveredChangeIndex_ = HitTestChange(clientPoint);
    if (prevHover != hoveredChangeIndex_)
        changed = true;

    if (changed)
        InvalidateRect(hwnd, nullptr, FALSE);
}

bool GitPanel::HandleInputClick(HWND hwnd, POINT pt)
{
    if (commitMessageInput_.HitTest(pt))
    {
        commitMessageInput_.SetFocused(true);
        commitMessageInput_.OnLeftButtonDown(hwnd, pt);
        return true;
    }
    return false;
}

void GitPanel::OnLeftButtonDown(HWND hwnd, POINT clientPoint)
{
    if (HandleResizeLeftButtonDown(hwnd, clientPoint))
        return;

    bool hitPrimaryAction = IsPointInRect(quickActionPrimaryRect_, clientPoint);
    bool hitToggleAction = IsPointInRect(quickActionToggleRect_, clientPoint);
    if (hitPrimaryAction)
    {
        quickActionMenuOpen_ = false;
        quickActionHoveredIndex_ = -1;
        ExecuteQuickAction(quickActionPrimaryIndex_);
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    if (hitToggleAction)
    {
        quickActionMenuOpen_ = !quickActionMenuOpen_;
        quickActionHoveredIndex_ = quickActionMenuOpen_ ? HitTestQuickActionMenuItem(clientPoint) : -1;
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    if (quickActionMenuOpen_)
    {
        int item = HitTestQuickActionMenuItem(clientPoint);
        if (item >= 0)
        {
            quickActionPrimaryIndex_ = item;
            quickActionMenuOpen_ = false;
            quickActionHoveredIndex_ = -1;
            ExecuteQuickAction(item);
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }

        quickActionMenuOpen_ = false;
        quickActionHoveredIndex_ = -1;
    }

    if (changesScrollbar_.OnLeftButtonDown(clientPoint))
    {
        capturedScrollbar_ = true;
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    if (HandleInputClick(hwnd, clientPoint))
    {
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    UnfocusInputs();

    int hit = HitTestChange(clientPoint);
    if (hit >= 0)
    {
        selectedChangeIndex_ = hit;

        if (hit < (int)changes_.size() && !repoRoot_.empty())
        {
            std::filesystem::path fullPath = std::filesystem::path(repoRoot_) /
                                             std::filesystem::path(changes_[(size_t)hit].path);
            fullPath = fullPath.lexically_normal();
            std::vector<int> addedLines;
            std::vector<int> deletedLines;
            GitDiffDecorations::SplitViewData splitData;
            if (BuildDiffViewForChange(changes_[(size_t)hit], addedLines, deletedLines, splitData))
            {
                GitDiffDecorations::LineSets set;
                set.addedLines = std::move(addedLines);
                set.deletedLines = std::move(deletedLines);
                GitDiffDecorations::SetForFile(fullPath.wstring(), set);
                GitDiffDecorations::SetSplitForFile(fullPath.wstring(), splitData);
                GitDiffDecorations::MarkPendingSplitOpen(fullPath.wstring());
            }
            else
            {
                GitDiffDecorations::ClearForFile(fullPath.wstring());
                GitDiffDecorations::ClearSplitForFile(fullPath.wstring());
            }
            auto *heapPath = new std::wstring(fullPath.wstring());
            PostMessageW(hwnd, WM_USER + 100, 0, (LPARAM)heapPath);
        }

        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

void GitPanel::OnLeftButtonUp(HWND hwnd)
{
    if (HandleResizeLeftButtonUp(hwnd))
        return;

    bool changed = false;
    if (changesScrollbar_.OnLeftButtonUp())
        changed = true;

    POINT pt = {0, 0};
    GetCursorPos(&pt);
    ScreenToClient(hwnd, &pt);
    if (commitMessageInput_.OnLeftButtonUp(hwnd, pt))
        changed = true;

    if (capturedScrollbar_)
    {
        ReleaseCapture();
        capturedScrollbar_ = false;
    }

    if (changed)
        InvalidateRect(hwnd, nullptr, FALSE);
}

void GitPanel::OnMouseWheel(HWND hwnd, int delta)
{
    POINT pt = {0, 0};
    GetCursorPos(&pt);
    ScreenToClient(hwnd, &pt);

    bool changed = false;
    if (IsPointInRect(changesRect_, pt))
        changed = changesScrollbar_.OnMouseWheel(delta);

    if (changed)
        InvalidateRect(hwnd, nullptr, FALSE);
}

void GitPanel::OnChar(wchar_t ch)
{
    if (commitMessageInput_.IsFocused())
        commitMessageInput_.OnChar(ch);
}

void GitPanel::OnKeyDown(WPARAM key)
{
    if (quickActionMenuOpen_ && key == VK_ESCAPE)
    {
        quickActionMenuOpen_ = false;
        quickActionHoveredIndex_ = -1;
        return;
    }

    if (commitMessageInput_.IsFocused())
    {
        commitMessageInput_.OnKeyDown(key);
        if (key == VK_TAB)
            commitMessageInput_.SetFocused(false);
        return;
    }

    if (key == VK_F5)
        RefreshStatus();
}

bool GitPanel::IsInputFocused() const
{
    return commitMessageInput_.IsFocused();
}

void GitPanel::UnfocusInputs()
{
    commitMessageInput_.SetFocused(false);
    quickActionMenuOpen_ = false;
    quickActionHoveredIndex_ = -1;
}

bool GitPanel::IsPointInRect(const D2D1_RECT_F &rect, POINT pt) const
{
    return pt.x >= rect.left && pt.x <= rect.right &&
           pt.y >= rect.top && pt.y <= rect.bottom;
}

int GitPanel::HitTestChange(POINT pt) const
{
    if (!IsPointInRect(changesRect_, pt))
        return -1;

    float listTop = changesRect_.top;
    if (pt.y < listTop || pt.y > changesRect_.bottom)
        return -1;

    float localY = static_cast<float>(pt.y) - listTop + changesScrollbar_.GetScrollOffset();
    if (localY < 0.0f)
        return -1;
    int idx = (int)(localY / changeRowHeight_);
    if (idx < 0 || idx >= (int)changes_.size())
        return -1;
    return idx;
}

int GitPanel::HitTestQuickActionMenuItem(POINT pt) const
{
    if (!quickActionMenuOpen_ || !IsPointInRect(quickActionMenuRect_, pt))
        return -1;

    const float menuHeight = quickActionMenuRect_.bottom - quickActionMenuRect_.top;
    if (menuHeight <= 0.0f)
        return -1;
    const float rowH = menuHeight / 4.0f;
    const float localY = (float)pt.y - quickActionMenuRect_.top;
    if (localY < 0.0f)
        return -1;
    int idx = (int)(localY / rowH);
    if (idx < 0 || idx >= 4)
        return -1;
    return idx;
}

const wchar_t *GitPanel::GetQuickActionLabel(int actionIndex) const
{
    switch (actionIndex)
    {
    case 0:
        return L"Commit & Push";
    case 1:
        return L"Commit";
    case 2:
        return L"Push";
    case 3:
        return L"Refresh";
    default:
        return L"Commit & Push";
    }
}

bool GitPanel::ExecuteQuickAction(int actionIndex)
{
    bool ok = false;
    switch (actionIndex)
    {
    case 0:
        ok = RunCommit(true);
        break;
    case 1:
        ok = RunCommit(false);
        break;
    case 2:
        ok = RunPushOnly();
        break;
    case 3:
        RefreshStatus();
        ok = true;
        break;
    default:
        break;
    }
    return ok;
}

void GitPanel::RefreshStatus()
{
    lastError_.clear();

    if (!libgit2Ready_)
    {
        isGitRepo_ = false;
        return;
    }

    std::wstring candidate = Trim(repoRoot_);
    if (candidate.empty())
        candidate = Trim(GetExplorerManager().GetState().rootPath);

    if (candidate.empty())
    {
        isGitRepo_ = false;
        repoRoot_.clear();
        changes_.clear();
        selectedChangeIndex_ = -1;
        hoveredChangeIndex_ = -1;
        return;
    }

    std::wstring previousPath;
    if (selectedChangeIndex_ >= 0 && selectedChangeIndex_ < (int)changes_.size())
        previousPath = changes_[(size_t)selectedChangeIndex_].path;

    git_repository *repo = nullptr;
    std::string candidateUtf8 = WideToUtf8(candidate);
    int rc = git_repository_open_ext(&repo, candidateUtf8.c_str(), GIT_REPOSITORY_OPEN_CROSS_FS, nullptr);
    if (rc != 0 || !repo)
    {
        isGitRepo_ = false;
        repoRoot_.clear();
        lastError_ = GetLastGitError(L"Unable to open repository.");
        changes_.clear();
        selectedChangeIndex_ = -1;
        hoveredChangeIndex_ = -1;
        return;
    }

    isGitRepo_ = true;
    const char *workdir = git_repository_workdir(repo);
    if (workdir && workdir[0] != '\0')
        repoRoot_ = Trim(Utf8ToWide(workdir));
    else
        repoRoot_ = candidate;

    changes_.clear();
    hoveredChangeIndex_ = -1;

    git_status_options statusOpts = GIT_STATUS_OPTIONS_INIT;
    statusOpts.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
    statusOpts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED |
                       GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS |
                       GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX |
                       GIT_STATUS_OPT_RENAMES_INDEX_TO_WORKDIR |
                       GIT_STATUS_OPT_SORT_CASE_SENSITIVELY;

    git_status_list *statusList = nullptr;
    rc = git_status_list_new(&statusList, repo, &statusOpts);
    if (rc != 0 || !statusList)
    {
        lastError_ = GetLastGitError(L"Unable to query repository status.");
        git_repository_free(repo);
        changes_.clear();
        selectedChangeIndex_ = -1;
        return;
    }

    size_t count = git_status_list_entrycount(statusList);
    for (size_t i = 0; i < count; ++i)
    {
        const git_status_entry *entry = git_status_byindex(statusList, i);
        if (!entry)
            continue;

        const char *path = nullptr;
        if (entry->index_to_workdir)
        {
            const git_diff_delta *d = entry->index_to_workdir;
            path = (d->new_file.path && d->new_file.path[0] != '\0') ? d->new_file.path : d->old_file.path;
        }
        if (!path && entry->head_to_index)
        {
            const git_diff_delta *d = entry->head_to_index;
            path = (d->new_file.path && d->new_file.path[0] != '\0') ? d->new_file.path : d->old_file.path;
        }
        if (!path || path[0] == '\0')
            continue;

        Panels::GitChange change;
        change.path = DecodeGitPath(Utf8ToWide(path));
        change.statusFlags = (unsigned int)entry->status;
        change.indexStatus = (wchar_t)StatusCharIndex(entry->status);
        change.worktreeStatus = (wchar_t)StatusCharWorktree(entry->status);
        if (change.indexStatus == L' ' && change.worktreeStatus == L' ')
            continue;

        changes_.push_back(std::move(change));
    }

    git_status_list_free(statusList);
    git_repository_free(repo);

    std::sort(changes_.begin(), changes_.end(), [](const Panels::GitChange &a, const Panels::GitChange &b)
    {
        return a.path < b.path;
    });

    selectedChangeIndex_ = -1;
    if (!previousPath.empty())
    {
        for (int i = 0; i < (int)changes_.size(); ++i)
        {
            if (changes_[(size_t)i].path == previousPath)
            {
                selectedChangeIndex_ = i;
                break;
            }
        }
    }
    if (selectedChangeIndex_ < 0 && !changes_.empty())
        selectedChangeIndex_ = 0;
}

bool GitPanel::BuildDiffViewForChange(const Panels::GitChange &change,
                                      std::vector<int> &addedLines,
                                      std::vector<int> &deletedLines,
                                      GitDiffDecorations::SplitViewData &splitData)
{
    addedLines.clear();
    deletedLines.clear();
    splitData.rows.clear();

    if (!libgit2Ready_ || !isGitRepo_ || repoRoot_.empty())
        return false;

    git_repository *repo = nullptr;
    int rc = git_repository_open_ext(&repo, WideToUtf8(repoRoot_).c_str(), GIT_REPOSITORY_OPEN_CROSS_FS, nullptr);
    if (rc != 0 || !repo)
        return false;

    std::string pathUtf8 = WideToUtf8(change.path);
    char *paths[1] = {const_cast<char *>(pathUtf8.c_str())};

    git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
    opts.flags = GIT_DIFF_INCLUDE_UNTRACKED |
                 GIT_DIFF_RECURSE_UNTRACKED_DIRS |
                 GIT_DIFF_INCLUDE_TYPECHANGE |
                 GIT_DIFF_SHOW_UNTRACKED_CONTENT;
    opts.context_lines = 3;
    opts.pathspec.count = 1;
    opts.pathspec.strings = paths;

    git_reference *headRef = nullptr;
    git_commit *headCommit = nullptr;
    git_tree *headTree = nullptr;
    if (git_repository_head(&headRef, repo) == 0 && headRef)
    {
        const git_oid *headOid = git_reference_target(headRef);
        if (headOid && git_commit_lookup(&headCommit, repo, headOid) == 0 && headCommit)
            git_commit_tree(&headTree, headCommit);
    }

    git_diff *diff = nullptr;
    rc = git_diff_tree_to_workdir_with_index(&diff, repo, headTree, &opts);
    if (rc == 0 && diff)
    {
        DiffCollectContext collectCtx;
        collectCtx.addedLines = &addedLines;
        collectCtx.deletedLines = &deletedLines;
        collectCtx.splitData = &splitData;
        git_diff_print(diff, GIT_DIFF_FORMAT_PATCH, CollectDiffLineNumbers, &collectCtx);
        FlushPendingDeletedRows(collectCtx);
    }

    git_diff_free(diff);
    git_tree_free(headTree);
    git_commit_free(headCommit);
    git_reference_free(headRef);
    git_repository_free(repo);

    // Special case: new untracked file can have no hunk line numbers with some diff states.
    if (addedLines.empty() && change.worktreeStatus == L'?' && !repoRoot_.empty())
    {
        std::filesystem::path full = std::filesystem::path(repoRoot_) / std::filesystem::path(change.path);
        std::wifstream in(full);
        if (in.is_open())
        {
            int lineIndex = 0;
            std::wstring line;
            while (std::getline(in, line))
            {
                addedLines.push_back(lineIndex++);
                GitDiffDecorations::SplitRow row;
                row.rightText = line;
                row.hasRight = true;
                row.rightAdded = true;
                splitData.rows.push_back(std::move(row));
            }
        }
    }

    SortUnique(addedLines);
    SortUnique(deletedLines);
    return !addedLines.empty() || !deletedLines.empty() || !splitData.rows.empty();
}

bool GitPanel::PushCurrentBranch(std::wstring &outError)
{
    outError.clear();

    if (!libgit2Ready_ || !isGitRepo_ || repoRoot_.empty())
    {
        outError = L"No git repository selected.";
        return false;
    }

    git_repository *repo = nullptr;
    int rc = git_repository_open_ext(&repo, WideToUtf8(repoRoot_).c_str(), GIT_REPOSITORY_OPEN_CROSS_FS, nullptr);
    if (rc != 0 || !repo)
    {
        outError = GetLastGitError(L"Unable to open repository for push.");
        return false;
    }

    git_reference *headRef = nullptr;
    rc = git_repository_head(&headRef, repo);
    if (rc != 0 || !headRef)
    {
        outError = GetLastGitError(L"Unable to resolve HEAD branch.");
        git_repository_free(repo);
        return false;
    }

    const char *branchName = nullptr;
    rc = git_branch_name(&branchName, headRef);
    if (rc != 0 || !branchName || branchName[0] == '\0')
    {
        outError = GetLastGitError(L"Unable to determine branch name.");
        git_reference_free(headRef);
        git_repository_free(repo);
        return false;
    }

    std::string remoteName;
    git_buf branchRemoteName = GIT_BUF_INIT;
    rc = git_branch_remote_name(&branchRemoteName, repo, git_reference_name(headRef));
    if (rc == 0 && branchRemoteName.ptr && branchRemoteName.size > 0)
    {
        remoteName.assign(branchRemoteName.ptr, branchRemoteName.size);
    }
    git_buf_dispose(&branchRemoteName);

    if (remoteName.empty())
    {
        git_strarray remotes = {0};
        if (git_remote_list(&remotes, repo) == 0 && remotes.count > 0)
        {
            if (remotes.count == 1 && remotes.strings && remotes.strings[0])
            {
                remoteName = remotes.strings[0];
            }
            else if (remotes.strings)
            {
                for (size_t i = 0; i < remotes.count; ++i)
                {
                    const char *name = remotes.strings[i];
                    if (name && std::string(name) == "origin")
                    {
                        remoteName = name;
                        break;
                    }
                }
                if (remoteName.empty())
                {
                    for (size_t i = 0; i < remotes.count; ++i)
                    {
                        if (remotes.strings[i])
                        {
                            remoteName = remotes.strings[i];
                            break;
                        }
                    }
                }
            }
        }
        git_strarray_dispose(&remotes);
    }

    if (remoteName.empty())
    {
        outError = L"No remote configured for push.";
        git_reference_free(headRef);
        git_repository_free(repo);
        return false;
    }

    git_remote *remote = nullptr;
    rc = git_remote_lookup(&remote, repo, remoteName.c_str());
    if (rc != 0 || !remote)
    {
        outError = GetLastGitError(L"Unable to resolve remote for push.");
        git_reference_free(headRef);
        git_repository_free(repo);
        return false;
    }

    std::wstring tokenWide;
    if (!GitHubAuth::LoadToken(tokenWide, outError))
    {
        git_remote_free(remote);
        git_reference_free(headRef);
        git_repository_free(repo);
        if (outError.empty())
            outError = L"GitHub is not connected. Sign in from Settings.";
        return false;
    }

    PushCredentialContext credCtx;
    credCtx.token = WideToUtf8(tokenWide);
    if (!tokenWide.empty())
        SecureZeroMemory(tokenWide.data(), tokenWide.size() * sizeof(wchar_t));
    if (credCtx.token.empty())
    {
        outError = L"Invalid GitHub OAuth session.";
        git_remote_free(remote);
        git_reference_free(headRef);
        git_repository_free(repo);
        return false;
    }

    git_push_options pushOpts = GIT_PUSH_OPTIONS_INIT;
    pushOpts.callbacks.credentials = AcquirePushCredentials;
    pushOpts.callbacks.payload = &credCtx;

    std::string srcRef = std::string("refs/heads/") + branchName;
    std::string dstRef = srcRef;
    std::string refspec = srcRef + ":" + dstRef;
    char *specs[1] = {const_cast<char *>(refspec.c_str())};
    git_strarray refspecArray = {specs, 1};

    rc = git_remote_push(remote, &refspecArray, &pushOpts);
    if (rc != 0)
        outError = GetLastGitError(L"Push failed.");

    git_remote_free(remote);
    git_reference_free(headRef);
    git_repository_free(repo);
    if (!credCtx.token.empty())
        SecureZeroMemory(credCtx.token.data(), credCtx.token.size());

    return rc == 0;
}

bool GitPanel::RunCommit()
{
    return RunCommit(true);
}

bool GitPanel::RunPushOnly()
{
    if (!libgit2Ready_)
    {
        lastError_ = L"libgit2 is not initialized.";
        return false;
    }
    if (!isGitRepo_)
    {
        lastError_ = L"No git repository selected.";
        return false;
    }
    if (!GitHubAuth::HasToken())
    {
        lastError_ = L"GitHub not connected. Sign in from Settings.";
        return false;
    }

    std::wstring pushError;
    bool pushed = PushCurrentBranch(pushError);
    if (!pushed)
    {
        if (pushError.empty())
            pushError = L"Push failed.";
        lastError_ = pushError;
    }
    else
    {
        lastError_.clear();
    }
    RefreshStatus();
    return pushed;
}

bool GitPanel::RunCommit(bool pushAfter)
{
    if (!libgit2Ready_)
    {
        lastError_ = L"libgit2 is not initialized.";
        return false;
    }

    if (!isGitRepo_)
    {
        lastError_ = L"No git repository selected.";
        return false;
    }
    if (!GitHubAuth::HasToken())
    {
        lastError_ = L"GitHub not connected. Sign in from Settings.";
        return false;
    }

    std::wstring message = Trim(commitMessageInput_.GetText());
    if (message.empty())
    {
        lastError_ = L"Commit message is required.";
        return false;
    }

    git_repository *repo = nullptr;
    int rc = git_repository_open_ext(&repo, WideToUtf8(repoRoot_).c_str(), GIT_REPOSITORY_OPEN_CROSS_FS, nullptr);
    if (rc != 0 || !repo)
    {
        lastError_ = GetLastGitError(L"Unable to open repository.");
        return false;
    }

    git_index *index = nullptr;
    rc = git_repository_index(&index, repo);
    if (rc == 0)
        rc = git_index_add_all(index, nullptr, GIT_INDEX_ADD_DEFAULT, nullptr, nullptr);
    if (rc == 0)
        rc = git_index_update_all(index, nullptr, nullptr, nullptr);
    if (rc == 0)
        rc = git_index_write(index);

    git_oid treeOid;
    git_tree *tree = nullptr;
    if (rc == 0)
        rc = git_index_write_tree(&treeOid, index);
    if (rc == 0)
        rc = git_tree_lookup(&tree, repo, &treeOid);

    bool hasChangesToCommit = true;
    git_reference *headRef = nullptr;
    git_commit *parentCommit = nullptr;
    const git_commit *parents[1] = {nullptr};
    size_t parentCount = 0;

    if (git_repository_head(&headRef, repo) == 0 && headRef)
    {
        const git_oid *headOid = git_reference_target(headRef);
        if (headOid && git_commit_lookup(&parentCommit, repo, headOid) == 0 && parentCommit)
        {
            parents[0] = parentCommit;
            parentCount = 1;

            git_tree *parentTree = nullptr;
            if (git_commit_tree(&parentTree, parentCommit) == 0 && parentTree)
            {
                const git_oid *parentTreeOid = git_tree_id(parentTree);
                if (parentTreeOid && git_oid_equal(parentTreeOid, &treeOid))
                    hasChangesToCommit = false;
            }
            git_tree_free(parentTree);
        }
    }
    else if (index && git_index_entrycount(index) == 0)
    {
        hasChangesToCommit = false;
    }

    if (rc != 0 || !tree)
    {
        lastError_ = GetLastGitError(L"Unable to prepare commit tree.");
    }
    else if (!hasChangesToCommit)
    {
        lastError_ = L"No staged changes to commit.";
        rc = -1;
    }
    else
    {
        git_signature *sig = nullptr;
        rc = git_signature_default(&sig, repo);
        if (rc != 0 || !sig)
            rc = git_signature_now(&sig, "Nebula User", "nebula@local.dev");

        if (rc == 0 && sig)
        {
            git_oid commitOid;
            std::string msgUtf8 = WideToUtf8(message);
            rc = git_commit_create(&commitOid, repo, "HEAD", sig, sig, nullptr,
                                   msgUtf8.c_str(), tree, parentCount, parents);
        }
        if (sig)
            git_signature_free(sig);
    }

    git_tree_free(tree);
    git_commit_free(parentCommit);
    git_reference_free(headRef);
    git_index_free(index);
    git_repository_free(repo);

    if (rc != 0)
    {
        if (lastError_.empty())
            lastError_ = GetLastGitError(L"Commit failed.");
        return false;
    }

    if (!pushAfter)
    {
        lastError_.clear();
        commitMessageInput_.SetText(L"");
        RefreshStatus();
        return true;
    }

    std::wstring pushError;
    bool pushed = PushCurrentBranch(pushError);
    if (!pushed)
    {
        if (pushError.empty())
            pushError = L"Push failed.";
        lastError_ = L"Committed locally, but push failed: " + pushError;
    }
    else
    {
        lastError_.clear();
    }
    commitMessageInput_.SetText(L"");
    RefreshStatus();
    return pushed;
}

std::wstring GitPanel::Trim(const std::wstring &s)
{
    size_t a = 0;
    while (a < s.size() && iswspace(s[a]))
        ++a;
    size_t b = s.size();
    while (b > a && iswspace(s[b - 1]))
        --b;
    return s.substr(a, b - a);
}

std::wstring GitPanel::DecodeGitPath(const std::wstring &pathField)
{
    std::wstring p = Trim(pathField);
    if (p.size() >= 2 && p.front() == L'"' && p.back() == L'"')
    {
        std::wstring out;
        for (size_t i = 1; i + 1 < p.size(); ++i)
        {
            wchar_t c = p[i];
            if (c == L'\\' && i + 1 < p.size() - 1)
            {
                wchar_t n = p[++i];
                switch (n)
                {
                case L'\\':
                    out.push_back(L'\\');
                    break;
                case L'"':
                    out.push_back(L'"');
                    break;
                case L't':
                    out.push_back(L'\t');
                    break;
                case L'r':
                    out.push_back(L'\r');
                    break;
                case L'n':
                    out.push_back(L'\n');
                    break;
                default:
                    out.push_back(n);
                    break;
                }
            }
            else
            {
                out.push_back(c);
            }
        }
        p.swap(out);
    }
    return p;
}

std::string GitPanel::WideToUtf8(const std::wstring &text)
{
    if (text.empty())
        return {};

    int len = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    if (len <= 0)
        return {};
    std::string out((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring GitPanel::Utf8ToWide(const std::string &text)
{
    if (text.empty())
        return {};

    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr, 0);
    if (len > 0)
    {
        std::wstring out((size_t)len, L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), out.data(), len);
        return out;
    }

    len = MultiByteToWideChar(CP_ACP, 0, text.data(), (int)text.size(), nullptr, 0);
    if (len <= 0)
        return {};
    std::wstring out((size_t)len, L'\0');
    MultiByteToWideChar(CP_ACP, 0, text.data(), (int)text.size(), out.data(), len);
    return out;
}

std::wstring GitPanel::GetLastGitError(const std::wstring &fallback)
{
    const git_error *err = git_error_last();
    if (!err || !err->message || err->message[0] == '\0')
        return fallback;
    return Utf8ToWide(err->message);
}
