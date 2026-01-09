#include "Selection.h"
#include <algorithm>
#include <dwrite.h>

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

namespace Orion
{
    namespace Rendering
    {
        // Styles de coins pour la sélection multi-ligne
        enum class CornerStyle
        {
            EXTERN,  // Coin arrondi normal (vers l'extérieur)
            INTERN,  // Coin arrondi inversé (vers l'intérieur)
            FLAT     // Pas de coin arrondi (ligne continue)
        };

        struct CornerStyles
        {
            CornerStyle topLeft = CornerStyle::EXTERN;
            CornerStyle bottomLeft = CornerStyle::EXTERN;
            CornerStyle topRight = CornerStyle::EXTERN;
            CornerStyle bottomRight = CornerStyle::EXTERN;
        };

        Selection::Selection(const SelectionConfig &cfg) : cfg_(cfg) {}
        Selection::~Selection() {}

        void Selection::Draw(ID2D1RenderTarget *ctx, const std::vector<D2D1_ROUNDED_RECT> &regions)
        {
            if (!ctx || regions.empty())
                return;

            ID2D1SolidColorBrush *brush = nullptr;
            ctx->CreateSolidColorBrush(cfg_.color, &brush);
            if (!brush)
                return;

            // Analyser les coins pour une sélection multi-ligne
            std::vector<CornerStyles> cornerStyles = AnalyzeCornerStyles(regions);

            for (size_t i = 0; i < regions.size(); ++i)
            {
                const auto &r = regions[i];
                const auto &corners = cornerStyles[i];

                if (cfg_.style == SelectionStyle::Rectangle)
                {
                    ctx->FillRectangle(r.rect, brush);
                }
                else if (cfg_.style == SelectionStyle::RoundedSmart)
                {
                    // Dessiner avec coins intelligents
                    DrawSmartRoundedSelection(ctx, brush, r.rect, corners);
                }
                else
                {
                    // RoundedSimple : tous les coins arrondis
                    ctx->FillRoundedRectangle(&r, brush);
                }
            }

            brush->Release();
        }

