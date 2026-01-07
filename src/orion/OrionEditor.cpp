#include "OrionEditor.h"
#include "../helpers/window_helpers.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include "syntax/Highlighter.h"
#include "CompletionPopup.h"
#include <dwrite_1.h>
#include <filesystem>

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

    Editor::Editor()
    {
        state_.lastBlinkTime = GetTickCount();
        highlighter_ = new ::Orion::Syntax::Highlighter();
        cachedTextFormat_ = nullptr;
        completionPopup_ = new CompletionPopup();
        BuildHeaderIndex();
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

        std::ifstream file;
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, filePath.c_str(), (int)filePath.size(), NULL, 0, NULL, NULL);
        std::string pathUtf8(size_needed, '\0');
        WideCharToMultiByte(CP_UTF8, 0, filePath.c_str(), (int)filePath.size(), pathUtf8.data(), size_needed, NULL, NULL);

        file.open(pathUtf8, std::ios::binary);
        if (!file.is_open())
        {
            state_.lines.push_back(L"// Could not open file");
            return;
        }

        std::string line;
        while (std::getline(file, line))
        {
            // SUPPRIMER LE \r SI PRÉSENT (Windows line endings)
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }

            if (line.empty())
            {
                state_.lines.push_back(L"");
            }
            else
            {
                int wsize = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), (int)line.size(), NULL, 0);
                std::wstring wline(wsize, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, line.c_str(), (int)line.size(), wline.data(), wsize);
                state_.lines.push_back(wline);
            }
        }

        if (state_.lines.empty())
        {
            state_.lines.push_back(L"");
        }

        file.close();
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

    void Editor::Draw(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd)
    {
        if (!ctx || !dwrite)
            return;
        if (state_.lines.empty())
            return;

        (void)hwnd;

        pDWriteFactory_ = dwrite;

        // Ensure ClearType text rendering and preserve previous settings
        D2D1_ANTIALIAS_MODE oldAA = ctx->GetAntialiasMode();
        D2D1_TEXT_ANTIALIAS_MODE oldTextAA = ctx->GetTextAntialiasMode();
        ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        ctx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

        UpdateCaretBlink();

        if (!fontMetricsInitialized_)
        {
            const wchar_t *editorFont = L"JetBrains Mono";
            const float editorFontSize = 14.0f * zoomLevel_; // unified editor font size (zoomed)
            {
                std::wstringstream ss;
                ss << L"Editor font requested: " << editorFont << L" size=" << editorFontSize;
                Logger::Instance().Log(ss.str());
            }
            IDWriteTextFormat *tmpFormat = nullptr;
            if (SUCCEEDED(dwrite->CreateTextFormat(editorFont, customFontCollection_,
                                                   DWRITE_FONT_WEIGHT_REGULAR,
                                                   DWRITE_FONT_STYLE_NORMAL,
                                                   DWRITE_FONT_STRETCH_NORMAL,
                                                   editorFontSize, L"en-us", &tmpFormat)) &&
                tmpFormat)
            {
                IDWriteTextLayout *tmpLayout = nullptr;
                const wchar_t sample[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
                const UINT32 sampleLen = (UINT32)wcslen(sample);
                if (SUCCEEDED(dwrite->CreateTextLayout(sample, sampleLen, tmpFormat, 2000.0f, 200.0f, &tmpLayout)) && tmpLayout)
                {
                    DWRITE_TEXT_METRICS tm = {};
                    tmpLayout->GetMetrics(&tm);
                    metrics_.characterWidth = (sampleLen > 0) ? (tm.width / (float)sampleLen) : 8.0f;
                    metrics_.leftPadding = 0.0f;

                    // Toujours récupérer les métriques de police - utiliser la collection custom si disponible
                    IDWriteFontCollection *fontCollection = customFontCollection_ ? customFontCollection_ : nullptr;
                    if (!fontCollection)
                    {
                        dwrite->GetSystemFontCollection(&fontCollection, FALSE);
                    }

                    if (fontCollection)
                    {
                        UINT32 index = 0;
                        BOOL exists = FALSE;
                        if (SUCCEEDED(fontCollection->FindFamilyName(editorFont, &index, &exists)))
                        {
                            std::wstringstream ss;
                            ss << L"Font family lookup for '" << editorFont << L"' exists=" << (exists ? L"true" : L"false");
                            Logger::Instance().Log(ss.str());

                            if (!exists)
                            {
                                Logger::Instance().Log(L"ATTENTION: Police 'JetBrains Mono' introuvable dans la collection !");
                                Logger::Instance().Log(L"Veuillez installer la police ou le rendu utilisera une police par défaut");
                            }
                        }
                        if (exists)
                        {
                            IDWriteFontFamily *family = nullptr;
                            if (SUCCEEDED(fontCollection->GetFontFamily(index, &family)) && family)
                            {
                                IDWriteFont *font = nullptr;
                                if (SUCCEEDED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font)) && font)
                                {
                                    IDWriteFontFace *fontFace = nullptr;
                                    if (SUCCEEDED(font->CreateFontFace(&fontFace)) && fontFace)
                                    {
                                        DWRITE_FONT_METRICS fm = {};
                                        fontFace->GetMetrics(&fm);
                                        float designUnitToDip = editorFontSize / (float)fm.designUnitsPerEm;
                                        fontAscent_ = fm.ascent * designUnitToDip;
                                        fontDescent_ = fm.descent * designUnitToDip;

                                        std::wstringstream ss2;
                                        ss2 << L"Font metrics for '" << editorFont << L"' : ascent=" << fontAscent_ << L" descent=" << fontDescent_ << L" designUnitsPerEm=" << fm.designUnitsPerEm;
                                        Logger::Instance().Log(ss2.str());

                                        // Line height SERRÉ comme VSCode
                                        metrics_.lineHeight = fontAscent_ + fontDescent_ + 5.0f;

                                        fontFace->Release();
                                    }
                                    font->Release();
                                }
                                family->Release();
                            }
                        }

                        // Ne release que si c'était la collection système
                        if (fontCollection != customFontCollection_)
                        {
                            fontCollection->Release();
                        }
                    }

                    // Si pas de métriques récupérées, logger l'erreur
                    if (fontAscent_ <= 0.0f || fontDescent_ <= 0.0f)
                    {
                        Logger::Instance().Log(L"ERREUR: Impossible de récupérer les métriques de la police 'JetBrains Mono'");
                        Logger::Instance().Log(L"Utilisation des valeurs par défaut");

                        fontAscent_ = 12.0f;
                        fontDescent_ = 4.0f;
                    }

                    // Use computed font metrics to set a matching line height (consistent)
                    metrics_.lineHeight = fontAscent_ + fontDescent_ + 5.0f; // unified padding
                    tmpLayout->Release();
                }
                tmpFormat->Release();
            }
            fontMetricsInitialized_ = true;
        }

        if (!cachedTextFormat_ && pDWriteFactory_)
        {
            const wchar_t *editorFont = L"JetBrains Mono";
            const float editorFontSize = 14.0f * zoomLevel_;

            std::wstringstream ss;
            ss << L"Creating persistent text format: " << editorFont << L" size=" << editorFontSize;
            Logger::Instance().Log(ss.str());

            // Use a slightly heavier weight for the cached format to improve perceived weight
            pDWriteFactory_->CreateTextFormat(
                editorFont, customFontCollection_,
                DWRITE_FONT_WEIGHT_REGULAR,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                editorFontSize, L"en-us",
                &cachedTextFormat_);

            if (cachedTextFormat_)
            {
                cachedTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                cachedTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                cachedTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                Logger::Instance().Log(L"Persistent editor text format created successfully.");
            }
            else
            {
                Logger::Instance().Log(L"Failed to create persistent editor text format.");
            }
        }

        // Entire editor clip (background + gutter). Content (text, selection, caret)
        // will be clipped separately so it cannot draw over the gutter.
        D2D1_RECT_F editorClip = D2D1::RectF(
            state_.leftEdge,
            state_.topEdge,
            state_.rightEdge,
            state_.bottomEdge);

        ctx->PushAxisAlignedClip(editorClip, D2D1_ANTIALIAS_MODE_ALIASED);

        // Fill editor background so gutter and content share the same color
        {
            ID2D1SolidColorBrush *bg = nullptr;
            ctx->CreateSolidColorBrush(theme_.background, &bg);
            if (bg)
            {
                ctx->FillRectangle(editorClip, bg);
                bg->Release();
            }
        }

        // Draw gutter first (outside content clip)
        DrawGutter(ctx, dwrite);
        DrawLineNumbers(ctx, dwrite);

        // Content clip (exclude gutter and reserve space for vertical/horizontal scrollbars)
        float contentLeft = state_.leftEdge + metrics_.gutterWidth;
        float contentRight = state_.rightEdge - (scrollbar_.IsVisible() ? 14.0f : 0.0f);
        float contentBottom = state_.bottomEdge - (hScrollbarVisible_ ? 14.0f : 0.0f);
        D2D1_RECT_F contentClip = D2D1::RectF(contentLeft, state_.topEdge, contentRight, contentBottom);
        ctx->PushAxisAlignedClip(contentClip, D2D1_ANTIALIAS_MODE_ALIASED);

        // Draw content elements inside contentClip so they never overlap gutter
        DrawActiveLine(ctx);
        DrawSelection(ctx); // Dessiner la sélection AVANT le texte
        DrawTextContent(ctx, dwrite);
        DrawSearchMatches(ctx);
        DrawCaret(ctx);

        // Draw completion popup if visible
        if (completionPopup_ && completionPopup_->IsVisible())
        {
            completionPopup_->Draw(ctx, dwrite);
        }

        // Pop content clip
        ctx->PopAxisAlignedClip();

        // Draw vertical scrollbar on top
        scrollbar_.Draw(ctx);

        // Draw horizontal scrollbar if needed
        if (hScrollbarVisible_)
            {
            // Use the same visual language as the vertical Scrollbar component
            float hLeft = state_.leftEdge + metrics_.gutterWidth;
            float hRight = state_.rightEdge - (scrollbar_.IsVisible() ? 14.0f : 0.0f);
            float hTop = state_.bottomEdge - 14.0f;
            float hBottom = state_.bottomEdge;

            // Track is subtle (keep a dark track to match theme)
            ID2D1SolidColorBrush *trackBrush = nullptr;
            ctx->CreateSolidColorBrush(D2D1::ColorF(0x1f1f20, 1.0f), &trackBrush);
            if (trackBrush)
            {
                ctx->FillRectangle(D2D1::RectF(hLeft, hTop, hRight, hBottom), trackBrush);
                trackBrush->Release();
            }

            // Thumb color follows drag/hover state; horizontal uses hIsDragging_ as drag state
            D2D1_COLOR_F thumbColor;
            if (hIsDragging_)
                thumbColor = D2D1::ColorF(0.45f, 0.45f, 0.45f, 0.9f);
            else
                thumbColor = D2D1::ColorF(0.25f, 0.25f, 0.25f, 0.4f);

            ID2D1SolidColorBrush *thumbBrush = nullptr;
            ctx->CreateSolidColorBrush(thumbColor, &thumbBrush);

            float thumbLeft = hLeft + hThumbPos_;
            float thumbRight = thumbLeft + hThumbWidth_;
            // Use smaller horizontal padding and rounded corners similar to vertical thumb
            D2D1_ROUNDED_RECT thumbRect = D2D1::RoundedRect(
                D2D1::RectF(thumbLeft + 4.0f, hTop + 2.0f, thumbRight - 4.0f, hBottom - 2.0f),
                3.0f, 3.0f);

            if (thumbBrush)
            {
                ctx->FillRoundedRectangle(thumbRect, thumbBrush);
                thumbBrush->Release();
            }
            }

        // Pop the full editor clip
        ctx->PopAxisAlignedClip();

        // ✨ NOUVEAU : Dessiner le SearchBox par-dessus tout
        if (searchBox_.IsVisible())
        {
            searchBox_.Draw(ctx, dwrite);
        }

        // restore previous antialiasing settings
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

        const auto& matches = searchBox_.GetMatches();
        if (matches.empty())
            return;

        int currentMatchIdx = searchBox_.GetCurrentMatchIndex();
        
        // Brushes pour les matches
        ID2D1SolidColorBrush* matchBrush = nullptr;
        ID2D1SolidColorBrush* currentMatchBrush = nullptr;
        ctx->CreateSolidColorBrush(D2D1::ColorF(0.8f, 0.6f, 0.0f, 0.4f), &matchBrush);
        ctx->CreateSolidColorBrush(D2D1::ColorF(0.9f, 0.4f, 0.0f, 0.6f), &currentMatchBrush);

        float contentLeft = state_.leftEdge + metrics_.gutterWidth + metrics_.leftPadding;

        for (size_t i = 0; i < matches.size(); ++i)
        {
            const auto& match = matches[i];
            
            // Skip if line is not visible
            float lineY = state_.topEdge + (match.line * metrics_.lineHeight) - state_.scrollOffsetY;
            if (lineY + metrics_.lineHeight < state_.topEdge || lineY > state_.bottomEdge)
                continue;

            // Calculate match position using DirectWrite for accuracy
            float startX = contentLeft - state_.scrollOffsetX;
            float endX = contentLeft - state_.scrollOffsetX;

            if (pDWriteFactory_ && match.line >= 0 && match.line < (int)state_.lines.size())
            {
                const std::wstring& line = state_.lines[match.line];
                
                IDWriteTextFormat* format = cachedTextFormat_;
                IDWriteTextFormat* tmpFmt = nullptr;
                if (!format)
                {
                    pDWriteFactory_->CreateTextFormat(
                        L"JetBrains Mono", customFontCollection_,
                        DWRITE_FONT_WEIGHT_NORMAL,
                        DWRITE_FONT_STYLE_NORMAL,
                        DWRITE_FONT_STRETCH_NORMAL,
                        14.0f * zoomLevel_, L"en-us",
                        &tmpFmt);
                    if (tmpFmt)
                        format = tmpFmt;
                }

                if (format)
                {
                    // Measure text before match start
                    if (match.startColumn > 0)
                    {
                        std::wstring textBefore = line.substr(0, match.startColumn);
                        IDWriteTextLayout* layout1 = nullptr;
                        if (SUCCEEDED(pDWriteFactory_->CreateTextLayout(
                                textBefore.c_str(),
                                (UINT32)textBefore.size(),
                                format,
                                10000.0f,
                                metrics_.lineHeight,
                                &layout1)) && layout1)
                        {
                            DWRITE_TEXT_METRICS tm1 = {};
                            layout1->GetMetrics(&tm1);
                            startX = contentLeft + tm1.width - state_.scrollOffsetX;
                            layout1->Release();
                        }
                    }

                    // Measure text up to match end
                    if (match.endColumn > 0 && match.endColumn <= (int)line.size())
                    {
                        std::wstring textBeforeEnd = line.substr(0, match.endColumn);
                        IDWriteTextLayout* layout2 = nullptr;
                        if (SUCCEEDED(pDWriteFactory_->CreateTextLayout(
                                textBeforeEnd.c_str(),
                                (UINT32)textBeforeEnd.size(),
                                format,
                                10000.0f,
                                metrics_.lineHeight,
                                &layout2)) && layout2)
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

            // Draw match highlight
            D2D1_RECT_F matchRect = D2D1::RectF(startX, lineY, endX, lineY + metrics_.lineHeight);
            
            bool isCurrent = (currentMatchIdx >= 0 && (int)i == currentMatchIdx);
            ID2D1SolidColorBrush* brush = isCurrent ? currentMatchBrush : matchBrush;
            
            if (brush)
            {
                ctx->FillRectangle(matchRect, brush);
            }

            // Draw border for current match
            if (isCurrent)
            {
                ID2D1SolidColorBrush* borderBrush = nullptr;
                ctx->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.5f, 0.0f), &borderBrush);
                if (borderBrush)
                {
                    ctx->DrawRectangle(matchRect, borderBrush, 1.5f);
                    borderBrush->Release();
                }
            }
        }

        if (matchBrush) matchBrush->Release();
        if (currentMatchBrush) currentMatchBrush->Release();
    }

    bool Editor::LoadCustomFont(IDWriteFactory *dwrite, const std::wstring &fontPath)
    {
        // Essayer de caster vers IDWriteFactory1 (disponible depuis Windows 7 SP1)
        IDWriteFactory1 *factory1 = nullptr;
        HRESULT hr = dwrite->QueryInterface(__uuidof(IDWriteFactory1), (void **)&factory1);

        if (FAILED(hr) || !factory1)
        {
            Logger::Instance().Log(L"❌ IDWriteFactory1 non disponible (besoin de Windows 7 SP1+)");
            return false;
        }

        

        // Create and register a font collection loader which will enumerate font files
        fontLoader_ = new CustomFontCollectionLoader();

        HRESULT regHr = dwrite->RegisterFontCollectionLoader(fontLoader_);
        if (FAILED(regHr))
        {
            Logger::Instance().Log(L"❌ Échec de l'enregistrement du font collection loader");
            fontLoader_->Release();
            fontLoader_ = nullptr;
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
            Logger::Instance().Log(L"❌ Échec de CreateCustomFontCollection");
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

        if (SUCCEEDED(hr) && exists)
        {
            Logger::Instance().Log(L"✅ 'JetBrains Mono' trouvée dans la collection custom !");
            return true;
        }
        else
        {
            Logger::Instance().Log(L"⚠️ Police chargée mais famille 'JetBrains Mono' introuvable");
            // La collection est quand même créée, on peut essayer de l'utiliser
            return true;
        }
    }

    void Editor::DrawActiveLine(ID2D1RenderTarget *ctx)
    {
        ID2D1SolidColorBrush *brush = nullptr;
        ctx->CreateSolidColorBrush(theme_.activeLineBackground, &brush);

        float contentLeft = state_.leftEdge + metrics_.gutterWidth + metrics_.leftPadding;
        float lineY = state_.topEdge +
                      (state_.caret.line * metrics_.lineHeight) - state_.scrollOffsetY;

        // Draw active line background only for content area (not gutter)
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

        ID2D1SolidColorBrush *brush = nullptr;
        ctx->CreateSolidColorBrush(theme_.selection, &brush);

        CaretPosition start = state_.selectionStart;
        CaretPosition end = state_.caret;

        if (start.line > end.line || (start.line == end.line && start.column > end.column))
        {
            std::swap(start, end);
        }

        float contentLeft = state_.leftEdge + metrics_.gutterWidth + metrics_.leftPadding;

        for (int line = start.line; line <= end.line; ++line)
        {
            if (line < 0 || line >= (int)state_.lines.size())
                continue;

            int startCol = (line == start.line) ? start.column : 0;
            int endCol = (line == end.line) ? end.column : (int)state_.lines[line].size();

            float lineY = state_.topEdge + (line * metrics_.lineHeight) - state_.scrollOffsetY;

            float x1 = contentLeft - state_.scrollOffsetX;
            float x2 = contentLeft - state_.scrollOffsetX;

            bool isEmptyLine = state_.lines[line].empty();

            // Detect lines that contain only whitespace (e.g. auto-inserted indent)
            bool onlyWhitespaceLine = true;
            for (wchar_t wc : state_.lines[line])
            {
                if (!iswspace(wc))
                {
                    onlyWhitespaceLine = false;
                    break;
                }
            }

            // Si ligne vide OU si c'est une sélection multi-ligne qui passe par cette ligne
            if (isEmptyLine && (start.line != end.line || startCol != endCol))
            {
                // Pour une ligne totalement vide, montrer une largeur minimale raisonnable :
                // mesurer la largeur d'un espace via DirectWrite si possible, sinon fallback.
                float spaceW = metrics_.characterWidth;
                if (pDWriteFactory_)
                {
                    IDWriteTextFormat *fmt = cachedTextFormat_;
                    IDWriteTextFormat *tmpFmt = nullptr;
                    if (!fmt)
                    {
                        if (SUCCEEDED(pDWriteFactory_->CreateTextFormat(
                            L"JetBrains Mono",
                            customFontCollection_,
                            DWRITE_FONT_WEIGHT_REGULAR,
                            DWRITE_FONT_STYLE_NORMAL,
                            DWRITE_FONT_STRETCH_NORMAL,
                            14.0f * zoomLevel_, L"en-us", &tmpFmt)) && tmpFmt)
                        {
                            fmt = tmpFmt;
                        }
                    }

                    if (fmt)
                    {
                        IDWriteTextLayout *tl = nullptr;
                        if (SUCCEEDED(pDWriteFactory_->CreateTextLayout(L" ", 1, fmt, 1000.0f, metrics_.lineHeight, &tl)) && tl)
                        {
                            DWRITE_TEXT_METRICS tm = {};
                            tl->GetMetrics(&tm);
                            if (tm.width > 0.0f)
                                spaceW = tm.width;
                            tl->Release();
                        }
                    }

                    if (tmpFmt)
                        tmpFmt->Release();
                }

                x2 = x1 + spaceW;
            }
            else if (onlyWhitespaceLine)
            {
                // Use a measured space width to compute selection rect for whitespace-only lines
                float spaceW = metrics_.characterWidth;
                if (pDWriteFactory_)
                {
                    IDWriteTextFormat *fmt = cachedTextFormat_;
                    IDWriteTextFormat *tmpFmt = nullptr;
                    if (!fmt)
                    {
                        if (SUCCEEDED(pDWriteFactory_->CreateTextFormat(
                            L"JetBrains Mono",
                            customFontCollection_,
                            DWRITE_FONT_WEIGHT_REGULAR,
                            DWRITE_FONT_STYLE_NORMAL,
                            DWRITE_FONT_STRETCH_NORMAL,
                            14.0f * zoomLevel_, L"en-us", &tmpFmt)) && tmpFmt)
                        {
                            fmt = tmpFmt;
                        }
                    }

                    if (fmt)
                    {
                        IDWriteTextLayout *tl = nullptr;
                        if (SUCCEEDED(pDWriteFactory_->CreateTextLayout(L" ", 1, fmt, 1000.0f, metrics_.lineHeight, &tl)) && tl)
                        {
                            DWRITE_TEXT_METRICS tm = {};
                            tl->GetMetrics(&tm);
                            if (tm.width > 0.0f)
                                spaceW = tm.width;
                            tl->Release();
                        }
                    }

                    if (tmpFmt)
                        tmpFmt->Release();
                }

                x1 = contentLeft + (startCol * spaceW) - state_.scrollOffsetX;
                x2 = contentLeft + (endCol * spaceW) - state_.scrollOffsetX;
            }
            else if (startCol == endCol && start.line == end.line)
            {
                // Pas de sélection visible si même position
                continue;
            }
            else if (pDWriteFactory_ && !isEmptyLine)
            {
                IDWriteTextFormat *format = cachedTextFormat_;
                IDWriteTextFormat *tmpFormat = nullptr;
                if (!format && pDWriteFactory_)
                {
                    pDWriteFactory_->CreateTextFormat(
                        L"JetBrains Mono", customFontCollection_,
                                DWRITE_FONT_WEIGHT_REGULAR,
                                DWRITE_FONT_STYLE_NORMAL,
                                DWRITE_FONT_STRETCH_NORMAL,
                                14.0f * zoomLevel_, L"en-us",
                                &tmpFormat);
                    if (tmpFormat)
                        format = tmpFormat;
                }

                if (format)
                {
                    // Mesurer la position de début
                    if (startCol > 0)
                    {
                        std::wstring textBefore = state_.lines[line].substr(0, startCol);
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
                            x1 = contentLeft + tm1.width - state_.scrollOffsetX;
                            layout1->Release();
                        }
                    }

                    // Mesurer la position de fin
                    if (endCol > 0 && endCol <= (int)state_.lines[line].size())
                    {
                        std::wstring textBeforeEnd = state_.lines[line].substr(0, endCol);
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
                            x2 = contentLeft + tm2.width - state_.scrollOffsetX;
                            layout2->Release();
                        }
                    }

                    if (tmpFormat)
                        tmpFormat->Release();
                }
            }
            else if (!isEmptyLine)
            {
                // Fallback pour lignes non-vides sans DirectWrite
                x1 = contentLeft + (startCol * metrics_.characterWidth) - state_.scrollOffsetX;
                x2 = contentLeft + (endCol * metrics_.characterWidth) - state_.scrollOffsetX;
            }

            D2D1_RECT_F rect = D2D1::RectF(x1, lineY, x2, lineY + metrics_.lineHeight);
            ctx->FillRectangle(rect, brush);
        }

        if (brush)
            brush->Release();
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

    void Editor::DrawGutter(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite)
    {
        (void)dwrite;

        ID2D1SolidColorBrush *bgBrush = nullptr;
        ctx->CreateSolidColorBrush(theme_.background, &bgBrush);

        D2D1_RECT_F gutterRect = D2D1::RectF(
            state_.leftEdge,
            state_.topEdge,
            state_.leftEdge + metrics_.gutterWidth,
            state_.bottomEdge);

        ctx->FillRectangle(gutterRect, bgBrush);

        if (bgBrush)
            bgBrush->Release();
    }

    void Editor::DrawLineNumbers(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite)
    {
        const wchar_t *editorFont = L"JetBrains Mono";
        const float editorFontSize = 14.0f * zoomLevel_; // unified editor font size
        IDWriteTextFormat *format = nullptr;
        dwrite->CreateTextFormat(editorFont, customFontCollection_,
                                 DWRITE_FONT_WEIGHT_NORMAL,
                                 DWRITE_FONT_STYLE_NORMAL,
                                 DWRITE_FONT_STRETCH_NORMAL,
                                 editorFontSize, L"en-us",
                                 &format);
        if (format)
        {
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        ID2D1SolidColorBrush *textBrush = nullptr;
        ID2D1SolidColorBrush *activeTextBrush = nullptr;
        ctx->CreateSolidColorBrush(theme_.lineNumberText, &textBrush);
        ctx->CreateSolidColorBrush(theme_.text, &activeTextBrush); // brighter color for active line number

        int firstVisibleLine = (int)(state_.scrollOffsetY / metrics_.lineHeight);
        int lastVisibleLine = (int)((state_.scrollOffsetY + (state_.bottomEdge - state_.topEdge)) / metrics_.lineHeight) + 1;

        firstVisibleLine = (std::max)(0, firstVisibleLine);
        lastVisibleLine = (std::min)((int)state_.lines.size(), lastVisibleLine);

        for (int i = firstVisibleLine; i < lastVisibleLine; ++i)
        {
            float lineY = state_.topEdge +
                          (i * metrics_.lineHeight) - state_.scrollOffsetY;

            wchar_t lineNum[16];
            swprintf_s(lineNum, 16, L"%d", i + 1);

            D2D1_RECT_F rect = D2D1::RectF(
                state_.leftEdge,
                lineY,
                state_.leftEdge + metrics_.gutterWidth,
                lineY + metrics_.lineHeight);

            ID2D1SolidColorBrush *brushToUse = (i == state_.caret.line) ? activeTextBrush : textBrush;
            ctx->DrawTextW(
                lineNum,
                (UINT32)wcslen(lineNum),
                format,
                rect,
                brushToUse,
                D2D1_DRAW_TEXT_OPTIONS_NONE,
                DWRITE_MEASURING_MODE_NATURAL);
        }

        if (format)
            format->Release();
        if (textBrush)
            textBrush->Release();
        if (activeTextBrush)
            activeTextBrush->Release();
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

    // Version corrigée avec gestion propre des brushes
    void Editor::DrawTextContent(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite)
    {
        const wchar_t *editorFont = L"JetBrains Mono";
        const float editorFontSize = 14.0f * zoomLevel_;
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

        for (int i = firstVisibleLine; i < lastVisibleLine; ++i)
        {
            float lineY = state_.topEdge + (i * metrics_.lineHeight) - state_.scrollOffsetY;

            const std::wstring &line = state_.lines[i];
            if (line.empty())
                continue;

            IDWriteTextLayout *lineLayout = nullptr;
            if (!format || FAILED(dwrite->CreateTextLayout(
                    line.c_str(),
                    (UINT32)line.size(),
                    format,
                    contentWidth,
                    metrics_.lineHeight,
                    &lineLayout)) ||
                !lineLayout)
            {
                if (lineLayout)
                    lineLayout->Release();
                continue;
            }

            auto tokens = highlighter_->TokenizeLine(line, ext);

            // ✅ Stocker les brushes pour les release après le Draw
            std::vector<ID2D1SolidColorBrush *> brushes;

            for (const auto &tok : tokens)
            {
                if (tok.length <= 0)
                    continue;

                // Only apply coloring for meaningful token types; leave others untouched
                if (tok.type == ::Orion::Syntax::TokenType::Normal)
                    continue;

                bool isColorable = (
                    tok.type == ::Orion::Syntax::TokenType::Keyword ||
                    tok.type == ::Orion::Syntax::TokenType::Type ||
                    tok.type == ::Orion::Syntax::TokenType::String ||
                    tok.type == ::Orion::Syntax::TokenType::Comment ||
                    tok.type == ::Orion::Syntax::TokenType::Number ||
                    tok.type == ::Orion::Syntax::TokenType::Preprocessor ||
                    tok.type == ::Orion::Syntax::TokenType::MarkdownHeading ||
                    tok.type == ::Orion::Syntax::TokenType::MarkdownCode ||
                    tok.type == ::Orion::Syntax::TokenType::MarkdownLink);

                if (!isColorable)
                    continue;

                D2D1_COLOR_F color = GetTokenColor(tok.type, ext);

                ID2D1SolidColorBrush *brush = nullptr;
                ctx->CreateSolidColorBrush(color, &brush);
                if (brush)
                {
                    DWRITE_TEXT_RANGE range = {(UINT32)tok.start, (UINT32)tok.length};
                    lineLayout->SetDrawingEffect(brush, range);
                    brushes.push_back(brush);
                }
            }

            float x = contentLeft - state_.scrollOffsetX;
            // Vertically center the text within the line using font ascent/descent
            float verticalOffset = 0.0f;
            float fontPixelHeight = fontAscent_ + fontDescent_;
            if (metrics_.lineHeight > fontPixelHeight)
                verticalOffset = (metrics_.lineHeight - fontPixelHeight) / 2.0f;
            D2D1_POINT_2F origin = D2D1::Point2F(x, lineY + verticalOffset);

            CustomTextRenderer renderer(ctx, defaultBrush);
            lineLayout->Draw(NULL, &renderer, origin.x, origin.y);

            lineLayout->Release();

            // ✅ Release tous les brushes APRÈS le Draw
            for (auto *b : brushes)
            {
                if (b)
                    b->Release();
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
            case ::Orion::Syntax::TokenType::Keyword: return D2D1::ColorF(0.9f, 0.4f, 0.4f); // reddish
            case ::Orion::Syntax::TokenType::Type: return D2D1::ColorF(0.4f, 0.8f, 0.9f);
            case ::Orion::Syntax::TokenType::String: return D2D1::ColorF(0.6f, 0.8f, 0.5f);
            case ::Orion::Syntax::TokenType::Comment: return D2D1::ColorF(0.45f, 0.45f, 0.45f);
            case ::Orion::Syntax::TokenType::Number: return D2D1::ColorF(0.8f, 0.6f, 0.9f);
            default: return theme_.text;
            }
        }
        else if (ext == L".md")
        {
            switch (type)
            {
            case ::Orion::Syntax::TokenType::MarkdownHeading: return D2D1::ColorF(0.95f, 0.9f, 0.6f);
            case ::Orion::Syntax::TokenType::MarkdownCode: return D2D1::ColorF(0.8f, 0.8f, 0.85f);
            case ::Orion::Syntax::TokenType::MarkdownLink: return D2D1::ColorF(0.5f, 0.75f, 0.95f);
            default: return theme_.text;
            }
        }
        else if (ext == L".json")
        {
            switch (type)
            {
            case ::Orion::Syntax::TokenType::String: return D2D1::ColorF(0.6f, 0.8f, 0.5f);
            case ::Orion::Syntax::TokenType::Number: return D2D1::ColorF(0.8f, 0.6f, 0.9f);
            default: return theme_.text;
            }
        }

        else if (ext == L".js" || ext == L".ts")
        {
            switch (type)
            {
            case ::Orion::Syntax::TokenType::Keyword: return D2D1::ColorF(0.4f, 0.6f, 0.95f);
            case ::Orion::Syntax::TokenType::Type: return D2D1::ColorF(0.4f, 0.9f, 0.9f);
            case ::Orion::Syntax::TokenType::String: return D2D1::ColorF(0.6f, 0.9f, 0.6f);
            case ::Orion::Syntax::TokenType::Comment: return D2D1::ColorF(0.5f, 0.5f, 0.5f);
            case ::Orion::Syntax::TokenType::Number: return D2D1::ColorF(0.9f, 0.6f, 0.9f);
            default: return theme_.text;
            }
        }

        else if (ext == L".rs")
        {
            switch (type)
            {
            case ::Orion::Syntax::TokenType::Keyword: return D2D1::ColorF(0.95f, 0.6f, 0.25f);
            case ::Orion::Syntax::TokenType::Type: return D2D1::ColorF(0.3f, 0.85f, 0.9f);
            case ::Orion::Syntax::TokenType::String: return D2D1::ColorF(0.6f, 0.9f, 0.6f);
            case ::Orion::Syntax::TokenType::Comment: return D2D1::ColorF(0.45f, 0.45f, 0.45f);
            default: return theme_.text;
            }
        }

        else if (ext == L".c" || ext == L".cpp" || ext == L".h" || ext == L".hpp")
        {
            // keep C/C++ style palette
            switch (type)
            {
            case ::Orion::Syntax::TokenType::Keyword: return D2D1::ColorF(0.86f, 0.58f, 0.22f);
            case ::Orion::Syntax::TokenType::Type: return D2D1::ColorF(0.4f, 0.8f, 1.0f);
            case ::Orion::Syntax::TokenType::String: return D2D1::ColorF(0.56f, 0.87f, 0.56f);
            case ::Orion::Syntax::TokenType::Comment: return D2D1::ColorF(0.5f, 0.5f, 0.5f);
            default: return theme_.text;
            }
        }

        // Default mapping (C/C++ style)
        switch (type)
        {
        case ::Orion::Syntax::TokenType::Keyword: return D2D1::ColorF(0.86f, 0.58f, 0.22f);
        case ::Orion::Syntax::TokenType::Type: return D2D1::ColorF(0.4f, 0.8f, 1.0f);
        case ::Orion::Syntax::TokenType::String: return D2D1::ColorF(0.56f, 0.87f, 0.56f);
        case ::Orion::Syntax::TokenType::Comment: return D2D1::ColorF(0.5f, 0.5f, 0.5f);
        case ::Orion::Syntax::TokenType::Number: return D2D1::ColorF(0.8f, 0.6f, 0.9f);
        case ::Orion::Syntax::TokenType::Preprocessor: return D2D1::ColorF(0.9f, 0.7f, 0.4f);
        case ::Orion::Syntax::TokenType::MarkdownHeading: return D2D1::ColorF(0.9f, 0.9f, 0.6f);
        case ::Orion::Syntax::TokenType::MarkdownCode: return D2D1::ColorF(0.8f, 0.8f, 0.85f);
        default: return theme_.text;
        }
    }

    D2D1_POINT_2F Editor::TextToScreenPosition(CaretPosition pos)
    {
        float contentLeft = state_.leftEdge + metrics_.gutterWidth + metrics_.leftPadding;
        float y = state_.topEdge + (pos.line * metrics_.lineHeight) - state_.scrollOffsetY;

        if (pos.column == 0)
        {
            float x = contentLeft - state_.scrollOffsetX;
            return D2D1::Point2F(x, y);
        }

        if (!cachedTextFormat_ || pos.line < 0 || pos.line >= (int)state_.lines.size())
        {
            float x = contentLeft - state_.scrollOffsetX;
            return D2D1::Point2F(x, y);
        }

        const std::wstring &line = state_.lines[pos.line];
        IDWriteTextLayout *layout = nullptr;

        if (SUCCEEDED(pDWriteFactory_->CreateTextLayout(
                line.c_str(),
                (UINT32)line.size(),
                cachedTextFormat_, // ✅ Utiliser le format caché
                10000.0f,
                metrics_.lineHeight,
                &layout)) &&
            layout)
        {
            FLOAT caretX = 0.0f;
            FLOAT caretY = 0.0f;
            DWRITE_HIT_TEST_METRICS hitMetrics = {};

            UINT32 textPos = (std::min)((UINT32)pos.column, (UINT32)line.size());

            layout->HitTestTextPosition(textPos, FALSE, &caretX, &caretY, &hitMetrics);

            float x = contentLeft + caretX - state_.scrollOffsetX;

            layout->Release();
            return D2D1::Point2F(x, y);
        }

        float x = contentLeft - state_.scrollOffsetX;
        return D2D1::Point2F(x, y);
    }

    CaretPosition Editor::ScreenToTextPosition(POINT screenPoint)
    {
        float contentLeft = state_.leftEdge + metrics_.gutterWidth + metrics_.leftPadding;

        int line = (int)((screenPoint.y - state_.topEdge + state_.scrollOffsetY) / metrics_.lineHeight);
        line = (std::max)(0, (std::min)(line, (int)state_.lines.size() - 1));

        if (line < 0 || line >= (int)state_.lines.size())
        {
            return {0, 0};
        }

        // Pour les lignes vides, retourner directement colonne 0
        if (state_.lines[line].empty())
        {
            return {line, 0};
        }

        // If the line is only whitespace (e.g. an auto-inserted indent),
        // DirectWrite hit testing can behave inconsistently. Use a simple
        // character-width based column calculation so spaces are selectable.
        bool onlyWhitespace = true;
        for (wchar_t wc : state_.lines[line])
        {
            if (!iswspace(wc))
            {
                onlyWhitespace = false;
                break;
            }
        }
        if (onlyWhitespace)
        {
            int column = (int)((screenPoint.x - contentLeft + state_.scrollOffsetX) / metrics_.characterWidth);
            column = (std::max)(0, (std::min)(column, (int)state_.lines[line].size()));
            return {line, column};
        }

        // Utiliser DirectWrite pour trouver la colonne précise
        if (pDWriteFactory_)
        {
            float clickX = screenPoint.x - contentLeft + state_.scrollOffsetX;

            IDWriteTextFormat *format = cachedTextFormat_;
            IDWriteTextFormat *tmpFmt = nullptr;
            if (!format && pDWriteFactory_)
            {
                pDWriteFactory_->CreateTextFormat(
                    L"JetBrains Mono", customFontCollection_,
                    DWRITE_FONT_WEIGHT_NORMAL,
                    DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL,
                    14.0f * zoomLevel_, L"en-us",
                    &tmpFmt);
                if (tmpFmt)
                    format = tmpFmt;
            }

            if (format)
            {
                IDWriteTextLayout *layout = nullptr;
                if (SUCCEEDED(pDWriteFactory_->CreateTextLayout(
                        state_.lines[line].c_str(),
                        (UINT32)state_.lines[line].size(),
                        format,
                        10000.0f,
                        metrics_.lineHeight,
                        &layout)) && layout)
                {
                    BOOL isTrailingHit = FALSE;
                    BOOL isInside = FALSE;
                    DWRITE_HIT_TEST_METRICS hitMetrics = {};

                    layout->HitTestPoint(clickX, 0, &isTrailingHit, &isInside, &hitMetrics);

                    int column = hitMetrics.textPosition;
                    if (isTrailingHit)
                        column++;

                    column = (std::max)(0, (std::min)(column, (int)state_.lines[line].size()));

                    layout->Release();
                    if (tmpFmt)
                        tmpFmt->Release();
                    return {line, column};
                }
                if (tmpFmt)
                    tmpFmt->Release();
            }
        }

        // Fallback à l'ancienne méthode
        int column = (int)((screenPoint.x - contentLeft + state_.scrollOffsetX) / metrics_.characterWidth);
        column = (std::max)(0, (std::min)(column, (int)state_.lines[line].size()));

        return {line, column};
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

        // ✨ NOUVEAU : Si le SearchBox est visible et le clic est dedans -> laisser le SearchBox gérer
        if (searchBox_.IsVisible())
        {
            bool inside = searchBox_.IsPointInSearchBox(pt);
            {
                wchar_t buf[256];
                swprintf_s(buf, L"Editor::OnLeftButtonDown - click=(%d,%d) searchVisible=1 insideSearchBox=%d",
                           pt.x, pt.y, inside ? 1 : 0);
                Logger::Instance().Log(buf);
            }
            if (inside)
            {
                searchBox_.OnLeftButtonDown(pt);
                return;
            }
        }

        // Si le SearchBox est visible mais que le clic se fait en-dehors, lui retirer le focus
        if (searchBox_.IsVisible())
        {
            searchBox_.SetInputFocused(false);
        }

        // If completion popup visible and click inside it, forward
        if (completionPopup_ && completionPopup_->IsVisible())
        {
            if (completionPopup_->IsPointInPopup(pt))
            {
                completionPopup_->OnLeftButtonDown(pt);
                // If an item was chosen, perform insertion
                std::wstring chosen = completionPopup_->GetSelectedItem();
                if (!chosen.empty())
                {
                    // Insert the chosen include path, replacing the fragment between
                    // '<' or '"' and the caret position
                    std::wstring &line = state_.lines[state_.caret.line];
                    size_t pos = line.rfind(L"#include");
                    if (pos != std::wstring::npos)
                    {
                        // find last '<' or '"' before caret and after #include
                        size_t lt = line.find_last_of(L"<", state_.caret.column - 1);
                        size_t qt = line.find_last_of(L'"', state_.caret.column - 1);
                        size_t start = std::wstring::npos;
                        if (lt != std::wstring::npos && lt > pos) start = lt + 1;
                        if (qt != std::wstring::npos && qt > pos) start = (start == std::wstring::npos) ? qt + 1 : std::min(start, qt + 1);
                        if (start == std::wstring::npos) start = state_.caret.column;
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
                // click outside popup -> hide it
                completionPopup_->Hide();
            }
        }

        if (pt.x < state_.leftEdge + metrics_.gutterWidth)
        {
            return;
        }

        state_.caret = ScreenToTextPosition(pt);
        EnsureCaretVisible();
        state_.hasSelection = false; // Réinitialiser la sélection
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
                if (state_.scrollOffsetX < 0.0f) state_.scrollOffsetX = 0.0f;
                if (state_.scrollOffsetX > maxScroll) state_.scrollOffsetX = maxScroll;
                hThumbPos_ = (state_.scrollOffsetX / maxScroll) * availableTrack;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return;
        }

        // Vérifier si on est dans la zone de l'éditeur
        float contentLeft = state_.leftEdge + metrics_.gutterWidth;
        bool isInEditorArea = !(pt.x < contentLeft || pt.x > state_.rightEdge || pt.y < state_.topEdge || pt.y > state_.bottomEdge);
        // If mouse is outside the editor AND no left-button drag is happening, ignore.
        if (!isInEditorArea && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000))
        {
            return;
        }

        if (GetAsyncKeyState(VK_LBUTTON) & 0x8000)
        {
            if (pt.x < state_.leftEdge + metrics_.gutterWidth)
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
                float over = ((int)state_.topEdge + 2) - pt.y;
                float speed = (std::max)(4.0f, over * 0.5f);
                scrollbar_.ScrollBy(-speed);
                state_.scrollOffsetY = scrollbar_.GetScrollOffset();
                needsRedraw = true;
            }
            else if (pt.y > (int)state_.bottomEdge - 2)
            {
                float over = pt.y - ((int)state_.bottomEdge - 2);
                float speed = (std::max)(4.0f, over * 0.5f);
                scrollbar_.ScrollBy(speed);
                state_.scrollOffsetY = scrollbar_.GetScrollOffset();
                needsRedraw = true;
            }

            // Recompute caret position after potential scroll
            CaretPosition newPos = ScreenToTextPosition(pt);
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
            // Nothing for now; popup handles clicks only
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
            // Supprimer la fin de la première ligne
            state_.lines[start.line].erase(start.column);

            // Ajouter le reste de la dernière ligne
            if (end.column < (int)state_.lines[end.line].size())
            {
                state_.lines[start.line] += state_.lines[end.line].substr(end.column);
            }

            // Supprimer les lignes intermédiaires
            state_.lines.erase(state_.lines.begin() + start.line + 1,
                               state_.lines.begin() + end.line + 1);
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
        EnsureCaretVisible();
    }

    void Editor::DeleteSelectionPublic()
    {
        DeleteSelection();
    }

    void Editor::OnChar(wchar_t ch)
    {
        // ✨ NOUVEAU : Si le SearchBox est visible ET focalisé, lui envoyer les caractères
        if (searchBox_.IsVisible() && searchBox_.IsInputFocused())
        {
            searchBox_.OnChar(ch);
            // Re-effectuer la recherche après chaque caractère
            searchBox_.PerformSearch(state_.lines);

            // Scroller vers le match courant si disponible
            if (!searchBox_.GetMatches().empty())
            {
                int idx = searchBox_.GetCurrentMatchIndex();
                if (idx >= 0 && idx < (int)searchBox_.GetMatches().size())
                {
                    const auto& match = searchBox_.GetMatches()[idx];
                    state_.caret.line = match.line;
                    state_.caret.column = match.startColumn;
                    EnsureCaretVisible();
                }
            }
            return;
        }

        // If a previous OnKeyDown consumed this char (e.g. Ctrl+Space), suppress it
        if (suppressNextChar_ && ch == L' ')
        {
            suppressNextChar_ = false;
            return;
        }

        if (ch < 32 && ch != L'\t' && ch != L'\r' && ch != L'\n')
            return;

        // Push undo snapshot before any mutation
        if (undoStack_.empty() || undoStack_.back().lines != state_.lines ||
            undoStack_.back().caret.line != state_.caret.line ||
            undoStack_.back().caret.column != state_.caret.column)
        {
            undoStack_.push_back(state_);
            if (undoStack_.size() > maxUndoEntries_)
                undoStack_.erase(undoStack_.begin());
        }

        // Supprimer la sélection si elle existe
        if (state_.hasSelection)
        {
            DeleteSelection();
        }

        // If completion popup visible and typing, hide it
        if (completionPopup_ && completionPopup_->IsVisible())
        {
            completionPopup_->Hide();
        }

        if (ch == L'\r' || ch == L'\n')
        {
            // Nouvelle ligne
            std::wstring currentLine = state_.lines[state_.caret.line];
            std::wstring before = currentLine.substr(0, state_.caret.column);
            std::wstring after = currentLine.substr(state_.caret.column);

            // Special-case: if caret is between matching braces/paren/brackets
            // (e.g. "{    }" or "(|)" with only whitespace between),
            // insert an indented blank line between them like VSCode.
            auto matching = [](wchar_t open)->wchar_t {
                switch (open)
                {
                case L'(': return L')';
                case L'{': return L'}';
                case L'[': return L']';
                default: return 0;
                }
            };

            // Find last non-space in before
            int lb = (int)before.size() - 1;
            while (lb >= 0 && iswspace(before[lb]))
                lb--;

            bool didSpecial = false;
            if (lb >= 0)
            {
                wchar_t openChar = before[lb];
                wchar_t expectedClose = matching(openChar);
                if (expectedClose != 0)
                {
                    // find first non-space in after
                    int fa = 0;
                    while (fa < (int)after.size() && iswspace(after[fa]))
                        fa++;
                    if (fa < (int)after.size() && after[fa] == expectedClose)
                    {
                        // compute base indent (leading whitespace of the current line)
                        std::wstring baseIndent;
                        for (size_t i = 0; i < currentLine.size(); ++i)
                        {
                            if (!iswspace(currentLine[i]))
                                break;
                            baseIndent.push_back(currentLine[i]);
                        }

                        // indent unit = 4 spaces (same as Tab behavior)
                        std::wstring innerIndent = baseIndent + L"    ";

                        // left content: before up to openChar (trim trailing spaces)
                        std::wstring left = before.substr(0, lb + 1);

                        // right content: after from first non-space (keep rest)
                        std::wstring right = after.substr(fa);

                        // Replace current line and insert two new lines
                        state_.lines[state_.caret.line] = left;
                        state_.lines.insert(state_.lines.begin() + state_.caret.line + 1, innerIndent);
                        state_.lines.insert(state_.lines.begin() + state_.caret.line + 2, baseIndent + right);

                        state_.caret.line++;
                        state_.caret.column = (int)innerIndent.size();
                        didSpecial = true;
                    }
                }
            }

            if (!didSpecial)
            {
                state_.lines[state_.caret.line] = before;
                state_.lines.insert(state_.lines.begin() + state_.caret.line + 1, after);

                state_.caret.line++;
                state_.caret.column = 0;
            }
        }
        else if (ch == L'\t')
        {
            // Tab = 4 espaces
            std::wstring spaces = L"    ";
            state_.lines[state_.caret.line].insert(state_.caret.column, spaces);
            state_.caret.column += 4;
        }
        else
        {
            // Auto-pairing for brackets and quotes, and skip-over for closing chars
            auto matching = [](wchar_t c)->wchar_t {
                switch (c)
                {
                case L'(': return L')';
                case L'{': return L'}';
                case L'[': return L']';
                case L'"': return L'"';
                case L'\'': return L'\'';
                default: return 0;
                }
            };

            wchar_t closeForOpen = matching(ch);
            bool isQuote = (ch == L'"' || ch == L'\'');

            if (closeForOpen != 0)
            {
                // Special handling for quotes because opening and closing are the same
                if (isQuote)
                {
                    std::wstring &line = state_.lines[state_.caret.line];

                    // If there's a single-line selection, wrap it with the quotes
                    if (state_.hasSelection)
                    {
                        CaretPosition a = state_.selectionStart;
                        CaretPosition b = state_.caret;
                        if (a.line > b.line || (a.line == b.line && a.column > b.column))
                            std::swap(a, b);

                        if (a.line == b.line)
                        {
                            std::wstring &selLine = state_.lines[a.line];
                            selLine.insert(b.column, 1, ch);
                            selLine.insert(a.column, 1, ch);
                            state_.hasSelection = false;
                            state_.caret.line = b.line;
                            state_.caret.column = b.column + 1;
                        }
                        else
                        {
                            // Fallback for multi-line selection: insert an empty pair at caret
                            std::wstring pairStr;
                            pairStr.push_back(ch);
                            pairStr.push_back(ch);
                            state_.lines[state_.caret.line].insert(state_.caret.column, pairStr);
                            state_.caret.column += 1;
                        }
                    }
                    else
                    {
                        // No selection: if next char is the same quote and not escaped, skip over it
                        if (state_.caret.column < (int)line.size() && line[state_.caret.column] == ch)
                        {
                            bool escaped = false;
                            if (state_.caret.column > 0 && line[state_.caret.column - 1] == L'\\')
                                escaped = true;

                            if (!escaped)
                            {
                                state_.caret.column++;
                            }
                            else
                            {
                                // insert escaped quote normally
                                state_.lines[state_.caret.line].insert(state_.caret.column, 1, ch);
                                state_.caret.column++;
                            }
                        }
                        else
                        {
                            // insert pair and place caret between
                            std::wstring pairStr;
                            pairStr.push_back(ch);
                            pairStr.push_back(ch);
                            state_.lines[state_.caret.line].insert(state_.caret.column, pairStr);
                            state_.caret.column += 1;
                        }
                    }
                }
                else
                {
                    // opening bracket behavior (wrap single-line selection or insert pair)
                    if (state_.hasSelection)
                    {
                        CaretPosition a = state_.selectionStart;
                        CaretPosition b = state_.caret;
                        if (a.line > b.line || (a.line == b.line && a.column > b.column))
                            std::swap(a, b);

                        if (a.line == b.line)
                        {
                            std::wstring &line = state_.lines[a.line];
                            line.insert(b.column, 1, closeForOpen);
                            line.insert(a.column, 1, ch);
                            state_.hasSelection = false;
                            state_.caret.line = b.line;
                            state_.caret.column = b.column + 1;
                        }
                        else
                        {
                            std::wstring pairStr;
                            pairStr.push_back(ch);
                            pairStr.push_back(closeForOpen);
                            state_.lines[state_.caret.line].insert(state_.caret.column, pairStr);
                            state_.caret.column += 1;
                        }
                    }
                    else
                    {
                        std::wstring pairStr;
                        pairStr.push_back(ch);
                        pairStr.push_back(closeForOpen);
                        state_.lines[state_.caret.line].insert(state_.caret.column, pairStr);
                        state_.caret.column += 1;
                    }
                }
            }
            else if (ch == L')' || ch == L'}' || ch == L']')
            {
                // If the next character is the same closing char, skip over it
                std::wstring &line = state_.lines[state_.caret.line];
                if (state_.caret.column < (int)line.size() && line[state_.caret.column] == ch)
                {
                    state_.caret.column++;
                }
                else
                {
                    // Otherwise insert normally
                    state_.lines[state_.caret.line].insert(state_.caret.column, 1, ch);
                    state_.caret.column++;
                }
            }
            else
            {
                // Caractère normal
                state_.lines[state_.caret.line].insert(state_.caret.column, 1, ch);
                state_.caret.column++;
            }
        }

        state_.caretVisible = true;
        EnsureCaretVisible();
        state_.lastBlinkTime = GetTickCount();
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
            if (state_.scrollOffsetX < 0.0f) state_.scrollOffsetX = 0.0f;
            if (state_.scrollOffsetX > maxScroll) state_.scrollOffsetX = maxScroll;
            float availableTrack = hViewportWidth_ - hThumbWidth_;
            if (maxScroll > 0.0f)
                hThumbPos_ = (state_.scrollOffsetX / maxScroll) * availableTrack;
            // Request redraw so the thumb and text positions update immediately
            if (hwnd)
                InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }

        if (ctrlPressed)
        {
            const int steps = delta / WHEEL_DELTA;
            if (steps > 0)
            {
                for (int i = 0; i < steps; ++i)
                    ZoomIn();
            }
            else if (steps < 0)
            {
                for (int i = 0; i < -steps; ++i)
                    ZoomOut();
            }
            return;
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
        if (state_.scrollOffsetX < 0.0f) state_.scrollOffsetX = 0.0f;
        if (state_.scrollOffsetX > maxScroll) state_.scrollOffsetX = maxScroll;
        float availableTrack = hViewportWidth_ - hThumbWidth_;
        if (maxScroll > 0.0f)
            hThumbPos_ = (state_.scrollOffsetX / maxScroll) * availableTrack;
        if (hwnd)
            InvalidateRect(hwnd, nullptr, FALSE);
    }

    void Editor::ZoomIn()
    {
        zoomLevel_ += ZOOM_STEP;
        if (zoomLevel_ > MAX_ZOOM)
            zoomLevel_ = MAX_ZOOM;
        // Force recalculation of font metrics and recreate cached format
        fontMetricsInitialized_ = false;
        if (cachedTextFormat_)
        {
            cachedTextFormat_->Release();
            cachedTextFormat_ = nullptr;
        }
    }

    void Editor::ZoomOut()
    {
        zoomLevel_ -= ZOOM_STEP;
        if (zoomLevel_ < MIN_ZOOM)
            zoomLevel_ = MIN_ZOOM;
        fontMetricsInitialized_ = false;
        if (cachedTextFormat_)
        {
            cachedTextFormat_->Release();
            cachedTextFormat_ = nullptr;
        }
    }

    void Editor::BuildHeaderIndex()
    {
        headerIndex_.clear();
        // simple scan of common folders
        try {
            namespace fs = std::filesystem;
            std::vector<fs::path> roots = { fs::path("src"), fs::path("external") };
            for (auto &r : roots)
            {
                if (!fs::exists(r)) continue;
                for (auto &p : fs::recursive_directory_iterator(r))
                {
                    if (!p.is_regular_file()) continue;
                    auto ext = p.path().extension().wstring();
                    if (ext == L".h" || ext == L".hpp" || ext == L".hh" || ext == L".inc")
                    {
                        // store a path relative to project root, using forward slashes
                        try {
                            fs::path rel = fs::relative(p.path(), fs::current_path());
                            std::string s = rel.generic_string();
                            // convert to wide
                            int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
                            std::wstring ws(wlen, L'\0');
                            if (wlen > 0)
                                MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), ws.data(), wlen);
                            headerIndex_.push_back(ws);
                        } catch (...) {
                            headerIndex_.push_back(p.path().filename().wstring());
                        }
                    }
                }
            }
            // dedupe
            std::sort(headerIndex_.begin(), headerIndex_.end());
            headerIndex_.erase(std::unique(headerIndex_.begin(), headerIndex_.end()), headerIndex_.end());
        } catch (...) {
            // ignore filesystem errors
        }
    }

    std::vector<std::wstring> Editor::GetIncludeSuggestions(const std::wstring &prefix) const
    {
        std::vector<std::wstring> out;
        for (const auto &h : headerIndex_)
        {
            if (prefix.empty()) out.push_back(h);
            else
            {
                std::wstring low = h;
                std::wstring lp = prefix;
                for (auto &c : low) c = towlower(c);
                for (auto &c : lp) c = towlower(c);
                if (low.rfind(lp, 0) == 0 || low.find(lp) != std::wstring::npos)
                    out.push_back(h);
            }
            if (out.size() >= 200) break;
        }
        return out;
    }

    void Editor::ResetZoom()
    {
        zoomLevel_ = 1.0f;
        fontMetricsInitialized_ = false;
        if (cachedTextFormat_)
        {
            cachedTextFormat_->Release();
            cachedTextFormat_ = nullptr;
        }
    }

    void Editor::OnKeyDown(WPARAM key)
    {
        {
            wchar_t buf[128];
            bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            swprintf_s(buf, L"Editor::OnKeyDown - key=%d ctrl=%d shift=%d", (int)key, ctrl ? 1 : 0, shift ? 1 : 0);
            Logger::Instance().Log(std::wstring(buf));
        }
        bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

        // ✨ NOUVEAU : Ctrl+F pour ouvrir la recherche
        if (ctrl && (key == 'F' || key == 'f'))
        {
            ShowSearch();
            // Effectuer une recherche initiale si du texte est déjà présent
            if (!searchBox_.GetSearchText().empty())
            {
                searchBox_.PerformSearch(state_.lines);
            }
            return;
        }

        // Ctrl+Space -> trigger completion popup for includes
        if (ctrl && key == VK_SPACE)
        {
            // only in C/C++/h/hpp/.c/.cpp or empty ext treat similarly
            std::wstring ext;
            if (!state_.filePath.empty())
            {
                size_t pos = state_.filePath.find_last_of(L'.');
                if (pos != std::wstring::npos)
                {
                    ext = state_.filePath.substr(pos);
                    for (auto &c : ext) c = towlower(c);
                }
            }

            // Only support include suggestion for C/C++ files
            if (ext == L".c" || ext == L".cpp" || ext == L".h" || ext == L".hpp" || ext.empty())
            {
                // determine if caret is within an include <...> or "..."
                const std::wstring &line = state_.lines[state_.caret.line];
                size_t posInclude = line.rfind(L"#include", state_.caret.column);
                if (posInclude != std::wstring::npos)
                {
                    // find last '<' or '"' after #include
                    size_t lt = line.find_last_of(L"<\"", state_.caret.column - 1);
                    if (lt != std::wstring::npos && lt > posInclude)
                    {
                        // extract prefix from after lt to caret
                        size_t start = lt + 1;
                        size_t end = (size_t)state_.caret.column;
                        if (end < start) end = start;
                        std::wstring prefix = line.substr(start, end - start);
                        auto suggestions = GetIncludeSuggestions(prefix);
                        if (!suggestions.empty())
                        {
                            completionPopup_->SetItems(suggestions);
                            D2D1_POINT_2F p = TextToScreenPosition(state_.caret);
                            completionPopup_->UpdateLayout(p.x, p.y + metrics_.lineHeight, 400.0f, metrics_.lineHeight);
                            completionPopup_->Show();
                            // Prevent the WM_CHAR for the space from inserting a literal space
                            suppressNextChar_ = true;
                        }
                        return;
                    }
                }
            }
        }

        // ✨ NOUVEAU : Si le SearchBox est visible et focalisé, gérer ses touches
        if (searchBox_.IsVisible() && searchBox_.IsInputFocused())
        {
            searchBox_.OnKeyDown(key);
            
            // Re-effectuer la recherche après certaines touches
            if (key == VK_BACK || key == VK_DELETE || key == VK_RETURN)
            {
                searchBox_.PerformSearch(state_.lines);
                
                // Scroller vers le match courant
                if (!searchBox_.GetMatches().empty())
                {
                    int idx = searchBox_.GetCurrentMatchIndex();
                    if (idx >= 0 && idx < (int)searchBox_.GetMatches().size())
                    {
                        const auto& match = searchBox_.GetMatches()[idx];
                        state_.caret.line = match.line;
                        state_.caret.column = match.startColumn;
                        EnsureCaretVisible();
                    }
                }
            }
            
            // Si Échap a été pressé, le SearchBox s'est fermé, on sort
            if (!searchBox_.IsVisible())
                return;
            
            // Ne pas propager les touches au reste de l'éditeur
            return;
        }

        // Ctrl+Z -> Undo
        if (ctrl && (key == 'Z' || key == 'z'))
        {
            Undo();
            return;
        }

        // Commencer/continuer une sélection si Shift est enfoncé
        if (shift && !state_.hasSelection)
        {
            state_.selectionStart = state_.caret;
            state_.hasSelection = true;
        }
        else if (!shift && state_.hasSelection &&
                 key != VK_BACK && key != VK_DELETE &&
                 key != 'C' && key != 'X' && key != 'V')
        {
            state_.hasSelection = false;
        }

        switch (key)
        {
        case VK_LEFT:
            if (state_.caret.column > 0)
            {
                state_.caret.column--;
            }
            else if (state_.caret.line > 0)
            {
                state_.caret.line--;
                state_.caret.column = (int)state_.lines[state_.caret.line].size();
            }
            break;

        case VK_RIGHT:
            if (state_.caret.column < (int)state_.lines[state_.caret.line].size())
            {
                state_.caret.column++;
            }
            else if (state_.caret.line < (int)state_.lines.size() - 1)
            {
                state_.caret.line++;
                state_.caret.column = 0;
            }
            break;

        case VK_UP:
            if (state_.caret.line > 0)
            {
                state_.caret.line--;
                state_.caret.column = (std::min)(state_.caret.column,
                                                 (int)state_.lines[state_.caret.line].size());
            }
            break;

        case VK_DOWN:
            if (state_.caret.line < (int)state_.lines.size() - 1)
            {
                state_.caret.line++;
                state_.caret.column = (std::min)(state_.caret.column,
                                                 (int)state_.lines[state_.caret.line].size());
            }
            break;

        case VK_HOME:
            state_.caret.column = 0;
            break;

        case VK_END:
            state_.caret.column = (int)state_.lines[state_.caret.line].size();
            break;

        case VK_BACK:
            // push undo snapshot before mutating
            if (undoStack_.empty() || undoStack_.back().lines != state_.lines ||
                undoStack_.back().caret.line != state_.caret.line ||
                undoStack_.back().caret.column != state_.caret.column)
            {
                undoStack_.push_back(state_);
                if (undoStack_.size() > maxUndoEntries_)
                    undoStack_.erase(undoStack_.begin());
            }

            if (state_.hasSelection)
            {
                DeleteSelection();
            }
            else if (state_.caret.column > 0)
            {
                state_.lines[state_.caret.line].erase(state_.caret.column - 1, 1);
                state_.caret.column--;
            }
            else if (state_.caret.line > 0)
            {
                // Fusionner avec la ligne précédente
                int prevLineLen = (int)state_.lines[state_.caret.line - 1].size();
                state_.lines[state_.caret.line - 1] += state_.lines[state_.caret.line];
                state_.lines.erase(state_.lines.begin() + state_.caret.line);
                state_.caret.line--;
                state_.caret.column = prevLineLen;
            }
            break;

        case VK_DELETE:
            // push undo snapshot before mutating
            if (undoStack_.empty() || undoStack_.back().lines != state_.lines ||
                undoStack_.back().caret.line != state_.caret.line ||
                undoStack_.back().caret.column != state_.caret.column)
            {
                undoStack_.push_back(state_);
                if (undoStack_.size() > maxUndoEntries_)
                    undoStack_.erase(undoStack_.begin());
            }

            if (state_.hasSelection)
            {
                DeleteSelection();
            }
            else if (state_.caret.column < (int)state_.lines[state_.caret.line].size())
            {
                state_.lines[state_.caret.line].erase(state_.caret.column, 1);
            }
            else if (state_.caret.line < (int)state_.lines.size() - 1)
            {
                // Fusionner avec la ligne suivante
                state_.lines[state_.caret.line] += state_.lines[state_.caret.line + 1];
                state_.lines.erase(state_.lines.begin() + state_.caret.line + 1);
            }
            break;

        case 'A':
            if (ctrl)
            {
                // Select All
                state_.selectionStart = {0, 0};
                state_.caret = {(int)state_.lines.size() - 1,
                                (int)state_.lines.back().size()};
                state_.hasSelection = true;
            }
            break;

        case 'C':
            if (ctrl && state_.hasSelection)
            {
                // Copier la sélection dans le presse-papier (Unicode)
                auto getSelectionText = [this]() -> std::wstring
                {
                    CaretPosition start = state_.selectionStart;
                    CaretPosition end = state_.caret;
                    if (start.line > end.line || (start.line == end.line && start.column > end.column))
                        std::swap(start, end);

                    std::wstring out;
                    if (start.line == end.line)
                    {
                        out = state_.lines[start.line].substr(start.column, end.column - start.column);
                        return out;
                    }

                    // multiple lines
                    out += state_.lines[start.line].substr(start.column);
                    out += L"\r\n";
                    for (int L = start.line + 1; L < end.line; ++L)
                    {
                        out += state_.lines[L];
                        out += L"\r\n";
                    }
                    out += state_.lines[end.line].substr(0, end.column);
                    return out;
                };

                std::wstring sel = getSelectionText();
                if (!sel.empty())
                {
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
            }
            break;

        case 'X':
            if (ctrl && state_.hasSelection)
            {
                // Couper: copier puis supprimer la sélection
                // Réutiliser le même code que pour Ctrl+C
                auto getSelectionText = [this]() -> std::wstring
                {
                    CaretPosition start = state_.selectionStart;
                    CaretPosition end = state_.caret;
                    if (start.line > end.line || (start.line == end.line && start.column > end.column))
                        std::swap(start, end);

                    std::wstring out;
                    if (start.line == end.line)
                    {
                        out = state_.lines[start.line].substr(start.column, end.column - start.column);
                        return out;
                    }

                    out += state_.lines[start.line].substr(start.column);
                    out += L"\r\n";
                    for (int L = start.line + 1; L < end.line; ++L)
                    {
                        out += state_.lines[L];
                        out += L"\r\n";
                    }
                    out += state_.lines[end.line].substr(0, end.column);
                    return out;
                };

                std::wstring sel = getSelectionText();
                if (!sel.empty())
                {
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

                DeleteSelection();
            }
            break;

        case 'V':
            if (ctrl)
            {
                // snapshot before paste
                if (undoStack_.empty() || undoStack_.back().lines != state_.lines ||
                    undoStack_.back().caret.line != state_.caret.line ||
                    undoStack_.back().caret.column != state_.caret.column)
                {
                    undoStack_.push_back(state_);
                    if (undoStack_.size() > maxUndoEntries_)
                        undoStack_.erase(undoStack_.begin());
                }

                // Coller depuis le presse-papier (Unicode)
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
                                {
                                    continue;
                                }
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
                                // nothing to paste
                            }
                            else if (parts.size() == 1)
                            {
                                // simple insert in current line
                                state_.lines[state_.caret.line].insert(state_.caret.column, parts[0]);
                                state_.caret.column += (int)parts[0].size();
                            }
                            else
                            {
                                // Insert multi-line: first part into current line, then insert middle lines, then append tail to last part
                                std::wstring currentLine = state_.lines[state_.caret.line];
                                std::wstring before = currentLine.substr(0, state_.caret.column);
                                std::wstring after = currentLine.substr(state_.caret.column);

                                // first line becomes before + parts[0]
                                state_.lines[state_.caret.line] = before + parts[0];

                                // insert middle parts
                                int insertAt = state_.caret.line + 1;
                                for (size_t i = 1; i < parts.size(); ++i)
                                {
                                    state_.lines.insert(state_.lines.begin() + insertAt, parts[i]);
                                    insertAt++;
                                }

                                // append the original 'after' to the last inserted line
                                state_.lines[insertAt - 1] += after;

                                // Move caret to end of the inserted content
                                state_.caret.line = insertAt - 1;
                                state_.caret.column = (int)(state_.lines[state_.caret.line].size() - after.size());
                            }
                        }
                    }
                    CloseClipboard();
                }
            }
            break;
        }

        EnsureCaretVisible();
        state_.caretVisible = true;
        state_.lastBlinkTime = GetTickCount();
    }

    void Editor::UpdateCaretBlink()
    {
        DWORD current = GetTickCount();
        if (current - state_.lastBlinkTime > 500)
        {
            state_.caretVisible = !state_.caretVisible;
            state_.lastBlinkTime = current;
        }
    }

    void Editor::SetCaret(int line, int column)
    {
        // Clamp line to valid range
        if (state_.lines.empty())
        {
            state_.caret.line = 0;
            state_.caret.column = 0;
            return;
        }
        
        state_.caret.line = (std::max)(0, (std::min)(line, (int)state_.lines.size() - 1));
        
        // Clamp column to valid range for the line
        int maxCol = (int)state_.lines[state_.caret.line].length();
        state_.caret.column = (std::max)(0, (std::min)(column, maxCol));
        
        // Clear any selection
        state_.hasSelection = false;
        
        // Make sure caret is visible
        EnsureCaretVisible();
    }

    void Editor::EnsureCaretVisible()
    {
        float viewportHeight = state_.bottomEdge - state_.topEdge;
        float caretTop = state_.caret.line * metrics_.lineHeight;
        float caretBottom = caretTop + metrics_.lineHeight;
        float pad = metrics_.lineHeight * 0.25f;

        float current = state_.scrollOffsetY;
        if (caretTop < current + pad)
        {
            scrollbar_.SetScrollOffset((std::max)(0.0f, caretTop - pad));
            state_.scrollOffsetY = scrollbar_.GetScrollOffset();
        }
        else if (caretBottom > current + viewportHeight - pad)
        {
            float desired = caretBottom - viewportHeight + pad;
            scrollbar_.SetScrollOffset(desired);
            state_.scrollOffsetY = scrollbar_.GetScrollOffset();
        }
    }

} // namespace Orion