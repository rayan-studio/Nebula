#pragma once

#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <vector>
#include <memory>
#include <windows.h>
#include "ui/components/scrollbar/Scrollbar.h"
#include "orion/font/CustomFontLoader.h"
#include "../search/SearchBox.h"
#include "../syntax/Highlighter.h"
#include "../completion/CompletionService.h"
// New helpers
#include "orion/geometry/IndentationHelper.h"
#include "../rendering/GuideRenderer.h"
// Selection rendering
#include "../selection/Selection.h"
#include "orion/caret/CaretPosition.h"

// ============================================================================
// HELPER : Conversion couleurs Web (hex) → Direct2D AVEC CORRECTION GAMMA sRGB
// ============================================================================
#include <cstdio>
#include <cmath>

namespace Orion
{
    // ========================================================================
    // CONVERSION GAMMA : sRGB → Linear (pour Direct2D)
    // ========================================================================

    inline float SRGBToLinear(float srgb)
    {
        // Formule officielle sRGB → Linear
        if (srgb <= 0.04045f)
            return srgb / 12.92f;
        else
            return powf((srgb + 0.055f) / 1.055f, 2.4f);
    }

    // ========================================================================
    // CONVERSION HEX → D2D1_COLOR_F avec correction gamma
    // ========================================================================

    inline D2D1_COLOR_F ColorFromHex(uint32_t hex, float alpha = 1.0f, bool applyGamma = true)
    {
        float r = ((hex >> 16) & 0xFF) / 255.0f;
        float g = ((hex >> 8) & 0xFF) / 255.0f;
        float b = (hex & 0xFF) / 255.0f;

        // ⚠️ IMPORTANT : Appliquer la correction gamma sRGB
        if (applyGamma)
        {
            r = SRGBToLinear(r);
            g = SRGBToLinear(g);
            b = SRGBToLinear(b);
        }

        return D2D1::ColorF(r, g, b, alpha);
    }

    // Fonction alternative avec string (ex: "#121212")
    inline D2D1_COLOR_F ColorFromString(const char *hexStr, float alpha = 1.0f, bool applyGamma = true)
    {
        if (!hexStr || hexStr[0] != '#')
            return D2D1::ColorF(0, 0, 0, alpha);

        uint32_t hex = 0;
        sscanf_s(hexStr + 1, "%x", &hex);
        return ColorFromHex(hex, alpha, applyGamma);
    }

} // namespace Orion

#define HEX_TO_D2D(hex) Orion::ColorFromHex(0x##hex, 1.0f, true)

// Macro SANS correction gamma (uniquement si tu veux forcer du sRGB brut)
#define HEX_TO_D2D_SRGB(hex) Orion::ColorFromHex(0x##hex, 1.0f, false)

namespace Orion
{
    namespace Syntax
    {
        class Highlighter;
    }

    class CompletionPopup;
    class Editor;
    namespace Caret
    {
        void SetCaret(Editor &editor, int line, int column);
    }

    struct EditorTheme
    {
        // =============================
        // UI / Background
        // =============================
        // Harmonized with titlebar (#121212)
        D2D1_COLOR_F background =
            D2D1::ColorF(18.0f / 255.0f, 18.0f / 255.0f, 18.0f / 255.0f, 1.0f);

        // #ECECEC
        D2D1_COLOR_F text =
            D2D1::ColorF(0.925490f, 0.925490f, 0.925490f, 1.0f);

        // Match editor background so gutter blends with editor (#121212)
        D2D1_COLOR_F gutterBackground =
            D2D1::ColorF(18.0f / 255.0f, 18.0f / 255.0f, 18.0f / 255.0f, 1.0f);

        // #FFFFFF
        D2D1_COLOR_F caret =
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);

        // #8A8A8A
        D2D1_COLOR_F lineNumberText =
            D2D1::ColorF(0.541176f, 0.541176f, 0.541176f, 1.0f);

        // Active line uses a faint contrast aligned with the dark theme
        D2D1_COLOR_F activeLineBackground =
            D2D1::ColorF(0.145098f, 0.145098f, 0.149019f, 1.0f);

        // #1F7AEB @ 22%
        D2D1_COLOR_F selection =
            D2D1::ColorF(0.121568f, 0.478431f, 0.921568f, 0.22f);

        // =============================
        // Syntax highlighting
        // (VS Code Dark+ inspired)
        // =============================

        // #62A0E8
        D2D1_COLOR_F keyword =
            D2D1::ColorF(0.384314f, 0.627451f, 0.909804f, 1.0f);

        // #DF9E80
        D2D1_COLOR_F string =
            D2D1::ColorF(0.874510f, 0.619608f, 0.501961f, 1.0f);

        // #7FB66A
        D2D1_COLOR_F comment =
            D2D1::ColorF(0.498039f, 0.713725f, 0.415686f, 1.0f);

        // #C6D9B0
        D2D1_COLOR_F number =
            D2D1::ColorF(0.776471f, 0.850980f, 0.690196f, 1.0f);

