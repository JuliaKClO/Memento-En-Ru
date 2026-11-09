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

#include "audio/lingvolsaaudio.h"

#include <vorbis/vorbisfile.h>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>

namespace {

constexpr auto MANIFEST_RESOURCE = ":/audio/lingvo_audio_manifest.tsv";
constexpr auto URL_SCHEME = "memento-lsa";
constexpr quint32 MAX_SAMPLE_COUNT = 10'000'000;
constexpr qsizetype DECODE_CHUNK_SIZE = 16 * 1024;

struct LsaEntry
{
    QString name;
    quint32 sampleOffset{};
    quint32 sampleCount{};
};

struct ArchiveIndex
{
    QString canonicalPath;
    qint64 fileSize{};
    qint64 modifiedTime{};
    qint64 oggBase{};
    QHash<QString, LsaEntry> entries;
};

struct AudioIds
{
    QString uk;
    QString us;
};

struct LsaRequest
{
    QString archivePath;
    QString expression;
    QString accent;
};

struct ShiftedFile
{
    QFile *file{};
    qint64 oggBase{};
};

QMutex &indexMutex()
{
    static QMutex mutex;
    return mutex;
}

QHash<QString, std::shared_ptr<const ArchiveIndex>> &indexCache()
{
    static QHash<QString, std::shared_ptr<const ArchiveIndex>> cache;
    return cache;
}

QString normalizedExpression(const QString &expression)
{
    return expression.normalized(QString::NormalizationForm_C)
        .toLower()
        .simplified();
}

const QHash<QString, AudioIds> &audioManifest()
{
    static const QHash<QString, AudioIds> manifest = [] {
        QHash<QString, AudioIds> result;
        QFile file(QString::fromLatin1(MANIFEST_RESOURCE));
        if (!file.open(QIODevice::ReadOnly))
        {
            qWarning("LingvoLsaAudio: Could not read audio manifest from %s",
                     qUtf8Printable(file.fileName()));
            return result;
        }

        while (!file.atEnd())
        {
            const QString line = QString::fromUtf8(file.readLine()).trimmed();
            if (line.isEmpty())
            {
                continue;
            }

            const QStringList fields = line.split(QLatin1Char('\t'));
            if (fields.size() < 3)
            {
                continue;
            }
            result.insert(
                normalizedExpression(fields[0]),
                AudioIds{
                    .uk = fields[1].trimmed().toLower(),
                    .us = fields[2].trimmed().toLower()
                }
            );
        }
        return result;
    }();
    return manifest;
}

bool readExact(QFile &file, char *buffer, qsizetype length)
{
    qsizetype read = 0;
    while (read < length)
    {
        const qint64 count = file.read(buffer + read, length - read);
        if (count <= 0)
        {
            return false;
        }
        read += static_cast<qsizetype>(count);
    }
    return true;
}

bool readU8(QFile &file, quint8 &value)
{
    char byte = 0;
    if (!readExact(file, &byte, 1))
    {
        return false;
    }
    value = static_cast<quint8>(static_cast<unsigned char>(byte));
    return true;
}

bool readU32Le(QFile &file, quint32 &value)
{
    std::array<uchar, 4> bytes{};
    if (!readExact(file, reinterpret_cast<char *>(bytes.data()), 4))
    {
        return false;
    }
    value = qFromLittleEndian<quint32>(bytes.data());
    return true;
}

bool readLsaString(QFile &file, QString &value)
{
    value.clear();
    for (qsizetype i = 0; i < 4096; ++i)
    {
        quint8 low = 0;
        if (!readU8(file, low))
        {
            return false;
        }
        if (low == 0xFF)
        {
            return true;
        }

        quint8 high = 0;
        if (!readU8(file, high))
        {
            return false;
        }
        if (high == 0xFF)
        {
            return true;
        }
        value.append(QChar(static_cast<ushort>(low | (high << 8))));
    }
    return false;
}

std::shared_ptr<const ArchiveIndex> readArchiveIndex(const QString &archivePath)
{
    QFileInfo info(archivePath);
    if (!info.isFile() || !info.isReadable())
    {
        return {};
    }

    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly))
    {
        return {};
    }

    QString signature;
    quint32 entryCount = 0;
    if (!readLsaString(file, signature) || signature != QStringLiteral("L9SA") ||
        !readU32Le(file, entryCount) || entryCount == 0 || entryCount > 1'000'000)
    {
        qWarning("LingvoLsaAudio: Invalid LSA index in %s",
                 qUtf8Printable(info.absoluteFilePath()));
        return {};
    }

    auto index = std::make_shared<ArchiveIndex>();
    index->canonicalPath = info.canonicalFilePath();
    if (index->canonicalPath.isEmpty())
    {
        index->canonicalPath = info.absoluteFilePath();
    }
    index->fileSize = info.size();
    index->modifiedTime = info.lastModified().toMSecsSinceEpoch();
    index->entries.reserve(static_cast<qsizetype>(entryCount));

    for (quint32 i = 0; i < entryCount; ++i)
    {
        QString name;
        if (!readLsaString(file, name))
        {
            return {};
        }

        quint32 sampleOffset = 0;
        if (i != 0)
        {
            quint8 marker = 0;
            if (!readU32Le(file, sampleOffset) || !readU8(file, marker))
            {
                return {};
            }
            if (marker == 0)
            {
                continue;
            }
            if (marker != 0xFF)
            {
                qWarning("LingvoLsaAudio: Unexpected marker in LSA index");
                return {};
            }
        }

        quint32 sampleCount = 0;
        if (!readU32Le(file, sampleCount))
        {
            return {};
        }
        name = name.trimmed().toLower();
        if (!name.isEmpty() && sampleCount != 0)
        {
            index->entries.insert(
                name,
                LsaEntry{
                    .name = name,
                    .sampleOffset = sampleOffset,
                    .sampleCount = sampleCount
                }
            );
        }
    }

    index->oggBase = file.pos();
    std::array<char, 4> oggSignature{};
    if (index->oggBase < 0 || !readExact(file, oggSignature.data(), 4) ||
        std::memcmp(oggSignature.data(), "OggS", 4) != 0)
    {
        qWarning("LingvoLsaAudio: LSA index is not followed by an Ogg stream");
        return {};
    }

    return index;
}

