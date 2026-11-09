////////////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2026 Ripose
//
// This file is part of Memento.
//
// Memento is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2 of the License.
//
// Memento is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Memento.  If not, see <https://www.gnu.org/licenses/>.
//
////////////////////////////////////////////////////////////////////////////////

#include "definition/structuredrichtext.h"

#include <algorithm>
#include <array>
#include <cmath>

#include <QGuiApplication>
#include <QJsonObject>
#include <QLocale>
#include <QQuickWindow>
#include <QSet>
#include <QStringList>
#include <QUrl>
#include <QVector>

#include "util/utils.h"

/* Begin Local Functions */

namespace
{

/* Number of physical sides represented by CSS box arrays */
constexpr std::size_t BOX_SIDE_COUNT = 4;

/* Array index for the top side of a CSS box */
constexpr std::size_t TOP_SIDE = 0;

/* Array index for the right side of a CSS box */
constexpr std::size_t RIGHT_SIDE = 1;

/* Array index for the bottom side of a CSS box */
constexpr std::size_t BOTTOM_SIDE = 2;

/* Array index for the left side of a CSS box */
constexpr std::size_t LEFT_SIDE = 3;

/* Alpha channel value representing a fully opaque QColor */
constexpr int OPAQUE_ALPHA = 255;

/* Smallest pixel value treated as visible by compatibility rendering */
constexpr double MINIMUM_VISIBLE_PIXELS = 0.001;

/* Pixel width used for the CSS thin border keyword */
constexpr double THIN_BORDER_WIDTH_PIXELS = 1.0;

/* Pixel width used for an unspecified solid border and CSS medium */
constexpr double DEFAULT_BORDER_WIDTH_PIXELS = 3.0;

/* Pixel width used for the CSS thick border keyword */
constexpr double THICK_BORDER_WIDTH_PIXELS = 5.0;

/* Initial selector-element capacity for a structured-content render */
constexpr qsizetype SELECTOR_ELEMENT_RESERVE = 128;

/* Initial ancestor-stack capacity for selector matching */
constexpr qsizetype ELEMENT_STACK_RESERVE = 32;

/* Initial sibling-state stack capacity for margin collapsing */
constexpr qsizetype SIBLING_STACK_RESERVE = 32;

/* Initial nested-list stack capacity */
constexpr qsizetype LIST_STACK_RESERVE = 8;

/* Initial capacity for resolved CSS values cached during a render */
constexpr qsizetype CSS_VALUE_CACHE_RESERVE = 64;

/* Initial character capacity for generated rich-text HTML */
constexpr qsizetype OUTPUT_RESERVE = 4096;

/* Zero-width marker used to indicate a list item boundary in selected text */
constexpr char16_t LIST_ITEM_SENTINEL = u'\u2060';

/* Zero-width marker used to indicate the start of generated list marker text */
constexpr char16_t LIST_MARKER_START_SENTINEL = u'\u2061';

/* Zero-width marker used to indicate the end of generated list marker text */
constexpr char16_t LIST_MARKER_END_SENTINEL = u'\u2062';

/* CSS side names in top, right, bottom, left order */
constexpr std::array<const char *, BOX_SIDE_COUNT> SIDE_NAMES = {
    "top",
    "right",
    "bottom",
    "left"
};

/* Longhand padding properties in CSS box-side order */
constexpr std::array<const char *, BOX_SIDE_COUNT> PADDING_PROPERTIES = {
    "padding-top",
    "padding-right",
    "padding-bottom",
    "padding-left"
};

/* Longhand margin properties in CSS box-side order */
constexpr std::array<const char *, BOX_SIDE_COUNT> MARGIN_PROPERTIES = {
    "margin-top",
    "margin-right",
    "margin-bottom",
    "margin-left"
};

/**
 * @brief Components parsed from a CSS border shorthand.
 */
struct ParsedBorder
{
    /* Border width token */
    QString width;

    /* Border style token */
    QString style;

    /* Border color token */
    QString color;
};

/**
 * @brief Split a CSS value into whitespace-delimited top-level tokens.
 *
 * Whitespace inside quoted strings and function parentheses is preserved.
 *
 * @param value The CSS value to tokenize.
 * @return The top-level CSS tokens in source order.
 */
QStringList splitCssTokens(const QString &value)
{
    QStringList tokens;
    QString current;
    QChar quote;
    bool inQuote = false;
    int parenthesisDepth = 0;

    for (const QChar ch : value.trimmed())
    {
        if (inQuote)
        {
            current += ch;
            if (ch == quote)
            {
                inQuote = false;
            }
        }
        else if (ch == '"' || ch == '\'')
        {
            inQuote = true;
            quote = ch;
            current += ch;
        }
        else if (ch == '(')
        {
            ++parenthesisDepth;
            current += ch;
        }
        else if (ch == ')')
        {
            --parenthesisDepth;
            current += ch;
        }
        else if (ch.isSpace() && parenthesisDepth == 0)
        {
            if (!current.isEmpty())
            {
                tokens.emplaceBack(std::move(current));
                current.clear();
            }
        }
        else
        {
            current += ch;
        }
    }
    if (!current.isEmpty())
    {
        tokens.emplaceBack(std::move(current));
    }
    return tokens;
}

/**
 * @brief Parse supported components from a CSS border shorthand.
 *
 * @param value The border shorthand value.
 * @return The discovered width, style, and color tokens.
 */
ParsedBorder parseBorderShorthand(const QString &value)
{
    /* CSS border styles recognized while classifying shorthand tokens */
    static const QSet<QString> BORDER_STYLES = {
        "none",
        "hidden",
        "dotted",
        "dashed",
        "solid",
        "double",
        "groove",
        "ridge",
        "inset",
        "outset"
    };

    ParsedBorder border;
    QStringList colorTokens;
    for (const QString &token : splitCssTokens(value))
    {
        const QString normalized = token.toLower();
        if (BORDER_STYLES.contains(normalized))
        {
            border.style = normalized;
        }
        else if (normalized == "thin" ||
                 normalized == "medium" ||
                 normalized == "thick" ||
                 normalized.front().isDigit() ||
                 normalized.front() == '.' ||
                 normalized.front() == '+' ||
                 normalized.front() == '-')
        {
            border.width = token;
        }
        else
        {
            colorTokens.emplaceBack(token);
        }
    }
    border.color = colorTokens.join(' ');
    return border;
}

/**
 * @brief Composite a foreground color over a background color.
 *
 * @param foreground The color painted over the background.
 * @param background The existing painted color.
 * @return The resulting premultiplied color, or an invalid color if clear.
 */
QColor compositeColor(const QColor &foreground, const QColor &background)
{
    const double foregroundAlpha = foreground.alphaF();
    const double backgroundAlpha = background.alphaF();
    const double outputAlpha =
        foregroundAlpha + backgroundAlpha * (1.0 - foregroundAlpha);
    if (outputAlpha <= 0.0)
    {
        return {};
    }

    return QColor::fromRgbF(
        (
            foreground.redF() * foregroundAlpha +
            background.redF() * backgroundAlpha *
                (1.0 - foregroundAlpha)
        ) / outputAlpha,
        (
            foreground.greenF() * foregroundAlpha +
            background.greenF() * backgroundAlpha *
                (1.0 - foregroundAlpha)
        ) / outputAlpha,
        (
            foreground.blueF() * foregroundAlpha +
            background.blueF() * backgroundAlpha *
                (1.0 - foregroundAlpha)
        ) / outputAlpha,
        outputAlpha
    );
}

/**
 * @brief Parse a CSS color supported by the compatibility renderer.
 *
 * QColor handles named and hexadecimal colors. This helper additionally
 * accepts comma-separated rgb() and rgba() values used by dictionaries.
 *
 * @param value The CSS color value.
 * @return The parsed color, or an invalid color when unsupported.
 */
QColor parseCssColor(const QString &value)
{
    QColor color = QColor::fromString(value.trimmed());
    if (color.isValid())
    {
        return color;
    }

    /* Pattern for comma-separated integer RGB channels and decimal alpha */
    static const QRegularExpression RGB_COLOR{
        R"(^rgba?\(\s*([+-]?\d+(?:\.\d+)?)\s*,\s*)"
        R"(([+-]?\d+(?:\.\d+)?)\s*,\s*)"
        R"(([+-]?\d+(?:\.\d+)?)\s*)"
        R"((?:,\s*([+-]?\d+(?:\.\d+)?)\s*)?\)$)",
        QRegularExpression::CaseInsensitiveOption
    };
    const QRegularExpressionMatch match =
        RGB_COLOR.match(value.trimmed());
    if (!match.hasMatch())
    {
        return {};
    }

    /* Maximum numeric value of an RGB color channel */
    constexpr double COLOR_CHANNEL_MAXIMUM = 255.0;
    const double red = std::clamp(
        match.captured(1).toDouble(),
        0.0,
        COLOR_CHANNEL_MAXIMUM
    );
    const double green = std::clamp(
        match.captured(2).toDouble(),
        0.0,
        COLOR_CHANNEL_MAXIMUM
    );
    const double blue = std::clamp(
        match.captured(3).toDouble(),
        0.0,
        COLOR_CHANNEL_MAXIMUM
    );
    const double alpha = match.captured(4).isEmpty() ?
        1.0 :
        std::clamp(match.captured(4).toDouble(), 0.0, 1.0);
    return QColor::fromRgbF(
        red / COLOR_CHANNEL_MAXIMUM,
        green / COLOR_CHANNEL_MAXIMUM,
        blue / COLOR_CHANNEL_MAXIMUM,
        alpha
    );
}

/**
 * @brief Check whether a plain glossary line is an ABBYY English/Russian
 * example or phrase row from the flattened Lingvo Yomitan export.
 *
 * The export used by LingvoUniversal stores these rows as plain text rather
 * than retaining the original DSL [ex] marker. Its English/Russian examples
 * use an em dash between the languages.
 */
bool splitBilingualExampleLine(
    const QString &line,
    QString &english,
    QString &separatorText,
    QString &translation)
{
    QString candidate = line;
    candidate.remove(QRegularExpression(
        QStringLiteral(R"(\[/?ex\])"),
        QRegularExpression::CaseInsensitiveOption
    ));
    static const QRegularExpression LATIN{R"([A-Za-z])"};
    static const QRegularExpression CYRILLIC{R"([\x{0400}-\x{04FF}])"};
    static const QRegularExpression LANGUAGE_SEPARATOR{R"(\s*[—–]\s*)"};
    QRegularExpressionMatchIterator separators =
        LANGUAGE_SEPARATOR.globalMatch(candidate);
    bool foundSeparator = false;
    while (separators.hasNext())
    {
        const QRegularExpressionMatch separator = separators.next();
        const QString englishPart = candidate.first(
            separator.capturedStart()
        ).trimmed();
        const QString russianPart = candidate.sliced(
            separator.capturedEnd()
        ).trimmed();
        if (!englishPart.isEmpty() && !russianPart.isEmpty() &&
            LATIN.match(englishPart).hasMatch() &&
            CYRILLIC.match(russianPart).hasMatch())
        {
            english = englishPart;
            separatorText = separator.captured().trimmed();
            translation = russianPart;
            foundSeparator = true;
        }
    }
    return foundSeparator;
}

bool isBilingualExampleLine(const QString &line)
{
    QString english;
    QString separator;
    QString translation;
    return splitBilingualExampleLine(
        line, english, separator, translation
    );
}

bool startsGlossaryDefinitionHeading(const QString &line)
{
    static const QRegularExpression HEADING{
        R"(^\s*(?:(?:VIII|VII|VI|IV|V|III|II|IX|X|I)\)|\d+[.)]|[①-⑳]|[a-zа-яё]\))(?=\s|$))",
        QRegularExpression::CaseInsensitiveOption
    };
    return HEADING.match(line).hasMatch();
}

bool startsGlossaryRomanSectionHeading(const QString &line)
{
    static const QRegularExpression ROMAN_SECTION_HEADER{
        R"(^\s*(VIII|VII|VI|IV|V|III|II|IX|X|I)(?:\)|[ \t]+\[[^\]\r\n]+\]|[ \t]+(?:n|v|adj|adv|pron|prep|conj|interj|det|aux|art|num)\b|$))",
        QRegularExpression::CaseInsensitiveOption
    };
    return ROMAN_SECTION_HEADER.match(line).hasMatch();
}

bool startsGlossarySectionHeading(const QString &line)
{
    return startsGlossaryDefinitionHeading(line) ||
        startsGlossaryRomanSectionHeading(line);
}

QString separateInlineRomanPronunciationSectionHeaders(const QString &text)
{
    static const QRegularExpression INLINE_ROMAN_HEADER{
        R"(([.!?])[ \t]+(VIII|VII|VI|IV|V|III|II|IX|X|I)\)?[ \t]+(\[[^\]\r\n]+\])[ \t]*$)",
        QRegularExpression::CaseInsensitiveOption
    };
    QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    for (qsizetype i = 0; i < lines.size(); ++i)
    {
        const QRegularExpressionMatch match =
            INLINE_ROMAN_HEADER.match(lines[i]);
        if (!match.hasMatch())
        {
            continue;
        }

        const QString sectionHeader =
            lines[i].sliced(match.capturedStart(2)).trimmed();
        lines[i] = lines[i].first(match.capturedStart(2)).trimmed();
        lines.insert(i + 1, sectionHeader);
        ++i;
    }
    return lines.join(QLatin1Char('\n'));
}

QJsonValue localizeGlossarySectionHeading(const QJsonValue &value)
{
    if (value.isString())
    {
        QString text = value.toString();
        static const QRegularExpression USAGE_HEADING{
            R"(^([ \t]*)(USAGE:))",
            QRegularExpression::CaseInsensitiveOption
        };
        static const QRegularExpression CHOICE_HEADING{
            R"(^([ \t]*(?:[•·][ \t]*)?)(CHOICE OF WORDS:))",
            QRegularExpression::CaseInsensitiveOption
        };
        text.replace(USAGE_HEADING, QStringLiteral("\\1УПОТРЕБЛЕНИЕ:"));
        text.replace(CHOICE_HEADING, QStringLiteral("\\1ВЫБОР СЛОВА:"));
        return text;
    }
    if (value.isArray())
    {
        QJsonArray localized;
        for (const QJsonValue &child : value.toArray())
        {
            localized.append(localizeGlossarySectionHeading(child));
        }
        return localized;
    }
    if (value.isObject())
    {
        QJsonObject object = value.toObject();
        if (object.contains(QStringLiteral("content")))
        {
            object.insert(
                QStringLiteral("content"),
                localizeGlossarySectionHeading(
                    object.value(QStringLiteral("content"))
                )
            );
        }
        if (object.value(QStringLiteral("text")).isString())
        {
            object.insert(
                QStringLiteral("text"),
                localizeGlossarySectionHeading(
                    object.value(QStringLiteral("text"))
                )
            );
        }
        return object;
    }
    return value;
}

const QRegularExpression &genericGlossaryLabelPattern()
{
    static const QRegularExpression LABELS{
        R"((?<![\p{L}])(?:(?:ед\.?\s+прош\.?\s+от\.?(?![\p{L}\d]))|(?:сравн\.?\s+ст\.?(?![\p{L}\d]))|(?:см\.?\s*тж\.?(?![\p{L}\d]))|(?:конн\.?\s+спорт\.?(?![\p{L}\d]))|(?:союз(?=[а-яё]\)))|(?:уст\.(?=[а-яё]\)))|(?:n|v|adj|adv|ad|pron|prp|prep|conj|interj|det|aux|art|num|abbr|intr|tr|vt|vi|pl|sing|count|uncount|sl)\.?(?![A-Za-z\d])|(?:сущ|прил|нареч|гл|мест|предл|союз|межд|числ|предик|прич|дееприч|част|тж|груб|уст|жарг|разг|дет|амер|мор|рел|воен|стр|фр|нем|библ|кул)\.?(?![\p{L}\d])|(?<=,\s)груб(?=[А-Яа-яЁё])))",
        QRegularExpression::CaseInsensitiveOption
    };
    return LABELS;
}

QString stripRussianGenderMarkers(const QString &text)
{
    // AmericanEnglish may encode noun gender with a standalone Cyrillic or
    // Latin abbreviation (for example, "м (wine)" or "m (wine)") after a
    // Cyrillic gloss. Keep units such as "5 m" and ordinary words intact by
    // requiring a Cyrillic letter before the marker and a gloss boundary
    // after it.
    static const QRegularExpression GENDER_MARKER{
        R"((?<=[А-Яа-яЁё])[ \t]+(?:[жмс]|[mfn])\.?(?=[ \t]*(?:[,;:()]|\]|\r?\n|$)))",
        QRegularExpression::CaseInsensitiveOption
    };
    QString cleaned = text;
    cleaned.replace(GENDER_MARKER, QString());
    return cleaned;
}

QJsonValue stripAmericanEnglishStructuredText(const QJsonValue &value)
{
    if (value.isString())
    {
        return stripRussianGenderMarkers(value.toString());
    }
    if (value.isArray())
    {
        QJsonArray cleaned;
        const QJsonArray array = value.toArray();
        for (const QJsonValue &child : array)
        {
            cleaned.append(stripAmericanEnglishStructuredText(child));
        }
        return cleaned;
    }
    if (value.isObject())
    {
        QJsonObject object = value.toObject();
        if (object.contains(QStringLiteral("content")))
        {
            object.insert(
                QStringLiteral("content"),
                stripAmericanEnglishStructuredText(
                    object.value(QStringLiteral("content"))
                )
            );
        }
        if (object.value(QStringLiteral("text")).isString())
        {
            object.insert(
                QStringLiteral("text"),
                stripRussianGenderMarkers(
                    object.value(QStringLiteral("text")).toString()
                )
            );
        }
        return object;
    }
    return value;
}

QJsonArray stripAmericanEnglishGlossaryMarkers(const QJsonArray &definitions)
{
    QJsonArray cleaned;
    for (const QJsonValue &definition : definitions)
    {
        if (definition.isString())
        {
            cleaned.append(stripRussianGenderMarkers(definition.toString()));
            continue;
        }
        if (!definition.isObject())
        {
            cleaned.append(definition);
            continue;
        }

        QJsonObject object = definition.toObject();
        const QString type = object.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("structured-content"))
        {
            object.insert(
                QStringLiteral("content"),
                stripAmericanEnglishStructuredText(
                    object.value(QStringLiteral("content"))
                )
            );
        }
        else if (type == QStringLiteral("text") &&
                 object.value(QStringLiteral("text")).isString())
        {
            object.insert(
                QStringLiteral("text"),
                stripRussianGenderMarkers(
                    object.value(QStringLiteral("text")).toString()
                )
            );
        }
        cleaned.append(object);
    }
    return cleaned;
}

QString formatGenericGlossaryLabels(
    const QString &text,
    const QString &labelColor)
{
    QString out;
    qsizetype position = 0;
    QRegularExpressionMatchIterator matches =
        genericGlossaryLabelPattern().globalMatch(text);
    while (matches.hasNext())
    {
        const QRegularExpressionMatch match = matches.next();
        out += text.sliced(position, match.capturedStart() - position)
                   .toHtmlEscaped();
        out += QStringLiteral(
            "<span style=\"color:%1; font-style:italic;\">%2</span>"
        ).arg(labelColor, match.captured().toHtmlEscaped());
        if (match.capturedEnd() < text.size() &&
            !text.at(match.capturedEnd()).isSpace() &&
            (text.at(match.capturedEnd()).isLetter() ||
             text.at(match.capturedEnd()) == QLatin1Char('(')))
        {
            out += QLatin1Char(' ');
        }
        position = match.capturedEnd();
    }
    out += text.sliced(position).toHtmlEscaped();
    return out;
}

void appendStyledGenericGlossaryText(
    const QString &text,
    QJsonArray &out)
{
    qsizetype position = 0;
    QRegularExpressionMatchIterator matches =
        genericGlossaryLabelPattern().globalMatch(text);
    while (matches.hasNext())
    {
        const QRegularExpressionMatch match = matches.next();
        if (match.capturedStart() > position)
        {
            out.append(text.sliced(
                position, match.capturedStart() - position
            ));
        }

        QJsonObject data;
        data.insert(
            QStringLiteral("class"),
            QStringLiteral("memento-grammar-label")
        );
        QJsonObject label;
        label.insert(QStringLiteral("tag"), QStringLiteral("span"));
        label.insert(QStringLiteral("data"), data);
        QJsonObject style;
        style.insert(QStringLiteral("color"), QStringLiteral("#176B7A"));
        style.insert(QStringLiteral("fontStyle"), QStringLiteral("italic"));
        label.insert(QStringLiteral("style"), style);
        label.insert(QStringLiteral("content"), match.captured());
        out.append(label);
        if (match.capturedEnd() == text.size() ||
            (!text.at(match.capturedEnd()).isSpace() &&
             (text.at(match.capturedEnd()).isLetter() ||
              text.at(match.capturedEnd()) == QLatin1Char('('))))
        {
            out.append(QStringLiteral(" "));
        }
        position = match.capturedEnd();
    }

    if (position == 0)
    {
        out.append(text);
    }
    else if (position < text.size())
    {
        out.append(text.sliced(position));
    }
}

bool stripLeadingGlossaryPronunciation(QString &text)
{
    static const QRegularExpression PRONUNCIATION_HEADER{
        R"(^([ \t]*(?:[•·][ \t]*)?)(\[[^\]\r\n]+\](?:(?:[ \t]*(?:,|/)[ \t]*|[ \t]+)?\[[^\]\r\n]+\])*)([ \t]*)(.*)$)"
    };
    QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    for (QString &line : lines)
    {
        if (line.trimmed().isEmpty())
        {
            continue;
        }
        const QRegularExpressionMatch match =
            PRONUNCIATION_HEADER.match(line);
        if (!match.hasMatch())
        {
            return false;
        }
        const QString transcription = match.captured(2).trimmed();
        if (transcription.compare(QStringLiteral("[ex]"),
                                  Qt::CaseInsensitive) == 0 ||
            transcription.compare(QStringLiteral("[/ex]"),
                                  Qt::CaseInsensitive) == 0)
        {
            return false;
        }

        line = match.captured(1) + match.captured(3) + match.captured(4);
        text = lines.join(QLatin1Char('\n'));
        return true;
    }
    return false;
}

QString formatBilingualExampleHtml(
    const QString &line,
    const QString &translationColor)
{
    QString english;
    QString separator;
    QString translation;
    if (!splitBilingualExampleLine(line, english, separator, translation))
    {
        return line.toHtmlEscaped();
    }

    return QStringLiteral(
        "<span style=\"font-weight:normal;\">%1 %2</span> "
        "<span style=\"color:%3;\">%4</span>"
    )
        .arg(
            english.toHtmlEscaped(),
            separator.toHtmlEscaped(),
            translationColor,
            translation.toHtmlEscaped()
        );
}

bool isLingvoExampleLine(const QString &line)
{
    static const QRegularExpression NUMBERED_RUSSIAN_EXAMPLE{
        R"(^\s*(?:[①-⑳]|\d{1,3}[.)]?)\s*[—–]\s*[\x{0400}-\x{04FF}])"
    };
    if (line.contains("[ex]", Qt::CaseInsensitive) ||
        line.contains("[/ex]", Qt::CaseInsensitive))
    {
        return true;
    }
    return isBilingualExampleLine(line) ||
        NUMBERED_RUSSIAN_EXAMPLE.match(line).hasMatch();
}

bool isEnglishLingvoExampleSentence(const QString &line)
{
    static const QRegularExpression ENGLISH_WORD{R"([A-Za-z][A-Za-z'’.-]*)"};
    static const QRegularExpression CYRILLIC{R"([\x{0400}-\x{04FF}])"};
    static const QRegularExpression ENGLISH_SENTENCE_END{
        R"((?:[.!?]|\))\s*$)"
    };
    static const QRegularExpression STARTS_WITH_ENGLISH_CAPITAL{
        R"(^\s*[A-Z])"
    };
    if (!STARTS_WITH_ENGLISH_CAPITAL.match(line).hasMatch() ||
        !ENGLISH_SENTENCE_END.match(line).hasMatch() ||
        CYRILLIC.match(line).hasMatch())
    {
        return false;
    }

    qsizetype wordCount = 0;
    auto words = ENGLISH_WORD.globalMatch(line);
    while (words.hasNext())
    {
        words.next();
        ++wordCount;
        if (wordCount >= 5)
        {
            return true;
        }
    }
    return false;
}

QString separateLetterFromOpeningParenthesis(const QString &text)
{
    static const QRegularExpression LETTER_BEFORE_OPENING_PARENTHESIS{
        R"((?<=\p{L})\()"
    };
    QString separated = text;
    separated.replace(
        LETTER_BEFORE_OPENING_PARENTHESIS, QStringLiteral(" (")
    );
    return separated;
}

bool isUntranslatedDashPhraseRow(const QString &line)
{
    static const QRegularExpression ENGLISH_PHRASE_ROW{
        R"(^[-–—]\s*[A-Za-z][A-Za-z'’.-]*(?:\s+.*)?$)"
    };
    static const QRegularExpression CYRILLIC{R"([\x{0400}-\x{04FF}])"};
    const QString candidate = line.trimmed();
    return ENGLISH_PHRASE_ROW.match(candidate).hasMatch() &&
        !CYRILLIC.match(candidate).hasMatch();
}

QString suppressUntranslatedPhraseLists(const QString &text)
{
    constexpr qsizetype MINIMUM_PHRASE_LIST_SIZE = 1;
    static const QRegularExpression EMPTY_NUMBERED_SENSE{
        R"(^[ \t]*\d{1,2}\)[ \t]*$)"
    };
    QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    for (qsizetype i = 0; i < lines.size();)
    {
        if (!isUntranslatedDashPhraseRow(lines[i]))
        {
            ++i;
            continue;
        }

        QVector<qsizetype> phraseRows;
        qsizetype end = i;
        while (end < lines.size())
        {
            if (isUntranslatedDashPhraseRow(lines[end]))
            {
                phraseRows.append(end);
            }
            else if (!lines[end].trimmed().isEmpty())
            {
                break;
            }
            ++end;
        }

        if (phraseRows.size() >= MINIMUM_PHRASE_LIST_SIZE)
        {
            qsizetype previous = i;
            while (previous > 0 && lines[previous - 1].trimmed().isEmpty())
            {
                --previous;
            }
            const qsizetype eraseStart = previous > 0 &&
                    EMPTY_NUMBERED_SENSE.match(lines[previous - 1]).hasMatch() ?
                previous - 1 : i;
            lines.erase(lines.begin() + eraseStart, lines.begin() + end);
            i = eraseStart;
            continue;
        }
        i = qMax(i + 1, end);
    }
    return lines.join(QLatin1Char('\n'));
}

QString splitAmericanEnglishAdjectiveSenseHeadings(const QString &text)
{
    const QChar ADJECTIVE_HEADING_MARKER(0xE000);
    static const QRegularExpression ADJECTIVE_SECTION{
        R"(^([ \t]*)(\d{1,2}\.)[ \t]*(a\.?)(?:[ \t]+([\x{0400}-\x{04FF}].*))?[ \t]*$)",
        QRegularExpression::CaseInsensitiveOption
    };
    static const QRegularExpression NUMBERED_SENSE_LINE{
        R"(^[ \t]*(?:\d{1,2}\)|[①-⑳])(?=\s|$))"
    };
    QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    for (qsizetype i = 0; i < lines.size(); ++i)
    {
        const QRegularExpressionMatch match =
            ADJECTIVE_SECTION.match(lines[i]);
        if (!match.hasMatch())
        {
            continue;
        }

        const QString indentation = match.captured(1);
        const QString sectionNumber = match.captured(2);
        const QString partOfSpeech = match.captured(3);
        const QString gloss = match.captured(4);
        if (gloss.isEmpty())
        {
            // Some AmericanEnglish entries place the POS abbreviation alone
            // on its line, followed by a separately numbered definition.
            // Require that structure so an article "a" stays ordinary text.
            if (i + 1 < lines.size() &&
                NUMBERED_SENSE_LINE.match(lines[i + 1]).hasMatch())
            {
                lines[i] = indentation + sectionNumber + QLatin1Char(' ') +
                    partOfSpeech + ADJECTIVE_HEADING_MARKER;
            }
            continue;
        }
        lines[i] = indentation + sectionNumber + QLatin1Char(' ') +
            partOfSpeech + ADJECTIVE_HEADING_MARKER;
        lines.insert(i + 1, indentation + QStringLiteral("1) ") + gloss);
        ++i;
    }
    return lines.join(QLatin1Char('\n'));
}

bool isExampleContainer(
    const QString &tag,
    const QString &className,
    const QString &contentName = {})
{
    static const QRegularExpression EXAMPLE_CLASS{
        R"((?:^|[\s_-])(?:examples?|ex|sentences?|sentence[-_ ]?examples?|examples?[-_ ]?sentences?)(?:$|[\s_-]))",
        QRegularExpression::CaseInsensitiveOption
    };
    const QString normalizedTag = tag.toLower();
    const QString normalizedContent = contentName.trimmed().toLower();
    return normalizedTag == QStringLiteral("blockquote") ||
        ((normalizedTag == QStringLiteral("div") ||
          normalizedTag == QStringLiteral("p") ||
          normalizedTag == QStringLiteral("span")) &&
         EXAMPLE_CLASS.match(className).hasMatch()) ||
        ((normalizedTag == QStringLiteral("div") ||
          normalizedTag == QStringLiteral("p")) &&
         normalizedContent == QStringLiteral("example-sentence"));
}

