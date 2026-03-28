#include "CmakeParser.h"
#include <windows.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <unordered_map>

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
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
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

static std::string StripQuotes(std::string s)
{
    if (s.size() >= 2)
    {
        if ((s.front() == '"' && s.back() == '"') ||
            (s.front() == '\'' && s.back() == '\''))
            return s.substr(1, s.size() - 2);
    }
    return s;
}

static std::string NormalizeSlashes(std::string s)
{
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

static std::string ResolveCmakeVarToken(const std::string &token,
                                        const std::unordered_map<std::string, std::string> &vars)
{
    std::string t = StripQuotes(token);
    if (t.size() < 4 || t.rfind("${", 0) != 0)
        return t;

    size_t close = t.find('}');
    if (close == std::string::npos)
        return t;

    std::string varName = Lower(t.substr(2, close - 2));
    auto it = vars.find(varName);
    if (it == vars.end())
        return t;

    std::string suffix = t.substr(close + 1);
    return it->second + suffix;
}

static bool TryExtractExternalRel(const std::string &valueRaw, std::string &outRel)
{
    std::string value = NormalizeSlashes(valueRaw);
    std::string lower = Lower(value);

    size_t pos = lower.find("/external/");
    if (pos == std::string::npos)
    {
        if (lower.rfind("external/", 0) == 0)
            pos = static_cast<size_t>(-1); // already starts with external/
        else
            return false;
    }

    std::string rel;
    if (pos == static_cast<size_t>(-1))
        rel = value;
    else
        rel = value.substr(pos + 1); // keep "external/..."

    // Keep at most external/<lib>
    std::string relLower = Lower(rel);
    size_t extPos = relLower.find("external/");
    if (extPos == std::string::npos)
        return false;

    std::string tail = rel.substr(extPos + 9);
    size_t slash = tail.find('/');
    std::string libName = (slash == std::string::npos) ? tail : tail.substr(0, slash);
    if (libName.empty())
        return false;

    outRel = "external/" + libName;
    return true;
}

static bool ContainsAny(const std::string &textLower, const std::vector<std::string> &needles)
{
    for (const auto &n : needles)
    {
        if (!n.empty() && textLower.find(n) != std::string::npos)
            return true;
    }
    return false;
}

static std::string ReadFileUtf8BestEffort(const fs::path &p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open())
        return {};
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static std::string ToLowerCopy(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

struct AutoSystemDeps
{
    bool needsOpenGL = false;
    bool needsWinsock = false;
    bool needsThreads = false;
};

static AutoSystemDeps DetectAutoSystemDeps(const fs::path &projectCmakePath, const std::string &subdirRel)
{
    AutoSystemDeps deps;

    fs::path projectRoot;
    try { projectRoot = projectCmakePath.parent_path(); }
    catch (...) { return deps; }

    fs::path libRoot = (projectRoot / fs::path(subdirRel)).lexically_normal();
    std::error_code ec;
    if (!fs::exists(libRoot, ec) || !fs::is_directory(libRoot, ec))
        return deps;

    // 1) Look at the library CMakeLists.txt first (fast + most reliable signal).
    std::string cmakeLower;
    {
        fs::path libCmake = libRoot / "CMakeLists.txt";
        cmakeLower = ToLowerCopy(ReadFileUtf8BestEffort(libCmake));
    }

    deps.needsOpenGL = ContainsAny(cmakeLower, {
        "find_package(opengl", "opengl::gl", "opengl_gl_preference", "glad", "glew"
    });

    deps.needsThreads = ContainsAny(cmakeLower, {
        "find_package(threads", "threads::threads", "pthread"
    });

    deps.needsWinsock = ContainsAny(cmakeLower, {
        "ws2_32", "winsock", "ws2tcpip"
    });

    // 2) Fallback signal from source/include tree (limited scan) for libs that
    // rely on system APIs without declaring them explicitly in top-level cmake.
    int scannedFiles = 0;
    const int kMaxFiles = 120;
    for (fs::recursive_directory_iterator it(libRoot, fs::directory_options::skip_permission_denied, ec), end;
         it != end && scannedFiles < kMaxFiles; ++it)
    {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;

        std::string ext = ToLowerCopy(it->path().extension().string());
        if (ext != ".h" && ext != ".hpp" && ext != ".hh" && ext != ".c" && ext != ".cpp" && ext != ".cc" && ext != ".cxx")
            continue;

        std::string txt = ToLowerCopy(ReadFileUtf8BestEffort(it->path()));
        if (txt.empty())
            continue;

        if (!deps.needsOpenGL && ContainsAny(txt, {"#include <gl/", "#include <glad/", "#include <glew/"}))
            deps.needsOpenGL = true;

        if (!deps.needsWinsock && ContainsAny(txt, {"#include <winsock2.h>", "#include <ws2tcpip.h>"}))
            deps.needsWinsock = true;

        ++scannedFiles;
    }

    return deps;
}

// ---------------------------------------------------------------------------
// ParseCmakeLists
// ---------------------------------------------------------------------------
CmakeProjectInfo ParseCmakeLists(const std::wstring &cmakePath)
{
    CmakeProjectInfo info;
    std::wstring firstExecutableTarget;
    std::wstring firstLibraryTarget;
    std::unordered_map<std::string, std::string> cmakeVars;

    auto appendLibIfNew = [&info](const CmakeSubLib &candidate)
    {
        std::wstring keyNew = candidate.subdirAbs.empty() ? candidate.subdirRel : candidate.subdirAbs;
        std::transform(keyNew.begin(), keyNew.end(), keyNew.begin(), ::towlower);

        for (const auto &existing : info.libraries)
        {
            std::wstring keyOld = existing.subdirAbs.empty() ? existing.subdirRel : existing.subdirAbs;
            std::transform(keyOld.begin(), keyOld.end(), keyOld.begin(), ::towlower);
            if (!keyNew.empty() && keyNew == keyOld)
                return;
        }
        info.libraries.push_back(candidate);
    };

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

        // ── set(VAR value) → keep a lightweight variable map for path resolution ──
        if (cmd == "set" && tokens.size() >= 2)
        {
            std::string var = Lower(StripQuotes(tokens[0]));
            std::string val = ResolveCmakeVarToken(tokens[1], cmakeVars);
            if (!var.empty() && !val.empty())
                cmakeVars[var] = NormalizeSlashes(StripQuotes(val));
        }

        // ── file(GLOB VAR pattern...) / file(GLOB_RECURSE VAR pattern...) ──
        else if (cmd == "file" && tokens.size() >= 3)
        {
            std::string mode = Lower(StripQuotes(tokens[0]));
            if (mode == "glob" || mode == "glob_recurse")
            {
                std::string outVar = Lower(StripQuotes(tokens[1]));
                std::string firstPattern;

                for (size_t i = 2; i < tokens.size(); ++i)
                {
                    std::string tk = Lower(StripQuotes(tokens[i]));
                    if (tk == "list_directories" || tk == "follow_symlinks" || tk == "configure_depends")
                        continue;

                    std::string resolved = ResolveCmakeVarToken(tokens[i], cmakeVars);
                    resolved = NormalizeSlashes(StripQuotes(resolved));
                    if (!resolved.empty())
                    {
                        firstPattern = resolved;
                        break;
                    }
                }

                if (!outVar.empty() && !firstPattern.empty())
                    cmakeVars[outVar] = firstPattern;
            }
        }

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

            // Detect libraries whose source list comes from external/<name>/...
            if (cmd == "add_library" && !tokens.empty())
            {
                std::string targetName = StripQuotes(tokens[0]);
                for (size_t i = 1; i < tokens.size(); ++i)
                {
                    std::string resolved = ResolveCmakeVarToken(tokens[i], cmakeVars);
                    std::string relExternal;
                    if (!TryExtractExternalRel(resolved, relExternal))
                        continue;

                    CmakeSubLib lib;
                    lib.subdirRel = ToWide(relExternal);
                    try { lib.name = ToWide(fs::path(relExternal).filename().string()); }
                    catch (...) { lib.name = ToWide(targetName); }
                    try {
                        lib.subdirAbs = (fs::path(info.projectRoot) / fs::path(relExternal)).lexically_normal().wstring();
                    } catch (...) {
                        lib.subdirAbs = lib.subdirRel;
                    }
                    appendLibIfNew(lib);
                    break;
                }
            }
        }

        // ── add_subdirectory → first arg is path ──
        else if (cmd == "add_subdirectory" && !tokens.empty())
        {
            std::string subdirRaw = StripQuotes(tokens[0]);
            std::string resolved = ResolveCmakeVarToken(subdirRaw, cmakeVars);
            std::string subdirRel = subdirRaw;

            std::string externalRel;
            if (TryExtractExternalRel(resolved, externalRel))
                subdirRel = externalRel;
            else if (!resolved.empty() && resolved.find("${") == std::string::npos)
                subdirRel = resolved;

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
                    fs::path p = fs::path(subdirRel);
                    if (p.is_absolute())
                        lib.subdirAbs = p.lexically_normal().wstring();
                    else
                        lib.subdirAbs = (fs::path(info.projectRoot) / p).lexically_normal().wstring();
                } catch (...) { lib.subdirAbs = lib.subdirRel; }
                appendLibIfNew(lib);
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

    auto toLower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return s;
    };

    std::string existingLower = toLower(existing);
    std::string subdirLower = toLower(subdirStr);
    std::string libLower = toLower(libStr);
    bool hasOpenGL32 = (existingLower.find("opengl32") != std::string::npos);
    bool hasWs2_32 = (existingLower.find("ws2_32") != std::string::npos);
    bool hasThreadsLink = (existingLower.find("threads::threads") != std::string::npos);

    AutoSystemDeps autoDeps = DetectAutoSystemDeps(fs::path(projectCmakePath), subdirStr);

    // If the subdirectory is already present, avoid appending duplicate blocks.
    {
        std::string markerQuoted = "add_subdirectory(\"" + subdirStr + "\")";
        std::string markerBare = "add_subdirectory(" + subdirStr + ")";
        if (existing.find(markerQuoted) != std::string::npos ||
            existing.find(markerBare) != std::string::npos ||
            existingLower.find(subdirLower) != std::string::npos)
            return true;

        // Built-in dependencies may be added via CMake variables
        // (e.g. add_subdirectory(${LIBGIT2_DIR} EXCLUDE_FROM_ALL)).
        if (subdirLower.find("libgit2") != std::string::npos &&
            existingLower.find("libgit2_dir") != std::string::npos)
            return true;
        if (subdirLower.find("ggwave") != std::string::npos &&
            existingLower.find("ggwave_dir") != std::string::npos)
            return true;
        if (subdirLower.find("libvterm") != std::string::npos &&
            existingLower.find("libvterm_dir") != std::string::npos)
            return true;

        // If target already links this library, do not append.
        std::string linkMarker = "target_link_libraries(" + targetStr;
        if (existingLower.find(toLower(linkMarker)) != std::string::npos &&
            existingLower.find(libLower) != std::string::npos)
            return true;
    }

    // Detect glad generator repo (Dav1dde/glad v2) — it exposes glad_add_library()
    // instead of a standard static target, so it needs a different cmake block.
    bool isGladGenerator = false;
    {
        fs::path projRoot;
        try { projRoot = fs::path(projectCmakePath).parent_path(); } catch (...) {}
        if (!projRoot.empty())
        {
            fs::path libRoot = projRoot / fs::path(subdirStr);
            std::error_code ec;
            isGladGenerator =
                fs::exists(libRoot / "cmake" / "GladConfig.cmake.in", ec) ||
                fs::exists(libRoot / "cmake" / "glad.cmake", ec)           ||
                (fs::exists(libRoot / "setup.py", ec) &&
                 fs::exists(libRoot / "glad",     ec));
        }
    }

    // Build the block to append
    std::ostringstream block;
    block << "\n";
    block << "# ── External library: " << libStr << " (added by Nebula) ──\n";

    if (isGladGenerator)
    {
        // glad v2 cmake integration: generates glad source at configure time.
        block << "add_subdirectory(\"" << subdirStr << "\")\n";
        block << "glad_add_library(glad STATIC REPRODUCIBLE API gl:core=4.6)\n";
        block << "target_link_libraries(" << targetStr << " PRIVATE glad)\n";
        if (!hasOpenGL32)
        {
            block << "if (WIN32)\n";
            block << "  target_link_libraries(" << targetStr << " PRIVATE opengl32)\n";
            block << "endif()\n";
        }
    }
    else
    {
        block << "add_subdirectory(\"" << subdirStr << "\")\n";
        if (!includeStr.empty())
            block << "target_include_directories(" << targetStr << " PRIVATE \"" << includeStr << "\")\n";
        block << "target_link_libraries(" << targetStr << " PRIVATE " << libStr << ")\n";

        if (autoDeps.needsOpenGL && !hasOpenGL32)
        {
            block << "if (WIN32)\n";
            block << "  target_link_libraries(" << targetStr << " PRIVATE opengl32)\n";
            block << "endif()\n";
        }
    }

    if (autoDeps.needsWinsock && !hasWs2_32)
    {
        block << "if (WIN32)\n";
        block << "  target_link_libraries(" << targetStr << " PRIVATE ws2_32)\n";
        block << "endif()\n";
    }

    if (autoDeps.needsThreads && !hasThreadsLink)
    {
        block << "find_package(Threads QUIET)\n";
        block << "if (Threads_FOUND)\n";
        block << "  target_link_libraries(" << targetStr << " PRIVATE Threads::Threads)\n";
        block << "endif()\n";
    }

    // Append to the file
    std::ofstream wf(projectCmakePath, std::ios::app);
    if (!wf.is_open()) return false;
    wf << block.str();
    wf.close();
    return true;
}

