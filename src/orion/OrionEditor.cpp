#include "OrionEditor.h"
#include "../helpers/window_helpers.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include "syntax/Highlighter.h"
#include "CompletionPopup.h"
#include "completion/CompletionService.h"
#include "completion/providers/HtmlCompletionProvider.h"
#include "completion/providers/CppCompletionProvider.h"
#include <dwrite_1.h>
#include <filesystem>
#include <excpt.h>
#include "rendering/gutter/Gutter.h"
#include "geometry/TextColumns.h"

// New rendering/geometry helpers
#include "geometry/IndentationHelper.h"
#include "rendering/GuideRenderer.h"
// Selection rendering
#include "selection/Selection.h"

#include "caret/Caret.h"

#include "ui/components/scrollbar/Scrollbar.h"
#include "utils/logger/Logger.h"

// Ensure Windows min/max macros don't interfere with std::min/std::max
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

namespace Orion
{
    static bool SafeHitTestTextPosition(
        IDWriteTextLayout *layout,
        UINT32 textPosition,
        FLOAT *outX,
        FLOAT *outY,
        DWRITE_HIT_TEST_METRICS *outMetrics)
    {
        if (!layout || !outX || !outY || !outMetrics)
            return false;

        __try
        {
            HRESULT hr = layout->HitTestTextPosition(textPosition, FALSE, outX, outY, outMetrics);
            return SUCCEEDED(hr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    Editor::Editor()
    {
        // TODO : A simplifier
        state_.lastBlinkTime = GetTickCount();
        highlighter_ = new ::Orion::Syntax::Highlighter();
        cachedTextFormat_ = nullptr;
        completionPopup_ = new CompletionPopup();

        // Initialize completion service and register providers
        completionService_ = std::make_unique<Completion::CompletionService>();
        completionService_->RegisterProvider(std::make_unique<Completion::HtmlCompletionProvider>());
        completionService_->RegisterProvider(std::make_unique<Completion::CppCompletionProvider>());

        Geometry::IndentConfig indentConfig = Geometry::IndentConfig{4, 8.0f};
        // characterWidth will be updated in Draw()
        indentHelper_ = std::make_unique<Geometry::IndentationHelper>(indentConfig);

        Rendering::GuideStyle guideStyle;
        guideStyle.normalColor = D2D1::ColorF(0.3f, 0.3f, 0.35f, 0.5f);
        guideStyle.activeColor = D2D1::ColorF(0.4f, 0.4f, 0.5f, 0.7f);

        guideRenderer_ = std::make_unique<Rendering::GuideRenderer>(indentConfig, guideStyle);

        // Configure selection rendering (use theme_.selection)
        Rendering::SelectionConfig selectionConfig;
        selectionConfig.color = theme_.selection;
        selectionConfig.cornerRadius = 3.0f;
        selectionConfig.style = Rendering::SelectionStyle::RoundedSmart;

        selection_ = std::make_unique<Rendering::Selection>(selectionConfig);
    }

    Editor::~Editor()
    {
        if (customFontCollection_)
        {
            customFontCollection_->Release();
            customFontCollection_ = nullptr;
        }

        if (fontLoader_)
        {
            if (fontCollectionRegisteredFactory_)
            {
                fontCollectionRegisteredFactory_->UnregisterFontCollectionLoader(fontLoader_);
                fontCollectionRegisteredFactory_->Release();
                fontCollectionRegisteredFactory_ = nullptr;
            }
            fontLoader_->Release();
            fontLoader_ = nullptr;
        }

        if (highlighter_)
        {
            delete highlighter_;
            highlighter_ = nullptr;
        }
        if (cachedTextFormat_)
        {
            cachedTextFormat_->Release();
            cachedTextFormat_ = nullptr;
        }
        if (completionPopup_)
        {
            delete completionPopup_;
            completionPopup_ = nullptr;
        }
    }

    void Editor::LoadFile(const std::wstring &filePath)
    {
        state_.filePath = filePath;
        state_.lines.clear();
        state_.caret = {0, 0};
        state_.scrollOffsetX = 0.0f;
        state_.scrollOffsetY = 0.0f;
        state_.encoding = L"Unknown";

        // --- Ouvrir le fichier (on garde ton approche: chemin -> UTF-8) ---
        int size_needed = WideCharToMultiByte(CP_UTF8, 0,
                                              filePath.c_str(), (int)filePath.size(),
                                              nullptr, 0, nullptr, nullptr);
        std::string pathUtf8(size_needed, '\0');
        WideCharToMultiByte(CP_UTF8, 0,
                            filePath.c_str(), (int)filePath.size(),
                            pathUtf8.data(), size_needed, nullptr, nullptr);

        std::ifstream file(pathUtf8, std::ios::binary);
        if (!file.is_open())
        {
            state_.lines.push_back(L"// Could not open file");
            state_.encoding = L"";
            return;
        }

        // --- Lire tout le fichier ---
        std::string data;
        file.seekg(0, std::ios::end);
        std::streamoff fsize = file.tellg();
        if (fsize > 0)
        {
            file.seekg(0, std::ios::beg);
            data.resize((size_t)fsize);
            file.read(&data[0], fsize);
        }
        file.close();

        // Helpers locaux
        auto split_wlines = [&](const std::wstring &w)
        {
            state_.lines.clear();
            size_t start = 0;
            size_t i = 0;
            const size_t n = w.size();

            while (i < n)
            {
                wchar_t c = w[i];
                if (c == L'\r' || c == L'\n')
                {
                    state_.lines.emplace_back(w.substr(start, i - start));

                    // consommer \r\n
                    if (c == L'\r' && (i + 1) < n && w[i + 1] == L'\n')
                        i++;

                    i++;
                    start = i;
                }
                else
                {
                    i++;
                }
            }

            // dernière ligne (même vide)
            state_.lines.emplace_back(w.substr(start));
            if (state_.lines.empty())
                state_.lines.push_back(L"");
        };

        auto decode_utf8_strict = [&](const char *src, int len, std::wstring &out) -> bool
        {
            out.clear();
            if (len <= 0)
                return true;

            int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, len, nullptr, 0);
            if (wlen <= 0)
                return false;

            out.resize((size_t)wlen);
            int wlen2 = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, len, out.data(), wlen);
            return wlen2 == wlen;
        };

        auto decode_ansi = [&](const char *src, int len, std::wstring &out)
        {
            out.clear();
            if (len <= 0)
                return;

            int wlen = MultiByteToWideChar(CP_ACP, 0, src, len, nullptr, 0);
            if (wlen <= 0)
                return;

            out.resize((size_t)wlen);
            MultiByteToWideChar(CP_ACP, 0, src, len, out.data(), wlen);
        };

        // --- Détection BOM + décodage robuste ---
        const unsigned char *p = reinterpret_cast<const unsigned char *>(data.data());
        const size_t len = data.size();

        std::wstring decoded;

        if (len >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF)
        {
            // UTF-8 BOM
            state_.encoding = L"UTF-8";
            const char *src = data.data() + 3;
            const int slen = (int)(len - 3);

            // BOM => on accepte en non-strict aussi, mais strict marche très bien
            if (!decode_utf8_strict(src, slen, decoded))
            {
                // ultra rare, mais au pire on tente sans flag
                int wlen = MultiByteToWideChar(CP_UTF8, 0, src, slen, nullptr, 0);
                decoded.resize((size_t)wlen);
                MultiByteToWideChar(CP_UTF8, 0, src, slen, decoded.data(), wlen);
            }

            split_wlines(decoded);
        }
        else if (len >= 2 && p[0] == 0xFF && p[1] == 0xFE)
        {
            // UTF-16 LE BOM
            state_.encoding = L"UTF-16 LE";
            const size_t byteCount = len - 2;
            const size_t wcharCount = byteCount / 2;

            std::wstring w;
            w.resize(wcharCount);
            for (size_t i = 0; i < wcharCount; ++i)
            {
                unsigned char lo = p[2 + i * 2];
                unsigned char hi = p[2 + i * 2 + 1];
                w[i] = (wchar_t)((hi << 8) | lo);
            }

            split_wlines(w);
        }
        else if (len >= 2 && p[0] == 0xFE && p[1] == 0xFF)
        {
            // UTF-16 BE BOM
            state_.encoding = L"UTF-16 BE";
            const size_t byteCount = len - 2;
            const size_t wcharCount = byteCount / 2;

            std::wstring w;
            w.resize(wcharCount);
            for (size_t i = 0; i < wcharCount; ++i)
            {
                unsigned char hi = p[2 + i * 2];
                unsigned char lo = p[2 + i * 2 + 1];
                w[i] = (wchar_t)((hi << 8) | lo);
            }

            split_wlines(w);
        }
        else
        {
            // Pas de BOM -> on tente UTF-8 strict SUR TOUT LE BUFFER (plus fiable que ligne par ligne)
            if (decode_utf8_strict(data.data(), (int)len, decoded))
            {
                state_.encoding = L"UTF-8";
                split_wlines(decoded);
            }
            else
            {
                // fallback ANSI
                state_.encoding = L"ANSI";
                decode_ansi(data.data(), (int)len, decoded);
                split_wlines(decoded);
            }
        }

        if (state_.lines.empty())
            state_.lines.push_back(L"");
    }

    void Editor::UpdateLayout(HWND hwnd, float left, float top, float right, float bottom)
    {
        (void)hwnd;
        state_.leftEdge = left;
        state_.topEdge = top;
        state_.rightEdge = right;
        state_.bottomEdge = bottom;

        // Update scrollbar layout: allow scrolling beyond last line so the final
        // line can be positioned at the top (like VSCode's "scrollBeyondLastLine").
        float width = state_.rightEdge - state_.leftEdge;
        float height = state_.bottomEdge - state_.topEdge;

        // Base content height is number of lines * lineHeight
        float baseContentHeight = (float)state_.lines.size() * metrics_.lineHeight;

        // Compute extra margin so the last line can be scrolled up to the top of the viewport:
        // extra = viewportHeight - lineHeight (clamped >= 0)
        float extra = height - metrics_.lineHeight;
        if (extra < 0.0f)
            extra = 0.0f;

        float contentHeight = baseContentHeight + extra;

        scrollbar_.UpdateLayout(state_.leftEdge, state_.topEdge, width, height, contentHeight);

        // synchronize editor scroll offset with scrollbar
        state_.scrollOffsetY = scrollbar_.GetScrollOffset();
        // Horizontal scrollbar: compute content width based on longest line
        float availableWidth = (right - left) - metrics_.gutterWidth;
        // Reserve space for vertical scrollbar if visible
        if (scrollbar_.IsVisible())
            availableWidth -= 14.0f;

        size_t maxLen = 0;
        for (const auto &ln : state_.lines)
            maxLen = (std::max)(maxLen, ln.size());

        float contentWidth = maxLen * metrics_.characterWidth + 20.0f; // padding
        hContentWidth_ = contentWidth;
        hViewportWidth_ = availableWidth;
        hScrollbarVisible_ = contentWidth > availableWidth;
        if (hScrollbarVisible_)
        {
            float ratio = hViewportWidth_ / hContentWidth_;
            hThumbWidth_ = (std::max)(hViewportWidth_ * ratio, 30.0f);
            float maxScroll = hContentWidth_ - hViewportWidth_;
            float availableTrack = hViewportWidth_ - hThumbWidth_;
            if (maxScroll > 0.0f)
                hThumbPos_ = (state_.scrollOffsetX / maxScroll) * availableTrack;
            else
                hThumbPos_ = 0.0f;
        }
        // ✨ NOUVEAU : Mettre à jour le layout du SearchBox
        float editorWidth = right - left - metrics_.gutterWidth;
        searchBox_.UpdateLayout(left + metrics_.gutterWidth, top, editorWidth);

        // Update completion popup layout near caret position
        if (completionPopup_ && state_.caret.line >= 0 && state_.caret.line < (int)state_.lines.size())
        {
            D2D1_POINT_2F screen = TextToScreenPosition(state_.caret);
            completionPopup_->UpdateLayout(screen.x, screen.y + metrics_.lineHeight, 400.0f, metrics_.lineHeight);
        }
    }

    // Tabs are preserved in the buffer; visual expansion is handled during rendering/measurement.

    void Editor::CreateEmpty()
    {
        state_.lines.clear();
        state_.lines.push_back(L"");
        state_.caret = {0, 0};
        state_.filePath.clear();
        state_.scrollOffsetX = 0.0f;
        state_.scrollOffsetY = 0.0f;
    }

    bool Editor::SaveToFile(const std::wstring &filePath)
    {
        // Convert wide path to UTF-8
        int needed = WideCharToMultiByte(CP_UTF8, 0, filePath.c_str(), (int)filePath.size(), NULL, 0, NULL, NULL);
        if (needed <= 0)
            return false;
        std::string pathUtf8(needed, '\0');
        WideCharToMultiByte(CP_UTF8, 0, filePath.c_str(), (int)filePath.size(), pathUtf8.data(), needed, NULL, NULL);

        std::ofstream ofs(pathUtf8, std::ios::binary);
        if (!ofs.is_open())
            return false;

        for (size_t i = 0; i < state_.lines.size(); ++i)
        {
            // Convert each wstring line to UTF-8
            const std::wstring &wline = state_.lines[i];
            int sz = WideCharToMultiByte(CP_UTF8, 0, wline.c_str(), (int)wline.size(), NULL, 0, NULL, NULL);
            std::string lineUtf8(sz, '\0');
            if (sz > 0)
                WideCharToMultiByte(CP_UTF8, 0, wline.c_str(), (int)wline.size(), lineUtf8.data(), sz, NULL, NULL);
            ofs << lineUtf8;
            if (i + 1 < state_.lines.size())
                ofs << "\n";
        }

        ofs.close();
        if (!ofs)
            return false;

        state_.filePath = filePath;
        return true;
    }

    // Méthode Draw principale
    void Editor::Draw(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite)
    {
        if (!ctx || !dwrite)
            return;

        pDWriteFactory_ = dwrite;

        // Créer le format texte caché si nécessaire
        if (!cachedTextFormat_)
        {
            HRESULT hr = pDWriteFactory_->CreateTextFormat(
                L"JetBrains Mono",
                customFontCollection_,
                DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                14.0f,
                L"en-us",
                &cachedTextFormat_);

            if (SUCCEEDED(hr) && cachedTextFormat_)
            {
                cachedTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                cachedTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                cachedTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            }
            else
            {
            }
        }

        D2D1_ANTIALIAS_MODE oldAA = ctx->GetAntialiasMode();
        D2D1_TEXT_ANTIALIAS_MODE oldTextAA = ctx->GetTextAntialiasMode();

        // Clip de l'éditeur complet (background + gutter)
        D2D1_RECT_F editorClip = D2D1::RectF(
            state_.leftEdge,
            state_.topEdge,
            state_.rightEdge,
            state_.bottomEdge);

        ctx->PushAxisAlignedClip(editorClip, D2D1_ANTIALIAS_MODE_ALIASED);

        // Remplir le fond de l'éditeur
        {
            ID2D1SolidColorBrush *bg = nullptr;
            ctx->CreateSolidColorBrush(theme_.background, &bg);
            if (bg)
            {
                ctx->FillRectangle(editorClip, bg);
                bg->Release();
            }
        }

        // Dessiner la gouttière et les numéros de ligne
        Orion::Gutter gutter;
        // Recalculate gutter width dynamically based on content
        metrics_.gutterWidth = gutter.CalculateGutterWidth(state_, metrics_);
        gutter.DrawGutter(ctx, state_, theme_, metrics_);
        gutter.DrawLineNumbers(ctx, dwrite, state_, theme_, metrics_, customFontCollection_);

        // Clip du contenu (exclure la gouttière et réserver l'espace pour les scrollbars)
        // Inclure leftPadding pour que les clips et hit-tests soient cohérents
        float contentLeft = state_.leftEdge + metrics_.gutterWidth + metrics_.leftPadding;
        float contentRight = state_.rightEdge - (scrollbar_.IsVisible() ? 14.0f : 0.0f);
        float contentBottom = state_.bottomEdge - (hScrollbarVisible_ ? 14.0f : 0.0f);
        D2D1_RECT_F contentClip = D2D1::RectF(contentLeft, state_.topEdge, contentRight, contentBottom);
        ctx->PushAxisAlignedClip(contentClip, D2D1_ANTIALIAS_MODE_ALIASED);

        // Dessiner les éléments de contenu
        DrawActiveLine(ctx);
        DrawSelection(ctx); // ✅ Dessiner la sélection AVANT le texte
        DrawTextContent(ctx, dwrite);
        DrawSearchMatches(ctx);
        DrawWhitespaceIndicators(ctx);
        DrawCaret(ctx);

        // Dessiner le popup de complétion si visible
        if (completionPopup_ && completionPopup_->IsVisible())
        {
            completionPopup_->Draw(ctx, dwrite);
        }

        // Pop du clip de contenu
        ctx->PopAxisAlignedClip();

        // Dessiner la scrollbar verticale
        scrollbar_.Draw(ctx);

        // Dessiner la scrollbar horizontale si nécessaire
        if (hScrollbarVisible_)
        {
            float hLeft = state_.leftEdge + metrics_.gutterWidth;
            float hRight = state_.rightEdge - (scrollbar_.IsVisible() ? 14.0f : 0.0f);
            float hTop = state_.bottomEdge - 14.0f;
            float hBottom = state_.bottomEdge;

            // Track (use gutter background from theme)
            ID2D1SolidColorBrush *trackBrush = nullptr;
            D2D1_COLOR_F trackColor = theme_.gutterBackground;
            trackColor.a = 1.0f;
            ctx->CreateSolidColorBrush(trackColor, &trackBrush);
            if (trackBrush)
            {
                ctx->FillRectangle(D2D1::RectF(hLeft, hTop, hRight, hBottom), trackBrush);
                trackBrush->Release();
            }

            // Thumb (use theme text color with adjusted alpha)
            D2D1_COLOR_F thumbColor = theme_.text;
            if (hIsDragging_)
                thumbColor.a = 0.9f;
            else
                thumbColor.a = 0.4f;

            ID2D1SolidColorBrush *thumbBrush = nullptr;
            ctx->CreateSolidColorBrush(thumbColor, &thumbBrush);

            float thumbLeft = hLeft + hThumbPos_;
            float thumbRight = thumbLeft + hThumbWidth_;
            D2D1_ROUNDED_RECT thumbRect = D2D1::RoundedRect(
                D2D1::RectF(thumbLeft + 4.0f, hTop + 2.0f, thumbRight - 4.0f, hBottom - 2.0f),
                3.0f, 3.0f);

            if (thumbBrush)
            {
                ctx->FillRoundedRectangle(thumbRect, thumbBrush);
                thumbBrush->Release();
            }
        }

        // Pop du clip de l'éditeur
        ctx->PopAxisAlignedClip();

        // Dessiner le SearchBox par-dessus tout
        if (searchBox_.IsVisible())
        {
            searchBox_.Draw(ctx, dwrite);
        }

        // Restaurer les paramètres d'antialiasing
        ctx->SetAntialiasMode(oldAA);
        ctx->SetTextAntialiasMode(oldTextAA);
    }

    void Editor::ShowSearch()
    {
        searchBox_.Show();
        // Si il y a une sélection, l'utiliser comme texte de recherche initial
        if (state_.hasSelection)
        {
            std::wstring sel = GetSelectionText();
            if (!sel.empty() && sel.find(L'\n') == std::wstring::npos)
            {
                searchBox_.SetSearchText(sel);
            }
        }
    }

    void Editor::HideSearch()
    {
        searchBox_.Hide();
    }

    void Editor::DrawSearchMatches(ID2D1RenderTarget *ctx)
    {
        if (!searchBox_.IsVisible())
            return;

        const auto &matches = searchBox_.GetMatches();
        if (matches.empty())
            return;

        int currentMatchIdx = searchBox_.GetCurrentMatchIndex();

        ID2D1SolidColorBrush *matchBrush = nullptr;
        ID2D1SolidColorBrush *currentMatchBrush = nullptr;
        ctx->CreateSolidColorBrush(D2D1::ColorF(0.8f, 0.6f, 0.0f, 0.4f), &matchBrush);
        ctx->CreateSolidColorBrush(D2D1::ColorF(0.9f, 0.4f, 0.0f, 0.6f), &currentMatchBrush);

        float contentLeft = state_.leftEdge + metrics_.gutterWidth + metrics_.leftPadding;

        for (size_t i = 0; i < matches.size(); ++i)
        {
            const auto &match = matches[i];

            float lineY = state_.topEdge + (match.line * metrics_.lineHeight) - state_.scrollOffsetY;
            if (lineY + metrics_.lineHeight < state_.topEdge || lineY > state_.bottomEdge)
                continue;

            float startX = contentLeft - state_.scrollOffsetX;
            float endX = contentLeft - state_.scrollOffsetX;

            if (pDWriteFactory_ && match.line >= 0 && match.line < (int)state_.lines.size())
            {
                const std::wstring &line = state_.lines[match.line];
                IDWriteTextFormat *format = cachedTextFormat_;
                IDWriteTextFormat *tmpFmt = nullptr;
                if (!format)
                {
                    pDWriteFactory_->CreateTextFormat(
                        L"JetBrains Mono", customFontCollection_,
                        DWRITE_FONT_WEIGHT_NORMAL,
                        DWRITE_FONT_STYLE_NORMAL,
                        DWRITE_FONT_STRETCH_NORMAL,
                        14.0f, L"en-us",
                        &tmpFmt);
                    if (tmpFmt)
                        format = tmpFmt;
                }

                if (format)
                {
                    if (match.startColumn > 0)
                    {
                        std::wstring textBefore = line.substr(0, match.startColumn);
                        IDWriteTextLayout *layout1 = nullptr;
                        if (SUCCEEDED(pDWriteFactory_->CreateTextLayout(
                                textBefore.c_str(),
                                (UINT32)textBefore.size(),
                                format,
                                10000.0f,
                                metrics_.lineHeight,
                                &layout1)) &&
                            layout1)
                        {
                            DWRITE_TEXT_METRICS tm1 = {};
                            layout1->GetMetrics(&tm1);
                            startX = contentLeft + tm1.width - state_.scrollOffsetX;
                            layout1->Release();
                        }
                    }

                    if (match.endColumn > 0 && match.endColumn <= (int)line.size())
                    {
                        std::wstring textBeforeEnd = line.substr(0, match.endColumn);
                        IDWriteTextLayout *layout2 = nullptr;
                        if (SUCCEEDED(pDWriteFactory_->CreateTextLayout(
                                textBeforeEnd.c_str(),
                                (UINT32)textBeforeEnd.size(),
                                format,
                                10000.0f,
                                metrics_.lineHeight,
                                &layout2)) &&
                            layout2)
                        {
                            DWRITE_TEXT_METRICS tm2 = {};
                            layout2->GetMetrics(&tm2);
                            endX = contentLeft + tm2.width - state_.scrollOffsetX;
                            layout2->Release();
                        }
                    }

                    if (tmpFmt)
                        tmpFmt->Release();
                }
            }

            D2D1_RECT_F matchRect = D2D1::RectF(startX, lineY, endX, lineY + metrics_.lineHeight);

            bool isCurrent = (currentMatchIdx >= 0 && (int)i == currentMatchIdx);
            ID2D1SolidColorBrush *brush = isCurrent ? currentMatchBrush : matchBrush;

            if (brush)
            {
                ctx->FillRectangle(matchRect, brush);
            }

            if (isCurrent)
            {
                ID2D1SolidColorBrush *borderBrush = nullptr;
                ctx->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.5f, 0.0f), &borderBrush);
                if (borderBrush)
                {
                    ctx->DrawRectangle(matchRect, borderBrush, 1.5f);
                    borderBrush->Release();
                }
            }
        }