std::shared_ptr<const ArchiveIndex> getArchiveIndex(const QString &archivePath)
{
    QFileInfo info(archivePath);
    if (!info.isFile() || !info.isReadable())
    {
        return {};
    }
    const QString canonical = info.canonicalFilePath().isEmpty() ?
        info.absoluteFilePath() : info.canonicalFilePath();
    const QString cacheKey = QStringLiteral("%1|%2|%3")
        .arg(canonical)
        .arg(info.size())
        .arg(info.lastModified().toMSecsSinceEpoch());

    {
        QMutexLocker lock(&indexMutex());
        const auto found = indexCache().constFind(cacheKey);
        if (found != indexCache().cend())
        {
            return found.value();
        }
    }

    std::shared_ptr<const ArchiveIndex> index =
        readArchiveIndex(info.absoluteFilePath());
    if (!index)
    {
        return {};
    }

    QMutexLocker lock(&indexMutex());
    if (indexCache().size() >= 4)
    {
        indexCache().clear();
    }
    indexCache().insert(cacheKey, index);
    return index;
}

QString archivePathFromUrl(const QString &input)
{
    const QUrl url(input);
    const QString path = url.isLocalFile() ? url.toLocalFile() : input;
    const QFileInfo info(path);
    return info.isFile() ? info.absoluteFilePath() : QString();
}

