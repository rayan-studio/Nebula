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

    struct SplitRow
    {
        std::wstring leftText;
        std::wstring rightText;
        bool hasLeft = false;
        bool hasRight = false;
        bool leftDeleted = false;
        bool rightAdded = false;
    };

    struct SplitViewData
    {
        std::vector<SplitRow> rows;
    };

    void SetForFile(const std::wstring &filePath, const LineSets &lines);
    bool GetForFile(const std::wstring &filePath, LineSets &outLines);
    void ClearForFile(const std::wstring &filePath);
    void SetSplitForFile(const std::wstring &filePath, const SplitViewData &data);
    bool GetSplitForFile(const std::wstring &filePath, SplitViewData &outData);
    void ClearSplitForFile(const std::wstring &filePath);
    void MarkPendingSplitOpen(const std::wstring &filePath);
    bool ConsumePendingSplitOpen(const std::wstring &filePath);
    void ClearAll();
}
