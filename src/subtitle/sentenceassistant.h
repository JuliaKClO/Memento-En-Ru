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

#pragma once

#include <QObject>
#include <QStringList>

/**
 * @brief Collects a sentence around a subtitle word and formats its prompt.
 */
class SentenceAssistant : public QObject
{
    Q_OBJECT

public:
    explicit SentenceAssistant(QObject *parent = nullptr);

    /**
     * @brief Collect a sentence around a cursor in one subtitle.
     *
     * Up to five neighboring subtitles on either side are considered. The
     * result is capped to a bounded context, and punctuation boundaries are
     * used to avoid including adjacent complete sentences.
     *
     * @param subtitles Ordered subtitle texts.
     * @param currentSubtitleIndex Index of the subtitle under the cursor.
     * @param cursorIndex Character offset in that subtitle.
     * @return The sentence containing the cursor, or the available fragment.
     */
    Q_INVOKABLE QString collectSentence(
        const QStringList &subtitles,
        int currentSubtitleIndex,
        int cursorIndex) const;

    /**
     * @brief Insert a sentence into the user's AI prompt template.
     * @param sentence The collected English sentence.
     * @param promptTemplate User-editable prompt template.
     * @return The complete prompt, or an empty string for empty input.
     */
    Q_INVOKABLE QString buildPrompt(
        const QString &sentence, const QString &promptTemplate) const;
};
