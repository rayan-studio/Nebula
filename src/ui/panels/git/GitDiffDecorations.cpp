#include "GitDiffDecorations.h"

#include <algorithm>
#include <cwctype>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace
{
    std::mutex g_gitDiffMutex;
    std::unordered_map<std::wstring, GitDiffDecorations::LineSets> g_gitDiffByFile;
    std::unordered_map<std::wstring, GitDiffDecorations::SplitViewData> g_gitSplitByFile;
    std::unordered_set<std::wstring> g_pendingSplitOpen;

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

void GitDiffDecorations::SetSplitForFile(const std::wstring &filePath, const SplitViewData &data)
{
    std::lock_guard<std::mutex> lock(g_gitDiffMutex);
    g_gitSplitByFile[NormalizePath(filePath)] = data;
}

bool GitDiffDecorations::GetSplitForFile(const std::wstring &filePath, SplitViewData &outData)
{
    std::lock_guard<std::mutex> lock(g_gitDiffMutex);
    auto it = g_gitSplitByFile.find(NormalizePath(filePath));
    if (it == g_gitSplitByFile.end())
        return false;
    outData = it->second;
    return true;
}

void GitDiffDecorations::ClearSplitForFile(const std::wstring &filePath)
{
    std::lock_guard<std::mutex> lock(g_gitDiffMutex);
    std::wstring key = NormalizePath(filePath);
    g_gitSplitByFile.erase(key);
    g_pendingSplitOpen.erase(key);
}

void GitDiffDecorations::MarkPendingSplitOpen(const std::wstring &filePath)
{
    std::lock_guard<std::mutex> lock(g_gitDiffMutex);
    g_pendingSplitOpen.insert(NormalizePath(filePath));
}

bool GitDiffDecorations::ConsumePendingSplitOpen(const std::wstring &filePath)
{
    std::lock_guard<std::mutex> lock(g_gitDiffMutex);
    std::wstring key = NormalizePath(filePath);
    auto it = g_pendingSplitOpen.find(key);
    if (it == g_pendingSplitOpen.end())
        return false;
    g_pendingSplitOpen.erase(it);
    return true;
}

void GitDiffDecorations::ClearAll()
{
    std::lock_guard<std::mutex> lock(g_gitDiffMutex);
    g_gitDiffByFile.clear();
    g_gitSplitByFile.clear();
    g_pendingSplitOpen.clear();
}
