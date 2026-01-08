#pragma once

#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <vector>
#include <memory>
#include <windows.h>
#include "ui/components/scrollbar/Scrollbar.h"
#include "orion/font/CustomFontLoader.h"
#include "orion/SearchBox.h"
#include "syntax/Highlighter.h"
#include "orion/completion/CompletionService.h"
// New helpers
#include "geometry/IndentationHelper.h"
#include "rendering/GuideRenderer.h"

namespace Orion
{

    struct CaretPosition
    {
        int line;
        int column;
    };

    namespace Syntax
    {
        class Highlighter;
    }

    class CompletionPopup;

    struct EditorTheme
    {
        D2D1_COLOR_F background = D2D1::ColorF(0.06f, 0.06f, 0.06f, 1.0f);
        D2D1_COLOR_F text = D2D1::ColorF(0.86f, 0.86f, 0.86f, 1.0f);
        D2D1_COLOR_F gutterBackground = D2D1::ColorF(0.09f, 0.09f, 0.09f, 1.0f);
        D2D1_COLOR_F caret = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
        D2D1_COLOR_F lineNumberText = D2D1::ColorF(0.5f, 0.5f, 0.5f, 1.0f);
        D2D1_COLOR_F activeLineBackground = D2D1::ColorF(0.14f, 0.14f, 0.14f, 1.0f);
        D2D1_COLOR_F selection = D2D1::ColorF(0.2f, 0.4f, 0.8f, 0.5f);
    };

    struct EditorMetrics
    {
        float lineHeight = 20.0f;    // correspond à font-size 14px
        float characterWidth = 8.4f; // largeur approximative d'un caractère
        float gutterWidth = 60.0f;   // garde tes 60 px
        float leftPadding = 12.0f;   // ça peut rester pareil
        float topPadding = 1.0f;     // idem
        float caretWidth = 2.0f;     // idem
    };

    struct EditorState
    {
        std::vector<std::wstring> lines;
        CaretPosition caret = {0, 0};
        std::wstring filePath;
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
        void Draw(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite, HWND hwnd);
        void UpdateLayout(HWND hwnd, float left, float top, float right, float bottom);
        bool HasFile() const { return !state_.lines.empty(); }
        bool LoadCustomFont(IDWriteFactory *dwrite, const std::wstring &fontPath);

        // Événements
        void OnLeftButtonDown(HWND hwnd, POINT pt);
        void OnLeftButtonUp(HWND hwnd, POINT pt);
        void OnMouseWheel(HWND hwnd, int delta, bool ctrlPressed = false);
        void OnHorizontalWheel(HWND hwnd, int delta);
        void OnMouseMove(HWND hwnd, POINT pt);
        // Zoom methods
        void ZoomIn();
        void ZoomOut();
        void ResetZoom();
        float GetZoomLevel() const { return zoomLevel_; }
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
        void SetCaret(int line, int column);
        std::wstring GetFilePath() const { return state_.filePath; }
        // Retourne le texte sélectionné (vide si pas de sélection)
        std::wstring GetSelectionText() const;
        // Indique si le buffer a du contenu non vide
        bool HasNonEmptyContent() const;
        // Clipboard operations
        void CopySelectionToClipboard();
        void CutSelectionToClipboard();
        void PasteFromClipboard();
        // Selection helpers
        void SelectAll();
        void DeleteSelectionPublic();
        // Cancel any ongoing mouse interaction (selection/drag)
        void CancelInteraction();
        // Undo support
        void Undo();

    private:
        CustomFontCollectionLoader *fontLoader_ = nullptr;
        // Factory pointer used to unregister the collection loader safely
        IDWriteFactory *fontCollectionRegisteredFactory_ = nullptr;
        IDWriteFontCollection *customFontCollection_ = nullptr;
        // Syntax highlighter (moved to separate module)
        Syntax::Highlighter *highlighter_ = nullptr;
        IDWriteTextFormat *cachedTextFormat_ = nullptr;
        void DrawActiveLine(ID2D1RenderTarget *ctx);
        void DrawSelection(ID2D1RenderTarget *ctx);
        void DrawGutter(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite);
        void DrawLineNumbers(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite);
        void DrawTextContent(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite);
        void DrawCaret(ID2D1RenderTarget *ctx);

        D2D1_POINT_2F TextToScreenPosition(CaretPosition pos);
        CaretPosition ScreenToTextPosition(POINT screenPoint);

        void UpdateCaretBlink();
        void DeleteSelection();
        void EnsureCaretVisible();

        EditorState state_;
        EditorTheme theme_;
        EditorMetrics metrics_;

        // Zoom
        float zoomLevel_ = 1.0f; // 1.0 = 100%, 1.5 = 150%, etc.
        const float MIN_ZOOM = 0.5f;
        const float MAX_ZOOM = 3.0f;
        const float ZOOM_STEP = 0.1f;

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
        // Search functionality
        SearchBox searchBox_;
        // Completion popup for simple suggestions (includes)
        CompletionPopup *completionPopup_ = nullptr;
        std::unique_ptr<Completion::CompletionService> completionService_;
        // Pending completion template (used for triggers like '!')
        bool pendingCompletionShow_ = false;
        std::wstring pendingCompletionLabel_;
        std::wstring pendingCompletionTemplate_;

        // When a key combo like Ctrl+Space is handled in OnKeyDown, Windows still
        // generates a WM_CHAR for the space. Use this flag to suppress the
        // following OnChar space insertion.
        bool suppressNextChar_ = false;

        // Header index and include suggestion responsibilities moved to CppCompletionProvider
        // Helper method pour dessiner les matches de recherche
        void DrawSearchMatches(ID2D1RenderTarget *ctx);
        // Simple undo stack (stores previous EditorState snapshots)
        std::vector<EditorState> undoStack_;
        size_t maxUndoEntries_ = 200;
        // Color mapping helper for syntax tokens per file extension
        D2D1_COLOR_F GetTokenColor(::Orion::Syntax::TokenType type, const std::wstring &ext) const;

        // ✨ NOUVEAUX MEMBRES
        std::unique_ptr<Geometry::IndentationHelper> indentHelper_;
        std::unique_ptr<Rendering::GuideRenderer> guideRenderer_;

        // Configuration d'indentation
        Geometry::IndentConfig GetIndentConfig() const;

        // Helper pour extraire l'extension du fichier
        std::wstring GetFileExtension() const;

        // Calculer les profondeurs HTML (garde l'ancienne logique)
        std::vector<int> CalculateHtmlDepths(int firstLine, int lastLine) const;
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
            DWRITE_MEASURING_MODE,
            DWRITE_GLYPH_RUN const *glyphRun,
            DWRITE_GLYPH_RUN_DESCRIPTION const *,
            IUnknown *clientDrawingEffect) override
        {
            ID2D1Brush *brush = defaultBrush_;
            if (clientDrawingEffect)
            {
                clientDrawingEffect->QueryInterface(&brush);
            }

            renderTarget_->DrawGlyphRun(
                D2D1::Point2F(baselineOriginX, baselineOriginY),
                glyphRun,
                brush,
                DWRITE_MEASURING_MODE_NATURAL);

            if (brush != defaultBrush_)
                brush->Release();
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