        std::vector<CornerStyles> Selection::AnalyzeCornerStyles(const std::vector<D2D1_ROUNDED_RECT> &regions)
        {
            std::vector<CornerStyles> result(regions.size());
            
            if (regions.size() <= 1)
            {
                // Une seule ligne : coins arrondis partout
                if (!regions.empty())
                {
                    result[0].topLeft = CornerStyle::EXTERN;
                    result[0].bottomLeft = CornerStyle::EXTERN;
                    result[0].topRight = CornerStyle::EXTERN;
                    result[0].bottomRight = CornerStyle::EXTERN;
                }
                return result;
            }

            const float epsilon = 1.0f;

            for (size_t i = 0; i < regions.size(); ++i)
            {
                const auto &curr = regions[i].rect;
                CornerStyles &style = result[i];

                // Par défaut : pas de coins arrondis
                style.topLeft = CornerStyle::FLAT;
                style.bottomLeft = CornerStyle::FLAT;
                style.topRight = CornerStyle::FLAT;
                style.bottomRight = CornerStyle::FLAT;

                // ===== Analyser le coin TOP-LEFT =====
                if (i == 0)
                {
                    // Première ligne : toujours arrondi en haut à gauche
                    style.topLeft = CornerStyle::EXTERN;
                }
                else
                {
                    const auto &prev = regions[i - 1].rect;
                    
                    // Le coin gauche de la ligne actuelle est-il à l'extérieur de la ligne précédente ?
                    if (curr.left < prev.left - epsilon)
                    {
                        // Plus à gauche que la ligne précédente : coin externe
                        style.topLeft = CornerStyle::EXTERN;
                    }
                    else if (curr.left > prev.left + epsilon && curr.left < prev.right - epsilon)
                    {
                        // À l'intérieur de la ligne précédente : coin inversé
                        style.topLeft = CornerStyle::INTERN;
                    }
                    // Sinon : aligné ou juste à côté → FLAT
                }

                // ===== Analyser le coin TOP-RIGHT =====
                if (i == 0)
                {
                    // Première ligne : toujours arrondi en haut à droite
                    style.topRight = CornerStyle::EXTERN;
                }
                else
                {
                    const auto &prev = regions[i - 1].rect;
                    
                    // Le coin droit de la ligne actuelle est-il à l'extérieur de la ligne précédente ?
                    if (curr.right > prev.right + epsilon)
                    {
                        // Plus à droite que la ligne précédente : coin externe
                        style.topRight = CornerStyle::EXTERN;
                    }
                    else if (curr.right < prev.right - epsilon && curr.right > prev.left + epsilon)
                    {
                        // À l'intérieur de la ligne précédente : coin inversé
                        style.topRight = CornerStyle::INTERN;
                    }
                    // Sinon : aligné → FLAT
                }

                // ===== Analyser le coin BOTTOM-LEFT =====
                if (i == regions.size() - 1)
                {
                    // Dernière ligne : toujours arrondi en bas à gauche
                    style.bottomLeft = CornerStyle::EXTERN;
                }
                else
                {
                    const auto &next = regions[i + 1].rect;
                    
                    // Le coin gauche de la ligne actuelle est-il à l'extérieur de la ligne suivante ?
                    if (curr.left < next.left - epsilon)
                    {
                        // Plus à gauche que la ligne suivante : coin externe
                        style.bottomLeft = CornerStyle::EXTERN;
                    }
                    else if (next.left > curr.left + epsilon && next.left < curr.right - epsilon)
                    {
                        // La ligne suivante commence à l'intérieur : coin inversé
                        style.bottomLeft = CornerStyle::INTERN;
                    }
                    // Sinon : aligné → FLAT
                }

                // ===== Analyser le coin BOTTOM-RIGHT =====
                if (i == regions.size() - 1)
                {
                    // Dernière ligne : toujours arrondi en bas à droite
                    style.bottomRight = CornerStyle::EXTERN;
                }
                else
                {
                    const auto &next = regions[i + 1].rect;
                    
                    // Le coin droit de la ligne actuelle est-il à l'extérieur de la ligne suivante ?
                    if (curr.right > next.right + epsilon)
                    {
                        // Plus à droite que la ligne suivante : coin externe
                        style.bottomRight = CornerStyle::EXTERN;
                    }
                    else if (next.right < curr.right - epsilon && next.right > curr.left + epsilon)
                    {
                        // La ligne suivante finit à l'intérieur : coin inversé
                        style.bottomRight = CornerStyle::INTERN;
                    }
                    // Sinon : aligné → FLAT
                }
            }

            return result;
        }

