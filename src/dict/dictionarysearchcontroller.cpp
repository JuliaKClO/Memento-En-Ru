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

#include "dict/dictionarysearchcontroller.h"

#include <QSet>
#include <QtConcurrentRun>

#include <QDebug>
#include <QStringList>

#ifdef MEMENTO_SYSTEM_QCORO
#include <QCoroFuture>
#else
#include <qcoro/core/qcorofuture.h>
#endif // MEMENTO_SYSTEM_QCORO

#include "dict/deconjugationquerygenerator.h"
#include "dict/englishquerygenerator.h"
#include "dict/exactquerygenerator.h"
#ifdef MEMENTO_MECAB_SUPPORT
#include "dict/mecabquerygenerator.h"
#endif // MEMENTO_MECAB_SUPPORT

/* Begin Constructor/Destructor */

DictionarySearchController::DictionarySearchController(
    Settings *settings, QObject *parent) :
    Dictionary(parent),
    m_settings(settings)
{
    connect(
        m_settings.get(), &Settings::searchMatcherExactChanged,
        this, &DictionarySearchController::updateGenerators,
        Qt::QueuedConnection
    );
    connect(
        m_settings.get(), &Settings::searchMatcherDeconjChanged,
        this, &DictionarySearchController::updateGenerators,
        Qt::QueuedConnection
    );
    connect(
        m_settings.get(), &Settings::searchMatcherEnglishChanged,
        this, &DictionarySearchController::updateGenerators,
        Qt::QueuedConnection
    );
    connect(
        m_settings.get(), &Settings::dictionaryOrderChanged,
        this, &DictionarySearchController::updateDictionaryOrder,
        Qt::QueuedConnection
    );
#ifdef MEMENTO_MECAB_SUPPORT
    connect(
        m_settings.get(), &Settings::searchMatcherMecabIpadicChanged,
        this, &DictionarySearchController::updateGenerators,
        Qt::QueuedConnection
    );
#endif // MEMENTO_MECAB_SUPPORT

    updateGenerators();
    updateDictionaryOrder();
}

DictionarySearchController::~DictionarySearchController()
{
    std::unique_lock lock{m_searchLifetimeMutex};
    m_shuttingDown = true;
    m_noActiveSearches.wait(
        lock,
        [this] () { return m_activeSearches == 0; }
    );
}

/* End Constructor/Destructor */
/* Begin Static Instance Methods */

DictionarySearchController *DictionarySearchController::instance() noexcept
{
    return m_instance;
}

DictionarySearchController *DictionarySearchController::createInstance(
    Settings *settings)
{
    if (m_instance == nullptr)
    {
        m_instance = new DictionarySearchController(settings);
    }
    return m_instance;
}

void DictionarySearchController::destroyInstance()
{
    DictionarySearchController *instance = m_instance;
    m_instance = nullptr;
    delete instance;
    instance = nullptr;
}

/* End Static Instance Methods */
/* Begin Search Methods */

QCoro::Task<QList<Term *>> DictionarySearchController::searchTermsAsync(
    QString query, QString text, qsizetype index)
{
    std::optional<SearchGuard> searchGuard = acquireSearchGuard();
    if (!searchGuard)
    {
        co_return {};
    }

    QList<Term *> terms = co_await QtConcurrent::run(
        [
            this,
            guard = std::move(*searchGuard),
            query = std::move(query),
            text = std::move(text),
            index
        ] () mutable
        {
            return searchTermsSync(std::move(query), std::move(text), index);
        }
    );
    if (!terms.isEmpty() && m_settings != nullptr)
    {
        terms.front()->setAutoPlay(m_settings->searchAutoPlayAudio());
    }

    co_return terms;
}