bool RemoveLibraryFromCmake(const std::wstring &projectCmakePath,
                            const std::wstring &subdirRel,
                            const std::wstring &libTargetName)
{
    std::ifstream rf(projectCmakePath, std::ios::binary);
    if (!rf.is_open()) return false;

    std::string existing((std::istreambuf_iterator<char>(rf)),
                         std::istreambuf_iterator<char>());
    rf.close();

    auto toFwdSlash = [](std::wstring w) -> std::string {
        std::replace(w.begin(), w.end(), L'\\', L'/');
        return ToNarrow(w);
    };

    const std::string subdirLower = ToLowerCopy(NormalizeSlashes(toFwdSlash(subdirRel)));
    const std::string libLower = ToLowerCopy(ToNarrow(libTargetName));
    const bool isGladLib = subdirLower.find("glad") != std::string::npos;
    const bool useCrLf = existing.find("\r\n") != std::string::npos;

    std::vector<std::string> lines;
    {
        std::istringstream ss(existing);
        std::string line;
        while (std::getline(ss, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            lines.push_back(line);
        }
    }

    auto isManagedComment = [](const std::string &line) {
        const std::string lower = ToLowerCopy(line);
        return lower.find("external library:") != std::string::npos &&
               lower.find("added by nebula") != std::string::npos;
    };

    auto isLibraryLine = [&](const std::string &line) {
        const std::string trimmed = Trim(line);
        const std::string lower = ToLowerCopy(NormalizeSlashes(trimmed));

        if (lower.find("add_subdirectory") != std::string::npos &&
            lower.find(subdirLower) != std::string::npos)
            return true;

        if (lower.find("target_include_directories") != std::string::npos &&
            lower.find(subdirLower) != std::string::npos)
            return true;

        if (!libLower.empty() &&
            lower.find("target_link_libraries") != std::string::npos &&
            lower.find(libLower) != std::string::npos)
            return true;

        if (isGladLib && lower.find("glad_add_library(") != std::string::npos)
            return true;

        return false;
    };

    std::vector<std::string> kept;
    kept.reserve(lines.size());
    bool removedAny = false;

    for (size_t i = 0; i < lines.size(); ++i)
    {
        bool remove = isLibraryLine(lines[i]);
        if (!remove && isManagedComment(lines[i]))
        {
            for (size_t j = i + 1; j < lines.size(); ++j)
            {
                if (Trim(lines[j]).empty())
                    continue;
                remove = isLibraryLine(lines[j]);
                break;
            }
        }

        if (remove)
        {
            removedAny = true;
            continue;
        }

        if (Trim(lines[i]).empty() && !kept.empty() && Trim(kept.back()).empty())
            continue;

        kept.push_back(lines[i]);
    }

    while (!kept.empty() && Trim(kept.back()).empty())
        kept.pop_back();

    if (!removedAny)
        return true;

    std::ofstream wf(projectCmakePath, std::ios::binary | std::ios::trunc);
    if (!wf.is_open()) return false;

    const char *newline = useCrLf ? "\r\n" : "\n";
    for (size_t i = 0; i < kept.size(); ++i)
    {
        wf << kept[i];
        wf << newline;
    }
    wf.close();
    return true;
}