        void Selection::DrawSmartRoundedSelection(
            ID2D1RenderTarget *ctx,
            ID2D1SolidColorBrush *brush,
            const D2D1_RECT_F &rect,
            const CornerStyles &corners)
        {
            const float radius = cfg_.cornerRadius;

            // Obtenir la factory D2D1
            ID2D1Factory *factory = nullptr;
            ctx->GetFactory(&factory);
            if (!factory)
            {
                ctx->FillRectangle(rect, brush);
                return;
            }

            // Créer la géométrie du rectangle avec coins personnalisés
            ID2D1PathGeometry *pathGeometry = nullptr;
            ID2D1GeometrySink *sink = nullptr;

            if (FAILED(factory->CreatePathGeometry(&pathGeometry)) || !pathGeometry)
            {
                factory->Release();
                ctx->FillRectangle(rect, brush);
                return;
            }

            if (FAILED(pathGeometry->Open(&sink)) || !sink)
            {
                pathGeometry->Release();
                factory->Release();
                ctx->FillRectangle(rect, brush);
                return;
            }

            sink->SetFillMode(D2D1_FILL_MODE_WINDING);

            // Extension pour éviter les gaps (plus petite que le radius)
            const float overlap = 0.5f;

            // Calculer les points de départ selon les styles de coins
            float startX = rect.left;
            float startY = rect.top;

            if (corners.topLeft == CornerStyle::EXTERN)
            {
                startX = rect.left + radius;
            }
            else if (corners.topLeft == CornerStyle::INTERN)
            {
                startX = rect.left - overlap;
                startY = rect.top - overlap;
            }

            sink->BeginFigure(D2D1::Point2F(startX, startY), D2D1_FIGURE_BEGIN_FILLED);

            // ===== Ligne supérieure jusqu'au coin supérieur droit =====
            float topRightX = rect.right;
            float topRightY = rect.top;

            if (corners.topRight == CornerStyle::EXTERN)
            {
                topRightX = rect.right - radius;
            }
            else if (corners.topRight == CornerStyle::INTERN)
            {
                topRightX = rect.right + overlap;
                topRightY = rect.top - overlap;
            }

            sink->AddLine(D2D1::Point2F(topRightX, topRightY));

            // ===== Coin supérieur droit =====
            if (corners.topRight == CornerStyle::EXTERN)
            {
                sink->AddArc(D2D1::ArcSegment(
                    D2D1::Point2F(rect.right, rect.top + radius),
                    D2D1::SizeF(radius, radius),
                    0.0f,
                    D2D1_SWEEP_DIRECTION_CLOCKWISE,
                    D2D1_ARC_SIZE_SMALL));
            }
            else if (corners.topRight == CornerStyle::FLAT)
            {
                sink->AddLine(D2D1::Point2F(rect.right, rect.top));
            }
            else // INTERN
            {
                sink->AddLine(D2D1::Point2F(rect.right + overlap, rect.top + overlap));
            }

            // ===== Ligne droite jusqu'au coin inférieur droit =====
            float bottomRightY = rect.bottom;
            float bottomRightX = rect.right;

            if (corners.bottomRight == CornerStyle::EXTERN)
            {
                bottomRightY = rect.bottom - radius;
            }
            else if (corners.bottomRight == CornerStyle::INTERN)
            {
                bottomRightX = rect.right + overlap;
                bottomRightY = rect.bottom + overlap;
            }

            sink->AddLine(D2D1::Point2F(bottomRightX, bottomRightY));

            // ===== Coin inférieur droit =====
            if (corners.bottomRight == CornerStyle::EXTERN)
            {
                sink->AddArc(D2D1::ArcSegment(
                    D2D1::Point2F(rect.right - radius, rect.bottom),
                    D2D1::SizeF(radius, radius),
                    0.0f,
                    D2D1_SWEEP_DIRECTION_CLOCKWISE,
                    D2D1_ARC_SIZE_SMALL));
            }
            else if (corners.bottomRight == CornerStyle::FLAT)
            {
                sink->AddLine(D2D1::Point2F(rect.right, rect.bottom));
            }
            else // INTERN
            {
                sink->AddLine(D2D1::Point2F(rect.right - overlap, rect.bottom + overlap));
            }

            // ===== Ligne inférieure jusqu'au coin inférieur gauche =====
            float bottomLeftX = rect.left;
            float bottomLeftY = rect.bottom;

            if (corners.bottomLeft == CornerStyle::EXTERN)
            {
                bottomLeftX = rect.left + radius;
            }
            else if (corners.bottomLeft == CornerStyle::INTERN)
            {
                bottomLeftX = rect.left - overlap;
                bottomLeftY = rect.bottom + overlap;
            }

            sink->AddLine(D2D1::Point2F(bottomLeftX, bottomLeftY));

            // ===== Coin inférieur gauche =====
            if (corners.bottomLeft == CornerStyle::EXTERN)
            {
                sink->AddArc(D2D1::ArcSegment(
                    D2D1::Point2F(rect.left, rect.bottom - radius),
                    D2D1::SizeF(radius, radius),
                    0.0f,
                    D2D1_SWEEP_DIRECTION_CLOCKWISE,
                    D2D1_ARC_SIZE_SMALL));
            }
            else if (corners.bottomLeft == CornerStyle::FLAT)
            {
                sink->AddLine(D2D1::Point2F(rect.left, rect.bottom));
            }
            else // INTERN
            {
                sink->AddLine(D2D1::Point2F(rect.left - overlap, rect.bottom - overlap));
            }

            // ===== Ligne gauche jusqu'au point de départ =====
            float topLeftY = rect.top;
            float topLeftX = rect.left;

            if (corners.topLeft == CornerStyle::EXTERN)
            {
                topLeftY = rect.top + radius;
            }
            else if (corners.topLeft == CornerStyle::INTERN)
            {
                topLeftX = rect.left - overlap;
                topLeftY = rect.top + overlap;
            }

            sink->AddLine(D2D1::Point2F(topLeftX, topLeftY));

            // ===== Coin supérieur gauche =====
            if (corners.topLeft == CornerStyle::EXTERN)
            {
                sink->AddArc(D2D1::ArcSegment(
                    D2D1::Point2F(startX, startY),
                    D2D1::SizeF(radius, radius),
                    0.0f,
                    D2D1_SWEEP_DIRECTION_CLOCKWISE,
                    D2D1_ARC_SIZE_SMALL));
            }

            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            
            if (SUCCEEDED(sink->Close()))
            {
                // Activer l'antialiasing pour un rendu ultra-lisse
                D2D1_ANTIALIAS_MODE oldMode = ctx->GetAntialiasMode();
                ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                ctx->FillGeometry(pathGeometry, brush);
                ctx->SetAntialiasMode(oldMode);
            }

            // Dessiner les coins inversés (INTERN) par-dessus
            ID2D1SolidColorBrush *bgBrush = nullptr;
            D2D1_COLOR_F bgColor = D2D1::ColorF(0x1e1e1e);
            ctx->CreateSolidColorBrush(bgColor, &bgBrush);

            if (bgBrush)
            {
                if (corners.topLeft == CornerStyle::INTERN)
                    DrawInverseCorner(ctx, factory, bgBrush, rect.left, rect.top, radius, 0);
                if (corners.topRight == CornerStyle::INTERN)
                    DrawInverseCorner(ctx, factory, bgBrush, rect.right, rect.top, radius, 1);
                if (corners.bottomRight == CornerStyle::INTERN)
                    DrawInverseCorner(ctx, factory, bgBrush, rect.right, rect.bottom, radius, 2);
                if (corners.bottomLeft == CornerStyle::INTERN)
                    DrawInverseCorner(ctx, factory, bgBrush, rect.left, rect.bottom, radius, 3);

                bgBrush->Release();
            }

            sink->Release();
            pathGeometry->Release();
            factory->Release();
        }