QString getEntryName(
    const QString &expression,
    const QString &accent)
{
    const auto it = audioManifest().constFind(normalizedExpression(expression));
    if (it == audioManifest().cend())
    {
        return {};
    }
    if (accent.compare(QStringLiteral("uk"), Qt::CaseInsensitive) == 0 ||
        accent.compare(QStringLiteral("gb"), Qt::CaseInsensitive) == 0)
    {
        return it->uk;
    }
    if (accent.compare(QStringLiteral("us"), Qt::CaseInsensitive) == 0 ||
        accent.compare(QStringLiteral("usa"), Qt::CaseInsensitive) == 0)
    {
        return it->us;
    }
    return {};
}

QString makeCachePath(
    const ArchiveIndex &index,
    const QString &entryName)
{
    QString cacheRoot = QStandardPaths::writableLocation(
        QStandardPaths::CacheLocation
    );
    if (cacheRoot.isEmpty())
    {
        cacheRoot = QDir::tempPath() + QStringLiteral("/memento-cache");
    }

    const QString archiveIdentity = QStringLiteral("%1|%2|%3")
        .arg(index.canonicalPath)
        .arg(index.fileSize)
        .arg(index.modifiedTime);
    const QString archiveHash = QString::fromLatin1(
        QCryptographicHash::hash(
            archiveIdentity.toUtf8(), QCryptographicHash::Md5
        ).toHex()
    );
    return QDir(cacheRoot).filePath(
        QStringLiteral("lingvo-lsa/%1/%2").arg(archiveHash, entryName)
    );
}

bool isValidCachedWave(const QString &path)
{
    QFileInfo info(path);
    if (!info.isFile() || info.size() <= 44)
    {
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        return false;
    }
    const QByteArray header = file.read(12);
    return header.size() == 12 &&
        header.left(4) == QByteArrayLiteral("RIFF") &&
        header.mid(8, 4) == QByteArrayLiteral("WAVE");
}

size_t vorbisRead(void *ptr, size_t size, size_t count, void *source)
{
    if (size == 0 || count == 0 || count >
        std::numeric_limits<size_t>::max() / size)
    {
        return 0;
    }
    const size_t requested = size * count;
    if (requested > static_cast<size_t>(std::numeric_limits<qint64>::max()))
    {
        return 0;
    }
    auto *shifted = static_cast<ShiftedFile *>(source);
    const qint64 read = shifted->file->read(
        static_cast<char *>(ptr), static_cast<qint64>(requested)
    );
    return read <= 0 ? 0 : static_cast<size_t>(read) / size;
}

int vorbisSeek(void *source, ogg_int64_t offset, int whence)
{
    auto *shifted = static_cast<ShiftedFile *>(source);
    const qint64 position = shifted->file->pos();
    qint64 target = 0;
    switch (whence)
    {
        case SEEK_SET:
            if (offset < 0 || offset > std::numeric_limits<qint64>::max() -
                shifted->oggBase)
            {
                return -1;
            }
            target = shifted->oggBase + static_cast<qint64>(offset);
            break;
        case SEEK_CUR:
            if ((offset > 0 && position >
                    std::numeric_limits<qint64>::max() - offset) ||
                (offset < 0 && position < -offset))
            {
                return -1;
            }
            target = position + static_cast<qint64>(offset);
            break;
        case SEEK_END:
            if ((offset > 0 && shifted->file->size() >
                    std::numeric_limits<qint64>::max() - offset) ||
                (offset < 0 && shifted->file->size() < -offset))
            {
                return -1;
            }
            target = shifted->file->size() + static_cast<qint64>(offset);
            break;
        default:
            return -1;
    }

    if (target < shifted->oggBase || target > shifted->file->size())
    {
        return -1;
    }
    return shifted->file->seek(target) ? 0 : -1;
}

long vorbisTell(void *source)
{
    const auto *shifted = static_cast<ShiftedFile *>(source);
    const qint64 position = shifted->file->pos();
    if (position < shifted->oggBase || position - shifted->oggBase > LONG_MAX)
    {
        return -1;
    }
    return static_cast<long>(position - shifted->oggBase);
}

