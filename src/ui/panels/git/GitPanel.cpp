#include "GitPanel.h"
#include "GitDiffDecorations.h"

#include "core/explorer/Explorer.h"
#include "helpers/window_helpers.h"
#include "ui/components/input/InputTheme.h"
#include "ui/theme/Theme.h"
#include "utils/auth/GitHubAuth.h"

#include <git2.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace
{
std::mutex g_libgit2Mutex;
int g_libgit2RefCount = 0;

void DrawTrimmedText(ID2D1RenderTarget *ctx,
                     IDWriteFactory *dwrite,
                     const std::wstring &text,
                     IDWriteTextFormat *format,
                     const D2D1_RECT_F &rect,
                     ID2D1Brush *brush)
{
    if (!ctx || !dwrite || !format || !brush || text.empty())
        return;

    const float width = rect.right - rect.left;
    const float height = rect.bottom - rect.top;
    if (width <= 1.0f || height <= 1.0f)
        return;

    IDWriteTextLayout *layout = nullptr;
    if (FAILED(dwrite->CreateTextLayout(text.c_str(), (UINT32)text.size(), format, width, height, &layout)) || !layout)
        return;

    DWRITE_TRIMMING trimming = {};
    trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;

    IDWriteInlineObject *ellipsis = nullptr;
    if (SUCCEEDED(dwrite->CreateEllipsisTrimmingSign(format, &ellipsis)) && ellipsis)
    {
        layout->SetTrimming(&trimming, ellipsis);
        ellipsis->Release();
    }

    ctx->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout, brush,
                        D2D1_DRAW_TEXT_OPTIONS_CLIP);
    layout->Release();
}

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
}