QList<Term *> DictionarySearchController::searchTermsSync(
    QString query, QString text, qsizetype index)
{
    if (m_shuttingDown)
    {
        return {};
    }

    std::vector<SearchQuery> queries = generateQueries(query);

    if (m_settings != nullptr && m_settings->searchDebugTrace())
    {
        qInfo().noquote() << "[SearchTrace] filtered text:" << text
                          << "index:" << index
                          << "query:" << query;
        for (const SearchQuery &candidate : queries)
        {
            qInfo().noquote()
                << "[SearchTrace] candidate source:"
                << static_cast<int>(candidate.source)
                << "lookup:" << candidate.deconj
                << "surface:" << candidate.surface;
        }
    }

    sortQueries(queries);
    filterDuplicates(queries);

    /* Query the database */
    QList<Term *> terms;
    QHash<const Term *, int> englishLemmaPriorities;
    for (const SearchQuery &query : queries)
    {
        if (m_shuttingDown)
        {
            qDeleteAll(terms);
            terms.clear();
            return {};
        }

        QString err;
        if (m_settings != nullptr && m_settings->searchDebugTrace())
        {
            qInfo().noquote()
                << "[SearchTrace] database lookup:"
                << query.deconj;
        }
        QList<Term *> results = m_db->queryTerms(query.deconj, nullptr, &err);
        if (!err.isEmpty())
        {
            qDeleteAll(results);
            results.clear();
            qWarning("Could not complete query: %s", qUtf8Printable(err));
            return {};
        }

        if (query.ruleFilter.size() > 0)
        {
            QList<Term *> filtered;
            for (Term *term : results)
            {
                bool keep = false;
                for (const TermDefinition *def : term->definitions())
                {
                    QSet<QString> rules{
                        std::begin(def->rules()), std::end(def->rules())
                    };
                    if (rules.intersects(query.ruleFilter))
                    {
                        keep = true;
                        break;
                    }
                }
                if (keep)
                {
                    filtered.emplaceBack(term);
                }
                else
                {
                    delete term;
                }
            }
            results = std::move(filtered);
        }

        if (m_settings != nullptr && m_settings->searchDebugTrace())
        {
            QStringList expressions;
            for (const Term *term : results)
            {
                expressions.emplaceBack(term->expression());
            }
            qInfo().noquote()
                << "[SearchTrace] matching headers for"
                << query.deconj << ':' << expressions;
        }

        QString clozePrefix;
        QString clozeBody;
        QString clozeSuffix;
        if (!results.isEmpty())
        {
            clozePrefix = text.left(index);
            clozeBody = text.mid(index, query.surface.size());
            clozeSuffix = text.right(
                text.size() - (index + query.surface.size())
            );
        }

        for (Term *term : results)
        {
            term->setClozePrefix(clozePrefix);
            term->setClozeBody(clozeBody);
            term->setClozeSuffix(clozeSuffix);
            term->setConjugationExplanation(query.conjugationExplanation);
            if (query.source == SearchQuery::Source::english &&
                query.deconj != query.surface.toLower() &&
                term->expression().compare(
                    query.deconj, Qt::CaseInsensitive
                ) == 0)
            {
                const auto existing = englishLemmaPriorities.constFind(term);
                if (existing == englishLemmaPriorities.cend() ||
                    query.englishCandidatePriority < *existing)
                {
                    englishLemmaPriorities.insert(
                        term, query.englishCandidatePriority
                    );
                }
            }
        }

        terms.append(std::move(results));
    }

    sortTerms(terms, englishLemmaPriorities);

    /* Keep the best-ranked result when several query generators find the
     * same dictionary entry at the same position in the source text. */
    QSet<QString> seenTermKeys;
    QList<Term *> uniqueTerms;
    uniqueTerms.reserve(terms.size());
    for (Term *term : terms)
    {
        QStringList dictionaryIds;
        for (const TermDefinition *definition : term->definitions())
        {
            if (definition->dictionaryInfo())
            {
                dictionaryIds.emplaceBack(
                    QString::number(definition->dictionaryInfo()->id())
                );
            }
        }
        std::sort(dictionaryIds.begin(), dictionaryIds.end());
        dictionaryIds.removeDuplicates();

        QString key;
        const auto appendPart = [&key] (const QString &part)
        {
            key.append(QString::number(part.size()));
            key.append(QLatin1Char(':'));
            key.append(part);
        };
        key.append(QString::number(dictionaryIds.size()));
        key.append(QLatin1Char(';'));
        for (const QString &id : dictionaryIds)
        {
            appendPart(id);
        }
        appendPart(term->expression());
        appendPart(term->reading());
        appendPart(term->clozePrefix());
        appendPart(term->clozeBody());
        appendPart(term->clozeSuffix());

        if (seenTermKeys.contains(key))
        {
            delete term;
            continue;
        }
        seenTermKeys.insert(key);
        uniqueTerms.emplaceBack(term);
    }
    terms = std::move(uniqueTerms);

    if (m_settings != nullptr && m_settings->searchDebugTrace())
    {
        QStringList expressions;
        for (const Term *term : terms)
        {
            expressions.emplaceBack(term->expression());
        }
        qInfo().noquote() << "[SearchTrace] final headers:" << expressions;
    }

    for (Term *term : terms)
    {
        term->moveToThread(thread());
    }

    return terms;
}