bool isUsageGroupContainer(const QString &tag, const QString &className)
{
    return (tag.compare(QStringLiteral("div"), Qt::CaseInsensitive) == 0 ||
            tag.compare(QStringLiteral("p"), Qt::CaseInsensitive) == 0) &&
        className.compare(
            QStringLiteral("memento-usage-group"),
            Qt::CaseInsensitive
        ) == 0;
}

bool isExampleStructuredNode(const QJsonValue &value)
{
    if (!value.isObject())
    {
        return false;
    }

    const QJsonObject obj = value.toObject();
    const QJsonObject data = obj.value(QStringLiteral("data")).toObject();
    return isExampleContainer(
        obj.value(QStringLiteral("tag")).toString(),
        data.value(QStringLiteral("class")).toString(),
        data.value(QStringLiteral("content")).toString()
    );
}

bool isExampleGroupSeparator(const QJsonValue &value)
{
    if (value.isString())
    {
        return value.toString().trimmed().isEmpty();
    }
    if (!value.isObject())
    {
        return false;
    }

    const QJsonObject obj = value.toObject();
    return obj.value(QStringLiteral("tag")).toString().compare(
               QStringLiteral("br"), Qt::CaseInsensitive
           ) == 0 ||
        (obj.value(QStringLiteral("type")).toString() ==
             QStringLiteral("text") &&
         obj.value(QStringLiteral("text")).toString().trimmed().isEmpty());
}

bool topLevelStructuredExampleNode(
    const QJsonValue &value,
    QJsonValue &exampleNode)
{
    if (!value.isObject())
    {
        return false;
    }

    const QJsonObject entry = value.toObject();
    if (entry.value(QStringLiteral("type")).toString() !=
        QStringLiteral("structured-content"))
    {
        return false;
    }

    const QJsonValue content = entry.value(QStringLiteral("content"));
    if (isExampleStructuredNode(content))
    {
        exampleNode = content;
        return true;
    }
    if (!content.isArray())
    {
        return false;
    }

    QJsonValue foundExample;
    for (const QJsonValue &child : content.toArray())
    {
        if (isExampleGroupSeparator(child))
        {
            continue;
        }
        if (!isExampleStructuredNode(child) || !foundExample.isUndefined())
        {
            return false;
        }
        foundExample = child;
    }
    if (foundExample.isUndefined())
    {
        return false;
    }

    exampleNode = foundExample;
    return true;
}

QJsonObject structuredExampleDivider()
{
    QJsonObject data;
    data.insert(QStringLiteral("class"), QStringLiteral("memento-divider"));

    QJsonObject style;
    style.insert(QStringLiteral("borderStyle"), QStringLiteral("solid"));
    style.insert(QStringLiteral("borderWidth"), QStringLiteral("1px 0 0 0"));
    style.insert(QStringLiteral("paddingTop"), QStringLiteral("5px"));
    style.insert(QStringLiteral("paddingBottom"), QStringLiteral("5px"));

    QJsonObject divider;
    divider.insert(QStringLiteral("tag"), QStringLiteral("div"));
    divider.insert(QStringLiteral("data"), data);
    divider.insert(QStringLiteral("style"), style);
    divider.insert(QStringLiteral("content"), QString());
    return divider;
}

QJsonArray groupAdjacentTopLevelStructuredExamples(const QJsonArray &array)
{
    QJsonArray grouped;
    for (qsizetype i = 0; i < array.size();)
    {
        QJsonValue firstExample;
        if (!topLevelStructuredExampleNode(array[i], firstExample))
        {
            grouped.append(array[i]);
            ++i;
            continue;
        }

        QJsonArray examples;
        examples.append(firstExample);
        qsizetype exampleCount = 1;
        qsizetype next = i + 1;
        while (next < array.size())
        {
            QJsonValue example;
            if (topLevelStructuredExampleNode(array[next], example))
            {
                examples.append(structuredExampleDivider());
                examples.append(example);
                ++exampleCount;
                ++next;
                continue;
            }
            if (isExampleGroupSeparator(array[next]) &&
                next + 1 < array.size() &&
                topLevelStructuredExampleNode(array[next + 1], example))
            {
                examples.append(structuredExampleDivider());
                examples.append(example);
                ++exampleCount;
                next += 2;
                continue;
            }
            break;
        }

        if (exampleCount == 1)
        {
            grouped.append(array[i]);
            ++i;
            continue;
        }

        QJsonArray groupContent;
        for (const QJsonValue &example : examples)
        {
            groupContent.append(example);
        }
        QJsonObject groupData;
        groupData.insert(
            QStringLiteral("class"), QStringLiteral("memento-example-group")
        );
        QJsonObject groupNode;
        groupNode.insert(QStringLiteral("tag"), QStringLiteral("div"));
        groupNode.insert(QStringLiteral("data"), groupData);
        groupNode.insert(QStringLiteral("content"), groupContent);

        QJsonObject groupedEntry = array[i].toObject();
        groupedEntry.insert(QStringLiteral("content"), groupNode);
        grouped.append(groupedEntry);
        i = next;
    }
    return grouped;
}

QString structuredNodeText(const QJsonValue &value)
{
    if (value.isString())
    {
        return value.toString();
    }
    if (value.isArray())
    {
        QString text;
        for (const QJsonValue &child : value.toArray())
        {
            text += structuredNodeText(child);
        }
        return text;
    }
    if (!value.isObject())
    {
        return {};
    }

    const QJsonObject obj = value.toObject();
    const QString tag = obj.value(QStringLiteral("tag")).toString()
        .toLower();
    if (tag == QStringLiteral("br"))
    {
        return QString(QLatin1Char('\n'));
    }

    QString text = structuredNodeText(
        obj.value(QStringLiteral("text"))
    );
    text += structuredNodeText(obj.value(QStringLiteral("content")));
    static const QSet<QString> BLOCK_TAGS{
        QStringLiteral("blockquote"), QStringLiteral("div"),
        QStringLiteral("li"), QStringLiteral("ol"),
        QStringLiteral("p"), QStringLiteral("ul")
    };
    if (BLOCK_TAGS.contains(tag) && !text.isEmpty() &&
        !text.endsWith(QLatin1Char('\n')))
    {
        text += QLatin1Char('\n');
    }
    return text;
}

bool isUntranslatedStructuredPhraseRow(const QJsonValue &value)
{
    const QString text = structuredNodeText(value).trimmed();
    return !text.isEmpty() && isUntranslatedDashPhraseRow(text);
}

bool isStructuredPhraseListSeparator(const QJsonValue &value)
{
    if (value.isString())
    {
        return value.toString().trimmed().isEmpty();
    }
    if (!value.isObject())
    {
        return false;
    }
    return value.toObject().value(QStringLiteral("tag")).toString()
        .compare(QStringLiteral("br"), Qt::CaseInsensitive) == 0;
}

QJsonArray suppressUntranslatedStructuredPhraseLists(const QJsonArray &array)
{
    constexpr qsizetype MINIMUM_PHRASE_LIST_SIZE = 1;
    static const QRegularExpression EMPTY_NUMBERED_SENSE{
        R"(^\s*\d{1,2}\)\s*$)"
    };
    QSet<qsizetype> removedIndices;
    for (qsizetype i = 0; i < array.size();)
    {
        if (!isUntranslatedStructuredPhraseRow(array[i]))
        {
            ++i;
            continue;
        }

        QVector<qsizetype> phraseRows;
        qsizetype end = i;
        while (end < array.size())
        {
            if (isUntranslatedStructuredPhraseRow(array[end]))
            {
                phraseRows.append(end);
                ++end;
                continue;
            }

            if (isStructuredPhraseListSeparator(array[end]) &&
                end + 1 < array.size() &&
                isUntranslatedStructuredPhraseRow(array[end + 1]))
            {
                ++end;
                continue;
            }
            break;
        }

        if (phraseRows.size() >= MINIMUM_PHRASE_LIST_SIZE)
        {
            for (qsizetype index = i; index < end; ++index)
            {
                removedIndices.insert(index);
            }

            qsizetype preceding = i;
            while (preceding > 0 &&
                   isStructuredPhraseListSeparator(array[preceding - 1]))
            {
                --preceding;
            }
            if (preceding > 0 && EMPTY_NUMBERED_SENSE.match(
                    structuredNodeText(array[preceding - 1]).trimmed()
                ).hasMatch())
            {
                removedIndices.insert(preceding - 1);
                for (qsizetype separator = preceding; separator < i; ++separator)
                {
                    removedIndices.insert(separator);
                }
            }
        }
        i = qMax(i + 1, end);
    }

    QJsonArray filtered;
    for (qsizetype i = 0; i < array.size(); ++i)
    {
        if (!removedIndices.contains(i))
        {
            filtered.append(array[i]);
        }
    }
    return filtered;
}

QJsonObject genericExampleGroup(const QStringList &examples)
{
    QJsonArray content;
    for (qsizetype i = 0; i < examples.size(); ++i)
    {
        if (i > 0)
        {
            QJsonObject dividerData;
            dividerData.insert(
                QStringLiteral("class"), QStringLiteral("memento-divider")
            );

            QJsonObject dividerStyle;
            dividerStyle.insert(QStringLiteral("borderColor"), QStringLiteral("#C8C8C8"));
            dividerStyle.insert(QStringLiteral("borderStyle"), QStringLiteral("solid"));
            dividerStyle.insert(QStringLiteral("borderWidth"), QStringLiteral("1px 0 0 0"));
            dividerStyle.insert(QStringLiteral("paddingTop"), QStringLiteral("5px"));
            dividerStyle.insert(QStringLiteral("paddingBottom"), QStringLiteral("5px"));

            QJsonObject divider;
            divider.insert(QStringLiteral("tag"), QStringLiteral("div"));
            divider.insert(QStringLiteral("data"), dividerData);
            divider.insert(QStringLiteral("style"), dividerStyle);
            divider.insert(QStringLiteral("content"), QString());
            content.append(divider);
        }

        QString english;
        QString separator;
        QString translation;
        if (splitBilingualExampleLine(
                examples[i], english, separator, translation))
        {
            content.append(english);
            content.append(QStringLiteral(" %1 ").arg(separator));

            QJsonObject translationData;
            translationData.insert(
                QStringLiteral("class"),
                QStringLiteral("memento-translation")
            );
            QJsonObject translationStyle;
            translationStyle.insert(
                QStringLiteral("color"), QStringLiteral("#666666")
            );

            QJsonObject translationNode;
            translationNode.insert(QStringLiteral("tag"), QStringLiteral("span"));
            translationNode.insert(QStringLiteral("data"), translationData);
            translationNode.insert(QStringLiteral("style"), translationStyle);
            translationNode.insert(QStringLiteral("content"), translation);
            content.append(translationNode);
        }
        else
        {
            content.append(examples[i]);
        }
    }

    QJsonObject data;
    data.insert(QStringLiteral("class"), QStringLiteral("example"));

    QJsonObject style;
    style.insert(QStringLiteral("backgroundColor"), QStringLiteral("#E6E6E6"));
    style.insert(QStringLiteral("borderColor"), QStringLiteral("#FFFFFF"));
    style.insert(QStringLiteral("borderStyle"), QStringLiteral("solid"));
    style.insert(QStringLiteral("borderWidth"), QStringLiteral("0 0 0 2px"));
    style.insert(QStringLiteral("padding"), QStringLiteral("5px 8px"));
    style.insert(QStringLiteral("marginTop"), QStringLiteral("4px"));
    style.insert(QStringLiteral("marginBottom"), QStringLiteral("4px"));
    style.insert(QStringLiteral("width"), QStringLiteral("fit-content"));

    QJsonObject node;
    node.insert(QStringLiteral("tag"), QStringLiteral("div"));
    node.insert(QStringLiteral("data"), data);
    node.insert(QStringLiteral("style"), style);
    node.insert(QStringLiteral("content"), content);
    return node;
}

QJsonObject genericUsageGroup(const QJsonValue &content)
{
    QJsonObject data;
    data.insert(QStringLiteral("class"),
                QStringLiteral("memento-usage-group"));

    QJsonObject style;
    style.insert(QStringLiteral("backgroundColor"), QStringLiteral("#E6E6E6"));
    style.insert(QStringLiteral("borderColor"), QStringLiteral("#FFFFFF"));
    style.insert(QStringLiteral("borderStyle"), QStringLiteral("solid"));
    style.insert(QStringLiteral("borderWidth"), QStringLiteral("0 0 0 2px"));
    style.insert(QStringLiteral("padding"), QStringLiteral("5px 8px"));
    style.insert(QStringLiteral("marginTop"), QStringLiteral("4px"));
    style.insert(QStringLiteral("marginBottom"), QStringLiteral("4px"));
    style.insert(QStringLiteral("width"), QStringLiteral("fit-content"));

    QJsonObject node;
    node.insert(QStringLiteral("tag"), QStringLiteral("div"));
    node.insert(QStringLiteral("data"), data);
    node.insert(QStringLiteral("style"), style);
    node.insert(QStringLiteral("content"), content);
    return node;
}

QString splitAttachedSenseMarkers(const QString &text);
QString separatePronunciationFromFollowingText(const QString &text);

QJsonValue filterGenericStructuredText(
    const QString &text,
    qsizetype &remainingExamples,
    bool insideUsageGroup = false)
{
    QString filteredText = separateInlineRomanPronunciationSectionHeaders(
        separatePronunciationFromFollowingText(
            suppressUntranslatedPhraseLists(text)
        )
    );
    stripLeadingGlossaryPronunciation(filteredText);
    const QStringList lines = splitAttachedSenseMarkers(filteredText).split(
        QLatin1Char('\n'), Qt::KeepEmptyParts
    );

    QJsonArray segments;
    QString plainText;
    bool foundExample = false;
    const auto flushPlainText = [&]()
    {
        if (!plainText.isEmpty())
        {
            appendStyledGenericGlossaryText(plainText, segments);
            plainText.clear();
        }
    };

    static const QRegularExpression USAGE_HEADER{
        R"(^\s*(USAGE:)\s*(.*)$)",
        QRegularExpression::CaseInsensitiveOption
    };

    for (qsizetype i = 0; i < lines.size();)
    {
        if (startsGlossaryDefinitionHeading(lines[i]))
        {
            remainingExamples = 2;
        }

        const QRegularExpressionMatch usageHeader =
            USAGE_HEADER.match(lines[i]);
        if (!insideUsageGroup && usageHeader.hasMatch())
        {
            flushPlainText();
            qsizetype usageEnd = lines.size();
            for (qsizetype next = i + 1; next < lines.size(); ++next)
            {
                if (startsGlossarySectionHeading(lines[next]))
                {
                    usageEnd = next;
                    break;
                }
            }
            while (usageEnd > i + 1 &&
                   lines[usageEnd - 1].trimmed().isEmpty())
            {
                --usageEnd;
            }
            const QString usageText = lines.mid(i, usageEnd - i)
                                          .join(QLatin1Char('\n')).trimmed();
            const QJsonValue usageContent = filterGenericStructuredText(
                usageText, remainingExamples, true
            );
            segments.append(genericUsageGroup(
                localizeGlossarySectionHeading(usageContent)
            ));
            i = usageEnd;
            continue;
        }

        if (!isBilingualExampleLine(lines[i]))
        {
            plainText += lines[i];
            if (i + 1 < lines.size())
            {
                plainText += QLatin1Char('\n');
            }
            ++i;
            continue;
        }

        foundExample = true;
        flushPlainText();
        QStringList examples;
        qsizetype next = i;
        while (next < lines.size())
        {
            if (isBilingualExampleLine(lines[next]))
            {
                if (remainingExamples > 0)
                {
                    examples.append(lines[next].trimmed());
                    --remainingExamples;
                }
                ++next;
                continue;
            }

            if (lines[next].trimmed().isEmpty() &&
                next + 1 < lines.size() &&
                isBilingualExampleLine(lines[next + 1]))
            {
                ++next;
                continue;
            }
            break;
        }

        if (!examples.isEmpty())
        {
            segments.append(genericExampleGroup(examples));
        }
        i = next;
        if (i < lines.size())
        {
            plainText += QLatin1Char('\n');
        }
    }
    flushPlainText();

    if (segments.isEmpty())
    {
        return foundExample ?
            QJsonValue(QJsonValue::Undefined) : QJsonValue(filteredText);
    }
    if (segments.size() == 1 && segments[0].isString())
    {
        return segments[0];
    }
    return segments;
}

bool containsNestedExampleNode(const QJsonValue &value)
{
    if (value.isArray())
    {
        for (const QJsonValue &child : value.toArray())
        {
            if (containsNestedExampleNode(child))
            {
                return true;
            }
        }
        return false;
    }
    if (!value.isObject())
    {
        return false;
    }
    if (isExampleStructuredNode(value))
    {
        return true;
    }
    return containsNestedExampleNode(
        value.toObject().value(QStringLiteral("content"))
    );
}

bool isEmptyStructuredContent(const QJsonValue &value)
{
    if (value.isUndefined() || value.isNull())
    {
        return true;
    }
    if (value.isString())
    {
        return value.toString().trimmed().isEmpty();
    }
    if (value.isArray())
    {
        for (const QJsonValue &child : value.toArray())
        {
            if (!isEmptyStructuredContent(child))
            {
                return false;
            }
        }
        return true;
    }
    if (value.isObject())
    {
        const QJsonObject obj = value.toObject();
        const QString tag = obj.value(QStringLiteral("tag")).toString()
            .toLower();
        if (tag == QStringLiteral("img"))
        {
            return false;
        }
        if (tag == QStringLiteral("br"))
        {
            return true;
        }
        if (obj.contains(QStringLiteral("text")) &&
            !isEmptyStructuredContent(obj.value(QStringLiteral("text"))))
        {
            return false;
        }
        return !obj.contains(QStringLiteral("content")) ||
            isEmptyStructuredContent(obj.value(QStringLiteral("content")));
    }
    return true;
}

QJsonValue filterStructuredExampleNodes(
    const QJsonValue &value,
    qsizetype &remainingExamples)
{
    if (value.isArray())
    {
        QJsonArray filtered;
        const QJsonArray content = suppressUntranslatedStructuredPhraseLists(
            value.toArray()
        );
        static const QRegularExpression USAGE_HEADER_ONLY{
            R"(^\s*USAGE:\s*$)",
            QRegularExpression::CaseInsensitiveOption
        };
        for (qsizetype i = 0; i < content.size();)
        {
            const QJsonValue &child = content[i];
            if (!isExampleStructuredNode(child) &&
                startsGlossaryDefinitionHeading(
                    structuredNodeText(child).trimmed()
                ))
            {
                remainingExamples = 2;
            }
            if (USAGE_HEADER_ONLY.match(
                    structuredNodeText(child).trimmed()).hasMatch())
            {
                QJsonArray usageContent;
                usageContent.append(child);
                qsizetype next = i + 1;
                while (next < content.size() &&
                       !startsGlossarySectionHeading(
                           structuredNodeText(content[next]).trimmed()
                       ))
                {
                    const QJsonValue usageChild = filterStructuredExampleNodes(
                        content[next], remainingExamples
                    );
                    if (!usageChild.isUndefined())
                    {
                        usageContent.append(usageChild);
                    }
                    ++next;
                }
                filtered.append(genericUsageGroup(
                    localizeGlossarySectionHeading(usageContent)
                ));
                i = next;
                continue;
            }

            const QJsonValue filteredChild = filterStructuredExampleNodes(
                child, remainingExamples
            );
            if (!filteredChild.isUndefined())
            {
                filtered.append(filteredChild);
            }
            ++i;
        }
        return filtered;
    }
    if (!value.isObject())
    {
        if (value.isString())
        {
            return filterGenericStructuredText(
                value.toString(), remainingExamples
            );
        }
        return value;
    }

    QJsonObject obj = value.toObject();
    if (isExampleStructuredNode(value))
    {
        const QJsonValue content = obj.value(QStringLiteral("content"));
        if (containsNestedExampleNode(content))
        {
            const QJsonValue filteredContent = filterStructuredExampleNodes(
                content, remainingExamples
            );
            if (isEmptyStructuredContent(filteredContent))
            {
                return QJsonValue(QJsonValue::Undefined);
            }
            obj.insert(QStringLiteral("content"), filteredContent);
            return obj;
        }

        const QString text = structuredNodeText(value);
        static const QRegularExpression LATIN{R"([A-Za-z])"};
        static const QRegularExpression CYRILLIC{R"([\x{0400}-\x{04FF}])"};
        const bool hasLatin = LATIN.match(text).hasMatch();
        const bool hasCyrillic = CYRILLIC.match(text).hasMatch();
        if (!hasLatin || !hasCyrillic || remainingExamples <= 0)
        {
            return QJsonValue(QJsonValue::Undefined);
        }
        --remainingExamples;
        return value;
    }

    if (obj.contains(QStringLiteral("content")))
    {
        const QJsonValue filteredContent = filterStructuredExampleNodes(
            obj.value(QStringLiteral("content")), remainingExamples
        );
        if (filteredContent.isUndefined())
        {
            obj.remove(QStringLiteral("content"));
        }
        else
        {
            obj.insert(QStringLiteral("content"), filteredContent);
        }
    }
    return obj;
}

/**
 * @brief Replace Arabic glossary list markers with circled numerals while
 * preserving the source order and text.
 */
QString useCircledSenseMarkers(const QString &line)
{
    static const QRegularExpression MARKER{
        R"((?<![\p{L}\d])(\d{1,2})[.)](?=\s|$))"
    };

    QString out;
    qsizetype position = 0;
    QRegularExpressionMatchIterator matches = MARKER.globalMatch(line);
    while (matches.hasNext())
    {
        const QRegularExpressionMatch match = matches.next();
        out += line.sliced(position, match.capturedStart() - position);

        // A dotted number inside a gloss is commonly a dictionary sense
        // reference (for example, "much 1.") rather than a new list item.
        // Only convert dotted Arabic markers at the start of the line.
        if (match.captured().endsWith(QLatin1Char('.')) &&
            !line.first(match.capturedStart()).trimmed().isEmpty())
        {
            out += match.captured();
            position = match.capturedEnd();
            continue;
        }

        const QString prefix = line.first(match.capturedStart());
        if (match.captured().endsWith(QLatin1Char(')')) &&
            prefix.lastIndexOf(QLatin1Char('(')) >
                prefix.lastIndexOf(QLatin1Char(')')))
        {
            out += match.captured();
            position = match.capturedEnd();
            continue;
        }

        bool ok = false;
        const int number = match.captured(1).toInt(&ok);
        if (ok && number >= 1 && number <= 20)
        {
            out += QChar(0x2460 + number - 1);
        }
        else if (ok && number >= 21 && number <= 35)
        {
            out += QChar(0x3251 + number - 21);
        }
        else if (ok && number >= 36 && number <= 50)
        {
            out += QChar(0x32B1 + number - 36);
        }
        else
        {
            out += match.captured();
        }
        position = match.capturedEnd();
    }
    out += line.sliced(position);
    return out;
}

/**
 * @brief Put inline glossary sense markers on their own lines.
 *
 * Flattened dictionary text may attach an explicit circled or Arabic sense
 * marker to an abbreviation or the preceding gloss (for example,
 * "; уст.①" or "label1)"). Move each marker to a new line and normalize
 * Arabic list markers to circled numerals. Only explicit enumeration forms
 * are considered; ordinary numbers and decimal values remain untouched.
 */
QString splitAttachedSenseMarkers(const QString &text)
{
    static const QRegularExpression INLINE_SENSE_MARKER{
        R"([①-⑳]|(?<!\d)(?:[1-9]|[1-4]\d|50)[.)](?=\s|[\p{L}]|$))"
    };

    QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    for (QString &line : lines)
    {
        QString normalized;
        qsizetype segmentStart = 0;
        QRegularExpressionMatchIterator matches =
            INLINE_SENSE_MARKER.globalMatch(line);
        while (matches.hasNext())
        {
            const QRegularExpressionMatch match = matches.next();
            const QString prefix = line.sliced(
                segmentStart, match.capturedStart() - segmentStart
            );

            // Keep dotted numbers that follow actual gloss text intact:
            // dictionaries use them as cross-reference sense numbers.
            if (match.captured().front().isDigit() &&
                match.captured().endsWith(QLatin1Char('.')) &&
                !prefix.trimmed().isEmpty())
            {
                continue;
            }

            // A parenthesized numeric citation such as "(Leviticus 11:4)"
            // is prose, not an Arabic sense marker. Do not split a closing
            // number while the prefix still has an unmatched opening paren.
            if (match.captured().front().isDigit() &&
                match.captured().endsWith(QLatin1Char(')')) &&
                prefix.lastIndexOf(QLatin1Char('(')) >
                    prefix.lastIndexOf(QLatin1Char(')')))
            {
                continue;
            }

            if (segmentStart == 0 && prefix.trimmed().isEmpty())
            {
                continue;
            }

            qsizetype indentationLength = 0;
            while (indentationLength < line.size() &&
                   line.at(indentationLength).isSpace())
            {
                ++indentationLength;
            }
            const QString indentation = line.first(indentationLength);
            normalized += prefix;
            while (!normalized.isEmpty() && normalized.back().isSpace())
            {
                normalized.chop(1);
            }
            normalized += QLatin1Char('\n');
            normalized += indentation;
            normalized += useCircledSenseMarkers(match.captured());
            segmentStart = match.capturedEnd();
        }

        if (segmentStart > 0)
        {
            normalized += line.sliced(segmentStart);
            line = useCircledSenseMarkers(normalized);
        }
        else
        {
            line = useCircledSenseMarkers(line);
        }
    }
    return lines.join(QLatin1Char('\n'));
}

/** @brief Restore a word boundary after bracketed pronunciation text. */
QString separatePronunciationFromFollowingText(const QString &text)
{
    static const QRegularExpression IPA_BOUNDARY{
        R"((\[(?!/?ex\])[^\]\r\n]+\])(?=[\p{L}\p{N}]))",
        QRegularExpression::CaseInsensitiveOption
    };
    QString result = text;
    result.replace(IPA_BOUNDARY, QStringLiteral("\\1 "));
    return result;
}

/**
 * @brief Separate Roman section markers from adjacent Arabic sense markers.
 *
 * Flattened dictionary text sometimes joins a Roman section number and its
 * first Arabic sense (for example, "II1) music"). Keep the section marker on
 * its own row and render the Arabic marker as a circled number.
 */
QString splitRomanNumberedSenseMarkers(const QString &text)
{
    static const QRegularExpression ROMAN_NUMBERED_SENSE{
        R"(^([ \t]*)(IV|III|II|I)\)?[ \t]*(\d{1,2}[.)])(?=[ \t]|$)(.*)$)"
    };

    QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    for (QString &line : lines)
    {
        const QRegularExpressionMatch match =
            ROMAN_NUMBERED_SENSE.match(line);
        if (!match.hasMatch())
        {
            continue;
        }

        const QString indentation = match.captured(1);
        const QString romanMarker = match.captured(2).toUpper();
        const QString senseMarker = useCircledSenseMarkers(
            match.captured(3)
        );
        const QString senseText = match.captured(4).trimmed();
        line = indentation + romanMarker + QLatin1Char('\n') +
            indentation + senseMarker;
        if (!senseText.isEmpty())
        {
            line += QLatin1Char(' ') + senseText;
        }
    }
    return lines.join(QLatin1Char('\n'));
}

/** @brief Separate a generic POS label from its first numbered sense. */
QString splitInlinePosSenseMarkers(const QString &text)
{
    static const QRegularExpression INLINE_POS_SENSE{
        R"(^([ \t]*)((?:n|v|adj|adv|pron|prep|conj|interj|det|aux|art|num|abbr|intr|tr|vt|vi|pl|sing|count|uncount)\.?)[ \t]*((?:[①-⑳]|\d{1,2}[.)]))(?=[ \t]|$)(.*)$)",
        QRegularExpression::CaseInsensitiveOption
    };

    QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    for (qsizetype i = 0; i < lines.size(); ++i)
    {
        const QRegularExpressionMatch match =
            INLINE_POS_SENSE.match(lines[i]);
        if (!match.hasMatch())
        {
            continue;
        }

        const QString indentation = match.captured(1);
        const QString posLabel = match.captured(2);
        const QString senseMarker = useCircledSenseMarkers(
            match.captured(3)
        );
        QString senseLine = indentation + senseMarker;
        const QString senseText = match.captured(4).trimmed();
        if (!senseText.isEmpty())
        {
            senseLine += QLatin1Char(' ') + senseText;
        }
        lines[i] = indentation + posLabel;
        lines.insert(i + 1, std::move(senseLine));
        ++i;
    }
    return lines.join(QLatin1Char('\n'));
}

