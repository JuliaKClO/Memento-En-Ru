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

#include "dict/data/irregularverbs.h"
#include "dict/data/irregularverbsdata.h"

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

namespace
{
struct VerbForms
{
    QStringList past;
    QStringList participle;
};

const QHash<QString, VerbForms> &verbForms()
{
    static const QHash<QString, VerbForms> forms = [] {
        const QJsonDocument document = QJsonDocument::fromJson(
            QByteArray::fromRawData(
                IRREGULAR_VERBS_JSON,
                qsizetype(sizeof(IRREGULAR_VERBS_JSON) - 1)
            )
        );
        const QJsonObject entries = document.object();
        QHash<QString, VerbForms> result;
        result.reserve(entries.size());
        for (auto it = entries.begin(); it != entries.end(); ++it)
        {
            const QJsonObject entry = it.value().toObject();
            const auto readForms = [](const QJsonValue &value) {
                QStringList result;
                for (const QJsonValue &form : value.toArray())
                {
                    const QString normalized = form.toString().toLower();
                    if (!normalized.isEmpty() && !result.contains(normalized))
                    {
                        result.append(normalized);
                    }
                }
                return result;
            };
            result.insert(
                it.key().toLower(),
                VerbForms{
                    readForms(entry.value(QStringLiteral("past"))),
                    readForms(entry.value(QStringLiteral("participle"))),
                }
            );
        }
        return result;
    }();
    return forms;
}

const QSet<QString> &knownRegularEntries()
{
    // These are present in the upstream broad list but only have regular
    // spellings in the published forms. Do not label them irregular or show
    // a redundant table when the dictionary has no form metadata.
    static const QSet<QString> values{
        "belay", "crash-dive", "hoist", "outsmell", "re-prove", "wont",
        "zinc",
    };
    return values;
}

const QSet<QString> &excludedFromIrregularTable()
{
    // Learn is a regular verb for the app's card display. Keep learnt/learned
    // in the lookup index, but don't label the lemma as irregular or show a
    // three-form table for it.
    static const QSet<QString> values{ "learn" };
    return values;
}

const QHash<QString, QStringList> &lemmasByForm()
{
    static const QHash<QString, QStringList> index = [] {
        QHash<QString, QStringList> result;
        for (auto it = verbForms().cbegin(); it != verbForms().cend(); ++it)
        {
            if (knownRegularEntries().contains(it.key()))
            {
                continue;
            }
            const QString lemma = it.key();
            const auto indexForms = [&result, &lemma] (const QStringList &forms) {
                for (const QString &form : forms)
                {
                    if (form == lemma)
                    {
                        continue;
                    }
                    QStringList &lemmas = result[form];
                    if (!lemmas.contains(lemma))
                    {
                        lemmas.append(lemma);
                    }
                }
            };
            indexForms(it->past);
            indexForms(it->participle);
        }
        return result;
    }();
    return index;
}
}

bool IrregularVerbForms::isBaseVerb(const QString &word)
{
    const QString normalized = word.trimmed().toLower();
    return !knownRegularEntries().contains(normalized) &&
        !excludedFromIrregularTable().contains(normalized) &&
        verbForms().contains(normalized);
}

QStringList IrregularVerbForms::lemmasForForm(const QString &form)
{
    return lemmasByForm().value(form.trimmed().toLower());
}

QStringList IrregularVerbForms::formsForLemma(const QString &lemma)
{
    const QString normalized = lemma.trimmed().toLower();
    if (knownRegularEntries().contains(normalized) ||
        excludedFromIrregularTable().contains(normalized))
    {
        return {};
    }
    const auto it = verbForms().constFind(normalized);
    if (it == verbForms().cend() || it->past.isEmpty() ||
        it->participle.isEmpty())
    {
        return {};
    }

    const auto joinedForms = [](const QStringList &forms) {
        QStringList distinct;
        for (const QString &form : forms)
        {
            if (!form.isEmpty() && !distinct.contains(form))
            {
                distinct.append(form);
            }
        }
        return distinct.join(QStringLiteral(" / "));
    };

    return {
        normalized,
        joinedForms(it->past),
        joinedForms(it->participle),
    };
}
