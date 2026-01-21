#pragma once
#include "ExplorerPlacement.h"

struct ExplorerLayoutState
{
    ExplorerPlacement placement = ExplorerPlacement::Left;
};

ExplorerLayoutState &GetExplorerLayoutState();