QString circledGlossaryMarkerToRoman(const QString &marker)
{
    if (marker.size() != 1)
    {
        return {};
    }

    int number = marker.front().unicode() - 0x2460 + 1;
    if (number < 1 || number > 20)
    {
        return {};
    }

    constexpr std::array<int, 5> ROMAN_VALUES{10, 9, 5, 4, 1};
    constexpr std::array<const char *, 5> ROMAN_SYMBOLS{
        "X", "IX", "V", "IV", "I"
    };
    QString roman;
    for (std::size_t i = 0; i < ROMAN_VALUES.size(); ++i)
    {
        while (number >= ROMAN_VALUES[i])
        {
            roman += QString::fromLatin1(ROMAN_SYMBOLS[i]);
            number -= ROMAN_VALUES[i];
        }
    }
    return roman;
}

QString formatExampleBlock(
    const QString &backgroundColor,
    const QString &dividerColor,
    const QString &content)
{
    return QStringLiteral(
        "<table class=\"memento-example-block\" "
            "data-example-bg=\"%1\" data-example-divider=\"%2\" "
            "cellspacing=\"0\" cellpadding=\"0\" "
            "bgcolor=\"#FFFFFF\"><tr>"
            "<td style=\"vertical-align: top; padding: 0 0 0 2px; "
                       "border: 0;\">"
                "<table cellspacing=\"0\" cellpadding=\"0\" "
                    "bgcolor=\"%1\"><tr>"
                "<td style=\"vertical-align: top; border: 0; "
                               "padding-bottom: 5px; "
                               "padding-left: 8px; "
                               "padding-right: 8px; "
                               "padding-top: 5px;\">%3</td>"
                "</tr></table>"
            "</td>"
        "</tr></table>"
    ).arg(backgroundColor, dividerColor, content);
}

QString mergeAdjacentExampleBlocks(QString html)
{
    static const QRegularExpression EXAMPLE_BLOCK{
        R"REGEX(<table class="memento-example-block" data-example-bg="([^"]+)" data-example-divider="([^"]+)" cellspacing="0" cellpadding="0" bgcolor="#FFFFFF"><tr><td style="vertical-align: top; padding: 0 0 0 2px; border: 0;"><table cellspacing="0" cellpadding="0" bgcolor="[^"]+"><tr><td style="vertical-align: top; border: 0; padding-bottom: 5px; padding-left: 8px; padding-right: 8px; padding-top: 5px;">(.*?)</td></tr></table></td></tr></table>)REGEX",
        QRegularExpression::DotMatchesEverythingOption
    };
    static const QRegularExpression LINE_BREAK{R"(<br\s*/?>)"};
    static const QRegularExpression NON_BREAKING_SPACE{
        R"(&(?:nbsp|#160|#x0*a0);)",
        QRegularExpression::CaseInsensitiveOption
    };

    while (true)
    {
        QRegularExpressionMatch previous;
        QRegularExpressionMatchIterator matches =
            EXAMPLE_BLOCK.globalMatch(html);
        bool merged = false;
        while (matches.hasNext())
        {
            const QRegularExpressionMatch current = matches.next();
            if (!previous.hasMatch())
            {
                previous = current;
                continue;
            }

            QString separator = html.sliced(
                previous.capturedEnd(),
                current.capturedStart() - previous.capturedEnd()
            );
            separator.remove(LINE_BREAK);
            separator.remove(NON_BREAKING_SPACE);
            if (separator.trimmed().isEmpty() &&
                previous.captured(1) == current.captured(1) &&
                previous.captured(2) == current.captured(2))
            {
                const QString combinedContent =
                    previous.captured(3) + QStringLiteral(
                        "<div style=\"border-top:1px solid %1; "
                              "background-color:%2; margin:0; "
                              "padding:5px 0;\"></div>"
                    ).arg(previous.captured(2), previous.captured(1)) +
                    current.captured(3);
                const QString combinedBlock = formatExampleBlock(
                    previous.captured(1),
                    previous.captured(2),
                    combinedContent
                );
                html.replace(
                    previous.capturedStart(),
                    current.capturedEnd() - previous.capturedStart(),
                    combinedBlock
                );
                merged = true;
                break;
            }
            previous = current;
        }

        if (!merged)
        {
            return html;
        }
    }
}

/**
 * @brief Get a leading Roman-numeral glossary marker, if present.
 */
QString leadingRomanGlossaryMarker(const QString &text)
{
    static const QRegularExpression MARKER{
        R"(^\s*(VIII|VII|VI|IV|V|III|II|IX|X|I)\))",
        QRegularExpression::CaseInsensitiveOption
    };
    static const QRegularExpression STANDALONE_MARKER{
        R"(^\s*(VIII|VII|VI|IV|V|III|II|IX|X|I)\s*$)",
        QRegularExpression::CaseInsensitiveOption
    };
    static const QRegularExpression JOINED_SENSE_MARKER{
        R"(^\s*(IV|III|II|I)\)?\s*\d{1,2}[.)](?=\s|$))",
        QRegularExpression::CaseInsensitiveOption
    };
    const QString firstLine = text.section(QLatin1Char('\n'), 0, 0);
    const QRegularExpressionMatch match = MARKER.match(firstLine);
    if (match.hasMatch())
    {
        return match.captured(1).toUpper();
    }

    const QRegularExpressionMatch standalone =
        STANDALONE_MARKER.match(firstLine);
    if (standalone.hasMatch())
    {
        return standalone.captured(1).toUpper();
    }

    const QRegularExpressionMatch joined =
        JOINED_SENSE_MARKER.match(firstLine);
    if (joined.hasMatch())
    {
        return joined.captured(1).toUpper();
    }

    // One source row is flattened as "Iстая"; only accept a missing close
    // parenthesis when an uppercase Roman numeral is joined directly to
    // Cyrillic text, so ordinary Latin words beginning with I stay untouched.
    static const QRegularExpression FLATTENED_MARKER{
        R"(^\s*(VIII|VII|VI|IV|V|III|II|IX|X|I)(?=[\x{0400}-\x{04FF}]))"
    };
    const QRegularExpressionMatch flattened =
        FLATTENED_MARKER.match(firstLine);
    return flattened.hasMatch() ?
        flattened.captured(1).toUpper() : QString();
}

bool startsCircledGlossaryMarker(const QString &text)
{
    static const QRegularExpression MARKER{R"(^\s*[①-⑳])"};
    return MARKER.match(text.section(QLatin1Char('\n'), 0, 0)).hasMatch();
}

bool startsNumberedGlossaryMarker(const QString &text)
{
    static const QRegularExpression MARKER{
        R"(^\s*(?:\d+[.)]|[a-zа-яё]\)|[A-Za-z]{1,4}\.?\d{1,2}\)|(?:n|v|adj|adv|pron|prep|conj|interj|det|aux|art|num|abbr|intr|tr|vt|vi|pl|sing|count|uncount)\.?\s*(?:[①-⑳]|\d{1,2}[.)]))(?=\s|$))",
        QRegularExpression::CaseInsensitiveOption
    };
    return MARKER.match(text.section(QLatin1Char('\n'), 0, 0)).hasMatch();
}

/**
 * @brief Render explicit hierarchy and example markers in a flat glossary.
 *
 * Explicit [ex] regions and bilingual English/Russian em-dash rows receive a
 * compact example box. Other plain text remains unchanged.
 */
QString formatGenericGlossaryText(
    const QString &text,
    const QColor &backgroundColor,
    bool indentCircled,
    bool includePronunciation = false,
    qsizetype *remainingExamples = nullptr,
    bool renderUsageSection = true,
    bool americanEnglish = false,
    bool includeSynonyms = true)
{
    static const QRegularExpression ROMAN_MARKER{
        R"(^\s*(VIII|VII|VI|IV|V|III|II|IX|X|I)(\))?)",
        QRegularExpression::CaseInsensitiveOption
    };
    static const QRegularExpression ARABIC_HEADING{
        R"(^\s*(\d+\.)(?=\s|$))"
    };
    static const QRegularExpression NUMBERED_SENSE{
        R"(^\s*(\d+\))(?=\s|$))"
    };
    static const QRegularExpression PREFIXED_NUMBERED_SENSE{
        R"(^\s*([A-Za-z]{1,4}\.?)(\d{1,2}\))(?=\s|$))"
    };
    static const QRegularExpression CIRCLED_SENSE{R"(^\s*([①-⑳]))"};
    static const QRegularExpression AMERICAN_ENGLISH_ROMAN_NUMBERED_POS_HEADING{
        R"(^\s*([①-⑳])\s*((?:(?:n|v|adj|adv|pron|prep|conj|interj|det|aux|art|num|abbr|intr|tr|vt|vi|pl|sing|count|uncount)\.?)(?=\s|\(|$)|a\.?(?=\x{E000}))(.*)$)",
        QRegularExpression::CaseInsensitiveOption
    };
    static const QRegularExpression LETTER_SENSE{
        R"(^\s*([a-zа-яё]\))(?=\s|$))",
        QRegularExpression::CaseInsensitiveOption
    };
    static const QRegularExpression PRONUNCIATION_HEADER{
        R"(^([ \t]*(?:[•·][ \t]*)?)(\[[^\]\r\n]+\](?:(?:[ \t]*(?:,|/)[ \t]*|[ \t]+)?\[[^\]\r\n]+\])*)([ \t]*)(.*)$)"
    };

    // Move a leading bracketed pronunciation out of the glossary display.
    // Keep any immediately following part-of-speech text in the definition.
    QString sourceText = text;
    if (americanEnglish)
    {
        // Split the fused AmericanEnglish adjective heading before the
        // generic marker normalizer converts its leading "2." to ②.
        sourceText = splitAmericanEnglishAdjectiveSenseHeadings(sourceText);
    }
    if (!includeSynonyms)
    {
        static const QRegularExpression SYNONYM_LINE{
            R"(^[ \t]*(?:synonyms?|syn)[ \t]*[:.][ \t]*(.*)$)",
            QRegularExpression::CaseInsensitiveOption
        };
        QStringList retainedLines;
        bool skipFollowingLine = false;
        for (const QString &line : sourceText.split(
                 QLatin1Char('\n'), Qt::KeepEmptyParts))
        {
            if (skipFollowingLine)
            {
                if (line.trimmed().isEmpty())
                {
                    continue;
                }
                skipFollowingLine = false;
                continue;
            }
            const QRegularExpressionMatch synonym =
                SYNONYM_LINE.match(line);
            if (synonym.hasMatch())
            {
                skipFollowingLine = synonym.captured(1).trimmed().isEmpty();
                continue;
            }
            retainedLines.append(line);
        }
        sourceText = retainedLines.join(QLatin1Char('\n'));
    }
    QString displayText = splitAttachedSenseMarkers(
        splitInlinePosSenseMarkers(
            splitRomanNumberedSenseMarkers(
                separatePronunciationFromFollowingText(sourceText)
            )
        )
    );
    QStringList displayLines = displayText.split(
        QLatin1Char('\n'), Qt::KeepEmptyParts
    );
    for (qsizetype lineIndex = 0; lineIndex < displayLines.size(); ++lineIndex)
    {
        QString &line = displayLines[lineIndex];
        if (line.trimmed().isEmpty())
        {
            continue;
        }
        const QRegularExpressionMatch pronunciation =
            PRONUNCIATION_HEADER.match(line);
        const QString transcription = pronunciation.captured(2).trimmed();
        if (pronunciation.hasMatch() &&
            transcription.compare(QStringLiteral("[ex]"),
                                  Qt::CaseInsensitive) != 0 &&
            transcription.compare(QStringLiteral("[/ex]"),
                                  Qt::CaseInsensitive) != 0)
        {
            const QString definitionSuffix = pronunciation.captured(4);
            if (includePronunciation)
            {
                line = pronunciation.captured(1) + pronunciation.captured(2);
                if (!definitionSuffix.trimmed().isEmpty())
                {
                    displayLines.insert(
                        lineIndex + 1,
                        pronunciation.captured(1) +
                            pronunciation.captured(3) + definitionSuffix
                    );
                }
            }
            else
            {
                line = definitionSuffix.trimmed().isEmpty() ?
                    QString() :
                    pronunciation.captured(1) +
                        pronunciation.captured(3) + definitionSuffix;
            }
        }
        break;
    }
    displayText = displayLines.join(QLatin1Char('\n'));
    displayText = separateLetterFromOpeningParenthesis(displayText);
    // Treat [ex] markers as markup only. The generic renderer boxes and counts
    // only bilingual rows, so an English-only example cannot consume the cap.
    displayText.remove(QRegularExpression(
        QStringLiteral(R"(\[/?ex\])"),
        QRegularExpression::CaseInsensitiveOption
    ));
    displayText = suppressUntranslatedPhraseLists(displayText);
    displayText = separateInlineRomanPronunciationSectionHeaders(displayText);

    QString usageText;
    if (renderUsageSection)
    {
        static const QRegularExpression USAGE_HEADER{
            R"(^\s*USAGE:\s*.*$)",
            QRegularExpression::CaseInsensitiveOption
        };
        const QStringList lines = displayText.split(
            QLatin1Char('\n'), Qt::KeepEmptyParts
        );
        for (qsizetype i = 0; i < lines.size(); ++i)
        {
            if (!USAGE_HEADER.match(lines[i]).hasMatch())
            {
                continue;
            }

            qsizetype usageEnd = lines.size();
            for (qsizetype next = i + 1; next < lines.size(); ++next)
            {
                if (startsGlossarySectionHeading(lines[next]))
                {
                    usageEnd = next;
                    break;
                }
            }
            while (usageEnd > i + 1 &&
                   lines[usageEnd - 1].trimmed().isEmpty())
            {
                --usageEnd;
            }

            usageText = lines.mid(i, usageEnd - i)
                            .join(QLatin1Char('\n')).trimmed();
            QStringList retainedLines = lines.mid(0, i);
            const QStringList followingLines = lines.mid(usageEnd);
            if (!followingLines.isEmpty())
            {
                if (!retainedLines.isEmpty() &&
                    !retainedLines.back().trimmed().isEmpty())
                {
                    retainedLines.append(QString());
                }
                for (const QString &line : followingLines)
                {
                    retainedLines.append(line);
                }
            }
            displayText = retainedLines.join(QLatin1Char('\n'));
            break;
        }
    }

    const bool darkCanvas = backgroundColor.isValid() &&
        backgroundColor.lightness() < 128;
    const QString exampleBackground = darkCanvas ?
        QStringLiteral("#3B3B3B") : QStringLiteral("#E6E6E6");
    const QString translationColor = darkCanvas ?
        QStringLiteral("#B8B8B8") : QStringLiteral("#666666");
    const QString dividerColor = darkCanvas ?
        QStringLiteral("#5A5A5A") : QStringLiteral("#C8C8C8");
    const QString headingColor = darkCanvas ?
        QStringLiteral("#A7D8A5") : QStringLiteral("#3E713C");
    const QString grammarColor = darkCanvas ?
        QStringLiteral("#8FD3E8") : QStringLiteral("#176B7A");

    const auto hasHierarchy = [](const QString &segment)
    {
        static const QRegularExpression ARABIC_HEADING{
            R"(^\s*\d+\.(?=\s|$))"
        };
        static const QRegularExpression NUMBERED_SENSE{
            R"(^\s*\d+\)(?=\s|$))"
        };
        static const QRegularExpression PREFIXED_NUMBERED_SENSE{
            R"(^\s*[A-Za-z]{1,4}\.?\d{1,2}\)(?=\s|$))"
        };
        static const QRegularExpression CIRCLED_SENSE{R"(^\s*[①-⑳])"};
        static const QRegularExpression LETTER_SENSE{
            R"(^\s*[a-zа-яё]\)(?=\s|$))",
            QRegularExpression::CaseInsensitiveOption
        };
        for (const QString &line : segment.split(QLatin1Char('\n')))
        {
            if (!leadingRomanGlossaryMarker(line).isEmpty() ||
                ARABIC_HEADING.match(line).hasMatch() ||
                NUMBERED_SENSE.match(line).hasMatch() ||
                PREFIXED_NUMBERED_SENSE.match(line).hasMatch() ||
                CIRCLED_SENSE.match(line).hasMatch() ||
                LETTER_SENSE.match(line).hasMatch())
            {
                return true;
            }
        }
        return false;
    };

    const auto renderPlainSegment = [&](
        const QString &segment,
        QString &out,
        int &activeDepth)
    {
        if (!hasHierarchy(segment))
        {
            out += formatGenericGlossaryLabels(segment, grammarColor)
                       .replace(QLatin1Char('\n'), "<br>");
            return;
        }

        const QStringList lines = segment.split(
            QLatin1Char('\n'), Qt::KeepEmptyParts
        );
        for (const QString &sourceLine : lines)
        {
            QString line = useCircledSenseMarkers(sourceLine);
            const QRegularExpressionMatch americanEnglishPosHeading =
                americanEnglish ?
                    AMERICAN_ENGLISH_ROMAN_NUMBERED_POS_HEADING.match(line) :
                    QRegularExpressionMatch{};
            const QString americanEnglishPosRoman =
                americanEnglishPosHeading.hasMatch() ?
                    circledGlossaryMarkerToRoman(
                        americanEnglishPosHeading.captured(1)
                    ) : QString();
            const QString americanEnglishPosLabel =
                americanEnglishPosHeading.captured(2).trimmed();
            if (americanEnglishPosHeading.hasMatch())
            {
                line = americanEnglishPosHeading.captured(3).trimmed();
                line.remove(QChar(0xE000));
            }
            QString marker;
            QString markerPrefix;
            QString romanMarkerPrefix;
            bool heading = americanEnglishPosHeading.hasMatch();
            int depth = activeDepth;

            if (americanEnglishPosHeading.hasMatch())
            {
                depth = 0;
                activeDepth = 1;
            }

            static const QRegularExpression PREFIXED_NUMBERED_SENSE{
                R"(^\s*([A-Za-z]{1,4}\.?)(\d{1,2}\))(?=\s|$))"
            };
            static const QRegularExpression ROMAN_NUMBERED_SENSE{
                R"(^\s*(VIII|VII|VI|IV|V|III|II|IX|X|I)(\d{1,2}\))(?=\s|$))",
                QRegularExpression::CaseInsensitiveOption
            };
            const QRegularExpressionMatch romanNumberedSense =
                ROMAN_NUMBERED_SENSE.match(line);
            const QRegularExpressionMatch prefixedNumberedSense =
                PREFIXED_NUMBERED_SENSE.match(line);
            const QString romanMarker = leadingRomanGlossaryMarker(line);
            const QRegularExpressionMatch roman = ROMAN_MARKER.match(line);
            if (romanNumberedSense.hasMatch())
            {
                romanMarkerPrefix = romanNumberedSense.captured(1).toUpper() +
                    QLatin1Char(')');
                marker = useCircledSenseMarkers(
                    romanNumberedSense.captured(2)
                );
                line.remove(0, romanNumberedSense.capturedEnd());
                heading = true;
                depth = 0;
                activeDepth = 1;
            }
            else if (prefixedNumberedSense.hasMatch())
            {
                markerPrefix = prefixedNumberedSense.captured(1);
                marker = useCircledSenseMarkers(
                    prefixedNumberedSense.captured(2)
                );
                line.remove(0, prefixedNumberedSense.capturedEnd());
                depth = qMax(0, activeDepth);
                activeDepth = depth;
            }
            else if (!romanMarker.isEmpty())
            {
                marker = roman.captured(1) + QLatin1Char(')');
                line.remove(0, roman.capturedEnd());
                heading = true;
                depth = 0;
                activeDepth = romanMarker == QStringLiteral("II") ? 1 : 0;
            }
            else
            {
                const QRegularExpressionMatch arabicHeading =
                    ARABIC_HEADING.match(line);
                const QRegularExpressionMatch numberedSense =
                    NUMBERED_SENSE.match(line);
                const QRegularExpressionMatch circledSense =
                    CIRCLED_SENSE.match(line);
                const QRegularExpressionMatch letterSense =
                    LETTER_SENSE.match(line);

                if (arabicHeading.hasMatch())
                {
                    marker = useCircledSenseMarkers(
                        arabicHeading.captured(1)
                    );
                    line.remove(0, arabicHeading.capturedEnd());
                    heading = true;
                    depth = 0;
                    activeDepth = 1;
                }
                else if (numberedSense.hasMatch())
                {
                    marker = useCircledSenseMarkers(
                        numberedSense.captured(1)
                    );
                    line.remove(0, numberedSense.capturedEnd());
                    depth = 0;
                    activeDepth = depth;
                }
                else if (circledSense.hasMatch())
                {
                    marker = circledSense.captured(1);
                    line.remove(0, circledSense.capturedEnd());
                    depth = qMax(activeDepth, indentCircled ? 1 : 0);
                    activeDepth = depth;
                }
                else if (letterSense.hasMatch())
                {
                    marker = letterSense.captured(1);
                    line.remove(0, letterSense.capturedEnd());
                    depth = qMax(1, activeDepth + 1);
                }
            }

            if (!marker.isEmpty())
            {
                while (!line.isEmpty() && line.front().isSpace())
                {
                    line.remove(0, 1);
                }
            }

            const int markerWidth = marker.isEmpty() ? 0 :
                (!romanMarkerPrefix.isEmpty() ? 48 :
                 (markerPrefix.isEmpty() ? 20 : 34));
            const int indent = depth * 18 + markerWidth;
            const QString markerHtml =
                americanEnglishPosHeading.hasMatch() ?
                QStringLiteral(
                    "<span style=\"color:%1; font-weight:700;\">%2</span>"
                    "&nbsp;&nbsp;<span style=\"color:%3; font-style:italic;\">%4</span>&nbsp;&nbsp;"
                ).arg(
                    headingColor,
                    americanEnglishPosRoman.toHtmlEscaped(),
                    grammarColor,
                    americanEnglishPosLabel.toHtmlEscaped()
                ) : marker.isEmpty() ? QString() :
                !romanMarkerPrefix.isEmpty() ?
                QStringLiteral(
                    "<span style=\"color:%2; font-weight:700;\">%1</span>"
                    "&nbsp;&nbsp;<span style=\"color:%2; font-weight:700;\">%3</span>&nbsp;&nbsp;"
                ).arg(
                    romanMarkerPrefix.toHtmlEscaped(),
                    headingColor,
                    marker.toHtmlEscaped()
                ) : markerPrefix.isEmpty() ?
                QStringLiteral(
                    "<span style=\"color:%1; font-weight:700;\">%2</span>&nbsp;&nbsp;"
                ).arg(headingColor, marker.toHtmlEscaped()) :
                QStringLiteral(
                    "<span style=\"color:%2; font-weight:700; font-style:italic;\">%1</span>"
                    "&nbsp;"
                    "<span style=\"color:%2; font-weight:700;\">%3</span>&nbsp;&nbsp;"
                ).arg(
                    markerPrefix.toHtmlEscaped(),
                    headingColor,
                    marker.toHtmlEscaped()
                );
            const QString content =
                formatGenericGlossaryLabels(line, grammarColor);
            out += QStringLiteral(
                "<p style=\"text-align:left; margin-top:%1px; margin-bottom:0px; margin-left:%2px; text-indent:-%3px;\">%4%5</p>"
            ).arg(heading ? 5 : 0)
             .arg(indent)
             .arg(markerWidth)
             .arg(markerHtml, content);
        }
    };

    struct ExampleSpan
    {
        qsizetype start;
        qsizetype end;
        QString content;
        qsizetype definitionIndex;
        bool choiceOfWords = false;
    };

    static const QRegularExpression CHOICE_OF_WORDS_HEADER{
        R"(^\s*(?:[•·][ \t]*)?CHOICE OF WORDS:)",
        QRegularExpression::CaseInsensitiveOption
    };
    const auto isNextSectionHeader = [](const QString &line)
    {
        return startsGlossarySectionHeading(line);
    };

    QVector<ExampleSpan> choiceOfWordsSpans;
    qsizetype choiceScanPosition = 0;
    while (choiceScanPosition <= displayText.size())
    {
        qsizetype lineEnd = displayText.indexOf(
            QLatin1Char('\n'), choiceScanPosition
        );
        if (lineEnd < 0)
        {
            lineEnd = displayText.size();
        }
        const QString line = displayText.sliced(
            choiceScanPosition, lineEnd - choiceScanPosition
        );
        if (CHOICE_OF_WORDS_HEADER.match(line).hasMatch())
        {
            qsizetype blockEnd = lineEnd;
            qsizetype nextLineStart = lineEnd < displayText.size() ?
                lineEnd + 1 : displayText.size() + 1;
            while (nextLineStart <= displayText.size())
            {
                qsizetype nextLineEnd = displayText.indexOf(
                    QLatin1Char('\n'), nextLineStart
                );
                if (nextLineEnd < 0)
                {
                    nextLineEnd = displayText.size();
                }
                const QString nextLine = displayText.sliced(
                    nextLineStart, nextLineEnd - nextLineStart
                );
                if (isNextSectionHeader(nextLine))
                {
                    blockEnd = nextLineStart;
                    break;
                }

                blockEnd = nextLineEnd;
                if (nextLineEnd == displayText.size())
                {
                    break;
                }
                nextLineStart = nextLineEnd + 1;
            }

            choiceOfWordsSpans.append({
                choiceScanPosition,
                blockEnd,
                displayText.sliced(
                    choiceScanPosition, blockEnd - choiceScanPosition
                ).trimmed(),
                0,
                true
            });
            if (blockEnd == displayText.size())
            {
                break;
            }
            choiceScanPosition = blockEnd;
            continue;
        }
        if (lineEnd == displayText.size())
        {
            break;
        }
        choiceScanPosition = lineEnd + 1;
    }

    QVector<ExampleSpan> exampleSpans;
    qsizetype definitionIndex = 0;
    qsizetype lineStart = 0;
    while (lineStart <= displayText.size())
    {
        qsizetype lineEnd = displayText.indexOf(QLatin1Char('\n'), lineStart);
        if (lineEnd < 0)
        {
            lineEnd = displayText.size();
        }
        const QString line = displayText.sliced(lineStart, lineEnd - lineStart);
        const bool insideChoiceOfWords = std::any_of(
            choiceOfWordsSpans.cbegin(),
            choiceOfWordsSpans.cend(),
            [lineStart](const ExampleSpan &span)
            {
                return lineStart >= span.start && lineStart < span.end;
            }
        );
        if (!insideChoiceOfWords && startsGlossarySectionHeading(line))
        {
            ++definitionIndex;
        }
        if (!insideChoiceOfWords && isBilingualExampleLine(line))
        {
            exampleSpans.append({lineStart, lineEnd, line, definitionIndex});
        }
        if (lineEnd == displayText.size())
        {
            break;
        }
        lineStart = lineEnd + 1;
    }
    for (const ExampleSpan &choiceOfWords : choiceOfWordsSpans)
    {
        exampleSpans.append(choiceOfWords);
    }
    std::sort(
        exampleSpans.begin(),
        exampleSpans.end(),
        [](const ExampleSpan &left, const ExampleSpan &right)
        {
            return left.start < right.start;
        }
    );

    if (remainingExamples != nullptr && !exampleSpans.isEmpty())
    {
        QVector<ExampleSpan> selectedExamples;
        QString limitedText;
        qsizetype sourcePosition = 0;
        qsizetype activeDefinitionIndex = 0;
        qsizetype examplesLeft = *remainingExamples;
        for (const ExampleSpan &example : exampleSpans)
        {
            limitedText += displayText.sliced(
                sourcePosition, example.start - sourcePosition
            );
            if (example.choiceOfWords)
            {
                const qsizetype start = limitedText.size();
                limitedText += displayText.sliced(
                    example.start, example.end - example.start
                );
                selectedExamples.append({
                    start,
                    limitedText.size(),
                    example.content,
                    example.definitionIndex,
                    true
                });
                sourcePosition = example.end;
                continue;
            }
            if (example.definitionIndex != activeDefinitionIndex)
            {
                activeDefinitionIndex = example.definitionIndex;
                if (activeDefinitionIndex > 0)
                {
                    examplesLeft = 2;
                    *remainingExamples = 2;
                }
            }
            else
            {
                examplesLeft = *remainingExamples;
            }

            if (examplesLeft > 0)
            {
                const qsizetype start = limitedText.size();
                const QString sourceExample = displayText.sliced(
                    example.start, example.end - example.start
                );
                limitedText += sourceExample;
                selectedExamples.append({
                    start,
                    limitedText.size(),
                    example.content,
                    example.definitionIndex,
                    false
                });
                --examplesLeft;
                *remainingExamples = examplesLeft;
                sourcePosition = example.end;
            }
            else
            {
                sourcePosition = example.end;
                if (sourcePosition < displayText.size() &&
                    displayText[sourcePosition] == QLatin1Char('\n'))
                {
                    ++sourcePosition;
                }
            }
        }
        limitedText += displayText.sliced(sourcePosition);
        displayText = std::move(limitedText);
        exampleSpans = std::move(selectedExamples);
    }

    QString usageBox;
    if (!usageText.trimmed().isEmpty())
    {
        QString usageHtml = formatGenericGlossaryText(
            usageText,
            backgroundColor,
            indentCircled,
            false,
            remainingExamples,
            false
        );
        static const QRegularExpression USAGE_LABEL{
            R"(^\s*(USAGE:))",
            QRegularExpression::CaseInsensitiveOption
        };
        const QRegularExpressionMatch usageLabel =
            USAGE_LABEL.match(usageHtml);
        if (usageLabel.hasMatch())
        {
            const QString label = QStringLiteral("УПОТРЕБЛЕНИЕ:");
            usageHtml.replace(
                usageLabel.capturedStart(1),
                usageLabel.capturedLength(1),
                QStringLiteral("<b>%1</b>").arg(label)
            );
        }
        usageBox = QStringLiteral(
            "<table cellspacing=\"0\" cellpadding=\"0\" "
                "style=\"margin-top:4px; margin-bottom:4px; "
                       "border-collapse:collapse;\"><tr>"
                "<td width=\"2\" bgcolor=\"#FFFFFF\"></td>"
                "<td bgcolor=\"%1\" "
                    "style=\"background-color:%1; padding:5px 8px;\">%2</td>"
            "</tr></table>"
        ).arg(exampleBackground, usageHtml);
    }

    if (exampleSpans.isEmpty() && !hasHierarchy(displayText))
    {
        return formatGenericGlossaryLabels(displayText, grammarColor)
            .replace(QLatin1Char('\n'), "<br>") + usageBox;
    }

    QString out;
    int activeDepth = 0;
    qsizetype position = 0;

    for (qsizetype i = 0; i < exampleSpans.size();)
    {
        const ExampleSpan &example = exampleSpans[i];
        renderPlainSegment(
            displayText.sliced(position, example.start - position),
            out,
            activeDepth
        );

        if (example.choiceOfWords)
        {
            QString choiceHtml = formatGenericGlossaryLabels(
                example.content, grammarColor
            ).replace(QLatin1Char('\n'), "<br>");
            static const QRegularExpression CHOICE_HEADING_TEXT{
                R"((CHOICE OF WORDS:))",
                QRegularExpression::CaseInsensitiveOption
            };
            const QRegularExpressionMatch choiceHeading =
                CHOICE_HEADING_TEXT.match(choiceHtml);
            if (choiceHeading.hasMatch())
            {
                choiceHtml.replace(
                    choiceHeading.capturedStart(1),
                    choiceHeading.capturedLength(1),
                    QStringLiteral("<b>ВЫБОР СЛОВА:</b>")
                );
            }
            out += formatExampleBlock(
                exampleBackground,
                dividerColor,
                choiceHtml
            );
            position = example.end;
            if (!displayText.sliced(position).trimmed().isEmpty())
            {
                out += QStringLiteral(
                    "<p style=\"font-size:4px; line-height:4px; "
                    "margin-top:0px; margin-bottom:0px;\">&nbsp;</p>"
                );
            }
            ++i;
            continue;
        }

        QString exampleContent = formatBilingualExampleHtml(
            example.content, translationColor
        );
        qsizetype groupEnd = example.end;
        qsizetype nextExample = i + 1;
        while (nextExample < exampleSpans.size())
        {
            const ExampleSpan &next = exampleSpans[nextExample];
            const QString separator = displayText.sliced(
                groupEnd, next.start - groupEnd
            );
            if (!separator.trimmed().isEmpty())
            {
                break;
            }

            exampleContent += QStringLiteral(
                "<div style=\"border-top:1px solid %1; "
                      "background-color:%2; margin:0; "
                      "padding:5px 0;\"></div>"
            ).arg(dividerColor, exampleBackground);
            exampleContent += formatBilingualExampleHtml(
                next.content, translationColor
            );
            groupEnd = next.end;
            ++nextExample;
        }

        out += formatExampleBlock(
            exampleBackground,
            dividerColor,
            exampleContent
        );
        position = groupEnd;
        i = nextExample;
    }
    renderPlainSegment(displayText.sliced(position), out, activeDepth);
    return out + usageBox;
}

