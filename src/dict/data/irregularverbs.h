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

#include <QString>
#include <QStringList>

/**
 * @brief Shared English irregular-verb data used by Lingvo cards and search.
 */
class IrregularVerbForms final
{
public:
    /**
     * @brief Returns whether the word is a base form in the irregular-verb
     * dataset.
     */
    [[nodiscard]]
    static bool isBaseVerb(const QString &word);

    /**
     * @brief Returns lemmas whose past or past-participle form matches a
     * surface form.
     */
    [[nodiscard]]
    static QStringList lemmasForForm(const QString &form);

    /**
     * @brief Returns the base, past, and past-participle forms for a lemma.
     */
    [[nodiscard]]
    static QStringList formsForLemma(const QString &lemma);
};