        // #E6E39A
        D2D1_COLOR_F function =
            D2D1::ColorF(0.901961f, 0.890196f, 0.603921f, 1.0f);

        // #59D0BC
        D2D1_COLOR_F type =
            D2D1::ColorF(0.349019f, 0.815686f, 0.737255f, 1.0f);

        // #E0E0E0
        D2D1_COLOR_F operator_ =
            D2D1::ColorF(0.878431f, 0.878431f, 0.878431f, 1.0f);

        // #A8E0FF
        D2D1_COLOR_F variable =
            D2D1::ColorF(0.658824f, 0.878431f, 1.0f, 1.0f);
    };

    struct EditorMetrics
    {
        float lineHeight = 20.0f;    // correspond à font-size 14px
        float characterWidth = 8.4f; // largeur approximative d'un caractère
        float gutterWidth = 90.0f;   // garde tes 60 px
        float leftPadding = 0.0f;
        float topPadding = 1.0f; // idem
        float caretWidth = 2.0f; // idem
    };

    struct EditorState
    {
        std::vector<std::wstring> lines;
        CaretPosition caret = {0, 0};
        std::wstring filePath;
        std::wstring encoding = L"UTF-8"; // Human-readable encoding label for footer
        float scrollOffsetX = 0.0f;
        float scrollOffsetY = 0.0f;

        // Zone de rendu
        float leftEdge = 0.0f;
        float topEdge = 0.0f;
        float rightEdge = 0.0f;
        float bottomEdge = 0.0f;

        // Caret blink
        bool caretVisible = true;
        DWORD lastBlinkTime = 0;

        // Sélection
        bool hasSelection = false;
        CaretPosition selectionStart = {0, 0};
    };

    class Editor
    {
    public:
        Editor();
        ~Editor();

        void LoadFile(const std::wstring &filePath);
        void Draw(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite);
        void UpdateLayout(HWND hwnd, float left, float top, float right, float bottom);
        bool HasFile() const { return !state_.lines.empty(); }
        bool LoadCustomFont(IDWriteFactory *dwrite, const std::wstring &fontPath);

        // Événements
        void OnLeftButtonDown(HWND hwnd, POINT pt);
        void OnLeftButtonUp(HWND hwnd, POINT pt);
        void OnMouseWheel(HWND hwnd, int delta, bool ctrlPressed = false);
        void OnHorizontalWheel(HWND hwnd, int delta);
        void OnMouseMove(HWND hwnd, POINT pt);
        // Zoom methods removed
        void OnChar(wchar_t ch);
        void OnKeyDown(WPARAM key);
        // Search methods
        void ShowSearch();
        void HideSearch();
        bool IsSearchVisible() const { return searchBox_.IsVisible(); }
        SearchBox *GetSearchBox() { return &searchBox_; }
        // Create an empty buffer for a new untitled tab
        void CreateEmpty();
        // Save buffer to file (UTF-8). Returns true on success.
        bool SaveToFile(const std::wstring &filePath);
        // Accessors for external UI (footer)
        CaretPosition GetCaret() const { return state_.caret; }
        std::wstring GetFilePath() const { return state_.filePath; }
        std::wstring GetEncoding() const { return state_.encoding; }
        // Retourne le texte sélectionné (vide si pas de sélection)
        std::wstring GetSelectionText() const;
        // Indique si le buffer a du contenu non vide
        bool HasNonEmptyContent() const;
        void CopySelectionToClipboard();
        void CutSelectionToClipboard();
        void PasteFromClipboard();
        void SelectAll();
        void DeleteSelectionPublic();
        void SetSelectionStyle(Rendering::SelectionStyle style);
        void SetSelectionColor(float r, float g, float b, float a);
        void CancelInteraction();
        void Undo();

        friend void Caret::SetCaret(Editor &editor, int line, int column);

    private:
        CustomFontCollectionLoader *fontLoader_ = nullptr;
        IDWriteFactory *fontCollectionRegisteredFactory_ = nullptr;
        IDWriteFontCollection *customFontCollection_ = nullptr;
        Syntax::Highlighter *highlighter_ = nullptr;
        IDWriteTextFormat *cachedTextFormat_ = nullptr;
        void DrawActiveLine(ID2D1RenderTarget *ctx);
        void DrawSelection(ID2D1RenderTarget *ctx);
        void DrawTextContent(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite);
        void DrawCaret(ID2D1RenderTarget *ctx);

        D2D1_POINT_2F TextToScreenPosition(CaretPosition pos);
        CaretPosition ScreenToTextPosition(POINT screenPoint);

        void DeleteSelection();

        EditorState state_;
        EditorTheme theme_;
        EditorMetrics metrics_;

        bool fontMetricsInitialized_ = false;
        float fontAscent_ = 12.0f; // <-- Valeur par défaut
        float fontDescent_ = 3.0f; // <-- Valeur par défaut

        IDWriteFactory *pDWriteFactory_ = nullptr;
        Scrollbar scrollbar_;
        // Horizontal scrollbar state
        bool hScrollbarVisible_ = false;
        float hContentWidth_ = 0.0f;
        float hViewportWidth_ = 0.0f;
        float hThumbWidth_ = 0.0f;
        float hThumbPos_ = 0.0f;
        bool hIsDragging_ = false;
        int hDragStartX_ = 0;
        float hDragStartOffset_ = 0.0f;
        SearchBox searchBox_;
        CompletionPopup *completionPopup_ = nullptr;
        std::unique_ptr<Completion::CompletionService> completionService_;
        bool pendingCompletionShow_ = false;
        std::wstring pendingCompletionLabel_;
        std::wstring pendingCompletionTemplate_;

        bool suppressNextChar_ = false;
        void DrawSearchMatches(ID2D1RenderTarget *ctx);
        void DrawWhitespaceIndicators(ID2D1RenderTarget *ctx);
        std::vector<EditorState> undoStack_;
        size_t maxUndoEntries_ = 200;
        D2D1_COLOR_F GetTokenColor(::Orion::Syntax::TokenType type, const std::wstring &ext) const;

        std::unique_ptr<Geometry::IndentationHelper> indentHelper_;
        std::unique_ptr<Rendering::GuideRenderer> guideRenderer_;
        std::unique_ptr<Rendering::Selection> selection_;

        Geometry::IndentConfig GetIndentConfig() const;
        // Brush cache for syntax highlighting (color -> brush)
        ID2D1SolidColorBrush *GetOrCreateBrush(ID2D1RenderTarget *ctx, const D2D1_COLOR_F &color);
        std::vector<std::pair<D2D1_COLOR_F, ID2D1SolidColorBrush *>> brushCache_;

        std::wstring GetFileExtension() const;

        std::vector<int> CalculateHtmlDepths(int firstLine, int lastLine) const;

        DWORD lastClickTime_ = 0;
        POINT lastClickPos_ = {0, 0};
        CaretPosition lastClickTextPos_ = {-1, -1};
        int clickCount_ = 0;
    };

    class CustomTextRenderer : public IDWriteTextRenderer
    {
    public:
        CustomTextRenderer(ID2D1RenderTarget *rt, ID2D1Brush *defaultBrush)
            : renderTarget_(rt), defaultBrush_(defaultBrush), refCount_(1) {}

        // IUnknown
        ULONG STDMETHODCALLTYPE AddRef() override { return ++refCount_; }
        ULONG STDMETHODCALLTYPE Release() override
        {
            ULONG count = --refCount_;
            if (count == 0)
                delete this;
            return count;
        }
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override
        {
            if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWriteTextRenderer) || riid == __uuidof(IDWritePixelSnapping))
            {
                *ppvObject = this;
                AddRef();
                return S_OK;
            }
            return E_NOINTERFACE;
        }

        // IDWritePixelSnapping
        HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void *, BOOL *isDisabled) override
        {
            *isDisabled = FALSE;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetCurrentTransform(void *, DWRITE_MATRIX *transform) override
        {
            renderTarget_->GetTransform(reinterpret_cast<D2D1_MATRIX_3X2_F *>(transform));
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void *, FLOAT *pixelsPerDip) override
        {
            FLOAT dpiX = 96.0f, dpiY = 96.0f;
            if (renderTarget_)
            {
                renderTarget_->GetDpi(&dpiX, &dpiY);
            }
            *pixelsPerDip = dpiX / 96.0f;
            return S_OK;
        }

        // IDWriteTextRenderer
        HRESULT STDMETHODCALLTYPE DrawGlyphRun(
            void *,
            FLOAT baselineOriginX,
            FLOAT baselineOriginY,
            DWRITE_MEASURING_MODE measuringMode,
            DWRITE_GLYPH_RUN const *glyphRun,
            DWRITE_GLYPH_RUN_DESCRIPTION const *,
            IUnknown *clientDrawingEffect) override
        {
            ID2D1Brush *brush = defaultBrush_;
            ID2D1Brush *effectBrush = nullptr;
            if (clientDrawingEffect)
            {
                if (SUCCEEDED(clientDrawingEffect->QueryInterface(__uuidof(ID2D1Brush), (void **)&effectBrush)) && effectBrush)
                {
                    brush = effectBrush;
                }
            }

            renderTarget_->DrawGlyphRun(
                D2D1::Point2F(baselineOriginX, baselineOriginY),
                glyphRun,
                brush,
                measuringMode);

            if (effectBrush)
                effectBrush->Release();
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE DrawUnderline(void *, FLOAT, FLOAT, DWRITE_UNDERLINE const *, IUnknown *) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE DrawStrikethrough(void *, FLOAT, FLOAT, DWRITE_STRIKETHROUGH const *, IUnknown *) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE DrawInlineObject(void *, FLOAT, FLOAT, IDWriteInlineObject *, BOOL, BOOL, IUnknown *) override { return S_OK; }

    private:
        ID2D1RenderTarget *renderTarget_;
        ID2D1Brush *defaultBrush_;
        ULONG refCount_;
    };

} // namespace Orion