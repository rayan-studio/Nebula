#pragma once
#include <string>
#include <vector>

namespace Orion::Geometry
{
    // Configuration d'indentation
    struct IndentConfig
    {
        int tabSize = 2;             // Nombre d'espaces par tab
        float characterWidth = 8.0f; // Largeur d'un caractère
    };

    // Résultat du calcul d'indentation d'une ligne
    struct IndentInfo
    {
        int level;             // Niveau d'indentation (en caractères)
        int visualColumn;      // Colonne visuelle (après expansion des tabs)
        bool isWhitespaceOnly; // Ligne contient uniquement des espaces/tabs
    };

    class IndentationHelper
    {
    public:
        explicit IndentationHelper(const IndentConfig &config);
        const IndentConfig &GetConfig() const { return config_; }

        // Calcule l'indentation d'une ligne donnée
        IndentInfo GetLineIndent(const std::wstring &line) const;

        // Calcule la position X à l'écran pour un niveau d'indentation
        float GetIndentScreenX(int indentLevel, float contentLeft, float scrollOffsetX) const;

        // Détermine si une ligne nécessite un guide d'indentation
        bool ShouldDrawGuide(int lineIndent, int guideLevel) const;

        // Calcule tous les niveaux d'indentation visibles dans une plage de lignes
        std::vector<int> GetVisibleIndentLevels(
            const std::vector<std::wstring> &lines,
            int firstLine,
            int lastLine,
            int maxLevels = 16) const;

    private:
        IndentConfig config_;
    };

} // namespace Orion::Geometry