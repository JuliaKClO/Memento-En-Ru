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

#include "dict/englishquerygenerator.h"

#include "dict/data/irregularverbs.h"

#include <utility>

#include <QHash>
#include <QRegularExpression>
#include <QSet>

namespace
{
const QHash<QString, QString> &irregularVerbs()
{
    /* Hand-curated seed; ambiguous forms such as "saw" are intentionally
     * omitted so that the original exact lookup remains the only candidate. */
    static const QHash<QString, QString> forms{
        { "am", "be" }, { "are", "be" }, { "was", "be" },
        { "were", "be" }, { "been", "be" },
        { "did", "do" }, { "does", "do" }, { "doing", "do" },
        { "done", "do" },
        { "had", "have" }, { "has", "have" },
        { "is", "be" }, { "being", "be" },
        { "went", "go" }, { "gone", "go" },
        { "goes", "go" }, { "going", "go" },
        { "grew", "grow" }, { "grown", "grow" },
        { "gave", "give" }, { "given", "give" },
        { "took", "take" }, { "taken", "take" },
        { "made", "make" },
        { "ate", "eat" }, { "eaten", "eat" },
        { "ran", "run" },
        { "spoke", "speak" }, { "spoken", "speak" },
        { "wrote", "write" }, { "written", "write" },
        { "bought", "buy" }, { "brought", "bring" },
        { "thought", "think" },
        { "left", "leave" }, { "felt", "feel" },
        { "kept", "keep" }, { "slept", "sleep" },
        { "sat", "sit" }, { "stood", "stand" },
        { "told", "tell" }, { "heard", "hear" },
        { "lost", "lose" }, { "won", "win" },
        { "met", "meet" }, { "sent", "send" },
        { "paid", "pay" }, { "sold", "sell" },
        { "found", "find" }, { "knew", "know" },
        { "known", "know" }, { "threw", "throw" },
        { "thrown", "throw" }, { "wore", "wear" },
        { "worn", "wear" }, { "became", "become" },
        { "began", "begin" },
        { "begun", "begin" }, { "broke", "break" },
        { "broken", "break" }, { "chose", "choose" },
        { "chosen", "choose" }, { "drove", "drive" },
        { "driven", "drive" }, { "fell", "fall" },
        { "fallen", "fall" }, { "forgot", "forget" },
        { "forgotten", "forget" }, { "got", "get" },
        { "gotten", "get" }, { "held", "hold" },
        { "rode", "ride" }, { "ridden", "ride" },
        { "sang", "sing" }, { "sung", "sing" },
        { "swam", "swim" }, { "swum", "swim" },
        { "understood", "understand" }, { "woke", "wake" },
        { "woken", "wake" }, { "died", "die" },
        { "tied", "tie" }, { "lied", "lie" },
    };
    return forms;
}

bool isAsciiConsonant(QChar character)
{
    const QChar lower = character.toLower();
    return lower >= QLatin1Char('a') && lower <= QLatin1Char('z') &&
        lower != QLatin1Char('a') && lower != QLatin1Char('e') &&
        lower != QLatin1Char('i') && lower != QLatin1Char('o') &&
        lower != QLatin1Char('u');
}

const QSet<QString> &ieVerbs()
{
    static const QSet<QString> values{ "die", "lie", "tie", "vie" };
    return values;
}

const QSet<QString> &nonVerbExceptions()
{
    /* Common lexical words whose endings resemble verb inflections but are
     * not useful verb-form candidates in this context-free first pass. */
    static const QSet<QString> values{
        "buildings", "countries", "evening", "feelings", "meetings",
        "morning", "news", "players", "series", "species", "things",
    };
    return values;
}

void addLemma(
    QHash<QString, int> &lemmas,
    QString lemma,
    int priority = 1,
    bool allowShort = false)
{
    lemma = lemma.toLower();
    if (lemma.size() >= (allowShort ? 2 : 3))
    {
        const auto existing = lemmas.constFind(lemma);
        if (existing == lemmas.cend() || priority < *existing)
        {
            lemmas.insert(std::move(lemma), priority);
        }
    }
}

bool shouldPreferFinalE(const QString &stem)
{
    if (stem.isEmpty() || IrregularVerbForms::isBaseVerb(stem))
    {
        return false;
    }

    const QChar last = stem.back().toLower();
    return last == QLatin1Char('c') || last == QLatin1Char('g') ||
        last == QLatin1Char('v') || last == QLatin1Char('z') ||
        last == QLatin1Char('s') || stem.endsWith("th") ||
        stem.endsWith("at") ||
        (stem.size() == 3 && isAsciiConsonant(stem.at(0)) &&
            !isAsciiConsonant(stem.at(1)) && isAsciiConsonant(stem.at(2)));
}

bool shouldKeepUnrestoredStemFallback(const QString &stem)
{
    /* A final-e hypothesis such as hope -> hoped normally does not also make
     * the bare stem a useful candidate (hop is spelled hopped). Keep the
     * explicit breath/breathe fallback, which is covered by the tests. */
    return stem.endsWith("th");
}

QHash<QString, int> verbLemmas(const QString &surface)
{
    const QString form = surface.toLower();
    QHash<QString, int> lemmas;

    const auto irregular = irregularVerbs().constFind(form);
    if (irregular != irregularVerbs().cend())
    {
        addLemma(lemmas, *irregular, 1, true);
        return lemmas;
    }

    // The complete shared list covers irregular forms outside the small
    // conservative seed map. Preserve "saw" as exact-only because it is also
    // the past tense of "see".
    if (form != QStringLiteral("saw"))
    {
        const QStringList fullListLemmas =
            IrregularVerbForms::lemmasForForm(form);
        if (!fullListLemmas.isEmpty())
        {
            for (const QString &lemma : fullListLemmas)
            {
                addLemma(lemmas, lemma, 1, true);
            }
            return lemmas;
        }
    }

    // Base-form irregular verbs can start multiword dictionary expressions,
    // even when the token itself does not need lemmatization ("break a leg").
    // "saw" is also the past tense of "see"; keep the exact-only behavior
    // for that ambiguous surface even though it has its own verb entry.
    if (form != QStringLiteral("saw") &&
        IrregularVerbForms::isBaseVerb(form))
    {
        addLemma(lemmas, form, 1, true);
        return lemmas;
    }

    if (nonVerbExceptions().contains(form))
    {
        return lemmas;
    }

    /* Do not interpret contractions as inflected verbs. */
    if (form.contains(QLatin1Char('\'')) || form.contains(QChar(0x2019)))
    {
        return lemmas;
    }

    if (form.size() > 4 && form.endsWith("ies"))
    {
        const QString stem = form.left(form.size() - 3);
        addLemma(lemmas, stem + QLatin1Char('y'));
    }
    else if (form.size() > 4 && form.endsWith("es"))
    {
        const QChar beforeEs = form.at(form.size() - 3);
        if (beforeEs == QLatin1Char('s') || beforeEs == QLatin1Char('x') ||
            beforeEs == QLatin1Char('z') || form.endsWith("ches") ||
            form.endsWith("shes"))
        {
            addLemma(lemmas, form.left(form.size() - 2));
        }
        else
        {
            addLemma(lemmas, form.left(form.size() - 1));
        }
    }
    else if (form.size() > 3 && form.endsWith(QLatin1Char('s')))
    {
        addLemma(lemmas, form.left(form.size() - 1));
    }

    if (form.size() > 4 && form.endsWith("ied"))
    {
        const QString stem = form.left(form.size() - 3);
        const QString ieForm = stem + "ie";
        if (ieVerbs().contains(ieForm))
        {
            addLemma(lemmas, ieForm);
        }
        else
        {
            addLemma(lemmas, stem + QLatin1Char('y'));
        }
    }
    else if (form.size() > 4 && form.endsWith("ed"))
    {
        QString stem = form.left(form.size() - 2);
        if (stem.size() > 2 && stem.back() == stem.at(stem.size() - 2) &&
            isAsciiConsonant(stem.back()))
        {
            stem.chop(1);
            addLemma(lemmas, stem);
        }
        else if (shouldPreferFinalE(stem))
        {
            addLemma(lemmas, stem + QLatin1Char('e'), 1);
            if (shouldKeepUnrestoredStemFallback(stem))
            {
                addLemma(lemmas, stem, 2);
            }
        }
        else
        {
            addLemma(lemmas, stem, 1);
            if (!stem.isEmpty() && isAsciiConsonant(stem.back()))
            {
                addLemma(lemmas, stem + QLatin1Char('e'), 2);
            }
        }
    }

    if (form.size() > 4 && form.endsWith("ying"))
    {
        const QString stem = form.left(form.size() - 4);
        const QString ieForm = stem + "ie";
        if (ieVerbs().contains(ieForm))
        {
            addLemma(lemmas, ieForm);
        }
        else
        {
            addLemma(lemmas, stem + QLatin1Char('y'));
        }
    }
    else if (form.size() > 4 && form.endsWith("ing"))
    {
        QString stem = form.left(form.size() - 3);
        if (stem.size() > 2 && stem.back() == stem.at(stem.size() - 2) &&
            isAsciiConsonant(stem.back()))
        {
            stem.chop(1);
            addLemma(lemmas, stem);
        }
        else if (shouldPreferFinalE(stem))
        {
            addLemma(lemmas, stem + QLatin1Char('e'), 1);
            if (shouldKeepUnrestoredStemFallback(stem))
            {
                addLemma(lemmas, stem, 2);
            }
        }
        else
        {
            addLemma(lemmas, stem, 1);
            if (!stem.isEmpty() && isAsciiConsonant(stem.back()))
            {
                addLemma(lemmas, stem + QLatin1Char('e'), 2);
            }
        }
    }

    return lemmas;
}

void addQueries(
    std::vector<SearchQuery> &queries,
    const QHash<QString, int> &lemmas,
    const QString &surface,
    const QString &phraseSuffix = {})
{
    for (auto it = lemmas.cbegin(); it != lemmas.cend(); ++it)
    {
        const QString query = phraseSuffix.isEmpty()
            ? it.key()
            : it.key() + QLatin1Char(' ') + phraseSuffix;
        queries.emplace_back(SearchQuery{
            SearchQuery::Source::english,
            query,
            surface,
            {},
            {},
            it.value(),
        });
    }
}
}

