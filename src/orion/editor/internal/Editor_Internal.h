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
    static bool SafeHitTestTextPosition(
        IDWriteTextLayout* layout,
        UINT32 textPosition,
        FLOAT* outX,
        FLOAT* outY,
        DWRITE_HIT_TEST_METRICS* outMetrics)
    {
        if (!layout || !outX || !outY || !outMetrics)
            return false;

    #ifdef _MSC_VER
        __try {
            HRESULT hr = layout->HitTestTextPosition(textPosition, FALSE, outX, outY, outMetrics);
            return SUCCEEDED(hr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    #else
        HRESULT hr = layout->HitTestTextPosition(textPosition, FALSE, outX, outY, outMetrics);
        return SUCCEEDED(hr);
    #endif
    }
}