/**
 * @brief Render flattened Lingvo glossary text with compact styling for
 * grammatical labels, synonyms, and example rows.
 */
QString formatLingvoGlossaryText(
    const QString &text,
    const QColor &backgroundColor,
    bool includePronunciation,
    bool filterExampleLines = true,
    bool includeSynonyms = true)
{
    const bool darkCanvas = backgroundColor.isValid() &&
        backgroundColor.lightness() < 128;
    const QString grammarColor = darkCanvas ? "#8FD3E8" : "#176B7A";
    const QString usageColor = darkCanvas ? "#E6AE75" : "#9A571C";
    const QString domainColor = darkCanvas ? "#8ED6B5" : "#287255";
    const QString editorialColor = darkCanvas ? "#F2C879" : "#8A5A00";
    const QString synonymColor = darkCanvas ? "#D8B4FE" : "#7041A6";
    const QString headingColor = darkCanvas ? "#A7D8A5" : "#3E713C";
    const QString dividerColor = darkCanvas ? "#5A5A5A" : "#C8C8C8";

    static const QRegularExpression GRAMMAR_LABEL{
        R"((?<![\p{L}])(?:ед\.?\s+прош\.?\s+от\.?|косвенный\s+падеж|косв\.\s*пад\.|фраз\.\s*гл\.|притяж\.\s*мест\.|прич\.\s*(?:наст|прош)\.\s*вр\.|прош\.\s*вр\.|неперех\.|перех\.|несов\.|сов\.|сравн\.?\s+ст\.?|превосх\.?\s+ст\.?|страд\.|действ\.|возвр\.|личн\.|мн\.|ед\.|муж\.|жен\.|ср\.|пад\.|гл\.|сущ\.|прил\.|нареч\.|мест\.|предл\.|союз(?=[а-яё]\))|союз\.?|межд\.|числ\.|предик\.|прич\.|дееприч\.|част\.|суф\.|преф\.|прист\.|ad\.?|prp\.?)(?![\p{L}]))",
        QRegularExpression::CaseInsensitiveOption
    };
    static const QRegularExpression JOINED_CONJUNCTION_LABEL{
        R"((?<![\p{L}])союз(?=[а-яё]\)))",
        QRegularExpression::CaseInsensitiveOption
    };
    static const QRegularExpression USAGE_LABEL{
        R"((?<![\p{L}])(?:sl\.?|простореч\.|пренебр\.|неодобр\.|устар\.|книжн\.|шутл\.|презр\.|сниж\.|детск\.|редк\.|разг\.|прост\.|поэт\.|ирон\.|груб\.|бран\.|вульг\.|фам\.|офиц\.|эвф\.|перен\.|уст\.(?=[а-яё]\))|уст\.|ист\.|диал\.|жарг\.|арго\.?|обл\.|преим\.|употр\.|часто|редко|амер\.|брит\.)(?![\p{L}]))",
        QRegularExpression::CaseInsensitiveOption
    };
    static const QRegularExpression JOINED_USAGE_LABEL{
        R"((?<![\p{L}])уст\.(?=[а-яё]\)))",
        QRegularExpression::CaseInsensitiveOption
    };
    // The DSL wraps these usage notes in [c][trn][com] ... [/com][/c].
    // Yomitan's exported glossary is plain text and drops those tags, so
    // restore the grouping for "обычно" followed by an English form or a
    // grammatical/usage abbreviation instead of special-casing one word.
    static const QRegularExpression USUAL_USAGE_LABEL{
        R"((?<![\p{L}])обычно(?:[ \t]+(?:(?:мн|ед|сущ|прил|нареч|гл|страд|действ|неперех|перех|прич|пренебр|презр|устар|разг)\.(?:[ \t]+(?:наст|прош)\.[ \t]*вр\.)?|[A-Za-z][A-Za-z'’.-]*(?:[ \t]+[A-Za-z][A-Za-z'’.-]*)*))?)",
        QRegularExpression::CaseInsensitiveOption
    };
    static const QRegularExpression DOMAIN_LABEL{
        R"((?<![\p{L}])(?:конн\.?\s+спорт\.?|электр\.|информ\.|полигр\.|геогр\.|физиол\.|психол\.|архит\.|лингв\.|филос\.|типогр\.|юрид\.|зоол\.|анат\.|геод\.|геол\.|астрол\.|театр\.|текст\.|стр\.|спорт\.|муз\.|бот\.|биол\.|физ\.|хим\.|мед\.|юр\.|тех\.|эл\.|мор\.|воен\.|фин\.|мат\.|астр\.|лит\.|рел\.|библ\.|кул\.|полит\.|эк\.|вет\.|нарк\.|крим\.|охот\.|карт\.|метал\.|с\.\s*х\.|ж\.-д\.)(?![\p{L}]))",
        QRegularExpression::CaseInsensitiveOption
    };
    static const QRegularExpression EDITORIAL_LABEL{
        R"((?<![\p{L}])(?:сокращ\.|букв\.|сокр\.|т\.\s*е\.|т\.\s*д\.|т\.\s*п\.|ср\.|см\.)(?![\p{L}]))",
        QRegularExpression::CaseInsensitiveOption
    };
    static const QRegularExpression SEE_ALSO_LABEL{
        R"((?<![\p{L}])см\.\s*тж\.(?![\p{L}\d]))",
        QRegularExpression::CaseInsensitiveOption
    };
    static const QRegularExpression LANGUAGE_LABEL{
        R"((?<![\p{L}])(?:лат\.|фр\.|нем\.)(?![\p{L}]))",
        QRegularExpression::CaseInsensitiveOption
    };
    static const QRegularExpression GRAM_SECTION_HEADER{
        R"(^\s*Gram:\s*$)",
        QRegularExpression::CaseInsensitiveOption
    };
    static const QRegularExpression SYN_PREFIX{
        R"(^\s*(Syn:)(.*)$)",
        QRegularExpression::CaseInsensitiveOption
    };
    static const QRegularExpression SYNONYM_NUMERIC_REFERENCE{
        R"(\(?\p{N}+[.)]?\)?)"
    };
    static const QRegularExpression SYNONYM_REFERENCE_ONLY{
        R"(^\s*(?:[①-⑳]|\(?\p{N}+[.)]?\)?)\s*$)"
    };
    static const QRegularExpression ANT_PREFIX{
        R"(^\s*(Ant:)(.*)$)",
        QRegularExpression::CaseInsensitiveOption
    };
    static const QRegularExpression SENSE_MARKER{R"(^\s*\d+\.)"};
    static const QRegularExpression GRAM_SECTION_BOUNDARY{
        R"(^\s*(?:[①-⑳]|\d+\.|[•·]{2,}|=+))"
    };
    static const QRegularExpression INLINE_SENSE_MARKER{
        R"(^\s*(\d+\.\s+.*?)([①-⑳].*)$)"
    };
    static const QRegularExpression INLINE_POS_HEADING{
        R"(^\s*((?:\d+\.\s*)?(?:фраз\.\s*гл\.|притяж\.\s*мест\.|прич\.\s*(?:наст|прош)\.\s*вр\.|гл\.|сущ\.|прил\.|нареч\.|мест\.|предл\.|союз\.?|межд\.|числ\.|предик\.|прич\.))(?=\s*[①-⑳]|\p{L})(.*)$)",
        QRegularExpression::CaseInsensitiveOption
    };
    static const QRegularExpression NUMBERED_POS_HEADING{
        R"(^([0-9]+\.\s*)(фраз\.\s*гл\.|притяж\.\s*мест\.|прич\.\s*(?:наст|прош)\.\s*вр\.|гл\.|сущ\.|прил\.|нареч\.|мест\.|предл\.|союз\.?|межд\.|числ\.|предик\.|прич\.)(.*)$)",
        QRegularExpression::CaseInsensitiveOption
    };
    static const QRegularExpression CIRCLED_MARKER{
        R"(^([①-⑳])\s*(.*)$)"
    };
    static const QRegularExpression RUSSIAN_SUBSENSE{
        R"(^\s*[а-яёА-ЯЁ]\)\s*(.*)$)"
    };
    static const QRegularExpression BULLET_MARKER{
        R"(^\s*[•·]\s*(.*)$)"
    };
    static const QRegularExpression DIALECT_MARKER{
        R"((?:брит|амер)\.)",
        QRegularExpression::CaseInsensitiveOption
    };
    static const QRegularExpression SLASH_SEPARATOR{R"(\s*/\s*)"};
    static const QRegularExpression IRREGULAR_VERB_FORMS{
        R"(\s*[;,]\s*прош\.\s*вр\.(?:\s*[A-Za-z][A-Za-z'’.-]*(?:[ \t]+[A-Za-z][A-Za-z'’.-]*)*(?:[ \t]*(?:,|/)[ \t]*[A-Za-z][A-Za-z'’.-]*(?:[ \t]+[A-Za-z][A-Za-z'’.-]*)*)*)?\s*[;,]?\s*прич\.\s*прош\.\s*вр\.\s*[A-Za-z][A-Za-z'’.-]*(?:[ \t]+[A-Za-z][A-Za-z'’.-]*)*(?:[ \t]*(?:,|/)[ \t]*[A-Za-z][A-Za-z'’.-]*(?:[ \t]+[A-Za-z][A-Za-z'’.-]*)*)*)",
        QRegularExpression::CaseInsensitiveOption
    };
    static const QRegularExpression PRONUNCIATION_LINE{
        R"(^([ \t]*)(?:[•·][ \t]*)?(\[[^\]\r\n]+\](?:(?:[ \t]*(?:,|/)[ \t]*|[ \t]+)?\[[^\]\r\n]+\])*)([ \t]*)(.*)$)"
    };
    static const QRegularExpression ROMAN_SECTION_HEADER{
        R"(^\s*(VIII|VII|VI|IV|V|III|II|IX|X|I)\)?\s*$)"
    };
    static const QRegularExpression ROMAN_PRONUNCIATION_PREFIX{
        R"(^([ \t]*(?:[•·][ \t]*)?)(VIII|VII|VI|IV|V|III|II|IX|X|I)\)?[ \t]+(\[[^\]\r\n]+\].*)$)",
        QRegularExpression::CaseInsensitiveOption
    };
    static const QRegularExpression ROMAN_NUMBERED_POS_HEADING{
        R"(^\s*([①-⑳])\s*(фраз\.\s*гл\.|притяж\.\s*мест\.|прич\.\s*(?:наст|прош)\.\s*вр\.|гл\.|сущ\.|прил\.|нареч\.|мест\.|предл\.|союз\.?|межд\.|числ\.|предик\.|прич\.)(.*)$)",
        QRegularExpression::CaseInsensitiveOption
    };
    static const QRegularExpression ROMAN_POS_HEADING{
        R"(^\s*(VIII|VII|VI|IV|V|III|II|IX|X|I)\s+(фраз\.\s*гл\.|притяж\.\s*мест\.|прич\.\s*(?:наст|прош)\.\s*вр\.|гл\.|сущ\.|прил\.|нареч\.|мест\.|предл\.|союз\.?|межд\.|числ\.|предик\.|прич\.)(.*)$)",
        QRegularExpression::CaseInsensitiveOption
    };

    struct DisplayLine {
        QString marker;
        QString text;
        int depth = 0;
        bool heading = false;
        bool example = false;
    };

    // Remove verb-form metadata before line parsing. The labels can be
    // separated by a wrapped newline; this also separates an immediately
    // following 1) marker so it can become the circled ①.
    QString glossaryText = splitAttachedSenseMarkers(
        splitRomanNumberedSenseMarkers(
            separatePronunciationFromFollowingText(text)
        )
    );
    glossaryText.replace(IRREGULAR_VERB_FORMS, QStringLiteral(" "));
    glossaryText = separateLetterFromOpeningParenthesis(glossaryText);
    glossaryText = suppressUntranslatedPhraseLists(glossaryText);
    QStringList sourceLines = glossaryText.split('\n', Qt::KeepEmptyParts);
    QSet<qsizetype> pairedExampleLines;
    static const QRegularExpression NUMBERED_RUSSIAN_TRANSLATION{
        R"(^\s*(?:[①-⑳]|\d{1,3}[.)]?)\s*[—–]\s*[\x{0400}-\x{04FF}])"
    };
    for (qsizetype i = 0; i < sourceLines.size(); ++i)
    {
        if (!isEnglishLingvoExampleSentence(sourceLines[i]))
        {
            continue;
        }
        qsizetype translationLine = i + 1;
        while (translationLine < sourceLines.size() &&
               sourceLines[translationLine].trimmed().isEmpty())
        {
            ++translationLine;
        }
        if (translationLine < sourceLines.size() &&
            NUMBERED_RUSSIAN_TRANSLATION.match(
                sourceLines[translationLine]
            ).hasMatch())
        {
            pairedExampleLines.insert(i);
            pairedExampleLines.insert(translationLine);
        }
    }
    QStringList displayLines;
    displayLines.reserve(sourceLines.size());
    QSet<qsizetype> indentedHeaderLineIndices;
    QSet<qsizetype> exampleLineIndices;
    bool firstContentLine = true;
    bool skippingGramSection = false;
    for (qsizetype i = 0; i < sourceLines.size(); ++i)
    {
        QString line = sourceLines[i];

        // Yomitan flattens ABBYY's "Gram:" blocks into ordinary text lines.
        // They contain grammar commentary rather than translation senses.
        if (skippingGramSection)
        {
            if (!GRAM_SECTION_BOUNDARY.match(line).hasMatch())
            {
                continue;
            }
            skippingGramSection = false;
            if (line.trimmed() == QStringLiteral("••") ||
                line.trimmed() == QStringLiteral("··"))
            {
                continue;
            }
        }
        if (GRAM_SECTION_HEADER.match(line).hasMatch())
        {
            skippingGramSection = true;
            continue;
        }

        const bool exampleLine = pairedExampleLines.contains(i) ||
            isLingvoExampleLine(line);
        if ((filterExampleLines && exampleLine) ||
            line.trimmed() == QStringLiteral("•") ||
            line.trimmed() == QStringLiteral("••"))
        {
            continue;
        }

        if (exampleLine)
        {
            line.replace(QRegularExpression(R"(\[/?ex\])"), QString());
            displayLines.emplaceBack(std::move(line));
            exampleLineIndices.insert(displayLines.size() - 1);
            firstContentLine = false;
            continue;
        }

        const auto antonym = ANT_PREFIX.match(line);
        if (antonym.hasMatch())
        {
            // ABBYY can serialize Ant: inline or place the antonym list on
            // the next line. Antonyms are intentionally omitted from cards.
            if (antonym.captured(2).trimmed().isEmpty() &&
                i + 1 < sourceLines.size() &&
                !GRAM_SECTION_HEADER.match(sourceLines[i + 1]).hasMatch() &&
                !GRAM_SECTION_BOUNDARY.match(sourceLines[i + 1]).hasMatch() &&
                !SYN_PREFIX.match(sourceLines[i + 1]).hasMatch() &&
                !ANT_PREFIX.match(sourceLines[i + 1]).hasMatch())
            {
                ++i;
            }
            continue;
        }

        if (line.trimmed().compare(QStringLiteral("Syn:"), Qt::CaseInsensitive) == 0 &&
            i + 1 < sourceLines.size() && !sourceLines[i + 1].trimmed().isEmpty() &&
            !isLingvoExampleLine(sourceLines[i + 1]) &&
            !CIRCLED_MARKER.match(sourceLines[i + 1]).hasMatch() &&
            !SENSE_MARKER.match(sourceLines[i + 1]).hasMatch())
        {
            line = line.trimmed() + QLatin1Char(' ') + sourceLines[++i].trimmed();
        }

        const auto synonymLine = SYN_PREFIX.match(line);
        if (synonymLine.hasMatch())
        {
            QString synonymText = synonymLine.captured(2);
            synonymText.remove(SYNONYM_NUMERIC_REFERENCE);
            synonymText.replace(
                QRegularExpression(R"([ \t]+(?=[,;]))"), QString()
            );
            synonymText.replace(
                QRegularExpression(R"([ \t]{2,})"), QStringLiteral(" ")
            );
            synonymText = synonymText.trimmed();

            // ABBYY/Yomitan may put the linked sense number on a separate
            // line after the synonym. Keep it out of the definition parser.
            if (i + 1 < sourceLines.size() &&
                SYNONYM_REFERENCE_ONLY.match(sourceLines[i + 1]).hasMatch())
            {
                ++i;
            }

            if (synonymText.isEmpty())
            {
                continue;
            }
            line = synonymLine.captured(1) + QLatin1Char(' ') + synonymText;
        }

        if (firstContentLine && !line.trimmed().isEmpty())
        {
            const auto romanPronunciation =
                ROMAN_PRONUNCIATION_PREFIX.match(line);
            if (romanPronunciation.hasMatch())
            {
                displayLines.emplaceBack(
                    romanPronunciation.captured(1) +
                        romanPronunciation.captured(2).toUpper()
                );
                line = romanPronunciation.captured(1) +
                    romanPronunciation.captured(3);
            }

            if (ROMAN_SECTION_HEADER.match(line).hasMatch())
            {
                displayLines.emplaceBack(std::move(line));
                continue;
            }

            // The dialect labels are displayed beside the headword along with
            // their independent audio buttons. Keep the pronunciation here.
            line.remove(DIALECT_MARKER);
            line.replace(SLASH_SEPARATOR, QStringLiteral(" "));

            // Pronunciation belongs on its own first line, without the
            // glossary bullet. Some converted entries append a POS/domain
            // label directly after the closing bracket; put that label on a
            // nested line instead of letting it run into the transcription.
            const auto pronunciation = PRONUNCIATION_LINE.match(line);
            if (pronunciation.hasMatch())
            {
                const QString metadata = pronunciation.captured(4).trimmed();
                const auto grammarAtStart = GRAMMAR_LABEL.match(metadata);
                const auto abbreviationAtStart = EDITORIAL_LABEL.match(metadata);
                const auto languageAtStart = LANGUAGE_LABEL.match(metadata);
                const bool isServiceMetadata =
                    (grammarAtStart.hasMatch() && grammarAtStart.capturedStart() == 0) ||
                    (abbreviationAtStart.hasMatch() && abbreviationAtStart.capturedStart() == 0) ||
                    (languageAtStart.hasMatch() && languageAtStart.capturedStart() == 0);

                if (!includePronunciation)
                {
                    // The UI displays the pronunciation in a dedicated row
                    // with the UK/US audio controls beside it.
                    if (!metadata.isEmpty())
                    {
                        displayLines.emplaceBack(metadata);
                        if (isServiceMetadata)
                        {
                            indentedHeaderLineIndices.insert(displayLines.size() - 1);
                        }
                    }
                    firstContentLine = false;
                    continue;
                }

                line = pronunciation.captured(1) + pronunciation.captured(2);
                if (!metadata.isEmpty() && !isServiceMetadata)
                {
                    line += pronunciation.captured(3) + metadata;
                }
                else if (isServiceMetadata)
                {
                    displayLines.emplaceBack(std::move(line));
                    displayLines.emplaceBack(metadata);
                    indentedHeaderLineIndices.insert(displayLines.size() - 1);
                    firstContentLine = false;
                    continue;
                }
            }
            firstContentLine = false;
        }
        displayLines.emplaceBack(std::move(line));
    }

    QVector<DisplayLine> rows;
    rows.reserve(displayLines.size() + 4);
    int activeDepth = 0;
    QString pendingRomanMarker;
    for (qsizetype displayLineIndex = 0;
         displayLineIndex < displayLines.size();
         ++displayLineIndex)
    {
        QString line = displayLines[displayLineIndex];
        if (exampleLineIndices.contains(displayLineIndex))
        {
            DisplayLine example;
            example.text = line.trimmed();
            example.example = true;
            rows.append(std::move(example));
            continue;
        }
        const bool isIndentedHeaderLine =
            indentedHeaderLineIndices.contains(displayLineIndex);
        line = useCircledSenseMarkers(line).trimmed();
        if (line.isEmpty())
        {
            continue;
        }

        const auto romanHeader = ROMAN_SECTION_HEADER.match(line);
        if (romanHeader.hasMatch())
        {
            if (!pendingRomanMarker.isEmpty())
            {
                DisplayLine previousRoman;
                previousRoman.text = pendingRomanMarker;
                previousRoman.heading = true;
                rows.append(std::move(previousRoman));
            }
            pendingRomanMarker = romanHeader.captured(1).toUpper();
            activeDepth = 0;
            continue;
        }

        const auto romanNumberedPosHeading =
            ROMAN_NUMBERED_POS_HEADING.match(line);
        const auto romanPosHeading = ROMAN_POS_HEADING.match(line);
        if (romanPosHeading.hasMatch() || romanNumberedPosHeading.hasMatch())
        {
            const bool hasCircledSectionMarker =
                romanNumberedPosHeading.hasMatch();
            const QRegularExpressionMatch &numberedPosHeading =
                romanNumberedPosHeading;
            const QString sectionMarker = !pendingRomanMarker.isEmpty() ?
                pendingRomanMarker : hasCircledSectionMarker ?
                    circledGlossaryMarkerToRoman(
                        numberedPosHeading.captured(1)
                    ) : romanPosHeading.captured(1).toUpper();
            const QString pos = hasCircledSectionMarker ?
                numberedPosHeading.captured(2).trimmed() :
                romanPosHeading.captured(2).trimmed();
            DisplayLine posRow;
            posRow.text = sectionMarker + QLatin1Char(' ') + pos;
            posRow.heading = true;
            rows.append(std::move(posRow));
            pendingRomanMarker.clear();
            activeDepth = 1;
            line = hasCircledSectionMarker ?
                numberedPosHeading.captured(3).trimmed() :
                romanPosHeading.captured(3).trimmed();
            if (line.isEmpty())
            {
                continue;
            }
        }
        else if (!pendingRomanMarker.isEmpty())
        {
            const auto posHeading = INLINE_POS_HEADING.match(line);
            const QString posLabel = posHeading.captured(1).trimmed();
            if (posHeading.hasMatch() && !posLabel.front().isDigit())
            {
                DisplayLine posRow;
                posRow.text = pendingRomanMarker + QLatin1Char(' ') +
                    posLabel;
                posRow.heading = true;
                rows.append(std::move(posRow));
                pendingRomanMarker.clear();
                activeDepth = 1;
                line = posHeading.captured(2).trimmed();
                if (line.isEmpty())
                {
                    continue;
                }
            }
            else
            {
                DisplayLine romanRow;
                romanRow.text = pendingRomanMarker;
                romanRow.heading = true;
                rows.append(std::move(romanRow));
                pendingRomanMarker.clear();
            }
        }

        const auto inlineSense = INLINE_SENSE_MARKER.match(line);
        if (inlineSense.hasMatch())
        {
            DisplayLine posRow;
            posRow.text = inlineSense.captured(1).trimmed();
            posRow.heading = true;
            rows.append(posRow);
            line = inlineSense.captured(2).trimmed();
        }

        // Some Lingvo entries join the part-of-speech label to its first
        // sense marker or gloss (for example, "сущ.1)" or
        // "прил.концертный"). Keep the label on its own row.
        const auto inlinePosHeading = INLINE_POS_HEADING.match(line);
        if (inlinePosHeading.hasMatch())
        {
            DisplayLine posRow;
            posRow.text = inlinePosHeading.captured(1).trimmed();
            posRow.heading = true;
            rows.append(posRow);
            activeDepth = 1;
            line = inlinePosHeading.captured(2).trimmed();
        }

        // The flattened Lingvo export can join the POS tag and its first
        // gloss (for example, "2. сущ.объятия"). Keep the group heading on
        // its own row and indent the gloss underneath it.
        const auto numberedPosHeading = NUMBERED_POS_HEADING.match(line);
        if (numberedPosHeading.hasMatch() &&
            !numberedPosHeading.captured(3).trimmed().isEmpty())
        {
            DisplayLine posRow;
            posRow.text = numberedPosHeading.captured(1).trimmed() +
                QLatin1Char(' ') + numberedPosHeading.captured(2).trimmed();
            posRow.heading = true;
            rows.append(posRow);
            activeDepth = 1;
            line = numberedPosHeading.captured(3).trimmed();
        }

        DisplayLine row;
        const auto circled = CIRCLED_MARKER.match(line);
        const auto russianSub = RUSSIAN_SUBSENSE.match(line);
        const auto bullet = BULLET_MARKER.match(line);
        if (SENSE_MARKER.match(line).hasMatch())
        {
            row.text = line;
            row.heading = true;
            activeDepth = 0;
        }
        else if (circled.hasMatch())
        {
            row.marker = circled.captured(1);
            row.text = circled.captured(2);
            row.depth = 1;
            activeDepth = 1;
        }
        else if (russianSub.hasMatch())
        {
            // Replace DSL's а), б), в)... subnumbering with nested bullets.
            row.marker = QStringLiteral("•");
            row.text = russianSub.captured(1);
            row.depth = 2;
            activeDepth = 2;
        }
        else if (bullet.hasMatch())
        {
            row.text = bullet.captured(1).trimmed();
            const auto grammaticalHeader = GRAMMAR_LABEL.match(row.text);
            if (grammaticalHeader.hasMatch() &&
                grammaticalHeader.capturedStart() == 0)
            {
                // A POS/domain label introduces an entry section, so it is
                // rendered as a heading rather than as a bulleted gloss.
                row.depth = activeDepth;
            }
            else
            {
                row.marker = QStringLiteral("•");
                row.depth = activeDepth == 0 ? 0 : qMax(2, activeDepth);
                if (row.depth > 0)
                {
                    activeDepth = row.depth;
                }
            }
        }
        else
        {
            row.text = line;
            if (SYN_PREFIX.match(line).hasMatch() || ANT_PREFIX.match(line).hasMatch())
            {
                row.depth = qMax(2, activeDepth);
            }
            else
            {
                row.depth = isIndentedHeaderLine ? 1 : activeDepth;
            }
        }
        if (!row.text.isEmpty() || !row.marker.isEmpty())
        {
            rows.append(row);
        }
    }
    if (!pendingRomanMarker.isEmpty())
    {
        DisplayLine romanRow;
        romanRow.text = pendingRomanMarker;
        romanRow.heading = true;
        rows.append(std::move(romanRow));
    }

    QString out;
    for (qsizetype rowIndex = 0; rowIndex < rows.size(); ++rowIndex)
    {
        const DisplayLine &row = rows[rowIndex];
        if (row.example)
        {
            const QString exampleBackground = darkCanvas ?
                QStringLiteral("#3B3B3B") : QStringLiteral("#E6E6E6");
            QStringList exampleLines;
            while (rowIndex < rows.size() && rows[rowIndex].example)
            {
                exampleLines.append(rows[rowIndex].text.toHtmlEscaped());
                ++rowIndex;
            }
            --rowIndex;
            QString exampleContent;
            for (const QString &exampleLine : exampleLines)
            {
                if (!exampleContent.isEmpty())
                {
                    exampleContent += QStringLiteral(
                        "<div style=\"border-top:1px solid %1; "
                              "background-color:%2; margin:0; "
                              "padding:5px 0;\"></div>"
                    ).arg(dividerColor, exampleBackground);
                }
                exampleContent += exampleLine;
            }
            out += formatExampleBlock(
                exampleBackground,
                dividerColor,
                exampleContent
            );
            continue;
        }

        QString content;
        const auto synonym = SYN_PREFIX.match(row.text);
        const auto antonym = ANT_PREFIX.match(row.text);
        if (!includeSynonyms && synonym.hasMatch())
        {
            continue;
        }
        if (synonym.hasMatch() || antonym.hasMatch())
        {
            const auto match = synonym.hasMatch() ? synonym : antonym;
            content = QStringLiteral(
                "<span style=\"font-size:85%;\"><i><span style=\"color:%1;\">%2</span></i> <span>%3</span></span>"
            ).arg(
                synonymColor,
                match.captured(1).toHtmlEscaped(),
                match.captured(2).trimmed().toHtmlEscaped()
            );
        }
        else
        {
            struct StyledLabelSpan
            {
                qsizetype start;
                qsizetype end;
                QString color;
            };
            QVector<StyledLabelSpan> styledLabelSpans;
            const auto collectStyledLabels = [&] (
                const QRegularExpression &pattern,
                const QString &color)
            {
                QRegularExpressionMatchIterator matches =
                    pattern.globalMatch(row.text);
                while (matches.hasNext())
                {
                    const QRegularExpressionMatch match = matches.next();
                    const qsizetype start = match.capturedStart();
                    const qsizetype end = match.capturedEnd();
                    const bool overlapsExistingLabel = std::any_of(
                        styledLabelSpans.cbegin(),
                        styledLabelSpans.cend(),
                        [start, end](const StyledLabelSpan &span)
                        {
                            return start < span.end && end > span.start;
                        }
                    );
                    if (!overlapsExistingLabel)
                    {
                        styledLabelSpans.append({start, end, color});
                    }
                }
            };

            // Service labels are italicized and colored by category so that
            // grammar, usage/register, subject area, language, and editorial
            // notes remain distinguishable without backgrounds or outlines.
            // The Lingvo [c] usage note has priority over its inner grammar label.
            collectStyledLabels(SEE_ALSO_LABEL, editorialColor);
            collectStyledLabels(USUAL_USAGE_LABEL, domainColor);
            collectStyledLabels(JOINED_CONJUNCTION_LABEL, grammarColor);
            collectStyledLabels(JOINED_USAGE_LABEL, usageColor);
            collectStyledLabels(GRAMMAR_LABEL, grammarColor);
            collectStyledLabels(USAGE_LABEL, usageColor);
            collectStyledLabels(DOMAIN_LABEL, domainColor);
            collectStyledLabels(LANGUAGE_LABEL, domainColor);
            collectStyledLabels(EDITORIAL_LABEL, editorialColor);

            std::sort(
                styledLabelSpans.begin(),
                styledLabelSpans.end(),
                [](const StyledLabelSpan &left, const StyledLabelSpan &right)
                {
                    return left.start < right.start;
                }
            );

            QString styled;
            qsizetype position = 0;
            for (const StyledLabelSpan &span : styledLabelSpans)
            {
                styled += row.text.sliced(
                    position,
                    span.start - position
                ).toHtmlEscaped();
                styled += QStringLiteral(
                    "<i><span style=\"color:%1;\">%2</span></i>"
                ).arg(
                    span.color,
                    row.text.sliced(span.start, span.end - span.start)
                        .toHtmlEscaped()
                );
                if (span.end < row.text.size() &&
                    !row.text.at(span.end).isSpace() &&
                    (row.text.at(span.end).isLetter() ||
                     row.text.at(span.end) == QLatin1Char('(')))
                {
                    styled += QLatin1Char(' ');
                }
                position = span.end;
            }
            styled += row.text.sliced(position).toHtmlEscaped();
            content = std::move(styled);

            if (row.heading)
            {
                static const QRegularExpression NUMBERED_HEADING{R"(^\s*\d+\.)"};
                static const QRegularExpression ROMAN_HEADING{
                    R"(^\s*(VIII|VII|VI|IV|V|III|II|IX|X|I)(?=\s|$))"
                };
                const QRegularExpressionMatch headingMatch =
                    NUMBERED_HEADING.match(content);
                if (headingMatch.hasMatch())
                {
                    const QString heading = headingMatch.captured();
                    content = QStringLiteral(
                        "<span style=\"color:%1; font-weight:700;\">%2</span>%3"
                    ).arg(
                        headingColor,
                        heading.toHtmlEscaped(),
                        content.sliced(headingMatch.capturedEnd())
                    );
                }
                else
                {
                    const QRegularExpressionMatch romanMatch =
                        ROMAN_HEADING.match(content);
                    if (romanMatch.hasMatch())
                    {
                        const QString roman = romanMatch.captured(1);
                        content = QStringLiteral(
                            "<span style=\"color:%1; font-weight:700;\">%2</span>%3"
                        ).arg(
                            headingColor,
                            roman.toHtmlEscaped(),
                            content.sliced(romanMatch.capturedEnd())
                        );
                    }
                }
            }
        }

        const QString marker = row.marker.toHtmlEscaped();
        const QString markerStyle = row.marker == QStringLiteral("•") ?
            QStringLiteral("font-weight:700;") :
            QStringLiteral("color:%1; font-weight:700;").arg(headingColor);
        const int synonymIndent = synonym.hasMatch() ? 12 : 0;
        const int indent = row.depth * 18 + synonymIndent;
        const int markerWidth = row.marker.isEmpty() ? 0 : 20;
        const int topMargin = row.heading ? 7 : 1;
        const QString markerHtml = row.marker.isEmpty() ? QString() :
            QStringLiteral("<span style=\"%1\">%2</span>&nbsp;&nbsp;")
                .arg(markerStyle, marker);
        out += QStringLiteral(
            "<p style=\"text-align:left; margin-top:%1px; margin-bottom:0px; margin-left:%2px; text-indent:-%3px;\">%4%5</p>"
        ).arg(topMargin).arg(indent + markerWidth).arg(markerWidth).arg(markerHtml, content);
    }
    return out;
}

}