GitPanel::GitPanel()
    : Panel(PanelId::Git)
{
    config_ = PanelConfig(
        PanelId::Git,
        L"assets/ressource/icons/git-sidebar.svg",
        L"Source Control",
        true,
        false,
        2);
    title_ = L"SOURCE CONTROL";

    ApplyInputTheme(commitMessageInput_, L"Commit message");

    claudeBridge_.onRequestStarted = [this]() {
        commitGenerationInFlight_ = true;
        commitGenerationBuffer_.clear();
        lastError_.clear();
        InvalidatePanel();
    };

    claudeBridge_.onTextDelta = [this](const std::wstring &delta) {
        commitGenerationBuffer_ += delta;
    };

    claudeBridge_.onRequestFinished = [this](const std::wstring & /*sessionId*/, const std::wstring &fallbackResult) {
        commitGenerationInFlight_ = false;
        std::wstring response = commitGenerationBuffer_;
        if (Trim(response).empty())
            response = fallbackResult;

        std::wstring message = NormalizeCommitMessage(response);
        if (message.empty())
            lastError_ = L"Claude did not return a usable commit message.";
        else
        {
            commitMessageInput_.SetText(message);
            lastError_.clear();
        }

        commitGenerationBuffer_.clear();
        InvalidatePanel();
    };

    claudeBridge_.onError = [this](const std::wstring &message) {
        commitGenerationInFlight_ = false;
        commitGenerationBuffer_.clear();
        lastError_ = Trim(message).empty() ? L"Commit message generation failed." : Trim(message);
        InvalidatePanel();
    };

    commitMessageInput_.onSubmit = [this]()
    {
        ExecuteQuickAction(0); // Commit & Push on Enter
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
    style.paddingLeft = UI::InputTheme::kHorizontalPadding;
    style.paddingRight = UI::InputTheme::kHorizontalPadding + 34.0f;
}

void GitPanel::SyncRepoPathFromExplorer()
{
    const std::wstring root = Trim(GetExplorerManager().GetState().rootPath);
    repoRoot_ = root;
}

void GitPanel::UpdateLayout(HWND hwnd)
{
    hwnd_ = hwnd;
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
    SyncRepoPathFromExplorer();
    if (becameVisible || !hasAutoRefreshed_ || periodicRefresh)
    {
        RefreshStatus();
        hasAutoRefreshed_ = true;
        lastAutoRefreshTick_ = now;
    }
    wasVisibleLastLayout_ = true;

    float x0 = state_.leftEdge + state_.leftPadding;
    float x1 = state_.rightEdge - state_.leftPadding;
    float y = state_.topEdge + state_.titleHeight + 8.0f;
    const float inputH  = 30.0f;
    const float actionH = 30.0f;
    const float secH    = 26.0f;
    const float gap     = 6.0f;

    // Commit message input always full-width
    commitMessageInput_.SetRect(D2D1::RectF(x0, y, x1, y + inputH));
    commitGenerateRect_ = D2D1::RectF(x1 - 30.0f, y + 4.0f, x1 - 4.0f, y + inputH - 4.0f);
    y += inputH + gap;

    // Primary "Commit & Push" full-width accent button
    quickActionPrimaryRect_ = D2D1::RectF(x0, y, x1, y + actionH);
    y += actionH + gap;

    // Secondary "Commit" (left) | "Push" (right) buttons
    const float secGap = 5.0f;
    float half = (x1 - x0 - secGap) * 0.5f;
    quickActionToggleRect_ = D2D1::RectF(x0,               y, x0 + half,        y + secH);
    quickActionMenuRect_   = D2D1::RectF(x0 + half + secGap, y, x1,             y + secH);
    y += secH + gap;

    quickActionMenuOpen_     = false;
    quickActionHoveredIndex_ = -1;

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

    // Branch bar (fixed, not scrolled)
    if (isGitRepo_)
    {
        branchBarRect_ = D2D1::RectF(x0, y, x1, y + kBranchBarH);
        y += kBranchBarH + 2.0f;

        // Toolbar buttons inside branch bar
        const float btnSz = 22.0f;
        const float btnY  = branchBarRect_.top + (kBranchBarH - btnSz) * 0.5f;
        refreshBtnRect_ = D2D1::RectF(x1 - btnSz, btnY, x1, btnY + btnSz);
        pullBtnRect_    = D2D1::RectF(refreshBtnRect_.left - btnSz - 4.0f, btnY,
                                       refreshBtnRect_.left - 4.0f, btnY + btnSz);
    }
    else
    {
        branchBarRect_  = D2D1::RectF(0, 0, 0, 0);
        pullBtnRect_    = D2D1::RectF(0, 0, 0, 0);
        refreshBtnRect_ = D2D1::RectF(0, 0, 0, 0);
    }

    float changesBottom = state_.bottomEdge - 4.0f;
    if (changesBottom < y + 80.0f)
        changesBottom = y + 80.0f;
    changesRect_ = D2D1::RectF(x0, y, x1, changesBottom);

    float changesViewport = (changesRect_.bottom - changesRect_.top);
    if (changesViewport < 0.0f)
        changesViewport = 0.0f;

    // Content height accounts for section headers
    float changesContent = 0.0f;
    const std::vector<int> *sectionItems[3] = {&conflictIndices_, &stagedIndices_, &changesIndices_};
    for (int s = 0; s < 3; ++s)
    {
        if (sectionItems[s]->empty())
            continue;
        changesContent += kSectionHeaderH;
        if (!sectionCollapsed_[s])
            changesContent += (float)sectionItems[s]->size() * changeRowHeight_;
    }
    if (changesContent == 0.0f && !changes_.empty())
        changesContent = changeRowHeight_; // fallback: should never happen

    changesScrollbar_.UpdateLayout(changesRect_.left, changesRect_.top,
                                   changesRect_.right - changesRect_.left, changesViewport, changesContent);
}

void GitPanel::Draw(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd)
{
    if (!visible_ || state_.physicalWidth <= 0)
        return;

    hwnd_ = hwnd;

    // Keep input visuals synced to runtime theme changes.
    ApplyInputTheme(commitMessageInput_, L"Commit message");

    D2D1_RECT_F clipRect = D2D1::RectF(state_.leftEdge, state_.topEdge, state_.rightEdge, state_.bottomEdge);
    ctx->PushAxisAlignedClip(clipRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    Panel::DrawBackground(ctx);
    Panel::DrawTitle(ctx, dwrite);

    commitMessageInput_.Draw(ctx, dwrite);
    DrawCommitGenerateButton(ctx, dwrite);

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

    DrawBranchBar(ctx, dwrite, hwnd);
    DrawChanges(ctx, dwrite, hwnd);
    changesScrollbar_.Draw(ctx);
    DrawQuickActions(ctx, dwrite, hwnd);

    Panel::DrawRightBorder(ctx);
    ctx->PopAxisAlignedClip();

}

void GitPanel::DrawBranchBar(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd)
{
    (void)hwnd;
    if (branchBarRect_.right <= branchBarRect_.left)
        return;

    const UI::Theme::Palette &palette = UI::Theme::GetPalette();
    const bool light = (UI::Theme::GetMode() == UI::Theme::Mode::Light);

    ID2D1SolidColorBrush *textBrush    = nullptr;
    ID2D1SolidColorBrush *dimBrush     = nullptr;
    ID2D1SolidColorBrush *hoverBrush   = nullptr;
    ID2D1SolidColorBrush *aheadBrush   = nullptr;
    ID2D1SolidColorBrush *behindBrush  = nullptr;
    ID2D1SolidColorBrush *sepBrush     = nullptr;

    D2D1_COLOR_F primary = UI::Theme::PrimaryText();
    ctx->CreateSolidColorBrush(primary, &textBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(primary.r, primary.g, primary.b, 0.45f), &dimBrush);
    ctx->CreateSolidColorBrush(palette.explorerRowHover, &hoverBrush);
    ctx->CreateSolidColorBrush(light ? D2D1::ColorF(0.18f, 0.58f, 0.24f) : D2D1::ColorF(0.42f, 0.78f, 0.38f), &aheadBrush);
    ctx->CreateSolidColorBrush(light ? D2D1::ColorF(0.17f, 0.45f, 0.82f) : D2D1::ColorF(0.39f, 0.67f, 0.93f), &behindBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(primary.r, primary.g, primary.b, 0.08f), &sepBrush);

    IDWriteTextFormat *iconFmt = nullptr;
    IDWriteTextFormat *branchFmt = nullptr;
    IDWriteTextFormat *countFmt  = nullptr;

    dwrite->CreateTextFormat(L"Segoe Fluent Icons", NULL, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &iconFmt);
    if (!iconFmt)
        dwrite->CreateTextFormat(L"Segoe MDL2 Assets", NULL, DWRITE_FONT_WEIGHT_NORMAL,
                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &iconFmt);

    dwrite->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &branchFmt);
    dwrite->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"en-us", &countFmt);

    if (iconFmt)
    {
        iconFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        iconFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (branchFmt)
    {
        branchFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        branchFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        branchFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    if (countFmt)
    {
        countFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        countFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        countFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    const D2D1_RECT_F &bar = branchBarRect_;

    // Bottom separator line
    if (sepBrush)
        ctx->FillRectangle(D2D1::RectF(bar.left, bar.bottom - 1.0f, bar.right, bar.bottom), sepBrush);

    // Branch glyph + name
    float x = bar.left;
    if (iconFmt && dimBrush)
    {
        D2D1_RECT_F glyphRect = D2D1::RectF(x, bar.top, x + 22.0f, bar.bottom);
        const std::wstring branchGlyph = L"\uEB05"; // BranchFork (Segoe Fluent)
        ctx->DrawTextW(branchGlyph.c_str(), 1, iconFmt, glyphRect, dimBrush);
        x += 22.0f;
    }

    // Compute right boundary (where buttons start)
    float rightBoundary = pullBtnRect_.left > bar.left ? pullBtnRect_.left - 6.0f : bar.right - 4.0f;

    if (!currentBranch_.empty() && branchFmt && textBrush)
    {
        // Leave room for ahead/behind badges (up to ~50px)
        float nameBoundary = rightBoundary - 52.0f;
        if (nameBoundary < x + 10.0f)
            nameBoundary = rightBoundary;
        D2D1_RECT_F nameRect = D2D1::RectF(x, bar.top, nameBoundary, bar.bottom);
        ctx->DrawTextW(currentBranch_.c_str(), (UINT32)currentBranch_.size(), branchFmt, nameRect, textBrush);

        // Ahead/behind badges (small, right of branch name)
        float badgeX = nameBoundary + 4.0f;
        if (aheadCount_ > 0 && countFmt && aheadBrush)
        {
            std::wstring s = L"\u2191" + std::to_wstring(aheadCount_); // ↑N
            D2D1_RECT_F r = D2D1::RectF(badgeX, bar.top, badgeX + 22.0f, bar.bottom);
            ctx->DrawTextW(s.c_str(), (UINT32)s.size(), countFmt, r, aheadBrush);
            badgeX += 24.0f;
        }
        if (behindCount_ > 0 && countFmt && behindBrush)
        {
            std::wstring s = L"\u2193" + std::to_wstring(behindCount_); // ↓N
            D2D1_RECT_F r = D2D1::RectF(badgeX, bar.top, badgeX + 22.0f, bar.bottom);
            ctx->DrawTextW(s.c_str(), (UINT32)s.size(), countFmt, r, behindBrush);
        }
    }

    // Toolbar buttons: Pull (↓), Refresh (⟳)
    struct BtnDef { const D2D1_RECT_F &rect; bool hovered; const wchar_t *glyph; };
    BtnDef buttons[] = {
        {pullBtnRect_,    pullBtnHovered_,    L"\uE895"}, // download/pull (Segoe MDL2)
        {refreshBtnRect_, refreshBtnHovered_, L"\uE72C"}, // refresh
    };
    for (auto &b : buttons)
    {
        if (b.rect.right <= b.rect.left)
            continue;
        if (b.hovered && hoverBrush)
        {
            D2D1_ANTIALIAS_MODE oldAA = ctx->GetAntialiasMode();
            ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            ctx->FillRoundedRectangle(D2D1::RoundedRect(b.rect, 4.0f, 4.0f), hoverBrush);
            ctx->SetAntialiasMode(oldAA);
        }
        if (iconFmt && dimBrush)
            ctx->DrawTextW(b.glyph, 1, iconFmt, b.rect, dimBrush);
    }

    if (textBrush)   textBrush->Release();
    if (dimBrush)    dimBrush->Release();
    if (hoverBrush)  hoverBrush->Release();
    if (aheadBrush)  aheadBrush->Release();
    if (behindBrush) behindBrush->Release();
    if (sepBrush)    sepBrush->Release();
    if (iconFmt)     iconFmt->Release();
    if (branchFmt)   branchFmt->Release();
    if (countFmt)    countFmt->Release();
}

void GitPanel::DrawCommitGenerateButton(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite)
{
    if (commitGenerateRect_.right <= commitGenerateRect_.left)
        return;

    const bool light = (UI::Theme::GetMode() == UI::Theme::Mode::Light);
    const bool enabled = isGitRepo_ && !changes_.empty() && !claudeBridge_.IsBusy() && !commitGenerationInFlight_;
    const wchar_t *label = commitGenerationInFlight_ ? L"..." : L"***";

    D2D1_COLOR_F fill = enabled
        ? (commitGenerateHovered_ ? UI::Theme::Accent() : D2D1::ColorF(UI::Theme::Accent().r, UI::Theme::Accent().g, UI::Theme::Accent().b, 0.16f))
        : (light ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.05f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.05f));
    D2D1_COLOR_F border = enabled
        ? D2D1::ColorF(UI::Theme::Accent().r, UI::Theme::Accent().g, UI::Theme::Accent().b, commitGenerateHovered_ ? 0.95f : 0.45f)
        : (light ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.14f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f));
    D2D1_COLOR_F text = enabled
        ? (commitGenerateHovered_ ? D2D1::ColorF(1.0f, 1.0f, 1.0f) : UI::Theme::Accent())
        : (light ? D2D1::ColorF(0.48f, 0.48f, 0.48f, 1.0f) : D2D1::ColorF(0.56f, 0.56f, 0.56f, 1.0f));

    ID2D1SolidColorBrush *fillBrush = nullptr;
    ID2D1SolidColorBrush *borderBrush = nullptr;
    ID2D1SolidColorBrush *textBrush = nullptr;
    IDWriteTextFormat *textFormat = nullptr;

    ctx->CreateSolidColorBrush(fill, &fillBrush);
    ctx->CreateSolidColorBrush(border, &borderBrush);
    ctx->CreateSolidColorBrush(text, &textBrush);
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             10.5f, L"en-us", &textFormat);

    if (textFormat)
    {
        textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        textFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    D2D1_ANTIALIAS_MODE oldAA = ctx->GetAntialiasMode();
    ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    D2D1_RECT_F snapped = D2D1::RectF(std::round(commitGenerateRect_.left), std::round(commitGenerateRect_.top),
                                      std::round(commitGenerateRect_.right), std::round(commitGenerateRect_.bottom));
    D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(snapped, 5.0f, 5.0f);
    if (fillBrush)
        ctx->FillRoundedRectangle(rounded, fillBrush);
    if (borderBrush)
        ctx->DrawRoundedRectangle(rounded, borderBrush, 1.0f);

    ctx->SetAntialiasMode(oldAA);

    if (textFormat && textBrush)
        ctx->DrawTextW(label, (UINT32)wcslen(label), textFormat, commitGenerateRect_, textBrush);

    if (fillBrush) fillBrush->Release();
    if (borderBrush) borderBrush->Release();
    if (textBrush) textBrush->Release();
    if (textFormat) textFormat->Release();
}

void GitPanel::DrawQuickActions(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd)
{
    (void)hwnd;
    if (quickActionPrimaryRect_.right <= quickActionPrimaryRect_.left)
        return;

    const bool light = (UI::Theme::GetMode() == UI::Theme::Mode::Light);
    D2D1_COLOR_F accent = UI::Theme::Accent();
    auto clamp1 = [](float v) { return v > 1.0f ? 1.0f : v; };
    D2D1_COLOR_F accentHov = D2D1::ColorF(
        clamp1(accent.r + 0.10f),
        clamp1(accent.g + 0.10f),
        clamp1(accent.b + 0.10f),
        1.0f);

    // Secondary buttons: glass-style — subtle transparent fill + crisp border
    D2D1_COLOR_F secBg  = light ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.06f)
                                : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.05f);
    D2D1_COLOR_F secHov = light ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.12f)
                                : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f);
    D2D1_COLOR_F secBd  = light ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.22f)
                                : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f);
    D2D1_COLOR_F secTxt = light ? D2D1::ColorF(0.15f, 0.15f, 0.15f, 1.0f)
                                : D2D1::ColorF(0.82f, 0.82f, 0.82f, 1.0f);

    ID2D1SolidColorBrush *primBgBrush  = nullptr;
    ID2D1SolidColorBrush *primHovBrush = nullptr;
    ID2D1SolidColorBrush *primTxtBrush = nullptr;
    ID2D1SolidColorBrush *secBgBrush   = nullptr;
    ID2D1SolidColorBrush *secHovBrush  = nullptr;
    ID2D1SolidColorBrush *secBdBrush   = nullptr;
    ID2D1SolidColorBrush *secTxtBrush  = nullptr;

    ctx->CreateSolidColorBrush(accent,    &primBgBrush);
    ctx->CreateSolidColorBrush(accentHov, &primHovBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.97f), &primTxtBrush);
    ctx->CreateSolidColorBrush(secBg,  &secBgBrush);
    ctx->CreateSolidColorBrush(secHov, &secHovBrush);
    ctx->CreateSolidColorBrush(secBd,  &secBdBrush);
    ctx->CreateSolidColorBrush(secTxt, &secTxtBrush);

    IDWriteTextFormat *btnFmt = nullptr;
    IDWriteTextFormat *secFmt = nullptr;
    dwrite->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                             DWRITE_FONT_STRETCH_NORMAL, 12.5f, L"en-us", &btnFmt);
    dwrite->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                             DWRITE_FONT_STRETCH_NORMAL, 11.5f, L"en-us", &secFmt);
    if (btnFmt)
    {
        btnFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        btnFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        btnFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    if (secFmt)
    {
        secFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        secFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        secFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    const float kR = 6.0f;
    D2D1_ANTIALIAS_MODE savedAA = ctx->GetAntialiasMode();
    ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    // Primary "Commit & Push" full-width accent button
    {
        const D2D1_RECT_F &r = quickActionPrimaryRect_;
        D2D1_RECT_F rr = D2D1::RectF(std::round(r.left), std::round(r.top),
                                      std::round(r.right), std::round(r.bottom));
        auto *bg = quickActionPrimaryHovered_ ? primHovBrush : primBgBrush;
        if (bg) ctx->FillRoundedRectangle(D2D1::RoundedRect(rr, kR, kR), bg);
    }

    // Secondary "Commit" button (left half)
    if (quickActionToggleRect_.right > quickActionToggleRect_.left)
    {
        const D2D1_RECT_F &r = quickActionToggleRect_;
        D2D1_RECT_F rr = D2D1::RectF(std::round(r.left), std::round(r.top),
                                      std::round(r.right), std::round(r.bottom));
        auto *bg = quickActionToggleHovered_ ? secHovBrush : secBgBrush;
        if (bg)       ctx->FillRoundedRectangle(D2D1::RoundedRect(rr, kR, kR), bg);
        if (secBdBrush) ctx->DrawRoundedRectangle(D2D1::RoundedRect(rr, kR, kR), secBdBrush, 1.0f);
    }

    // Secondary "Push" button (right half)
    if (quickActionMenuRect_.right > quickActionMenuRect_.left)
    {
        const D2D1_RECT_F &r = quickActionMenuRect_;
        D2D1_RECT_F rr = D2D1::RectF(std::round(r.left), std::round(r.top),
                                      std::round(r.right), std::round(r.bottom));
        bool pushHov = (quickActionHoveredIndex_ == 2);
        auto *bg = pushHov ? secHovBrush : secBgBrush;
        if (bg)       ctx->FillRoundedRectangle(D2D1::RoundedRect(rr, kR, kR), bg);
        if (secBdBrush) ctx->DrawRoundedRectangle(D2D1::RoundedRect(rr, kR, kR), secBdBrush, 1.0f);
    }

    ctx->SetAntialiasMode(savedAA);

    // Labels
    if (btnFmt && primTxtBrush)
        ctx->DrawTextW(L"Commit & Push", 13, btnFmt, quickActionPrimaryRect_, primTxtBrush);

    if (secFmt && secTxtBrush)
    {
        if (quickActionToggleRect_.right > quickActionToggleRect_.left)
            ctx->DrawTextW(L"Commit", 6, secFmt, quickActionToggleRect_, secTxtBrush);
        if (quickActionMenuRect_.right > quickActionMenuRect_.left)
            ctx->DrawTextW(L"Push", 4, secFmt, quickActionMenuRect_, secTxtBrush);
    }

    if (primBgBrush)  primBgBrush->Release();
    if (primHovBrush) primHovBrush->Release();
    if (primTxtBrush) primTxtBrush->Release();
    if (secBgBrush)   secBgBrush->Release();
    if (secHovBrush)  secHovBrush->Release();
    if (secBdBrush)   secBdBrush->Release();
    if (secTxtBrush)  secTxtBrush->Release();
    if (btnFmt) btnFmt->Release();
    if (secFmt) secFmt->Release();
}

void GitPanel::DrawChanges(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd)
{
    const UI::Theme::Palette &palette = UI::Theme::GetPalette();
    const bool light = (UI::Theme::GetMode() == UI::Theme::Mode::Light);

    ID2D1SolidColorBrush *pathBrush     = nullptr;
    ID2D1SolidColorBrush *dimBrush      = nullptr;
    ID2D1SolidColorBrush *hoverBrush    = nullptr;
    ID2D1SolidColorBrush *selectedBrush = nullptr;
    ID2D1SolidColorBrush *sectionBrush  = nullptr;
    ID2D1SolidColorBrush *statusBrush   = nullptr;

    D2D1_COLOR_F primary = UI::Theme::PrimaryText();
    ctx->CreateSolidColorBrush(primary, &pathBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(primary.r, primary.g, primary.b, 0.45f), &dimBrush);
    ctx->CreateSolidColorBrush(palette.explorerRowHover, &hoverBrush);
    ctx->CreateSolidColorBrush(palette.explorerRowActive, &selectedBrush);
    ctx->CreateSolidColorBrush(D2D1::ColorF(primary.r, primary.g, primary.b, 0.06f), &sectionBrush);
    ctx->CreateSolidColorBrush(UI::Theme::Accent(), &statusBrush);

    IDWriteTextFormat *rowFmt     = nullptr;
    IDWriteTextFormat *dimFmt     = nullptr;
    IDWriteTextFormat *sectionFmt = nullptr;

    dwrite->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                             DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &rowFmt);
    dwrite->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                             DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"en-us", &dimFmt);
    dwrite->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                             DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"en-us", &sectionFmt);

    for (IDWriteTextFormat *f : {rowFmt, dimFmt, sectionFmt})
    {
        if (!f) continue;
        f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    D2D1_RECT_F listClip = D2D1::RectF(changesRect_.left, changesRect_.top, changesRect_.right, changesRect_.bottom);
    ctx->PushAxisAlignedClip(listClip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    const float scroll = changesScrollbar_.GetScrollOffset();
    const float rowRightInset = changesScrollbar_.IsVisible() ? 16.0f : 4.0f;
    const float insetX = 4.0f;
    const float corner = UI::InputTheme::kCornerRadius;
    const float iconPx = (float)win32_dpi_scale((int)GetExplorerManager().GetState().iconSize, win32_get_dpi_for_window(hwnd));

    auto drawRoundedFill = [&](const D2D1_RECT_F &r, ID2D1Brush *brush)
    {
        if (!brush) return;
        D2D1_RECT_F rr = D2D1::RectF(std::round(r.left), std::round(r.top), std::round(r.right), std::round(r.bottom));
        D2D1_ANTIALIAS_MODE oldAA = ctx->GetAntialiasMode();
        ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        ctx->FillRoundedRectangle(D2D1::RoundedRect(rr, corner, corner), brush);
        ctx->SetAntialiasMode(oldAA);
    };

    // Helper: pick status color for a change
    auto statusColor = [&](const Panels::GitChange &c) -> D2D1_COLOR_F
    {
        if (c.indexStatus == L'U' || c.worktreeStatus == L'U' || (c.statusFlags & GIT_STATUS_CONFLICTED))
            return light ? D2D1::ColorF(0.85f, 0.25f, 0.15f) : D2D1::ColorF(0.95f, 0.45f, 0.35f);
        if (c.indexStatus == L'D' || c.worktreeStatus == L'D')
            return light ? D2D1::ColorF(0.78f, 0.24f, 0.24f) : D2D1::ColorF(0.92f, 0.37f, 0.37f);
        if (c.indexStatus == L'A')
            return light ? D2D1::ColorF(0.18f, 0.58f, 0.24f) : D2D1::ColorF(0.42f, 0.78f, 0.38f);
        if (c.indexStatus == L'?' || c.worktreeStatus == L'?')
            return light ? D2D1::ColorF(0.58f, 0.52f, 0.12f) : D2D1::ColorF(0.73f, 0.84f, 0.40f);
        if (c.indexStatus == L'M' || c.worktreeStatus == L'M')
            return light ? D2D1::ColorF(0.17f, 0.45f, 0.82f) : D2D1::ColorF(0.39f, 0.67f, 0.93f);
        if (c.indexStatus == L'R' || c.worktreeStatus == L'R')
            return light ? D2D1::ColorF(0.52f, 0.18f, 0.72f) : D2D1::ColorF(0.72f, 0.45f, 0.92f);
        return light ? D2D1::ColorF(0.18f, 0.58f, 0.24f) : D2D1::ColorF(0.42f, 0.78f, 0.38f);
    };

    // Helper: pick display status letter
    auto statusLetter = [&](const Panels::GitChange &c) -> wchar_t
    {
        if (c.statusFlags & GIT_STATUS_CONFLICTED) return L'C';
        if (c.indexStatus != L' ')                 return c.indexStatus;
        return c.worktreeStatus;
    };

    const wchar_t *sectionNames[3] = {L"MERGE CONFLICTS", L"STAGED CHANGES", L"CHANGES"};
    const std::vector<int> *sectionItems[3] = {&conflictIndices_, &stagedIndices_, &changesIndices_};

    float virtualY = 0.0f; // running position in virtual (scrolled) space

    for (int s = 0; s < 3; ++s)
    {
        if (sectionItems[s]->empty())
            continue;

        // --- Section header ---
        float headerTop    = listClip.top + virtualY - scroll;
        float headerBottom = headerTop + kSectionHeaderH;
        virtualY += kSectionHeaderH;

        if (headerBottom >= listClip.top && headerTop <= listClip.bottom)
        {
            D2D1_RECT_F hdrRect = D2D1::RectF(listClip.left, headerTop, listClip.right - rowRightInset, headerBottom);

            // Subtle background for section row
            if (sectionBrush)
                ctx->FillRectangle(D2D1::RectF(hdrRect.left, std::round(headerTop),
                                               hdrRect.right, std::round(headerBottom)), sectionBrush);

            // Hover highlight for section
            if (s == hoveredSectionIndex_ && hoverBrush)
                ctx->FillRectangle(D2D1::RectF(hdrRect.left, std::round(headerTop),
                                               hdrRect.right, std::round(headerBottom)), hoverBrush);

            // Chevron: ▶ or ▼
            if (dimBrush && sectionFmt)
            {
                const wchar_t *chevron = sectionCollapsed_[s] ? L"\u25B6" : L"\u25BC";
                D2D1_RECT_F chevRect = D2D1::RectF(hdrRect.left + insetX, headerTop,
                                                   hdrRect.left + insetX + 14.0f, headerBottom);
                ctx->DrawTextW(chevron, 1, sectionFmt, chevRect, dimBrush);
            }

            // Section name
            if (sectionFmt && dimBrush)
            {
                D2D1_RECT_F nameRect = D2D1::RectF(hdrRect.left + insetX + 16.0f, headerTop,
                                                   hdrRect.right - 36.0f, headerBottom);
                ctx->DrawTextW(sectionNames[s], (UINT32)wcslen(sectionNames[s]), sectionFmt, nameRect, dimBrush);
            }

            // Count badge (pill with number)
            if (statusBrush)
            {
                std::wstring cnt = std::to_wstring(sectionItems[s]->size());
                float badgeW = (float)cnt.size() * 7.0f + 10.0f;
                if (badgeW < 18.0f) badgeW = 18.0f;
                D2D1_RECT_F badgeRect = D2D1::RectF(
                    hdrRect.right - badgeW - 4.0f,
                    headerTop + (kSectionHeaderH - 14.0f) * 0.5f,
                    hdrRect.right - 4.0f,
                    headerTop + (kSectionHeaderH + 14.0f) * 0.5f);

                // Badge background
                D2D1_COLOR_F badgeBg = statusColor(changes_[(size_t)(*sectionItems[s])[0]]);
                badgeBg.a = 0.18f;
                ID2D1SolidColorBrush *badgeBgBrush = nullptr;
                ctx->CreateSolidColorBrush(badgeBg, &badgeBgBrush);
                if (badgeBgBrush)
                {
                    D2D1_ANTIALIAS_MODE oldAA = ctx->GetAntialiasMode();
                    ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                    ctx->FillRoundedRectangle(D2D1::RoundedRect(badgeRect, 7.0f, 7.0f), badgeBgBrush);
                    ctx->SetAntialiasMode(oldAA);
                    badgeBgBrush->Release();
                }

                // Badge text
                ID2D1SolidColorBrush *badgeTextBrush = nullptr;
                D2D1_COLOR_F badgeFgColor = statusColor(changes_[(size_t)(*sectionItems[s])[0]]);
                ctx->CreateSolidColorBrush(badgeFgColor, &badgeTextBrush);
                if (badgeTextBrush)
                {
                    IDWriteTextFormat *centeredBadgeFmt = nullptr;
                    dwrite->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                            DWRITE_FONT_STRETCH_NORMAL, 10.0f, L"en-us", &centeredBadgeFmt);
                    if (centeredBadgeFmt)
                    {
                        centeredBadgeFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                        centeredBadgeFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                        centeredBadgeFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                        ctx->DrawTextW(cnt.c_str(), (UINT32)cnt.size(), centeredBadgeFmt, badgeRect, badgeTextBrush);
                        centeredBadgeFmt->Release();
                    }
                    badgeTextBrush->Release();
                }
            }
        }

        if (sectionCollapsed_[s])
            continue;

        // --- File rows for this section ---
        for (int idx : *sectionItems[s])
        {
            float rowTop    = listClip.top + virtualY - scroll;
            float rowBottom = rowTop + changeRowHeight_;
            virtualY += changeRowHeight_;

            if (rowBottom < listClip.top)
                continue;
            if (rowTop > listClip.bottom)
                break;

            const Panels::GitChange &c = changes_[(size_t)idx];
            D2D1_RECT_F rowRect  = D2D1::RectF(listClip.left, rowTop, listClip.right - rowRightInset, rowBottom);
            D2D1_RECT_F fillRect = D2D1::RectF(rowRect.left + insetX, std::round(rowTop),
                                               rowRect.right - insetX, std::round(rowBottom));

            if (idx == selectedChangeIndex_ && selectedBrush)
                drawRoundedFill(fillRect, selectedBrush);
            else if (idx == hoveredChangeIndex_ && hoverBrush)
                drawRoundedFill(fillRect, hoverBrush);

            // File icon
            std::filesystem::path relPath(c.path);
            ExplorerItem iconItem;
            iconItem.isDirectory = false;
            iconItem.extension   = relPath.extension().string();
            iconItem.name        = relPath.filename().wstring();
            iconItem.fullPath    = c.path;
            ID2D1Bitmap *icon = GetExplorerManager().GetIconForItemPublic(ctx, iconItem, hwnd);

            float iconX = rowRect.left + insetX + state_.leftPadding + 12.0f; // extra indent inside section
            if (icon)
            {
                float iconY = std::round(rowTop + (changeRowHeight_ - iconPx) * 0.5f);
                D2D1_RECT_F iconRect = D2D1::RectF(std::round(iconX), iconY,
                                                   std::round(iconX + iconPx), iconY + iconPx);
                ctx->DrawBitmap(icon, iconRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            }

            float textX = iconX + iconPx + 6.0f;

            // Status badge on the right (single colored letter)
            wchar_t letter = statusLetter(c);
            D2D1_COLOR_F sColor = statusColor(c);
            const float badgeSz = 18.0f;
            D2D1_RECT_F badgeRect = D2D1::RectF(
                rowRect.right - badgeSz - 4.0f,
                rowTop + (changeRowHeight_ - badgeSz) * 0.5f,
                rowRect.right - 4.0f,
                rowTop + (changeRowHeight_ + badgeSz) * 0.5f);

            ID2D1SolidColorBrush *sBrush = nullptr;
            ctx->CreateSolidColorBrush(sColor, &sBrush);
            if (sBrush)
            {
                // Badge background (subtle tint)
                D2D1_COLOR_F bgCol = sColor;
                bgCol.a = 0.15f;
                ID2D1SolidColorBrush *bgBrush = nullptr;
                ctx->CreateSolidColorBrush(bgCol, &bgBrush);
                if (bgBrush)
                {
                    D2D1_ANTIALIAS_MODE oldAA = ctx->GetAntialiasMode();
                    ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                    ctx->FillRoundedRectangle(D2D1::RoundedRect(badgeRect, 3.0f, 3.0f), bgBrush);
                    ctx->SetAntialiasMode(oldAA);
                    bgBrush->Release();
                }

                // Badge letter
                IDWriteTextFormat *badgeLetterFmt = nullptr;
                dwrite->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, 10.0f, L"en-us", &badgeLetterFmt);
                if (badgeLetterFmt)
                {
                    badgeLetterFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    badgeLetterFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    badgeLetterFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                    ctx->DrawTextW(&letter, 1, badgeLetterFmt, badgeRect, sBrush);
                    badgeLetterFmt->Release();
                }
                sBrush->Release();
            }

            // Filename (just the filename, bold)
            float nameRight = badgeRect.left - 6.0f;
            std::wstring filename = relPath.filename().wstring();
            std::wstring parentDir = relPath.parent_path().wstring();
            const float contentWidth = nameRight - textX;
            if (contentWidth <= 1.0f)
                continue;

            const float labelGap = 6.0f;
            const float minPathWidth = 72.0f;
            const float minNameWidth = 96.0f;
            float nameWidth = contentWidth;

            if (!parentDir.empty() && contentWidth > (minNameWidth + minPathWidth + labelGap))
            {
                nameWidth = (std::min)(contentWidth * 0.58f, contentWidth - minPathWidth - labelGap);
                nameWidth = (std::max)(minNameWidth, nameWidth);
                nameWidth = (std::min)(nameWidth, contentWidth - minPathWidth - labelGap);
            }

            if (rowFmt && pathBrush && !filename.empty())
            {
                D2D1_RECT_F nameRect = D2D1::RectF(textX, rowTop, textX + nameWidth, rowBottom);
                DrawTrimmedText(ctx, dwrite, filename, rowFmt, nameRect, pathBrush);
            }

            if (!parentDir.empty() && dimFmt && dimBrush)
            {
                float dirLeft = textX + nameWidth + labelGap;
                D2D1_RECT_F dirRect = D2D1::RectF(dirLeft, rowTop, nameRight, rowBottom);
                DrawTrimmedText(ctx, dwrite, parentDir, dimFmt, dirRect, dimBrush);
            }
        }
    }

    if (changes_.empty() && rowFmt && pathBrush)
    {
        D2D1_RECT_F emptyRect = D2D1::RectF(listClip.left + 12.0f, listClip.top + 12.0f,
                                             listClip.right - 12.0f, listClip.top + 32.0f);
        const wchar_t *msg = isGitRepo_ ? L"No pending changes." : L"Open a git repository to see changes.";
        ctx->DrawTextW(msg, (UINT32)wcslen(msg), rowFmt, emptyRect, dimBrush ? dimBrush : pathBrush);
    }

    ctx->PopAxisAlignedClip();

    if (rowFmt)     rowFmt->Release();
    if (dimFmt)     dimFmt->Release();
    if (sectionFmt) sectionFmt->Release();
    if (pathBrush)     pathBrush->Release();
    if (dimBrush)      dimBrush->Release();
    if (hoverBrush)    hoverBrush->Release();
    if (selectedBrush) selectedBrush->Release();
    if (sectionBrush)  sectionBrush->Release();
    if (statusBrush)   statusBrush->Release();
}

