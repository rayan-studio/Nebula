#pragma once

#include <dwrite.h>
#ifdef _MSC_VER
#include <excpt.h>
#endif

#ifndef DWRITE_MAKE_FONT_FEATURE_TAG
#define DWRITE_MAKE_FONT_FEATURE_TAG(tag1, tag2, tag3, tag4) \
    static_cast<DWRITE_FONT_FEATURE_TAG>(DWRITE_MAKE_OPENTYPE_TAG(tag1, tag2, tag3, tag4))
#endif

namespace Orion
{
}
