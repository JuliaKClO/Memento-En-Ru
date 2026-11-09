////////////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2024 Ripose
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

#include "dict/exactquerygenerator.h"

namespace
{
bool isJapaneseCharacter(QChar character)
{
    const ushort code = character.unicode();
    return (code >= 0x3040 && code <= 0x30FF) ||
        (code >= 0x3400 && code <= 0x9FFF) ||
        (code >= 0xF900 && code <= 0xFAFF) ||
        (code >= 0xFF66 && code <= 0xFF9D);
}

QString compactJapaneseWhitespace(const QString &text)
{
    QString compact;
    compact.reserve(text.size());
    for (qsizetype i = 0; i < text.size();)
    {
        if (!text.at(i).isSpace())
        {
            compact.append(text.at(i++));
            continue;
        }

        const qsizetype start = i;
        while (i < text.size() && text.at(i).isSpace())
        {
            ++i;
        }
        const bool betweenJapanese = start > 0 && i < text.size() &&
            isJapaneseCharacter(text.at(start - 1)) &&
            isJapaneseCharacter(text.at(i));
        if (!betweenJapanese)
        {
            compact.append(text.mid(start, i - start));
        }
    }
    return compact;
}
}

std::vector<SearchQuery> ExactQueryGenerator::generateQueries(
    const QString &text) const
{
    std::vector<SearchQuery> queries;

    QString query = text;
    while (!query.isEmpty())
    {
        SearchQuery sq{};
        sq.deconj = query;
        sq.surface = query;
        sq.source = SearchQuery::Source::exact;
        queries.emplace_back(sq);

        /* Compact Japanese spacing without joining Latin words or crossing
         * a Japanese/Latin boundary in mixed subtitles. */
        sq.deconj = compactJapaneseWhitespace(query);
        if (sq.deconj != query)
        {
            sq.surface = query.trimmed();
            queries.emplace_back(std::move(sq));
        }

        query.chop(1);
    }

    return queries;
}
