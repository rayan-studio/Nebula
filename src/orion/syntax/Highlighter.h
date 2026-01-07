#pragma once

#include <string>
#include <vector>
#include <d2d1.h>

namespace Orion::Syntax
{
    enum class TokenType {
        Normal,
        Keyword,
        Type,
        String,
        Comment,
        Number,
        Preprocessor,
        MarkdownHeading,
        MarkdownCode,
        MarkdownLink
    };

    struct Token {
        int start;
        int length;
        TokenType type;
    };

    // Highlighter is stateful for markdown code fence tracking
    class Highlighter {
    public:
        Highlighter();

        // Tokenize a single line given the file extension (including the leading dot, e.g. ".cpp")
        std::vector<Token> TokenizeLine(const std::wstring &line, const std::wstring &ext);

    private:
        bool mdInCodeBlock_ = false;
    };

}
