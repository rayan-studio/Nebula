#pragma once

#include <dwrite.h>
#include <excpt.h>

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

        __try
        {
            HRESULT hr = layout->HitTestTextPosition(textPosition, FALSE, outX, outY, outMetrics);
            return SUCCEEDED(hr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}
