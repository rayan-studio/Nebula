#include "Footer.h"
#include "helpers/window_helpers.h"
#include "core/explorer/Explorer.h"
#include <dwrite.h>
#include <string>
#include <algorithm>
#include <vector>

// Globals for footer hover handling
static std::vector<D2D1_RECT_F> g_footer_segment_rects;
static int g_footer_hovered_index = -1;
static std::wstring g_footer_hint;
static ULONGLONG g_footer_hint_until = 0;

static std::wstring DetectLanguageFromPath(const std::wstring &path)
{
    if (path.empty())
        return L"Plain Text";
    size_t pos = path.find_last_of(L'.');
    if (pos == std::wstring::npos)
        return L"Plain Text";
    std::wstring ext = path.substr(pos + 1);
    for (auto &c : ext) c = towlower(c);

    if (ext == L"cpp" || ext == L"cc" || ext == L"cxx") return L"C++";
    if (ext == L"c") return L"C";
    if (ext == L"h" || ext == L"hpp") return L"C/C++ Header";
    if (ext == L"py") return L"Python";
    if (ext == L"js") return L"JavaScript";
    if (ext == L"ts") return L"TypeScript";
    if (ext == L"cs") return L"C#";
    if (ext == L"java") return L"Java";
    if (ext == L"md") return L"Markdown";
    if (ext == L"json") return L"JSON";
    if (ext == L"html" || ext == L"htm") return L"HTML";
    if (ext == L"css") return L"CSS";
    if (ext == L"rs") return L"Rust";

    // fallback: return extension uppercased
    for (auto &c : ext) c = towupper(c);
    return ext;
}

