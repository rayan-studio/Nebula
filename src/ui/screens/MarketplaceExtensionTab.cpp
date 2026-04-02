#include "MarketplaceExtensionTab.h"

#include "core/explorer/Explorer.h"
#include "ui/marketplace/MarketplaceVisuals.h"
#include "ui/panels/LibraryDatabase.h"
#include "ui/theme/Theme.h"
#include "utils/logger/Logger.h"

#include <winhttp.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static D2D1_COLOR_F CategoryColor(const std::wstring& cat)
{
    return MarketplaceCategoryColor(cat);
}

static std::wstring Initials(const std::wstring& name)
{
    return MarketplaceInitials(name);
}

static std::wstring FormatNumber(int n)
{
    if (n >= 1000) {
        float k = n / 1000.0f;
        wchar_t buf[32];
        if (k < 10.0f)
            swprintf_s(buf, L"%.1fk", k);
        else
            swprintf_s(buf, L"%.0fk", k);
        return buf;
    }
    return std::to_wstring(n);
}

// ---------------------------------------------------------------------------
// Static network helpers
// ---------------------------------------------------------------------------

bool MarketplaceExtensionTabView::ParseGitHubUrl(const std::wstring& gitUrl,
                                                  std::wstring& owner,
                                                  std::wstring& repo)
{
    const std::wstring prefix = L"https://github.com/";
    if (gitUrl.size() <= prefix.size()) return false;
    if (gitUrl.substr(0, prefix.size()) != prefix) return false;

    std::wstring rest = gitUrl.substr(prefix.size());
    // Strip .git suffix
    if (rest.size() > 4 && rest.substr(rest.size() - 4) == L".git")
        rest = rest.substr(0, rest.size() - 4);

    auto slash = rest.find(L'/');
    if (slash == std::wstring::npos) return false;

    owner = rest.substr(0, slash);
    repo  = rest.substr(slash + 1);
    return !owner.empty() && !repo.empty();
}

bool MarketplaceExtensionTabView::FetchRawUrl(const std::wstring& host,
                                               const std::wstring& path,
                                               std::string& outBody)
{
    outBody.clear();

    HINTERNET hSess = WinHttpOpen(L"Nebula/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSess) return false;

    HINTERNET hConn = WinHttpConnect(hSess, host.c_str(),
        INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConn) { WinHttpCloseHandle(hSess); return false; }

    HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", path.c_str(),
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hReq) {
        WinHttpCloseHandle(hConn);
        WinHttpCloseHandle(hSess);
        return false;
    }

    BOOL ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok) ok = WinHttpReceiveResponse(hReq, nullptr);

    bool success = false;
    if (ok) {
        DWORD status = 0;
        DWORD sz = sizeof(status);
        WinHttpQueryHeaders(hReq,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
            WINHTTP_NO_HEADER_INDEX);

        if (status == 200) {
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0) {
                std::vector<char> buf(avail);
                DWORD read = 0;
                WinHttpReadData(hReq, buf.data(), avail, &read);
                outBody.append(buf.data(), read);
            }
            success = true;
        }
    }

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConn);
    WinHttpCloseHandle(hSess);
    return success;
}

std::wstring MarketplaceExtensionTabView::NormalizeReadmeMarkdown(const std::wstring& md)
{
    std::wstring result;
    result.reserve(md.size());

    for (size_t i = 0; i < md.size(); ++i) {
        wchar_t ch = md[i];
        if (ch == L'\r') {
            if (i + 1 < md.size() && md[i + 1] == L'\n') i++;
            result.push_back(L'\n');
        } else {
            result.push_back(ch);
        }
    }

    size_t start = result.find_first_not_of(L"\n\t ");
    if (start != std::wstring::npos) {
        result = result.substr(start);
    } else {
        result.clear();
    }

    const size_t kMax = 120000;
    if (result.size() > kMax) {
        result.resize(kMax);
        result += L"\n\n...\n\nREADME truncated for preview.";
    }

    return result;
}

