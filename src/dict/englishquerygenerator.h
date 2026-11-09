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

#include "dict/querygenerator.h"

/**
 * @brief Generates English verb lemma and phrase queries from subtitle text.
 *
 * Irregular forms use the shared irregular-verb dataset; regular forms use
 * conservative spelling rules. The original query is retained by the other
 * registered generators.
 */
class EnglishQueryGenerator final : public QueryGenerator
{
public:
    [[nodiscard]]
    inline bool valid() const override
    {
        return true;
    }

    [[nodiscard]]
    std::vector<SearchQuery> generateQueries(
        const QString &text) const override;
};
