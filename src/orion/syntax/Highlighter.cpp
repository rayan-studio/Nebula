#include "Highlighter.h"
#include <unordered_set>
#include <cwctype>

using namespace Orion::Syntax;

Highlighter::Highlighter() {}

static bool IsIdentifierStart(wchar_t c) { return (iswalpha(c) || c == L'_'); }
static bool IsIdentifierPart(wchar_t c) { return (iswalnum(c) || c == L'_'); }

static const std::unordered_set<std::wstring> cppKeywords = {
    L"if", L"else", L"for", L"while", L"return", L"switch", L"case",
    L"break", L"continue", L"goto", L"constexpr", L"const", L"static",
    L"inline", L"virtual", L"override", L"public", L"private", L"protected",
    L"namespace", L"using", L"class", L"struct", L"enum", L"template",
    L"typename", L"this", L"new", L"delete", L"try", L"catch", L"throw"
};

static const std::unordered_set<std::wstring> cppTypes = {
    L"int", L"float", L"double", L"char", L"bool", L"void", L"long", L"short",
    L"size_t", L"std", L"auto"
};

static const std::unordered_set<std::wstring> jsKeywords = {
    L"if", L"else", L"for", L"while", L"return", L"switch", L"case",
    L"break", L"continue", L"function", L"var", L"let", L"const",
    L"class", L"extends", L"new", L"delete", L"try", L"catch", L"throw",
    L"async", L"await", L"import", L"from", L"export", L"default",
    L"typeof", L"instanceof", L"in", L"of"
};

static const std::unordered_set<std::wstring> jsTypes = {
    L"Number", L"String", L"Boolean", L"Object", L"Array", L"Map", L"Set", L"Promise"
};

static const std::unordered_set<std::wstring> rustKeywords = {
    L"fn", L"let", L"mut", L"pub", L"impl", L"trait", L"enum", L"struct",
    L"use", L"crate", L"mod", L"self", L"super", L"as", L"where", L"match",
    L"if", L"else", L"loop", L"for", L"while", L"return", L"break", L"continue",
    L"const", L"static", L"unsafe", L"async", L"await"
};

static const std::unordered_set<std::wstring> rustTypes = {
    L"i32", L"i64", L"u32", L"u64", L"usize", L"isize", L"f32", L"f64", L"String", L"Vec"
};

std::vector<Token> Highlighter::TokenizeLine(const std::wstring &line, const std::wstring &ext)
{
    std::vector<Token> out;

    // Markdown handling
    if (ext == L".md")
    {
        // Code fence start/end
        if (line.rfind(L"```", 0) == 0) {
            mdInCodeBlock_ = !mdInCodeBlock_;
            out.push_back({0, (int)line.size(), TokenType::MarkdownCode});
            return out;
        }
        if (mdInCodeBlock_) {
            out.push_back({0, (int)line.size(), TokenType::MarkdownCode});
            return out;
        }
        // Heading
        size_t pos = 0;
        while (pos < line.size() && line[pos] == L'#') pos++;
        if (pos > 0 && pos < line.size() && line[pos] == L' ') {
            out.push_back({0, (int)line.size(), TokenType::MarkdownHeading});
            return out;
        }
        // Fallback: plain text
        out.push_back({0, (int)line.size(), TokenType::Normal});
        return out;
    }

    // Choose keyword/type sets based on extension
    const std::unordered_set<std::wstring> *kwSet = &cppKeywords;
    const std::unordered_set<std::wstring> *typeSet = &cppTypes;
    bool treatBacktickAsString = false;
    if (ext == L".js" || ext == L".ts") {
        kwSet = &jsKeywords;
        typeSet = &jsTypes;
        treatBacktickAsString = true;
    } else if (ext == L".rs") {
        kwSet = &rustKeywords;
        typeSet = &rustTypes;
    }

    // C/C++ style tokenization (basic)
    int i = 0;
    int n = (int)line.size();
    while (i < n)
    {
        wchar_t c = line[i];

        // whitespace — preserve as Normal tokens so spacing is kept when drawing
        if (iswspace(c)) {
            int start = i;
            while (i < n && iswspace(line[i])) i++;
            out.push_back({start, i - start, TokenType::Normal});
            continue;
        }

        // single-line comment (//)
        if (c == L'/' && i+1 < n && line[i+1] == L'/') {
            out.push_back({i, n - i, TokenType::Comment});
            break;
        }

        // block comment start (/* ... */) - consume to line end or end token
        if (c == L'/' && i+1 < n && line[i+1] == L'*') {
            int start = i;
            i += 2;
            while (i+1 < n && !(line[i] == L'*' && line[i+1] == L'/')) i++;
            if (i+1 < n) i += 2; // skip closing */
            else i = n;
            out.push_back({start, i - start, TokenType::Comment});
            continue;
        }

        // string literal: handle " ' and (optionally) ` for JS
        if (c == L'"' || c == L'\'' || (treatBacktickAsString && c == L'`')) {
            int start = i;
            wchar_t quote = c;
            i++;
            while (i < n) {
                if (line[i] == L'\\' && i+1 < n) { i += 2; continue; }
                if (line[i] == quote) { i++; break; }
                i++;
            }
            out.push_back({start, i - start, TokenType::String});
            continue;
        }

        // preprocessor
        if (c == L'#' && i == 0) {
            out.push_back({0, n, TokenType::Preprocessor});
            break;
        }

        // number
        if (iswdigit(c)) {
            int start = i;
            while (i < n && (iswdigit(line[i]) || line[i] == L'.' || line[i]==L'x' || line[i]==L'X' || iswxdigit(line[i]))) i++;
            out.push_back({start, i - start, TokenType::Number});
            continue;
        }

        // identifier/keyword/type
        if (IsIdentifierStart(c)) {
            int start = i;
            i++;
            while (i < n && IsIdentifierPart(line[i])) i++;
            std::wstring word = line.substr(start, i - start);
            std::wstring low = word; for (auto &ch : low) ch = towlower(ch);
            if (kwSet->find(low) != kwSet->end()) {
                out.push_back({start, (int)(i-start), TokenType::Keyword});
            } else if (typeSet->find(low) != typeSet->end()) {
                out.push_back({start, (int)(i-start), TokenType::Type});
            } else {
                out.push_back({start, (int)(i-start), TokenType::Normal});
            }
            continue;
        }

        // punctuation/others - render as normal
        out.push_back({i, 1, TokenType::Normal});
        i++;
    }

    return out;
}