        void Selection::DrawRoundedCorner(
            ID2D1RenderTarget *ctx,
            ID2D1SolidColorBrush *brush,
            float x, float y, float radius, float angleOffset)
        {
            // Cette méthode n'est plus utilisée - on utilise DrawSmartRoundedSelection à la place
            (void)ctx;
            (void)brush;
            (void)x;
            (void)y;
            (void)radius;
            (void)angleOffset;
        }

        void Selection::DrawInverseCorner(
            ID2D1RenderTarget *ctx,
            ID2D1Factory *factory,
            ID2D1SolidColorBrush *bgBrush,
            float x, float y, float radius, int corner)
        {
            // Coins inversés : créer une géométrie précise avec léger overlap
            ID2D1PathGeometry *pathGeometry = nullptr;
            ID2D1GeometrySink *sink = nullptr;

            if (FAILED(factory->CreatePathGeometry(&pathGeometry)) || !pathGeometry)
                return;

            if (FAILED(pathGeometry->Open(&sink)) || !sink)
            {
                pathGeometry->Release();
                return;
            }

            sink->SetFillMode(D2D1_FILL_MODE_WINDING);

            // Augmenter légèrement la taille pour couvrir les gaps
            const float size = radius + 0.5f;

            switch (corner)
            {
            case 0: // Top-left inverse (coin en haut à gauche)
                sink->BeginFigure(D2D1::Point2F(x - size, y - size), D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1::Point2F(x + 0.5f, y - size));
                sink->AddLine(D2D1::Point2F(x + 0.5f, y + 0.5f));
                sink->AddArc(D2D1::ArcSegment(
                    D2D1::Point2F(x - size, y + 0.5f),
                    D2D1::SizeF(size, size),
                    0.0f,
                    D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                    D2D1_ARC_SIZE_SMALL));
                break;

            case 1: // Top-right inverse
                sink->BeginFigure(D2D1::Point2F(x - 0.5f, y - size), D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1::Point2F(x + size, y - size));
                sink->AddLine(D2D1::Point2F(x + size, y + 0.5f));
                sink->AddArc(D2D1::ArcSegment(
                    D2D1::Point2F(x - 0.5f, y + 0.5f),
                    D2D1::SizeF(size, size),
                    0.0f,
                    D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                    D2D1_ARC_SIZE_SMALL));
                break;

            case 2: // Bottom-right inverse
                sink->BeginFigure(D2D1::Point2F(x + size, y - 0.5f), D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1::Point2F(x + size, y + size));
                sink->AddLine(D2D1::Point2F(x - 0.5f, y + size));
                sink->AddArc(D2D1::ArcSegment(
                    D2D1::Point2F(x - 0.5f, y - 0.5f),
                    D2D1::SizeF(size, size),
                    0.0f,
                    D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                    D2D1_ARC_SIZE_SMALL));
                break;

            case 3: // Bottom-left inverse
                sink->BeginFigure(D2D1::Point2F(x + 0.5f, y + size), D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1::Point2F(x - size, y + size));
                sink->AddLine(D2D1::Point2F(x - size, y - 0.5f));
                sink->AddArc(D2D1::ArcSegment(
                    D2D1::Point2F(x + 0.5f, y - 0.5f),
                    D2D1::SizeF(size, size),
                    0.0f,
                    D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                    D2D1_ARC_SIZE_SMALL));
                break;
            }

            sink->EndFigure(D2D1_FIGURE_END_CLOSED);

            if (SUCCEEDED(sink->Close()))
            {
                // Activer l'antialiasing pour un rendu ultra-lisse
                D2D1_ANTIALIAS_MODE oldMode = ctx->GetAntialiasMode();
                ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                ctx->FillGeometry(pathGeometry, bgBrush);
                ctx->SetAntialiasMode(oldMode);
            }

            sink->Release();
            pathGeometry->Release();
        }

