#include "dict/englishquerygenerator.h"
#include "dict/exactquerygenerator.h"
#include "dict/data/irregularverbs.h"

#include <QCoreApplication>

#include <iostream>

namespace
{
bool containsQuery(
    const std::vector<SearchQuery> &queries,
    const QString &lookup,
    const QString &surface = {})
{
    for (const SearchQuery &query : queries)
    {
        if (query.deconj == lookup &&
            (surface.isEmpty() || query.surface == surface))
        {
            return true;
        }
    }
    return false;
}

bool containsCompactCandidate(
    const std::vector<SearchQuery> &queries, const QString &candidate)
{
    for (const SearchQuery &query : queries)
    {
        if (query.deconj == candidate)
        {
            return true;
        }
    }
    return false;
}

int failures = 0;

void expect(bool condition, const char *name)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << name << '\n';
        ++failures;
    }
}

void expectLemma(
    const EnglishQueryGenerator &generator,
    const QString &text,
    const QString &lemma,
    const QString &surface)
{
    const std::vector<SearchQuery> queries = generator.generateQueries(text);
    expect(
        containsQuery(queries, lemma, surface),
        qPrintable(text + " -> " + lemma)
    );
    for (const SearchQuery &query : queries)
    {
        expect(
            query.source == SearchQuery::Source::english,
            "English generator labels its own query source"
        );
    }
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    EnglishQueryGenerator english;
    ExactQueryGenerator exact;

    expectLemma(english, "grew", "grow", "grew");
    expectLemma(english, "grew up in London", "grow", "grew");
    expectLemma(english, "grew up in London", "grow up", "grew up");
    expectLemma(english, "Grew up in London", "grow up", "Grew up");
    expectLemma(english, "grown up.", "grow up", "grown up");
    expectLemma(english, "growing up fast", "grow up", "growing up");
    expectLemma(english, "went home", "go", "went");
    expectLemma(english, "bit the bullet", "bite the bullet", "bit the bullet");
    expectLemma(english, "Bit the bullet", "bite the bullet", "Bit the bullet");
    expectLemma(english, "Break a leg", "break a leg", "Break a leg");
    expectLemma(english, "break a leg!", "break a leg", "break a leg");
    expect(
        IrregularVerbForms::formsForLemma("know") ==
            QStringList{ "know", "knew", "known" },
        "the shared irregular-verb list provides know's complete forms"
    );
    expect(
        IrregularVerbForms::formsForLemma("be") ==
            QStringList{ "be", "was / were", "been" },
        "the shared irregular-verb list retains all be forms"
    );
    expect(
        IrregularVerbForms::formsForLemma("get") ==
            QStringList{ "get", "got", "got / gotten" },
        "the shared irregular-verb list retains alternative participles"
    );
    expect(
        IrregularVerbForms::formsForLemma("learn").isEmpty() &&
            !IrregularVerbForms::isBaseVerb("learn") &&
            IrregularVerbForms::lemmasForForm("learnt").contains("learn"),
        "learn is treated as regular while its learnt spelling remains searchable"
    );
    expect(
        IrregularVerbForms::isBaseVerb("break") &&
            IrregularVerbForms::isBaseVerb("bite") &&
            !IrregularVerbForms::isBaseVerb("zinc"),
        "the shared list recognizes irregular bases and filters regular entries"
    );
    expectLemma(english, "gone home", "go", "gone");
    expectLemma(english, "was here", "be", "was");
    expectLemma(english, "did it", "do", "did");
    expectLemma(english, "gave up smoking", "give up", "gave up");
    expectLemma(english, "given up", "give up", "given up");
    expectLemma(english, "took off his shoes", "take off", "took off");
    expectLemma(english, "taken off", "take off", "taken off");
    expectLemma(english, "turned up", "turn up", "turned up");
    expectLemma(english, "looked up at the station", "look", "looked");
    expectLemma(english, "looked up at the station", "look up", "looked up");
    expectLemma(english, "turned the light off", "turn", "turned");
    expectLemma(english, "stopped", "stop", "stopped");
    expectLemma(english, "running", "run", "running");
    expectLemma(english, "sitting", "sit", "sitting");
    expectLemma(english, "making", "make", "making");
    expectLemma(english, "becoming", "become", "becoming");
    expectLemma(english, "studied", "study", "studied");
    expectLemma(english, "hoped", "hope", "hoped");
    expectLemma(english, "hated", "hate", "hated");
    expectLemma(english, "hating", "hate", "hating");
    expectLemma(english, "dated", "date", "dated");
    expectLemma(english, "dating", "date", "dating");
    expectLemma(english, "stated", "state", "stated");
    expectLemma(english, "tasting", "taste", "tasting");
    expectLemma(english, "embraced", "embrace", "embraced");
    expectLemma(english, "breathing heavily", "breathe", "breathing");
    expectLemma(
        english,
        "breathing heavily",
        "breathe heavily",
        "breathing heavily"
    );
    expectLemma(english, "breathing in", "breathe in", "breathing in");
    expectLemma(english, "breathed deeply", "breathe", "breathed");
    expectLemma(english, "breathes slowly", "breathe", "breathes");
    expectLemma(english, "creating", "create", "creating");
    expectLemma(english, "inviting", "invite", "inviting");
    expectLemma(english, "prepared", "prepare", "prepared");
    expectLemma(english, "imagining", "imagine", "imagining");
    expect(
        containsQuery(
            english.generateQueries("breathing heavily"), "breath", "breathing"
        ),
        "breath remains available as a lower-priority fallback"
    );
    expectLemma(english, "Greedy", "greedy", "Greedy");
    expectLemma(english, "With", "with", "With");
    expectLemma(english, "fell into place", "fall", "fell");
    expectLemma(english, "fell into place", "fall into", "fell into");
    expectLemma(english, "fell into place", "fall into place", "fell into place");
    expect(
        !containsQuery(english.generateQueries("hoped"), "hop", "hoped"),
        "final-e restoration does not also propose the unrelated hop root"
    );
    expectLemma(english, "hoped", "hope", "hoped");
    expectLemma(english, "hating", "hate", "hating");
    expectLemma(english, "dated", "date", "dated");
    expectLemma(english, "dating", "date", "dating");
    expectLemma(english, "loved", "love", "loved");
    expectLemma(english, "loving", "love", "loving");
    expectLemma(english, "raced", "race", "raced");
    expectLemma(english, "racing", "race", "racing");
    expectLemma(english, "closed", "close", "closed");
    expectLemma(english, "closing", "close", "closing");
    expectLemma(english, "created", "create", "created");
    expectLemma(english, "creating", "create", "creating");
    expect(
        !containsQuery(english.generateQueries("hating"), "hat", "hating") &&
            !containsQuery(english.generateQueries("dated"), "dat", "dated") &&
            !containsQuery(english.generateQueries("loved"), "lov", "loved") &&
            !containsQuery(english.generateQueries("racing"), "rac", "racing") &&
            !containsQuery(english.generateQueries("closed"), "clos", "closed") &&
            !containsQuery(english.generateQueries("creating"), "creat", "creating"),
        "final-e candidates omit the unattested bare stem across spelling rules"
    );
    expectLemma(english, "hopped", "hop", "hopped");
    expectLemma(english, "hopping", "hop", "hopping");
    expect(
        !containsQuery(english.generateQueries("hopped"), "hope", "hopped") &&
            !containsQuery(english.generateQueries("hopping"), "hope", "hopping"),
        "doubled consonants retain the non-final-e verb root"
    );
    expect(
        containsQuery(english.generateQueries("breathing"), "breath", "breathing"),
        "the documented breath fallback remains available"
    );
    expect(
        !containsQuery(english.generateQueries("fell up, quickly"), "fall up, quickly") &&
            !containsQuery(english.generateQueries("grew up, London"), "grow up, London"),
        "phrase extension stops at punctuation and does not join sentence fragments"
    );

    expect(
        english.generateQueries("better yesterday").empty(),
        "ambiguous better remains an exact-only lookup"
    );
    expect(
        english.generateQueries("saw the boat").empty(),
        "ambiguous saw remains an exact-only lookup"
    );
    expect(
        english.generateQueries("news headline").empty() &&
            english.generateQueries("things changed").empty() &&
            english.generateQueries("morning light").empty(),
        "common non-verbal words do not receive suffix guesses"
    );
    expect(
        english.generateQueries("don't go").empty(),
        "contractions are not parsed as inflected verbs"
    );
    expect(
        english.generateQueries("up at").empty(),
        "preposition up is not normalized as a verb in up at"
    );
    expect(
        english.generateQueries("日本語 English words").empty(),
        "English morphology does not start on Japanese text"
    );
    expect(
        english.generateQueries("日本語のEnglish字幕").empty(),
        "mixed text is unchanged when query begins with Japanese"
    );

    const std::vector<SearchQuery> upAt = exact.generateQueries("up at");
    expect(
        containsCompactCandidate(upAt, "up at"),
        "exact query preserves the spaced up at candidate"
    );
    expect(
        !containsCompactCandidate(upAt, "upat") &&
            !containsCompactCandidate(upAt, "pat"),
        "Latin neighboring words are not compacted"
    );
    const std::vector<SearchQuery> turnUp = exact.generateQueries("turn up");
    expect(
        containsCompactCandidate(turnUp, "turn up") &&
            !containsCompactCandidate(turnUp, "turnup"),
        "exact English phrasal query retains its space"
    );
    const std::vector<SearchQuery> lookedUpAt =
        exact.generateQueries("looked up at the station");
    expect(
        containsCompactCandidate(lookedUpAt, "looked up at the station") &&
            !containsCompactCandidate(lookedUpAt, "lookedupatthestation") &&
            !containsCompactCandidate(lookedUpAt, "pat"),
        "English words in a sentence are not joined"
    );

    const std::vector<SearchQuery> japanese = exact.generateQueries("日本 語");
    expect(
        containsCompactCandidate(japanese, "日本語"),
        "Japanese whitespace compact candidate remains available"
    );
    const std::vector<SearchQuery> mixed =
        exact.generateQueries("日本語 English words");
    expect(
        !containsCompactCandidate(mixed, "日本語English") &&
            !containsCompactCandidate(mixed, "Englishwords"),
        "mixed Japanese/English words are not joined"
    );
    const std::vector<SearchQuery> mixedJapaneseSpacing =
        exact.generateQueries("日本 語 English words");
    expect(
        containsCompactCandidate(mixedJapaneseSpacing, "日本語 English words"),
        "Japanese spacing is compacted without removing the mixed-language boundary"
    );
    expect(
        !containsCompactCandidate(mixedJapaneseSpacing, "日本語English words") &&
            !containsCompactCandidate(mixedJapaneseSpacing, "日本語 Englishwords"),
        "compaction does not cross Japanese/Latin boundary or join English words"
    );

    if (failures == 0)
    {
        std::cout << "All English query generator tests passed.\n";
        return 0;
    }
    std::cerr << failures << " test(s) failed.\n";
    return 1;
}
