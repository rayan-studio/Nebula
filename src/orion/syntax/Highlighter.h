#pragma once

#include <string>
#include <vector>

namespace Orion::Syntax
{
    enum class TokenType
    {
        Normal,
        Keyword,
        Type,
        Function,
        Macro,
        Variable,
        String,
        Comment,
        Number,
        Preprocessor,

        // Markdown
        MarkdownHeading,
        MarkdownCodeFence,
        MarkdownCode,
        MarkdownInlineCode,
        MarkdownEmphasis,
        MarkdownStrong,
        MarkdownLinkText,
        MarkdownLinkUrl,
        MarkdownListMarker,
        MarkdownQuote,
        MarkdownHr,
        MarkdownEscape
    };

    struct Token
    {
        int start;
        int length;
        TokenType type;
    };

    class Highlighter
    {
    public:
        Highlighter();

        // Tokenizer "normal" (code, html, etc.)
        std::vector<Token> TokenizeLine(const std::wstring &line, const std::wstring &ext);

        // Markdown: état géré côté Editor (stateless => pas de bug au scroll)
        void AdvanceMarkdownState(const std::wstring &line, bool &inCodeBlock, std::wstring &fenceLang) const;

        std::vector<Token> TokenizeMarkdownLine(const std::wstring &line, bool &inCodeBlock, std::wstring &fenceLang);
    };
}