/* End Local Functions */

QString StructuredRichText::formatLingvoGlossaryForExport(
    const QString &text,
    bool includeSynonyms)
{
    // Anki stores the Lingvo transcription in {reading}; keep it out of the
    // glossary/meaning field, while keeping examples hidden as in the UI.
    return formatLingvoGlossaryText(
        text, QColor(Qt::white), false, true, includeSynonyms
    );
}

QString StructuredRichText::formatGenericGlossaryForExport(
    const QString &text,
    qsizetype &remainingExamples,
    bool includeSynonyms)
{
    return formatGenericGlossaryText(
        text, QColor(Qt::white), false, false, &remainingExamples,
        true, false, includeSynonyms
    );
}

QJsonArray StructuredRichText::stripAmericanEnglishGenderMarkers(
    const QJsonArray &definitions)
{
    return stripAmericanEnglishGlossaryMarkers(definitions);
}

QString StructuredRichText::stripAmericanEnglishGenderMarkers(
    const QString &text)
{
    return stripRussianGenderMarkers(text);
}

QJsonValue StructuredRichText::filterGenericStructuredExamples(
    const QJsonValue &content,
    qsizetype &remainingExamples)
{
    return filterStructuredExampleNodes(content, remainingExamples);
}

QString StructuredRichText::formatGlossarySelectionForExport(
    const QString &text,
    const QString &dictionaryName)
{
    if (dictionaryName.contains(
            QStringLiteral("AmericanEnglish"), Qt::CaseInsensitive))
    {
        return formatGenericGlossaryText(
            stripAmericanEnglishGenderMarkers(text),
            QColor(Qt::white),
            false,
            false,
            nullptr,
            true,
            true
        );
    }
    if (dictionaryName.contains(
            QStringLiteral("LingvoUniversal"), Qt::CaseInsensitive))
    {
        // Lingvo Universal examples remain hidden in the dictionary card and in
        // selected glossary text.
        return formatLingvoGlossaryText(text, QColor(Qt::white), false);
    }

    // Apply the same marker and hierarchy formatting used in generic cards.
    return formatGenericGlossaryText(
        text, QColor(Qt::white), false, false, nullptr
    );
}

/* Begin Constructor/Destructor */

StructuredRichText::StructuredRichText(QObject *parent) : QObject(parent)
{

}

StructuredRichText::~StructuredRichText()
{

}

/* End Constructor/Destructor */
/* Begin Public Methods */

QString StructuredRichText::parse(
    const DictionaryInfo *info,
    const QJsonArray &content,
    Setting::GlossaryStyle style,
    const QQuickItem *item,
    const QFont &font,
    const QColor &color,
    const QColor &backgroundColor,
    bool includeSynonyms) const
{
    if (info == nullptr)
    {
        return "";
    }

    const bool isLingvoUniversal = info->name().contains(
        QStringLiteral("LingvoUniversal"), Qt::CaseInsensitive
    );
    const bool isAmericanEnglish = info->name().contains(
        QStringLiteral("AmericanEnglish"), Qt::CaseInsensitive
    );

    /* JSON keys and discriminator values for top-level glossary entries */
    constexpr const char *KEY_TYPE = "type";
    constexpr const char *KEY_CONTENT = "content";
    constexpr const char *KEY_TEXT = "text";
    constexpr const char *VALUE_TYPE_IMAGE = "image";
    constexpr const char *VALUE_TYPE_STRUCTURED_CONTENT = "structured-content";
    constexpr const char *VALUE_TYPE_TEXT = "text";

    StructuredRichText::Context ctx;
    ctx.info = info;
    ctx.suppressExampleContent = isLingvoUniversal;
    ctx.style = style;
    if (item && item->window())
    {
        ctx.screen = item->window()->screen();
    }
    ctx.font = font;
    ctx.dictionaryStyles = info->styles();
    if (ctx.dictionaryStyles != nullptr)
    {
        ctx.needsElementChildCount =
            ctx.dictionaryStyles->parsedStylesheet().usesElementChildCount;
    }

    ctx.rootFontPixelSize = fontPixelSize(font, ctx.screen);
    ctx.parentFontPixelSize = ctx.rootFontPixelSize;
    if (color.isValid())
    {
        ctx.glossaryTextColor = color.name(
            color.alpha() == OPAQUE_ALPHA ?
                QColor::HexRgb :
                QColor::HexArgb
        );
        ctx.textColor = ctx.glossaryTextColor;
    }
    if (backgroundColor.isValid())
    {
        ctx.glossaryBackgroundColor = backgroundColor.name(
            backgroundColor.alpha() == OPAQUE_ALPHA ?
                QColor::HexRgb :
                QColor::HexArgb
        );
        ctx.backgroundColor = ctx.glossaryBackgroundColor;
    }
    ctx.paintedBackgroundColor = ctx.backgroundColor;
    ctx.selectorElements.reserve(SELECTOR_ELEMENT_RESERVE);
    ctx.elements.reserve(ELEMENT_STACK_RESERVE);
    ctx.siblings.reserve(SIBLING_STACK_RESERVE);
    ctx.lists.reserve(LIST_STACK_RESERVE);
    ctx.resolvedCssValues.reserve(CSS_VALUE_CACHE_RESERVE);
    ctx.basepath = DirectoryUtils::getDictionaryResourceDir();
#if defined(Q_OS_WIN)
    ctx.basepath.prepend('/');
    ctx.basepath.replace('\\', '/');
#endif
    ctx.basepath.prepend("file://");
    ctx.basepath += '/';
    ctx.basepath += info->name();
    ctx.basepath += '/';

    // Some dictionaries export each phrase-list item as its own top-level
    // glossary node. Filter untranslated phrase lists across those siblings
    // too, including in LingvoUniversal.
    QJsonArray displayContent =
        suppressUntranslatedStructuredPhraseLists(content);
    if (isAmericanEnglish)
    {
        displayContent = stripAmericanEnglishGenderMarkers(displayContent);
    }
    // Top-level entries are rendered one by one, so group adjacent example
    // nodes here before the renderer can give each one its own box.
    displayContent = groupAdjacentTopLevelStructuredExamples(displayContent);

    const bool containsSc = containsStructuredContent(displayContent);
    const bool shouldUseBullets =
        style == Setting::GlossaryStyleBullet && !containsSc &&
        !isLingvoUniversal;

    QSet<qsizetype> numberedGlossaryEntries;
    QSet<qsizetype> circledSubEntries;
    QString activeRomanMarker;
    for (qsizetype i = 0; i < displayContent.size(); ++i)
    {
        const QJsonValue value = displayContent[i];
        if (!value.isString())
        {
            continue;
        }
        const QString text = value.toString();
        const QString romanMarker = leadingRomanGlossaryMarker(text);
        if (!romanMarker.isEmpty())
        {
            numberedGlossaryEntries.insert(i);
            activeRomanMarker = romanMarker;
        }
        else if (startsNumberedGlossaryMarker(text) ||
                 startsCircledGlossaryMarker(text))
        {
            numberedGlossaryEntries.insert(i);
            if (activeRomanMarker == QStringLiteral("II") &&
                startsCircledGlossaryMarker(text))
            {
                circledSubEntries.insert(i);
            }
            else
            {
                activeRomanMarker.clear();
            }
        }
    }

    QString glossary;
    glossary.reserve(OUTPUT_RESERVE);
    glossary = "<html><head></head><body>";

    StructuredList fallbackList{"ul", "disc"};
    for (qsizetype i = 0; i < displayContent.size(); ++i)
    {
        qsizetype remainingExamples = 2;
        const QJsonValue &val = displayContent[i];
        const bool circledSubEntry = circledSubEntries.contains(i);
        const bool renderNumberedEntry =
            numberedGlossaryEntries.contains(i) || circledSubEntry;
        const bool useFallbackBullet =
            shouldUseBullets && !renderNumberedEntry;

        if (useFallbackBullet)
        {
            /* Marker-to-content gap relative to the inherited font size */
            constexpr double RELATIVE_INDENT_FACTOR = 0.35;

            ++fallbackList.item;
            glossary += "<table "
                "cellspacing=\"0\" "
                "width=\"100%\">"
                    "<tr><td "
                    "style=\""
                        "vertical-align: top; "
                        "white-space: nowrap; "
                        "padding-right: ";
            glossary += formatPixelSize(
                ctx.parentFontPixelSize * RELATIVE_INDENT_FACTOR
            );
            glossary += "px;\">";
            addSelectionSentinel(LIST_ITEM_SENTINEL, glossary);
            addSelectionSentinel(LIST_MARKER_START_SENTINEL, glossary);
            glossary += escapeHtml(listMarker(fallbackList, ""));
            addSelectionSentinel(LIST_MARKER_END_SENTINEL, glossary);
            glossary += "</td>"
                "<td width=\"100%\" style=\"vertical-align: top;\">";
        }

        switch (val.type())
        {
            case QJsonValue::Type::String:
                if (isLingvoUniversal)
                {
                    glossary += formatLingvoGlossaryText(
                        val.toString(), backgroundColor, false, true,
                        includeSynonyms
                    );
                }
                else
                {
                    glossary += formatGenericGlossaryText(
                        val.toString(),
                        backgroundColor,
                        circledSubEntry,
                        false,
                        &remainingExamples,
                        true,
                        isAmericanEnglish,
                        includeSynonyms
                    );
                }
                break;

            case QJsonValue::Type::Object:
            {
                QJsonObject obj = val.toObject();
                if (obj[KEY_TYPE] == VALUE_TYPE_STRUCTURED_CONTENT)
                {
                    QJsonValue structuredContent = obj[KEY_CONTENT];
                    if (!isLingvoUniversal)
                    {
                        structuredContent = filterGenericStructuredExamples(
                            structuredContent, remainingExamples
                        );
                    }
                    addStructuredChildren(
                        structuredContent, ctx, glossary
                    );
                }
                else if (obj[KEY_TYPE] == VALUE_TYPE_IMAGE)
                {
                    addImage(obj, ctx, glossary);
                }
                else if (obj[KEY_TYPE] == VALUE_TYPE_TEXT)
                {
                    if (isLingvoUniversal)
                    {
                        addText(obj, glossary, includeSynonyms);
                    }
                    else
                    {
                        glossary += formatGenericGlossaryText(
                            obj[KEY_TEXT].toString(),
                            backgroundColor,
                            circledSubEntry,
                            false,
                            &remainingExamples,
                            true,
                            isAmericanEnglish,
                            includeSynonyms
                        );
                    }
                }
                break;
            }

            default:
                break;
        }

        if (containsSc)
        {
            /* Ignore this check if structured content is detected */
        }
        else if (useFallbackBullet)
        {
            glossary += "</td></tr></table>";
        }
        else if (i >= displayContent.size() - 1)
        {
            /* Avoid putting <br> or | after the fine line */
        }
        else if (style == Setting::GlossaryStyleLineBreak)
        {
            glossary += "<br>";
        }
        else if (style == Setting::GlossaryStylePipe)
        {
            glossary += " | ";
        }
    }

    glossary = mergeAdjacentExampleBlocks(std::move(glossary));
    glossary += "</body></html>";

    return glossary;
}

/* End Public Methods */
/* Begin Structured Content Parsing */

void StructuredRichText::addStructuredData(
    const QJsonObject &obj, QString &out) const
{
    for (const QString &key : obj.keys())
    {
        const QJsonValue &val = obj[key];
        if (!val.isString())
        {
            continue;
        }
        out += ' ';
        out += structuredDataAttributeName(key);
        out += "=\"";
        out += escapeHtml(val.toString());
        out += '"';
    }
}

void StructuredRichText::addStructuredStyle(
    const QJsonObject &obj,
    StructuredRichText::Context &ctx,
    StructuredRichText::ElementRenderState &state) const
{
    /* Structured style key that changes the inherited font size */
    constexpr const char *KEY_FONT_SIZE = "fontSize";

    const auto addString = [&obj, &ctx, &state, this]
    (const char *jsonKey, const char *cssProperty)
    {
        if (obj[jsonKey].isString())
        {
            addResolvedCssDeclaration(
                cssProperty,
                obj[jsonKey].toString(),
                ctx,
                state.declarations
            );
        }
    };
    const auto addSpacing = [&obj, &ctx, &state, this]
    (const char *jsonKey, const char *cssProperty)
    {
        if (obj[jsonKey].isString())
        {
            addResolvedCssDeclaration(
                cssProperty,
                obj[jsonKey].toString(),
                ctx,
                state.declarations
            );
        }
        else if (obj[jsonKey].isDouble())
        {
            addResolvedCssDeclaration(
                cssProperty,
                QString::number(obj[jsonKey].toDouble()) + "em",
                ctx,
                state.declarations
            );
        }
    };

    addString("fontStyle", "font-style");
    addString("fontWeight", "font-weight");
    if (obj[KEY_FONT_SIZE].isString())
    {
        const QString fontSize =
            resolveCssValue(obj[KEY_FONT_SIZE].toString(), ctx);
        if (!fontSize.isEmpty())
        {
            const double pixels = cssFontSizeToPixels(
                fontSize,
                ctx.screen,
                ctx.parentFontPixelSize,
                ctx.rootFontPixelSize
            );
            if (pixels >= 0.0)
            {
                state.fontPixelSize = pixels;
                addCssDeclaration(
                    "font-size",
                    formatPixelSize(pixels) + "px",
                    state.declarations
                );
            }
            else
            {
                addCssDeclaration("font-size", fontSize, state.declarations);
            }
        }
    }

    addString("color", "color");
    addString("background", "background");
    addString("backgroundColor", "background-color");
    addString("width", "width");
    if (obj["textDecorationLine"].isArray())
    {
        QStringList lines;
        const QJsonArray values =
            obj["textDecorationLine"].toArray();
        lines.reserve(values.size());
        for (const QJsonValue &value : values)
        {
            if (value.isString())
            {
                lines.emplaceBack(value.toString());
            }
        }
        addResolvedCssDeclaration(
            "text-decoration-line",
            lines.join(' '),
            ctx,
            state.declarations
        );
    }
    else
    {
        addString("textDecorationLine", "text-decoration-line");
    }
    addString("textDecorationStyle", "text-decoration-style");
    addString("textDecorationColor", "text-decoration-color");
    addString("borderColor", "border-color");
    addString("borderStyle", "border-style");
    addString("borderRadius", "border-radius");
    addString("borderWidth", "border-width");
    addString("clipPath", "clip-path");
    addString("verticalAlign", "vertical-align");
    addString("textAlign", "text-align");
    addString("textEmphasis", "text-emphasis");
    addString("textShadow", "text-shadow");
    addSpacing("margin", "margin");
    addSpacing("marginTop", "margin-top");
    addSpacing("marginRight", "margin-right");
    addSpacing("marginBottom", "margin-bottom");
    addSpacing("marginLeft", "margin-left");
    addString("padding", "padding");
    addString("paddingTop", "padding-top");
    addString("paddingRight", "padding-right");
    addString("paddingBottom", "padding-bottom");
    addString("paddingLeft", "padding-left");
    addString("wordBreak", "word-break");
    addString("whiteSpace", "white-space");
    addString("cursor", "cursor");
    addString("listStyleType", "list-style-type");
}

void StructuredRichText::addStructuredContentHelper(
    const QString &str, QString &out) const
{
    out += escapeHtml(splitAttachedSenseMarkers(
        splitInlinePosSenseMarkers(
            splitRomanNumberedSenseMarkers(
                separatePronunciationFromFollowingText(str)
            )
        )
    )).replace('\n', "<br>");
}

void StructuredRichText::addStructuredContentHelper(
    const QString &str,
    StructuredRichText::Context &ctx,
    QString &out) const
{
    flushPendingVerticalMargin(ctx, out);

    if (ctx.linkHref.isEmpty() && ctx.titleTooltip.isEmpty())
    {
        addStructuredContentHelper(str, out);
        return;
    }

    const bool titleOnly =
        ctx.linkHref.isEmpty() && !ctx.titleTooltip.isEmpty();
    const QString href = ctx.titleTooltip.isEmpty() ?
        ctx.linkHref :
        internalLinkHref(ctx.linkHref, ctx.titleTooltip, "", "title");
    addAnchorStart(href, titleOnly ? ctx.textColor : "", out);
    addStructuredContentHelper(str, out);
    out += "</a>";
}

void StructuredRichText::addStructuredContentHelper(
    const QJsonArray &arr,
    StructuredRichText::Context &ctx,
    QString &out) const
{
    const auto hasExampleAncestor = [&ctx]()
    {
        for (const qsizetype elementIndex : ctx.elements)
        {
            const StructuredElement &element =
                ctx.selectorElements[elementIndex];
            if (isExampleContainer(
                    element.tag,
                    element.attributes.value("data-sc-class"),
                    element.attributes.value("data-sc-content")))
            {
                return true;
            }
        }
        return false;
    };

    const auto isWhitespace = [](const QJsonValue &value)
    {
        return value.isString() && value.toString().trimmed().isEmpty();
    };

    const bool insideExampleContainer = hasExampleAncestor();
    for (qsizetype i = 0; i < arr.size();)
    {
        const QJsonValue &value = arr[i];
        if (ctx.suppressExampleContent && isExampleStructuredNode(value))
        {
            ++i;
            continue;
        }

        if (!insideExampleContainer && isExampleStructuredNode(value))
        {
            QJsonArray examples;
            qsizetype exampleCount = 0;
            qsizetype next = i;
            while (next < arr.size())
            {
                const QJsonValue &candidate = arr[next];
                if (isExampleStructuredNode(candidate))
                {
                    if (exampleCount > 0)
                    {
                        examples.append(structuredExampleDivider());
                    }
                    examples.append(candidate);
                    ++exampleCount;
                    ++next;
                    continue;
                }

                if (exampleCount > 0 && isWhitespace(candidate) &&
                    next + 1 < arr.size() &&
                    isExampleStructuredNode(arr[next + 1]))
                {
                    ++next;
                    continue;
                }
                break;
            }

            if (exampleCount > 1)
            {
                QJsonObject data;
                data.insert(QStringLiteral("class"),
                            QStringLiteral("memento-example-group"));
                QJsonObject group;
                group.insert(QStringLiteral("tag"), QStringLiteral("div"));
                group.insert(QStringLiteral("data"), data);
                group.insert(QStringLiteral("content"), examples);
                addStructuredContent(group, ctx, out);
                i = next;
                continue;
            }
        }

        addStructuredContent(value, ctx, out);
        ++i;
    }
}

void StructuredRichText::addStructuredContentHelper(
    const QJsonObject &obj,
    StructuredRichText::Context &ctx,
    QString &out) const
{
    if (ctx.suppressExampleContent && isExampleStructuredNode(obj))
    {
        return;
    }

    /* Structured-content keys consumed by special element renderers */
    constexpr const char *KEY_ALT = "alt";
    constexpr const char *KEY_BACKGROUND = "background";
    constexpr const char *KEY_BORDER = "border";
    constexpr const char *KEY_BORDER_RADIUS = "borderRadius";
    constexpr const char *KEY_CONTENT = "content";
    constexpr const char *KEY_DATA = "data";
    constexpr const char *KEY_DESCRIPTION = "description";
    constexpr const char *KEY_HEIGHT = "height";
    constexpr const char *KEY_HREF = "href";
    constexpr const char *KEY_PATH = "path";
    constexpr const char *KEY_PIXELATED = "pixelated";
    constexpr const char *KEY_RENDERING = "imageRendering";
    constexpr const char *KEY_TAG = "tag";
    constexpr const char *KEY_TITLE = "title";
    constexpr const char *KEY_UNITS = "sizeUnits";
    constexpr const char *KEY_VERT_ALIGN = "verticalAlign";
    constexpr const char *KEY_WIDTH = "width";

    QString tag = obj[KEY_TAG].toString();
    if (!isSupportedStructuredTag(tag))
    {
        return;
    }
    else if (tag == "ruby")
    {
        flushPendingVerticalMargin(ctx, out);
        ctx.elements.emplaceBack(structuredElement(obj, ctx));
        addRuby(obj, ctx, out);
        ctx.elements.removeLast();
    }
    else if (obj[KEY_TAG].toString() == "a" &&
        anchorNeedsTooltipHandling(obj, ctx))
    {
        flushPendingVerticalMargin(ctx, out);
        ctx.elements.emplaceBack(structuredElement(obj, ctx));

        const QString oldLinkHref = ctx.linkHref;
        const QString oldTitleTooltip = ctx.titleTooltip;
        if (obj[KEY_HREF].isString())
        {
            ctx.linkHref = obj[KEY_HREF].toString();
        }
        if (obj[KEY_TITLE].isString())
        {
            ctx.titleTooltip = obj[KEY_TITLE].toString();
        }
        addStructuredChildren(obj[KEY_CONTENT], ctx, out);
        ctx.titleTooltip = oldTitleTooltip;
        ctx.linkHref = oldLinkHref;

        ctx.elements.removeLast();
    }
    else if (tag == "br")
    {
        flushPendingVerticalMargin(ctx, out);
        ctx.elements.emplaceBack(structuredElement(obj, ctx));
        out += '<';
        out += tag;
        if (obj[KEY_DATA].isObject())
        {
            addStructuredData(obj[KEY_DATA].toObject(), out);
        }
        out += '>';
        ctx.elements.removeLast();
    }
    else if (tag == "img")
    {
        flushPendingVerticalMargin(ctx, out);
        ctx.elements.emplaceBack(structuredElement(obj, ctx));

        QString filename = escapeHtml(ctx.basepath + obj[KEY_PATH].toString());

        const QString imageTitle = obj[KEY_TITLE].isString() ?
            obj[KEY_TITLE].toString() :
            ctx.titleTooltip;

        const bool imageTitleOnly = ctx.linkHref.isEmpty() &&
            !imageTitle.isEmpty();

        if (!imageTitle.isEmpty())
        {
            addAnchorStart(
                internalLinkHref(ctx.linkHref, imageTitle, "", "title"),
                imageTitleOnly ? ctx.textColor : "",
                out
            );
        }

        out += "<img src=\"";
        out += filename;
        out += '"';

        if (obj[KEY_DATA].isObject())
        {
            addStructuredData(obj[KEY_DATA].toObject(), out);
        }

        if (obj[KEY_ALT].isString())
        {
            out += " alt=\"";
            out += escapeHtml(obj[KEY_ALT].toString());
            out += '"';
        }

        if (obj[KEY_DESCRIPTION].isString())
        {
            out += " description=\"";
            out += escapeHtml(obj[KEY_DESCRIPTION].toString());
            out += '"';
        }

        QString units = obj[KEY_UNITS].toString("px");
        if (obj[KEY_WIDTH].isDouble())
        {
            QString cssSize = QString("%1%2")
                .arg(obj[KEY_WIDTH].toDouble())
                .arg(units);
            const double pixelSize = cssFontSizeToPixels(
                cssSize,
                ctx.screen,
                ctx.parentFontPixelSize,
                ctx.rootFontPixelSize
            );

            out += " width=\"";
            out += formatPixelSize(pixelSize);
            out += '"';
        }
        if (obj[KEY_HEIGHT].isDouble())
        {
            QString cssSize = QString("%1%2")
                .arg(obj[KEY_HEIGHT].toDouble())
                .arg(units);
            const double pixelSize = cssFontSizeToPixels(
                cssSize,
                ctx.screen,
                ctx.parentFontPixelSize,
                ctx.rootFontPixelSize
            );

            out += " height=\"";
            out += formatPixelSize(pixelSize);
            out += '"';
        }

        CssDeclarations declarations;
        addMatchingCssRules(ctx, declarations);

        if (obj[KEY_RENDERING].isString())
        {
            addCssDeclaration(
                "image-rendering",
                obj[KEY_RENDERING].toString("auto"),
                declarations
            );
        }
        else if (obj[KEY_PIXELATED].toBool(false))
        {
            addCssDeclaration("image-rendering", "pixelated", declarations);
        }

        if (obj[KEY_BACKGROUND].toBool(true))
        {
            addCssDeclaration(
                "background-color", "currentColor", declarations
            );
        }

        if (obj[KEY_VERT_ALIGN].isString())
        {
            addCssDeclaration(
                "vertical-align",
                obj[KEY_VERT_ALIGN].toString(),
                declarations
            );
        }

        if (obj[KEY_BORDER].isString())
        {
            addCssDeclaration(
                "border",
                obj[KEY_BORDER].toString(),
                declarations
            );
        }

        if (obj[KEY_BORDER_RADIUS].isString())
        {
            addCssDeclaration(
                "border-radius",
                obj[KEY_BORDER_RADIUS].toString(),
                declarations
            );
        }

        if (!declarations.isEmpty())
        {
            out += " style=\"";
            addCssDeclarations(declarations, out);
            out += '"';
        }
        out += '>';

        if (!imageTitle.isEmpty())
        {
            out += "</a>";
        }

        ctx.elements.removeLast();
    }
    else
    {
        addStructuredElement(obj, tag, ctx, out);
    }
}

