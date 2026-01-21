#include "ExplorerLayoutState.h"

ExplorerLayoutState &GetExplorerLayoutState()
{
    static ExplorerLayoutState state;
    return state;
}
