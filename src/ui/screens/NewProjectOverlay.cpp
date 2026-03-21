#include "ui/screens/NewProjectOverlay.h"
#include "core/window/Window.h"
#include "helpers/window_helpers.h"
#include "core/explorer/Explorer.h"
#include "ui/theme/Theme.h"
#include <filesystem>
#include <shlobj.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <cwctype>

namespace
{
static float AlignToPixel(float value, float scale)
{
    return (std::floor(value * scale) + 0.5f) / scale;
}

static D2D1_RECT_F PixelSnapRect(const D2D1_RECT_F &rect, float scale)
{
    return D2D1::RectF(
        (std::floor(rect.left * scale) + 0.5f) / scale,
        (std::floor(rect.top * scale) + 0.5f) / scale,
        (std::floor(rect.right * scale) - 0.5f) / scale,
        (std::floor(rect.bottom * scale) - 0.5f) / scale);
}

static std::wstring GetDefaultSourceReposPath()
{
    PWSTR profilePath = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &profilePath)) && profilePath)
    {
        std::filesystem::path base(profilePath);
        CoTaskMemFree(profilePath);
        out = (base / L"source" / L"repos").wstring();
    }
    return out;
}

static std::wstring ToLowerCopy(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t c)
                   { return (wchar_t)std::towlower(c); });
    return value;
}

static std::wstring TrimWhitespace(const std::wstring &value)
{
    size_t start = 0;
    size_t end = value.size();
    while (start < end && std::iswspace(value[start]))
        start++;
    while (end > start && std::iswspace(value[end - 1]))
        end--;
    return value.substr(start, end - start);
}
}

