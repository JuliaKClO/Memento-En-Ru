#include "definition/structuredrichtext.h"

#include <iostream>

namespace
{

bool require(bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
    }
    return condition;
}

}

int main()
{
    const QString pronunciation =
        StructuredRichText::formatLingvoGlossaryForExport(
            QStringLiteral("• [em]сущ.; полигр.\n1) буква m")
        );
    const qsizetype metadataParagraph = pronunciation.indexOf(
        QStringLiteral("<p style=")
    );
    const qsizetype metadataEnd = pronunciation.indexOf(
        QStringLiteral("</p>"), metadataParagraph
    );
    const QString metadata = pronunciation.mid(
        metadataParagraph, metadataEnd - metadataParagraph
    );

    if (!require(!pronunciation.contains(QStringLiteral("[em]")),
                 "Pronunciation should stay in Reading instead of being duplicated in Meaning") ||
        !require(metadataParagraph >= 0 &&
                     metadata.contains(QStringLiteral("margin-left:18px")) &&
                     metadata.contains(QStringLiteral("сущ.")) &&
                     metadata.contains(QStringLiteral("полигр.")),
                 "POS/domain metadata should render below the pronunciation with indentation"))
    {
        return 1;
    }

    const QString phraseVerb =
        StructuredRichText::formatLingvoGlossaryForExport(
            QStringLiteral("• фраз. гл.\n① снимать")
        );
    if (!require(phraseVerb.contains(QStringLiteral("фраз.")) &&
                     !phraseVerb.contains(QStringLiteral("<span style=\"font-weight:700;\">•</span>")),
                 "A part-of-speech heading should not have a bullet"))
    {
        return 1;
    }

    const QString abbreviation =
        StructuredRichText::formatLingvoGlossaryForExport(
            QStringLiteral("• [s]сокр. от second\n1) с, секунда")
        );
    if (!require(abbreviation.contains(
                     QStringLiteral(
                         "<i><span style=\"color:#8A5A00;\">сокр.</span></i>"
                     )) &&
                     abbreviation.contains(QStringLiteral("сокр.")),
                 "сокр. should use its own italic colored style"))
    {
        return 1;
    }

    const QString colloquial =
        StructuredRichText::formatLingvoGlossaryForExport(
            QStringLiteral("② разг. вызывать рвоту")
        );
    if (!require(colloquial.contains(QStringLiteral(
                         "<i><span style=\"color:#9A571C;\">разг.</span></i>"
                     )) &&
                     !colloquial.contains(QStringLiteral("background-color:#E4F3F6; font-weight:700;")),
                 "разг. should be italic without the grammar-label color box"))
    {
        return 1;
    }

    qsizetype remainingExamples = 0;
    const QString americanEnglishUsageLabels =
        StructuredRichText::formatGenericGlossaryForExport(
            QStringLiteral("• разг, дет\nмама, мамочка"),
            remainingExamples
        );
    const QString ordinaryWords =
        StructuredRichText::formatGenericGlossaryForExport(
            QStringLiteral("разговорный детский"),
            remainingExamples
        );
    if (!require(americanEnglishUsageLabels.contains(QStringLiteral(
                         "<span style=\"color:#176B7A; font-style:italic;\">разг</span>"
                     )) &&
                     americanEnglishUsageLabels.contains(QStringLiteral(
                         "<span style=\"color:#176B7A; font-style:italic;\">дет</span>"
                     )) &&
                     americanEnglishUsageLabels.contains(QStringLiteral("мама, мамочка")) &&
                     !ordinaryWords.contains(QStringLiteral("font-style:italic")),
                 "разг and дет should render as usage labels without matching ordinary words"))
    {
        return 1;
    }

    const QString irregularVerb =
        StructuredRichText::formatLingvoGlossaryForExport(
            QStringLiteral(
                "1. гл.\n; прош. вр. knew; прич. прош. вр. known1) знать"
            )
        );
    if (!require(irregularVerb.contains(QStringLiteral("①")) &&
                     irregularVerb.contains(QStringLiteral("знать")) &&
                     !irregularVerb.contains(QStringLiteral("knew")) &&
                     !irregularVerb.contains(QStringLiteral("known1)")),
                 "verb metadata should be removed and the attached sense marker circled"))
    {
        return 1;
    }

    const QString sharedParticipleForm =
        StructuredRichText::formatLingvoGlossaryForExport(
            QStringLiteral(
                "1. гл.; прош. вр., прич. прош. вр. met1) встречаться"
            )
        );
    if (!require(sharedParticipleForm.contains(QStringLiteral("①")) &&
                     sharedParticipleForm.contains(QStringLiteral("встречаться")) &&
                     !sharedParticipleForm.contains(QStringLiteral("met1)")),
                 "a shared past/participle form should be removed before the sense marker"))
    {
        return 1;
    }

    const QString inlineAntonyms =
        StructuredRichText::formatLingvoGlossaryForExport(
            QStringLiteral(
                "① решать\nSyn: determine, resolve\nAnt: fluctuate, hesitate"
            )
        );
    const QString separateAntonyms =
        StructuredRichText::formatLingvoGlossaryForExport(
            QStringLiteral("① решать\nAnt:\nfluctuate, hesitate\n② быть решающим")
        );
    if (!require(inlineAntonyms.contains(QStringLiteral("Syn:")) &&
                     !inlineAntonyms.contains(QStringLiteral("Ant:")) &&
                     !inlineAntonyms.contains(QStringLiteral("fluctuate")) &&
                     !inlineAntonyms.contains(QStringLiteral("hesitate")) &&
                     !separateAntonyms.contains(QStringLiteral("Ant:")) &&
                     !separateAntonyms.contains(QStringLiteral("fluctuate")) &&
                     separateAntonyms.contains(QStringLiteral("②")) &&
                     separateAntonyms.contains(QStringLiteral("быть решающим")),
                 "Ant: labels and their values should be omitted without removing later senses"))
    {
        return 1;
    }

    const QString pastLabel =
        StructuredRichText::formatLingvoGlossaryForExport(
            QStringLiteral("прош. вр. используется в примере")
        );
    if (!require(pastLabel.contains(QStringLiteral(
                         "<i><span style=\"color:#176B7A;\">прош. вр.</span></i>"
                     )) &&
                     pastLabel.contains(QStringLiteral("прош. вр.")),
                 "прош. вр. should use the grammar-label formatting"))
    {
        return 1;
    }

    const QString selectedText = QStringLiteral(
        "говорить; сказать\nSyn: communicate, speak\n• заявлять\nSyn: declare"
    );
    const QString selectedLingvoGlossary =
        StructuredRichText::formatGlossarySelectionForExport(
            selectedText,
            QStringLiteral("LingvoUniversal (En-Ru)")
        );
    const QString selectedOtherDictionary =
        StructuredRichText::formatGlossarySelectionForExport(
            selectedText,
            QStringLiteral("Other dictionary")
        );
    const QString selectedLingvoExample =
        StructuredRichText::formatGlossarySelectionForExport(
            QStringLiteral("- break a leg"),
            QStringLiteral("LingvoUniversal (En-Ru)")
        );
    const qsizetype synonymParagraphStart =
        selectedLingvoGlossary.indexOf(QStringLiteral("Syn:</span>"));
    const qsizetype synonymParagraphOpen =
        selectedLingvoGlossary.lastIndexOf(QStringLiteral("<p style="), synonymParagraphStart);
    const qsizetype synonymParagraphClose =
        selectedLingvoGlossary.indexOf(QStringLiteral("</p>"), synonymParagraphStart);
    const QString synonymParagraph = selectedLingvoGlossary.mid(
        synonymParagraphOpen,
        synonymParagraphClose - synonymParagraphOpen
    );
    DictionaryInfo lingvoInfo;
    lingvoInfo.setName(QStringLiteral("LingvoUniversal (En-Ru)"));
    QJsonArray uiGlossaryContent;
    uiGlossaryContent.append(selectedText);
    const StructuredRichText richTextRenderer;
    const QString uiLightGlossary = richTextRenderer.parse(
        &lingvoInfo,
        uiGlossaryContent,
        Setting::GlossaryStyleBullet,
        nullptr,
        QFont(),
        QColor(Qt::black),
        QColor(Qt::white)
    );
    const QString uiDarkGlossary = richTextRenderer.parse(
        &lingvoInfo,
        uiGlossaryContent,
        Setting::GlossaryStyleBullet,
        nullptr,
        QFont(),
        QColor(Qt::white),
        QColor(QStringLiteral("#202020"))
    );
    const qsizetype uiDarkSynonymStart =
        uiDarkGlossary.indexOf(QStringLiteral("Syn:</span>"));
    const qsizetype uiDarkSynonymOpen = uiDarkGlossary.lastIndexOf(
        QStringLiteral("<p style="), uiDarkSynonymStart
    );
    const qsizetype uiDarkSynonymClose = uiDarkGlossary.indexOf(
        QStringLiteral("</p>"), uiDarkSynonymStart
    );
    const QString uiDarkSynonymParagraph = uiDarkGlossary.mid(
        uiDarkSynonymOpen,
        uiDarkSynonymClose - uiDarkSynonymOpen
    );
    if (!require(selectedLingvoGlossary.contains(QStringLiteral("<p style=")) &&
                     synonymParagraph.contains(QStringLiteral("margin-left:48px")) &&
                     synonymParagraph.contains(QStringLiteral("font-size:85%")) &&
                     synonymParagraph.contains(QStringLiteral(
                         "<i><span style=\"color:#7041A6;\">Syn:</span></i> <span>communicate, speak</span>"
                     )) &&
                     selectedLingvoGlossary.contains(QStringLiteral("заявлять")),
                 "Lingvo Anki export should style synonym rows at 85% with the expected indentation and label-only italics"))
    {
        return 1;
    }

    if (!require(uiLightGlossary.contains(selectedLingvoGlossary),
                 "The light dictionary view should match the Lingvo Anki glossary formatting"))
    {
        return 1;
    }

    if (!require(uiDarkSynonymParagraph.contains(QStringLiteral("margin-left:48px")) &&
                     uiDarkSynonymParagraph.contains(QStringLiteral("font-size:85%")) &&
                     uiDarkSynonymParagraph.contains(QStringLiteral(
                         "<i><span style=\"color:#D8B4FE;\">Syn:</span></i> <span>communicate, speak</span>"
                     )),
                 "The dark dictionary view should preserve synonym indentation and use the dark palette"))
    {
        return 1;
    }

    if (!require(selectedOtherDictionary.contains(QStringLiteral("Syn:")) &&
                     selectedOtherDictionary.contains(QStringLiteral("communicate, speak")) &&
                     selectedOtherDictionary.contains(QStringLiteral("declare")) &&
                     !selectedOtherDictionary.contains(QStringLiteral("#7041A6")),
                 "Generic selections should retain synonym text without Lingvo styling"))
    {
        return 1;
    }

    if (!require(!selectedLingvoExample.contains(QStringLiteral("break a leg")),
                 "Untranslated Lingvo phrase rows should stay filtered from selected glossary export"))
    {
        return 1;
    }

    std::cout << "Lingvo renderer style tests passed.\n";
    return 0;
}
