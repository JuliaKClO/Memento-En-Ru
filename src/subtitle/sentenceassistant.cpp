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

#include "subtitle/sentenceassistant.h"

#include <algorithm>

#include <QSet>

#include "setting/keys.h"

namespace
{
constexpr int MAX_NEIGHBOR_SUBTITLES{5};
constexpr qsizetype MAX_CONTEXT_LENGTH{1200};

QString simplifySubtitle(const QString &text)
{
    return text.simplified();
}

bool isClosingPunctuation(QChar character)
{
    return character == u'"' || character == u'\'' || character == u')' ||
        character == u']' || character == u'}' || character == u'»' ||
        character == u'”' || character == u'’';
}

bool isAsciiLetter(QChar character)
{
    return (character >= u'a' && character <= u'z') ||
        (character >= u'A' && character <= u'Z');
}

bool isAbbreviationPeriod(const QString &text, qsizetype index)
{
    if (index <= 0 || index + 1 >= text.size())
    {
        return false;
    }

    if (text[index - 1].isDigit() && text[index + 1].isDigit())
    {
        return true;
    }
    if (text[index + 1] == u'.' && index + 2 < text.size() &&
        isAsciiLetter(text[index + 2]))
    {
        return true;
    }

    qsizetype tokenStart = index - 1;
    while (tokenStart >= 0 &&
           (isAsciiLetter(text[tokenStart]) || text[tokenStart] == u'.'))
    {
        --tokenStart;
    }
    const QString token = text.mid(tokenStart + 1, index - tokenStart - 1)
        .toLower();
    static const QSet<QString> ABBREVIATIONS{
        QStringLiteral("mr"), QStringLiteral("mrs"), QStringLiteral("ms"),
        QStringLiteral("dr"), QStringLiteral("prof"), QStringLiteral("sr"),
        QStringLiteral("jr"), QStringLiteral("st"), QStringLiteral("vs"),
        QStringLiteral("etc"), QStringLiteral("e.g"), QStringLiteral("i.e"),
        QStringLiteral("no"), QStringLiteral("fig"),
    };
    if (ABBREVIATIONS.contains(token))
    {
        return true;
    }

    // Treat initials and dotted initialisms as part of the name when followed
    // by another capitalized word (for example, "J. R. R. Tolkien" or "U.S.").
    if (token.size() == 1 && isAsciiLetter(token.front()))
    {
        qsizetype next = index + 1;
        while (next < text.size() && text[next].isSpace())
        {
            ++next;
        }
        if (next < text.size() && text[next].isUpper())
        {
            return true;
        }
    }
    if (token.contains(QLatin1Char('.')) &&
        std::all_of(token.cbegin(), token.cend(), [](QChar character) {
            return isAsciiLetter(character) || character == u'.';
        }))
    {
        return true;
    }

    return false;
}

bool isSentenceEnd(const QString &text, qsizetype index)
{
    const QChar character = text[index];
    if (character == u'.')
    {
        return !isAbbreviationPeriod(text, index);
    }
    return character == u'?' || character == u'!' || character == u'…';
}

QString sentenceAt(const QString &text, qsizetype cursor)
{
    qsizetype sentenceStart = 0;
    qsizetype sentenceEnd = -1;
    qsizetype cursorSentenceStart = 0;
    qsizetype cursorSentenceEnd = text.size();

    for (qsizetype index = 0; index < text.size(); ++index)
    {
        if (!isSentenceEnd(text, index))
        {
            continue;
        }

        qsizetype end = index + 1;
        while (end < text.size() &&
               (text[end] == u'.' || text[end] == u'?' || text[end] == u'!' ||
                text[end] == u'…' || isClosingPunctuation(text[end])))
        {
            ++end;
        }

        if (cursor >= sentenceStart && cursor < end)
        {
            cursorSentenceStart = sentenceStart;
            cursorSentenceEnd = end;
            sentenceEnd = end;
            break;
        }

        sentenceStart = end;
        while (sentenceStart < text.size() && text[sentenceStart].isSpace())
        {
            ++sentenceStart;
        }
        index = end - 1;
    }

    if (sentenceEnd < 0 && cursor >= sentenceStart)
    {
        cursorSentenceStart = sentenceStart;
        cursorSentenceEnd = text.size();
    }

    return text.mid(cursorSentenceStart, cursorSentenceEnd - cursorSentenceStart)
        .trimmed();
}

QString joinSubtitles(const QStringList &subtitles, int start, int end)
{
    QStringList pieces;
    pieces.reserve(end - start + 1);
    for (int index = start; index <= end; ++index)
    {
        const QString piece = simplifySubtitle(subtitles[index]);
        if (!piece.isEmpty())
        {
            pieces.emplaceBack(piece);
        }
    }
    return pieces.join(u' ');
}
}

