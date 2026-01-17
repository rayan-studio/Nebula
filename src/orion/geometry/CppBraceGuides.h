#pragma once
#include <string>
#include <vector>

#include "IndentGuides.h"

namespace Orion::Geometry
{
    // Calcule des guides basés sur les blocs { } pour C/C++.
    // - firstLine/lastLine : fenêtre d'analyse (inclusive/exclusive)
    // - activeLine : ligne du caret (pour marquer le guide actif)
    // NOTE: lastLine est EXCLUSIF (comme tes visible ranges).
    std::vector<IndentGuide> ComputeCppBraceGuides(
        const std::vector<std::wstring>& lines,
        int tabSize,
        int activeLine,
        int firstLine,
        int lastLine);
}