void StructuredRichText::addStructuredChildren(
    const QJsonValue &val,
    StructuredRichText::Context &ctx,
    QString &out) const
{
    SiblingState siblings;
    if (ctx.needsElementChildCount)
    {
        siblings.elementCount = structuredElementChildCount(val);
    }
    ctx.siblings.emplaceBack(std::move(siblings));
    addStructuredContent(val, ctx, out);
    flushPendingVerticalMargin(ctx, out);
    ctx.siblings.removeLast();
}

qsizetype StructuredRichText::structuredElementChildCount(
    const QJsonValue &val) const
{
    if (val.isArray())
    {
        qsizetype count = 0;
        const QJsonArray arr = val.toArray();
        for (const QJsonValue &child : arr)
        {
            count += structuredElementChildCount(child);
        }
        return count;
    }
    if (!val.isObject())
    {
        return 0;
    }

    const QJsonObject obj = val.toObject();
    const QString tag = obj["tag"].toString().toLower();
    return isSupportedStructuredTag(tag) ? 1 : 0;
}

void StructuredRichText::addStructuredElement(
    const QJsonObject &obj,
    const QString &tag,
    StructuredRichText::Context &ctx,
    QString &out) const
{
    /* Structured-content key containing an element's children */
    constexpr const char *KEY_CONTENT = "content";

    ctx.elements.emplaceBack(structuredElement(obj, ctx));

    const QString oldTitleTooltip = ctx.titleTooltip;
    ElementRenderState state = elementRenderState(obj, tag, ctx);
    const bool blockElement = state.layout != ElementLayout::Inline;
    const bool collapsibleBlock = state.marginFlow == MarginFlow::Collapsible;
    const bool propagatesTrailingChildMargin =
        state.marginFlow == MarginFlow::PropagateLastChild;

    if (blockElement)
    {
        addPendingVerticalMargin(
            state.box.margins[static_cast<std::size_t>(BoxSide::Top)],
            state.fontPixelSize,
            ctx,
            ctx.siblings.back()
        );
        if (!collapsibleBlock)
        {
            flushPendingVerticalMargin(ctx, out);
        }
    }
    else
    {
        flushPendingVerticalMargin(ctx, out);
    }
    addStructuredElementStart(state, ctx, out);

    if (state.layout == ElementLayout::List)
    {
        ctx.lists.emplaceBack(StructuredList{tag, state.listMarkerType});
    }

    std::swap(ctx.parentFontPixelSize, state.fontPixelSize);
    std::swap(ctx.textColor, state.textColor);
    std::swap(ctx.titleTooltip, state.titleTooltip);
    std::swap(ctx.textDirection, state.textDirection);
    std::swap(ctx.paintedBackgroundColor, state.paintedBackgroundColor);

    SiblingState completedChildren;
    if (blockElement)
    {
        SiblingState childSiblings;
        if (ctx.needsElementChildCount)
        {
            childSiblings.elementCount =
                structuredElementChildCount(obj[KEY_CONTENT]);
        }
        if (collapsibleBlock)
        {
            mergePendingVerticalMargins(
                ctx.siblings.back(),
                childSiblings
            );
            clearPendingVerticalMargins(ctx.siblings.back());
        }

        ctx.siblings.emplaceBack(std::move(childSiblings));
        addStructuredContent(obj[KEY_CONTENT], ctx, out);
        if (!collapsibleBlock && !propagatesTrailingChildMargin)
        {
            flushPendingVerticalMargin(ctx, out);
        }
        completedChildren = ctx.siblings.takeLast();
    }
    else
    {
        addStructuredChildren(obj[KEY_CONTENT], ctx, out);
    }

    std::swap(ctx.paintedBackgroundColor, state.paintedBackgroundColor);
    std::swap(ctx.textDirection, state.textDirection);
    std::swap(ctx.titleTooltip, state.titleTooltip);
    std::swap(ctx.textColor, state.textColor);
    std::swap(ctx.parentFontPixelSize, state.fontPixelSize);
    ctx.titleTooltip = oldTitleTooltip;

    if (state.layout == ElementLayout::List)
    {
        ctx.lists.removeLast();
    }

    addStructuredElementEnd(state, ctx, out);
    if (blockElement)
    {
        if (collapsibleBlock || propagatesTrailingChildMargin)
        {
            mergePendingVerticalMargins(
                completedChildren,
                ctx.siblings.back()
            );
        }
        addPendingVerticalMargin(
            state.box.margins[static_cast<std::size_t>(BoxSide::Bottom)],
            state.fontPixelSize,
            ctx,
            ctx.siblings.back()
        );
    }
    ctx.elements.removeLast();
}

StructuredRichText::ElementRenderState
StructuredRichText::elementRenderState(
    const QJsonObject &obj,
    const QString &tag,
    StructuredRichText::Context &ctx) const
{
    ElementRenderState state;
    state.tag = tag;
    state.outputTag = tag;
    state.fontPixelSize = ctx.parentFontPixelSize;
    state.textColor = ctx.textColor;
    state.titleTooltip = ctx.titleTooltip;
    state.textDirection = ctx.textDirection;
    state.paintedBackgroundColor = ctx.paintedBackgroundColor;
    state.textOnlyContent = obj["content"].isString();
    if (obj["lang"].isString())
    {
        state.textDirection =
            QLocale(obj["lang"].toString()).textDirection();
    }

    addElementAttributes(obj, state);
    resolveElementStyles(obj, ctx, state);
    resolveElementLayout(ctx, state);
    applyElementCompatibility(ctx, state);

    return state;
}

void StructuredRichText::addElementAttributes(
    const QJsonObject &obj,
    StructuredRichText::ElementRenderState &state) const
{
    /* Structured-content keys emitted as HTML element attributes */
    constexpr const char *KEY_COLSPAN = "colSpan";
    constexpr const char *KEY_DATA = "data";
    constexpr const char *KEY_HREF = "href";
    constexpr const char *KEY_LANG = "lang";
    constexpr const char *KEY_ROWSPAN = "rowSpan";
    constexpr const char *KEY_TITLE = "title";

    if (obj[KEY_HREF].isString())
    {
        state.attributes += " href=\"";
        state.attributes += escapeHtml(obj[KEY_HREF].toString());
        state.attributes += '"';
    }

    if (obj[KEY_DATA].isObject())
    {
        addStructuredData(obj[KEY_DATA].toObject(), state.attributes);
    }

    if (obj[KEY_COLSPAN].isDouble())
    {
        state.attributes += " colspan=\"";
        state.attributes += QString::number(
            static_cast<int>(obj[KEY_COLSPAN].toDouble())
        );
        state.attributes += '"';
    }

    if (obj[KEY_ROWSPAN].isDouble())
    {
        state.attributes += " rowspan=\"";
        state.attributes += QString::number(
            static_cast<int>(obj[KEY_ROWSPAN].toDouble())
        );
        state.attributes += '"';
    }

    if (obj[KEY_TITLE].isString())
    {
        state.titleTooltip = obj[KEY_TITLE].toString();
        state.attributes += " title=\"";
        state.attributes += escapeHtml(obj[KEY_TITLE].toString());
        state.attributes += '"';
    }

    if (obj[KEY_LANG].isString())
    {
        state.attributes += " lang=\"";
        state.attributes += escapeHtml(obj[KEY_LANG].toString());
        state.attributes += '"';
    }
}

void StructuredRichText::resolveElementStyles(
    const QJsonObject &obj,
    StructuredRichText::Context &ctx,
    StructuredRichText::ElementRenderState &state) const
{
    /* Structured-content key containing direct style declarations */
    constexpr const char *KEY_STYLE = "style";

    if (state.tag == "table")
    {
        addCssDeclaration("border", "1px solid", state.declarations);
        addCssDeclaration(
            "border-collapse", "collapse", state.declarations
        );
    }
    else if (state.tag == "th" || state.tag == "td")
    {
        addCssDeclaration("border", "1px solid", state.declarations);
        addCssDeclaration(
            "border-collapse", "collapse", state.declarations
        );
        addCssDeclaration("padding", "5px", state.declarations);
    }

    addMatchingCssRules(
        ctx,
        state.declarations,
        &state.beforeContent,
        &state.afterContent
    );

    if (obj[KEY_STYLE].isObject())
    {
        addStructuredStyle(
            obj[KEY_STYLE].toObject(),
            ctx,
            state
        );
    }
    if (state.declarations.contains("font-size"))
    {
        const double pixelSize = cssFontSizeToPixels(
            state.declarations["font-size"],
            ctx.screen,
            ctx.parentFontPixelSize,
            ctx.rootFontPixelSize
        );
        if (pixelSize >= 0.0)
        {
            state.fontPixelSize = pixelSize;
        }
    }
    if (state.declarations.contains("color"))
    {
        state.textColor = state.declarations["color"];
    }
}

void StructuredRichText::resolveElementLayout(
    StructuredRichText::Context &ctx,
    StructuredRichText::ElementRenderState &state) const
{
    const StructuredElement &currentElement =
        ctx.selectorElements[ctx.elements.back()];
    const bool exampleContainer = isExampleContainer(
        state.tag,
        currentElement.attributes.value("data-sc-class"),
        currentElement.attributes.value("data-sc-content")
    );
    const QString className =
        currentElement.attributes.value("data-sc-class");
    const bool usageGroupContainer = isUsageGroupContainer(
        state.tag, className
    );
    if (className == QStringLiteral("memento-translation"))
    {
        const QColor canvasColor = parseCssColor(ctx.paintedBackgroundColor);
        const QString translationColor =
            canvasColor.isValid() && canvasColor.lightness() < 128 ?
                QStringLiteral("#B8B8B8") : QStringLiteral("#666666");
        state.declarations.insert("color", translationColor);
        state.textColor = translationColor;
    }
    else if (className == QStringLiteral("memento-grammar-label"))
    {
        const QColor canvasColor = parseCssColor(ctx.paintedBackgroundColor);
        const QString grammarColor =
            canvasColor.isValid() && canvasColor.lightness() < 128 ?
                QStringLiteral("#8FD3E8") : QStringLiteral("#176B7A");
        state.declarations.insert("color", grammarColor);
        state.declarations.insert("font-style", QStringLiteral("italic"));
        state.textColor = grammarColor;
    }
    else if (className == QStringLiteral("memento-divider"))
    {
        const QColor canvasColor = parseCssColor(ctx.paintedBackgroundColor);
        const bool darkCanvas = canvasColor.isValid() &&
            canvasColor.lightness() < 128;
        const QString dividerColor =
            darkCanvas ? QStringLiteral("#5A5A5A") : QStringLiteral("#C8C8C8");
        state.declarations.insert("border-color", dividerColor);
        state.declarations.insert(
            "background-color",
            darkCanvas ? QStringLiteral("#3B3B3B") : QStringLiteral("#E6E6E6")
        );
    }

    bool hasGlossaryBoxAncestor = false;
    for (qsizetype i = ctx.elements.size() - 1; i > 0; --i)
    {
        const StructuredElement &ancestor =
            ctx.selectorElements[ctx.elements[i - 1]];
        const QString ancestorClass =
            ancestor.attributes.value("data-sc-class");
        if (isExampleContainer(
                ancestor.tag,
                ancestorClass,
                ancestor.attributes.value("data-sc-content")) ||
            isUsageGroupContainer(ancestor.tag, ancestorClass))
        {
            hasGlossaryBoxAncestor = true;
            break;
        }
    }
    if ((exampleContainer || usageGroupContainer) &&
        !hasGlossaryBoxAncestor)
    {
        const QColor canvasColor = parseCssColor(ctx.paintedBackgroundColor);
        const bool darkCanvas = canvasColor.isValid() &&
            canvasColor.lightness() < 128;
        state.declarations.insert(
            "background-color",
            darkCanvas ? QStringLiteral("#3B3B3B") : QStringLiteral("#E6E6E6")
        );
        state.declarations.insert(
            "border-left", QStringLiteral("2px solid #FFFFFF")
        );
        state.declarations.insert("padding", QStringLiteral("5px 8px"));
        state.declarations.insert("margin-top", QStringLiteral("4px"));
        state.declarations.insert("margin-bottom", QStringLiteral("4px"));
        state.declarations.insert("width", QStringLiteral("fit-content"));
    }

    const QString display =
        state.declarations.take("display").trimmed().toLower();
    const QString listType =
        state.declarations.value("list-style-type");
    state.listMarkerType = normalizeListMarker(listType.isEmpty() ?
        defaultListMarker(state.tag) :
        listType
    );
    const bool isList = state.tag == "ul" || state.tag == "ol";
    const bool isListItem = state.tag == "li" && !ctx.lists.isEmpty();
    const bool inlineListItem =
        isListItem && display.compare("inline", Qt::CaseInsensitive) == 0;
    if (isListItem)
    {
        StructuredList &list = ctx.lists.back();
        ++list.item;
        state.listMarker = listMarker(list, listType);
    }

    const auto isBoxDeclaration = [] (const QString &property) -> bool
    {
        return property == "background-color" ||
            property.startsWith("border") ||
            property.startsWith("padding");
    };
    const bool styledBlock =
        (
            state.tag == "div" ||
            state.tag == "p" ||
            state.tag == "blockquote" ||
            state.tag == "details" ||
            state.tag == "summary" ||
            (exampleContainer && state.tag == "span")
        ) &&
        std::any_of(
            state.declarations.keyBegin(),
            state.declarations.keyEnd(),
            isBoxDeclaration
        );
    state.paddedSpan =
        state.tag == "span" &&
        (
            ctx.selectorElements[ctx.elements.back()].attributes.value(
                "data-sc-class"
            ) == "tag" ||
            ctx.selectorElements[ctx.elements.back()].attributes.value(
                "data-sc-content"
            ) == "forms-label"
        );

    if (isList)
    {
        state.layout = ElementLayout::List;
        state.outputTag = "div";
    }
    else if (inlineListItem)
    {
        state.layout = ElementLayout::Inline;
        state.outputTag = "span";
    }
    else if (isListItem && !state.listMarker.isEmpty())
    {
        state.layout = ElementLayout::MarkedListItem;
        state.outputTag = "table";
        state.widthPolicy = WidthPolicy::Fill;
    }
    else if (isListItem)
    {
        state.layout = ElementLayout::MarkerlessListItem;
        state.outputTag = "div";
    }
    else if (state.tag == "details" || styledBlock)
    {
        state.layout = ElementLayout::Box;
        state.outputTag = "table";
        state.widthPolicy = WidthPolicy::Fill;
    }
    else if (state.tag == "div" || state.tag == "summary")
    {
        state.layout = ElementLayout::Block;
        state.outputTag = "div";
    }

    if (isList || isListItem)
    {
        state.declarations.remove("list-style-type");
    }
    if (state.listMarkerType == "none")
    {
        state.listMarkerType.clear();
    }

    const QString width = state.declarations.value("width");
    if (state.layout == ElementLayout::Box &&
        (width.compare("fit-content", Qt::CaseInsensitive) == 0 ||
         width.compare("max-content", Qt::CaseInsensitive) == 0))
    {
        state.widthPolicy = WidthPolicy::FitContent;
        state.declarations.remove("width");
    }
    else if (state.layout == ElementLayout::Box && !width.isEmpty())
    {
        state.widthPolicy = WidthPolicy::Explicit;
    }
}

void StructuredRichText::applyElementCompatibility(
    const StructuredRichText::Context &ctx,
    StructuredRichText::ElementRenderState &state) const
{
    if (state.tag == "span")
    {
        const QString left = state.declarations.take("margin-left");
        const QString right = state.declarations.take("margin-right");
        if (state.textDirection == Qt::RightToLeft)
        {
            state.inlineSpacingBefore = right;
            state.inlineSpacingAfter = left;
        }
        else
        {
            state.inlineSpacingBefore = left;
            state.inlineSpacingAfter = right;
        }
    }

    if (state.layout == ElementLayout::Inline)
    {
        state.marginFlow = MarginFlow::Inline;
        return;
    }

    resolveBoxStyle(ctx, state);
    state.marginFlow = marginFlow(state);
}

void StructuredRichText::resolveBoxStyle(
    const StructuredRichText::Context &ctx,
    StructuredRichText::ElementRenderState &state) const
{
    const bool hasPadding =
        state.declarations.contains("padding") ||
        std::any_of(
            PADDING_PROPERTIES.begin(),
            PADDING_PROPERTIES.end(),
            [&state] (const char *property)
            {
                return state.declarations.contains(property);
            }
        );
    if (hasPadding)
    {
        const QString paddingShorthand =
            state.declarations.take("padding");
        for (std::size_t side = 0; side < BOX_SIDE_COUNT; ++side)
        {
            QString value =
                state.declarations.take(PADDING_PROPERTIES[side]);
            if (value.isEmpty())
            {
                value = cssBoxSideValue(
                    paddingShorthand,
                    static_cast<qsizetype>(side)
                );
            }
            const double pixels = cssFontSizeToPixels(
                value,
                ctx.screen,
                state.fontPixelSize,
                ctx.rootFontPixelSize
            );
            if (pixels > MINIMUM_VISIBLE_PIXELS)
            {
                state.box.padding[side] = pixels;
            }
        }
    }

    for (std::size_t side = 0; side < BOX_SIDE_COUNT; ++side)
    {
        state.box.margins[side] =
            state.declarations.take(MARGIN_PROPERTIES[side]);
    }

    if (state.layout == ElementLayout::Block &&
        state.textOnlyContent)
    {
        QString &left = state.box.margins[LEFT_SIDE];
        QString &right = state.box.margins[RIGHT_SIDE];
        if (state.textDirection == Qt::RightToLeft)
        {
            state.contentInlineSpacingBefore = std::move(right);
            state.contentInlineSpacingAfter = std::move(left);
        }
        else
        {
            state.contentInlineSpacingBefore = std::move(left);
            state.contentInlineSpacingAfter = std::move(right);
        }
    }

    if (state.declarations.contains("background-color"))
    {
        const QString background =
            state.declarations.take("background-color");
        QColor resolvedBackground = parseCssColor(background);
        if (resolvedBackground.isValid() &&
            resolvedBackground.alpha() < OPAQUE_ALPHA)
        {
            const QColor parent =
                parseCssColor(ctx.paintedBackgroundColor);
            if (parent.isValid())
            {
                resolvedBackground =
                    compositeColor(resolvedBackground, parent);
            }
        }
        if (resolvedBackground.isValid())
        {
            state.box.backgroundColor = resolvedBackground.name(
                resolvedBackground.alpha() == OPAQUE_ALPHA ?
                    QColor::HexRgb :
                    QColor::HexArgb
            );
            state.paintedBackgroundColor = state.box.backgroundColor;
        }
        else
        {
            state.box.backgroundColor = background;
        }
    }

    const bool hasBorder = std::any_of(
        state.declarations.keyBegin(),
        state.declarations.keyEnd(),
        [] (const QString &property)
        {
            return property == "border" ||
                property.startsWith("border-");
        }
    );
    if (!hasBorder)
    {
        state.declarations.remove("border-radius");
        state.box.enabled =
            state.layout == ElementLayout::Box ||
            !state.box.backgroundColor.isEmpty() ||
            hasPadding;
        const bool hasHorizontalMargins =
            !isZeroSpacing(state.box.margins[LEFT_SIDE]) ||
            !isZeroSpacing(state.box.margins[RIGHT_SIDE]);
        if (state.layout == ElementLayout::Block &&
            !state.textOnlyContent &&
            hasHorizontalMargins)
        {
            state.layout = ElementLayout::Box;
            state.outputTag = "table";
            state.widthPolicy = WidthPolicy::Fill;
            state.box.enabled = true;
        }
        return;
    }

    const ParsedBorder commonBorder =
        parseBorderShorthand(state.declarations.take("border"));
    const QString borderWidths =
        state.declarations.take("border-width");
    const QString borderStyles =
        state.declarations.take("border-style");
    const QString borderColors =
        state.declarations.take("border-color");

    std::size_t paintedBorderCount = 0;
    QString sharedBorderColor;
    bool sharedBorderColorMatches = true;
    for (std::size_t side = 0; side < BOX_SIDE_COUNT; ++side)
    {
        ParsedBorder border = commonBorder;
        const QString sideName = SIDE_NAMES[side];
        const QString boxWidth = cssBoxSideValue(
            borderWidths,
            static_cast<qsizetype>(side)
        );
        const QString boxStyle = cssBoxSideValue(
            borderStyles,
            static_cast<qsizetype>(side)
        );
        const QString boxColor = cssBoxSideValue(
            borderColors,
            static_cast<qsizetype>(side)
        );
        if (!boxWidth.isEmpty())
        {
            border.width = boxWidth;
        }
        if (!boxStyle.isEmpty())
        {
            border.style = boxStyle;
        }
        if (!boxColor.isEmpty())
        {
            border.color = boxColor;
        }

        const ParsedBorder sideBorder = parseBorderShorthand(
            state.declarations.take("border-" + sideName)
        );
        if (!sideBorder.width.isEmpty())
        {
            border.width = sideBorder.width;
        }
        if (!sideBorder.style.isEmpty())
        {
            border.style = sideBorder.style;
        }
        if (!sideBorder.color.isEmpty())
        {
            border.color = sideBorder.color;
        }

        const QString widthProperty = "border-" + sideName + "-width";
        const QString styleProperty = "border-" + sideName + "-style";
        const QString colorProperty = "border-" + sideName + "-color";
        if (state.declarations.contains(widthProperty))
        {
            border.width = state.declarations.take(widthProperty);
        }
        if (state.declarations.contains(styleProperty))
        {
            border.style = state.declarations.take(styleProperty);
        }
        if (state.declarations.contains(colorProperty))
        {
            border.color = state.declarations.take(colorProperty);
        }

        if (border.width.isEmpty() &&
            border.style.compare("solid", Qt::CaseInsensitive) == 0)
        {
            border.width =
                formatPixelSize(DEFAULT_BORDER_WIDTH_PIXELS) + "px";
        }
        double widthPixels = cssFontSizeToPixels(
            border.width,
            ctx.screen,
            state.fontPixelSize,
            ctx.rootFontPixelSize
        );
        if (border.width.compare("thin", Qt::CaseInsensitive) == 0)
        {
            widthPixels = THIN_BORDER_WIDTH_PIXELS;
        }
        else if (border.width.compare("medium", Qt::CaseInsensitive) == 0)
        {
            widthPixels = DEFAULT_BORDER_WIDTH_PIXELS;
        }
        else if (border.width.compare("thick", Qt::CaseInsensitive) == 0)
        {
            widthPixels = THICK_BORDER_WIDTH_PIXELS;
        }

        if (border.color.isEmpty() ||
            border.color.compare("currentcolor", Qt::CaseInsensitive) == 0)
        {
            border.color = state.textColor;
        }

        BorderSide &resolved = state.box.borders[side];
        resolved.color = border.color;
        resolved.style = border.style;
        resolved.widthPixels = std::max(0.0, widthPixels);
        const QColor color = parseCssColor(resolved.color);
        resolved.painted =
            resolved.style.compare("solid", Qt::CaseInsensitive) == 0 &&
            resolved.widthPixels > MINIMUM_VISIBLE_PIXELS &&
            color.isValid();
        if (resolved.painted)
        {
            ++paintedBorderCount;
            if (sharedBorderColor.isEmpty())
            {
                sharedBorderColor = resolved.color;
            }
            else if (sharedBorderColor != resolved.color)
            {
                sharedBorderColorMatches = false;
            }
        }
        else if (!resolved.style.isEmpty() &&
                 resolved.style.compare("none", Qt::CaseInsensitive) != 0 &&
                 resolved.widthPixels > MINIMUM_VISIBLE_PIXELS)
        {
            state.cellDeclarations["border-" + sideName] =
                formatPixelSize(resolved.widthPixels) + "px " +
                resolved.style + ' ' + resolved.color;
        }
    }
    state.declarations.remove("border-radius");

    state.box.compactBorderFrame =
        paintedBorderCount > 0 && sharedBorderColorMatches;
    state.box.enabled =
        state.layout == ElementLayout::Box ||
        paintedBorderCount > 0 ||
        !state.box.backgroundColor.isEmpty() ||
        std::any_of(
            state.box.padding.begin(),
            state.box.padding.end(),
            [] (double value)
            {
                return value > MINIMUM_VISIBLE_PIXELS;
            }
        );

    const bool hasHorizontalMargins =
        !isZeroSpacing(state.box.margins[LEFT_SIDE]) ||
        !isZeroSpacing(state.box.margins[RIGHT_SIDE]);
    if (state.layout == ElementLayout::Block &&
        !state.textOnlyContent &&
        hasHorizontalMargins)
    {
        state.layout = ElementLayout::Box;
        state.outputTag = "table";
        state.widthPolicy = WidthPolicy::Fill;
        state.box.enabled = true;
    }
}

StructuredRichText::MarginFlow StructuredRichText::marginFlow(
    const StructuredRichText::ElementRenderState &state) const
{
    if (state.layout == ElementLayout::Inline)
    {
        return MarginFlow::Inline;
    }

    const bool hasVerticalPadding =
        state.box.padding[TOP_SIDE] > MINIMUM_VISIBLE_PIXELS ||
        state.box.padding[BOTTOM_SIDE] > MINIMUM_VISIBLE_PIXELS;
    const bool hasPaintedBorder = std::any_of(
        state.box.borders.begin(),
        state.box.borders.end(),
        [] (const BorderSide &border)
        {
            return border.painted;
        }
    );
    if (state.box.enabled &&
        (!state.box.backgroundColor.isEmpty() ||
         hasVerticalPadding ||
         hasPaintedBorder))
    {
        return MarginFlow::Contained;
    }
    if (state.layout == ElementLayout::MarkedListItem)
    {
        return MarginFlow::PropagateLastChild;
    }
    return MarginFlow::Collapsible;
}