SentenceAssistant::SentenceAssistant(QObject *parent) : QObject(parent)
{

}

QString SentenceAssistant::collectSentence(
    const QStringList &subtitles,
    int currentSubtitleIndex,
    int cursorIndex) const
{
    if (subtitles.isEmpty() || currentSubtitleIndex < 0 ||
        currentSubtitleIndex >= subtitles.size())
    {
        return {};
    }

    const QString current = simplifySubtitle(subtitles[currentSubtitleIndex]);
    if (current.isEmpty())
    {
        return {};
    }

    const QString normalizedPrefix = simplifySubtitle(
        subtitles[currentSubtitleIndex].left(
            std::clamp<qsizetype>(
                cursorIndex, 0, subtitles[currentSubtitleIndex].size()
            )
        )
    );
    qsizetype localCursor = std::min(normalizedPrefix.size(), current.size());

    int start = std::max(0, currentSubtitleIndex - MAX_NEIGHBOR_SUBTITLES);
    int end = std::min<int>(
        static_cast<int>(subtitles.size()) - 1,
        currentSubtitleIndex + MAX_NEIGHBOR_SUBTITLES
    );
    QString joined = joinSubtitles(subtitles, start, end);

    // Drop the most distant cues first if this local window exceeds the cap.
    while (joined.size() > MAX_CONTEXT_LENGTH &&
           (start < currentSubtitleIndex || end > currentSubtitleIndex))
    {
        const int leftDistance = currentSubtitleIndex - start;
        const int rightDistance = end - currentSubtitleIndex;
        if (leftDistance >= rightDistance && start < currentSubtitleIndex)
        {
            ++start;
        }
        else if (end > currentSubtitleIndex)
        {
            --end;
        }
        joined = joinSubtitles(subtitles, start, end);
    }

    qsizetype cursor = localCursor;
    for (int index = start; index < currentSubtitleIndex; ++index)
    {
        const QString piece = simplifySubtitle(subtitles[index]);
        if (!piece.isEmpty())
        {
            cursor += piece.size() + 1;
        }
    }

    if (joined.size() > MAX_CONTEXT_LENGTH)
    {
        const qsizetype left = std::clamp<qsizetype>(
            cursor - MAX_CONTEXT_LENGTH / 2,
            0,
            joined.size() - MAX_CONTEXT_LENGTH
        );
        joined = joined.mid(left, MAX_CONTEXT_LENGTH);
        cursor = std::clamp<qsizetype>(
            cursor - left, 0, joined.size() - 1
        );
    }
    else
    {
        cursor = std::clamp<qsizetype>(cursor, 0, joined.size() - 1);
    }

    return sentenceAt(joined, cursor);
}

QString SentenceAssistant::buildPrompt(
    const QString &sentence, const QString &promptTemplate) const
{
    if (sentence.isEmpty())
    {
        return {};
    }

    QString prompt = promptTemplate.trimmed();
    if (prompt.isEmpty())
    {
        prompt = Keys::Search::AI_PROMPT_DEFAULT;
    }

    const QString sentenceMarker = QStringLiteral("{{SENTENCE}}");
    if (prompt.contains(sentenceMarker))
    {
        prompt.replace(sentenceMarker, sentence);
    }
    else
    {
        prompt += QStringLiteral("\n\n**Предложение для разбора:**\n") + sentence;
    }
    return prompt;
}