void DrawFooterD2D(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd, const std::wstring &filePath, int line, int column, const std::wstring &encoding)
{
    if (!ctx) return;

    if (!g_footer_hint.empty())
    {
        ULONGLONG now = GetTickCount64();
        if (g_footer_hint_until != 0 && now > g_footer_hint_until)
        {
            g_footer_hint.clear();
            g_footer_hint_until = 0;
        }
    }

    RECT client;
    GetClientRect(hwnd, &client);
    UINT dpi = win32_get_dpi_for_window(hwnd);

    int footerLogicalH = 28; // logical px
    int footerH = win32_dpi_scale(footerLogicalH, dpi);

    float left = 0.0f;
    float right = (float)client.right;
    float top = (float)(client.bottom - footerH);
    float bottom = (float)client.bottom;

    // background same as title bar
    ID2D1SolidColorBrush *bgBrush = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(18.0f / 255.0f, 18.0f / 255.0f, 18.0f / 255.0f), &bgBrush);
    D2D1_RECT_F rect = D2D1::RectF(left, top, right, bottom);
    ctx->FillRectangle(rect, bgBrush);

    // top border (same color as title bar border)
    ID2D1SolidColorBrush *borderBrush = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(48.0f / 255.0f, 48.0f / 255.0f, 48.0f / 255.0f), &borderBrush);
    D2D1_POINT_2F l = D2D1::Point2F(left, top + 0.5f);
    D2D1_POINT_2F r = D2D1::Point2F(right, top + 0.5f);
    ctx->DrawLine(l, r, borderBrush, 1.0f);

    // Notification icon (simple circular indicator) on the left
    ID2D1SolidColorBrush *notifBrush = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0x4ec9b0), &notifBrush);
    int notifLogicalR = 6; // logical radius
    int notifR = win32_dpi_scale(notifLogicalR, dpi);
    float iconCx = left + 12.0f + (float)notifR;
    float iconCy = top + ((bottom - top) * 0.5f);
    D2D1_ELLIPSE outer = D2D1::Ellipse(D2D1::Point2F(iconCx, iconCy), (FLOAT)notifR, (FLOAT)notifR);
    ctx->FillEllipse(outer, notifBrush);

    // Small inner dot to create a ring effect (use footer background color)
    ID2D1SolidColorBrush *innerBrush = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(18.0f / 255.0f, 18.0f / 255.0f, 18.0f / 255.0f), &innerBrush);
    D2D1_ELLIPSE inner = D2D1::Ellipse(D2D1::Point2F(iconCx, iconCy + 1.0f), (FLOAT)(notifR * 0.45f), (FLOAT)(notifR * 0.45f));
    ctx->FillEllipse(inner, innerBrush);

    // Prepare right-side status text (line/column + language + encoding)
    std::wstring lang = DetectLanguageFromPath(filePath);
    wchar_t buf[256];
    swprintf_s(buf, 256, L"Ln %d, Col %d - %s - %s",
               (line + 1), (column + 1), lang.c_str(), encoding.c_str());

    // Create right-aligned format for status (use JetBrains Mono to match editor)
    IDWriteTextFormat *statusFmt = nullptr;
    if (dwrite)
    {
        dwrite->CreateTextFormat(L"JetBrains Mono", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &statusFmt);
        if (statusFmt)
        {
            statusFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            statusFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
    }

    // Create left-aligned format for path (use JetBrains Mono)
    IDWriteTextFormat *pathFmt = nullptr;
    if (dwrite)
    {
        dwrite->CreateTextFormat(L"JetBrains Mono", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &pathFmt);
        if (pathFmt)
        {
            pathFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            pathFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }
    }

    ID2D1SolidColorBrush *textBrush = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0.85f, 0.85f, 0.85f), &textBrush);

    // Prepare hover brush early so it's visible in the whole function scope
    ID2D1SolidColorBrush *segmentHoverBrush = nullptr;
    ctx->CreateSolidColorBrush(D2D1::ColorF(0x2a2d2e), &segmentHoverBrush);

    if (!filePath.empty() && pathFmt && textBrush)
    {
        float pathLeft = left + 12.0f + (float)(notifR * 2) + 6.0f;
        float pathRight = right - 160.0f;
        D2D1_RECT_F pathRectF = D2D1::RectF(pathLeft, top, pathRight, bottom);

        // Split filePath by backslash
        std::vector<std::wstring> parts;
        {
            std::wstring cur;
            for (wchar_t c : filePath)
            {
                if (c == L'\\')
                {
                    if (!cur.empty()) parts.push_back(cur);
                    cur.clear();
                }
                else
                {
                    cur += c;
                }
            }
            if (!cur.empty()) parts.push_back(cur);
        }

        float x = pathRectF.left;
        float availableHeight = pathRectF.bottom - pathRectF.top;
        int arrowPx = win32_dpi_scale(12, dpi);
        float arrowF = (float)arrowPx;
        std::string chevronAsset = "assets\\ressource\\icons\\chevron-right.svg";

        // Prepare hover rect storage for mouse hit-testing. We'll store one rect per segment
        g_footer_segment_rects.clear();

        for (size_t i = 0; i < parts.size(); ++i)
        {
            const std::wstring &seg = parts[i];

            float availableWidth = pathRectF.right - x;
            if (availableWidth <= 8.0f) break;

            // Create layout constrained to remaining width
            IDWriteTextLayout *segLayout = nullptr;
            HRESULT hr = dwrite->CreateTextLayout(seg.c_str(), (UINT32)seg.size(), pathFmt, availableWidth, availableHeight, &segLayout);
            if (SUCCEEDED(hr) && segLayout)
            {
                // If it doesn't fit, set trimming and draw truncated then stop
                DWRITE_TEXT_METRICS metrics;
                segLayout->GetMetrics(&metrics);
                if (metrics.widthIncludingTrailingWhitespace > availableWidth - 4.0f)
                {
                    // Apply trimming and redraw with ellipsis
                    DWRITE_TRIMMING trimming = {};
                    trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
                    IDWriteInlineObject *ellipsisToken = nullptr;
                    if (SUCCEEDED(dwrite->CreateEllipsisTrimmingSign(pathFmt, &ellipsisToken)))
                    {
                        segLayout->SetTrimming(&trimming, ellipsisToken);
                    }
                    ctx->DrawTextLayout(D2D1::Point2F(x, pathRectF.top), segLayout, textBrush);
                    if (ellipsisToken) ellipsisToken->Release();
                    segLayout->Release();
                    break; // no space for further segments
                }

                // Draw full segment
                // compute seg rect for hover background using metrics and add generous padding
                float segW = metrics.widthIncludingTrailingWhitespace;
                float padH = 6.0f; // horizontal padding
                float padV = 4.0f; // vertical padding
                D2D1_RECT_F segRect = D2D1::RectF(x - padH, pathRectF.top + padV, x + segW + padH, pathRectF.bottom - padV);

                // push and get index (use same rect for hit-testing)
                g_footer_segment_rects.push_back(segRect);
                int segIndex = (int)g_footer_segment_rects.size() - 1;

                // If this segment is hovered, draw rounded background (use segRect directly)
                if (g_footer_hovered_index == segIndex && segmentHoverBrush)
                {
                    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(segRect, 6.0f, 6.0f);
                    ctx->FillRoundedRectangle(rr, segmentHoverBrush);
                }

                // Draw text at original x (segRect.left + padH) so it aligns correctly
                ctx->DrawTextLayout(D2D1::Point2F(x, pathRectF.top), segLayout, textBrush);
                segLayout->Release();

                x += segW + 6.0f; // spacing

                // Draw chevron if not last
                if (i + 1 < parts.size())
                {
                    ID2D1Bitmap *cbmp = GetExplorerManager().LoadSvgIconPublic(ctx, chevronAsset, arrowPx, dpi);
                    if (cbmp)
                    {
                        float y = pathRectF.top + (availableHeight - arrowF) * 0.5f;
                        D2D1_RECT_F dst = D2D1::RectF(x, y, x + arrowF, y + arrowF);
                        ctx->DrawBitmap(cbmp, dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                        cbmp->Release();
                        x += arrowF + 6.0f;
                    }
                }
            }
            else
            {
                // Fallback: draw raw text
                ctx->DrawTextW(seg.c_str(), (UINT32)seg.size(), pathFmt, D2D1::RectF(x, pathRectF.top, pathRectF.right, pathRectF.bottom), textBrush);
                break;
            }
        }
    }

    // expose the last computed segment rects for mouse handling
    // store in a static variable accessible by the footer mouse handlers below
    // (we keep s_segmentRects alive via a static pointer wrapper)

    if (segmentHoverBrush)
        segmentHoverBrush->Release();

    // Optional centered hint (e.g., shortcut prompts)
    if (!g_footer_hint.empty() && dwrite)
    {
        IDWriteTextFormat *hintFmt = nullptr;
        dwrite->CreateTextFormat(L"JetBrains Mono", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                 DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &hintFmt);
        if (hintFmt)
        {
            hintFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            hintFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        if (hintFmt && textBrush)
        {
            float hintLeft = left + 180.0f;
            float hintRight = right - 200.0f;
            if (hintRight > hintLeft + 40.0f)
            {
                D2D1_RECT_F hintRect = D2D1::RectF(hintLeft, top, hintRight, bottom);

                IDWriteTextLayout *measureLayout = nullptr;
                if (SUCCEEDED(dwrite->CreateTextLayout(g_footer_hint.c_str(), (UINT32)g_footer_hint.size(), hintFmt,
                                                     1000.0f, hintRect.bottom - hintRect.top, &measureLayout)) && measureLayout)
                {
                    measureLayout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                    measureLayout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);

                    DWRITE_TEXT_METRICS metrics;
                    measureLayout->GetMetrics(&metrics);
                    float padX = 10.0f;
                    float pillW = metrics.widthIncludingTrailingWhitespace + padX * 2.0f;
                    float maxW = hintRect.right - hintRect.left;
                    float hardMax = 360.0f;
                    if (pillW > maxW) pillW = maxW;
                    if (pillW > hardMax) pillW = hardMax;

                    float centerX = (left + right) * 0.5f;
                    float pillLeft = centerX - pillW * 0.5f;
                    float pillRight = pillLeft + pillW;
                    if (pillLeft < hintRect.left)
                    {
                        pillLeft = hintRect.left;
                        pillRight = pillLeft + pillW;
                    }
                    if (pillRight > hintRect.right)
                    {
                        pillRight = hintRect.right;
                        pillLeft = pillRight - pillW;
                    }

                    IDWriteTextLayout *hintLayout = nullptr;
                    if (SUCCEEDED(dwrite->CreateTextLayout(g_footer_hint.c_str(), (UINT32)g_footer_hint.size(), hintFmt,
                                                         pillW, hintRect.bottom - hintRect.top, &hintLayout)) && hintLayout)
                    {
                        hintLayout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                        hintLayout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                        DWRITE_TRIMMING trimming = {};
                        trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
                        IDWriteInlineObject *ellipsis = nullptr;
                        if (SUCCEEDED(dwrite->CreateEllipsisTrimmingSign(hintFmt, &ellipsis)))
                        {
                            hintLayout->SetTrimming(&trimming, ellipsis);
                            ellipsis->Release();
                        }

                        ID2D1SolidColorBrush *hintBg = nullptr;
                        ctx->CreateSolidColorBrush(D2D1::ColorF(0x2a2d2e, 0.6f), &hintBg);
                        if (hintBg)
                        {
                            D2D1_RECT_F pillRect = D2D1::RectF(pillLeft, top + 4.0f, pillRight, bottom - 4.0f);
                            D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(pillRect, 6.0f, 6.0f);
                            ctx->FillRoundedRectangle(rr, hintBg);
                            hintBg->Release();
                        }

                        ctx->DrawTextLayout(D2D1::Point2F(pillLeft, top), hintLayout, textBrush);
                        hintLayout->Release();
                    }

                    measureLayout->Release();
                }
            }
        }

        if (hintFmt)
            hintFmt->Release();
    }

    // Draw the right-side status text
    if (statusFmt && textBrush)
    {
        float textLeft = left + 12.0f + (float)(notifR * 2) + 6.0f;
        D2D1_RECT_F statusRect = D2D1::RectF(textLeft, top, right - 12.0f, bottom);
        ctx->DrawTextW(buf, (UINT32)wcslen(buf), statusFmt, statusRect, textBrush, D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
    }

    if (statusFmt) statusFmt->Release();
    if (pathFmt) pathFmt->Release();
    if (bgBrush) bgBrush->Release();
    if (borderBrush) borderBrush->Release();
    if (textBrush) textBrush->Release();
}

void Footer_SetHint(HWND hwnd, const std::wstring &text, unsigned int durationMs)
{
    g_footer_hint = text;
    g_footer_hint_until = (durationMs > 0) ? (GetTickCount64() + durationMs) : 0;
    if (hwnd)
        InvalidateRect(hwnd, NULL, FALSE);
}

void Footer_ClearHint(HWND hwnd)
{
    if (!g_footer_hint.empty())
    {
        g_footer_hint.clear();
        g_footer_hint_until = 0;
        if (hwnd)
            InvalidateRect(hwnd, NULL, FALSE);
    }
}

// Mouse handling: update hovered segment based on mouse position and invalidate when changed
void Footer_OnMouseMove(HWND hwnd, POINT pt)
{
    int newHover = -1;
    for (size_t i = 0; i < g_footer_segment_rects.size(); ++i)
    {
        const D2D1_RECT_F &r = g_footer_segment_rects[i];
        if (pt.x >= (int)r.left && pt.x <= (int)r.right && pt.y >= (int)r.top && pt.y <= (int)r.bottom)
        {
            newHover = (int)i;
            break;
        }
    }

    if (newHover != g_footer_hovered_index)
    {
        g_footer_hovered_index = newHover;
        InvalidateRect(hwnd, NULL, FALSE);
    }
}

void Footer_ClearHover(HWND hwnd)
{
    if (g_footer_hovered_index != -1)
    {
        g_footer_hovered_index = -1;
        InvalidateRect(hwnd, NULL, FALSE);
    }
}
