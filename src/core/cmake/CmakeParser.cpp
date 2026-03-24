#include "CmakeParser.h"
#include <windows.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static std::string Trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, b - a + 1);
}

static std::wstring ToWide(const std::string &s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    if (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

static std::string ToNarrow(const std::wstring &w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
    if (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

// Lower-case a string for case-insensitive keyword matching
static std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

// Split a string on whitespace / semicolons (CMake list separator)
static std::vector<std::string> SplitTokens(const std::string &s)
{
    std::vector<std::string> tokens;
    std::istringstream ss(s);
    std::string tok;
    while (ss >> tok)
    {
        // Also split on semicolons
        std::string cur;
        for (char c : tok)
        {
            if (c == ';') { if (!cur.empty()) { tokens.push_back(Trim(cur)); cur.clear(); } }
            else cur += c;
        }
        if (!cur.empty()) tokens.push_back(Trim(cur));
    }
    return tokens;
}

// Read a full CMake command's argument string (everything inside the outer parens).
// `content`  — full file content
// `startPos` — position of the '(' character
// Returns the argument string (without parens), or "" on error.
static std::string ExtractArgs(const std::string &content, size_t startPos)
{
    if (startPos >= content.size() || content[startPos] != '(') return "";
    int depth = 0;
    size_t begin = startPos + 1;
    for (size_t i = startPos; i < content.size(); ++i)
    {
        if (content[i] == '(')      ++depth;
        else if (content[i] == ')') { --depth; if (depth == 0) return content.substr(begin, i - begin); }
    }
    return "";
}

// Return true if token is a CMake keyword we should skip in argument lists
static bool IsScopeKeyword(const std::string &t)
{
    const char *kw[] = {"private","public","interface","before","system","after","exclude_from_all"};
    std::string l = Lower(t);
    for (auto *k : kw) if (l == k) return true;
    return false;
}

// ---------------------------------------------------------------------------
// ParseCmakeLists
// ---------------------------------------------------------------------------
CmakeProjectInfo ParseCmakeLists(const std::wstring &cmakePath)
{
    CmakeProjectInfo info;
    std::wstring firstExecutableTarget;
    std::wstring firstLibraryTarget;

    // Project root = directory containing CMakeLists.txt
    try { info.projectRoot = fs::path(cmakePath).parent_path().wstring(); }
    catch (...) { info.projectRoot = cmakePath; }

    // Read file as UTF-8 bytes
    std::ifstream f(cmakePath);
    if (!f.is_open()) return info;

    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    f.close();

    // Strip line comments (# ...) but preserve newlines so positions are stable
    std::string stripped;
    stripped.reserve(content.size());
    bool inLineComment = false;
    for (size_t i = 0; i < content.size(); ++i)
    {
        if (content[i] == '#' && !inLineComment) inLineComment = true;
        if (content[i] == '\n')                  inLineComment = false;
        stripped += inLineComment ? ' ' : content[i];
    }

    // Walk through the stripped content looking for CMake commands
    size_t pos = 0;
    while (pos < stripped.size())
    {
        // Skip whitespace
        while (pos < stripped.size() && std::isspace((unsigned char)stripped[pos])) ++pos;
        if (pos >= stripped.size()) break;

        // Read an identifier
        size_t idStart = pos;
        while (pos < stripped.size() && (std::isalnum((unsigned char)stripped[pos]) || stripped[pos] == '_')) ++pos;
        if (pos == idStart) { ++pos; continue; }   // Not an identifier, skip char

        std::string cmd = Lower(stripped.substr(idStart, pos - idStart));

        // Skip whitespace before '('
        while (pos < stripped.size() && std::isspace((unsigned char)stripped[pos])) ++pos;
        if (pos >= stripped.size() || stripped[pos] != '(') continue;

        std::string args = ExtractArgs(stripped, pos);
        // Advance past the closing ')'
        int depth = 0;
        while (pos < stripped.size()) {
            if (stripped[pos] == '(') ++depth;
            else if (stripped[pos] == ')') { --depth; if (depth == 0) { ++pos; break; } }
            ++pos;
        }

        std::vector<std::string> tokens = SplitTokens(args);
        if (tokens.empty()) continue;

        // ── add_executable / add_library → capture target names ──
        if ((cmd == "add_executable" || cmd == "add_library"))
        {
            // Skip WIN32, SHARED, STATIC, MODULE, INTERFACE, OBJECT keywords
            for (auto &t : tokens)
            {
                std::string l = Lower(t);
                if (l != "win32" && l != "shared" && l != "static" && l != "module"
                    && l != "interface" && l != "object" && l != "imported"
                    && l != "alias" && l != "unknown")
                {
                    std::wstring parsed = ToWide(t);
                    if (cmd == "add_executable" && firstExecutableTarget.empty())
                        firstExecutableTarget = parsed;
                    if (cmd == "add_library" && firstLibraryTarget.empty())
                        firstLibraryTarget = parsed;
                    break;
                }
            }
        }

        // ── add_subdirectory → first arg is path ──
        else if (cmd == "add_subdirectory" && !tokens.empty())
        {
            std::string subdirRel = tokens[0];
            // Skip system/external CMake helper directories
            std::string lp = Lower(subdirRel);
            if (lp.find("cmake") == std::string::npos)   // skip cmake utility dirs
            {
                CmakeSubLib lib;
                lib.subdirRel = ToWide(subdirRel);
                // Display name = last component of the path
                try {
                    lib.name = ToWide(fs::path(subdirRel).filename().string());
                } catch (...) { lib.name = lib.subdirRel; }
                // Absolute path
                try {
                    lib.subdirAbs = (fs::path(info.projectRoot) / subdirRel).lexically_normal().wstring();
                } catch (...) { lib.subdirAbs = lib.subdirRel; }
                info.libraries.push_back(std::move(lib));
            }
        }

        // ── target_include_directories → collect paths (skip target name + scope keywords) ──
        else if (cmd == "target_include_directories" && tokens.size() > 2)
        {
            bool pastTarget = false, pastScope = false;
            for (size_t i = 1; i < tokens.size(); ++i)    // skip tokens[0] (target name)
            {
                if (!pastTarget) { pastTarget = true; continue; }
                if (IsScopeKeyword(tokens[i])) { pastScope = true; continue; }
                if (!pastScope) continue;
                // Path token — skip pure CMake variables if they expand to nothing useful
                std::wstring p = ToWide(tokens[i]);
                if (!p.empty()) info.includeDirs.push_back(p);
            }
        }
    }

    if (!firstExecutableTarget.empty())
        info.targetName = firstExecutableTarget;
    else
        info.targetName = firstLibraryTarget;

    return info;
}

// ---------------------------------------------------------------------------
// AddLibraryToCmake
// ---------------------------------------------------------------------------
bool AddLibraryToCmake(const std::wstring &projectCmakePath,
                        const std::wstring &projectTargetName,
                        const std::wstring &subdirRel,
                        const std::wstring &libTargetName,
                        const std::wstring &includeRel)
{
    // Read existing CMakeLists.txt
    std::ifstream rf(projectCmakePath);
    if (!rf.is_open()) return false;
    std::string existing((std::istreambuf_iterator<char>(rf)),
                          std::istreambuf_iterator<char>());
    rf.close();

    // Convert paths to narrow strings for the cmake file (forward slashes)
    auto toFwdSlash = [](std::wstring w) -> std::string {
        std::replace(w.begin(), w.end(), L'\\', L'/');
        return ToNarrow(w);
    };

    std::string subdirStr  = toFwdSlash(subdirRel);
    std::string includeStr = toFwdSlash(includeRel);
    std::string targetStr  = ToNarrow(projectTargetName);
    std::string libStr     = ToNarrow(libTargetName);

    // If the subdirectory is already present, avoid appending duplicate blocks.
    {
        std::string marker = "add_subdirectory(\"" + subdirStr + "\")";
        if (existing.find(marker) != std::string::npos)
            return true;
    }

    // Build the block to append
    std::ostringstream block;
    block << "\n";
    block << "# ── External library: " << libStr << " (added by Nebula) ──\n";
    block << "add_subdirectory(\"" << subdirStr << "\")\n";
    if (!includeStr.empty())
        block << "target_include_directories(" << targetStr << " PRIVATE \"" << includeStr << "\")\n";
    block << "target_link_libraries(" << targetStr << " PRIVATE " << libStr << ")\n";

    // Append to the file
    std::ofstream wf(projectCmakePath, std::ios::app);
    if (!wf.is_open()) return false;
    wf << block.str();
    wf.close();
    return true;
}