void MarketplaceExtensionTabView::FetchReadmeAsync(HWND hwnd,
                                                    const std::wstring& libName,
                                                    const std::wstring& gitUrl)
{
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        if (readmeCache_.count(libName)) return;
        if (lastFetchedFor_ == libName) return;
        lastFetchedFor_ = libName;
    }

    std::thread([this, hwnd, libName, gitUrl]() {
        std::wstring owner, repo;
        std::wstring content;

        if (ParseGitHubUrl(gitUrl, owner, repo)) {
            std::string body;
            for (const wchar_t* branch : { L"master", L"main" }) {
                std::wstring path = L"/" + owner + L"/" + repo + L"/" + branch + L"/README.md";
                if (FetchRawUrl(L"raw.githubusercontent.com", path, body))
                    break;
                body.clear();
            }

            if (!body.empty()) {
                int len = MultiByteToWideChar(CP_UTF8, 0, body.c_str(),
                    (int)body.size(), nullptr, 0);
                if (len > 0) {
                    std::wstring wide(len, L'\0');
                    MultiByteToWideChar(CP_UTF8, 0, body.c_str(),
                        (int)body.size(), wide.data(), len);
                    content = NormalizeReadmeMarkdown(wide);
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(cacheMutex_);
            readmeCache_[libName] = content;
        }

        if (hwnd) InvalidateRect(hwnd, nullptr, FALSE);
    }).detach();
}

// ---------------------------------------------------------------------------

void MarketplaceExtensionTabView::SetLibraryName(const std::wstring &name)
{
    if (currentLibraryName_ == name) return;
    currentLibraryName_ = name;
    actionButtonHovered_ = false;
    readmePreviewTabHovered_ = false;
    readmeMarkdownTabHovered_ = false;
    repositoryLinkHovered_ = false;
    readmePreviewEditor_.CancelInteraction();

    currentPreviewText_.clear();
    readmeMode_ = ReadmeMode::Preview;
    readmePreviewEditor_.CreateEmpty();
    readmePreviewEditor_.SetEmbeddedPreviewMode(true);
    readmePreviewEditor_.SetTextContent(L"Loading README...", false);
    ApplyReadmeEditorMode();

    if (!name.empty()) {
        LibraryInfo lib;
        if (LibraryDatabase::Instance().GetLibraryCopy(name, lib))
            FetchReadmeAsync(hwnd_, name, lib.gitUrl);
    }
}

void MarketplaceExtensionTabView::UpdateLayout(HWND hwnd, float left, float top,
                                                float right, float bottom)
{
    hwnd_ = hwnd;
    bounds_ = D2D1::RectF(left, top, right, bottom);

    const UINT dpi = GetDpiForWindow(hwnd);
    const float scale = dpi / 96.0f;
    const float pad   = 24.0f * scale;

    const float iconSz = 68.0f * scale;
    const float btnH   = 34.0f * scale;
    const float btnW   = 120.0f * scale;

    // Button: top-right, aligned with icon top
    actionButtonRect_ = D2D1::RectF(
        bounds_.right - pad - btnW,
        bounds_.top   + pad,
        bounds_.right - pad,
        bounds_.top   + pad + btnH);

    // README clip area: below header separator, above bottom stats block
    const float headerH  = pad + iconSz + 18.0f * scale;
    const float statsH   = 110.0f * scale;   // stats + repo sections at bottom
    readmeClipRect_ = D2D1::RectF(
        bounds_.left,
        bounds_.top + headerH,
        bounds_.right,
        bounds_.bottom - statsH);

    const float toggleTop = readmeClipRect_.top + 14.0f * scale;
    const float toggleH = 26.0f * scale;
    const float previewToggleW = 82.0f * scale;
    const float markdownToggleW = 96.0f * scale;

    readmePreviewTabRect_ = D2D1::RectF(
        bounds_.left + pad + 66.0f * scale,
        toggleTop,
        bounds_.left + pad + 66.0f * scale + previewToggleW,
        toggleTop + toggleH);
    readmeMarkdownTabRect_ = D2D1::RectF(
        readmePreviewTabRect_.right + 8.0f * scale,
        toggleTop,
        readmePreviewTabRect_.right + 8.0f * scale + markdownToggleW,
        toggleTop + toggleH);

    const float previewTop = toggleTop + toggleH + 12.0f * scale;
    readmePreviewEditor_.UpdateLayout(
        hwnd,
        bounds_.left + pad,
        previewTop,
        bounds_.right - pad,
        readmeClipRect_.bottom);

    cardRect_ = bounds_;
}

bool MarketplaceExtensionTabView::IsPointInRect(POINT pt, const D2D1_RECT_F &rect) const
{
    return pt.x >= rect.left && pt.x <= rect.right &&
           pt.y >= rect.top  && pt.y <= rect.bottom;
}

void MarketplaceExtensionTabView::ApplyReadmeEditorMode()
{
    readmePreviewEditor_.SetMarkdownViewMode(
        readmeMode_ == ReadmeMode::Preview
            ? Orion::MarkdownViewMode::Preview
            : Orion::MarkdownViewMode::Code);
}

// ---------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------
void MarketplaceExtensionTabView::Draw(ID2D1RenderTarget *ctx,
                                       IDWriteFactory *dwrite, HWND hwnd)
{
    if (!ctx || !dwrite) return;
    if (hwnd_ != hwnd) { hwnd_ = hwnd; }

    LibraryInfo library;
    LibraryInfo* lib = nullptr;
    if (!currentLibraryName_.empty() &&
        LibraryDatabase::Instance().GetLibraryCopy(currentLibraryName_, library))
    {
        lib = &library;
    }

    repositoryLinkRect_ = D2D1::RectF(0, 0, 0, 0);

    // ── Background ──────────────────────────────────────────────────────────
    {
        ID2D1SolidColorBrush *bg = nullptr;
        ctx->CreateSolidColorBrush(UI::Theme::ChromeBackground(), &bg);
        if (bg) { ctx->FillRectangle(bounds_, bg); bg->Release(); }
    }

    if (!lib) {
        IDWriteTextFormat *fmt = nullptr;
        dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &fmt);
        ID2D1SolidColorBrush *br = nullptr;
        ctx->CreateSolidColorBrush(UI::Theme::MutedText(), &br);
        if (fmt && br) {
            fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            const wchar_t *msg = L"Select a library to view details.";
            ctx->DrawTextW(msg, (UINT32)wcslen(msg), fmt, bounds_, br);
        }
        if (fmt) fmt->Release();
        if (br)  br->Release();
        return;
    }

    const UINT dpi   = GetDpiForWindow(hwnd);
    const float scale = dpi / 96.0f;
    const float pad   = 24.0f * scale;
    const float x     = bounds_.left  + pad;
    const float xEnd  = bounds_.right - pad;

    D2D1_COLOR_F catColor = CategoryColor(lib->category);

    // ── Large icon block ────────────────────────────────────────────────────
    const float iconSz  = 68.0f * scale;
    const float iconTop = bounds_.top + pad;
    D2D1_RECT_F iconRect = D2D1::RectF(x, iconTop, x + iconSz, iconTop + iconSz);

    MarketplaceEnsureAvatarAsync(lib->avatarUrl, hwnd_);
    ID2D1Bitmap* avatar = MarketplaceLoadAvatarBitmap(ctx, lib->avatarUrl);
    if (avatar)
    {
        ctx->DrawBitmap(avatar, iconRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        ID2D1SolidColorBrush *border = nullptr;
        ctx->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.10f), &border);
        if (border) {
            ctx->DrawRoundedRectangle(D2D1::RoundedRect(iconRect, 14.0f, 14.0f), border, 1.0f);
            border->Release();
        }
    }
    else
    {
        ID2D1SolidColorBrush *ibr = nullptr;
        ctx->CreateSolidColorBrush(catColor, &ibr);
        if (ibr) {
            ctx->FillRoundedRectangle(D2D1::RoundedRect(iconRect, 14.0f, 14.0f), ibr);
            ibr->Release();
        }
        IDWriteTextFormat *fmt = nullptr;
        dwrite->CreateTextFormat(L"Segoe UI Variable Display", nullptr,
            DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 22.0f * scale, L"en-us", &fmt);
        if (!fmt)
            dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                22.0f * scale, L"en-us", &fmt);
        ID2D1SolidColorBrush *wbr = nullptr;
        ctx->CreateSolidColorBrush(D2D1::ColorF(1,1,1,0.95f), &wbr);
        if (fmt && wbr) {
            fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            std::wstring ini = Initials(lib->name);
            ctx->DrawTextW(ini.c_str(), (UINT32)ini.size(), fmt, iconRect, wbr);
        }
        if (fmt) fmt->Release();
        if (wbr) wbr->Release();
    }

    // ── Library name ────────────────────────────────────────────────────────
    const float nameL   = x + iconSz + 18.0f * scale;
    const float nameTop = iconTop + 4.0f * scale;
    {
        IDWriteTextFormat *fmt = nullptr;
        dwrite->CreateTextFormat(L"Segoe UI Variable Display", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 22.0f * scale, L"en-us", &fmt);
        if (!fmt)
            dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                22.0f * scale, L"en-us", &fmt);
        ID2D1SolidColorBrush *br = nullptr;
        ctx->CreateSolidColorBrush(UI::Theme::PrimaryText(), &br);
        if (fmt && br) {
            fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            float nameR = actionButtonRect_.left - 12.0f * scale;
            ctx->DrawTextW(lib->name.c_str(), (UINT32)lib->name.size(), fmt,
                           D2D1::RectF(nameL, nameTop, nameR, nameTop + 30.0f * scale), br);
        }
        if (fmt) fmt->Release();
        if (br)  br->Release();
    }

    // ── Author + version ────────────────────────────────────────────────────
    {
        IDWriteTextFormat *fmt = nullptr;
        dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 12.0f * scale, L"en-us", &fmt);
        ID2D1SolidColorBrush *br = nullptr;
        ctx->CreateSolidColorBrush(UI::Theme::MutedText(), &br);
        if (fmt && br) {
            fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            std::wstring meta = L"by " + lib->author;
            if (!lib->version.empty())
                meta += L"   •   v" + lib->version;
            else if (!lib->language.empty())
                meta += L"   •   " + lib->language;
            ctx->DrawTextW(meta.c_str(), (UINT32)meta.size(), fmt,
                D2D1::RectF(nameL, nameTop + 33.0f * scale, xEnd, nameTop + 48.0f * scale), br);
        }
        if (fmt) fmt->Release();
        if (br)  br->Release();
    }

    // ── Category pill badge ──────────────────────────────────────────────────
    {
        IDWriteTextFormat *fmt = nullptr;
        dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            10.0f * scale, L"en-us", &fmt);
        float badgeW = (float)lib->category.size() * 6.5f * scale + 16.0f * scale;
        float badgeY = nameTop + 52.0f * scale;
        D2D1_RECT_F badge = D2D1::RectF(nameL, badgeY, nameL + badgeW, badgeY + 18.0f * scale);
        ID2D1SolidColorBrush *bgBr = nullptr;
        ctx->CreateSolidColorBrush(D2D1::ColorF(catColor.r, catColor.g, catColor.b, 0.18f), &bgBr);
        if (bgBr) { ctx->FillRoundedRectangle(D2D1::RoundedRect(badge, 3.0f, 3.0f), bgBr); bgBr->Release(); }
        ID2D1SolidColorBrush *txtBr = nullptr;
        ctx->CreateSolidColorBrush(D2D1::ColorF(catColor.r, catColor.g, catColor.b, 0.9f), &txtBr);
        if (fmt && txtBr) {
            fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            ctx->DrawTextW(lib->category.c_str(), (UINT32)lib->category.size(), fmt, badge, txtBr);
        }
        if (fmt)   fmt->Release();
        if (txtBr) txtBr->Release();
    }

    // ── Action button (Install / Uninstall) ──────────────────────────────────
    {
        bool installed = lib->isInstalled;
        D2D1_COLOR_F btnColor;
        if (installed) {
            btnColor = actionButtonHovered_
                ? D2D1::ColorF(0.75f, 0.20f, 0.20f)
                : D2D1::ColorF(0.20f, 0.60f, 0.35f);
        } else {
            btnColor = actionButtonHovered_
                ? UI::Theme::AccentStrong()
                : UI::Theme::Accent();
        }
        ID2D1SolidColorBrush *btnBr = nullptr;
        ctx->CreateSolidColorBrush(btnColor, &btnBr);
        if (btnBr) {
            ctx->FillRoundedRectangle(
                D2D1::RoundedRect(actionButtonRect_, 6.0f, 6.0f), btnBr);
            btnBr->Release();
        }
        IDWriteTextFormat *fmt = nullptr;
        dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 12.5f * scale, L"en-us", &fmt);
        ID2D1SolidColorBrush *txtBr = nullptr;
        ctx->CreateSolidColorBrush(D2D1::ColorF(1,1,1), &txtBr);
        if (fmt && txtBr) {
            fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            const wchar_t *label = installed
                ? (actionButtonHovered_ ? L"Remove" : L"Installed \u2713")
                : L"Install";
            ctx->DrawTextW(label, (UINT32)wcslen(label), fmt, actionButtonRect_, txtBr);
        }
        if (fmt)   fmt->Release();
        if (txtBr) txtBr->Release();
    }

    // ── Separator below header ───────────────────────────────────────────────
    float sepY = bounds_.top + pad + iconSz + 14.0f * scale;
    {
        ID2D1SolidColorBrush *sepBr = nullptr;
        ctx->CreateSolidColorBrush(UI::Theme::ChromeBorder(), &sepBr);
        if (sepBr) {
            ctx->DrawLine(D2D1::Point2F(x, sepY), D2D1::Point2F(xEnd, sepY), sepBr, 0.5f);
            sepBr->Release();
        }
    }

    // ── README rendered with the editor markdown preview component ──────────
    float readmeTop = sepY + 14.0f * scale;

    // Determine text to show
    std::wstring readmeText;
    bool fetching = false;
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        auto it = readmeCache_.find(currentLibraryName_);
        if (it != readmeCache_.end())
            readmeText = it->second;
        else
            fetching = (lastFetchedFor_ == currentLibraryName_);
    }

    // Trigger fetch if needed.
    if (readmeText.empty() && !fetching)
        FetchReadmeAsync(hwnd, currentLibraryName_, lib->gitUrl);

    const std::wstring displayText = readmeText.empty()
        ? (fetching ? L"Loading README..." : lib->description)
        : readmeText;

    // README label + controls
    {
        auto drawPill = [&](const D2D1_RECT_F& rect,
                            const wchar_t* label,
                            bool active,
                            bool hovered,
                            bool enabled,
                            float alphaScale = 1.0f) {
            ID2D1SolidColorBrush* bgBr = nullptr;
            ID2D1SolidColorBrush* borderBr = nullptr;
            ID2D1SolidColorBrush* txtBr = nullptr;
            IDWriteTextFormat* fmt = nullptr;

            const D2D1_COLOR_F accent = UI::Theme::Accent();
            const D2D1_COLOR_F bg = active
                ? D2D1::ColorF(accent.r, accent.g, accent.b, enabled ? 0.20f * alphaScale : 0.10f * alphaScale)
                : D2D1::ColorF(1.0f, 1.0f, 1.0f, hovered && enabled ? 0.08f * alphaScale : 0.04f * alphaScale);
            const D2D1_COLOR_F border = active
                ? D2D1::ColorF(accent.r, accent.g, accent.b, enabled ? 0.55f * alphaScale : 0.28f * alphaScale)
                : D2D1::ColorF(1.0f, 1.0f, 1.0f, hovered && enabled ? 0.18f * alphaScale : 0.10f * alphaScale);
            const D2D1_COLOR_F text = active
                ? D2D1::ColorF(accent.r, accent.g, accent.b, enabled ? 1.0f * alphaScale : 0.55f * alphaScale)
                : D2D1::ColorF(1.0f, 1.0f, 1.0f, enabled ? 0.88f * alphaScale : 0.45f * alphaScale);

            ctx->CreateSolidColorBrush(bg, &bgBr);
            ctx->CreateSolidColorBrush(border, &borderBr);
            ctx->CreateSolidColorBrush(text, &txtBr);
            dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
                DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 11.0f * scale, L"en-us", &fmt);

            if (bgBr)
                ctx->FillRoundedRectangle(D2D1::RoundedRect(rect, 7.0f * scale, 7.0f * scale), bgBr);
            if (borderBr)
                ctx->DrawRoundedRectangle(D2D1::RoundedRect(rect, 7.0f * scale, 7.0f * scale), borderBr, 1.0f);
            if (fmt && txtBr) {
                fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                ctx->DrawTextW(label, (UINT32)wcslen(label), fmt, rect, txtBr);
            }

            if (bgBr) bgBr->Release();
            if (borderBr) borderBr->Release();
            if (txtBr) txtBr->Release();
            if (fmt) fmt->Release();
        };

        IDWriteTextFormat *lFmt = nullptr;
        dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 10.0f * scale, L"en-us", &lFmt);
        ID2D1SolidColorBrush *mBr = nullptr;
        D2D1_COLOR_F mc = UI::Theme::MutedText(); mc.a *= 0.7f;
        ctx->CreateSolidColorBrush(mc, &mBr);
        if (lFmt && mBr) {
            lFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            lFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            const wchar_t *lbl = L"README";
            ctx->DrawTextW(lbl, (UINT32)wcslen(lbl), lFmt,
                D2D1::RectF(x, readmeTop + 6.0f * scale, x + 56.0f * scale, readmeTop + 20.0f * scale), mBr);
        }
        if (lFmt) lFmt->Release();
        if (mBr)  mBr->Release();

        drawPill(readmePreviewTabRect_, L"Preview",
                 readmeMode_ == ReadmeMode::Preview,
                 readmePreviewTabHovered_, true);
        drawPill(readmeMarkdownTabRect_, L"Markdown",
                 readmeMode_ == ReadmeMode::Markdown,
                 readmeMarkdownTabHovered_, true);
    }

    if (displayText != currentPreviewText_) {
        currentPreviewText_ = displayText;
        readmePreviewEditor_.CreateEmpty();
        readmePreviewEditor_.SetEmbeddedPreviewMode(true);
        readmePreviewEditor_.SetTextContent(currentPreviewText_, false);
        ApplyReadmeEditorMode();
    } else {
        ApplyReadmeEditorMode();
    }

    readmePreviewEditor_.Draw(ctx, dwrite);

    float readmeAreaBottom = readmeClipRect_.bottom;

    // ── Stats + Repository (fixed at bottom) ─────────────────────────────────
    float y = readmeAreaBottom + 10.0f * scale;

    // Separator
    {
        ID2D1SolidColorBrush *sepBr = nullptr;
        ctx->CreateSolidColorBrush(UI::Theme::ChromeBorder(), &sepBr);
        if (sepBr) {
            ctx->DrawLine(D2D1::Point2F(x, y), D2D1::Point2F(xEnd, y), sepBr, 0.5f);
            sepBr->Release();
        }
    }
    y += 12.0f * scale;

    // Stats row
    {
        struct StatItem { const wchar_t* label; std::wstring value; };
        StatItem stats[] = {
            { L"Stars",    FormatNumber(lib->stars) },
            { L"Language", lib->language.empty() ? L"Unknown" : lib->language },
            { L"License",  lib->license.empty() ? L"Unknown" : lib->license },
        };

        IDWriteTextFormat *valFmt = nullptr, *lblFmt = nullptr;
        dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 13.0f * scale, L"en-us", &valFmt);
        dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 10.0f * scale, L"en-us", &lblFmt);
        ID2D1SolidColorBrush *pBr = nullptr, *mBr = nullptr;
        ctx->CreateSolidColorBrush(UI::Theme::PrimaryText(), &pBr);
        D2D1_COLOR_F muted = UI::Theme::MutedText(); muted.a *= 0.75f;
        ctx->CreateSolidColorBrush(muted, &mBr);

        float statX = x;
        const float colW = (xEnd - x) / 3.0f;
        for (auto& s : stats) {
            if (valFmt && pBr) {
                valFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                valFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                ctx->DrawTextW(s.value.c_str(), (UINT32)s.value.size(), valFmt,
                    D2D1::RectF(statX, y, statX + colW - 4.0f, y + 18.0f * scale), pBr);
            }
            if (lblFmt && mBr) {
                lblFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                lblFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                ctx->DrawTextW(s.label, (UINT32)wcslen(s.label), lblFmt,
                    D2D1::RectF(statX, y + 19.0f * scale, statX + colW - 4.0f, y + 30.0f * scale), mBr);
            }
            statX += colW;
        }
        if (valFmt) valFmt->Release();
        if (lblFmt) lblFmt->Release();
        if (pBr)    pBr->Release();
        if (mBr)    mBr->Release();
        y += 38.0f * scale;
    }

    // Repository
    {
        ID2D1SolidColorBrush *sepBr = nullptr;
        ctx->CreateSolidColorBrush(UI::Theme::ChromeBorder(), &sepBr);
        if (sepBr) {
            ctx->DrawLine(D2D1::Point2F(x, y), D2D1::Point2F(xEnd, y), sepBr, 0.5f);
            sepBr->Release();
        }
        y += 10.0f * scale;
    }
    {
        IDWriteTextFormat *lFmt = nullptr;
        dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 10.0f * scale, L"en-us", &lFmt);
        ID2D1SolidColorBrush *mBr = nullptr;
        D2D1_COLOR_F mc = UI::Theme::MutedText(); mc.a *= 0.7f;
        ctx->CreateSolidColorBrush(mc, &mBr);
        if (lFmt && mBr) {
            lFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            lFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            ctx->DrawTextW(L"REPOSITORY", 10, lFmt,
                D2D1::RectF(x, y, xEnd, y + 14.0f * scale), mBr);
        }
        if (lFmt) lFmt->Release();
        if (mBr)  mBr->Release();
        y += 16.0f * scale;
    }
    {
        IDWriteTextFormat *fmt = nullptr;
        dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 11.5f * scale, L"en-us", &fmt);
        ID2D1SolidColorBrush *br = nullptr;
        D2D1_COLOR_F ac = UI::Theme::Accent();
        ac.a = repositoryLinkHovered_ ? 1.0f : 0.85f;
        ctx->CreateSolidColorBrush(ac, &br);
        if (fmt && br) {
            fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            D2D1_RECT_F linkRowRect = D2D1::RectF(x, y, xEnd, y + 18.0f * scale);
            ctx->DrawTextW(lib->gitUrl.c_str(), (UINT32)lib->gitUrl.size(), fmt,
                linkRowRect, br,
                D2D1_DRAW_TEXT_OPTIONS_CLIP);

            IDWriteTextLayout *layout = nullptr;
            if (SUCCEEDED(dwrite->CreateTextLayout(
                    lib->gitUrl.c_str(),
                    (UINT32)lib->gitUrl.size(),
                    fmt,
                    (std::max)(1.0f, xEnd - x),
                    24.0f * scale,
                    &layout)))
            {
                DWRITE_TEXT_METRICS metrics{};
                layout->GetMetrics(&metrics);
                float linkW = (std::min)(xEnd - x, metrics.widthIncludingTrailingWhitespace);
                repositoryLinkRect_ = D2D1::RectF(x, y, x + linkW, y + (std::max)(18.0f * scale, metrics.height));
                layout->Release();
            }
            else
            {
                repositoryLinkRect_ = linkRowRect;
            }

            if (repositoryLinkHovered_)
            {
                float underlineY = repositoryLinkRect_.bottom - 1.0f;
                ctx->DrawLine(
                    D2D1::Point2F(repositoryLinkRect_.left, underlineY),
                    D2D1::Point2F(repositoryLinkRect_.right, underlineY),
                    br,
                    1.0f);
            }
        }
        if (fmt) fmt->Release();
        if (br)  br->Release();
    }
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------

