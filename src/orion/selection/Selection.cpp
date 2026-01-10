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

            for (size_t i = 0; i < regions.size(); ++i)
            {
                const auto &r = regions[i];

                if (cfg_.style == SelectionStyle::Rectangle)
                {
                    ctx->FillRectangle(r.rect, brush);
                }
                else if (cfg_.style == SelectionStyle::RoundedSmart)
                {
                    // Dessiner avec coins intelligents
                    DrawSmartRoundedSelection(ctx, brush, r.rect, regions, i);
                }
                else
                {
                    // RoundedSimple : tous les coins arrondis
                    ctx->FillRoundedRectangle(&r, brush);
                }
            }

            brush->Release();
        }

        void Selection::DrawSmartRoundedSelection(
            ID2D1RenderTarget *ctx,
            ID2D1SolidColorBrush *brush,
            const D2D1_RECT_F &rect,
            const std::vector<D2D1_ROUNDED_RECT> &allRegions,
            size_t currentIndex)
        {
            const float radius = cfg_.cornerRadius;
            const float epsilon = 1.0f;

            // Détecter si c'est une ligne vide (très petite)
            bool isEmptyLine = (rect.right - rect.left) < (epsilon * 2.0f);

            // Déterminer quels coins doivent être arrondis
            bool roundTopLeft = false;
            bool roundTopRight = false;
            bool roundBottomLeft = false;
            bool roundBottomRight = false;

            // Première ligne : coins du haut arrondis
            if (currentIndex == 0)
            {
                roundTopLeft = true;
                roundTopRight = true;
            }
            else
            {
                const auto &prevRect = allRegions[currentIndex - 1].rect;
                bool prevIsEmpty = (prevRect.right - prevRect.left) < (epsilon * 2.0f);
                
                // Coin haut-gauche arrondi si on dépasse à gauche OU si c'est une ligne vide après une ligne normale
                if (rect.left < prevRect.left - epsilon || (isEmptyLine && !prevIsEmpty))
                {
                    roundTopLeft = true;
                }
                
                // Coin haut-droit arrondi si on dépasse à droite OU si c'est une ligne vide après une ligne normale
                if (rect.right > prevRect.right + epsilon || (isEmptyLine && !prevIsEmpty))
                {
                    roundTopRight = true;
                }
            }

            // Dernière ligne : coins du bas arrondis
            if (currentIndex == allRegions.size() - 1)
            {
                roundBottomLeft = true;
                roundBottomRight = true;
            }
            else
            {
                const auto &nextRect = allRegions[currentIndex + 1].rect;
                bool nextIsEmpty = (nextRect.right - nextRect.left) < (epsilon * 2.0f);
                
                // Coin bas-gauche arrondi si on dépasse à gauche OU si c'est une ligne vide avant une ligne normale
                if (rect.left < nextRect.left - epsilon || (isEmptyLine && !nextIsEmpty))
                {
                    roundBottomLeft = true;
                }
                
                // Coin bas-droit arrondi si on dépasse à droite OU si c'est une ligne vide avant une ligne normale
                if (rect.right > nextRect.right + epsilon || (isEmptyLine && !nextIsEmpty))
                {
                    roundBottomRight = true;
                }
            }

            // Si aucun coin n'est arrondi, dessiner un rectangle simple
            if (!roundTopLeft && !roundTopRight && !roundBottomLeft && !roundBottomRight)
            {
                ctx->FillRectangle(rect, brush);
                return;
            }

            // Créer une géométrie avec coins sélectifs
            ID2D1Factory *factory = nullptr;
            ctx->GetFactory(&factory);
            if (!factory)
            {
                ctx->FillRectangle(rect, brush);
                return;
            }

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

            // Commencer au coin supérieur gauche
            D2D1_POINT_2F startPoint = roundTopLeft 
                ? D2D1::Point2F(rect.left, rect.top + radius)
                : D2D1::Point2F(rect.left, rect.top);

            sink->BeginFigure(startPoint, D2D1_FIGURE_BEGIN_FILLED);

            // Coin supérieur gauche
            if (roundTopLeft)
            {
                sink->AddArc(D2D1::ArcSegment(
                    D2D1::Point2F(rect.left + radius, rect.top),
                    D2D1::SizeF(radius, radius),
                    0.0f,
                    D2D1_SWEEP_DIRECTION_CLOCKWISE,
                    D2D1_ARC_SIZE_SMALL));
            }

            // Ligne supérieure
            D2D1_POINT_2F topRight = roundTopRight
                ? D2D1::Point2F(rect.right - radius, rect.top)
                : D2D1::Point2F(rect.right, rect.top);
            sink->AddLine(topRight);

            // Coin supérieur droit
            if (roundTopRight)
            {
                sink->AddArc(D2D1::ArcSegment(
                    D2D1::Point2F(rect.right, rect.top + radius),
                    D2D1::SizeF(radius, radius),
                    0.0f,
                    D2D1_SWEEP_DIRECTION_CLOCKWISE,
                    D2D1_ARC_SIZE_SMALL));
            }

            // Ligne droite
            D2D1_POINT_2F bottomRight = roundBottomRight
                ? D2D1::Point2F(rect.right, rect.bottom - radius)
                : D2D1::Point2F(rect.right, rect.bottom);
            sink->AddLine(bottomRight);

            // Coin inférieur droit
            if (roundBottomRight)
            {
                sink->AddArc(D2D1::ArcSegment(
                    D2D1::Point2F(rect.right - radius, rect.bottom),
                    D2D1::SizeF(radius, radius),
                    0.0f,
                    D2D1_SWEEP_DIRECTION_CLOCKWISE,
                    D2D1_ARC_SIZE_SMALL));
            }

            // Ligne inférieure
            D2D1_POINT_2F bottomLeft = roundBottomLeft
                ? D2D1::Point2F(rect.left + radius, rect.bottom)
                : D2D1::Point2F(rect.left, rect.bottom);
            sink->AddLine(bottomLeft);

            // Coin inférieur gauche
            if (roundBottomLeft)
            {
                sink->AddArc(D2D1::ArcSegment(
                    D2D1::Point2F(rect.left, rect.bottom - radius),
                    D2D1::SizeF(radius, radius),
                    0.0f,
                    D2D1_SWEEP_DIRECTION_CLOCKWISE,
                    D2D1_ARC_SIZE_SMALL));
            }

            // Ligne gauche (retour au début)
            sink->AddLine(startPoint);

            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            
            if (SUCCEEDED(sink->Close()))
            {
                D2D1_ANTIALIAS_MODE oldMode = ctx->GetAntialiasMode();
                ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                ctx->FillGeometry(pathGeometry, brush);
                ctx->SetAntialiasMode(oldMode);
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
            (void)ctx;
            (void)factory;
            (void)bgBrush;
            (void)x;
            (void)y;
            (void)radius;
            (void)corner;
        }

        // Helper function pour GetXPositionForColumn
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

                // Ajouter un petit padding à droite pour que ça respire (comme VSCode)
                const float rightPadding = characterWidth * 0.3f;
                x2 += rightPadding;

                // Largeur minimale pour les lignes vides ou très petites
                const float minWidth = characterWidth * 0.5f; // Demi-caractère minimum
                if (x2 - x1 < minWidth)
                    x2 = x1 + minWidth;

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