#include "Panel.h"
#include "helpers/window_helpers.h"
#include "utils/logger/Logger.h"
#include "ui/theme/Theme.h"
#include <cmath>

Panel::Panel(PanelId id)
    : id_(id),
      title_(L"Panel"),
      visible_(true),
      active_(false)
{
}

bool Panel::IsPointInPanel(POINT clientPoint) const
{
    if (!visible_ || state_.physicalWidth <= 0)
        return false;

    return (clientPoint.x >= state_.leftEdge && clientPoint.x <= state_.rightEdge &&
            clientPoint.y >= state_.topEdge && clientPoint.y <= state_.bottomEdge);
}

void Panel::UpdateBaseLayout(HWND hwnd, float leftEdge)
{
    RECT clientRect;
    GetClientRect(hwnd, &clientRect);

    UINT dpi = win32_get_dpi_for_window(hwnd);
    float scale = dpi / 96.0f;

    RECT tbRect = win32_titlebar_rect(hwnd);
    int footerHeight = win32_dpi_scale(28, dpi);

    state_.physicalWidth = static_cast<int>(state_.logicalWidth * scale);
    state_.leftEdge = leftEdge;
    state_.rightEdge = leftEdge + state_.physicalWidth;
    state_.topEdge = static_cast<float>(tbRect.bottom);
    state_.bottomEdge = static_cast<float>(clientRect.bottom - footerHeight);
}

void Panel::DrawBackground(ID2D1RenderTarget *ctx)
{
    ID2D1SolidColorBrush *bgBrush = nullptr;
    ctx->CreateSolidColorBrush(UI::Theme::ChromeBackground(), &bgBrush);

    if (bgBrush)
    {
        D2D1_RECT_F rect = D2D1::RectF(state_.leftEdge, state_.topEdge, state_.rightEdge, state_.bottomEdge);
        ctx->FillRectangle(rect, bgBrush);
        bgBrush->Release();
    }
}

void Panel::DrawTitle(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite)
{
    IDWriteTextFormat *titleFormat = nullptr;
    dwrite->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             13.0f, L"en-us", &titleFormat);

    if (titleFormat)
    {
        titleFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        titleFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    ID2D1SolidColorBrush *textBrush = nullptr;
    ctx->CreateSolidColorBrush(UI::Theme::PrimaryText(), &textBrush);

    D2D1_RECT_F titleRect = D2D1::RectF(
        state_.leftEdge + state_.leftPadding,
        state_.topEdge + state_.topPadding,
        state_.rightEdge - state_.leftPadding,
        state_.topEdge + state_.titleHeight);

    if (titleFormat && textBrush)
    {
        ctx->DrawTextW(title_.c_str(), static_cast<UINT32>(title_.length()), titleFormat, titleRect, textBrush);
    }

    if (titleFormat)
        titleFormat->Release();
    if (textBrush)
        textBrush->Release();
}

void Panel::DrawRightBorder(ID2D1RenderTarget *ctx)
{
    ID2D1SolidColorBrush *brush = nullptr;

    D2D1_COLOR_F borderColor =
        (state_.isHoveringResizeZone || state_.isResizing)
            ? UI::Theme::Accent()
            : UI::Theme::ChromeBorder();

    ctx->CreateSolidColorBrush(borderColor, &brush);

    if (brush)
    {
        float resizeEdgeX = IsResizeHandleOnLeft() ? state_.leftEdge : state_.rightEdge;
        float bx = std::round(resizeEdgeX) - 0.5f;
        D2D1_POINT_2F p1 = D2D1::Point2F(bx, state_.topEdge);
        D2D1_POINT_2F p2 = D2D1::Point2F(bx, state_.bottomEdge);

        D2D1_ANTIALIAS_MODE oldAA = ctx->GetAntialiasMode();
        ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);

        float thickness = (state_.isHoveringResizeZone || state_.isResizing) ? 2.0f : 1.0f;
        ctx->DrawLine(p1, p2, brush, thickness);

        ctx->SetAntialiasMode(oldAA);
        brush->Release();
    }
}