        // Helper function pour GetXPositionForColumn (inchangée)
        static float GetXPositionForColumn(
            const std::wstring &line,
            int column,
            float contentLeft,
            float scrollOffsetX,
            int tabSize,
            float charWidth,
            IDWriteFactory *dwriteFactory,
            IDWriteTextFormat *textFormat)
        {
            if (line.empty())
                return contentLeft - scrollOffsetX;

            if (!dwriteFactory || !textFormat)
                return contentLeft - scrollOffsetX;

            IDWriteTextLayout *layout = nullptr;
            HRESULT hr = dwriteFactory->CreateTextLayout(
                line.c_str(),
                (UINT32)line.size(),
                textFormat,
                10000.0f,
                100.0f,
                &layout);

            if (FAILED(hr) || !layout)
                return contentLeft - scrollOffsetX;

            float xPos = 0.0f;
            UINT32 textPos = std::min((UINT32)column, (UINT32)line.size());

            FLOAT caretX = 0.0f;
            FLOAT caretY = 0.0f;
            DWRITE_HIT_TEST_METRICS hitMetrics = {};

            bool success = true;
            __try
            {
                layout->HitTestTextPosition(textPos, FALSE, &caretX, &caretY, &hitMetrics);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                success = false;
            }

            if (success)
            {
                DWRITE_OVERHANG_METRICS om = {};
                if (SUCCEEDED(layout->GetOverhangMetrics(&om)))
                {
                    caretX -= om.left;
                }
                xPos = caretX;
            }

            layout->Release();
            return contentLeft + xPos - scrollOffsetX;
        }

