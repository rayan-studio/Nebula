#include "GitDiffDecorations.h"

#include <algorithm>
#include <cwctype>
#include <mutex>
#include <unordered_map>

namespace
{
    std::mutex g_gitDiffMutex;
    std::unordered_map<std::wstring, GitDiffDecorations::LineSets> g_gitDiffByFile;

    std::wstring NormalizePath(const std::wstring &filePath)
    {
        std::wstring out = filePath;
        std::replace(out.begin(), out.end(), L'/', L'\\');
        for (wchar_t &ch : out)
            ch = (wchar_t)towlower(ch);
        return out;
    }
}

void GitDiffDecorations::SetForFile(const std::wstring &filePath, const LineSets &lines)
{
    std::lock_guard<std::mutex> lock(g_gitDiffMutex);
    g_gitDiffByFile[NormalizePath(filePath)] = lines;
}

bool GitDiffDecorations::GetForFile(const std::wstring &filePath, LineSets &outLines)
{
    std::lock_guard<std::mutex> lock(g_gitDiffMutex);
    auto it = g_gitDiffByFile.find(NormalizePath(filePath));
    if (it == g_gitDiffByFile.end())
        return false;
    outLines = it->second;
    return true;
}

void GitDiffDecorations::ClearForFile(const std::wstring &filePath)
{
    std::lock_guard<std::mutex> lock(g_gitDiffMutex);
    g_gitDiffByFile.erase(NormalizePath(filePath));
}

void GitDiffDecorations::ClearAll()
{
    std::lock_guard<std::mutex> lock(g_gitDiffMutex);
    g_gitDiffByFile.clear();
}