void StructuredRichText::addStructuredElementStart(
    const StructuredRichText::ElementRenderState &state,
    const StructuredRichText::Context &ctx,
    QString &out) const
{
    addInlineSpacer(
        state.inlineSpacingBefore,
        state.fontPixelSize,
        ctx,
        out
    );

    if (state.layout == ElementLayout::Box)
    {
        addBoxStart(state, ctx, out);
    }
    else
    {
        out += '<';
        out += state.outputTag;
        if (state.layout == ElementLayout::MarkedListItem)
        {
            out += " cellspacing=\"0\""
                " width=\"100%\"";
        }
        out += state.attributes;
        if (!state.declarations.isEmpty())
        {
            out += " style=\"";
            addCssDeclarations(state.declarations, out);
            out += '"';
        }
        out += '>';
    }

    if (state.layout == ElementLayout::MarkerlessListItem)
    {
        addVerticalPixelSpacer(state.box.padding[TOP_SIDE], out);
        addSelectionSentinel(LIST_ITEM_SENTINEL, out);
    }

    if (state.layout == ElementLayout::MarkedListItem)
    {
        /* Marker-to-content gap relative to the item's font size */
        constexpr double RELATIVE_INDENT_FACTOR = 0.35;
        const char *paddingProperty =
            state.textDirection == Qt::RightToLeft ?
                "padding-left" :
                "padding-right";

        out += "<tr><td style=\"vertical-align: top; white-space: nowrap; ";
        out += paddingProperty;
        out += ": ";
        out += formatPixelSize(
            state.fontPixelSize * RELATIVE_INDENT_FACTOR
        );
        out += "px;\">";
        addSelectionSentinel(LIST_ITEM_SENTINEL, out);
        addSelectionSentinel(LIST_MARKER_START_SENTINEL, out);
        out += escapeHtml(state.listMarker);
        addSelectionSentinel(LIST_MARKER_END_SENTINEL, out);
        out += "</td><td width=\"100%\" style=\"vertical-align: top;\">";
    }

    if (state.paddedSpan)
    {
        out += "&nbsp;";
    }

    addInlineSpacer(
        state.contentInlineSpacingBefore,
        state.fontPixelSize,
        ctx,
        out
    );

    if (!state.beforeContent.isEmpty())
    {
        out += escapeHtml(state.beforeContent);
    }
}

void StructuredRichText::addStructuredElementEnd(
    const StructuredRichText::ElementRenderState &state,
    const StructuredRichText::Context &ctx,
    QString &out) const
{
    if (!state.afterContent.isEmpty())
    {
        out += escapeHtml(state.afterContent);
    }
    if (state.paddedSpan)
    {
        out += "&nbsp;";
    }
    addInlineSpacer(
        state.contentInlineSpacingAfter,
        state.fontPixelSize,
        ctx,
        out
    );
    if (state.layout == ElementLayout::MarkerlessListItem)
    {
        addVerticalPixelSpacer(state.box.padding[BOTTOM_SIDE], out);
    }
    if (state.layout == ElementLayout::Box)
    {
        addBoxEnd(state, ctx, out);
    }
    else
    {
        if (state.layout == ElementLayout::MarkedListItem)
        {
            out += "</td></tr>";
        }

        out += "</";
        out += state.outputTag;
        out += '>';
    }

    addInlineSpacer(
        state.inlineSpacingAfter,
        state.fontPixelSize,
        ctx,
        out
    );

    if (state.paddedSpan)
    {
        out += ' ';
    }
}

void StructuredRichText::addSelectionSentinel(
    char16_t sentinel, QString &out) const
{
    out += QChar(sentinel);
}

void StructuredRichText::addBoxStart(
    const StructuredRichText::ElementRenderState &state,
    const StructuredRichText::Context &ctx,
    QString &out) const
{
    const auto marginPixels =
        [&state, &ctx, this] (std::size_t side) -> double
    {
        return std::max(
            0.0,
            cssFontSizeToPixels(
                state.box.margins[side],
                ctx.screen,
                state.fontPixelSize,
                ctx.rootFontPixelSize
            )
        );
    };
    const double leftMargin = marginPixels(LEFT_SIDE);
    const double rightMargin = marginPixels(RIGHT_SIDE);
    const bool hasMarginGrid =
        leftMargin > MINIMUM_VISIBLE_PIXELS ||
        rightMargin > MINIMUM_VISIBLE_PIXELS;

    if (hasMarginGrid)
    {
        out += "<table cellspacing=\"0\""
            " width=\"100%\"><tr>";
        if (leftMargin > MINIMUM_VISIBLE_PIXELS)
        {
            out += "<td width=\"";
            out += formatPixelSize(leftMargin);
            out += "\"></td>";
        }
        out += "<td width=\"100%\" style=\"vertical-align: top;\">";
    }
    if (!state.box.enabled)
    {
        return;
    }

    const auto addTableStart =
        [&state, &out, this] (
            const QString &background,
            bool outerTable)
    {
        out += "<table cellspacing=\"0\"";
        if (state.widthPolicy == WidthPolicy::Fill)
        {
            out += " width=\"100%\"";
        }
        if (!background.isEmpty())
        {
            out += " bgcolor=\"";
            out += background;
            out += '"';
        }
        if (outerTable)
        {
            out += state.attributes;
            if (!state.declarations.isEmpty())
            {
                out += " style=\"";
                addCssDeclarations(state.declarations, out);
                out += '"';
            }
        }
        out += "><tr><td style=\"vertical-align: top;";
    };

    const auto addBorderPadding =
        [&state, &out, this] (std::size_t side)
    {
        const BorderSide &border = state.box.borders[side];
        if (!border.painted)
        {
            return;
        }
        out += "padding-";
        out += SIDE_NAMES[side];
        out += ": ";
        out += formatPixelSize(border.widthPixels);
        out += "px;";
    };

    const auto closeOpeningCell = [&out] ()
    {
        out += "\">";
    };

    const auto firstPaintedBorder =
        std::find_if(
            state.box.borders.begin(),
            state.box.borders.end(),
            [] (const BorderSide &border)
            {
                return border.painted;
            }
        );
    const bool hasPaintedBorder =
        firstPaintedBorder != state.box.borders.end();
    const QString contentBackground =
        state.paintedBackgroundColor;

    bool outerTable = true;
    if (hasPaintedBorder && state.box.compactBorderFrame)
    {
        addTableStart(firstPaintedBorder->color, outerTable);
        for (std::size_t side = 0; side < BOX_SIDE_COUNT; ++side)
        {
            addBorderPadding(side);
        }
        closeOpeningCell();
        outerTable = false;
    }
    else if (hasPaintedBorder)
    {
        /* Stable nesting order for independently colored border layers */
        constexpr std::array<std::size_t, BOX_SIDE_COUNT> BORDER_LAYER_ORDER = {
            TOP_SIDE,
            RIGHT_SIDE,
            BOTTOM_SIDE,
            LEFT_SIDE
        };
        for (const std::size_t side : BORDER_LAYER_ORDER)
        {
            const BorderSide &border = state.box.borders[side];
            if (!border.painted)
            {
                continue;
            }
            addTableStart(border.color, outerTable);
            addBorderPadding(side);
            closeOpeningCell();
            outerTable = false;
        }
    }

    addTableStart(contentBackground, outerTable);
    /* Cell-padding emission order used by the Qt HTML table workaround */
    constexpr std::array<std::size_t, BOX_SIDE_COUNT> PADDING_ORDER = {
        BOTTOM_SIDE,
        LEFT_SIDE,
        RIGHT_SIDE,
        TOP_SIDE
    };
    for (const std::size_t side : PADDING_ORDER)
    {
        if (state.box.padding[side] <= MINIMUM_VISIBLE_PIXELS)
        {
            continue;
        }
        out += "padding-";
        out += SIDE_NAMES[side];
        out += ": ";
        out += formatPixelSize(state.box.padding[side]);
        out += "px;";
    }
    addCssDeclarations(state.cellDeclarations, out);
    closeOpeningCell();
}

void StructuredRichText::addBoxEnd(
    const StructuredRichText::ElementRenderState &state,
    const StructuredRichText::Context &ctx,
    QString &out) const
{
    if (state.box.enabled)
    {
        out += "</td></tr></table>";
        const std::size_t paintedBorderCount =
            static_cast<std::size_t>(std::count_if(
                state.box.borders.begin(),
                state.box.borders.end(),
                [] (const BorderSide &border)
                {
                    return border.painted;
                }
            ));
        const std::size_t borderLayerCount =
            state.box.compactBorderFrame && paintedBorderCount > 0 ?
                1 :
                paintedBorderCount;
        for (std::size_t layer = 0;
             layer < borderLayerCount;
             ++layer)
        {
            out += "</td></tr></table>";
        }
    }

    const auto marginPixels =
    [&state, &ctx, this] (std::size_t side) -> double
    {
        return std::max(
            0.0,
            cssFontSizeToPixels(
                state.box.margins[side],
                ctx.screen,
                state.fontPixelSize,
                ctx.rootFontPixelSize
            )
        );
    };
    const double leftMargin = marginPixels(LEFT_SIDE);
    const double rightMargin = marginPixels(RIGHT_SIDE);
    if (leftMargin > MINIMUM_VISIBLE_PIXELS ||
        rightMargin > MINIMUM_VISIBLE_PIXELS)
    {
        out += "</td>";
        if (rightMargin > MINIMUM_VISIBLE_PIXELS)
        {
            out += "<td width=\"";
            out += formatPixelSize(rightMargin);
            out += "\"></td>";
        }
        out += "</tr></table>";
    }
}

void StructuredRichText::addInlineSpacer(
    const QString &spacing,
    double fontPixelSize,
    const StructuredRichText::Context &ctx,
    QString &out) const
{
    const double pixelSize = cssFontSizeToPixels(
        spacing,
        ctx.screen,
        fontPixelSize,
        ctx.rootFontPixelSize
    );
    if (pixelSize <= 0.0)
    {
        return;
    }

    out += "<span style=\"font-size: 1px;letter-spacing: ";
    out += formatPixelSize(pixelSize);
    out += "px;\">&nbsp;</span>";
}

void StructuredRichText::addVerticalSpacer(
    const QString &spacing,
    double fontPixelSize,
    const StructuredRichText::Context &ctx,
    QString &out) const
{
    const QString value = spacing.trimmed();
    if (isZeroSpacing(value))
    {
        return;
    }

    const double pixelSize = cssFontSizeToPixels(
        value,
        ctx.screen,
        fontPixelSize,
        ctx.rootFontPixelSize
    );
    if (pixelSize < 0.0)
    {
        return;
    }

    addVerticalPixelSpacer(pixelSize, out);
}

void StructuredRichText::addVerticalPixelSpacer(
    double pixelSize, QString &out) const
{
    /* Threshold below which a spacer is omitted from generated HTML */
    constexpr double MINIMUM_VISIBLE_SPACING_PIXELS = 0.001;

    /* Line box height used to represent an arbitrary vertical spacer */
    constexpr double SPACER_LINE_HEIGHT_PIXELS = 1.0;

    if (std::abs(pixelSize) < MINIMUM_VISIBLE_SPACING_PIXELS)
    {
        return;
    }

    out += "<div style=\"font-size: ";
    out += formatPixelSize(SPACER_LINE_HEIGHT_PIXELS);
    out += "px;line-height: ";
    out += formatPixelSize(SPACER_LINE_HEIGHT_PIXELS);
    out += "px;margin-top: ";
    out += formatPixelSize(pixelSize - SPACER_LINE_HEIGHT_PIXELS);
    out += "px;\">&nbsp;</div>";
}

void StructuredRichText::addPendingVerticalMargin(
    const QString &spacing,
    double fontPixelSize,
    const StructuredRichText::Context &ctx,
    StructuredRichText::SiblingState &siblings) const
{
    /* CSS length syntax accepted by the margin-collapsing compatibility path */
    static const QRegularExpression CSS_LENGTH{
        R"(^[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:px|%|em|rem|ex|rex|ch|rch|)"
        R"(cap|rcap|ic|ric|lh|rlh|vw|svw|lvw|dvw|vi|svi|lvi|dvi|vh|)"
        R"(svh|lvh|dvh|vb|svb|lvb|dvb|vmin|svmin|lvmin|dvmin|vmax|)"
        R"(svmax|lvmax|dvmax|pt|pc|in|cm|mm|q)$)",
        QRegularExpression::CaseInsensitiveOption
    };

    const QString value = spacing.trimmed();
    if (isZeroSpacing(value) || !CSS_LENGTH.match(value).hasMatch())
    {
        return;
    }

    const double pixelSize = cssFontSizeToPixels(
        value,
        ctx.screen,
        fontPixelSize,
        ctx.rootFontPixelSize
    );
    if (pixelSize > 0.0)
    {
        siblings.pendingPositiveMarginPixels = std::max(
            siblings.pendingPositiveMarginPixels,
            pixelSize
        );
    }
    else if (pixelSize < 0.0)
    {
        siblings.pendingNegativeMarginPixels = std::min(
            siblings.pendingNegativeMarginPixels,
            pixelSize
        );
    }
}

void StructuredRichText::mergePendingVerticalMargins(
    const StructuredRichText::SiblingState &source,
    StructuredRichText::SiblingState &destination) const
{
    destination.pendingPositiveMarginPixels = std::max(
        destination.pendingPositiveMarginPixels,
        source.pendingPositiveMarginPixels
    );
    destination.pendingNegativeMarginPixels = std::min(
        destination.pendingNegativeMarginPixels,
        source.pendingNegativeMarginPixels
    );
}

void StructuredRichText::flushPendingVerticalMargin(
    StructuredRichText::Context &ctx,
    QString &out) const
{
    if (ctx.siblings.isEmpty())
    {
        return;
    }

    SiblingState &siblings = ctx.siblings.back();
    const double margin =
        siblings.pendingPositiveMarginPixels +
        siblings.pendingNegativeMarginPixels;
    clearPendingVerticalMargins(siblings);
    addVerticalPixelSpacer(margin, out);
}

void StructuredRichText::clearPendingVerticalMargins(
    StructuredRichText::SiblingState &siblings) const
{
    siblings.pendingPositiveMarginPixels = 0.0;
    siblings.pendingNegativeMarginPixels = 0.0;
}

QString StructuredRichText::cssBoxSideValue(
    const QString &value, qsizetype side) const
{
    /* Side count and indexes used to expand CSS box shorthands */
    constexpr qsizetype BOX_SIDE_COUNT = 4;
    constexpr qsizetype TOP_SIDE = 0;
    constexpr qsizetype RIGHT_SIDE = 1;
    constexpr qsizetype BOTTOM_SIDE = 2;

    if (side < 0 || side >= BOX_SIDE_COUNT)
    {
        return "";
    }

    QStringList values;
    QString current;
    int parenthesisDepth = 0;
    for (const QChar ch : value.trimmed())
    {
        if (ch == '(')
        {
            ++parenthesisDepth;
        }
        else if (ch == ')')
        {
            --parenthesisDepth;
        }

        if (ch.isSpace() && parenthesisDepth == 0)
        {
            if (!current.isEmpty())
            {
                values.emplaceBack(current);
                current.clear();
            }
        }
        else
        {
            current += ch;
        }
    }
    if (!current.isEmpty())
    {
        values.emplaceBack(current);
    }

    switch (values.size())
    {
    case 1:
        return values[0];

    case 2:
        return side == TOP_SIDE || side == BOTTOM_SIDE ?
            values[0] :
            values[1];

    case 3:
        if (side == TOP_SIDE)
        {
            return values[0];
        }
        if (side == RIGHT_SIDE)
        {
            return values[1];
        }
        return side == BOTTOM_SIDE ? values[2] : values[1];

    case BOX_SIDE_COUNT:
        return values[side];

    default:
        return "";
    }
}

bool StructuredRichText::isZeroSpacing(const QString &spacing) const
{
    /* CSS numeric zero with an optional unit */
    static const QRegularExpression ZERO_SPACING{
        R"(^[+-]?(?:0+(?:\.0*)?|\.0+)(?:[a-z%]+)?$)",
        QRegularExpression::CaseInsensitiveOption
    };

    const QString value = spacing.trimmed();
    return value.isEmpty() || ZERO_SPACING.match(value).hasMatch();
}

void StructuredRichText::addStructuredContent(
    const QJsonValue &val,
    StructuredRichText::Context &ctx,
    QString &out) const
{
    switch (val.type())
    {
    case QJsonValue::Type::String:
        addStructuredContentHelper(val.toString(), ctx, out);
        break;

    case QJsonValue::Type::Array:
        addStructuredContentHelper(val.toArray(), ctx, out);
        break;

    case QJsonValue::Type::Object:
        addStructuredContentHelper(val.toObject(), ctx, out);
        break;

    default:
        break;
    }
}

/* End Structured Content Parsing */
/* Begin Other Object Parsers */

void StructuredRichText::addImage(
    const QJsonObject &obj,
    StructuredRichText::Context &ctx,
    QString &out) const
{
    /* Legacy glossary image object keys */
    constexpr const char *KEY_PATH = "path";
    constexpr const char *KEY_WIDTH = "width";
    constexpr const char *KEY_HEIGHT = "height";
    constexpr const char *KEY_TITLE = "title";
    constexpr const char *KEY_RENDERING = "imageRendering";
    constexpr const char *KEY_DESCRIPTION = "description";

    QString filename = escapeHtml(ctx.basepath + obj[KEY_PATH].toString());

    if (obj[KEY_TITLE].isString())
    {
        addAnchorStart(
            internalLinkHref("", obj[KEY_TITLE].toString(), "", "title"),
            ctx.textColor,
            out
        );
    }

    out += "<img src=\"";
    out += filename;
    out += '"';

    if (obj[KEY_WIDTH].isDouble())
    {
        out += " width=\"";
        out += QString::number(obj[KEY_WIDTH].toDouble(1.0));
        out += '"';
    }
    if (obj[KEY_HEIGHT].isDouble())
    {
        out += " height=\"";
        out += QString::number(obj[KEY_HEIGHT].toDouble(1.0));
        out += '"';
    }

    out += " style=\"display: inline-table; vertical-align: top;";
    if (obj[KEY_RENDERING].isString())
    {
        out += "image-rendering: ";
        out += obj[KEY_RENDERING].toString("auto");
        out += ";";
    }
    out += '"';

    out += '>';

    if (obj[KEY_TITLE].isString())
    {
        out += "</a>";
    }

    if (obj[KEY_DESCRIPTION].isString())
    {
        out += "<br>";
        out += escapeHtml(obj[KEY_DESCRIPTION].toString())
            .replace('\n', "<br>");
    }
}

void StructuredRichText::addText(
    const QJsonObject &obj,
    QString &out,
    bool includeSynonyms) const
{
    /* Legacy glossary text object key */
    constexpr const char *KEY_TEXT = "text";
    QString text = obj[KEY_TEXT].toString();
    if (!includeSynonyms)
    {
        static const QRegularExpression SYNONYM_LINE{
            R"(^[ \t]*(?:synonyms?|syn)[ \t]*[:.][ \t]*(.*)$)",
            QRegularExpression::CaseInsensitiveOption
        };
        QStringList retainedLines;
        bool skipFollowingLine = false;
        for (const QString &line : text.split(
                 QLatin1Char('\n'), Qt::KeepEmptyParts))
        {
            if (skipFollowingLine)
            {
                if (line.trimmed().isEmpty())
                {
                    continue;
                }
                skipFollowingLine = false;
                continue;
            }
            const QRegularExpressionMatch synonym = SYNONYM_LINE.match(line);
            if (synonym.hasMatch())
            {
                skipFollowingLine = synonym.captured(1).trimmed().isEmpty();
                continue;
            }
            retainedLines.append(line);
        }
        text = retainedLines.join(QLatin1Char('\n'));
    }
    out += escapeHtml(splitAttachedSenseMarkers(
        splitInlinePosSenseMarkers(
            splitRomanNumberedSenseMarkers(
                separatePronunciationFromFollowingText(
                    text
                )
            )
        )
    )).replace('\n', "<br>");
}

/* End Other Object Parsers */
/* Begin Helpers */

bool StructuredRichText::containsStructuredContent(
    const QJsonArray &content) const
{
    return std::any_of(
        std::begin(content), std::end(content),
        [] (const QJsonValue &value) -> bool
        {
            /* Top-level discriminator for structured glossary entries */
            constexpr const char *KEY_TYPE = "type";
            constexpr const char *VALUE_STRUCTURED_CONTENT =
                "structured-content";

            return value.type() == QJsonValue::Object &&
                value.toObject()[KEY_TYPE] == VALUE_STRUCTURED_CONTENT;
        }
    );
}

QString StructuredRichText::escapeHtml(const QString &str) const
{
    return str.toHtmlEscaped();
}

void StructuredRichText::addRuby(
    const QJsonObject &obj,
    StructuredRichText::Context &ctx,
    QString &out) const
{
    /* Structured-content key containing ruby base and reading children */
    constexpr const char *KEY_CONTENT = "content";

    QJsonArray base;
    QString reading;
    splitRubyContent(obj[KEY_CONTENT], base, reading);

    if (reading.isEmpty())
    {
        addStructuredChildren(base, ctx, out);
        return;
    }

    addAnchorStart(
        internalLinkHref(
            ctx.linkHref,
            reading,
            structuredContentText(base),
            "ruby"
        ),
        ctx.linkHref.isEmpty() ? ctx.textColor : "",
        out
    );

    const QString oldLinkHref = ctx.linkHref;
    const QString oldTitleTooltip = ctx.titleTooltip;
    ctx.linkHref.clear();
    ctx.titleTooltip.clear();
    addStructuredChildren(base, ctx, out);
    ctx.titleTooltip = oldTitleTooltip;
    ctx.linkHref = oldLinkHref;

    out += "</a>";
}

void StructuredRichText::splitRubyContent(
    const QJsonValue &val,
    QJsonArray &base,
    QString &reading) const
{
    /* Structured-content keys used to separate ruby base and reading nodes */
    constexpr const char *KEY_CONTENT = "content";
    constexpr const char *KEY_TAG = "tag";

    switch (val.type())
    {
    case QJsonValue::Type::Array:
    {
        const QJsonArray arr = val.toArray();
        for (const QJsonValue &child : arr)
        {
            if (child.isObject())
            {
                const QJsonObject obj = child.toObject();
                const QString tag = obj[KEY_TAG].toString();
                if (tag == "rt")
                {
                    reading += structuredContentText(obj[KEY_CONTENT]);
                    continue;
                }
                if (tag == "rp")
                {
                    continue;
                }
            }
            base.append(child);
        }
        break;
    }

    case QJsonValue::Type::Object:
    {
        const QJsonObject obj = val.toObject();
        const QString tag = obj[KEY_TAG].toString();
        if (tag == "rt")
        {
            reading += structuredContentText(obj[KEY_CONTENT]);
        }
        else if (tag != "rp")
        {
            base.append(val);
        }
        break;
    }

    default:
        base.append(val);
        break;
    }
}

QString StructuredRichText::structuredContentText(const QJsonValue &val) const
{
    /* Structured-content key recursively traversed for plain text */
    constexpr const char *KEY_CONTENT = "content";

    switch (val.type())
    {
    case QJsonValue::Type::String:
        return val.toString();

    case QJsonValue::Type::Array:
    {
        QString text;
        const QJsonArray arr = val.toArray();
        for (const QJsonValue &child : arr)
        {
            text += structuredContentText(child);
        }
        return text;
    }

    case QJsonValue::Type::Object:
        return structuredContentText(val.toObject()[KEY_CONTENT]);

    default:
        return "";
    }
}

bool StructuredRichText::containsRuby(const QJsonValue &val) const
{
    /* Structured-content keys used while searching for ruby descendants */
    constexpr const char *KEY_CONTENT = "content";
    constexpr const char *KEY_TAG = "tag";

    switch (val.type())
    {
    case QJsonValue::Type::Array:
    {
        const QJsonArray arr = val.toArray();
        return std::any_of(
            std::begin(arr), std::end(arr),
            [this] (const QJsonValue &child) -> bool
            {
                return containsRuby(child);
            }
        );
    }

    case QJsonValue::Type::Object:
    {
        const QJsonObject obj = val.toObject();
        if (obj[KEY_TAG].toString() == "ruby")
        {
            return true;
        }
        return containsRuby(obj[KEY_CONTENT]);
    }

    default:
        return false;
    }
}

bool StructuredRichText::anchorNeedsTooltipHandling(
    const QJsonObject &obj,
    const StructuredRichText::Context &ctx) const
{
    /* Structured-content keys that require custom anchor tooltip handling */
    constexpr const char *KEY_CONTENT = "content";
    constexpr const char *KEY_TITLE = "title";

    return containsRuby(obj[KEY_CONTENT]) ||
        obj[KEY_TITLE].isString() ||
        !ctx.titleTooltip.isEmpty();
}

QString StructuredRichText::internalLinkHref(
    const QString &target,
    const QString &tooltip,
    const QString &tooltipText,
    const QString &tooltipType) const
{
    QString out = "memento://glossary-link?";
    bool hasParam = false;

    if (!target.isEmpty())
    {
        out += "target=";
        out += QString::fromUtf8(QUrl::toPercentEncoding(target));
        hasParam = true;
    }

    if (!tooltip.isEmpty())
    {
        if (hasParam)
        {
            out += '&';
        }
        out += "tooltip=";
        out += QString::fromUtf8(QUrl::toPercentEncoding(tooltip));
        hasParam = true;
    }

    if (!tooltipText.isEmpty())
    {
        if (hasParam)
        {
            out += '&';
        }
        out += "tooltipText=";
        out += QString::fromUtf8(QUrl::toPercentEncoding(tooltipText));
        hasParam = true;
    }

    if (!tooltipType.isEmpty())
    {
        if (hasParam)
        {
            out += '&';
        }
        out += "tooltipType=";
        out += QString::fromUtf8(QUrl::toPercentEncoding(tooltipType));
    }

    return out;
}

void StructuredRichText::addAnchorStart(
    const QString &href,
    const QString &color,
    QString &out) const
{
    /* Quote substitution used to keep generated style attributes valid */
    constexpr const char QUOTE_SEARCH = '"';
    constexpr const char QUOTE_ESCAPE = '\'';

    out += "<a href=\"";
    out += escapeHtml(href);
    out += '"';
    if (!color.isEmpty())
    {
        out += " style=\"color: ";
        out += escapeHtml(QString(color).replace(QUOTE_SEARCH, QUOTE_ESCAPE));
        out += "; text-decoration: none;\"";
    }
    out += '>';
}

void StructuredRichText::addCssDeclaration(
    const QString &property,
    const QString &value,
    StructuredRichText::CssDeclarations &declarations) const
{
    const QString name = property.trimmed().toLower();
    QString cssValue = value.trimmed();

    if (name.isEmpty() || cssValue.isEmpty())
    {
        return;
    }

    cssValue.replace('"', '\'');

    if (name == "margin")
    {
        /* Maximum component count permitted by the CSS margin shorthand */
        constexpr qsizetype MAXIMUM_MARGIN_VALUES = 4;

        QStringList values;
        QString current;
        bool inQuote = false;
        QChar quote;
        int parenDepth = 0;

        for (const QChar ch : cssValue)
        {
            if (inQuote)
            {
                current += ch;
                if (ch == quote)
                {
                    inQuote = false;
                }
            }
            else if (ch == '"' || ch == '\'')
            {
                inQuote = true;
                quote = ch;
                current += ch;
            }
            else if (ch == '(')
            {
                ++parenDepth;
                current += ch;
            }
            else if (ch == ')')
            {
                --parenDepth;
                current += ch;
            }
            else if (ch.isSpace() && parenDepth == 0)
            {
                if (!current.isEmpty())
                {
                    values.emplaceBack(current);
                    current.clear();
                }
            }
            else
            {
                current += ch;
            }
        }
        if (!current.isEmpty())
        {
            values.emplaceBack(current);
        }
        if (values.isEmpty() ||
            values.size() > MAXIMUM_MARGIN_VALUES)
        {
            return;
        }

        const QString top = values[0];
        const QString right = values.size() > 1 ? values[1] : top;
        const QString bottom = values.size() > 2 ? values[2] : top;
        const QString left = values.size() > 3 ? values[3] : right;
        addCssDeclaration("margin-top", top, declarations);
        addCssDeclaration("margin-right", right, declarations);
        addCssDeclaration("margin-bottom", bottom, declarations);
        addCssDeclaration("margin-left", left, declarations);
        return;
    }

    if (name == "clip-path" &&
        cssValue.startsWith("circle(", Qt::CaseInsensitive))
    {
        declarations["border-radius"] = "50%";
        return;
    }

    /* Make sure the function is supported by Qt rich text before using it */
    QRegularExpressionMatchIterator functions =
        m_cssFunctionRegex.globalMatch(cssValue);
    while (functions.hasNext())
    {
        const QString functionName =
            functions.next().captured(1).toLower();
        if (!m_supportedCssFunctions.contains(functionName))
        {
            return;
        }
    }

    if (name == "text-decoration-line")
    {
        declarations["text-decoration"] = cssValue;
    }
    else if (name == "content")
    {
        /* Remove quotes before injecting text */
        if (cssValue.size() >= 2 &&
            ((cssValue.front() == '"' && cssValue.back() == '"') ||
             (cssValue.front() == '\'' && cssValue.back() == '\'')))
        {
            cssValue = cssValue.sliced(1, cssValue.size() - 2);
        }
        declarations[name] = cssValue;
    }
    else if (name == "background")
    {
        if (!cssValue.contains(' ') && !cssValue.contains("url("))
        {
            declarations["background-color"] = cssValue;
        }
    }
    else if (name == "list-style")
    {
        for (const QString &token : splitCssTokens(cssValue))
        {
            const QString marker = normalizeListMarker(token);
            if (marker == "none" || isBuiltInListMarker(marker))
            {
                declarations["list-style-type"] = marker;
                return;
            }
        }
    }
    else if (m_supportedCssProperties.contains(name))
    {
        declarations[name] = cssValue;
    }
}

