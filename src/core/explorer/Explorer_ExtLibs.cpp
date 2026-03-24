// ExplorerExtLibs.cpp
// External Libraries section implementation for ExplorerManager.
// Included at the bottom of Explorer.cpp via the build system.

#include "Explorer.h"
#include "core/cmake/CmakeParser.h"
#include "core/cmake/CloneDialog.h"
#include "utils/logger/Logger.h"
#include "ui/theme/Theme.h"

#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <filesystem>
#include <algorithm>
#include <thread>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helper: detect the best include directory for a library.
// Prefers <lib>/include/, falls back to <lib>/ itself (e.g. imgui, stb, …).
// ---------------------------------------------------------------------------
static std::wstring DetectIncludeDir(const std::wstring &libAbsPath,
                                     const std::wstring &libRelPath)
{
    for (const wchar_t *sub : { L"\\include", L"/include" })
    {
        if (fs::is_directory(libAbsPath + sub))
            return libRelPath + L"/include";
    }
    // No include/ subdir — headers live at the library root (imgui, stb, etc.)
    return libRelPath;
}

// ---------------------------------------------------------------------------
// Helper: find an existing CMake build directory inside rootPath.
// ---------------------------------------------------------------------------
static std::wstring FindBuildDir(const std::wstring &rootPath)
{
    static const wchar_t *kCandidates[] = {
        L"build", L"cmake-build-debug", L"cmake-build-release",
        L"cmake-build-relwithdebinfo", L"out\\build", L"_build",
        L"Release", L"Debug"
    };
    for (auto *name : kCandidates)
    {
        std::wstring candidate = rootPath + L"\\" + name;
        if (fs::exists(candidate + L"\\CMakeCache.txt"))
            return candidate;
    }
    return {};
}

static std::wstring RepoNameFromUrl(const std::wstring &url)
{
    std::wstring u = url;
    if (u.size() >= 4 && u.substr(u.size() - 4) == L".git")
        u = u.substr(0, u.size() - 4);
    while (!u.empty() && (u.back() == L'/' || u.back() == L'\\'))
        u.pop_back();
    size_t pos = u.find_last_of(L"/\\:");
    if (pos != std::wstring::npos)
        u = u.substr(pos + 1);
    return u.empty() ? L"library" : u;
}

static std::wstring NormalizeSlashes(std::wstring value)
{
    std::replace(value.begin(), value.end(), L'\\', L'/');
    return value;
}

