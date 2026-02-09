#pragma once

#include <string>
#include <vector>

namespace GitDiffDecorations
{
    struct LineSets
    {
        std::vector<int> addedLines;
        std::vector<int> deletedLines;
    };

    void SetForFile(const std::wstring &filePath, const LineSets &lines);
    bool GetForFile(const std::wstring &filePath, LineSets &outLines);
    void ClearForFile(const std::wstring &filePath);
    void ClearAll();
}

