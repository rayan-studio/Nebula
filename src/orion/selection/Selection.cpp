#include "Selection.h"
#include "../caret/CaretPosition.h"

#include "../geometry/TextColumns.h" // <-- IMPORTANT pour Orion::Geometry::AdvanceVisualCol

#include <algorithm>
#include <dwrite.h>
#include <string>
#include <vector>

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
        // ============================================================
        // Helper : colonne logique -> position X écran (tabs inclus)
        // ============================================================
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
            (void)dwriteFactory;
            (void)textFormat;

            const float baseX = contentLeft - scrollOffsetX;

            if (column <= 0 || line.empty())
                return baseX;

            int col = column;
            if (col > (int)line.size())
                col = (int)line.size();

            // logique -> visuel (tabs)
            int visualCol = 0;
            for (int i = 0; i < col; ++i)
            {
                visualCol = Orion::Geometry::AdvanceVisualCol(visualCol, line[i], tabSize);
            }

            return baseX + (visualCol * charWidth);
        }

        // ============================================================
        // Selection
        // ============================================================

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
                    DrawSmartRoundedSelection(ctx, brush, r.rect, regions, i);
                }
                else
                {
                    ctx->FillRoundedRectangle(&r, brush);
                }
            }

            brush->Release();
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

            CaretPosition a = start;
            CaretPosition b = end;
            if (a.line > b.line || (a.line == b.line && a.column > b.column))
                std::swap(a, b);

            if (lines.empty())
                return out;

            int sLine = (std::max)(0, a.line);
            int eLine = (std::max)(0, b.line);

            sLine = (std::min)(sLine, (int)lines.size() - 1);
            eLine = (std::min)(eLine, (int)lines.size() - 1);

            for (int line = sLine; line <= eLine; ++line)
            {
                float y = topEdge + (line * lineHeight) - scrollOffsetY;

                const std::wstring &ln = lines[line];
                int lineLen = (int)ln.size();

                float x1 = contentLeft - scrollOffsetX;
                float x2 = contentLeft - scrollOffsetX;

                if (sLine == eLine)
                {
                    int sc = (std::max)(0, (std::min)(a.column, lineLen));
                    int ec = (std::max)(0, (std::min)(b.column, lineLen));

                    x1 = GetXPositionForColumn(ln, sc, contentLeft, scrollOffsetX, tabSize, characterWidth, dwriteFactory, textFormat);
                    x2 = GetXPositionForColumn(ln, ec, contentLeft, scrollOffsetX, tabSize, characterWidth, dwriteFactory, textFormat);
                }
                else if (line == sLine)
                {
                    int sc = (std::max)(0, (std::min)(a.column, lineLen));
                    x1 = GetXPositionForColumn(ln, sc, contentLeft, scrollOffsetX, tabSize, characterWidth, dwriteFactory, textFormat);
                    x2 = GetXPositionForColumn(ln, lineLen, contentLeft, scrollOffsetX, tabSize, characterWidth, dwriteFactory, textFormat);
                }
                else if (line == eLine)
                {
                    int ec = (std::max)(0, (std::min)(b.column, lineLen));
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

                // petit padding style VSCode
                x2 += characterWidth * 0.3f;

                // largeur minimum (ligne vide etc.)
                const float minWidth = characterWidth * 0.5f;
                if (x2 - x1 < minWidth)
                    x2 = x1 + minWidth;

                D2D1_ROUNDED_RECT rr{};
                rr.rect = D2D1_RECT_F{x1, y, x2, y + lineHeight};
                rr.radiusX = cornerRadius;
                rr.radiusY = cornerRadius;
                out.push_back(rr);
            }

            return out;
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

            bool isEmptyLine = (rect.right - rect.left) < (epsilon * 2.0f);

            bool roundTopLeft = false;
            bool roundTopRight = false;
            bool roundBottomLeft = false;
            bool roundBottomRight = false;

            if (currentIndex == 0)
            {
                roundTopLeft = true;
                roundTopRight = true;
            }
            else
            {
                const auto &prevRect = allRegions[currentIndex - 1].rect;
                bool prevIsEmpty = (prevRect.right - prevRect.left) < (epsilon * 2.0f);

                if (rect.left < prevRect.left - epsilon || (isEmptyLine && !prevIsEmpty))
                    roundTopLeft = true;

                if (rect.right > prevRect.right + epsilon || (isEmptyLine && !prevIsEmpty))
                    roundTopRight = true;
            }

            if (currentIndex == allRegions.size() - 1)
            {
                roundBottomLeft = true;
                roundBottomRight = true;
            }
            else
            {
                const auto &nextRect = allRegions[currentIndex + 1].rect;
                bool nextIsEmpty = (nextRect.right - nextRect.left) < (epsilon * 2.0f);

                if (rect.left < nextRect.left - epsilon || (isEmptyLine && !nextIsEmpty))
                    roundBottomLeft = true;

                if (rect.right > nextRect.right + epsilon || (isEmptyLine && !nextIsEmpty))
                    roundBottomRight = true;
            }

            if (!roundTopLeft && !roundTopRight && !roundBottomLeft && !roundBottomRight)
            {
                ctx->FillRectangle(rect, brush);
                return;
            }

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

            D2D1_POINT_2F startPoint = roundTopLeft
                                           ? D2D1::Point2F(rect.left, rect.top + radius)
                                           : D2D1::Point2F(rect.left, rect.top);

            sink->BeginFigure(startPoint, D2D1_FIGURE_BEGIN_FILLED);

            if (roundTopLeft)
            {
                sink->AddArc(D2D1::ArcSegment(
                    D2D1::Point2F(rect.left + radius, rect.top),
                    D2D1::SizeF(radius, radius),
                    0.0f,
                    D2D1_SWEEP_DIRECTION_CLOCKWISE,
                    D2D1_ARC_SIZE_SMALL));
            }

            D2D1_POINT_2F topRight = roundTopRight
                                         ? D2D1::Point2F(rect.right - radius, rect.top)
                                         : D2D1::Point2F(rect.right, rect.top);
            sink->AddLine(topRight);

            if (roundTopRight)
            {
                sink->AddArc(D2D1::ArcSegment(
                    D2D1::Point2F(rect.right, rect.top + radius),
                    D2D1::SizeF(radius, radius),
                    0.0f,
                    D2D1_SWEEP_DIRECTION_CLOCKWISE,
                    D2D1_ARC_SIZE_SMALL));
            }

            D2D1_POINT_2F bottomRight = roundBottomRight
                                            ? D2D1::Point2F(rect.right, rect.bottom - radius)
                                            : D2D1::Point2F(rect.right, rect.bottom);
            sink->AddLine(bottomRight);

            if (roundBottomRight)
            {
                sink->AddArc(D2D1::ArcSegment(
                    D2D1::Point2F(rect.right - radius, rect.bottom),
                    D2D1::SizeF(radius, radius),
                    0.0f,
                    D2D1_SWEEP_DIRECTION_CLOCKWISE,
                    D2D1_ARC_SIZE_SMALL));
            }

            D2D1_POINT_2F bottomLeft = roundBottomLeft
                                           ? D2D1::Point2F(rect.left + radius, rect.bottom)
                                           : D2D1::Point2F(rect.left, rect.bottom);
            sink->AddLine(bottomLeft);

            if (roundBottomLeft)
            {
                sink->AddArc(D2D1::ArcSegment(
                    D2D1::Point2F(rect.left, rect.bottom - radius),
                    D2D1::SizeF(radius, radius),
                    0.0f,
                    D2D1_SWEEP_DIRECTION_CLOCKWISE,
                    D2D1_ARC_SIZE_SMALL));
            }

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

    } // namespace Rendering
} // namespace Orion