void MarketplaceExtensionTabView::OnMouseMove(HWND hwnd, POINT clientPoint)
{
    const bool wasHover = actionButtonHovered_;
    const bool wasPreviewHover = readmePreviewTabHovered_;
    const bool wasMarkdownHover = readmeMarkdownTabHovered_;
    const bool wasRepoHover = repositoryLinkHovered_;
    actionButtonHovered_ = IsPointInRect(clientPoint, actionButtonRect_);
    readmePreviewTabHovered_ = IsPointInRect(clientPoint, readmePreviewTabRect_);
    readmeMarkdownTabHovered_ = IsPointInRect(clientPoint, readmeMarkdownTabRect_);
    repositoryLinkHovered_ = IsPointInRect(clientPoint, repositoryLinkRect_);

    if (readmePreviewEditor_.IsPointInEditorBounds(clientPoint) || readmePreviewEditor_.IsDragSelecting())
        readmePreviewEditor_.OnMouseMove(hwnd, clientPoint);

    if (IsPointInView(clientPoint))
        SetCursor(LoadCursorW(nullptr, CursorForPoint(clientPoint)));

    if (wasHover != actionButtonHovered_ ||
        wasPreviewHover != readmePreviewTabHovered_ ||
        wasMarkdownHover != readmeMarkdownTabHovered_ ||
        wasRepoHover != repositoryLinkHovered_)
        InvalidateRect(hwnd, nullptr, FALSE);
}

