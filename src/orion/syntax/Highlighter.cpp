#include "Highlighter.h"

#include <unordered_set>
#include <cwctype>
#include <algorithm>

using namespace Orion::Syntax;

Highlighter::Highlighter() {}

// -----------------------------
// Common identifier helpers
// -----------------------------
static bool IsIdentifierStart(wchar_t c) { return (iswalpha(c) || c == L'_'); }
static bool IsIdentifierPart(wchar_t c)  { return (iswalnum(c) || c == L'_'); }

// -----------------------------
// Keyword/type sets
// -----------------------------
static const std::unordered_set<std::wstring> cppKeywords = {
    L"if", L"else", L"for", L"while", L"return", L"switch", L"case",
    L"break", L"continue", L"goto", L"constexpr", L"const", L"static",
    L"inline", L"virtual", L"override", L"public", L"private", L"protected",
    L"namespace", L"using", L"class", L"struct", L"enum", L"template",
    L"typename", L"this", L"new", L"delete", L"try", L"catch", L"throw",
    L"nullptr", L"true", L"false",
    // Primitive types as keywords (matches CLion / JetBrains coloring)
    L"int", L"float", L"double", L"char", L"bool", L"void", L"long", L"short",
    L"unsigned", L"signed", L"auto", L"decltype", L"explicit", L"extern",
    L"register", L"volatile", L"mutable", L"noexcept", L"final", L"default"
};

static const std::unordered_set<std::wstring> cppTypes = {
    L"size_t", L"std",
    L"string", L"wstring", L"string_view", L"vector", L"array", L"deque", L"list", L"forward_list",
    L"map", L"set", L"unordered_map", L"unordered_set", L"pair", L"tuple", L"optional", L"variant",
    L"regex", L"smatch", L"wregex", L"basic_regex",
    L"unique_ptr", L"shared_ptr", L"weak_ptr", L"function",
    L"filesystem", L"path"
};