        if (matchBrush)
            matchBrush->Release();
        if (currentMatchBrush)
            currentMatchBrush->Release();
    }

    void Editor::DrawWhitespaceIndicators(ID2D1RenderTarget *ctx)
    {
        int firstVisibleLine = (int)(state_.scrollOffsetY / metrics_.lineHeight);
        int lastVisibleLine = (int)((state_.scrollOffsetY + (state_.bottomEdge - state_.topEdge)) / metrics_.lineHeight) + 1;

        firstVisibleLine = (std::max)(0, firstVisibleLine);
        lastVisibleLine = (std::min)((int)state_.lines.size(), lastVisibleLine);

        float contentLeft = state_.leftEdge + metrics_.gutterWidth + metrics_.leftPadding;

        const D2D1_COLOR_F colColorsArr[] = {
            theme_.keyword,
            theme_.string,
            theme_.comment,
            theme_.number,
            theme_.function,
            theme_.variable};
        const size_t colCount = sizeof(colColorsArr) / sizeof(colColorsArr[0]);

        ID2D1SolidColorBrush *colBrushesArr[16] = {0};
        for (size_t idx = 0; idx < colCount; ++idx)
        {
            D2D1_COLOR_F tmp = colColorsArr[idx];
            tmp.a = 0.85f;
            ID2D1SolidColorBrush *b = nullptr;
            ctx->CreateSolidColorBrush(tmp, &b);
            colBrushesArr[idx] = b;
        }

        ID2D1SolidColorBrush *fallbackBrush = nullptr;
        D2D1_COLOR_F fallbackColor = theme_.text;
        fallbackColor.a = 0.5f;
        ctx->CreateSolidColorBrush(fallbackColor, &fallbackBrush);
        if (!fallbackBrush)
        {
            for (size_t idx = 0; idx < colCount; ++idx)
                if (colBrushesArr[idx])
                    colBrushesArr[idx]->Release();
            return;
        }

        // Draw whitespace indicators for visible lines. We create a text layout per-line
        // so we can perform hit-tests to compute precise glyph positions.
        for (int i = firstVisibleLine; i < lastVisibleLine; ++i)
        {
            float lineY = state_.topEdge + (i * metrics_.lineHeight) - state_.scrollOffsetY;
            const std::wstring &line = state_.lines[i];

            if (line.empty())
                continue;

            IDWriteTextLayout *layout = nullptr;
            DWRITE_OVERHANG_METRICS om = {};
            bool haveLayout = false;

            if (pDWriteFactory_ && cachedTextFormat_)
            {
                HRESULT hr = pDWriteFactory_->CreateTextLayout(
                    line.c_str(),
                    (UINT32)line.size(),
                    cachedTextFormat_,
                    10000.0f,
                    metrics_.lineHeight,
                    &layout);

                if (SUCCEEDED(hr) && layout)
                {
                    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                    // Désactiver les ligatures comme partout ailleurs
                    IDWriteTypography *typography = nullptr;
                    if (SUCCEEDED(pDWriteFactory_->CreateTypography(&typography)) && typography)
                    {
                        DWRITE_FONT_FEATURE ff = {DWRITE_MAKE_FONT_FEATURE_TAG('l', 'i', 'g', 'a'), 0};
                        typography->AddFontFeature(ff);
                        DWRITE_TEXT_RANGE fullRange = {0, (UINT32)line.size()};
                        layout->SetTypography(typography, fullRange);
                        typography->Release();
                    }

                    if (SUCCEEDED(layout->GetOverhangMetrics(&om)))
                        haveLayout = true;
                }
            }
            const float cw = metrics_.characterWidth;

            int visualCol = 0;
            for (size_t col = 0; col < line.size(); ++col)
            {
                wchar_t ch = line[col];

                // largeur en colonnes pour ce char (tab = plusieurs colonnes)
                int visualColNext = Orion::Geometry::AdvanceVisualCol(visualCol, ch, 4);

                // X de début/fin de cellule (ou "span" pour tab)
                float charX = contentLeft - state_.scrollOffsetX + (visualCol * cw);
                float nextX = contentLeft - state_.scrollOffsetX + (visualColNext * cw);

                // On ne dessine que pour espaces/tabs/nbsp
                if (ch == L' ' || ch == L'\t' || ch == L'\u00A0')
                {
                    float cellWidth = nextX - charX;

                    float rectWidth = (std::max)(3.0f, cellWidth * 0.6f);
                    float rectHeight = (std::max)(3.0f, metrics_.lineHeight * 0.35f);

                    float centerX = (charX + nextX) * 0.5f;
                    float centerY = lineY + (metrics_.lineHeight * 0.5f);

                    D2D1_RECT_F rect = D2D1::RectF(
                        centerX - rectWidth * 0.5f,
                        centerY - rectHeight * 0.5f,
                        centerX + rectWidth * 0.5f,
                        centerY + rectHeight * 0.5f);

                    ctx->FillRectangle(rect, fallbackBrush);
                }

                visualCol = visualColNext;
            }

            // Draw the actual text for the line using a fresh layout and the
            // default brush. We create a separate layout to ensure drawing
            // uses the same typography and metrics as hit-testing.
            if (cachedTextFormat_)
            {
                IDWriteTextLayout *lineLayout = nullptr;
                HRESULT hr2 = pDWriteFactory_->CreateTextLayout(
                    line.c_str(),
                    (UINT32)line.size(),
                    cachedTextFormat_,
                    10000.0f,
                    metrics_.lineHeight,
                    &lineLayout);

                if (SUCCEEDED(hr2) && lineLayout)
                {
                    lineLayout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

                    IDWriteTypography *typography2 = nullptr;
                    if (SUCCEEDED(pDWriteFactory_->CreateTypography(&typography2)) && typography2)
                    {
                        DWRITE_FONT_FEATURE ff = {DWRITE_MAKE_FONT_FEATURE_TAG('l', 'i', 'g', 'a'), 0};
                        typography2->AddFontFeature(ff);
                        DWRITE_TEXT_RANGE fullRange = {0, (UINT32)line.size()};
                        lineLayout->SetTypography(typography2, fullRange);
                        typography2->Release();
                    }

                    DWRITE_TEXT_METRICS tm2 = {};
                    if (SUCCEEDED(lineLayout->GetMetrics(&tm2)))
                    {
                        DWRITE_OVERHANG_METRICS om2 = {};
                        if (SUCCEEDED(lineLayout->GetOverhangMetrics(&om2)))
                        {
                            float verticalOffset = 0.0f;
                            if (metrics_.lineHeight > tm2.height)
                                verticalOffset = (metrics_.lineHeight - tm2.height) / 2.0f;

                            ID2D1SolidColorBrush *drawBrush = nullptr;
                            ctx->CreateSolidColorBrush(theme_.text, &drawBrush);
                            CustomTextRenderer renderer(ctx, drawBrush ? drawBrush : nullptr);
                            float drawX = contentLeft - state_.scrollOffsetX;
                            float drawY = lineY + verticalOffset;
                            lineLayout->Draw(NULL, &renderer, drawX, drawY);
                            if (drawBrush)
                                drawBrush->Release();
                        }
                        else
                        {
                            ID2D1SolidColorBrush *drawBrush = nullptr;
                            ctx->CreateSolidColorBrush(theme_.text, &drawBrush);
                            CustomTextRenderer renderer(ctx, drawBrush ? drawBrush : nullptr);
                            float drawX = contentLeft - state_.scrollOffsetX;
                            float drawY = lineY;
                            lineLayout->Draw(NULL, &renderer, drawX, drawY);
                            if (drawBrush)
                                drawBrush->Release();
                        }
                    }

                    lineLayout->Release();
                }
            }

            if (layout)
            {
                layout->Release();
                layout = nullptr;
            }
        }

        for (size_t idx = 0; idx < colCount; ++idx)
            if (colBrushesArr[idx])
                colBrushesArr[idx]->Release();
        if (fallbackBrush)
            fallbackBrush->Release();
    }

    bool Editor::LoadCustomFont(IDWriteFactory *dwrite, const std::wstring &fontPath)
    {
        // Essayer de caster vers IDWriteFactory1 (disponible depuis Windows 7 SP1)
        IDWriteFactory1 *factory1 = nullptr;
        HRESULT hr = dwrite->QueryInterface(__uuidof(IDWriteFactory1), (void **)&factory1);

        // Create and register a font collection loader which will enumerate font files
        fontLoader_ = new CustomFontCollectionLoader();

        HRESULT regHr = dwrite->RegisterFontCollectionLoader(fontLoader_);
        if (FAILED(regHr))
        {
            fontLoader_->Release();
            fontLoader_ = nullptr;
            if (factory1)
                factory1->Release();
            return false;
        }

        // Remember the factory used for registration so we can unregister later
        fontCollectionRegisteredFactory_ = dwrite;
        fontCollectionRegisteredFactory_->AddRef();

        // Use the font path as the collection key
        const void *collectionKey = fontPath.c_str();
        UINT32 collectionKeySize = (UINT32)((fontPath.size() + 1) * sizeof(wchar_t));

        hr = factory1->CreateCustomFontCollection(
            fontLoader_,
            collectionKey,
            collectionKeySize,
            &customFontCollection_);

        // If creation failed, unregister and cleanup
        if (FAILED(hr) || !customFontCollection_)
        {
            // unregister and release the loader/factory
            fontCollectionRegisteredFactory_->UnregisterFontCollectionLoader(fontLoader_);
            fontLoader_->Release();
            fontLoader_ = nullptr;
            fontCollectionRegisteredFactory_->Release();
            fontCollectionRegisteredFactory_ = nullptr;
            factory1->Release();
            return false;
        }

        factory1->Release();

        // Vérifier que la police est bien dans la collection
        UINT32 index = 0;
        BOOL exists = FALSE;
        hr = customFontCollection_->FindFamilyName(L"JetBrains Mono", &index, &exists);

        if (FAILED(hr))
            return false;

        return exists == TRUE;
    }

    void Editor::DrawActiveLine(ID2D1RenderTarget *ctx)
    {
        // Do not draw active-line highlight when a selection is present
        if (state_.hasSelection)
            return;

        ID2D1SolidColorBrush *brush = nullptr;
        ctx->CreateSolidColorBrush(theme_.activeLineBackground, &brush);

        float contentLeft = state_.leftEdge + metrics_.gutterWidth + metrics_.leftPadding;
        float lineY = state_.topEdge + (state_.caret.line * metrics_.lineHeight) - state_.scrollOffsetY;

        D2D1_RECT_F rect = D2D1::RectF(
            contentLeft - metrics_.leftPadding,
            lineY,
            state_.rightEdge,
            lineY + metrics_.lineHeight);

        ctx->FillRectangle(rect, brush);

        if (brush)
            brush->Release();
    }

    void Editor::DrawSelection(ID2D1RenderTarget *ctx)
    {
        if (!state_.hasSelection)
            return;
        if (!selection_)
            return;

        float contentLeft = state_.leftEdge + metrics_.gutterWidth + metrics_.leftPadding;

        // Convertir les positions du caret en positions de sélection
        Orion::CaretPosition start = {state_.selectionStart.line, state_.selectionStart.column};
        Orion::CaretPosition end = {state_.caret.line, state_.caret.column};

        auto regions = Rendering::Selection::CalculateRegions(
            start,
            end,
            state_.lines,
            contentLeft,
            state_.topEdge,
            state_.scrollOffsetX,
            state_.scrollOffsetY,
            metrics_.lineHeight,
            4, // tab size removed; use literal
            metrics_.characterWidth,
            3.0f,
            pDWriteFactory_,
            cachedTextFormat_);

        // Filter out any invalid regions (defensive: NaN/inf/degenerate)
        std::vector<D2D1_ROUNDED_RECT> valid;
        for (const auto &r : regions)
        {
            const D2D1_RECT_F &rc = r.rect;
            if (!std::isfinite(rc.left) || !std::isfinite(rc.top) || !std::isfinite(rc.right) || !std::isfinite(rc.bottom))
                continue;
            // ignore excessively large or inverted rects
            if (rc.right <= rc.left || rc.bottom <= rc.top)
                continue;
            valid.push_back(r);
        }

        if (!valid.empty())
            selection_->Draw(ctx, valid);
    }

    void Editor::DrawCaret(ID2D1RenderTarget *ctx)
    {
        if (!state_.caretVisible)
            return;

        ID2D1SolidColorBrush *brush = nullptr;
        ctx->CreateSolidColorBrush(theme_.caret, &brush);

        D2D1_POINT_2F caretPos = TextToScreenPosition(state_.caret);

        D2D1_RECT_F rect = D2D1::RectF(
            caretPos.x,
            caretPos.y,
            caretPos.x + metrics_.caretWidth,
            caretPos.y + metrics_.lineHeight);

        ctx->FillRectangle(rect, brush);

        if (brush)
            brush->Release();
    }

    // ----- New helper implementations -----
    Geometry::IndentConfig Editor::GetIndentConfig() const
    {
        Geometry::IndentConfig config = Geometry::IndentConfig{4, metrics_.characterWidth};
        return config;
    }

    std::wstring Editor::GetFileExtension() const
    {
        if (state_.filePath.empty())
            return L"";

        size_t pos = state_.filePath.find_last_of(L'.');
        if (pos == std::wstring::npos)
            return L"";

        std::wstring ext = state_.filePath.substr(pos);
        for (auto &c : ext)
            c = towlower(c);
        return ext;
    }

    std::vector<int> Editor::CalculateHtmlDepths(int firstLine, int lastLine) const
    {
        int numLines = lastLine - firstLine;
        std::vector<int> cumDepth(numLines, 0);
        int depth = 0;

        for (int li = firstLine; li < lastLine && li < (int)state_.lines.size(); ++li)
        {
            const std::wstring &ln = state_.lines[li];
            cumDepth[li - firstLine] = depth;

            for (size_t p = 0; p < ln.size(); ++p)
            {
                if (ln[p] == L'<')
                {
                    if (p + 1 < ln.size() && (ln[p + 1] == L'!' || ln[p + 1] == L'?'))
                    {
                        size_t q = ln.find(L'>', p + 1);
                        if (q == std::wstring::npos)
                            break;
                        p = q;
                        continue;
                    }

                    bool closing = (p + 1 < ln.size() && ln[p + 1] == L'/');

                    size_t q = ln.find(L'>', p + 1);
                    if (q == std::wstring::npos)
                        break;

                    bool selfClosing = false;
                    if (q > p + 1 && ln[q - 1] == L'/')
                        selfClosing = true;

                    if (closing)
                    {
                        if (depth > 0)
                            depth--;
                    }
                    else if (!selfClosing)
                    {
                        depth++;
                    }

                    p = q;
                }
            }
        }

        return cumDepth;
    }

    std::wstring Editor::GetSelectionText() const
    {
        if (!state_.hasSelection)
            return L"";

        CaretPosition a = state_.selectionStart;
        CaretPosition b = state_.caret;
        if (a.line > b.line || (a.line == b.line && a.column > b.column))
            std::swap(a, b);

        std::wstring out;
        if (a.line == b.line)
        {
            if (a.line >= 0 && a.line < (int)state_.lines.size())
            {
                const std::wstring &line = state_.lines[a.line];
                int start = std::min<int>(std::max<int>(0, a.column), (int)line.size());
                int end = std::min<int>(std::max<int>(0, b.column), (int)line.size());
                if (end > start)
                    out = line.substr(start, end - start);
            }
            return out;
        }

        for (int L = a.line; L <= b.line && L < (int)state_.lines.size(); ++L)
        {
            const std::wstring &line = state_.lines[L];
            if (L == a.line)
            {
                int start = std::min<int>(std::max<int>(0, a.column), (int)line.size());
                out += line.substr(start);
            }
            else if (L == b.line)
            {
                int end = std::min<int>(std::max<int>(0, b.column), (int)line.size());
                out += line.substr(0, end);
            }
            else
            {
                out += line;
            }

            if (L < b.line)
                out.push_back(L'\n');
        }

        return out;
    }

    void Editor::CopySelectionToClipboard()
    {
        std::wstring sel = GetSelectionText();
        if (sel.empty())
            return;

        if (OpenClipboard(NULL))
        {
            EmptyClipboard();
            SIZE_T bytes = (sel.size() + 1) * sizeof(wchar_t);
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (hMem)
            {
                void *ptr = GlobalLock(hMem);
                if (ptr)
                {
                    memcpy(ptr, sel.c_str(), bytes);
                    GlobalUnlock(hMem);
                    SetClipboardData(CF_UNICODETEXT, hMem);
                }
                else
                {
                    GlobalFree(hMem);
                }
            }
            CloseClipboard();
        }
    }

    void Editor::CutSelectionToClipboard()
    {
        std::wstring sel = GetSelectionText();
        if (sel.empty())
            return;
        if (OpenClipboard(NULL))
        {
            EmptyClipboard();
            SIZE_T bytes = (sel.size() + 1) * sizeof(wchar_t);
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (hMem)
            {
                void *ptr = GlobalLock(hMem);
                if (ptr)
                {
                    memcpy(ptr, sel.c_str(), bytes);
                    GlobalUnlock(hMem);
                    SetClipboardData(CF_UNICODETEXT, hMem);
                }
                else
                {
                    GlobalFree(hMem);
                }
            }
            CloseClipboard();
        }

        // Delete selection after copying
        DeleteSelection();
    }

    void Editor::PasteFromClipboard()
    {
        if (OpenClipboard(NULL))
        {
            HANDLE hData = GetClipboardData(CF_UNICODETEXT);
            if (hData)
            {
                wchar_t *clip = static_cast<wchar_t *>(GlobalLock(hData));
                if (clip)
                {
                    std::wstring text(clip);
                    GlobalUnlock(hData);

                    // Normaliser les sauts de ligne: supprimer \r puis splitter sur \n
                    std::wstring tmp;
                    tmp.reserve(text.size());
                    for (size_t i = 0; i < text.size(); ++i)
                    {
                        if (text[i] == L'\r')
                            continue;
                        tmp.push_back(text[i]);
                    }

                    // Si sélection active, la supprimer avant insertion
                    if (state_.hasSelection)
                    {
                        DeleteSelection();
                    }

                    // Split par '\n'
                    std::vector<std::wstring> parts;
                    size_t start = 0;
                    while (start <= tmp.size())
                    {
                        size_t pos = tmp.find(L'\n', start);
                        if (pos == std::wstring::npos)
                        {
                            parts.push_back(tmp.substr(start));
                            break;
                        }
                        parts.push_back(tmp.substr(start, pos - start));
                        start = pos + 1;
                    }

                    if (parts.empty())
                    {
                        // nothing
                    }
                    else if (parts.size() == 1)
                    {
                        state_.lines[state_.caret.line].insert(state_.caret.column, parts[0]);
                        state_.caret.column += (int)parts[0].size();
                    }
                    else
                    {
                        std::wstring currentLine = state_.lines[state_.caret.line];
                        std::wstring before = currentLine.substr(0, state_.caret.column);
                        std::wstring after = currentLine.substr(state_.caret.column);

                        state_.lines[state_.caret.line] = before + parts[0];

                        int insertAt = state_.caret.line + 1;
                        for (size_t i = 1; i < parts.size(); ++i)
                        {
                            state_.lines.insert(state_.lines.begin() + insertAt, parts[i]);
                            insertAt++;
                        }

                        state_.lines[insertAt - 1] += after;
                        state_.caret.line = insertAt - 1;
                        state_.caret.column = (int)(state_.lines[state_.caret.line].size() - after.size());
                    }
                }
            }
            CloseClipboard();
        }
    }

    // (ExpandTabs removed)

    // Version corrigée avec gestion propre des brushes
    void Editor::DrawTextContent(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite)
    {
        const wchar_t *editorFont = L"JetBrains Mono";
        const float editorFontSize = 14.0f;
        IDWriteTextFormat *format = cachedTextFormat_;
        IDWriteTextFormat *tmpFmt = nullptr;
        if (!format && dwrite)
        {
            dwrite->CreateTextFormat(
                editorFont, customFontCollection_,
                DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                editorFontSize, L"en-us",
                &tmpFmt);
            if (tmpFmt)
                format = tmpFmt;
        }
        if (format && format != cachedTextFormat_)
        {
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }

        int firstVisibleLine = (int)(state_.scrollOffsetY / metrics_.lineHeight);
        int lastVisibleLine = (int)((state_.scrollOffsetY + (state_.bottomEdge - state_.topEdge)) / metrics_.lineHeight) + 1;

        firstVisibleLine = (std::max)(0, firstVisibleLine);
        lastVisibleLine = (std::min)((int)state_.lines.size(), lastVisibleLine);

        float contentLeft = state_.leftEdge + metrics_.gutterWidth + metrics_.leftPadding;
        float contentWidth = state_.rightEdge - contentLeft;
        const float scrollbarWidth = 14.0f;
        if (scrollbar_.IsVisible())
            contentWidth -= scrollbarWidth;

        std::wstring ext;
        if (!state_.filePath.empty())
        {
            size_t pos = state_.filePath.find_last_of(L'.');
            if (pos != std::wstring::npos)
            {
                ext = state_.filePath.substr(pos);
                for (auto &c : ext)
                    c = towlower(c);
            }
        }

        ID2D1SolidColorBrush *defaultBrush = nullptr;
        ctx->CreateSolidColorBrush(theme_.text, &defaultBrush);

        // --- Replaced guide rendering with GuideRenderer usage ---
        {
            // Update indent config from current metrics
            Geometry::IndentConfig indentConfig = GetIndentConfig();
            indentHelper_ = std::make_unique<Geometry::IndentationHelper>(indentConfig);

            // Configure style depending on extension
            Rendering::GuideStyle guideStyle;
            if (ext == L".html" || ext == L".htm")
            {
                guideStyle.normalColor = D2D1::ColorF(0.35f, 0.6f, 0.95f, 0.6f);
                guideStyle.activeColor = D2D1::ColorF(0.2f, 0.6f, 1.0f, 0.7f);
                // HTML lines should start a little higher to align with tag glyphs
                guideStyle.topMargin = 0.06f;
                guideStyle.bottomMargin = 0.06f;
            }
            else
            {
                guideStyle.normalColor = D2D1::ColorF(0.3f, 0.3f, 0.35f, 0.5f);
                guideStyle.activeColor = D2D1::ColorF(0.4f, 0.4f, 0.5f, 0.7f);
            }

            guideRenderer_ = std::make_unique<Rendering::GuideRenderer>(indentConfig, guideStyle);

            Rendering::GuideRenderContext renderCtx;
            renderCtx.contentLeft = contentLeft;
            renderCtx.topEdge = state_.topEdge;
            renderCtx.scrollOffsetY = state_.scrollOffsetY;
            renderCtx.scrollOffsetX = state_.scrollOffsetX;
            renderCtx.lineHeight = metrics_.lineHeight;
            renderCtx.firstVisibleLine = firstVisibleLine;
            renderCtx.lastVisibleLine = lastVisibleLine;
            renderCtx.caretLine = state_.caret.line;
            renderCtx.dwriteFactory = pDWriteFactory_;
            renderCtx.textFormat = format;

            if (ext == L".c" || ext == L".cpp" || ext == L".h" || ext == L".hpp")
            {
                guideRenderer_->DrawCppGuides(ctx, state_.lines, renderCtx);
            }
        }

        for (int i = firstVisibleLine; i < lastVisibleLine; ++i)
        {
            float lineY = state_.topEdge + (i * metrics_.lineHeight) - state_.scrollOffsetY;
            const std::wstring &line = state_.lines[i];

            if (line.empty())
                continue;

            IDWriteTextLayout *layout = nullptr;
            DWRITE_OVERHANG_METRICS om = {};
            bool haveLayout = false;

            if (pDWriteFactory_ && cachedTextFormat_)
            {
                HRESULT hr = pDWriteFactory_->CreateTextLayout(
                    line.c_str(),
                    (UINT32)line.size(),
                    cachedTextFormat_,
                    10000.0f,
                    metrics_.lineHeight,
                    &layout);

                if (SUCCEEDED(hr) && layout)
                {
                    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

                    IDWriteTypography *typography = nullptr;
                    if (SUCCEEDED(pDWriteFactory_->CreateTypography(&typography)) && typography)
                    {
                        DWRITE_FONT_FEATURE ff = {DWRITE_MAKE_FONT_FEATURE_TAG('l', 'i', 'g', 'a'), 0};
                        typography->AddFontFeature(ff);
                        DWRITE_TEXT_RANGE fullRange = {0, (UINT32)line.size()};
                        layout->SetTypography(typography, fullRange);
                        typography->Release();
                    }

                    if (SUCCEEDED(layout->GetOverhangMetrics(&om)))
                    {
                        haveLayout = true;
                    }
                    else
                    {
                    }
                }
                else
                {
                }
            }

            int visualCol = 0;

            for (size_t col = 0; col < line.size(); ++col)
            {
                wchar_t ch = line[col];

                if (ch != L' ' && ch != L'\t' && ch != L'\u00A0')
                {
                    visualCol++;
                    continue;
                }

                // Whitespace rendering moved to DrawWhitespaceIndicators.
                visualCol++;
                continue;
            }

            if (layout)
            {
                layout->Release();
                layout = nullptr;
            }
        }

        if (defaultBrush)
            defaultBrush->Release();
        if (tmpFmt)
            tmpFmt->Release();
    }

    D2D1_COLOR_F Editor::GetTokenColor(::Orion::Syntax::TokenType type, const std::wstring &ext) const
    {
        // Default theme-neutral colors
        if (ext == L".py")
        {
            switch (type)
            {
            case ::Orion::Syntax::TokenType::Keyword:
                return D2D1::ColorF(0.9f, 0.4f, 0.4f); // reddish
            case ::Orion::Syntax::TokenType::Type:
                return D2D1::ColorF(0.4f, 0.8f, 0.9f);
            case ::Orion::Syntax::TokenType::String:
                return D2D1::ColorF(0.6f, 0.8f, 0.5f);
            case ::Orion::Syntax::TokenType::Comment:
                return D2D1::ColorF(0.45f, 0.45f, 0.45f);
            case ::Orion::Syntax::TokenType::Number:
                return D2D1::ColorF(0.8f, 0.6f, 0.9f);
            default:
                return theme_.text;
            }
        }
        else if (ext == L".md")
        {
            switch (type)
            {
            case ::Orion::Syntax::TokenType::MarkdownHeading:
                return D2D1::ColorF(0.95f, 0.9f, 0.6f);
            case ::Orion::Syntax::TokenType::MarkdownCode:
                return D2D1::ColorF(0.8f, 0.8f, 0.85f);
            case ::Orion::Syntax::TokenType::MarkdownLink:
                return D2D1::ColorF(0.5f, 0.75f, 0.95f);
            default:
                return theme_.text;
            }
        }
        else if (ext == L".json")
        {
            switch (type)
            {
            case ::Orion::Syntax::TokenType::String:
                return D2D1::ColorF(0.6f, 0.8f, 0.5f);
            case ::Orion::Syntax::TokenType::Number:
                return D2D1::ColorF(0.8f, 0.6f, 0.9f);
            default:
                return theme_.text;
            }
        }

        else if (ext == L".js" || ext == L".ts")
        {
            switch (type)
            {
            case ::Orion::Syntax::TokenType::Keyword:
                return D2D1::ColorF(0.4f, 0.6f, 0.95f);
            case ::Orion::Syntax::TokenType::Type:
                return D2D1::ColorF(0.4f, 0.9f, 0.9f);
            case ::Orion::Syntax::TokenType::String:
                return D2D1::ColorF(0.6f, 0.9f, 0.6f);
            case ::Orion::Syntax::TokenType::Comment:
                return D2D1::ColorF(0.5f, 0.5f, 0.5f);
            case ::Orion::Syntax::TokenType::Number:
                return D2D1::ColorF(0.9f, 0.6f, 0.9f);
            default:
                return theme_.text;
            }
        }

        else if (ext == L".rs")
        {
            switch (type)
            {
            case ::Orion::Syntax::TokenType::Keyword:
                return D2D1::ColorF(0.95f, 0.6f, 0.25f);
            case ::Orion::Syntax::TokenType::Type:
                return D2D1::ColorF(0.3f, 0.85f, 0.9f);
            case ::Orion::Syntax::TokenType::String:
                return D2D1::ColorF(0.6f, 0.9f, 0.6f);
            case ::Orion::Syntax::TokenType::Comment:
                return D2D1::ColorF(0.45f, 0.45f, 0.45f);
            default:
                return theme_.text;
            }
        }

        else if (ext == L".html" || ext == L".htm")
        {
            switch (type)
            {
            // tag names -> green
            case ::Orion::Syntax::TokenType::Keyword:
                return D2D1::ColorF(0.36f, 0.8f, 0.45f);
            // attribute names -> cyan-ish
            case ::Orion::Syntax::TokenType::Type:
                return D2D1::ColorF(0.4f, 0.85f, 0.95f);
            // attribute values -> light green
            case ::Orion::Syntax::TokenType::String:
                return D2D1::ColorF(0.6f, 0.9f, 0.6f);
            case ::Orion::Syntax::TokenType::Comment:
                return D2D1::ColorF(0.5f, 0.5f, 0.5f);
            default:
                return theme_.text;
            }
        }

        // Tokenizer / Syntaxe highlighting pour C/C++ : garder les mêmes couleurs
        else if (ext == L".c" || ext == L".cpp" || ext == L".h" || ext == L".hpp")
        {
            // keep C/C++ style palette
            switch (type)
            {
            case ::Orion::Syntax::TokenType::Keyword:
                return D2D1::ColorF(0.86f, 0.58f, 0.22f);
            case ::Orion::Syntax::TokenType::Type:
                return D2D1::ColorF(0.4f, 0.8f, 1.0f);
            case ::Orion::Syntax::TokenType::String:
                return D2D1::ColorF(0.56f, 0.87f, 0.56f);
            case ::Orion::Syntax::TokenType::Comment:
                return D2D1::ColorF(0.5f, 0.5f, 0.5f);
            default:
                return theme_.text;
            }
        }

        // Default mapping (C/C++ style)
        switch (type)
        {
        case ::Orion::Syntax::TokenType::Keyword:
            return D2D1::ColorF(0.86f, 0.58f, 0.22f);
        case ::Orion::Syntax::TokenType::Type:
            return D2D1::ColorF(0.4f, 0.8f, 1.0f);
        case ::Orion::Syntax::TokenType::String:
            return D2D1::ColorF(0.56f, 0.87f, 0.56f);
        case ::Orion::Syntax::TokenType::Comment:
            return D2D1::ColorF(0.5f, 0.5f, 0.5f);
        case ::Orion::Syntax::TokenType::Number:
            return D2D1::ColorF(0.8f, 0.6f, 0.9f);
        case ::Orion::Syntax::TokenType::Preprocessor:
            return D2D1::ColorF(0.9f, 0.7f, 0.4f);
        case ::Orion::Syntax::TokenType::MarkdownHeading:
            return D2D1::ColorF(0.9f, 0.9f, 0.6f);
        case ::Orion::Syntax::TokenType::MarkdownCode:
            return D2D1::ColorF(0.8f, 0.8f, 0.85f);
        default:
            return theme_.text;
        }
    }

    void Editor::OnLeftButtonDown(HWND hwnd, POINT pt)
    {
        (void)hwnd;

        // Check scrollbar interaction first
        if (scrollbar_.OnLeftButtonDown(pt))
        {
            SetCapture(hwnd);
            return;
        }

        // Horizontal scrollbar hit test
        if (hScrollbarVisible_)
        {
            float hLeft = state_.leftEdge + metrics_.gutterWidth;
            float hRight = state_.rightEdge - (scrollbar_.IsVisible() ? 14.0f : 0.0f);
            float hTop = state_.bottomEdge - 14.0f;
            float hBottom = state_.bottomEdge;
            float thumbLeft = hLeft + hThumbPos_;
            float thumbRight = thumbLeft + hThumbWidth_;
            if (pt.x >= (int)thumbLeft && pt.x <= (int)thumbRight && pt.y >= (int)hTop && pt.y <= (int)hBottom)
            {
                hIsDragging_ = true;
                hDragStartX_ = pt.x;
                hDragStartOffset_ = state_.scrollOffsetX;
                SetCapture(hwnd);
                return;
            }
            if (pt.x >= (int)hLeft && pt.x <= (int)hRight && pt.y >= (int)hTop && pt.y <= (int)hBottom)
            {
                // Click on track -> page left/right
                if (pt.x < thumbLeft)
                    state_.scrollOffsetX = (std::max)(0.0f, state_.scrollOffsetX - hViewportWidth_ * 0.8f);
                else
                    state_.scrollOffsetX = (std::min)(hContentWidth_ - hViewportWidth_, state_.scrollOffsetX + hViewportWidth_ * 0.8f);
                // update thumb
                float maxScroll = hContentWidth_ - hViewportWidth_;
                float availableTrack = hViewportWidth_ - hThumbWidth_;
                if (maxScroll > 0.0f)
                    hThumbPos_ = (state_.scrollOffsetX / maxScroll) * availableTrack;
                else
                    hThumbPos_ = 0.0f;
                InvalidateRect(hwnd, nullptr, FALSE);
                return;
            }
        }

        // SearchBox handling
        if (searchBox_.IsVisible())
        {
            bool inside = searchBox_.IsPointInSearchBox(pt);
            {
            }
            if (inside)
            {
                searchBox_.OnLeftButtonDown(pt);
                return;
            }
        }

        if (searchBox_.IsVisible())
        {
            searchBox_.SetInputFocused(false);
        }

        // Completion popup
        if (completionPopup_ && completionPopup_->IsVisible())
        {
            if (completionPopup_->IsPointInPopup(pt))
            {
                completionPopup_->OnLeftButtonDown(pt);
                std::wstring chosen = completionPopup_->GetSelectedItem();
                if (!chosen.empty())
                {
                    std::wstring &line = state_.lines[state_.caret.line];
                    size_t pos = line.rfind(L"#include");
                    if (pos != std::wstring::npos)
                    {
                        size_t lt = line.find_last_of(L"<", state_.caret.column - 1);
                        size_t qt = line.find_last_of(L'"', state_.caret.column - 1);
                        size_t start = std::wstring::npos;
                        if (lt != std::wstring::npos && lt > pos)
                            start = lt + 1;
                        if (qt != std::wstring::npos && qt > pos)
                            start = (start == std::wstring::npos) ? qt + 1 : std::min(start, qt + 1);
                        if (start == std::wstring::npos)
                            start = state_.caret.column;
                        size_t caretPos = (size_t)state_.caret.column;
                        if (caretPos > start)
                            line.erase(start, caretPos - start);
                        line.insert(start, chosen);
                        state_.caret.column = (int)(start + chosen.size());
                    }
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return;
            }
            else
            {
                completionPopup_->Hide();
            }
        }

        // ✅ Vérifier qu'on clique dans la zone de l'éditeur
        if (pt.x < state_.leftEdge + metrics_.gutterWidth + metrics_.leftPadding)
        {
            return;
        }

        // ✅ PROTECTION: Vérifier que state_.lines n'est pas vide
        if (state_.lines.empty())
        {
            state_.caret = {0, 0};
            state_.hasSelection = false;
            return;
        }

        // Determine clicked text position first (more stable than raw screen coords)
        CaretPosition clickedPos = ScreenToTextPosition(pt);

        // ✅ SÉCURITÉ: Vérifier que la position est valide
        if (clickedPos.line < 0 || clickedPos.line >= (int)state_.lines.size())
        {
            clickedPos.line = (std::max)(0, (std::min)(clickedPos.line, (int)state_.lines.size() - 1));
        }
        if (clickedPos.column < 0)
        {
            clickedPos.column = 0;
        }
        if (clickedPos.line >= 0 && clickedPos.line < (int)state_.lines.size())
        {
            clickedPos.column = (std::min)(clickedPos.column, (int)state_.lines[clickedPos.line].size());
        }

        // Manage click count (single/double/triple click) using text position
        DWORD now = GetTickCount();
        UINT dblTime = GetDoubleClickTime();
        bool sameTextPos = (clickedPos.line == lastClickTextPos_.line && clickedPos.column == lastClickTextPos_.column);
        if (now - lastClickTime_ <= dblTime && sameTextPos)
        {
            clickCount_ = (clickCount_ < 3) ? clickCount_ + 1 : 1;
        }
        else
        {
            clickCount_ = 1;
        }
        lastClickTime_ = now;
        lastClickPos_ = pt;
        lastClickTextPos_ = clickedPos;

        // Shift+Click selection
        bool shiftPressed = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

        if (shiftPressed)
        {
            if (!state_.hasSelection)
            {
                state_.selectionStart = state_.caret;
            }
            state_.caret = clickedPos;
            state_.hasSelection = true;
            state_.caretVisible = true;
            state_.lastBlinkTime = GetTickCount();
            Orion::Caret::EnsureCaretVisible(state_, metrics_, scrollbar_);
            if (hwnd)
                InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }

        // Handle double-click (select word) and triple-click (select line)
        if (clickCount_ == 2 || clickCount_ == 3)
        {
            int line = clickedPos.line;
            if (line < 0 || line >= (int)state_.lines.size())
                return;

            const std::wstring &ln = state_.lines[line];

            if (clickCount_ == 3)
            {
                // Triple-click -> select entire line
                state_.selectionStart = {line, 0};
                state_.caret = {line, (int)ln.size()};
                state_.hasSelection = true;
                state_.caretVisible = true;
                state_.lastBlinkTime = GetTickCount();
                if (hwnd)
                    InvalidateRect(hwnd, nullptr, FALSE);
                return;
            }

            // Double-click -> select word under caret
            if (ln.empty())
            {
                state_.selectionStart = {line, 0};
                state_.caret = {line, 0};
                state_.hasSelection = true;
                state_.caretVisible = true;
                state_.lastBlinkTime = GetTickCount();
                if (hwnd)
                    InvalidateRect(hwnd, nullptr, FALSE);
                return;
            }

            auto isWordChar = [](wchar_t c)
            {
                return (iswalnum(c) != 0) || (c == L'_');
            };

            int col = clickedPos.column;
            if (col < 0)
                col = 0;
            if (col > (int)ln.size())
                col = (int)ln.size();

            int idx = col;
            if (idx == (int)ln.size())
                idx = (int)ln.size() - 1;

            // ✅ PROTECTION: Vérifier idx valide avant d'accéder
            if (idx < 0 || idx >= (int)ln.size())
            {
                state_.selectionStart = {line, 0};
                state_.caret = {line, (int)ln.size()};
                state_.hasSelection = true;
                state_.caretVisible = true;
                state_.lastBlinkTime = GetTickCount();
                Orion::Caret::EnsureCaretVisible(state_, metrics_, scrollbar_);
                if (hwnd)
                    InvalidateRect(hwnd, nullptr, FALSE);
                return;
            }

            // Si cliqué sur whitespace/punct, essayer voisin
            if (!isWordChar(ln[idx]))
            {
                if (idx > 0 && isWordChar(ln[idx - 1]))
                    idx = idx - 1;
            }

            // Expand to word boundaries
            int left = idx;
            while (left > 0 && isWordChar(ln[left - 1]))
                --left;
            int right = idx;
            while (right + 1 < (int)ln.size() && isWordChar(ln[right + 1]))
                ++right;

            // Si pas de mot trouvé, sélectionner le caractère
            if (left > right)
            {
                left = idx;
                right = idx;
            }

            state_.selectionStart = {line, left};
            state_.caret = {line, right + 1};
            state_.hasSelection = true;
            state_.caretVisible = true;
            state_.lastBlinkTime = GetTickCount();
            Orion::Caret::EnsureCaretVisible(state_, metrics_, scrollbar_);
            if (hwnd)
                InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }

        // Normal click: move caret and clear selection
        state_.caret = clickedPos;
        Orion::Caret::EnsureCaretVisible(state_, metrics_, scrollbar_);
        state_.hasSelection = false;
        state_.caretVisible = true;
        state_.lastBlinkTime = GetTickCount();
    }

    void Editor::OnMouseMove(HWND hwnd, POINT pt)
    {
        (void)hwnd;

        // If scrollbar handles mouse move (hover/drag), let it update scroll
        if (scrollbar_.OnMouseMove(pt))
        {
            state_.scrollOffsetY = scrollbar_.GetScrollOffset();
            // Scrollbar handled it - request window redraw
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }

        // If horizontal scrollbar is dragging, handle it
        if (hIsDragging_)
        {
            float hLeft = state_.leftEdge + metrics_.gutterWidth;
            float availableTrack = hViewportWidth_ - hThumbWidth_;
            int deltaX = pt.x - hDragStartX_;
            float maxScroll = hContentWidth_ - hViewportWidth_;
            if (availableTrack > 0.0f && maxScroll > 0.0f)
            {
                float scrollDelta = (deltaX / availableTrack) * maxScroll;
                state_.scrollOffsetX = hDragStartOffset_ + scrollDelta;
                if (state_.scrollOffsetX < 0.0f)
                    state_.scrollOffsetX = 0.0f;
                if (state_.scrollOffsetX > maxScroll)
                    state_.scrollOffsetX = maxScroll;
                hThumbPos_ = (state_.scrollOffsetX / maxScroll) * availableTrack;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return;
        }

        // Vérifier si on est dans la zone de l'éditeur
        // Utiliser la même référence que pour le rendu (inclut leftPadding)
        float contentLeft = state_.leftEdge + metrics_.gutterWidth + metrics_.leftPadding;
        bool isInEditorArea = !(pt.x < contentLeft || pt.x > state_.rightEdge || pt.y < state_.topEdge || pt.y > state_.bottomEdge);
        // If mouse is outside the editor AND no left-button drag is happening, ignore.
        if (!isInEditorArea && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000))
        {
            return;
        }

        if (GetAsyncKeyState(VK_LBUTTON) & 0x8000)
        {
            if (pt.x < state_.leftEdge + metrics_.gutterWidth + metrics_.leftPadding)
            {
                return;
            }

            if (!state_.hasSelection)
            {
                state_.selectionStart = state_.caret;
                state_.hasSelection = true;
            }

            bool needsRedraw = false;

            // Auto-scroll when dragging selection outside the editor viewport
            if (pt.y < (int)state_.topEdge + 2)
            {
                float over = ((float)state_.topEdge + 2.0f) - (float)pt.y;
                float speed = (std::max)(4.0f, over * 0.5f);
                scrollbar_.ScrollBy(-speed);
                state_.scrollOffsetY = scrollbar_.GetScrollOffset();
                needsRedraw = true;
            }
            else if (pt.y > (int)state_.bottomEdge - 2)
            {
                float over = (float)pt.y - ((float)state_.bottomEdge - 2.0f);
                float speed = (std::max)(4.0f, over * 0.5f);
                scrollbar_.ScrollBy(speed);
                state_.scrollOffsetY = scrollbar_.GetScrollOffset();
                needsRedraw = true;
            }

            // Recompute caret position after potential scroll
            CaretPosition newPos = ScreenToTextPosition(pt);
            // Clamp newPos to valid ranges to avoid races with rapid clicks
            if (newPos.line < 0)
                newPos.line = 0;
            if (newPos.line >= (int)state_.lines.size())
                newPos.line = (int)state_.lines.size() - 1;
            if (newPos.line >= 0 && newPos.line < (int)state_.lines.size())
            {
                int maxCol = (int)state_.lines[newPos.line].size();
                if (newPos.column < 0)
                    newPos.column = 0;
                if (newPos.column > maxCol)
                    newPos.column = maxCol;
            }

            if (newPos.line != state_.caret.line || newPos.column != state_.caret.column)
            {
                state_.caret = newPos;
                state_.caretVisible = true;
                state_.lastBlinkTime = GetTickCount();
                needsRedraw = true;
            }

            // Seulement invalider si quelque chose a changé
            if (needsRedraw)
            {
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }

        // Forward mouse to completion popup if visible
        if (completionPopup_ && completionPopup_->IsVisible())
        {
            completionPopup_->OnMouseMove(pt);
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
    }

    void Editor::CancelInteraction()
    {
        state_.hasSelection = false;
        // reset caret blink to visible state
        state_.caretVisible = true;
        state_.lastBlinkTime = GetTickCount();
        // cancel scrollbar drag
        if (scrollbar_.IsDragging())
        {
            scrollbar_.OnLeftButtonUp();
            ReleaseCapture();
        }
    }

    // ------------------------------------------------------------
    // Editor::TextToScreenPosition / ScreenToTextPosition
    // ------------------------------------------------------------
    D2D1_POINT_2F Orion::Editor::TextToScreenPosition(CaretPosition pos)
    {
        // base X du texte (après gutter + padding) + scroll
        const float contentLeft = state_.leftEdge + metrics_.gutterWidth + metrics_.leftPadding;
        const float baseX = contentLeft - state_.scrollOffsetX;

        // clamp line
        if (state_.lines.empty())
            return D2D1::Point2F(baseX, state_.topEdge - state_.scrollOffsetY);

        int line = pos.line;
        if (line < 0)
            line = 0;
        if (line >= (int)state_.lines.size())
            line = (int)state_.lines.size() - 1;

        // y
        const float y = state_.topEdge + (line * metrics_.lineHeight) - state_.scrollOffsetY;

        // clamp column
        const std::wstring &ln = state_.lines[line];
        int col = pos.column;
        if (col < 0)
            col = 0;
        if (col > (int)ln.size())
            col = (int)ln.size();

        // logique -> visuel (tabs)
        int visualCol = 0;
        const int tabSize = 4; // tu utilises 4 un peu partout
        for (int i = 0; i < col; ++i)
            visualCol = Orion::Geometry::AdvanceVisualCol(visualCol, ln[i], tabSize);

        const float x = baseX + (visualCol * metrics_.characterWidth);
        return D2D1::Point2F(x, y);
    }

    Orion::CaretPosition Orion::Editor::ScreenToTextPosition(POINT screenPoint)
    {
        CaretPosition out{0, 0};

        if (state_.lines.empty())
            return out;

        const float contentLeft = state_.leftEdge + metrics_.gutterWidth + metrics_.leftPadding;
        const float baseX = contentLeft - state_.scrollOffsetX;

        // --- line ---
        const float adjustedY = (float)screenPoint.y + state_.scrollOffsetY;
        int line = (int)((adjustedY - state_.topEdge) / metrics_.lineHeight);
        line = (std::max)(0, (std::min)(line, (int)state_.lines.size() - 1));

        const std::wstring &ln = state_.lines[line];

        // --- target visual col ---
        float localX = (float)screenPoint.x - baseX;
        if (localX < 0.0f)
            localX = 0.0f;

        int targetVisual = (int)std::floor((localX / metrics_.characterWidth) + 0.5f);
        if (targetVisual < 0)
            targetVisual = 0;

        // visuel -> logique (tabs)
        const int tabSize = 4;

        int visual = 0;
        int col = 0;
        for (int i = 0; i < (int)ln.size(); ++i)
        {
            int nextVisual = Orion::Geometry::AdvanceVisualCol(visual, ln[i], tabSize);

            // “caret style”: au milieu d’un tab, choisir début/fin selon moitié
            int mid = (visual + nextVisual) / 2;
            if (targetVisual < mid)
            {
                col = i;
                break;
            }

            visual = nextVisual;
            col = i + 1;
        }

        // clamp
        if (col < 0)
            col = 0;
        if (col > (int)ln.size())
            col = (int)ln.size();

        out.line = line;
        out.column = col;
        return out;
    }

    void Editor::DeleteSelection()
    {
        if (!state_.hasSelection)
            return;

        // Push undo snapshot before modifying text
        if (undoStack_.empty() || undoStack_.back().lines != state_.lines ||
            undoStack_.back().caret.line != state_.caret.line ||
            undoStack_.back().caret.column != state_.caret.column)
        {
            undoStack_.push_back(state_);
            if (undoStack_.size() > maxUndoEntries_)
                undoStack_.erase(undoStack_.begin());
        }

        CaretPosition start = state_.selectionStart;
        CaretPosition end = state_.caret;

        if (start.line > end.line || (start.line == end.line && start.column > end.column))
        {
            std::swap(start, end);
        }

        // Cas simple : même ligne
        if (start.line == end.line)
        {
            state_.lines[start.line].erase(start.column, end.column - start.column);
        }
        else
        {
            // Build new merged line: prefix of first line + suffix of last line
            std::wstring prefix = state_.lines[start.line].substr(0, start.column);
            std::wstring suffix;
            if (end.column < (int)state_.lines[end.line].size())
                suffix = state_.lines[end.line].substr(end.column);

            state_.lines[start.line] = prefix + suffix;

            // Erase all lines between start.line+1 and end.line inclusive
            if (end.line > start.line)
            {
                state_.lines.erase(state_.lines.begin() + start.line + 1,
                                   state_.lines.begin() + end.line + 1);
            }
        }

        state_.caret = start;
        state_.hasSelection = false;
    }

    bool Editor::HasNonEmptyContent() const
    {
        for (const auto &ln : state_.lines)
        {
            if (!ln.empty())
                return true;
        }
        return false;
    }

    void Editor::Undo()
    {
        if (undoStack_.empty())
            return;
        EditorState prev = undoStack_.back();
        undoStack_.pop_back();
        state_ = prev;
        state_.caretVisible = true;
        state_.lastBlinkTime = GetTickCount();
    }

    void Editor::SelectAll()
    {
        if (state_.lines.empty())
            return;
        state_.selectionStart = {0, 0};
        int lastLine = (int)state_.lines.size() - 1;
        int lastCol = (int)state_.lines[lastLine].size();
        state_.caret.line = lastLine;
        state_.caret.column = lastCol;
        state_.hasSelection = true;
        Orion::Caret::EnsureCaretVisible(state_, metrics_, scrollbar_);
    }

    void Editor::DeleteSelectionPublic()
    {
        DeleteSelection();
    }

    void Editor::OnLeftButtonUp(HWND hwnd, POINT pt)
    {
        (void)hwnd;
        (void)pt;
        // If scrollbar was handling a drag, release it
        if (scrollbar_.OnLeftButtonUp())
        {
            ReleaseCapture();
            return;
        }
        if (hIsDragging_)
        {
            hIsDragging_ = false;
            ReleaseCapture();
            return;
        }
        // Release completion popup thumb drag if any
        if (completionPopup_ && completionPopup_->IsVisible())
        {
            completionPopup_->OnLeftButtonUp();
        }
    }

    void Editor::OnMouseWheel(HWND hwnd, int delta, bool ctrlPressed)
    {
        (void)hwnd;
        // If Shift is pressed, scroll horizontally
        bool shiftPressed = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        if (shiftPressed && hScrollbarVisible_)
        {
            // Natural wheel mapping: positive delta -> scroll left/up interpretation
            float scrollAmount = (delta / 120.0f) * 40.0f; // same base speed as vertical
            state_.scrollOffsetX += scrollAmount;
            float maxScroll = hContentWidth_ - hViewportWidth_;
            if (state_.scrollOffsetX < 0.0f)
                state_.scrollOffsetX = 0.0f;
            if (state_.scrollOffsetX > maxScroll)
                state_.scrollOffsetX = maxScroll;
            float availableTrack = hViewportWidth_ - hThumbWidth_;
            if (maxScroll > 0.0f)
                hThumbPos_ = (state_.scrollOffsetX / maxScroll) * availableTrack;
            // Request redraw so the thumb and text positions update immediately
            if (hwnd)
                InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }

        (void)ctrlPressed; // Ctrl-based zoom removed

        // If completion popup is visible and the cursor is over it, scroll the popup
        if (completionPopup_ && completionPopup_->IsVisible())
        {
            POINT cur;
            GetCursorPos(&cur);
            ScreenToClient(hwnd, &cur);
            if (completionPopup_->IsPointInPopup(cur))
            {
                if (completionPopup_->OnMouseWheel(delta))
                {
                    if (hwnd)
                        InvalidateRect(hwnd, nullptr, FALSE);
                    return;
                }
            }
        }

        if (scrollbar_.OnMouseWheel(delta))
        {
            state_.scrollOffsetY = scrollbar_.GetScrollOffset();
            if (hwnd)
                InvalidateRect(hwnd, nullptr, FALSE);
        }
    }

    void Editor::OnHorizontalWheel(HWND hwnd, int delta)
    {
        if (!hScrollbarVisible_)
            return;

        float scrollAmount = (delta / 120.0f) * 40.0f;
        state_.scrollOffsetX += scrollAmount;
        float maxScroll = hContentWidth_ - hViewportWidth_;
        if (state_.scrollOffsetX < 0.0f)
            state_.scrollOffsetX = 0.0f;
        if (state_.scrollOffsetX > maxScroll)
            state_.scrollOffsetX = maxScroll;
        float availableTrack = hViewportWidth_ - hThumbWidth_;
        if (maxScroll > 0.0f)
            hThumbPos_ = (state_.scrollOffsetX / maxScroll) * availableTrack;
        if (hwnd)
            InvalidateRect(hwnd, nullptr, FALSE);
    }

} // namespace Orion