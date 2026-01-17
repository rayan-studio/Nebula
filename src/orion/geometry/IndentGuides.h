#pragma once
#include <vector>

namespace Orion::Geometry
{
    struct IndentGuide
    {
        int startLine = 0;
        int endLine = 0;        // inclusif
        int visualCol = 0;      // colonne visuelle (tabs expand)
        bool active = false;    // guide du bloc contenant le caret
    };
}