void StructuredRichText::addResolvedCssDeclaration(
    const QString &property,
    const QString &value,
    StructuredRichText::Context &ctx,
    StructuredRichText::CssDeclarations &declarations) const
{
    const QString resolved = resolveCssValue(value, ctx);
    if (resolved.isEmpty())
    {
        return;
    }
    addCssDeclaration(property, resolved, declarations);
}

void StructuredRichText::addCssDeclarations(
    const StructuredRichText::CssDeclarations &declarations,
    QString &out) const
{
    for (const auto &[key, value] : declarations.asKeyValueRange())
    {
        /* These are stored as metadata, not handled directly by styles */
        if (key == "content" || key == "list-style-type")
        {
            continue;
        }

        out += key;
        out += ": ";
        out += value;
        out += ';';
    }
}

QString StructuredRichText::resolveCssValue(
    const QString &value,
    StructuredRichText::Context &ctx) const
{
    const auto cached = ctx.resolvedCssValues.constFind(value);
    if (cached != ctx.resolvedCssValues.cend())
    {
        return cached.value();
    }

    QString resolved = resolveCssVariables(value.trimmed(), ctx);
    if (resolved.isEmpty())
    {
        ctx.resolvedCssValues[value] = "";
        return "";
    }

    resolved = resolveCssCalculations(resolved);
    if (resolved.isEmpty())
    {
        ctx.resolvedCssValues[value] = "";
        return "";
    }

    if (resolved.startsWith("color-mix(", Qt::CaseInsensitive))
    {
        resolved = resolveColorMix(resolved);
    }
    else if (resolved.contains("gradient(", Qt::CaseInsensitive))
    {
        resolved = cssGradientFallback(resolved);
    }
    ctx.resolvedCssValues[value] = resolved;
    return resolved;
}

QString StructuredRichText::resolveCssVariables(
    QString value,
    const StructuredRichText::Context &ctx) const
{
    /* Recursion guard for nested or cyclic CSS variable substitution */
    constexpr int MAX_REPLACEMENTS = 32;

    /* Character count of the opening "var(" token */
    constexpr qsizetype CSS_VARIABLE_PREFIX_LENGTH = 4;

    for (int replacementCount = 0;
         replacementCount < MAX_REPLACEMENTS;
         ++replacementCount)
    {
        const qsizetype start = value.indexOf(
            "var(",
            0,
            Qt::CaseInsensitive
        );
        if (start < 0)
        {
            return value;
        }

        qsizetype end = -1;
        int depth = 1;
        for (qsizetype i = start + CSS_VARIABLE_PREFIX_LENGTH;
             i < value.size();
             ++i)
        {
            if (value[i] == '(')
            {
                ++depth;
            }
            else if (value[i] == ')' && --depth == 0)
            {
                end = i;
                break;
            }
        }
        if (end < 0)
        {
            return "";
        }

        const QStringList arguments = splitCssArguments(
            value.sliced(
                start + CSS_VARIABLE_PREFIX_LENGTH,
                end - start - CSS_VARIABLE_PREFIX_LENGTH
            )
        );
        if (arguments.isEmpty())
        {
            return "";
        }

        const QString name = arguments[0].trimmed().toLower();
        QString fallback;
        if (arguments.size() > 1)
        {
            fallback = arguments.sliced(1).join(',');
        }

        QString replacement;
        if (name == "--text-color" || name == "--fg")
        {
            replacement = ctx.glossaryTextColor.isEmpty() ?
                fallback :
                ctx.glossaryTextColor;
        }
        else if (name == "--font-size-no-units")
        {
            replacement = formatPixelSize(ctx.rootFontPixelSize);
        }
        else if (name == "--background-color" || name == "--canvas")
        {
            replacement = ctx.glossaryBackgroundColor.isEmpty() ?
                fallback :
                ctx.glossaryBackgroundColor;
        }
        else
        {
            replacement = fallback;
        }

        if (replacement.isEmpty())
        {
            return "";
        }
        replacement = resolveCssVariables(replacement, ctx);
        if (replacement.isEmpty())
        {
            return "";
        }
        value.replace(start, end - start + 1, replacement);
    }

    return "";
}

QString StructuredRichText::resolveCssCalculations(QString value) const
{
    /* Recursion guard for nested CSS calc() substitutions */
    constexpr int MAX_REPLACEMENTS = 16;

    /* Character count of the opening "calc(" token */
    constexpr qsizetype CSS_CALC_PREFIX_LENGTH = 5;

    for (int replacementCount = 0;
         replacementCount < MAX_REPLACEMENTS;
         ++replacementCount)
    {
        const qsizetype start = value.indexOf(
            "calc(",
            0,
            Qt::CaseInsensitive
        );
        if (start < 0)
        {
            return value;
        }

        qsizetype end = -1;
        int depth = 1;
        for (qsizetype i = start + CSS_CALC_PREFIX_LENGTH;
             i < value.size();
             ++i)
        {
            if (value[i] == '(')
            {
                ++depth;
            }
            else if (value[i] == ')' && --depth == 0)
            {
                end = i;
                break;
            }
        }
        if (end < 0)
        {
            return "";
        }

        const QString replacement = resolveCssCalc(
            value.sliced(start, end - start + 1)
        );
        if (replacement.isEmpty())
        {
            return "";
        }
        value.replace(start, end - start + 1, replacement);
    }

    return "";
}

QString StructuredRichText::resolveColorMix(const QString &value) const
{
    /* Number of colors supported by the compatibility color-mix parser */
    constexpr qsizetype COLOR_COUNT = 2;

    /* Character count of the opening "color-mix(" token */
    constexpr qsizetype COLOR_MIX_PREFIX_LENGTH = 10;

    /* Character count of a CSS function's closing parenthesis */
    constexpr qsizetype CSS_FUNCTION_SUFFIX_LENGTH = 1;

    /* Equal weighting used when neither mixed color specifies a percentage */
    constexpr double DEFAULT_COLOR_PERCENTAGE = 50.0;

    /* Percentage total used to infer one omitted color weight */
    constexpr double TOTAL_COLOR_PERCENTAGE = 100.0;

    /* Sentinel identifying a color weight omitted from color-mix() */
    constexpr double UNSPECIFIED_PERCENTAGE = -1.0;

    if (!value.endsWith(')'))
    {
        return "";
    }

    QStringList arguments = splitCssArguments(
        value.sliced(
            COLOR_MIX_PREFIX_LENGTH,
            value.size() -
                COLOR_MIX_PREFIX_LENGTH -
                CSS_FUNCTION_SUFFIX_LENGTH
        )
    );
    if (!arguments.isEmpty() &&
        arguments.front().startsWith("in ", Qt::CaseInsensitive))
    {
        arguments.removeFirst();
    }
    if (arguments.size() != COLOR_COUNT)
    {
        return "";
    }

    std::array<QColor, COLOR_COUNT> colors;
    std::array<double, COLOR_COUNT> percentages = {
        UNSPECIFIED_PERCENTAGE,
        UNSPECIFIED_PERCENTAGE
    };
    /* Trailing percentage attached to an individual color-mix argument */
    static const QRegularExpression PERCENTAGE{
        R"(\s+([0-9]+(?:\.[0-9]+)?)%\s*$)"
    };

    for (qsizetype i = 0; i < COLOR_COUNT; ++i)
    {
        QString colorValue = arguments[i].trimmed();
        const QRegularExpressionMatch match = PERCENTAGE.match(colorValue);
        if (match.hasMatch())
        {
            percentages[i] = match.captured(1).toDouble();
            colorValue = colorValue.first(match.capturedStart()).trimmed();
        }
        colors[i] = QColor::fromString(colorValue);
        if (!colors[i].isValid())
        {
            return "";
        }
    }

    if (percentages[0] < 0.0 && percentages[1] < 0.0)
    {
        percentages[0] = DEFAULT_COLOR_PERCENTAGE;
        percentages[1] = DEFAULT_COLOR_PERCENTAGE;
    }
    else if (percentages[0] < 0.0)
    {
        percentages[0] = TOTAL_COLOR_PERCENTAGE - percentages[1];
    }
    else if (percentages[1] < 0.0)
    {
        percentages[1] = TOTAL_COLOR_PERCENTAGE - percentages[0];
    }

    const double total = percentages[0] + percentages[1];
    if (total <= 0.0)
    {
        return "";
    }
    const double firstWeight = percentages[0] / total;
    const double secondWeight = percentages[1] / total;
    const double alpha =
        colors[0].alphaF() * firstWeight +
        colors[1].alphaF() * secondWeight;
    if (alpha <= 0.0)
    {
        return QColor(Qt::transparent).name(QColor::HexArgb);
    }

    return QColor::fromRgbF(
        (
            colors[0].redF() * colors[0].alphaF() * firstWeight +
            colors[1].redF() * colors[1].alphaF() * secondWeight
        ) / alpha,
        (
            colors[0].greenF() * colors[0].alphaF() * firstWeight +
            colors[1].greenF() * colors[1].alphaF() * secondWeight
        ) / alpha,
        (
            colors[0].blueF() * colors[0].alphaF() * firstWeight +
            colors[1].blueF() * colors[1].alphaF() * secondWeight
        ) / alpha,
        alpha
    ).name(QColor::HexArgb);
}

QString StructuredRichText::resolveCssCalc(const QString &value) const
{
    /* Simple dimension-by-number division supported inside calc() */
    static const QRegularExpression DIVISION{
        R"(^calc\(\s*([+-]?(?:\d+(?:\.\d*)?|\.\d+))([a-z%]+)\s*/\s*)"
        R"(([+-]?(?:\d+(?:\.\d*)?|\.\d+))\s*\)$)",
        QRegularExpression::CaseInsensitiveOption
    };

    const QRegularExpressionMatch match = DIVISION.match(value);
    if (!match.hasMatch())
    {
        return "";
    }

    const double denominator = match.captured(3).toDouble();
    if (denominator == 0.0)
    {
        return "";
    }
    return formatPixelSize(
        match.captured(1).toDouble() / denominator
    ) + match.captured(2);
}

QString StructuredRichText::cssGradientFallback(const QString &value) const
{
    const qsizetype open = value.indexOf('(');
    if (open < 0 || !value.endsWith(')'))
    {
        return "";
    }

    const QStringList arguments = splitCssArguments(
        value.sliced(open + 1, value.size() - open - 2)
    );
    if (arguments.isEmpty())
    {
        return "";
    }

    QString colorValue = arguments.front().trimmed();
    /* Trailing percentage position attached to a gradient color stop */
    static const QRegularExpression COLOR_STOP{
        R"(\s+[0-9]+(?:\.[0-9]+)?%\s*$)"
    };
    const QRegularExpressionMatch match = COLOR_STOP.match(colorValue);
    if (match.hasMatch())
    {
        colorValue = colorValue.first(match.capturedStart()).trimmed();
    }

    const QColor color = QColor::fromString(colorValue);
    return color.isValid() ? color.name(QColor::HexArgb) : "";
}

QStringList StructuredRichText::splitCssArguments(
    const QString &arguments) const
{
    QStringList result;
    QString current;
    bool inQuote = false;
    QChar quote;
    int parenDepth = 0;

    for (const QChar ch : arguments)
    {
        if (inQuote)
        {
            current += ch;
            if (ch == quote)
            {
                inQuote = false;
            }
        }
        else if (ch == '"' || ch == '\'')
        {
            inQuote = true;
            quote = ch;
            current += ch;
        }
        else if (ch == '(')
        {
            ++parenDepth;
            current += ch;
        }
        else if (ch == ')')
        {
            --parenDepth;
            current += ch;
        }
        else if (ch == ',' && parenDepth == 0)
        {
            result.emplaceBack(current.trimmed());
            current.clear();
        }
        else
        {
            current += ch;
        }
    }
    result.emplaceBack(current.trimmed());
    return result;
}

QString StructuredRichText::normalizeListMarker(QString marker) const
{
    marker = marker.trimmed();
    marker.replace('"', '\'');
    if (marker.size() >= 2 &&
        ((marker.front() == '\'' && marker.back() == '\'') ||
         (marker.front() == '"' && marker.back() == '"')))
    {
        marker = marker.sliced(1, marker.size() - 2);
    }
    return marker;
}

QString StructuredRichText::defaultListMarker(const QString &tag) const
{
    if (tag == "ol")
    {
        return "1";
    }
    if (tag == "ul")
    {
        return "disc";
    }
    return "";
}

QString StructuredRichText::listMarker(
    const StructuredList &list, const QString &marker) const
{
    QString normalized = normalizeListMarker(marker);
    if (normalized.isEmpty())
    {
        normalized = list.marker;
    }
    if (normalized == "none")
    {
        return "";
    }
    if (!isBuiltInListMarker(normalized))
    {
        return normalized;
    }
    if (normalized == "disc")
    {
        return QString{QChar(0x2022)};
    }
    if (normalized == "circle")
    {
        return QString{QChar(0x25E6)};
    }
    if (normalized == "square")
    {
        return QString{QChar(0x25AA)};
    }
    if (normalized == "1")
    {
        return useCircledSenseMarkers(
            QString::number(list.item) + QLatin1Char('.')
        );
    }
    if (normalized == "a" || normalized == "A")
    {
        /* Number of symbols in the Latin alphabetic list-marker sequence */
        constexpr int LETTERS_IN_ALPHABET = 26;

        QString alpha;
        qsizetype value = list.item;
        while (value > 0)
        {
            --value;
            const QChar ch{
                'a' + static_cast<char>(value % LETTERS_IN_ALPHABET)
            };
            alpha.prepend(normalized == "A" ? ch.toUpper() : ch);
            value /= LETTERS_IN_ALPHABET;
        }
        return alpha + '.';
    }
    return list.marker;
}

bool StructuredRichText::isBuiltInListMarker(const QString &marker) const
{
    const QString normalized = normalizeListMarker(marker);
    return normalized.isEmpty() ||
        normalized == "disc" ||
        normalized == "circle" ||
        normalized == "square" ||
        normalized == "1" ||
        normalized == "a" ||
        normalized == "A";
}

void StructuredRichText::addMatchingCssRules(
    StructuredRichText::Context &ctx,
    StructuredRichText::CssDeclarations &declarations,
    QString *beforeContent,
    QString *afterContent) const
{
    if (ctx.dictionaryStyles == nullptr || ctx.elements.isEmpty())
    {
        return;
    }

    const ParsedStylesheet &stylesheet =
        ctx.dictionaryStyles->parsedStylesheet();
    const QList<qsizetype> &candidateIndexes =
        ctx.dictionaryStyles->candidateRuleIndexes(
            ctx.selectorElements[ctx.elements.back()].tag
        );
    for (const qsizetype index : candidateIndexes)
    {
        const CssRule &rule = stylesheet.rules[index];
        if (!cssRuleMatches(rule, ctx))
        {
            continue;
        }
        if (rule.pseudoElement != DictionaryStyles::CssPseudoElement::None)
        {
            QString *content =
                rule.pseudoElement == DictionaryStyles::CssPseudoElement::After ?
                    afterContent :
                    beforeContent;
            if (content != nullptr)
            {
                for (const CssDeclaration &declaration : rule.declarations)
                {
                    if (declaration.property == "content")
                    {
                        *content = declaration.value;
                    }
                }
            }
            continue;
        }

        for (const CssDeclaration &declaration : rule.declarations)
        {
            QString value = resolveCssValue(declaration.value, ctx);
            if (value.isEmpty())
            {
                continue;
            }
            if (declaration.property == "font-size")
            {
                const double pixelSize = cssFontSizeToPixels(
                    value,
                    ctx.screen,
                    ctx.parentFontPixelSize,
                    ctx.rootFontPixelSize
                );
                if (pixelSize >= 0.0)
                {
                    value = formatPixelSize(pixelSize) + "px";
                }
            }
            addCssDeclaration(declaration.property, value, declarations);
        }
    }
}

bool StructuredRichText::cssRuleMatches(
    const CssRule &rule,
    const StructuredRichText::Context &ctx) const
{
    if (rule.selector.isEmpty() || ctx.elements.isEmpty())
    {
        return false;
    }

    const qsizetype stackIndex = ctx.elements.size() - 1;
    return cssRuleMatchesAt(
        rule,
        ctx,
        rule.selector.size() - 1,
        stackIndex,
        ctx.elements[stackIndex]
    );
}

bool StructuredRichText::cssRuleMatchesAt(
    const CssRule &rule,
    const StructuredRichText::Context &ctx,
    qsizetype partIndex,
    qsizetype stackIndex,
    qsizetype selectorElementIndex) const
{
    const StructuredElement &element =
        ctx.selectorElements[selectorElementIndex];
    if (partIndex < 0 ||
        !cssSelectorPartMatches(rule.selector[partIndex], element))
    {
        return false;
    }
    if (partIndex == 0)
    {
        return true;
    }

    switch (rule.selector[partIndex].combinator)
    {
    case DictionaryStyles::CssCombinator::Child:
        if (stackIndex <= 0)
        {
            return false;
        }
        return cssRuleMatchesAt(
            rule,
            ctx,
            partIndex - 1,
            stackIndex - 1,
            ctx.elements[stackIndex - 1]
        );

    case DictionaryStyles::CssCombinator::AdjacentSibling:
        if (element.previousSibling < 0)
        {
            return false;
        }
        return cssRuleMatchesAt(
            rule,
            ctx,
            partIndex - 1,
            stackIndex,
            element.previousSibling
        );

    case DictionaryStyles::CssCombinator::GeneralSibling:
        for (qsizetype sibling = element.previousSibling;
             sibling >= 0;
             sibling = ctx.selectorElements[sibling].previousSibling)
        {
            if (cssRuleMatchesAt(
                    rule,
                    ctx,
                    partIndex - 1,
                    stackIndex,
                    sibling
                ))
            {
                return true;
            }
        }
        return false;

    case DictionaryStyles::CssCombinator::Descendant:
        for (qsizetype i = stackIndex - 1; i >= 0; --i)
        {
            if (cssRuleMatchesAt(
                    rule,
                    ctx,
                    partIndex - 1,
                    i,
                    ctx.elements[i]
                ))
            {
                return true;
            }
        }
        return false;
    }

    return false;
}

bool StructuredRichText::cssSelectorPartMatches(
    const CssSelectorPart &part,
    const StructuredElement &element) const
{
    if (!part.tag.isEmpty() && part.tag != element.tag)
    {
        return false;
    }
    for (const QString &className : part.classNames)
    {
        if (!element.classes.contains(className))
        {
            return false;
        }
    }
    for (const DictionaryStyles::CssAttributeSelector &attribute :
         part.attributes)
    {
        const auto it = element.attributes.constFind(attribute.name);
        if (it == element.attributes.cend())
        {
            return false;
        }
        switch (attribute.op)
        {
        case DictionaryStyles::CssAttributeOperator::Exists:
            break;

        case DictionaryStyles::CssAttributeOperator::Equals:
            if (it.value() != attribute.value)
            {
                return false;
            }
            break;

        case DictionaryStyles::CssAttributeOperator::StartsWith:
            if (!it.value().startsWith(attribute.value))
            {
                return false;
            }
            break;
        }
    }
    if (!part.pseudoClasses.isEmpty() && element.childIndex <= 0)
    {
        return false;
    }
    for (const DictionaryStyles::CssPseudoClassSelector &pseudo :
         part.pseudoClasses)
    {
        bool matched = false;
        switch (pseudo.type)
        {
        case DictionaryStyles::CssPseudoClass::FirstChild:
            matched = element.childIndex == 1;
            break;

        case DictionaryStyles::CssPseudoClass::LastChild:
            if (element.childCount <= 0)
            {
                return false;
            }
            matched = element.childIndex == element.childCount;
            break;

        case DictionaryStyles::CssPseudoClass::NthChild:
            matched = element.childIndex == pseudo.childIndex;
            break;
        }

        if (pseudo.negated ? matched : !matched)
        {
            return false;
        }
    }
    return true;
}

qsizetype StructuredRichText::structuredElement(
    const QJsonObject &obj,
    StructuredRichText::Context &ctx) const
{
    /* Structured-content keys recorded for CSS selector matching */
    constexpr const char *KEY_DATA = "data";
    constexpr const char *KEY_HREF = "href";
    constexpr const char *KEY_LANG = "lang";
    constexpr const char *KEY_TAG = "tag";
    constexpr const char *KEY_TITLE = "title";

    StructuredElement element;
    element.tag = obj[KEY_TAG].toString().toLower();
    element.classes.insert("gloss-sc-" + element.tag);
    if (element.tag == "details")
    {
        element.attributes["open"] = "";
    }
    if (element.tag == "img")
    {
        element.classes.insert("gloss-image");
        element.classes.insert("gloss-image-container");
        element.classes.insert("gloss-image-link");
    }

    if (obj[KEY_DATA].isObject())
    {
        const QJsonObject data = obj[KEY_DATA].toObject();
        for (const QString &key : data.keys())
        {
            if (data[key].isString())
            {
                element.attributes[structuredDataAttributeName(key)] =
                    data[key].toString();
            }
        }
    }

    /* Standard attributes copied into the selector-node attribute map */
    constexpr std::array<const char *, 3> STANDARD_ATTRIBUTES = {
        KEY_HREF,
        KEY_LANG,
        KEY_TITLE
    };
    for (const char *key : STANDARD_ATTRIBUTES)
    {
        if (obj[key].isString())
        {
            element.attributes[key] = obj[key].toString();
        }
    }

    if (!ctx.siblings.isEmpty())
    {
        SiblingState &siblings = ctx.siblings.back();
        element.previousSibling = siblings.previousElement;
        element.childIndex = ++siblings.visitedElementCount;
        element.childCount = siblings.elementCount;
    }

    const qsizetype index = ctx.selectorElements.size();
    ctx.selectorElements.emplaceBack(std::move(element));
    if (!ctx.siblings.isEmpty())
    {
        ctx.siblings.back().previousElement = index;
    }
    return index;
}

QString StructuredRichText::structuredDataAttributeName(
    const QString &key) const
{
    QString out = "data-sc-";
    for (const QChar ch : key)
    {
        if (ch.isUpper())
        {
            out += '-';
            out += ch.toLower();
        }
        else
        {
            out += ch;
        }
    }
    return out;
}

bool StructuredRichText::isSupportedStructuredTag(const QString &tag) const
{
    return m_supportedTags.contains(tag);
}

double StructuredRichText::cssFontSizeToPixels(
    const QString &size,
    const QScreen *screen,
    const QFont &font) const
{
    const double fontSize = fontPixelSize(font, screen);
    return cssFontSizeToPixels(size, screen, fontSize, fontSize);
}


double StructuredRichText::cssFontSizeToPixels(
    const QString &size,
    const QScreen *screen,
    double parentFontPixelSize,
    double rootFontPixelSize) const
{
    const QString normalized = size.trimmed().toLower();
    if (normalized.isEmpty())
    {
        return -1.0;
    }

    if (normalized == "inherit" || normalized == "unset" ||
        normalized == "revert" || normalized == "revert-layer")
    {
        return parentFontPixelSize;
    }
    if (normalized == "initial" || normalized == "medium")
    {
        return rootFontPixelSize;
    }
    if (normalized == "xx-small")
    {
        return rootFontPixelSize * 3.0 / 5.0;
    }
    if (normalized == "x-small")
    {
        return rootFontPixelSize * 3.0 / 4.0;
    }
    if (normalized == "small")
    {
        return rootFontPixelSize * 8.0 / 9.0;
    }
    if (normalized == "large")
    {
        return rootFontPixelSize * 6.0 / 5.0;
    }
    if (normalized == "x-large")
    {
        return rootFontPixelSize * 3.0 / 2.0;
    }
    if (normalized == "xx-large")
    {
        return rootFontPixelSize * 2.0;
    }
    if (normalized == "xxx-large")
    {
        return rootFontPixelSize * 3.0;
    }
    if (normalized == "smaller")
    {
        return parentFontPixelSize / 1.2;
    }
    if (normalized == "larger")
    {
        return parentFontPixelSize * 1.2;
    }

    qsizetype index = 0;
    bool ok = false;
    normalized.toDouble(&ok);
    if (ok)
    {
        index = normalized.size();
    }
    else
    {
        while (index < normalized.size())
        {
            const QChar ch = normalized[index];
            if (!ch.isDigit() && ch != '-' && ch != '+' && ch != '.')
            {
                break;
            }
            ++index;
        }
        if (index == 0)
        {
            return -1.0;
        }
    }

    const QString number = normalized.first(index);
    ok = false;
    const double numericValue = number.toDouble(&ok);
    if (!ok || !std::isfinite(numericValue))
    {
        return -1.0;
    }

    const QString unit = normalized.sliced(index).trimmed();
    if (unit.isEmpty())
    {
        return numericValue == 0.0 ? 0.0 : -1.0;
    }
    if (unit == "px")
    {
        return numericValue;
    }
    if (unit == "%")
    {
        return parentFontPixelSize * numericValue / 100.0;
    }
    if (unit == "em")
    {
        return parentFontPixelSize * numericValue;
    }
    if (unit == "rem")
    {
        return rootFontPixelSize * numericValue;
    }
    if (unit == "ex")
    {
        return parentFontPixelSize * numericValue / 2.0;
    }
    if (unit == "rex")
    {
        return rootFontPixelSize * numericValue / 2.0;
    }
    if (unit == "ch")
    {
        return parentFontPixelSize * numericValue / 2.0;
    }
    if (unit == "rch")
    {
        return rootFontPixelSize * numericValue / 2.0;
    }
    if (unit == "cap")
    {
        return parentFontPixelSize * numericValue * 0.7;
    }
    if (unit == "rcap")
    {
        return rootFontPixelSize * numericValue * 0.7;
    }
    if (unit == "ic")
    {
        return parentFontPixelSize * numericValue;
    }
    if (unit == "ric")
    {
        return rootFontPixelSize * numericValue;
    }
    if (unit == "lh")
    {
        return parentFontPixelSize * numericValue * 1.2;
    }
    if (unit == "rlh")
    {
        return rootFontPixelSize * numericValue * 1.2;
    }

    if (screen != nullptr)
    {
        const QSize screenSize = screen->availableGeometry().size();
        if (unit == "vw" || unit == "svw" || unit == "lvw" || unit == "dvw" ||
            unit == "vi" || unit == "svi" || unit == "lvi" || unit == "dvi")
        {
            return screenSize.width() * numericValue / 100.0;
        }
        if (unit == "vh" || unit == "svh" || unit == "lvh" || unit == "dvh" ||
            unit == "vb" || unit == "svb" || unit == "lvb" || unit == "dvb")
        {
            return screenSize.height() * numericValue / 100.0;
        }
        if (unit == "vmin" || unit == "svmin" || unit == "lvmin" ||
            unit == "dvmin")
        {
            return std::min(screenSize.width(), screenSize.height()) *
                numericValue / 100.0;
        }
        if (unit == "vmax" || unit == "svmax" || unit == "lvmax" ||
            unit == "dvmax")
        {
            return std::max(screenSize.width(), screenSize.height()) *
                numericValue / 100.0;
        }
    }

    const double dpi = screenDpi(screen);
    if (unit == "pt")
    {
        return numericValue * dpi / 72.0;
    }
    if (unit == "pc")
    {
        return numericValue * dpi / 6.0;
    }
    if (unit == "in")
    {
        return numericValue * dpi;
    }
    if (unit == "cm")
    {
        return numericValue * dpi / 2.54;
    }
    if (unit == "mm")
    {
        return numericValue * dpi / 25.4;
    }
    if (unit == "q")
    {
        return numericValue * dpi / 101.6;
    }

    return -1.0;
}

double StructuredRichText::fontPixelSize(
    const QFont &font, const QScreen *screen) const
{
    /* Typographic points per physical inch */
    constexpr double POINTS_IN_INCH = 72.0;

    /* Browser-compatible font size used when the QFont has no valid size */
    constexpr double DEFAULT_PIXEL_SIZE = 16.0;

    if (font.pixelSize() > 0)
    {
        return font.pixelSize();
    }
    if (font.pointSizeF() > 0)
    {
        return font.pointSizeF() * screenDpi(screen) / POINTS_IN_INCH;
    }
    return DEFAULT_PIXEL_SIZE;
}

double StructuredRichText::screenDpi(const QScreen *screen) const
{
    /* CSS reference pixel density used when no screen DPI is available */
    constexpr double DEFAULT_SCREEN_DPI = 96.0;

    if (screen == nullptr)
    {
        screen = QGuiApplication::primaryScreen();
        if (screen == nullptr)
        {
            return DEFAULT_SCREEN_DPI;
        }
    }
    const double dpi = screen->logicalDotsPerInch();
    return dpi > 0.0 ? dpi : DEFAULT_SCREEN_DPI;
}

QString StructuredRichText::formatPixelSize(double size) const
{
    QString formatted = QString::number(size, 'f', 3);
    while (formatted.endsWith('0'))
    {
        formatted.chop(1);
    }
    if (formatted.endsWith('.'))
    {
        formatted.chop(1);
    }
    return formatted;
}

/* End Helpers */