QCoro::Task<Kanji *> DictionarySearchController::searchKanjiAsync(
    QString character, QString text, qsizetype index)
{
    std::optional<SearchGuard> searchGuard = acquireSearchGuard();
    if (!searchGuard)
    {
        co_return nullptr;
    }

    Kanji *kanji = co_await QtConcurrent::run(
        [
            this,
            guard = std::move(*searchGuard),
            character = std::move(character),
            text = std::move(text),
            index
        ] () mutable
        {
            return searchKanjiSync(
                std::move(character), std::move(text), index
            );
        }
    );
    co_return kanji;
}

Kanji *DictionarySearchController::searchKanjiSync(
    QString character, QString text, qsizetype index)
{
    if (m_shuttingDown)
    {
        return nullptr;
    }

    QString err;
    Kanji *kanji = m_db->queryKanji(character, nullptr, &err);

    if (kanji == nullptr)
    {
        return nullptr;
    }

    /* Delete the Kanji if there are no definitions left */
    if (kanji->definitions().isEmpty())
    {
        delete kanji;
        return nullptr;
    }

    /* Add information */
    kanji->setClozePrefix(text.left(index));
    kanji->setClozeBody(text.mid(index, character.size()));
    kanji->setClozeSuffix(text.right(
        text.size() - (index + character.size())
    ));

    /* Sort all the information */
    m_dictionaryOrderMutex.lockForRead();
    std::sort(
        std::begin(kanji->m_frequencies), std::end(kanji->m_frequencies),
        [this] (const Frequency *lhs, const Frequency *rhs) -> bool
        {
            return m_dictionaryOrder[lhs->dictionaryInfo()->id()] <
                   m_dictionaryOrder[rhs->dictionaryInfo()->id()];
        }
    );
    std::sort(
        std::begin(kanji->m_definitions), std::end(kanji->m_definitions),
        [this] (const KanjiDefinition *lhs, const KanjiDefinition *rhs) -> bool
        {
            return m_dictionaryOrder[lhs->dictionaryInfo()->id()] <
                   m_dictionaryOrder[rhs->dictionaryInfo()->id()];
        }
    );
    m_dictionaryOrderMutex.unlock();
    for (KanjiDefinition *def : kanji->m_definitions)
    {
        sortTags(def->m_tags);
    }

    kanji->moveToThread(thread());

    return kanji;
}

/* End Search Methods */
/* Begin Settings Handlers */

void DictionarySearchController::updateGenerators()
{
    if (m_settings == nullptr)
    {
        return;
    }

    QWriteLocker lock{&m_generatorsMutex};

    m_generators.clear();

    if (m_settings->searchMatcherExact())
    {
        m_generators.emplace_back(std::make_unique<ExactQueryGenerator>());
    }
    if (m_settings->searchMatcherDeconj())
    {
        m_generators.emplace_back(
            std::make_unique<DeconjugationQueryGenerator>()
        );
    }
    if (m_settings->searchMatcherEnglish())
    {
        m_generators.emplace_back(std::make_unique<EnglishQueryGenerator>());
    }
#ifdef MEMENTO_MECAB_SUPPORT
    if (m_settings->searchMatcherMecabIpadic())
    {
        m_generators.emplace_back(std::make_unique<MeCabQueryGenerator>());
    }
#endif // MEMENTO_MECAB_SUPPORT
}

void DictionarySearchController::updateDictionaryOrder()
{
    if (m_settings == nullptr)
    {
        return;
    }

    QWriteLocker lock{&m_dictionaryOrderMutex};

    m_dictionaryOrder.clear();
    const QList<int64_t> &order = m_settings->dictionaryOrder();
    for (qsizetype i{0}; i < order.size(); ++i)
    {
        m_dictionaryOrder.emplace(order[i], i);
    }
}