// ============================================================================
// Resize Logic
// ============================================================================

bool Panel::IsPointInResizeZone(POINT clientPoint) const
{
    if (!visible_ || state_.physicalWidth <= 0)
        return false;

    float resizeEdgeX = IsResizeHandleOnLeft() ? state_.leftEdge : state_.rightEdge;

    // Zone active sur toute la hauteur du panel (IMPORTANT)
    float top = state_.topEdge;
    float bottom = state_.bottomEdge;

    return (clientPoint.x >= (int)std::floor(resizeEdgeX - RESIZE_ZONE_WIDTH) &&
            clientPoint.x <= (int)std::ceil(resizeEdgeX + RESIZE_ZONE_WIDTH) &&
            clientPoint.y >= (int)top &&
            clientPoint.y <= (int)bottom);
}

bool Panel::HandleResizeMouseMove(HWND hwnd, POINT clientPoint)
{
    bool stateChanged = false;

    if (state_.isResizing)
    {
        // Resize actif: utiliser clientPoint.x (stable)
        UINT dpi = win32_get_dpi_for_window(hwnd);

        int deltaPhysical = clientPoint.x - state_.resizeStartClientX;
        int deltaLogical = MulDiv(deltaPhysical, 96, (int)dpi);

        int newWidth = IsResizeHandleOnLeft()
                           ? (state_.resizeStartWidth - deltaLogical)
                           : (state_.resizeStartWidth + deltaLogical);
        if (newWidth < state_.minWidth)
            newWidth = state_.minWidth;
        if (newWidth > state_.maxWidth)
            newWidth = state_.maxWidth;

        if (newWidth != state_.logicalWidth)
        {
            state_.logicalWidth = newWidth;
            UpdateLayout(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            stateChanged = true;
        }

        SetCursor(LoadCursor(NULL, IDC_SIZEWE));
        return stateChanged;
    }

    // Hover resize zone
    bool wasHovering = state_.isHoveringResizeZone;

    // (Optionnel mais stable): assure rightEdge correct
    // Si UpdateLayout est lourd chez toi, tu peux enlever cette ligne.
    // UpdateLayout(hwnd);

    state_.isHoveringResizeZone = IsPointInResizeZone(clientPoint);

    if (state_.isHoveringResizeZone)
    {
        SetCursor(LoadCursor(NULL, IDC_SIZEWE));
        if (!wasHovering)
        {
            InvalidateRect(hwnd, nullptr, FALSE);
            stateChanged = true;
        }
    }
    else if (wasHovering)
    {
        InvalidateRect(hwnd, nullptr, FALSE);
        stateChanged = true;
    }

    return stateChanged;
}

bool Panel::HandleResizeLeftButtonDown(HWND hwnd, POINT clientPoint)
{
    // IMPORTANT: rightEdge correct avant hit-test
    UpdateLayout(hwnd);

    if (IsPointInResizeZone(clientPoint))
    {
        state_.isResizing = true;

        state_.resizeStartClientX = clientPoint.x;   // <- stable
        state_.resizeStartWidth = state_.logicalWidth;

        SetCapture(hwnd);

        Logger::Instance().Log(std::wstring(L"Panel::HandleResizeLeftButtonDown - capture set, startWidth=") +
                               std::to_wstring(state_.resizeStartWidth));

        SetCursor(LoadCursor(NULL, IDC_SIZEWE));
        return true;
    }

    return false;
}

bool Panel::HandleResizeLeftButtonUp(HWND hwnd)
{
    if (state_.isResizing)
    {
        state_.isResizing = false;
        ReleaseCapture();

        Logger::Instance().Log(L"Panel::HandleResizeLeftButtonUp - capture released");

        // Ne force pas forcément la flèche ici: Window::WM_SETCURSOR peut gérer
        // mais OK de le mettre:
        SetCursor(LoadCursor(NULL, IDC_ARROW));

        InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }
    return false;
}

void Panel::ClearResizeHover(HWND hwnd)
{
    if (state_.isHoveringResizeZone)
    {
        state_.isHoveringResizeZone = false;
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}
