#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <functional>
#include <windows.h>
#include <cmath>

namespace UI
{
    class Button
    {
    public:
        Button() = default;

        void SetRect(const D2D1_RECT_F& r) { rect_ = r; }
        const D2D1_RECT_F& GetRect() const { return rect_; }

        void SetText(const std::wstring& t) { text_ = t; }

        // Permet d’utiliser ta JetBrains Mono custom (chargée depuis assets).
        // Non-owning: c’est toi qui gères la durée de vie de la collection.
        void SetFontCollection(IDWriteFontCollection* fc) { fontCollection_ = fc; }

        void SetEnabled(bool e) { enabled_ = e; }
        bool IsEnabled() const { return enabled_; }

        void SetCornerRadius(float r) { cornerRadius_ = r; }
        void SetOnClick(std::function<void()> cb) { onClick_ = std::move(cb); }

        bool OnMouseMove(POINT pt)
        {
            bool old = hovered_;
            hovered_ = enabled_ && HitTest(pt);
            return old != hovered_;
        }

        bool OnMouseDown(POINT pt)
        {
            if (!enabled_) return false;
            if (!HitTest(pt)) return false;
            pressed_ = true;
            return true;
        }

        bool OnMouseUp(POINT pt)
        {
            if (!enabled_) { pressed_ = false; return false; }

            bool wasPressed = pressed_;
            pressed_ = false;

            if (wasPressed && HitTest(pt))
            {
                if (onClick_) onClick_();
                return true;
            }
            return false;
        }

        void CancelPress() { pressed_ = false; }

        void Draw(ID2D1RenderTarget* ctx, IDWriteFactory* dwrite)
        {
            if (!ctx || !dwrite) return;

            // ---------------------------
            // Couleurs style Nebula
            // ---------------------------
            const D2D1_COLOR_F bg      = D2D1::ColorF(0.12f, 0.12f, 0.12f, 1.0f);
            const D2D1_COLOR_F bgHover = D2D1::ColorF(0.16f, 0.16f, 0.16f, 1.0f);
            const D2D1_COLOR_F bgDown  = D2D1::ColorF(0.10f, 0.14f, 0.20f, 1.0f);

            const D2D1_COLOR_F border  = D2D1::ColorF(0.28f, 0.28f, 0.28f, 1.0f);
            const D2D1_COLOR_F accent  = D2D1::ColorF(0.30f, 0.50f, 0.80f, 1.0f);

            const D2D1_COLOR_F textCol = enabled_
                ? D2D1::ColorF(0.92f, 0.92f, 0.92f, 1.0f)
                : D2D1::ColorF(0.55f, 0.55f, 0.55f, 1.0f);

            // ---------------------------
            // Pixel snapping (anti flou)
            // ---------------------------
            D2D1_RECT_F fillRect   = SnapRectFill(rect_);
            D2D1_RECT_F strokeRect = SnapRectStroke(rect_);

            // Fill dépend de l’état
            D2D1_COLOR_F fill = bg;
            if (enabled_ && pressed_) fill = bgDown;
            else if (enabled_ && hovered_) fill = bgHover;

            ID2D1SolidColorBrush* bFill = nullptr;
            ID2D1SolidColorBrush* bBorder = nullptr;
            ID2D1SolidColorBrush* bText = nullptr;
            ID2D1SolidColorBrush* bAccent = nullptr;

            ctx->CreateSolidColorBrush(fill, &bFill);
            ctx->CreateSolidColorBrush(border, &bBorder);
            ctx->CreateSolidColorBrush(textCol, &bText);
            ctx->CreateSolidColorBrush(accent, &bAccent);

            // ---------------------------
            // AA uniquement pour le bouton
            // ---------------------------
            D2D1_ANTIALIAS_MODE oldAA = ctx->GetAntialiasMode();
            ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

            // 1) Fond arrondi (lisse)
            D2D1_ROUNDED_RECT rrFill = D2D1::RoundedRect(fillRect, cornerRadius_, cornerRadius_);
            if (bFill) ctx->FillRoundedRectangle(rrFill, bFill);

            // 2) Accent bar hover - plein largeur, coins supérieurs arrondis
            //    Trick: on dessine un rounded rect complet MAIS on le clip sur la zone topStrip
            if (enabled_ && hovered_ && bAccent)
            {
                // hauteur de la bar
                const float stripH = 2.0f;

                // zone clip top strip (pixel aligned)
                D2D1_RECT_F clipStrip = fillRect;
                clipStrip.bottom = clipStrip.top + stripH;

                // clip pour que ça ne remplisse QUE le haut
                ctx->PushAxisAlignedClip(clipStrip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

                // dessiner un rounded rect complet => coins haut arrondis parfaits
                // (les coins bas ne comptent pas car le clip coupe)
                ctx->FillRoundedRectangle(rrFill, bAccent);

                ctx->PopAxisAlignedClip();
            }

            // 3) Contour arrondi (net) : stroke rect sur demi-pixel
            D2D1_ROUNDED_RECT rrStroke = D2D1::RoundedRect(strokeRect, cornerRadius_, cornerRadius_);
            if (bBorder) ctx->DrawRoundedRectangle(rrStroke, bBorder, 1.0f);

            // Restore AA
            ctx->SetAntialiasMode(oldAA);

            // ---------------------------
            // Texte (JetBrains Mono)
            // ---------------------------
            IDWriteTextFormat* fmt = nullptr;

            // Important: utiliser ta collection si fournie, sinon fallback système
            dwrite->CreateTextFormat(
                L"JetBrains Mono",
                fontCollection_,
                DWRITE_FONT_WEIGHT_MEDIUM,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                12.5f,
                L"fr-fr",
                &fmt);

            if (fmt)
            {
                fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

                D2D1_RECT_F tr = fillRect;
                tr.left  += 6.0f;
                tr.right -= 6.0f;

                if (bText)
                {
                    ctx->DrawTextW(
                        text_.c_str(),
                        (UINT32)text_.size(),
                        fmt,
                        tr,
                        bText);
                }

                fmt->Release();
            }

            if (bFill) bFill->Release();
            if (bBorder) bBorder->Release();
            if (bText) bText->Release();
            if (bAccent) bAccent->Release();
        }

    private:
        static float RoundToPixel(float v) { return std::round(v); }

        // Fill: coords entières -> net
        static D2D1_RECT_F SnapRectFill(const D2D1_RECT_F& r)
        {
            return D2D1::RectF(
                RoundToPixel(r.left),
                RoundToPixel(r.top),
                RoundToPixel(r.right),
                RoundToPixel(r.bottom)
            );
        }

        // Stroke 1px: demi-pixel -> net
        static D2D1_RECT_F SnapRectStroke(const D2D1_RECT_F& r)
        {
            D2D1_RECT_F fr = SnapRectFill(r);
            return D2D1::RectF(
                fr.left + 0.5f,
                fr.top + 0.5f,
                fr.right - 0.5f,
                fr.bottom - 0.5f
            );
        }

        bool HitTest(POINT pt) const
        {
            return pt.x >= (LONG)rect_.left && pt.x <= (LONG)rect_.right &&
                   pt.y >= (LONG)rect_.top  && pt.y <= (LONG)rect_.bottom;
        }

        D2D1_RECT_F rect_ = D2D1::RectF(0, 0, 0, 0);
        std::wstring text_;

        bool hovered_ = false;
        bool pressed_ = false;
        bool enabled_ = true;

        float cornerRadius_ = 6.0f;

        // Non-owning pointer (utiliser la même collection que ton Editor)
        IDWriteFontCollection* fontCollection_ = nullptr;

        std::function<void()> onClick_;
    };
}
