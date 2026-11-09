#include "subtitle/sentenceassistant.h"

#include <QCoreApplication>

#include <iostream>

namespace
{
int failures = 0;

void expect(bool condition, const char *name)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << name << '\n';
        ++failures;
    }
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    SentenceAssistant assistant;

    const QString oneCue = "I grew up in London. Then I moved away.";
    expect(
        assistant.collectSentence({oneCue}, 0, oneCue.indexOf("grew")) ==
            "I grew up in London.",
        "select the complete sentence from one subtitle"
    );

    expect(
        assistant.collectSentence(
            {"I grew", "up in London."}, 0, 3
        ) == "I grew up in London.",
        "join two subtitle cues without treating the cue edge as punctuation"
    );

    expect(
        assistant.collectSentence(
            {"I thought", "this would", "never happen!"}, 1, 5
        ) == "I thought this would never happen!",
        "join three subtitle cues"
    );

    const QString twoSentences = "First sentence ends. Mr. Smith arrived.";
    expect(
        assistant.collectSentence(
            {twoSentences}, 0, twoSentences.indexOf("Smith")
        ) == "Mr. Smith arrived.",
        "select the sentence under the cursor and keep Mr. abbreviation"
    );

    expect(
        assistant.collectSentence({"Only a fragment"}, 0, 5) ==
            "Only a fragment",
        "use a subtitle fragment when no neighboring subtitles exist"
    );

    expect(
        assistant.collectSentence(
            {"This unfinished", "sentence continues"}, 0, 5
        ) == "This unfinished sentence continues",
        "include available context for an unfinished sentence"
    );

    const QString longCaption(1600, u'a');
    const QString capped = assistant.collectSentence(
        {longCaption}, 0, 1350
    );
    expect(
        !capped.isEmpty() && capped.size() <= 1200 &&
            capped.size() < longCaption.size(),
        "cap an overlong subtitle context"
    );

    const QString sentence = "She grew up in London.";
    const QString defaultPrompt = assistant.buildPrompt(sentence, {});
    expect(
        defaultPrompt.contains(QStringLiteral("Дай перевод,")) &&
            defaultPrompt.contains(
                QStringLiteral(
                    "объясни простым языком грамматику и лексику следующего предложения"
                )
            ),
        "keep the current Russian product default prompt"
    );
    expect(
        defaultPrompt.contains(sentence),
        "include the sentence in the default prompt"
    );
    expect(
        !defaultPrompt.contains("{{SENTENCE}}"),
        "replace the default prompt sentence placeholder"
    );

    const QString customTemplate =
        QStringLiteral("Analyze: {{SENTENCE}}\nRepeat: {{SENTENCE}}");
    const QString customPrompt = assistant.buildPrompt(sentence, customTemplate);
    expect(
        customPrompt.contains(QStringLiteral("Analyze: ") + sentence) &&
            customPrompt.contains(QStringLiteral("Repeat: ") + sentence),
        "replace each sentence marker in a custom prompt"
    );
    expect(
        !customPrompt.contains("{{SENTENCE}}"),
        "remove all sentence markers after custom prompt substitution"
    );

    const QString noMarkerPrompt = assistant.buildPrompt(
        sentence, QStringLiteral("Translate and explain the sentence.")
    );
    expect(
        noMarkerPrompt.contains(sentence),
        "append the sentence when a custom prompt has no marker"
    );
    expect(
        noMarkerPrompt.contains(QStringLiteral("**Предложение для разбора:**")),
        "label the appended sentence when a custom prompt has no marker"
    );
    expect(
        assistant.buildPrompt({}, {}).isEmpty(),
        "do not generate a prompt without a sentence"
    );

    if (failures == 0)
    {
        std::cout << "All sentence assistant tests passed.\n";
        return 0;
    }
    std::cerr << failures << " sentence assistant test(s) failed.\n";
    return 1;
}