void MarketplaceExtensionTabView::OnMouseWheel(HWND hwnd, int delta)
{
    POINT cursor{};
    GetCursorPos(&cursor);
    ScreenToClient(hwnd, &cursor);

    if (readmePreviewEditor_.IsPointInEditorBounds(cursor)) {
        readmePreviewEditor_.OnMouseWheel(hwnd, delta, false);
    }

    InvalidateRect(hwnd, nullptr, FALSE);
}

void MarketplaceExtensionTabView::OnLeftButtonDown(HWND hwnd, POINT clientPoint)
{
    if (IsPointInRect(clientPoint, readmePreviewTabRect_))
    {
        readmeMode_ = ReadmeMode::Preview;
        ApplyReadmeEditorMode();
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    if (IsPointInRect(clientPoint, readmeMarkdownTabRect_))
    {
        readmeMode_ = ReadmeMode::Markdown;
        ApplyReadmeEditorMode();
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    if (IsPointInRect(clientPoint, repositoryLinkRect_))
    {
        if (!currentLibraryName_.empty())
        {
            LibraryInfo lib;
            if (LibraryDatabase::Instance().GetLibraryCopy(currentLibraryName_, lib) && !lib.gitUrl.empty())
            {
                HINSTANCE r = ShellExecuteW(hwnd, L"open", lib.gitUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                if ((INT_PTR)r <= 32)
                {
                    Logger::Instance().Log(L"Failed to open repository URL: " + lib.gitUrl);
                }
            }
        }
        return;
    }

    if (readmePreviewEditor_.IsPointInEditorBounds(clientPoint)) {
        readmePreviewEditor_.OnLeftButtonDown(hwnd, clientPoint);
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    if (!IsPointInRect(clientPoint, actionButtonRect_)) return;
    if (currentLibraryName_.empty()) return;

    LibraryInfo lib;
    if (!LibraryDatabase::Instance().GetLibraryCopy(currentLibraryName_, lib)) return;

    if (!lib.isInstalled)
    {
        std::wstring installError;
        bool ok = GetExplorerManager().InstallLibraryFromGitUrl(
            hwnd, lib.gitUrl, lib.name, &installError);
        if (ok) {
            LibraryDatabase::Instance().SetInstalled(currentLibraryName_, true);
            Logger::Instance().Log(L"Installed: " + currentLibraryName_);
        } else {
            if (installError.empty()) installError = L"Failed to install library.";
            MessageBoxW(hwnd, installError.c_str(), L"Library Install",
                MB_OK | MB_ICONERROR);
            Logger::Instance().Log(L"Install failed: " + currentLibraryName_
                + L" - " + installError);
        }
    }
    else
    {
        std::wstring uninstallError;
        bool ok = GetExplorerManager().UninstallLibraryFromGitUrl(
            hwnd, lib.gitUrl, lib.name, &uninstallError);
        if (ok) {
            LibraryDatabase::Instance().RefreshInstallationStatus(GetExplorerManager().GetState().rootPath);
            Logger::Instance().Log(L"Uninstalled: " + currentLibraryName_);
        } else {
            if (uninstallError.empty()) uninstallError = L"Failed to remove library.";
            MessageBoxW(hwnd, uninstallError.c_str(), L"Library Remove",
                MB_OK | MB_ICONERROR);
            Logger::Instance().Log(L"Uninstall failed: " + currentLibraryName_
                + L" - " + uninstallError);
        }
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

void MarketplaceExtensionTabView::OnLeftButtonUp(HWND /*hwnd*/)
{
    // Keep editor interaction state consistent (selection drag, etc.).
    POINT cursor{};
    GetCursorPos(&cursor);
    if (hwnd_) {
        ScreenToClient(hwnd_, &cursor);
        if (readmePreviewEditor_.IsDragSelecting() ||
            readmePreviewEditor_.IsPointInEditorBounds(cursor))
        {
            readmePreviewEditor_.OnLeftButtonUp(hwnd_, cursor);
        }
    }
}

bool MarketplaceExtensionTabView::IsPointInView(POINT clientPoint) const
{
    return IsPointInRect(clientPoint, bounds_);
}

bool MarketplaceExtensionTabView::WantsHandCursor(POINT clientPoint) const
{
    return CursorForPoint(clientPoint) == IDC_HAND;
}

LPCWSTR MarketplaceExtensionTabView::CursorForPoint(POINT clientPoint) const
{
    if (!IsPointInView(clientPoint))
        return IDC_ARROW;

    if (IsPointInRect(clientPoint, actionButtonRect_) ||
        IsPointInRect(clientPoint, repositoryLinkRect_) ||
        IsPointInRect(clientPoint, readmePreviewTabRect_) ||
        IsPointInRect(clientPoint, readmeMarkdownTabRect_))
    {
        return IDC_HAND;
    }

    if (readmePreviewEditor_.IsPointInEditorBounds(clientPoint))
    {
        if (readmePreviewEditor_.IsPointOnPreviewMarkdownCopyButton(clientPoint))
            return IDC_HAND;
        if (readmeMode_ == ReadmeMode::Markdown)
            return IDC_IBEAM;
    }

    return IDC_ARROW;
}

bool MarketplaceExtensionTabView::UpdateUiAnimation()
{
    return readmePreviewEditor_.UpdatePreviewUiAnimation();
}

bool MarketplaceExtensionTabView::OnKeyDown(WPARAM key)
{
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    if (!ctrl)
        return false;

    if (key == 'C' || key == 'c')
    {
        readmePreviewEditor_.CopySelectionToClipboard();
        return true;
    }

    if (key == 'A' || key == 'a')
    {
        readmePreviewEditor_.SelectAll();
        return true;
    }

    return false;
}