static const std::unordered_set<std::wstring> cppKnownTypes = {
    L"D2D1_RECT_F", L"D2D1_POINT_2F", L"D2D1_ELLIPSE", L"D2D1_ROUNDED_RECT",
    L"D2D1_MATRIX_3X2_F", L"D2D1_COLOR_F", L"D2D1_SIZE_F", L"D2D1_SIZE_U",
    L"ID2D1RenderTarget", L"ID2D1SolidColorBrush", L"ID2D1Bitmap", L"ID2D1Brush",
    L"IDWriteFactory", L"IDWriteTextFormat", L"IDWriteTextLayout", L"IDWriteTypography",
    L"HWND", L"RECT", L"POINT", L"SIZE", L"HBITMAP", L"HICON", L"HBRUSH",
    L"UINT", L"DWORD", L"LPARAM", L"WPARAM", L"LRESULT", L"HANDLE", L"HRESULT",
    L"std", L"std::string", L"std::wstring", L"std::vector", L"std::map", L"std::unordered_map",
    L"NSVGimage", L"NSVGrasterizer"
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

// ============================================================
// Markdown helpers (stateless, state is passed by the editor)
// ============================================================
static bool md_StartsWith(const std::wstring &s, const std::wstring &p) { return s.rfind(p, 0) == 0; }
static bool md_IsSpace(wchar_t c) { return iswspace(c) != 0; }

static std::wstring md_Trim(const std::wstring &s)
{
    size_t a = 0, b = s.size();
    while (a < b && md_IsSpace(s[a])) a++;
    while (b > a && md_IsSpace(s[b - 1])) b--;
    return s.substr(a, b - a);
}

static std::wstring md_ParseFenceLang(const std::wstring &line)
{
    std::wstring rest = md_Trim(line.substr(3));
    size_t sp = rest.find_first_of(L" \t");
    if (sp != std::wstring::npos) rest = rest.substr(0, sp);
    for (auto &c : rest) c = towlower(c);
    return rest;
}

void Highlighter::AdvanceMarkdownState(const std::wstring &line, bool &inCodeBlock, std::wstring &fenceLang) const
{
    if (md_StartsWith(line, L"```"))
    {
        inCodeBlock = !inCodeBlock;
        if (inCodeBlock) fenceLang = md_ParseFenceLang(line);
        else fenceLang.clear();
    }
}

std::vector<Token> Highlighter::TokenizeMarkdownLine(const std::wstring &line, bool &inCodeBlock, std::wstring &fenceLang)
{
    std::vector<Token> out;

    // fence line
    if (md_StartsWith(line, L"```"))
    {
        AdvanceMarkdownState(line, inCodeBlock, fenceLang);
        out.push_back({0, (int)line.size(), TokenType::MarkdownCodeFence});
        return out;
    }

    // inside code block
    if (inCodeBlock)
    {
        std::wstring lang = fenceLang;
        if (lang == L"cpp" || lang == L"c" || lang == L"h" || lang == L"hpp")
            return TokenizeLine(line, L".cpp");
        if (lang == L"js" || lang == L"javascript")
            return TokenizeLine(line, L".js");
        if (lang == L"ts" || lang == L"typescript")
            return TokenizeLine(line, L".ts");
        if (lang == L"rs" || lang == L"rust")
            return TokenizeLine(line, L".rs");
        if (lang == L"py" || lang == L"python")
            return TokenizeLine(line, L".py");
        if (lang == L"html")
            return TokenizeLine(line, L".html");

        out.push_back({0, (int)line.size(), TokenType::MarkdownCode});
        return out;
    }

    // heading
    {
        size_t p = 0;
        while (p < line.size() && line[p] == L'#') p++;
        if (p > 0 && p < line.size() && line[p] == L' ')
        {
            out.push_back({0, (int)line.size(), TokenType::MarkdownHeading});
            return out;
        }
    }

    // hr --- *** ___ (trim only)
    {
        std::wstring t = md_Trim(line);
        if (t.size() >= 3)
        {
            bool allDash = true, allStar = true, allUnd = true;
            for (wchar_t c : t)
            {
                if (c != L'-') allDash = false;
                if (c != L'*') allStar = false;
                if (c != L'_') allUnd = false;
            }
            if (allDash || allStar || allUnd)
            {
                out.push_back({0, (int)line.size(), TokenType::MarkdownHr});
                return out;
            }
        }
    }

    // quote
    if (!line.empty() && line[0] == L'>')
    {
        out.push_back({0, 1, TokenType::MarkdownQuote});
        if (line.size() > 1)
            out.push_back({1, (int)line.size() - 1, TokenType::Normal});
        return out;
    }

    // list markers
    {
        size_t i = 0;
        while (i < line.size() && md_IsSpace(line[i])) i++;

        if (i < line.size())
        {
            bool isBullet = (line[i] == L'-' || line[i] == L'*' || line[i] == L'+');
            if (isBullet && i + 1 < line.size() && md_IsSpace(line[i + 1]))
            {
                if (i > 0) out.push_back({0, (int)i, TokenType::Normal});
                out.push_back({(int)i, 1, TokenType::MarkdownListMarker});
                out.push_back({(int)i + 1, (int)line.size() - ((int)i + 1), TokenType::Normal});
                return out;
            }

            // numbered list "1."
            if (iswdigit(line[i]))
            {
                size_t j = i;
                while (j < line.size() && iswdigit(line[j])) j++;
                if (j + 1 < line.size() && line[j] == L'.' && md_IsSpace(line[j + 1]))
                {
                    if (i > 0) out.push_back({0, (int)i, TokenType::Normal});
                    out.push_back({(int)i, (int)(j - i + 1), TokenType::MarkdownListMarker});
                    out.push_back({(int)j + 1, (int)line.size() - ((int)j + 1), TokenType::Normal});
                    return out;
                }
            }
        }
    }

    // inline scan: `code`, **strong**, *em*, [text](url)
    const int n = (int)line.size();
    int k = 0;
    int last = 0;

    auto flushNormal = [&](int to)
    {
        if (to > last)
            out.push_back({last, to - last, TokenType::Normal});
        last = to;
    };

    while (k < n)
    {
        // inline code
        if (line[k] == L'`')
        {
            flushNormal(k);
            int start = k++;
            while (k < n && line[k] != L'`') k++;
            if (k < n) k++;
            out.push_back({start, k - start, TokenType::MarkdownInlineCode});
            last = k;
            continue;
        }

        // link [text](url)
        if (line[k] == L'[')
        {
            int a = k;
            int close = (int)line.find(L']', k + 1);
            if (close != -1 && close + 1 < n && line[close + 1] == L'(')
            {
                int closeUrl = (int)line.find(L')', close + 2);
                if (closeUrl != -1)
                {
                    flushNormal(a);
                    out.push_back({a, close - a + 1, TokenType::MarkdownLinkText});
                    out.push_back({close + 1, closeUrl - (close + 1) + 1, TokenType::MarkdownLinkUrl});
                    k = closeUrl + 1;
                    last = k;
                    continue;
                }
            }
        }

        // strong **...**
        if (k + 1 < n && line[k] == L'*' && line[k + 1] == L'*')
        {
            int start = k;
            int end = (int)line.find(L"**", k + 2);
            if (end != -1)
            {
                flushNormal(start);
                end += 2;
                out.push_back({start, end - start, TokenType::MarkdownStrong});
                k = end;
                last = k;
                continue;
            }
        }

        // emphasis *...*
        if (line[k] == L'*')
        {
            int start = k;
            int end = (int)line.find(L"*", k + 1);
            if (end != -1)
            {
                flushNormal(start);
                end += 1;
                out.push_back({start, end - start, TokenType::MarkdownEmphasis});
                k = end;
                last = k;
                continue;
            }
        }

        k++;
    }

    flushNormal(n);
    return out;
}

// ============================================================
// TokenizeLine (NON-Markdown)
// ============================================================
std::vector<Token> Highlighter::TokenizeLine(const std::wstring &line, const std::wstring &ext)
{
    std::vector<Token> out;

    // IMPORTANT: Markdown must be tokenized via TokenizeMarkdownLine in Editor.
    if (ext == L".md")
    {
        out.push_back({0, (int)line.size(), TokenType::Normal});
        return out;
    }

    // Choose keyword/type sets based on extension
    const std::unordered_set<std::wstring> *kwSet = &cppKeywords;
    const std::unordered_set<std::wstring> *typeSet = &cppTypes;
    bool treatBacktickAsString = false;

    if (ext == L".js" || ext == L".ts")
    {
        kwSet = &jsKeywords;
        typeSet = &jsTypes;
        treatBacktickAsString = true;
    }
    else if (ext == L".rs")
    {
        kwSet = &rustKeywords;
        typeSet = &rustTypes;
    }

    // Simple HTML tokenization
    if (ext == L".html" || ext == L".htm")
    {
        int i = 0;
        int n = (int)line.size();
        while (i < n)
        {
            wchar_t c = line[i];

            // Comment <!-- ... -->
            if (c == L'<' && i + 3 < n && line[i + 1] == L'!' && line[i + 2] == L'-' && line[i + 3] == L'-')
            {
                int start = i;
                i += 4;
                while (i + 2 < n && !(line[i] == L'-' && line[i + 1] == L'-' && line[i + 2] == L'>'))
                    i++;
                if (i + 2 < n) i += 3; else i = n;
                out.push_back({start, i - start, TokenType::Comment});
                continue;
            }

            // Tag start
            if (c == L'<')
            {
                out.push_back({i, 1, TokenType::Normal});
                i++;

                // optional '/' for closing tags
                if (i < n && line[i] == L'/') { out.push_back({i, 1, TokenType::Normal}); i++; }

                // tag name
                int nameStart = i;
                while (i < n && (iswalpha(line[i]) || line[i] == L':' || line[i] == L'-' || iswdigit(line[i])))
                    i++;
                if (i > nameStart)
                    out.push_back({nameStart, i - nameStart, TokenType::Keyword});

                // attributes until '>' or '/>'
                while (i < n)
                {
                    // skip whitespace
                    int ws = i;
                    while (i < n && iswspace(line[i])) i++;
                    if (i > ws)
                        out.push_back({ws, i - ws, TokenType::Normal});

                    if (i >= n || line[i] == L'>') break;
                    if (line[i] == L'/') { out.push_back({i, 1, TokenType::Normal}); i++; continue; }

                    // attr name
                    int astart = i;
                    while (i < n && (iswalnum(line[i]) || line[i] == L'-' || line[i] == L':' || line[i] == L'_'))
                        i++;
                    if (i > astart)
                        out.push_back({astart, i - astart, TokenType::Type});

                    // skip whitespace
                    while (i < n && iswspace(line[i])) i++;
                    // equal sign
                    if (i < n && line[i] == L'=') { out.push_back({i, 1, TokenType::Normal}); i++; }
                    while (i < n && iswspace(line[i])) i++;

                    // value
                    if (i < n && (line[i] == L'"' || line[i] == L'\''))
                    {
                        wchar_t q = line[i];
                        int vstart = i;
                        i++;
                        while (i < n)
                        {
                            if (line[i] == L'\\' && i + 1 < n) { i += 2; continue; }
                            if (line[i] == q) { i++; break; }
                            i++;
                        }
                        out.push_back({vstart, i - vstart, TokenType::String});
                    }
                    else
                    {
                        // unquoted value
                        int vstart = i;
                        while (i < n && !iswspace(line[i]) && line[i] != L'>') i++;
                        if (i > vstart)
                            out.push_back({vstart, i - vstart, TokenType::String});
                    }
                }

                // closing '>' if present
                if (i < n && line[i] == L'>') { out.push_back({i, 1, TokenType::Normal}); i++; }
                continue;
            }

            // text outside tags
            int start = i;
            while (i < n && line[i] != L'<') i++;
            if (i > start)
                out.push_back({start, i - start, TokenType::Normal});
        }

        return out;
    }

    // C/C++/JS/TS/Rust style tokenization (basic)
    auto isCppLikeExt = [&]() -> bool
    {
        return ext == L".c" || ext == L".cpp" || ext == L".cc" || ext == L".cxx" ||
            ext == L".h" || ext == L".hpp" || ext == L".hh" || ext == L".hxx" || ext == L".inl";
    };

    if (isCppLikeExt())
    {
        int n = (int)line.size();
        int firstNonBOM = 0;
        while (firstNonBOM < n && line[firstNonBOM] == 0xFEFF)
            firstNonBOM++;

        int i = firstNonBOM;
        while (i < n && iswspace(line[i]))
            i++;

        if (i < n && line[i] == L'#')
        {
            int hashPos = i;
            i++;
            while (i < n && iswspace(line[i]))
                i++;
            int wordStart = i;
            while (i < n && iswalpha(line[i]))
                i++;

            std::wstring directive = line.substr(wordStart, i - wordStart);
            for (auto &c : directive) c = towlower(c);
            if (directive == L"include")
            {
                out.push_back({firstNonBOM, i - firstNonBOM, TokenType::Preprocessor});

                int wsStart = i;
                while (wsStart < n && iswspace(line[wsStart]))
                    wsStart++;
                if (wsStart > i)
                    out.push_back({i, wsStart - i, TokenType::Normal});

                if (wsStart < n && (line[wsStart] == L'<' || line[wsStart] == L'"'))
                {
                    wchar_t endCh = (line[wsStart] == L'<') ? L'>' : L'"';
                    int incStart = wsStart;
                    int j = wsStart + 1;
                    while (j < n && line[j] != endCh)
                        j++;
                    if (j < n)
                        j++;
                    out.push_back({incStart, j - incStart, TokenType::String});
                    if (j < n)
                        out.push_back({j, n - j, TokenType::Normal});
                }
                else if (wsStart < n)
                {
                    out.push_back({wsStart, n - wsStart, TokenType::Normal});
                }

                return out;
            }
        }
    }

    int i = 0;
    int n = (int)line.size();
    bool isCpp = (ext == L".c" || ext == L".cpp" || ext == L".cc" || ext == L".cxx" ||
                  ext == L".h" || ext == L".hpp" || ext == L".hh" || ext == L".hxx" || ext == L".inl");
    bool prevWasType = false;
    while (i < n)
    {
        wchar_t c = line[i];

        // whitespace
        if (iswspace(c))
        {
            int start = i;
            while (i < n && iswspace(line[i])) i++;
            out.push_back({start, i - start, TokenType::Normal});
            continue;
        }

        // single-line comment (//)
        if (c == L'/' && i + 1 < n && line[i + 1] == L'/')
        {
            out.push_back({i, n - i, TokenType::Comment});
            break;
        }

        // block comment (/*...*/)
        if (c == L'/' && i + 1 < n && line[i + 1] == L'*')
        {
            int start = i;
            i += 2;
            while (i + 1 < n && !(line[i] == L'*' && line[i + 1] == L'/'))
                i++;
            if (i + 1 < n) i += 2; else i = n;
            out.push_back({start, i - start, TokenType::Comment});
            continue;
        }

        // string literal
        if (c == L'"' || c == L'\'' || (treatBacktickAsString && c == L'`'))
        {
            int start = i;
            wchar_t quote = c;
            i++;
            while (i < n)
            {
                if (line[i] == L'\\' && i + 1 < n) { i += 2; continue; }
                if (line[i] == quote) { i++; break; }
                i++;
            }
            out.push_back({start, i - start, TokenType::String});
            continue;
        }

        // preprocessor
        if (c == L'#')
        {
            bool onlyBeforeWhitespace = true;
            int firstNonBOM = 0;

            for (int j = 0; j < i; ++j)
            {
                wchar_t pc = line[j];
                if (pc == 0xFEFF) { firstNonBOM = j + 1; continue; }
                if (!iswspace(pc)) { onlyBeforeWhitespace = false; break; }
            }

            if (onlyBeforeWhitespace)
            {
                while (!out.empty() && out.back().start < i) out.pop_back();
                out.push_back({firstNonBOM, n - firstNonBOM, TokenType::Preprocessor});
                break;
            }
        }

        if (c == L';' || c == L'{' || c == L'}')
        {
            out.push_back({i, 1, TokenType::Normal});
            i++;
            prevWasType = false;
            continue;
        }

        // number
        if (iswdigit(c))
        {
            int start = i;
            while (i < n && (iswdigit(line[i]) || line[i] == L'.' || line[i] == L'x' || line[i] == L'X' || iswxdigit(line[i])))
                i++;
            out.push_back({start, i - start, TokenType::Number});
            prevWasType = false;
            continue;
        }

        // identifier
        if (IsIdentifierStart(c))
        {
            int start = i;
            i++;
            while (i < n && IsIdentifierPart(line[i])) i++;

            std::wstring word = line.substr(start, i - start);
            std::wstring low = word;
            for (auto &ch : low) ch = towlower(ch);

            auto isMacro = [&](const std::wstring &w) -> bool
            {
                if (w.size() < 2)
                    return false;
                bool hasUpper = false;
                for (wchar_t ch : w)
                {
                    if (iswalpha(ch))
                    {
                        if (!iswupper(ch))
                            return false;
                        hasUpper = true;
                    }
                    else if (!(iswdigit(ch) || ch == L'_'))
                    {
                        return false;
                    }
                }
                return hasUpper;
            };

            if (isMacro(word))
                out.push_back({start, (int)(i - start), TokenType::Macro});
            else if (kwSet->find(low) != kwSet->end())
                out.push_back({start, (int)(i - start), TokenType::Keyword});
            else if (typeSet->find(low) != typeSet->end() || cppKnownTypes.find(word) != cppKnownTypes.end() ||
                     (ext == L".c" || ext == L".cpp" || ext == L".h" || ext == L".hpp") && iswupper(word[0]))
            {
                out.push_back({start, (int)(i - start), TokenType::Type});
                if (isCpp)
                    prevWasType = true;
            }
            else
            {
                int j = i;
                while (j < n && iswspace(line[j])) j++;
                if (j < n && line[j] == L'(')
                {
                    out.push_back({start, (int)(i - start), TokenType::Function});
                    prevWasType = false;
                }
                else
                {
                    if (isCpp)
                        out.push_back({start, (int)(i - start), TokenType::Variable});
                    else
                        out.push_back({start, (int)(i - start), TokenType::Normal});
                    prevWasType = false;
                }
            }

            continue;
        }

        // punctuation/other
        out.push_back({i, 1, TokenType::Normal});
        i++;
        if (!(c == L':' || c == L'*' || c == L'&' || c == L'<' || c == L'>' || c == L','))
            prevWasType = false;
    }

    return out;
}
