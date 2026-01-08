#include "IndentationHelper.h"
#include <algorithm>

namespace Orion::Geometry
{
    IndentationHelper::IndentationHelper(const IndentConfig &config)
        : config_(config)
    {
    }

    IndentInfo IndentationHelper::GetLineIndent(const std::wstring &line) const
    {
        IndentInfo info{};
        info.level = 0;
        info.visualColumn = 0;
        info.isWhitespaceOnly = true;

        for (size_t i = 0; i < line.size(); ++i)
        {
            wchar_t ch = line[i];

            if (ch == L' ')
            {
                info.level++;
                info.visualColumn++;
            }
            else if (ch == L'\t')
            {
                info.level += config_.tabSize;
                info.visualColumn += config_.tabSize;
            }
            else
            {
                // Premier caractère non-blanc trouvé
                info.isWhitespaceOnly = false;
                break;
            }
        }

        // Si on a parcouru toute la ligne sans trouver de caractère non-blanc
        if (info.isWhitespaceOnly && !line.empty())
        {
            info.isWhitespaceOnly = true;
        }
        else if (line.empty())
        {
            info.isWhitespaceOnly = true;
            info.level = 0;
        }

        return info;
    }
    float IndentationHelper::GetIndentScreenX(int indentLevel, float contentLeft, float scrollOffsetX) const
    {
        return contentLeft + ((indentLevel - config_.tabSize) * config_.characterWidth) - scrollOffsetX;
    }

    bool IndentationHelper::ShouldDrawGuide(int lineIndent, int guideLevel) const
    {
        // guideLevel and lineIndent are both expressed in "spaces" (after tab expansion)
        return lineIndent >= guideLevel;
    }
    std::vector<int> IndentationHelper::GetVisibleIndentLevels(
        const std::vector<std::wstring> &lines,
        int firstLine,
        int lastLine,
        int maxLevels) const
    {
        std::vector<int> levels;

        // Parcourir toutes les lignes visibles
        for (int i = firstLine; i < lastLine && i < (int)lines.size(); ++i)
        {
            IndentInfo info = GetLineIndent(lines[i]);

            if (info.level > 0)
            {
                // Ajouter chaque multiple de tabSize jusqu'au niveau actuel
                for (int lvl = config_.tabSize; lvl <= info.level; lvl += config_.tabSize)
                {
                    if (std::find(levels.begin(), levels.end(), lvl) == levels.end())
                    {
                        levels.push_back(lvl);
                    }
                }
            }
        }

        // Trier les niveaux
        std::sort(levels.begin(), levels.end());

        return levels;
    }

} // namespace Orion::Geometry