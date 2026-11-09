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
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <QString>

/**
 * @brief Resolves LingvoUniversal British and American recordings stored in a
 * SoundEn.lsa archive.
 *
 * LSA contains a single Vorbis stream and an index of PCM sample ranges. This
 * helper decodes only the selected range into a cached WAV file.
 */
namespace LingvoLsaAudio
{

/**
 * @brief Create a virtual Memento URL for one Lingvo recording.
 *
 * Returns an empty string if the archive or dictionary mapping does not have
 * the requested recording.
 */
[[nodiscard]]
QString makeUrl(
    const QString &archivePath,
    const QString &expression,
    const QString &accent);

/** @brief Check whether a URL is a Memento Lingvo LSA URL. */
[[nodiscard]]
bool isVirtualUrl(const QString &url);

/** @brief Decode a virtual LSA URL to a cached standalone WAV. */
[[nodiscard]]
QString materialize(const QString &url);

} // namespace LingvoLsaAudio