/* End Settings Handlers */
/* Begin Search Helpers */

std::vector<SearchQuery> DictionarySearchController::generateQueries(
    const QString &text) const
{
    QReadLocker lock{&m_generatorsMutex};

    std::vector<SearchQuery> queries;
    for (const std::unique_ptr<QueryGenerator> &gen : m_generators)
    {
        std::vector<SearchQuery> currQueries = gen->generateQueries(text);
        queries.insert(
            std::end(queries),
            std::make_move_iterator(std::begin(currQueries)),
            std::make_move_iterator(std::end(currQueries))
        );
    }
    return queries;
}

void DictionarySearchController::sortQueries(std::vector<SearchQuery> &queries)
{
    const auto sourcePriority = [] (SearchQuery::Source source)
    {
        switch (source)
        {
            case SearchQuery::Source::exact:
                return 0;
            case SearchQuery::Source::deconj:
                return 1;
            case SearchQuery::Source::english:
                return 2;
            case SearchQuery::Source::mecab:
                return 3;
        }
        return 4;
    };
    std::sort(
        std::begin(queries), std::end(queries),
        [&sourcePriority] (const SearchQuery &lhs, const SearchQuery &rhs)
        {
            if (lhs.deconj == rhs.deconj)
            {
                if (lhs.surface.size() != rhs.surface.size())
                {
                    return lhs.surface.size() > rhs.surface.size();
                }
                return sourcePriority(lhs.source) <
                    sourcePriority(rhs.source);
            }
            return lhs.deconj > rhs.deconj;
        }
    );
}

void DictionarySearchController::filterDuplicates(
    std::vector<SearchQuery> &queries)
{
#ifdef MEMENTO_MECAB_SUPPORT
    /* Remove all duplicates that both MeCab and the deconjugator got.
     * Prefer deconjugator results over MeCab. */
    QSet<QString> deconjQueries;
    for (const SearchQuery &query : queries)
    {
        if (query.source == SearchQuery::Source::deconj)
        {
            deconjQueries.insert(query.deconj);
        }
    }
    queries.erase(
        std::remove_if(
            std::begin(queries), std::end(queries),
            [&deconjQueries] (const SearchQuery &query) -> bool
            {
                switch (query.source)
                {
                    case SearchQuery::Source::mecab:
                        return deconjQueries.contains(query.deconj);

                    case SearchQuery::Source::deconj:
                    case SearchQuery::Source::exact:
                    case SearchQuery::Source::english:
                        return false;
                }
                return false;
            }
        ),
        std::end(queries)
    );
#endif // MEMENTO_MECAB_SUPPORT

    auto last = std::unique(
        std::begin(queries), std::end(queries),
        [] (const SearchQuery &lhs, const SearchQuery &rhs) -> bool
        {
            return lhs.deconj == rhs.deconj && lhs.ruleFilter == rhs.ruleFilter;
        }
    );
    queries.erase(last, std::end(queries));

    std::erase_if(
        queries,
        [] (const SearchQuery &query) -> bool
        {
            return query.deconj.isEmpty();
        }
    );
}