namespace UI
{
void NewProjectOverlay::Draw(Window &window, ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, const RECT &clientRect)
{
    if (!window.newProjectVisible_ || !ctx || !dwrite)
        return;

    const UI::Theme::Palette &palette = UI::Theme::GetPalette();

    D2D1_ANTIALIAS_MODE oldAA = ctx->GetAntialiasMode();
    D2D1_TEXT_ANTIALIAS_MODE oldTextAA = ctx->GetTextAntialiasMode();
    ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    ctx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

    UINT dpi = win32_get_dpi_for_window(window.hwnd_);
    float scale = (float)dpi / 96.0f;

    RECT tbRect = win32_titlebar_rect(window.hwnd_);
    D2D1_RECT_F full = D2D1::RectF((float)clientRect.left, (float)tbRect.bottom, (float)clientRect.right, (float)clientRect.bottom);

    ID2D1SolidColorBrush *bg = nullptr;
    ctx->CreateSolidColorBrush(UI::Theme::ChromeBackground(), &bg);
    if (bg)
    {
        ctx->FillRectangle(full, bg);
        bg->Release();
    }

    const bool showHome = (window.newProjPage_ == Window::NewProjectPage::Home);
    float padX = 24.0f * scale;
    float padY = 14.0f * scale;
    float gap = 24.0f * scale;
    float fullW = full.right - full.left;
    float leftW = (fullW - padX * 2.0f - gap) * 0.68f;
    float rightW = (fullW - padX * 2.0f - gap) - leftW;

    D2D1_RECT_F leftPanel = {};
    D2D1_RECT_F rightPanel = {};
    if (showHome)
    {
        leftPanel = D2D1::RectF(full.left + padX, full.top + padY, full.left + padX + leftW, full.bottom - padY);
        rightPanel = D2D1::RectF(leftPanel.right + gap, leftPanel.top, leftPanel.right + gap + rightW, leftPanel.bottom);
    }
    else
    {
        leftPanel = D2D1::RectF(0, 0, 0, 0);
        rightPanel = D2D1::RectF(full.left + padX, full.top + padY, full.right - padX, full.bottom - padY);
    }
    window.newProjCardRect_ = rightPanel;

    const wchar_t *uiFont = L"Segoe UI Variable Text";
    const wchar_t *iconFont = L"Segoe Fluent Icons";
    IDWriteFontCollection *uiCollection = nullptr;

    NewProjectRenderContext rc = {};
    rc.scale = scale;
    rc.full = full;
    rc.leftPanel = leftPanel;
    rc.rightPanel = rightPanel;
    rc.uiFont = uiFont;
    rc.iconFont = iconFont;
    rc.uiCollection = uiCollection;

    ctx->CreateSolidColorBrush(palette.inputBackground, &rc.panelBg);
    ctx->CreateSolidColorBrush(UI::Theme::ChromeBorder(), &rc.panelBorder);
    ctx->CreateSolidColorBrush(UI::Theme::ChromeBorder(), &rc.divider);
    ctx->CreateSolidColorBrush(UI::Theme::MutedText(), &rc.muted);
    ctx->CreateSolidColorBrush(UI::Theme::PrimaryText(), &rc.text);
    ctx->CreateSolidColorBrush(palette.explorerToolbarHover, &rc.subtle);

    if (rc.panelBg)
        ctx->FillRoundedRectangle(D2D1::RoundedRect(rightPanel, 8.0f * scale, 8.0f * scale), rc.panelBg);
    if (rc.panelBorder)
        ctx->DrawRoundedRectangle(D2D1::RoundedRect(PixelSnapRect(rightPanel, scale), 8.0f * scale, 8.0f * scale), rc.panelBorder, 1.0f);
    if (showHome && rc.divider)
    {
        float dividerX = AlignToPixel(rightPanel.left - gap * 0.5f, scale);
        float dividerTop = AlignToPixel(rightPanel.top, scale);
        float dividerBottom = AlignToPixel(rightPanel.bottom, scale);
        ctx->DrawLine(D2D1::Point2F(dividerX, dividerTop),
                      D2D1::Point2F(dividerX, dividerBottom), rc.divider, 1.0f);
    }

    dwrite->CreateTextFormat(uiFont, uiCollection, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             18.0f * scale, L"en-us", &rc.titleFmt);
    dwrite->CreateTextFormat(uiFont, uiCollection, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             11.0f * scale, L"en-us", &rc.sectionFmt);
    dwrite->CreateTextFormat(uiFont, uiCollection, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             10.0f * scale, L"en-us", &rc.labelFmt);
    dwrite->CreateTextFormat(uiFont, uiCollection, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             12.0f * scale, L"en-us", &rc.itemFmt);
    dwrite->CreateTextFormat(uiFont, uiCollection, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             10.0f * scale, L"en-us", &rc.subFmt);
    dwrite->CreateTextFormat(iconFont, uiCollection, DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             14.0f * scale, L"en-us", &rc.iconFmt);
    dwrite->CreateTextFormat(uiFont, uiCollection, DWRITE_FONT_WEIGHT_MEDIUM,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             11.0f * scale, L"en-us", &rc.actionFmt);
    if (rc.iconFmt)
        rc.iconFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    if (showHome)
    {
        // Left header
        D2D1_RECT_F leftTitle = D2D1::RectF(leftPanel.left, leftPanel.top, leftPanel.right, leftPanel.top + 26.0f * scale);
        if (rc.titleFmt && rc.text)
            ctx->DrawTextW(L"Commencez", 9, rc.titleFmt, leftTitle, rc.text);

        // Search bar
        float searchH = 30.0f * scale;
        float searchY = leftTitle.bottom + 10.0f * scale;
        window.newProjLocationInput_.SetRect(D2D1::RectF(leftPanel.left, searchY, leftPanel.right, searchY + searchH));
        auto &searchStyle = window.newProjLocationInput_.GetStyle();
        searchStyle.backgroundColor = D2D1::ColorF(0.15f, 0.15f, 0.15f, 1.0f);
        searchStyle.borderColor = D2D1::ColorF(0.24f, 0.24f, 0.24f, 1.0f);
        searchStyle.focusBorderColor = palette.inputFocusBorder;
        searchStyle.cornerRadius = 8.0f * scale;
        searchStyle.fontSize = 12.0f * scale;
        searchStyle.padding = 9.0f * scale;
        searchStyle.textColor = D2D1::ColorF(0.92f, 0.92f, 0.92f, 1.0f);
        searchStyle.placeholderColor = D2D1::ColorF(0.48f, 0.48f, 0.48f, 1.0f);
        searchStyle.iconColor = D2D1::ColorF(0.55f, 0.55f, 0.55f, 1.0f);
        searchStyle.iconSize = 14.0f * scale;
        searchStyle.iconPadding = 26.0f * scale;
        searchStyle.fontFamily = uiFont;
        searchStyle.fontCollection = uiCollection;
        window.newProjLocationInput_.Draw(ctx, dwrite);

    // Recents list
    float recentsLabelY = searchY + searchH + 14.0f * scale;
    if (rc.sectionFmt && rc.muted)
    {
        D2D1_RECT_F recentsLabel = D2D1::RectF(leftPanel.left, recentsLabelY, leftPanel.right, recentsLabelY + 16.0f * scale);
        ctx->DrawTextW(L"Recents", 7, rc.sectionFmt, recentsLabel, rc.muted);
    }
    D2D1_RECT_F listRect = D2D1::RectF(leftPanel.left, recentsLabelY + 18.0f * scale, leftPanel.right, leftPanel.bottom - 10.0f * scale);
    if (rc.panelBorder)
        ctx->DrawRoundedRectangle(D2D1::RoundedRect(PixelSnapRect(listRect, scale), 8.0f * scale, 8.0f * scale), rc.panelBorder, 1.0f);

    window.recentProjectRects_.clear();
    window.recentProjectIndexMap_.clear();
    float rowH = 44.0f * scale;
    float rowGap = 6.0f * scale;
    float rowY = listRect.top + 10.0f * scale;
    std::wstring filter = TrimWhitespace(window.newProjLocationInput_.GetText());
    filter = ToLowerCopy(filter);
    bool filterActive = !filter.empty();

    for (size_t i = 0; i < window.recentProjects_.size(); ++i)
    {
        const std::wstring &path = window.recentProjects_[i].path;
        std::filesystem::path p(path);
        std::wstring name = p.filename().wstring();
        if (name.empty())
            name = path;

        if (filterActive)
        {
            std::wstring nameLower = ToLowerCopy(name);
            if (nameLower.find(filter) == std::wstring::npos)
                continue;
        }

        D2D1_RECT_F rowRect = D2D1::RectF(listRect.left + 10.0f * scale, rowY,
                                          listRect.right - 10.0f * scale, rowY + rowH);
        window.recentProjectRects_.push_back(rowRect);
        window.recentProjectIndexMap_.push_back((int)i);

        bool hovered = ((int)window.recentProjectRects_.size() - 1 == window.recentProjectHover_);
        if (hovered && rc.subtle)
            ctx->FillRoundedRectangle(D2D1::RoundedRect(rowRect, 6.0f * scale, 6.0f * scale), rc.subtle);
        if (rc.panelBorder)
            ctx->DrawRoundedRectangle(D2D1::RoundedRect(PixelSnapRect(rowRect, scale), 6.0f * scale, 6.0f * scale), rc.panelBorder, 1.0f);

        if (rc.iconFmt && rc.muted)
        {
            D2D1_RECT_F iconRect = D2D1::RectF(rowRect.left + 8.0f * scale, rowRect.top, rowRect.left + 30.0f * scale, rowRect.bottom);
            ctx->DrawTextW(L"\uE8B7", 1, rc.iconFmt, iconRect, rc.muted);
        }

        if (rc.itemFmt && rc.text)
        {
            D2D1_RECT_F nameRect = D2D1::RectF(rowRect.left + 34.0f * scale, rowRect.top + 6.0f * scale,
                                               rowRect.right - 10.0f * scale, rowRect.top + 24.0f * scale);
            ctx->DrawTextW(name.c_str(), (UINT32)name.size(), rc.itemFmt, nameRect, rc.text);
        }
        if (rc.subFmt && rc.muted)
        {
            D2D1_RECT_F pathRect = D2D1::RectF(rowRect.left + 34.0f * scale, rowRect.top + 22.0f * scale,
                                               rowRect.right - 10.0f * scale, rowRect.bottom - 6.0f * scale);
            ctx->DrawTextW(path.c_str(), (UINT32)path.size(), rc.subFmt, pathRect, rc.muted);
        }

        rowY += rowH + rowGap;
        if (rowY + rowH > listRect.bottom)
            break;
    }

    if (window.recentProjectRects_.empty() && rc.subFmt && rc.muted)
    {
        D2D1_RECT_F emptyRect = D2D1::RectF(listRect.left + 10.0f * scale, listRect.top + 10.0f * scale,
                                            listRect.right - 10.0f * scale, listRect.bottom - 10.0f * scale);
        rc.subFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        rc.subFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (filterActive)
            ctx->DrawTextW(L"Aucun resultat", 13, rc.subFmt, emptyRect, rc.muted);
        else
            ctx->DrawTextW(L"Aucun projet recent", 19, rc.subFmt, emptyRect, rc.muted);
        rc.subFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        rc.subFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    }
    else
    {
        window.newProjLocationInput_.SetRect(D2D1::RectF(0, 0, 0, 0));
    }

    if (window.newProjPage_ == Window::NewProjectPage::Home)
        window.newProjHomeView_.Draw(window, ctx, dwrite, rc);
    else
        window.newProjCreateView_.Draw(window, ctx, dwrite, rc);

    if (rc.panelBg) rc.panelBg->Release();
    if (rc.panelBorder) rc.panelBorder->Release();
    if (rc.divider) rc.divider->Release();
    if (rc.muted) rc.muted->Release();
    if (rc.text) rc.text->Release();
    if (rc.subtle) rc.subtle->Release();
    if (rc.titleFmt) rc.titleFmt->Release();
    if (rc.sectionFmt) rc.sectionFmt->Release();
    if (rc.labelFmt) rc.labelFmt->Release();
    if (rc.itemFmt) rc.itemFmt->Release();
    if (rc.subFmt) rc.subFmt->Release();
    if (rc.iconFmt) rc.iconFmt->Release();
    if (rc.actionFmt) rc.actionFmt->Release();

    ctx->SetAntialiasMode(oldAA);
    ctx->SetTextAntialiasMode(oldTextAA);
}

bool NewProjectOverlay::HandleMouseDown(Window &window, HWND hwnd, POINT pt)
{
    if (!window.newProjectVisible_)
        return false;

    if (window.newProjPage_ == Window::NewProjectPage::Home && !window.recentProjectRects_.empty())
    {
        for (size_t i = 0; i < window.recentProjectRects_.size(); ++i)
        {
            const auto &r = window.recentProjectRects_[i];
            if (pt.x >= (int)r.left && pt.x <= (int)r.right &&
                pt.y >= (int)r.top && pt.y <= (int)r.bottom)
            {
                if (i < window.recentProjectIndexMap_.size())
                {
                    int idx = window.recentProjectIndexMap_[i];
                    if (idx >= 0 && idx < (int)window.recentProjects_.size())
                        window.OpenProjectAtPath(window.recentProjects_[idx].path);
                }
                return true;
            }
        }
    }

    if (window.newProjPage_ == Window::NewProjectPage::Create && window.newProjNameInput_.HitTest(pt))
    {
        window.newProjNameFocused_ = true;
        window.newProjLocationFocused_ = false;
        window.newProjNameInput_.SetFocused(true);
        window.newProjLocationInput_.SetFocused(false);
        window.newProjNameInput_.OnLeftButtonDown(hwnd, pt);
        return true;
    }
    if (window.newProjLocationInput_.HitTest(pt))
    {
        window.newProjNameFocused_ = false;
        window.newProjLocationFocused_ = true;
        window.newProjNameInput_.SetFocused(false);
        window.newProjLocationInput_.SetFocused(true);
        window.newProjLocationInput_.OnLeftButtonDown(hwnd, pt);
        return true;
    }
    if (window.newProjPage_ == Window::NewProjectPage::Create)
    {
        for (size_t i = 0; i < window.newProjTemplateRects_.size(); ++i)
        {
            const auto &r = window.newProjTemplateRects_[i];
            if (pt.x >= (int)r.left && pt.x <= (int)r.right &&
                pt.y >= (int)r.top && pt.y <= (int)r.bottom)
            {
                window.newProjTemplateIndex_ = (int)i;
                InvalidateRect(window.hwnd_, nullptr, FALSE);
                return true;
            }
        }
    }

    if (pt.x >= (int)window.newProjOpenRect_.left && pt.x <= (int)window.newProjOpenRect_.right &&
        pt.y >= (int)window.newProjOpenRect_.top && pt.y <= (int)window.newProjOpenRect_.bottom)
    {
        if (window.newProjPage_ == Window::NewProjectPage::Create)
        {
            window.newProjPage_ = Window::NewProjectPage::Home;
            window.newProjNameFocused_ = false;
            window.newProjLocationFocused_ = false;
            window.newProjNameInput_.SetFocused(false);
            window.newProjLocationInput_.SetFocused(false);
            InvalidateRect(window.hwnd_, nullptr, FALSE);
        }
        else
        {
            window.OpenProjectDialog();
            if (!GetExplorerManager().GetState().rootPath.empty())
            {
                window.HideNewProjectOverlay();
                InvalidateRect(window.hwnd_, nullptr, FALSE);
            }
        }
        return true;
    }
    if (pt.x >= (int)window.newProjBrowseRect_.left && pt.x <= (int)window.newProjBrowseRect_.right &&
        pt.y >= (int)window.newProjBrowseRect_.top && pt.y <= (int)window.newProjBrowseRect_.bottom)
    {
        if (window.newProjPage_ == Window::NewProjectPage::Home)
        {
            std::wstring root = GetDefaultSourceReposPath();
            if (!root.empty())
            {
                ShellExecuteW(nullptr, L"open", root.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
        return true;
    }

    if (pt.x >= (int)window.newProjCreateRect_.left && pt.x <= (int)window.newProjCreateRect_.right &&
        pt.y >= (int)window.newProjCreateRect_.top && pt.y <= (int)window.newProjCreateRect_.bottom)
    {
        if (window.newProjPage_ == Window::NewProjectPage::Home)
        {
            window.newProjPage_ = Window::NewProjectPage::Create;
            window.newProjNameFocused_ = true;
            window.newProjLocationFocused_ = false;
            window.newProjNameInput_.SetFocused(true);
            window.newProjLocationInput_.SetFocused(false);
            InvalidateRect(window.hwnd_, nullptr, FALSE);
            return true;
        }
        window.CreateProjectFromOverlay();
        return true;
    }

    if (pt.x >= (int)window.newProjCancelRect_.left && pt.x <= (int)window.newProjCancelRect_.right &&
        pt.y >= (int)window.newProjCancelRect_.top && pt.y <= (int)window.newProjCancelRect_.bottom)
    {
        if (window.newProjPage_ == Window::NewProjectPage::Create)
        {
            window.newProjPage_ = Window::NewProjectPage::Home;
            window.newProjNameFocused_ = false;
            window.newProjLocationFocused_ = false;
            window.newProjNameInput_.SetFocused(false);
            window.newProjLocationInput_.SetFocused(false);
            InvalidateRect(window.hwnd_, nullptr, FALSE);
        }
        else
        {
            window.HideNewProjectOverlay();
        }
        return true;
    }

    return true;
}

bool NewProjectOverlay::HandleMouseUp(Window &window, HWND hwnd, POINT pt)
{
    if (!window.newProjectVisible_)
        return false;
    bool used = false;
    if (window.newProjPage_ == Window::NewProjectPage::Create)
        used |= window.newProjNameInput_.OnLeftButtonUp(hwnd, pt);
    used |= window.newProjLocationInput_.OnLeftButtonUp(hwnd, pt);
    return true;
}

bool NewProjectOverlay::HandleMouseMove(Window &window, HWND hwnd, POINT pt)
{
    if (!window.newProjectVisible_)
        return false;

    window.newProjNameInput_.OnMouseMove(hwnd, pt);
    window.newProjLocationInput_.OnMouseMove(hwnd, pt);

    int templateHover = -1;
    if (window.newProjPage_ == Window::NewProjectPage::Create)
    {
        for (size_t i = 0; i < window.newProjTemplateRects_.size(); ++i)
        {
            const auto &r = window.newProjTemplateRects_[i];
            if (pt.x >= (int)r.left && pt.x <= (int)r.right &&
                pt.y >= (int)r.top && pt.y <= (int)r.bottom)
            {
                templateHover = (int)i;
                break;
            }
        }
    }

    bool hoverOpen = (pt.x >= (int)window.newProjOpenRect_.left && pt.x <= (int)window.newProjOpenRect_.right &&
                      pt.y >= (int)window.newProjOpenRect_.top && pt.y <= (int)window.newProjOpenRect_.bottom);
    bool hoverBrowse = false;
    if (window.newProjPage_ == Window::NewProjectPage::Home)
    {
        hoverBrowse = (pt.x >= (int)window.newProjBrowseRect_.left && pt.x <= (int)window.newProjBrowseRect_.right &&
                       pt.y >= (int)window.newProjBrowseRect_.top && pt.y <= (int)window.newProjBrowseRect_.bottom);
    }
    bool hoverCancel = (pt.x >= (int)window.newProjCancelRect_.left && pt.x <= (int)window.newProjCancelRect_.right &&
                        pt.y >= (int)window.newProjCancelRect_.top && pt.y <= (int)window.newProjCancelRect_.bottom);
    bool hoverCreate = (pt.x >= (int)window.newProjCreateRect_.left && pt.x <= (int)window.newProjCreateRect_.right &&
                        pt.y >= (int)window.newProjCreateRect_.top && pt.y <= (int)window.newProjCreateRect_.bottom);

    int recentHover = -1;
    if (window.newProjPage_ == Window::NewProjectPage::Home)
    {
        for (size_t i = 0; i < window.recentProjectRects_.size(); ++i)
        {
            const auto &r = window.recentProjectRects_[i];
            if (pt.x >= (int)r.left && pt.x <= (int)r.right &&
                pt.y >= (int)r.top && pt.y <= (int)r.bottom)
            {
                recentHover = (int)i;
                break;
            }
        }
    }

    bool changed = (templateHover != window.newProjTemplateHover_) || (hoverOpen != window.newProjOpenHover_) ||
                   (hoverBrowse != window.newProjBrowseHover_) ||
                   (hoverCancel != window.newProjCancelHover_) || (hoverCreate != window.newProjCreateHover_) ||
                   (recentHover != window.recentProjectHover_);
    window.newProjTemplateHover_ = templateHover;
    window.newProjOpenHover_ = hoverOpen;
    window.newProjBrowseHover_ = hoverBrowse;
    window.newProjCancelHover_ = hoverCancel;
    window.newProjCreateHover_ = hoverCreate;
    window.recentProjectHover_ = recentHover;

    if (changed)
        InvalidateRect(window.hwnd_, nullptr, FALSE);

    return true;
}

bool NewProjectOverlay::HandleChar(Window &window, wchar_t ch)
{
    if (!window.newProjectVisible_)
        return false;
    if (window.newProjPage_ == Window::NewProjectPage::Create && window.newProjNameFocused_)
        return window.newProjNameInput_.OnChar(ch);
    if (window.newProjLocationFocused_)
        return window.newProjLocationInput_.OnChar(ch);
    return false;
}

bool NewProjectOverlay::HandleKeyDown(Window &window, WPARAM key)
{
    if (!window.newProjectVisible_)
        return false;

    if (key == VK_ESCAPE)
    {
        window.HideNewProjectOverlay();
        return true;
    }

    if (key == VK_TAB)
    {
        if (window.newProjPage_ == Window::NewProjectPage::Create)
        {
            bool toLocation = window.newProjNameFocused_;
            window.newProjNameFocused_ = !toLocation;
            window.newProjLocationFocused_ = toLocation;
            window.newProjNameInput_.SetFocused(!toLocation);
            window.newProjLocationInput_.SetFocused(toLocation);
        }
        else
        {
            window.newProjNameFocused_ = false;
            window.newProjLocationFocused_ = true;
            window.newProjNameInput_.SetFocused(false);
            window.newProjLocationInput_.SetFocused(true);
        }
        return true;
    }

    if (window.newProjPage_ == Window::NewProjectPage::Create && window.newProjNameFocused_)
    {
        if (key == VK_RETURN)
            return window.CreateProjectFromOverlay();
        return window.newProjNameInput_.OnKeyDown(key);
    }
    if (window.newProjLocationFocused_)
        return window.newProjLocationInput_.OnKeyDown(key);
    return false;
}
} // namespace UI