void GitPanel::OnMouseMove(HWND hwnd, POINT clientPoint)
{
    hwnd_ = hwnd;
    bool changed = HandleResizeMouseMove(hwnd, clientPoint);
    if (state_.isResizing)
        return;
    if (state_.isHoveringResizeZone)
        return;

    bool prevGenerateHover = commitGenerateHovered_;
    bool prevPrimaryHover = quickActionPrimaryHovered_;
    bool prevToggleHover  = quickActionToggleHovered_;
    int  prevQuickHover   = quickActionHoveredIndex_;
    bool prevPull         = pullBtnHovered_;
    bool prevRefresh      = refreshBtnHovered_;
    int  prevSection      = hoveredSectionIndex_;

    commitGenerateHovered_      = IsPointInRect(commitGenerateRect_, clientPoint);
    quickActionPrimaryHovered_ = IsPointInRect(quickActionPrimaryRect_, clientPoint);
    quickActionToggleHovered_  = IsPointInRect(quickActionToggleRect_,  clientPoint);
    quickActionHoveredIndex_   = IsPointInRect(quickActionMenuRect_, clientPoint) ? 2 : -1;
    pullBtnHovered_            = IsPointInRect(pullBtnRect_,    clientPoint);
    refreshBtnHovered_         = IsPointInRect(refreshBtnRect_, clientPoint);
    hoveredSectionIndex_       = HitTestSection(clientPoint);

    if (prevGenerateHover != commitGenerateHovered_      ||
        prevPrimaryHover != quickActionPrimaryHovered_ ||
        prevToggleHover  != quickActionToggleHovered_  ||
        prevQuickHover   != quickActionHoveredIndex_   ||
        prevPull         != pullBtnHovered_            ||
        prevRefresh      != refreshBtnHovered_         ||
        prevSection      != hoveredSectionIndex_)
        changed = true;

    if (commitMessageInput_.OnMouseMove(hwnd, clientPoint))
        changed = true;

    if (changesScrollbar_.OnMouseMove(clientPoint))
        changed = true;

    int prevHover = hoveredChangeIndex_;
    if (changesScrollbar_.IsHoveringThumb() ||
        changesScrollbar_.IsHoveringTrack()  ||
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
    hwnd_ = hwnd;
    if (HandleResizeLeftButtonDown(hwnd, clientPoint))
        return;

    // Branch bar buttons
    if (IsPointInRect(pullBtnRect_, clientPoint))
    {
        PullFromRemote();
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    if (IsPointInRect(refreshBtnRect_, clientPoint))
    {
        RefreshStatus();
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    // Section header collapse/expand
    int secHit = HitTestSection(clientPoint);
    if (secHit >= 0)
    {
        sectionCollapsed_[secHit] = !sectionCollapsed_[secHit];
        // Recompute scroll content size
        UpdateLayout(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    if (IsPointInRect(commitGenerateRect_, clientPoint))
    {
        StartCommitMessageGeneration();
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    if (IsPointInRect(quickActionPrimaryRect_, clientPoint))
    {
        ExecuteQuickAction(0); // Commit & Push
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    if (IsPointInRect(quickActionToggleRect_, clientPoint))
    {
        ExecuteQuickAction(1); // Commit only
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    if (IsPointInRect(quickActionMenuRect_, clientPoint))
    {
        ExecuteQuickAction(2); // Push only
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
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
    hwnd_ = hwnd;
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
    hwnd_ = hwnd;
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

    float scroll  = changesScrollbar_.GetScrollOffset();
    float localY  = (float)pt.y - changesRect_.top + scroll;
    if (localY < 0.0f)
        return -1;

    const std::vector<int> *sectionItems[3] = {&conflictIndices_, &stagedIndices_, &changesIndices_};
    float y = 0.0f;
    for (int s = 0; s < 3; ++s)
    {
        if (sectionItems[s]->empty())
            continue;
        // Skip section header
        if (localY < y + kSectionHeaderH)
            return -1; // hit section header, not a file row
        y += kSectionHeaderH;
        if (sectionCollapsed_[s])
            continue;
        for (int idx : *sectionItems[s])
        {
            if (localY < y + changeRowHeight_)
                return idx;
            y += changeRowHeight_;
        }
    }
    return -1;
}

int GitPanel::HitTestSection(POINT pt) const
{
    if (!IsPointInRect(changesRect_, pt))
        return -1;

    float scroll = changesScrollbar_.GetScrollOffset();
    float localY = (float)pt.y - changesRect_.top + scroll;
    if (localY < 0.0f)
        return -1;

    const std::vector<int> *sectionItems[3] = {&conflictIndices_, &stagedIndices_, &changesIndices_};
    float y = 0.0f;
    for (int s = 0; s < 3; ++s)
    {
        if (sectionItems[s]->empty())
            continue;
        if (localY < y + kSectionHeaderH)
            return s;
        y += kSectionHeaderH;
        if (!sectionCollapsed_[s])
            y += (float)sectionItems[s]->size() * changeRowHeight_;
    }
    return -1;
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

    const std::wstring explorerRoot = Trim(GetExplorerManager().GetState().rootPath);
    std::wstring candidate = explorerRoot.empty() ? Trim(repoRoot_) : explorerRoot;

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

    // Categorize into sections
    conflictIndices_.clear();
    stagedIndices_.clear();
    changesIndices_.clear();
    for (int i = 0; i < (int)changes_.size(); ++i)
    {
        const auto &c = changes_[(size_t)i];
        bool conflict = (c.statusFlags & GIT_STATUS_CONFLICTED) != 0;
        if (conflict)
        {
            conflictIndices_.push_back(i);
        }
        else if (c.indexStatus != L' ')
        {
            stagedIndices_.push_back(i);
            if (c.worktreeStatus != L' ')
                changesIndices_.push_back(i); // also in worktree
        }
        else if (c.worktreeStatus != L' ')
        {
            changesIndices_.push_back(i);
        }
    }

    RefreshBranchInfo();
}

void GitPanel::RefreshBranchInfo()
{
    currentBranch_.clear();
    aheadCount_  = 0;
    behindCount_ = 0;

    if (!isGitRepo_ || repoRoot_.empty())
        return;

    git_repository *repo = nullptr;
    if (git_repository_open_ext(&repo, WideToUtf8(repoRoot_).c_str(), GIT_REPOSITORY_OPEN_CROSS_FS, nullptr) != 0 || !repo)
        return;

    git_reference *headRef = nullptr;
    if (git_repository_head(&headRef, repo) == 0 && headRef)
    {
        const char *name = nullptr;
        if (git_branch_name(&name, headRef) == 0 && name)
            currentBranch_ = Utf8ToWide(name);

        if (!currentBranch_.empty())
        {
            git_buf upstreamName = GIT_BUF_INIT;
            if (git_branch_upstream_name(&upstreamName, repo, git_reference_name(headRef)) == 0 && upstreamName.ptr)
            {
                git_reference *upstreamRef = nullptr;
                if (git_reference_lookup(&upstreamRef, repo, upstreamName.ptr) == 0 && upstreamRef)
                {
                    const git_oid *localOid  = git_reference_target(headRef);
                    const git_oid *remoteOid = git_reference_target(upstreamRef);
                    if (localOid && remoteOid)
                    {
                        size_t ahead = 0, behind = 0;
                        if (git_graph_ahead_behind(&ahead, &behind, repo, localOid, remoteOid) == 0)
                        {
                            aheadCount_  = (int)ahead;
                            behindCount_ = (int)behind;
                        }
                    }
                    git_reference_free(upstreamRef);
                }
            }
            git_buf_dispose(&upstreamName);
        }

        git_reference_free(headRef);
    }

    git_repository_free(repo);
}

void GitPanel::StartCommitMessageGeneration()
{
    if (commitGenerationInFlight_ || claudeBridge_.IsBusy())
        return;

    if (!isGitRepo_ || repoRoot_.empty())
    {
        lastError_ = L"Open a git repository before generating a commit message.";
        InvalidatePanel();
        return;
    }

    if (changes_.empty())
    {
        lastError_ = L"No pending changes to summarize.";
        InvalidatePanel();
        return;
    }

    const std::wstring executablePath = ResolveClaudeExecutablePath();
    if (executablePath.empty())
    {
        lastError_ = L"Claude CLI not configured. Configure it from the Claude panel first.";
        InvalidatePanel();
        return;
    }

    ClaudeCliBridge::RequestOptions options;
    options.executablePath = executablePath;
    options.workingDirectory = repoRoot_;
    options.prompt = BuildCommitGenerationPrompt();

    if (!claudeBridge_.StartRequest(options))
    {
        lastError_ = L"A Claude request is already running.";
        InvalidatePanel();
    }
}

bool GitPanel::FetchFromRemote()
{
    if (!libgit2Ready_ || !isGitRepo_ || repoRoot_.empty())
    {
        lastError_ = L"No git repository.";
        return false;
    }

    git_repository *repo = nullptr;
    if (git_repository_open_ext(&repo, WideToUtf8(repoRoot_).c_str(), GIT_REPOSITORY_OPEN_CROSS_FS, nullptr) != 0 || !repo)
    {
        lastError_ = GetLastGitError(L"Unable to open repository for fetch.");
        return false;
    }

    // Find remote (prefer origin)
    git_remote *remote = nullptr;
    if (git_remote_lookup(&remote, repo, "origin") != 0 || !remote)
    {
        git_strarray remotes = {0};
        if (git_remote_list(&remotes, repo) == 0 && remotes.count > 0 && remotes.strings[0])
            git_remote_lookup(&remote, repo, remotes.strings[0]);
        git_strarray_dispose(&remotes);
    }

    if (!remote)
    {
        lastError_ = L"No remote to fetch from.";
        git_repository_free(repo);
        return false;
    }

    // Load auth token (optional – public repos work without it)
    std::wstring tokenWide;
    std::wstring authErr;
    GitHubAuth::LoadToken(tokenWide, authErr);

    PushCredentialContext credCtx;
    if (!tokenWide.empty())
    {
        credCtx.token = WideToUtf8(tokenWide);
        SecureZeroMemory(tokenWide.data(), tokenWide.size() * sizeof(wchar_t));
    }

    git_fetch_options fetchOpts = GIT_FETCH_OPTIONS_INIT;
    if (!credCtx.token.empty())
    {
        fetchOpts.callbacks.credentials = AcquirePushCredentials;
        fetchOpts.callbacks.payload     = &credCtx;
    }

    int rc = git_remote_fetch(remote, nullptr, &fetchOpts, "fetch");

    git_remote_free(remote);
    git_repository_free(repo);

    if (!credCtx.token.empty())
        SecureZeroMemory(credCtx.token.data(), credCtx.token.size());

    if (rc != 0)
    {
        lastError_ = GetLastGitError(L"Fetch failed.");
        return false;
    }

    lastError_.clear();
    RefreshBranchInfo();
    return true;
}

bool GitPanel::PullFromRemote()
{
    // Step 1: fetch
    if (!FetchFromRemote())
        return false;

    if (!isGitRepo_ || repoRoot_.empty())
        return false;

    git_repository *repo = nullptr;
    if (git_repository_open_ext(&repo, WideToUtf8(repoRoot_).c_str(), GIT_REPOSITORY_OPEN_CROSS_FS, nullptr) != 0 || !repo)
    {
        lastError_ = GetLastGitError(L"Unable to open repository for pull.");
        return false;
    }

    git_reference *headRef = nullptr;
    if (git_repository_head(&headRef, repo) != 0 || !headRef)
    {
        lastError_ = GetLastGitError(L"Unable to resolve HEAD for pull.");
        git_repository_free(repo);
        return false;
    }

    bool ok = false;

    git_buf upstreamName = GIT_BUF_INIT;
    if (git_branch_upstream_name(&upstreamName, repo, git_reference_name(headRef)) == 0 && upstreamName.ptr)
    {
        git_reference *upstreamRef = nullptr;
        if (git_reference_lookup(&upstreamRef, repo, upstreamName.ptr) == 0 && upstreamRef)
        {
            const git_oid *localOid  = git_reference_target(headRef);
            const git_oid *remoteOid = git_reference_target(upstreamRef);

            if (localOid && remoteOid)
            {
                size_t ahead = 0, behind = 0;
                if (git_graph_ahead_behind(&ahead, &behind, repo, localOid, remoteOid) == 0)
                {
                    if (behind == 0)
                    {
                        // Already up to date
                        ok = true;
                        lastError_.clear();
                    }
                    else if (ahead > 0)
                    {
                        lastError_ = L"Cannot pull: local and remote have diverged. Please merge manually.";
                    }
                    else
                    {
                        // Pure fast-forward: move branch ref + checkout
                        git_reference *newRef = nullptr;
                        if (git_reference_set_target(&newRef, headRef, remoteOid, "pull: Fast-forward") == 0)
                        {
                            git_object *remoteObj = nullptr;
                            if (git_object_lookup(&remoteObj, repo, remoteOid, GIT_OBJECT_COMMIT) == 0)
                            {
                                git_checkout_options coOpts = GIT_CHECKOUT_OPTIONS_INIT;
                                coOpts.checkout_strategy = GIT_CHECKOUT_SAFE;
                                git_checkout_tree(repo, remoteObj, &coOpts);
                                git_object_free(remoteObj);
                            }
                            if (newRef) git_reference_free(newRef);
                            ok = true;
                            lastError_.clear();
                        }
                        else
                        {
                            lastError_ = GetLastGitError(L"Fast-forward failed.");
                        }
                    }
                }
            }
            git_reference_free(upstreamRef);
        }
        else
        {
            lastError_ = L"No upstream branch configured for pull.";
        }
    }
    else
    {
        lastError_ = L"No upstream branch configured for pull.";
    }

    git_buf_dispose(&upstreamName);
    git_reference_free(headRef);
    git_repository_free(repo);

    RefreshStatus();
    return ok;
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

void GitPanel::InvalidatePanel() const
{
    if (hwnd_)
        InvalidateRect(hwnd_, nullptr, FALSE);
}

std::wstring GitPanel::BuildCommitGenerationPrompt() const
{
    std::wstringstream prompt;
    prompt << L"You are generating a git commit message for the current repository.\n";
    prompt << L"Output only the commit message subject line.\n";
    prompt << L"No quotes. No markdown. No explanation. No bullets.\n";
    prompt << L"Use imperative mood and keep it under 72 characters.\n";
    prompt << L"Nebula will stage all current changes before creating the commit.\n";
    prompt << L"You may inspect the repository with the available tools if needed.\n\n";

    if (!repoRoot_.empty())
        prompt << L"Repository root: " << repoRoot_ << L"\n";
    if (!currentBranch_.empty())
        prompt << L"Current branch: " << currentBranch_ << L"\n";

    prompt << L"\nCurrent changes:\n";
    for (const Panels::GitChange &change : changes_)
    {
        if (change.statusFlags & GIT_STATUS_CONFLICTED)
        {
            prompt << L"- conflict: " << change.path << L"\n";
            continue;
        }

        bool wroteAnyLabel = false;
        if (change.indexStatus != L' ')
        {
            prompt << L"- " << StatusLabel(change.indexStatus, true) << L": " << change.path;
            wroteAnyLabel = true;
        }
        if (change.worktreeStatus != L' ')
        {
            prompt << (wroteAnyLabel ? L" | " : L"- ");
            prompt << StatusLabel(change.worktreeStatus, false) << L": " << change.path;
        }
        prompt << L"\n";
    }

    prompt << L"\nReturn the final commit message only.";
    return prompt.str();
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

std::wstring GitPanel::NormalizeCommitMessage(const std::wstring &text)
{
    std::wstring trimmed = Trim(text);
    if (trimmed.empty())
        return {};

    std::wstringstream input(trimmed);
    std::wstring line;
    while (std::getline(input, line))
    {
        line = Trim(line);
        if (line.empty() || line == L"```")
            continue;

        if (line.size() >= 2 && ((line.front() == L'"' && line.back() == L'"') ||
                                 (line.front() == L'\'' && line.back() == L'\'')))
            line = Trim(line.substr(1, line.size() - 2));

        if (line.rfind(L"```", 0) == 0)
            continue;
        if (line.rfind(L"- ", 0) == 0 || line.rfind(L"* ", 0) == 0)
            line = Trim(line.substr(2));
        if (line.rfind(L"Commit message:", 0) == 0)
            line = Trim(line.substr(15));

        if (line.empty())
            continue;

        trimmed = line;
        break;
    }

    std::wstring normalized;
    normalized.reserve(trimmed.size());
    bool previousSpace = false;
    for (wchar_t ch : trimmed)
    {
        if (ch == L'\r' || ch == L'\n' || ch == L'\t')
            ch = L' ';
        if (iswspace(ch))
        {
            if (!previousSpace)
                normalized.push_back(L' ');
            previousSpace = true;
        }
        else
        {
            normalized.push_back(ch);
            previousSpace = false;
        }
    }
    return Trim(normalized);
}

std::wstring GitPanel::StatusLabel(wchar_t status, bool staged)
{
    switch (status)
    {
    case L'A': return staged ? L"staged added" : L"added";
    case L'M': return staged ? L"staged modified" : L"modified";
    case L'D': return staged ? L"staged deleted" : L"deleted";
    case L'R': return staged ? L"staged renamed" : L"renamed";
    case L'T': return staged ? L"staged type changed" : L"type changed";
    case L'?': return L"untracked";
    case L'U': return L"conflict";
    case L'!': return L"unreadable";
    default:   return staged ? L"staged changed" : L"changed";
    }
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

std::wstring GitPanel::GetClaudeSettingsPath()
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

std::wstring GitPanel::ResolveClaudeExecutablePath()
{
    const std::wstring settingsPath = GetClaudeSettingsPath();
    if (!settingsPath.empty())
    {
        std::wifstream in{std::filesystem::path(settingsPath)};
        if (in.is_open())
        {
            std::wstring configured;
            std::getline(in, configured);
            configured = Trim(configured);
            if (!configured.empty())
                return configured;
        }
    }

    return DetectClaudeExecutable();
}