template<typename T>
void writeLittleEndian(QByteArray &bytes, qsizetype offset, T value)
{
    qToLittleEndian<T>(value, reinterpret_cast<uchar *>(bytes.data() + offset));
}

QByteArray makeWaveHeader(quint32 sampleRate, quint16 channels, quint32 dataSize)
{
    QByteArray header(44, '\0');
    std::memcpy(header.data(), "RIFF", 4);
    writeLittleEndian<quint32>(header, 4, 36 + dataSize);
    std::memcpy(header.data() + 8, "WAVEfmt ", 8);
    writeLittleEndian<quint32>(header, 16, 16);
    writeLittleEndian<quint16>(header, 20, 1);
    writeLittleEndian<quint16>(header, 22, channels);
    writeLittleEndian<quint32>(header, 24, sampleRate);
    writeLittleEndian<quint32>(header, 28, sampleRate * channels * 2);
    writeLittleEndian<quint16>(header, 32, channels * 2);
    writeLittleEndian<quint16>(header, 34, 16);
    std::memcpy(header.data() + 36, "data", 4);
    writeLittleEndian<quint32>(header, 40, dataSize);
    return header;
}

bool decodeEntry(
    const ArchiveIndex &index,
    const LsaEntry &entry,
    const QString &outputPath)
{
    if (entry.sampleCount == 0 || entry.sampleCount > MAX_SAMPLE_COUNT)
    {
        qWarning("LingvoLsaAudio: Invalid sample range for %s",
                 qUtf8Printable(entry.name));
        return false;
    }

    QFile archive(index.canonicalPath);
    if (!archive.open(QIODevice::ReadOnly) || !archive.seek(index.oggBase))
    {
        qWarning("LingvoLsaAudio: Could not open LSA audio stream");
        return false;
    }

    ShiftedFile shifted{&archive, index.oggBase};
    OggVorbis_File vorbis{};
    const ov_callbacks callbacks{vorbisRead, vorbisSeek, nullptr, vorbisTell};
    const int openResult = ov_open_callbacks(
        &shifted, &vorbis, nullptr, 0, callbacks
    );
    if (openResult != 0)
    {
        qWarning("LingvoLsaAudio: Could not open the LSA Vorbis stream (%d)",
                 openResult);
        return false;
    }

    bool success = false;
    QString error;
    do
    {
        if (ov_pcm_seek(&vorbis, entry.sampleOffset) != 0)
        {
            error = QStringLiteral("could not seek to audio entry");
            break;
        }

        vorbis_info *info = ov_info(&vorbis, -1);
        if (info == nullptr || info->rate <= 0 ||
            (info->channels != 1 && info->channels != 2))
        {
            error = QStringLiteral("unsupported Vorbis format");
            break;
        }

        const quint64 frameBytes = static_cast<quint64>(info->channels) * 2;
        const quint64 dataSize =
            static_cast<quint64>(entry.sampleCount) * frameBytes;
        if (dataSize == 0 || dataSize >
            std::numeric_limits<quint32>::max() - 36)
        {
            error = QStringLiteral("decoded audio range is too large");
            break;
        }

        QDir outputDir = QFileInfo(outputPath).dir();
        if (!outputDir.mkpath(QStringLiteral(".")))
        {
            error = QStringLiteral("could not create audio cache directory");
            break;
        }

        QSaveFile output(outputPath);
        if (!output.open(QIODevice::WriteOnly))
        {
            error = QStringLiteral("could not open cached audio file");
            break;
        }

        const QByteArray header = makeWaveHeader(
            static_cast<quint32>(info->rate),
            static_cast<quint16>(info->channels),
            static_cast<quint32>(dataSize)
        );
        if (output.write(header) != header.size())
        {
            error = QStringLiteral("could not write WAV header");
            break;
        }

        quint64 decodedTotal = 0;
        int bitstream = 0;
        std::array<char, DECODE_CHUNK_SIZE> buffer{};
        while (decodedTotal < dataSize)
        {
            const int request = static_cast<int>(std::min<quint64>(
                buffer.size(), dataSize - decodedTotal
            ));
            const long decoded = ov_read(
                &vorbis,
                buffer.data(),
                request,
                0,
                2,
                1,
                &bitstream
            );
            if (decoded <= 0)
            {
                error = QStringLiteral("Vorbis stream ended before the clip did");
                break;
            }
            if (output.write(buffer.data(), decoded) != decoded)
            {
                error = QStringLiteral("could not write decoded audio");
                break;
            }
            decodedTotal += static_cast<quint64>(decoded);
        }

        if (!error.isEmpty())
        {
            break;
        }
        if (output.commit())
        {
            success = true;
        }
        else
        {
            error = QStringLiteral("could not finish cached WAV file");
        }
    } while (false);

    ov_clear(&vorbis);
    if (!success)
    {
        qWarning("LingvoLsaAudio: %s (%s)",
                 qUtf8Printable(error), qUtf8Printable(entry.name));
    }
    return success;
}

