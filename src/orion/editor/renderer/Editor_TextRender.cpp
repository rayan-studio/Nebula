#include "orion/editor/Editor.h"
#include "../../completion/popup/Popup.h"
#include "orion/editor/internal/Editor_Internal.h"
#include "../../geometry/CppBraceGuides.h"
#include "../../../utils/logger/Logger.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "orion/geometry/TextColumns.h"
#include "orion/geometry/IndentationHelper.h"
#include "orion/rendering/GuideRenderer.h"
#include "ui/theme/Theme.h"

// Ensure Windows min/max macros don't interfere with std::min/std::max
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

namespace Orion
{
    // ----- helpers -----
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

    ID2D1SolidColorBrush *Editor::GetOrCreateBrush(ID2D1RenderTarget *ctx, const D2D1_COLOR_F &color)
    {
        // Try to find an existing brush with identical color
        for (auto &p : brushCache_)
        {
            D2D1_COLOR_F c = p.first;
            if (memcmp(&c, &color, sizeof(D2D1_COLOR_F)) == 0)
                return p.second;
        }

        ID2D1SolidColorBrush *b = nullptr;
        if (ctx)
            ctx->CreateSolidColorBrush(color, &b);

        if (b)
            brushCache_.push_back(std::make_pair(color, b));

        return b;
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

    void Editor::DrawWhitespaceIndicators(ID2D1RenderTarget *ctx)
    {
        EnsureFoldLineMaps();
        int visibleCount = GetVisibleLineCount();
        int firstVisibleLine = (int)(state_.scrollOffsetY / metrics_.lineHeight);
        int lastVisibleLine = (int)((state_.scrollOffsetY + (state_.bottomEdge - state_.topEdge)) / metrics_.lineHeight) + 1;

        firstVisibleLine = (std::max)(0, firstVisibleLine);
        lastVisibleLine = (std::min)(visibleCount, lastVisibleLine);

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

        for (int v = firstVisibleLine; v < lastVisibleLine; ++v)
        {
            int i = VisibleLineToActualLine(v);
            float lineY = state_.topEdge + (v * metrics_.lineHeight) - state_.scrollOffsetY;
            const std::wstring &line = state_.lines[i];
            if (line.empty())
                continue;

            const float cw = metrics_.characterWidth;
            const int tabSize = GetIndentConfig().tabSize;

            int visualCol = 0;
            for (size_t col = 0; col < line.size(); ++col)
            {
                wchar_t ch = line[col];
                int visualColNext = Orion::Geometry::AdvanceVisualCol(visualCol, ch, tabSize);

                float charX = contentLeft - state_.scrollOffsetX + (visualCol * cw);
                float nextX = contentLeft - state_.scrollOffsetX + (visualColNext * cw);

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
        }

        for (size_t idx = 0; idx < colCount; ++idx)
            if (colBrushesArr[idx])
                colBrushesArr[idx]->Release();
        if (fallbackBrush)
            fallbackBrush->Release();
    }

    void Editor::DrawTextContent(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite)
    {
        EnsureFoldLineMaps();
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
            const float tabStop = metrics_.characterWidth * (float)GetIndentConfig().tabSize;
            format->SetIncrementalTabStop(tabStop);
        }

        int visibleCount = GetVisibleLineCount();
        int firstVisibleLine = (int)(state_.scrollOffsetY / metrics_.lineHeight);
        int lastVisibleLine = (int)((state_.scrollOffsetY + (state_.bottomEdge - state_.topEdge)) / metrics_.lineHeight) + 1;

        firstVisibleLine = (std::max)(0, firstVisibleLine);
        lastVisibleLine = (std::min)(visibleCount, lastVisibleLine);

        float contentLeft = state_.leftEdge + metrics_.gutterWidth + metrics_.leftPadding;

        std::wstring ext = GetFileExtension();

        ID2D1SolidColorBrush *defaultBrush = nullptr;
        ctx->CreateSolidColorBrush(theme_.text, &defaultBrush);

        // ------------------------------------------------------------
        // ✅ Markdown state (fix bug when scrolling into middle of file)
        // ------------------------------------------------------------
        bool mdInCodeBlock = false;
        std::wstring mdFenceLang;

        if (ext == L".md" && highlighter_)
        {
            // Recompute fence state up to the first visible line so scrolling doesn't break markdown
            int firstActualLine = VisibleLineToActualLine(firstVisibleLine);
            int scanEnd = (std::min)(firstActualLine, (int)state_.lines.size());
            for (int li = 0; li < scanEnd; ++li)
            {
                highlighter_->AdvanceMarkdownState(state_.lines[li], mdInCodeBlock, mdFenceLang);
            }
        }

        // Guides
        if (collapsedFolds_.empty())
        {
            int firstActualLine = VisibleLineToActualLine(firstVisibleLine);
            int lastActualLine = VisibleLineToActualLine((std::max)(firstVisibleLine, lastVisibleLine - 1));
            Geometry::IndentConfig indentConfig = GetIndentConfig();
            indentHelper_ = std::make_unique<Geometry::IndentationHelper>(indentConfig);

            Rendering::GuideStyle guideStyle;
            if (ext == L".html" || ext == L".htm")
            {
                guideStyle.normalColor = D2D1::ColorF(theme_.keyword.r, theme_.keyword.g, theme_.keyword.b, appliedUiThemeIsLight_ ? 0.45f : 0.70f);
                guideStyle.activeColor = D2D1::ColorF(theme_.type.r, theme_.type.g, theme_.type.b, appliedUiThemeIsLight_ ? 0.60f : 0.85f);
                guideStyle.topMargin = 0.06f;
                guideStyle.bottomMargin = 0.06f;
            }
            else
            {
                guideStyle.normalColor = D2D1::ColorF(theme_.lineNumberText.r, theme_.lineNumberText.g, theme_.lineNumberText.b, appliedUiThemeIsLight_ ? 0.45f : 0.65f);
                guideStyle.activeColor = D2D1::ColorF(theme_.text.r, theme_.text.g, theme_.text.b, appliedUiThemeIsLight_ ? 0.50f : 0.80f);
            }
            guideStyle.lineWidth = 0.75f;

            guideRenderer_ = std::make_unique<Rendering::GuideRenderer>(indentConfig, guideStyle);

            Rendering::GuideRenderContext renderCtx;
            renderCtx.contentLeft = contentLeft;
            renderCtx.topEdge = state_.topEdge;
            renderCtx.scrollOffsetY = state_.scrollOffsetY;
            renderCtx.scrollOffsetX = state_.scrollOffsetX;
            renderCtx.lineHeight = metrics_.lineHeight;
            renderCtx.charWidth = metrics_.characterWidth;
            renderCtx.lines = &state_.lines;
            renderCtx.firstVisibleLine = firstActualLine;
            renderCtx.lastVisibleLine = lastActualLine + 1;
            renderCtx.dwriteFactory = pDWriteFactory_;
            renderCtx.textFormat = format;

            if (ext == L".c" || ext == L".cpp" || ext == L".h" || ext == L".hpp")
            {
                // Scan slightly beyond the visible window to capture opening braces
                const int margin = 200;
                int scanFirst = (std::max)(0, firstActualLine - margin);
                int scanLast = (std::min)((int)state_.lines.size(), lastActualLine + 1 + margin);

                (void)scanFirst;
                (void)scanLast;

                auto guides = Orion::Geometry::ComputeCppBraceGuides(
                    state_.lines,
                    GetIndentConfig().tabSize,
                    state_.caret.line,
                    0,
                    (int)state_.lines.size());

                guideRenderer_->DrawGuides(ctx, guides, renderCtx);
            }
        }

        std::vector<Editor::Diagnostic> diagnostics = GetDiagnostics();
        ID2D1SolidColorBrush *errorBrush = nullptr;
        ID2D1SolidColorBrush *warningBrush = nullptr;
        ctx->CreateSolidColorBrush(D2D1::ColorF(0.90f, 0.25f, 0.25f, 1.0f), &errorBrush);
        ctx->CreateSolidColorBrush(D2D1::ColorF(0.95f, 0.65f, 0.25f, 1.0f), &warningBrush);

        for (int v = firstVisibleLine; v < lastVisibleLine; ++v)
        {
            int i = VisibleLineToActualLine(v);
            float lineY = state_.topEdge + (v * metrics_.lineHeight) - state_.scrollOffsetY;
            const std::wstring &line = state_.lines[i];
            int collapsedEnd = -1;
            bool isCollapsedLine = IsCollapsedFoldStart(i, &collapsedEnd);
            std::wstring displayLine = line;
            if (isCollapsedLine)
                displayLine += L"  ...";

            if (displayLine.empty())
                continue;

            IDWriteTextLayout *layout = nullptr;

            if (pDWriteFactory_ && format)
            {
                HRESULT hr = pDWriteFactory_->CreateTextLayout(
                    displayLine.c_str(),
                    (UINT32)displayLine.size(),
                    format,
                    10000.0f,
                    metrics_.lineHeight,
                    &layout);

                if (SUCCEEDED(hr) && layout)
                {
                    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

                    IDWriteTypography *typography = nullptr;
                    if (SUCCEEDED(pDWriteFactory_->CreateTypography(&typography)) && typography)
                    {
                        // JetBrains Mono ligatures (liga/calt/dlig)
                        DWRITE_FONT_FEATURE features[] = {
                            {DWRITE_MAKE_FONT_FEATURE_TAG('l', 'i', 'g', 'a'), 1},
                            {DWRITE_MAKE_FONT_FEATURE_TAG('c', 'a', 'l', 't'), 1},
                            {DWRITE_MAKE_FONT_FEATURE_TAG('d', 'l', 'i', 'g'), 1},
                        };

                        for (auto &f : features)
                            typography->AddFontFeature(f);

                        DWRITE_TEXT_RANGE fullRange = {0, (UINT32)displayLine.size()};
                        layout->SetTypography(typography, fullRange);
                        typography->Release();
                    }
                }
            }

            if (layout)
            {
                DWRITE_TEXT_METRICS tm = {};
                float verticalOffset = 0.0f;
                if (SUCCEEDED(layout->GetMetrics(&tm)))
                {
                    if (metrics_.lineHeight > tm.height)
                        verticalOffset = (metrics_.lineHeight - tm.height) * 0.5f;
                }

                // Apply syntax highlighting: set drawing effects per token on the layout
                if (highlighter_ && !isCollapsedLine)
                {
                    std::vector<::Orion::Syntax::Token> tokens;

                    // ✅ Markdown uses stateful tokenization driven by editor-side state
                    if (ext == L".md")
                        tokens = highlighter_->TokenizeMarkdownLine(line, mdInCodeBlock, mdFenceLang);
                    else
                        tokens = highlighter_->TokenizeLine(line, ext);

                    for (const auto &t : tokens)
                    {
                        D2D1_COLOR_F col = GetTokenColor(t.type, ext);
                        ID2D1SolidColorBrush *b = GetOrCreateBrush(ctx, col);
                        if (b)
                        {
                            DWRITE_TEXT_RANGE r;
                            r.startPosition = (UINT32)t.start;
                            r.length = (UINT32)t.length;
                            layout->SetDrawingEffect((IUnknown *)b, r);

                            // ------------------------------------------------------------
                            // Optional Markdown styling (bold/italic/underline)
                            // ------------------------------------------------------------
                            if (ext == L".md")
                            {
                                switch (t.type)
                                {
                                case ::Orion::Syntax::TokenType::MarkdownHeading:
                                    layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, r);
                                    break;
                                case ::Orion::Syntax::TokenType::MarkdownStrong:
                                    layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, r);
                                    break;
                                case ::Orion::Syntax::TokenType::MarkdownEmphasis:
                                    layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, r);
                                    break;
                                case ::Orion::Syntax::TokenType::MarkdownLinkText:
                                    layout->SetUnderline(TRUE, r);
                                    break;
                                default:
                                    break;
                                }
                            }
                        }
                    }
                }

                ID2D1SolidColorBrush *drawBrush = nullptr;
                ctx->CreateSolidColorBrush(theme_.text, &drawBrush);

                if (drawBrush)
                {
                    CustomTextRenderer renderer(ctx, drawBrush);

                    float drawX = contentLeft - state_.scrollOffsetX;
                    float drawY = lineY + verticalOffset;

                    layout->Draw(nullptr, &renderer, drawX, drawY);
                    drawBrush->Release();

                    // Ctrl+hover definition underline
                    if (defHoverActive_ && defHoverLine_ == i && defHoverStart_ >= 0 && defHoverEnd_ > defHoverStart_)
                    {
                        UINT32 count = 0;
                        UINT32 length = (UINT32)(defHoverEnd_ - defHoverStart_);
                        layout->HitTestTextRange((UINT32)defHoverStart_, length, drawX, drawY, nullptr, 0, &count);
                        if (count > 0)
                        {
                            std::vector<DWRITE_HIT_TEST_METRICS> metrics(count);
                            layout->HitTestTextRange((UINT32)defHoverStart_, length, drawX, drawY, metrics.data(), count, &count);

                            D2D1_COLOR_F linkColor = UI::Theme::GetPalette().sidebarIndicator;
                            ID2D1SolidColorBrush *linkBrush = nullptr;
                            ctx->CreateSolidColorBrush(linkColor, &linkBrush);
                            if (linkBrush)
                            {
                                for (UINT32 mi = 0; mi < count; ++mi)
                                {
                                    const auto &m = metrics[mi];
                                    float x1 = m.left;
                                    float x2 = m.left + m.width;
                                    float y = m.top + m.height - 1.0f;
                                    ctx->DrawLine(D2D1::Point2F(x1, y), D2D1::Point2F(x2, y), linkBrush, 1.0f);
                                }
                                linkBrush->Release();
                            }
                        }
                    }

                    // Diagnostics: underline ranges with a subtle wavy line
                    if (!diagnostics.empty())
                    {
                        for (const auto &diag : diagnostics)
                        {
                            if (diag.line != i)
                                continue;

                            int start = diag.startCol;
                            int length = diag.endCol - diag.startCol;
                            if (length <= 0)
                                length = 1;

                            UINT32 count = 0;
                            layout->HitTestTextRange((UINT32)start, (UINT32)length, drawX, drawY, nullptr, 0, &count);
                            if (count == 0)
                                continue;

                            std::vector<DWRITE_HIT_TEST_METRICS> metrics(count);
                            layout->HitTestTextRange((UINT32)start, (UINT32)length, drawX, drawY, metrics.data(), count, &count);

                            ID2D1SolidColorBrush *lineBrush = diag.isError ? errorBrush : warningBrush;
                            if (!lineBrush)
                                continue;

                            for (UINT32 mi = 0; mi < count; ++mi)
                            {
                                const auto &m = metrics[mi];
                                float x1 = m.left;
                                float x2 = m.left + m.width;
                                float y = m.top + m.height - 1.0f;
                                float amp = 1.2f;
                                float step = 4.0f;
                                bool up = true;

                                for (float x = x1; x < x2; x += step)
                                {
                                    float nx = (x + step > x2) ? x2 : x + step;
                                    float y1 = y + (up ? -amp : amp);
                                    float y2 = y + (up ? amp : -amp);
                                    ctx->DrawLine(D2D1::Point2F(x, y1), D2D1::Point2F(nx, y2), lineBrush, 1.2f);
                                    up = !up;
                                }
                            }
                        }
                    }
                }

                layout->Release();
                layout = nullptr;
            }
        }

        if (defaultBrush)
            defaultBrush->Release();
        if (tmpFmt)
            tmpFmt->Release();
        if (errorBrush)
            errorBrush->Release();
        if (warningBrush)
            warningBrush->Release();
    }

    D2D1_COLOR_F Editor::GetTokenColor(::Orion::Syntax::TokenType type, const std::wstring &ext) const
    {
        // (copié exactement de ton code)
        if (ext == L".py")
        {
            switch (type)
            {
            case ::Orion::Syntax::TokenType::Keyword:
                return D2D1::ColorF(0.9f, 0.4f, 0.4f);
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
            case ::Orion::Syntax::TokenType::MarkdownLinkText:
            case ::Orion::Syntax::TokenType::MarkdownLinkUrl:
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
            case ::Orion::Syntax::TokenType::Keyword:
                return D2D1::ColorF(0.36f, 0.8f, 0.45f);
            case ::Orion::Syntax::TokenType::Type:
                return D2D1::ColorF(0.4f, 0.85f, 0.95f);
            case ::Orion::Syntax::TokenType::String:
                return D2D1::ColorF(0.6f, 0.9f, 0.6f);
            case ::Orion::Syntax::TokenType::Comment:
                return D2D1::ColorF(0.5f, 0.5f, 0.5f);
            default:
                return theme_.text;
            }
        }
        else if (ext == L".c" || ext == L".cpp" || ext == L".h" || ext == L".hpp")
        {
            switch (type)
            {
            case ::Orion::Syntax::TokenType::Keyword:
                return D2D1::ColorF(0.86f, 0.58f, 0.22f);
            case ::Orion::Syntax::TokenType::Type:
                return D2D1::ColorF(0.4f, 0.8f, 1.0f);
            case ::Orion::Syntax::TokenType::Function:
                return theme_.function;
            case ::Orion::Syntax::TokenType::Macro:
                return D2D1::ColorF(0.90f, 0.70f, 0.40f);
            case ::Orion::Syntax::TokenType::Variable:
                return theme_.variable;
            case ::Orion::Syntax::TokenType::String:
                return D2D1::ColorF(0.56f, 0.87f, 0.56f);
            case ::Orion::Syntax::TokenType::Comment:
                return D2D1::ColorF(0.5f, 0.5f, 0.5f);
            case ::Orion::Syntax::TokenType::Preprocessor:
                return D2D1::ColorF(0.9f, 0.7f, 0.4f);
            default:
                return theme_.text;
            }
        }

        switch (type)
        {
        case ::Orion::Syntax::TokenType::Keyword:
            return D2D1::ColorF(0.86f, 0.58f, 0.22f);
        case ::Orion::Syntax::TokenType::Type:
            return D2D1::ColorF(0.4f, 0.8f, 1.0f);
        case ::Orion::Syntax::TokenType::Function:
            return theme_.function;
        case ::Orion::Syntax::TokenType::Macro:
            return D2D1::ColorF(0.90f, 0.70f, 0.40f);
        case ::Orion::Syntax::TokenType::Variable:
            return theme_.variable;
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
}