static std::string ToUtf8(const std::wstring &value)
{
    if (value.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1)
        return {};
    std::string out((size_t)(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

static std::wstring Utf8ToWide(const char *value)
{
    if (!value || !*value)
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, value, -1, nullptr, 0);
    if (n <= 1)
        return {};
    std::wstring out((size_t)(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value, -1, out.data(), n);
    return out;
}

// ---------------------------------------------------------------------------
// Helper: re-run CMake in the background so clangd gets a fresh
// compile_commands.json.  Called after successfully modifying CMakeLists.txt.
// ---------------------------------------------------------------------------
static void TriggerCmakeReconfigure(const std::wstring &rootPath)
{
    std::wstring buildDir = FindBuildDir(rootPath);
    if (buildDir.empty()) return;   // no existing build dir — skip

    std::thread([rootPath, buildDir]()
    {
        // cmake <buildDir> regenerates without changing generators/settings.
        // We add -DCMAKE_EXPORT_COMPILE_COMMANDS=ON so the JSON is always written.
        std::wstring cmdLine =
            L"cmake \"" + buildDir + L"\" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON";

        STARTUPINFOW si = { sizeof(si) };
        si.dwFlags      = STARTF_USESHOWWINDOW;
        si.wShowWindow  = SW_HIDE;
        PROCESS_INFORMATION pi = {};

        if (CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, rootPath.c_str(), &si, &pi))
        {
            WaitForSingleObject(pi.hProcess, 60000);  // max 60 s
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);

            // Copy compile_commands.json to project root so clangd finds it
            std::wstring src  = buildDir + L"\\compile_commands.json";
            std::wstring dest = rootPath  + L"\\compile_commands.json";
            try {
                if (fs::exists(src))
                    fs::copy_file(src, dest, fs::copy_options::overwrite_existing);
            } catch (...) {}
        }
    }).detach();
}

// ---------------------------------------------------------------------------
// ParseExternalLibs — call after project is opened / CMakeLists.txt changes
// ---------------------------------------------------------------------------
void ExplorerManager::ParseExternalLibs()
{
    extLibs_.cmake = CmakeProjectInfo{};
    if (state_.rootPath.empty()) return;

    std::wstring cmakePath = state_.rootPath + L"\\CMakeLists.txt";
    if (!fs::exists(cmakePath)) cmakePath = state_.rootPath + L"/CMakeLists.txt";
    if (!fs::exists(cmakePath)) return;

    extLibs_.cmake = ParseCmakeLists(cmakePath);
    Logger::Instance().Log(
        std::wstring(L"ExternalLibs: ") +
        std::to_wstring(extLibs_.cmake.libraries.size()) +
        L" libs parsed from " + cmakePath);
}

// ---------------------------------------------------------------------------
// HitTestExtLib
// Virtual indices: 0=header, 1..N=lib entries, N+1=add-button, -1=none
// ---------------------------------------------------------------------------
int ExplorerManager::HitTestExtLib(POINT pt, float sectionY) const
{
    const float L = state_.leftEdge + 1.f;
    const float R = state_.rightEdge - 1.f;
    if ((float)pt.x < L || (float)pt.x > R) return -1;

    float y = sectionY;

    if ((float)pt.y >= y && (float)pt.y < y + ExtLibSection::kHeaderH) return 0;
    y += ExtLibSection::kHeaderH;
    if (!extLibs_.expanded) return -1;

    for (int i = 0; i < (int)extLibs_.cmake.libraries.size(); ++i)
    {
        if ((float)pt.y >= y && (float)pt.y < y + ExtLibSection::kItemH) return i + 1;
        y += ExtLibSection::kItemH;
    }

    int addIdx = (int)extLibs_.cmake.libraries.size() + 1;
    if ((float)pt.y >= y && (float)pt.y < y + ExtLibSection::kAddBtnH) return addIdx;
    y += ExtLibSection::kAddBtnH;

    int cloneIdx = (int)extLibs_.cmake.libraries.size() + 2;
    if ((float)pt.y >= y && (float)pt.y < y + ExtLibSection::kCloneBtnH) return cloneIdx;
    return -1;
}

// ---------------------------------------------------------------------------
// DrawExternalLibsSection
// ---------------------------------------------------------------------------
void ExplorerManager::DrawExternalLibsSection(ID2D1RenderTarget *ctx, IDWriteFactory *dwrite)
{
    using namespace UI::Theme;

    float sectionY = state_.bottomEdge - extLibs_.TotalHeight();
    const float L  = state_.leftEdge;
    const float R  = state_.rightEdge;

    // Separator line
    {
        ID2D1SolidColorBrush *sepBr = nullptr;
        ctx->CreateSolidColorBrush(ChromeBorder(), &sepBr);
        if (sepBr) {
            ctx->DrawLine(D2D1::Point2F(L, sectionY + 0.5f),
                          D2D1::Point2F(R, sectionY + 0.5f), sepBr, 1.f);
            sepBr->Release();
        }
    }

    // Brushes
    ID2D1SolidColorBrush *bgBr  = nullptr;
    ID2D1SolidColorBrush *txtBr = nullptr;
    ID2D1SolidColorBrush *mutBr = nullptr;
    ID2D1SolidColorBrush *hovBr = nullptr;
    ID2D1SolidColorBrush *accBr = nullptr;
    const Palette &p = GetPalette();
    ctx->CreateSolidColorBrush(p.chromeBgFocused,       &bgBr);
    ctx->CreateSolidColorBrush(PrimaryText(),            &txtBr);
    ctx->CreateSolidColorBrush(MutedText(),              &mutBr);
    ctx->CreateSolidColorBrush(p.explorerToolbarHover,   &hovBr);
    ctx->CreateSolidColorBrush(p.accent,                 &accBr);

    // Section background
    if (bgBr)
        ctx->FillRectangle(D2D1::RectF(L, sectionY, R, state_.bottomEdge), bgBr);

    // Text formats
    IDWriteTextFormat *tfLabel = nullptr;
    IDWriteTextFormat *tfSmall = nullptr;
    IDWriteTextFormat *tfIcon  = nullptr;

    auto mkFmt = [&](const wchar_t *face, float sz, DWRITE_FONT_WEIGHT wt, IDWriteTextFormat **out) {
        dwrite->CreateTextFormat(face, nullptr, wt, DWRITE_FONT_STYLE_NORMAL,
                                 DWRITE_FONT_STRETCH_NORMAL, sz, L"en-us", out);
        if (!*out)
            dwrite->CreateTextFormat(L"Segoe UI", nullptr, wt, DWRITE_FONT_STYLE_NORMAL,
                                     DWRITE_FONT_STRETCH_NORMAL, sz, L"en-us", out);
        if (*out) {
            (*out)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            (*out)->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }
    };

    mkFmt(L"Segoe UI Variable Text", 12.f,  DWRITE_FONT_WEIGHT_SEMI_BOLD, &tfLabel);
    mkFmt(L"Segoe UI Variable Text", 11.5f, DWRITE_FONT_WEIGHT_NORMAL,    &tfSmall);
    dwrite->CreateTextFormat(L"Segoe MDL2 Assets", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 11.f, L"en-us", &tfIcon);
    if (tfIcon) {
        tfIcon->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        tfIcon->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    float y   = sectionY;
    float pad = 10.f;

    // ---- Header row ----
    {
        bool hov = (extLibs_.hoveredItem == 0);
        if (hov && hovBr)
            ctx->FillRectangle(D2D1::RectF(L, y, R, y + ExtLibSection::kHeaderH), hovBr);

        // Chevron  ▼ / ▶
        if (tfIcon && mutBr)
        {
            const wchar_t *chev = extLibs_.expanded ? L"\uE972" : L"\uE974";
            ctx->DrawTextW(chev, 1, tfIcon,
                           D2D1::RectF(L + pad, y, L + pad + 14.f, y + ExtLibSection::kHeaderH),
                           mutBr);
        }

        // Title
        if (tfLabel && txtBr)
        {
            std::wstring title = L"External Libraries";
            ctx->DrawTextW(title.c_str(), (UINT32)title.size(), tfLabel,
                           D2D1::RectF(L + pad + 14.f, y, R - 28.f, y + ExtLibSection::kHeaderH),
                           txtBr, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        // "+" button on the right (always visible, even when collapsed)
        if (tfIcon && (extLibs_.hoveredItem == 0 ? accBr : mutBr))
        {
            ID2D1SolidColorBrush *plusBr = (extLibs_.hoveredItem == 0) ? accBr : mutBr;
            ctx->DrawTextW(L"\uE710", 1, tfIcon,
                           D2D1::RectF(R - 26.f, y, R - 2.f, y + ExtLibSection::kHeaderH),
                           plusBr);
        }
    }
    y += ExtLibSection::kHeaderH;

    if (extLibs_.expanded)
    {
        // ---- Library entries ----
        for (int i = 0; i < (int)extLibs_.cmake.libraries.size(); ++i)
        {
            const auto &lib = extLibs_.cmake.libraries[i];
            bool hov = (extLibs_.hoveredItem == i + 1);
            if (hov && hovBr)
                ctx->FillRectangle(D2D1::RectF(L, y, R, y + ExtLibSection::kItemH), hovBr);

            // Package icon  (E8F1 = Library)
            if (tfIcon && mutBr)
                ctx->DrawTextW(L"\uE8F1", 1, tfIcon,
                               D2D1::RectF(L + pad + 12.f, y, L + pad + 28.f, y + ExtLibSection::kItemH),
                               mutBr);

            // Name  +  dim path on the right
            if (tfSmall && txtBr)
                ctx->DrawTextW(lib.name.c_str(), (UINT32)lib.name.size(), tfSmall,
                               D2D1::RectF(L + pad + 30.f, y, R - pad, y + ExtLibSection::kItemH),
                               txtBr, D2D1_DRAW_TEXT_OPTIONS_CLIP);

            y += ExtLibSection::kItemH;
        }

        // ---- "Add local folder..." row ----
        {
            int addIdx = (int)extLibs_.cmake.libraries.size() + 1;
            bool hov   = (extLibs_.hoveredItem == addIdx);
            if (hov && hovBr)
                ctx->FillRectangle(D2D1::RectF(L, y, R, y + ExtLibSection::kAddBtnH), hovBr);

            if (tfIcon && accBr)
                ctx->DrawTextW(L"\uE710", 1, tfIcon,
                               D2D1::RectF(L + pad + 12.f, y, L + pad + 28.f, y + ExtLibSection::kAddBtnH),
                               accBr);
            if (tfSmall && accBr)
            {
                std::wstring lbl = L"Add local folder\u2026";
                ctx->DrawTextW(lbl.c_str(), (UINT32)lbl.size(), tfSmall,
                               D2D1::RectF(L + pad + 30.f, y, R - pad, y + ExtLibSection::kAddBtnH),
                               accBr, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            y += ExtLibSection::kAddBtnH;
        }

        // ---- "Clone from Git..." row ----
        {
            int cloneIdx = (int)extLibs_.cmake.libraries.size() + 2;
            bool hov     = (extLibs_.hoveredItem == cloneIdx);
            if (hov && hovBr)
                ctx->FillRectangle(D2D1::RectF(L, y, R, y + ExtLibSection::kCloneBtnH), hovBr);

            if (tfIcon && accBr)
                ctx->DrawTextW(L"\uE896", 1, tfIcon,
                               D2D1::RectF(L + pad + 12.f, y, L + pad + 28.f, y + ExtLibSection::kCloneBtnH),
                               accBr);
            if (tfSmall && accBr)
            {
                std::wstring lbl = L"Clone from Git\u2026";
                ctx->DrawTextW(lbl.c_str(), (UINT32)lbl.size(), tfSmall,
                               D2D1::RectF(L + pad + 30.f, y, R - pad, y + ExtLibSection::kCloneBtnH),
                               accBr, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }
    }

    // Release
    auto rel = [](auto *&r) { if (r) { r->Release(); r = nullptr; } };
    rel(bgBr); rel(txtBr); rel(mutBr); rel(hovBr); rel(accBr);
    rel(tfLabel); rel(tfSmall); rel(tfIcon);
}

// ---------------------------------------------------------------------------
// HandleExtLibClick
// ---------------------------------------------------------------------------
void ExplorerManager::HandleExtLibClick(HWND hwnd, int virtualIdx)
{
    if (virtualIdx < 0) return;

    if (virtualIdx == 0)
    {
        extLibs_.expanded = !extLibs_.expanded;
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    int addIdx = (int)extLibs_.cmake.libraries.size() + 1;
    if (virtualIdx == addIdx)
    {
        AddLibraryDialog(hwnd);
        return;
    }

    int cloneIdx = (int)extLibs_.cmake.libraries.size() + 2;
    if (virtualIdx == cloneIdx)
    {
        CloneLibraryDialog(hwnd);
        return;
    }

    // Library row — open folder
    int libIdx = virtualIdx - 1;
    if (libIdx >= 0 && libIdx < (int)extLibs_.cmake.libraries.size())
    {
        const std::wstring &abs = extLibs_.cmake.libraries[libIdx].subdirAbs;
        if (!abs.empty())
            ShellExecuteW(nullptr, L"open", abs.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

// ---------------------------------------------------------------------------
// AddLibraryDialog
// ---------------------------------------------------------------------------
void ExplorerManager::AddLibraryDialog(HWND hwnd)
{
    IFileOpenDialog *dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg))))
        return;

    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Select Library Project Folder");

    if (SUCCEEDED(dlg->Show(hwnd)))
    {
        IShellItem *item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)))
        {
            PWSTR pszPath = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &pszPath)) && pszPath)
            {
                std::wstring libFolder(pszPath);
                CoTaskMemFree(pszPath);

                // Parse the library's CMakeLists.txt (if present)
                std::wstring libCmake = libFolder + L"\\CMakeLists.txt";
                if (!fs::exists(libCmake)) libCmake = libFolder + L"/CMakeLists.txt";
                CmakeProjectInfo libInfo;
                if (fs::exists(libCmake)) libInfo = ParseCmakeLists(libCmake);

                std::wstring libTarget = libInfo.targetName;
                if (libTarget.empty())
                {
                    try { libTarget = fs::path(libFolder).filename().wstring(); }
                    catch (...) { libTarget = L"library"; }
                }

                // Relative path from project root
                std::wstring subdirRel;
                try {
                    subdirRel = fs::relative(libFolder, state_.rootPath).wstring();
                    std::replace(subdirRel.begin(), subdirRel.end(), L'\\', L'/');
                } catch (...) { subdirRel = libFolder; }

                // Detect include directory (fallback to lib root if no include/ subdir)
                std::wstring includeRel = DetectIncludeDir(libFolder, subdirRel);

                // Locate project CMakeLists.txt
                std::wstring projCmake = state_.rootPath + L"\\CMakeLists.txt";
                if (!fs::exists(projCmake)) projCmake = state_.rootPath + L"/CMakeLists.txt";

                std::wstring projTarget = extLibs_.cmake.targetName;
                if (projTarget.empty()) projTarget = L"Nebula";

                if (AddLibraryToCmake(projCmake, projTarget, subdirRel, libTarget, includeRel))
                {
                    Logger::Instance().Log(L"AddLibrary: added " + libTarget + L" to " + projCmake);
                    TriggerCmakeReconfigure(state_.rootPath);
                    ParseExternalLibs();
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                else
                {
                    Logger::Instance().Log(L"AddLibrary: could not modify " + projCmake);
                }
            }
            item->Release();
        }
    }
    dlg->Release();
}

// ---------------------------------------------------------------------------
// CloneLibraryDialog
// ---------------------------------------------------------------------------
void ExplorerManager::CloneLibraryDialog(HWND hwnd)
{
    if (state_.rootPath.empty()) return;

    // Clone destination: <projectRoot>\external
    std::wstring destDir = state_.rootPath + L"\\external";

    std::wstring clonedPath = ShowCloneDialog(hwnd, destDir);
    if (clonedPath.empty()) return;   // cancelled or error

    // Parse the cloned library's CMakeLists.txt
    std::wstring libCmake = clonedPath + L"\\CMakeLists.txt";
    if (!fs::exists(libCmake)) libCmake = clonedPath + L"/CMakeLists.txt";
    CmakeProjectInfo libInfo;
    if (fs::exists(libCmake)) libInfo = ParseCmakeLists(libCmake);

    std::wstring libTarget = libInfo.targetName;
    if (libTarget.empty())
    {
        try { libTarget = fs::path(clonedPath).filename().wstring(); }
        catch (...) { libTarget = L"library"; }
    }

    // Relative path from project root
    std::wstring subdirRel;
    try {
        subdirRel = fs::relative(clonedPath, state_.rootPath).wstring();
        std::replace(subdirRel.begin(), subdirRel.end(), L'\\', L'/');
    } catch (...) { subdirRel = clonedPath; }

    // Detect include directory (fallback to lib root if no include/ subdir)
    std::wstring includeRel = DetectIncludeDir(clonedPath, subdirRel);

    // Locate project CMakeLists.txt
    std::wstring projCmake = state_.rootPath + L"\\CMakeLists.txt";
    if (!fs::exists(projCmake)) projCmake = state_.rootPath + L"/CMakeLists.txt";

    std::wstring projTarget = extLibs_.cmake.targetName;
    if (projTarget.empty()) projTarget = L"Nebula";

    if (AddLibraryToCmake(projCmake, projTarget, subdirRel, libTarget, includeRel))
    {
        Logger::Instance().Log(L"CloneLibrary: added " + libTarget + L" to " + projCmake);
        TriggerCmakeReconfigure(state_.rootPath);
        ParseExternalLibs();
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    else
    {
        Logger::Instance().Log(L"CloneLibrary: could not modify " + projCmake);
    }
}

bool ExplorerManager::InstallLibraryFromGitUrl(HWND hwnd,
                                               const std::wstring &gitUrl,
                                               const std::wstring &displayName,
                                               std::wstring *outError)
{
    auto fail = [&](const std::wstring &msg) -> bool
    {
        if (outError)
            *outError = msg;
        Logger::Instance().Log(L"InstallLibraryFromGitUrl: " + msg);
        return false;
    };

    if (state_.rootPath.empty())
        return fail(L"No project is open. Open a project folder before installing from Marketplace.");
    if (gitUrl.empty())
        return fail(L"Library git URL is empty.");

    std::wstring destDir = state_.rootPath + L"\\external";
    std::wstring repoName = RepoNameFromUrl(gitUrl);
    std::wstring destPath = destDir + L"\\" + repoName;

    try
    {
        fs::create_directories(destDir);
    }
    catch (...)
    {
        return fail(L"Could not create external directory: " + destDir);
    }

    bool alreadyExists = false;
    try
    {
        alreadyExists = fs::exists(destPath);
    }
    catch (...)
    {
        alreadyExists = false;
    }

    if (!alreadyExists)
    {
        std::wstring clonedPath = ShowInstallCloneDialog(hwnd, destDir, gitUrl);
        if (clonedPath.empty())
            return fail(L"Clone cancelled or failed for " + (displayName.empty() ? repoName : displayName));
    }

    std::wstring libCmake = destPath + L"\\CMakeLists.txt";
    if (!fs::exists(libCmake))
        libCmake = destPath + L"/CMakeLists.txt";

    CmakeProjectInfo libInfo;
    if (fs::exists(libCmake))
        libInfo = ParseCmakeLists(libCmake);

    std::wstring libTarget = libInfo.targetName;
    if (libTarget.empty())
        libTarget = repoName;

    std::wstring subdirRel;
    try
    {
        subdirRel = fs::relative(destPath, state_.rootPath).wstring();
    }
    catch (...)
    {
        subdirRel = L"external/" + repoName;
    }
    subdirRel = NormalizeSlashes(subdirRel);
    std::wstring includeRel = DetectIncludeDir(destPath, subdirRel);

    ParseExternalLibs();

    bool alreadyInCmake = false;
    for (const auto &lib : extLibs_.cmake.libraries)
    {
        if (NormalizeSlashes(lib.subdirRel) == subdirRel)
        {
            alreadyInCmake = true;
            break;
        }
    }

    if (!alreadyInCmake)
    {
        std::wstring projCmake = state_.rootPath + L"\\CMakeLists.txt";
        if (!fs::exists(projCmake))
            projCmake = state_.rootPath + L"/CMakeLists.txt";

        std::wstring projTarget = extLibs_.cmake.targetName;
        if (projTarget.empty())
            projTarget = L"Nebula";

        if (!AddLibraryToCmake(projCmake, projTarget, subdirRel, libTarget, includeRel))
            return fail(L"Failed to update CMakeLists.txt for " + libTarget);
    }

    TriggerCmakeReconfigure(state_.rootPath);
    ParseExternalLibs();
    InvalidateRect(hwnd, nullptr, FALSE);
    Logger::Instance().Log(L"InstallLibraryFromGitUrl: installed " + (displayName.empty() ? repoName : displayName));
    return true;
}