void DictionarySearchController::sortTerms(
    QList<Term *> &terms,
    const QHash<const Term *, int> &englishLemmaPriorities) const
{
    QReadLocker dictionaryOrderLock{&m_dictionaryOrderMutex};
    const auto dictionaryPriority = [this] (const Term *term)
    {
        const QList<TermDefinition *> &definitions = term->definitions();
        if (definitions.isEmpty() ||
            definitions.front()->dictionaryInfo() == nullptr)
        {
            return m_dictionaryOrder.size();
        }
        return m_dictionaryOrder.value(
            definitions.front()->dictionaryInfo()->id(),
            m_dictionaryOrder.size()
        );
    };

    std::sort(
        std::begin(terms), std::end(terms),
        [&englishLemmaPriorities, &dictionaryPriority]
        (const Term *lhs, const Term *rhs) -> bool
        {
            if (lhs->clozeBody().size() != rhs->clozeBody().size())
            {
                return lhs->clozeBody().size() > rhs->clozeBody().size();
            }
            const bool lhsExact = lhs->expression().compare(
                lhs->clozeBody(), Qt::CaseInsensitive
            ) == 0;
            const bool rhsExact = rhs->expression().compare(
                rhs->clozeBody(), Qt::CaseInsensitive
            ) == 0;
            if (lhsExact != rhsExact)
            {
                return lhsExact;
            }
            const int lhsEnglishPriority =
                englishLemmaPriorities.value(lhs, 100);
            const int rhsEnglishPriority =
                englishLemmaPriorities.value(rhs, 100);
            if (lhsEnglishPriority != rhsEnglishPriority)
            {
                return lhsEnglishPriority < rhsEnglishPriority;
            }
            if (lhs->expression().size() != rhs->expression().size())
            {
                return lhs->expression().size() > rhs->expression().size();
            }
            if (lhs->conjugationExplanation().isEmpty() !=
                rhs->conjugationExplanation().isEmpty())
            {
                return lhs->conjugationExplanation().size() >
                    rhs->conjugationExplanation().size();
            }
            const qsizetype lhsPriority = dictionaryPriority(lhs);
            const qsizetype rhsPriority = dictionaryPriority(rhs);
            if (lhsPriority != rhsPriority)
            {
                return lhsPriority < rhsPriority;
            }
            return lhs->score() > rhs->score();
        }
    );

    for (Term *term : terms)
    {
        std::sort(
            std::begin(term->m_definitions), std::end(term->m_definitions),
            [this]
            (const TermDefinition *lhs, const TermDefinition *rhs) -> bool
            {
                qsizetype lhsPriority =
                    m_dictionaryOrder[lhs->dictionaryInfo()->id()];
                qsizetype rhsPriority =
                m_dictionaryOrder[rhs->dictionaryInfo()->id()];
                return lhsPriority < rhsPriority ||
                    (
                        lhsPriority == rhsPriority &&
                        lhs->score() > rhs->score()
                    );
            }
        );
        std::sort(
            std::begin(term->m_frequencies), std::end(term->m_frequencies),
            [this] (const Frequency *lhs, const Frequency *rhs) -> bool
            {
                return m_dictionaryOrder[lhs->dictionaryInfo()->id()] <
                       m_dictionaryOrder[rhs->dictionaryInfo()->id()];
            }
        );
        sortTags(term->m_tags);
        for (TermDefinition *def : term->definitions())
        {
            sortTags(def->m_tags);
        }
    }
}

void DictionarySearchController::sortTags(QList<Tag *> &tags) const
{
    std::sort(
        std::begin(tags), std::end(tags),
        [] (const Tag *lhs, const Tag *rhs) -> bool
        {
            return lhs->order() < rhs->order() ||
                (lhs->order() == rhs->order() && lhs->score() > rhs->score());
        }
    );
}

/* End Search Helpers */
/* Begin Search Guard */

DictionarySearchController::SearchGuard::SearchGuard(
    DictionarySearchController *controller) noexcept :
    m_controller(controller)
{

}

DictionarySearchController::SearchGuard::SearchGuard(
    SearchGuard &&other) noexcept :
    m_controller(std::exchange(other.m_controller, nullptr))
{

}

DictionarySearchController::SearchGuard &
DictionarySearchController::SearchGuard::operator=(
    SearchGuard &&other) noexcept
{
    if (this != &other)
    {
        reset();
        m_controller = std::exchange(other.m_controller, nullptr);
    }
    return *this;
}

DictionarySearchController::SearchGuard::~SearchGuard()
{
    reset();
}

void DictionarySearchController::SearchGuard::reset() noexcept
{
    if (m_controller == nullptr)
    {
        return;
    }
    DictionarySearchController *controller = m_controller;
    m_controller = nullptr;
    controller->releaseSearchGuard();
}

std::optional<DictionarySearchController::SearchGuard>
DictionarySearchController::acquireSearchGuard()
{
    std::lock_guard lock{m_searchLifetimeMutex};
    if (m_shuttingDown)
    {
        return std::nullopt;
    }
    ++m_activeSearches;
    return SearchGuard{this};
}

void DictionarySearchController::releaseSearchGuard() noexcept
{
    std::lock_guard lock{m_searchLifetimeMutex};
    --m_activeSearches;
    if (m_activeSearches == 0)
    {
        m_noActiveSearches.notify_all();
    }
}

/* End Search Guard */