bool parseVirtualUrl(const QString &value, LsaRequest &request)
{
    const QUrl url(value);
    if (url.scheme() != QString::fromLatin1(URL_SCHEME))
    {
        return false;
    }

    const QUrlQuery query(url);
    request.archivePath = archivePathFromUrl(
        query.queryItemValue(QStringLiteral("archive"), QUrl::FullyDecoded)
    );
    request.expression = query.queryItemValue(
        QStringLiteral("expression"), QUrl::FullyDecoded
    );
    request.accent = query.queryItemValue(
        QStringLiteral("accent"), QUrl::FullyDecoded
    ).toLower();
    return !request.archivePath.isEmpty() && !request.expression.isEmpty() &&
        (request.accent == QStringLiteral("uk") ||
         request.accent == QStringLiteral("us"));
}

QString materializeRequest(const LsaRequest &request)
{
    const QString entryName = getEntryName(request.expression, request.accent);
    if (entryName.isEmpty())
    {
        return {};
    }

    const std::shared_ptr<const ArchiveIndex> index =
        getArchiveIndex(request.archivePath);
    if (!index)
    {
        return {};
    }
    const auto entryIt = index->entries.constFind(entryName);
    if (entryIt == index->entries.cend())
    {
        return {};
    }

    const QString path = makeCachePath(*index, entryName);
    if (isValidCachedWave(path))
    {
        return path;
    }
    if (!decodeEntry(*index, entryIt.value(), path))
    {
        return {};
    }
    return path;
}

} // namespace

QString LingvoLsaAudio::makeUrl(
    const QString &archivePath,
    const QString &expression,
    const QString &accent)
{
    const QString localPath = archivePathFromUrl(archivePath);
    const QString normalizedAccent = accent.toLower();
    if (localPath.isEmpty() || normalizedExpression(expression).isEmpty() ||
        getEntryName(expression, normalizedAccent).isEmpty())
    {
        return {};
    }

    const std::shared_ptr<const ArchiveIndex> index = getArchiveIndex(localPath);
    if (!index || !index->entries.contains(
            getEntryName(expression, normalizedAccent)))
    {
        return {};
    }

    QUrl url;
    url.setScheme(QString::fromLatin1(URL_SCHEME));
    url.setHost(QStringLiteral("audio"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("archive"), localPath);
    query.addQueryItem(
        QStringLiteral("expression"), normalizedExpression(expression)
    );
    query.addQueryItem(QStringLiteral("accent"), normalizedAccent);
    url.setQuery(query);
    return url.toString(QUrl::FullyEncoded);
}

bool LingvoLsaAudio::isVirtualUrl(const QString &url)
{
    return QUrl(url).scheme() == QString::fromLatin1(URL_SCHEME);
}

QString LingvoLsaAudio::materialize(const QString &url)
{
    LsaRequest request;
    if (!parseVirtualUrl(url, request))
    {
        return {};
    }
    return materializeRequest(request);
}