        std::vector<D2D1_ROUNDED_RECT> Selection::CalculateRegions(
            CaretPosition start,
            CaretPosition end,
            const std::vector<std::wstring> &lines,
            float contentLeft,
            float topEdge,
            float scrollOffsetX,
            float scrollOffsetY,
            float lineHeight,
            int tabSize,
            float characterWidth,
            float cornerRadius,
            IDWriteFactory *dwriteFactory,
            IDWriteTextFormat *textFormat)
        {
            std::vector<D2D1_ROUNDED_RECT> out;

            auto earlier = start;
            auto later = end;
            if (earlier.line > later.line || (earlier.line == later.line && earlier.column > later.column))
            {
                std::swap(earlier, later);
            }

            int sLine = std::max(0, earlier.line);
            int eLine = std::max(0, later.line);

            for (int line = sLine; line <= eLine; ++line)
            {
                float y = topEdge + (line * lineHeight) - scrollOffsetY;

                const std::wstring empty;
                const std::wstring &ln = (line >= 0 && line < (int)lines.size()) ? lines[line] : empty;
                int lineLen = (int)ln.size();

                float x1 = contentLeft - scrollOffsetX;
                float x2 = contentLeft - scrollOffsetX;

                if (sLine == eLine)
                {
                    int sc = std::max(0, std::min((int)earlier.column, lineLen));
                    int ec = std::max(0, std::min((int)later.column, lineLen));

                    x1 = GetXPositionForColumn(ln, sc, contentLeft, scrollOffsetX, tabSize, characterWidth, dwriteFactory, textFormat);
                    x2 = GetXPositionForColumn(ln, ec, contentLeft, scrollOffsetX, tabSize, characterWidth, dwriteFactory, textFormat);
                }
                else if (line == sLine)
                {
                    int sc = std::max(0, std::min((int)earlier.column, lineLen));

                    x1 = GetXPositionForColumn(ln, sc, contentLeft, scrollOffsetX, tabSize, characterWidth, dwriteFactory, textFormat);
                    x2 = GetXPositionForColumn(ln, lineLen, contentLeft, scrollOffsetX, tabSize, characterWidth, dwriteFactory, textFormat);
                }
                else if (line == eLine)
                {
                    int ec = std::max(0, std::min((int)later.column, lineLen));

                    x1 = contentLeft - scrollOffsetX;
                    x2 = GetXPositionForColumn(ln, ec, contentLeft, scrollOffsetX, tabSize, characterWidth, dwriteFactory, textFormat);
                }
                else
                {
                    x1 = contentLeft - scrollOffsetX;
                    x2 = GetXPositionForColumn(ln, lineLen, contentLeft, scrollOffsetX, tabSize, characterWidth, dwriteFactory, textFormat);
                }

                if (x2 < x1)
                    std::swap(x1, x2);

                if (x2 - x1 < 1.0f)
                    x2 = x1 + 1.0f;

                D2D1_ROUNDED_RECT rr;
                rr.rect = D2D1_RECT_F{x1, y, x2, y + lineHeight};
                rr.radiusX = cornerRadius;
                rr.radiusY = cornerRadius;
                out.push_back(rr);
            }

            return out;
        }

    } // namespace Rendering
} // namespace Orion