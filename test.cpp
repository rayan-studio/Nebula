void Editor::DrawWhitespaceIndicators(ID2D1RenderTarget *ctx)
    {
        int firstVisibleLine = (int)(state_.scrollOffsetY / metrics_.lineHeight);
        int lastVisibleLine = (int)((state_.scrollOffsetY + (state_.bottomEdge - state_.topEdge)) / metrics_.lineHeight) + 1;

        firstVisibleLine = (std::max)(0, firstVisibleLine);
        lastVisibleLine = (std::min)((int)state_.lines.size(), lastVisibleLine);

        float contentLeft = state_.leftEdge + metrics_.gutterWidth + metrics_.leftPadding;

        const D2D1_COLOR_F colColorsArr[] = {
            theme_.keyword,
            theme_.string,
            theme_.comment,
            theme_.number,
            theme_.function,
            theme_.variable};
        const size_t colCount = sizeof(colColorsArr) / sizeof(colColorsArr[0]);

        ID2D1SolidColorBrush *colBrushesArr[16] = {0};
        for (size_t idx = 0; idx < colCount; ++idx)
        {
            D2D1_COLOR_F tmp = colColorsArr[idx];
            tmp.a = 0.85f;
            ID2D1SolidColorBrush *b = nullptr;
            ctx->CreateSolidColorBrush(tmp, &b);
            colBrushesArr[idx] = b;
        }

        ID2D1SolidColorBrush *fallbackBrush = nullptr;
        D2D1_COLOR_F fallbackColor = theme_.text;
        fallbackColor.a = 0.5f;
        ctx->CreateSolidColorBrush(fallbackColor, &fallbackBrush);
        if (!fallbackBrush)
        {
            for (size_t idx = 0; idx < colCount; ++idx)
                if (colBrushesArr[idx])
                    colBrushesArr[idx]->Release();
            return;
        }

        for (size_t idx = 0; idx < colCount; ++idx)
            if (colBrushesArr[idx])
                colBrushesArr[idx]->Release();
        if (fallbackBrush)
            fallbackBrush->Release();
    }