std::vector<SearchQuery> EnglishQueryGenerator::generateQueries(
    const QString &text) const
{
    constexpr qsizetype MAX_PHRASE_WORDS = 5;
    static const QRegularExpression wordExpression{
        "^([A-Za-z]+(?:['’][A-Za-z]+)*)"
    };
    const QRegularExpressionMatch match = wordExpression.match(text);
    if (!match.hasMatch())
    {
        return {};
    }

    const QString surface = match.captured(1);
    if (surface.contains(QLatin1Char('\'')) || surface.contains(QChar(0x2019)))
    {
        return {};
    }

    const QHash<QString, int> lemmas = verbLemmas(surface);
    std::vector<SearchQuery> queries;

    /* Database expression equality is case-sensitive. Preserve the exact
     * candidate from ExactQueryGenerator, and add a lowercase full-token
     * candidate so title-cased subtitle words can match lowercase headings. */
    const QString normalizedSurface = surface.toLower();
    if (normalizedSurface != surface)
    {
        queries.emplace_back(SearchQuery{
            SearchQuery::Source::english,
            normalizedSurface,
            surface,
            {},
            {},
        });
    }

    if (lemmas.isEmpty())
    {
        return queries;
    }

    addQueries(queries, lemmas, surface);

    static const QRegularExpression followingWord{
        "\\G[ \\t]+([A-Za-z]+)"
    };
    qsizetype phraseEnd = match.capturedLength(1);
    QStringList phraseWords;
    for (qsizetype wordCount = 1; wordCount < MAX_PHRASE_WORDS; ++wordCount)
    {
        const QRegularExpressionMatch nextWord =
            followingWord.match(text, phraseEnd);
        if (!nextWord.hasMatch())
        {
            break;
        }
        const QString word = nextWord.captured(1);
        phraseWords.emplaceBack(word.toLower());
        phraseEnd = nextWord.capturedEnd(1);
        addQueries(
            queries,
            lemmas,
            text.left(phraseEnd),
            phraseWords.join(QLatin1Char(' '))
        );
    }

    return queries;
}
