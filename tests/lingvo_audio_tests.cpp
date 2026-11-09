#include "audio/lingvolsaaudio.h"

#include <QCoreApplication>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtEndian>

#include <cstdio>
#include <cstdlib>

static void check(bool ok, const char *message)
{
    if (!ok) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}

static void appendString(QByteArray &data, const QString &text)
{
    for (QChar c : text) {
        const ushort ch = c.unicode();
        data.append(static_cast<char>(ch & 0xff));
        data.append(static_cast<char>(ch >> 8));
    }
    data.append(static_cast<char>(0xff));
}

static void appendU32(QByteArray &data, quint32 value)
{
    char bytes[4];
    qToLittleEndian(value, reinterpret_cast<uchar *>(bytes));
    data.append(bytes, sizeof(bytes));
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir dir;
    check(dir.isValid(), "temporary audio directory");
    qputenv("MEMENTO_LINGVO_MANIFEST", "nonexistent-override.tsv");

    QByteArray index;
    appendString(index, QStringLiteral("L9SA"));
    appendU32(index, 2);
    appendString(index, QStringLiteral("a.wav"));
    appendU32(index, 1);
    appendString(index, QStringLiteral("2.wav"));
    appendU32(index, 1);
    index.append(static_cast<char>(0xff));
    appendU32(index, 1);
    index.append("OggS", 4);
    QFile archive(dir.filePath("synthetic.lsa"));
    check(archive.open(QIODevice::WriteOnly), "create synthetic archive");
    check(archive.write(index) == index.size(), "write synthetic archive");
    archive.close();
    for (const auto &accent : {QStringLiteral("uk"), QStringLiteral("us")}) {
        const QString url = LingvoLsaAudio::makeUrl(archive.fileName(), QStringLiteral("a"), accent);
        check(LingvoLsaAudio::isVirtualUrl(url), "embedded index resolves accent without sidecar");
    }

    if (argc > 1) {
        const QString path = QString::fromLocal8Bit(argv[1]);
        for (const auto &accent : {QStringLiteral("uk"), QStringLiteral("us")}) {
            const QString url = LingvoLsaAudio::makeUrl(path, QStringLiteral("a"), accent);
            check(LingvoLsaAudio::isVirtualUrl(url), "real archive resolves accent");
            QFile wav(LingvoLsaAudio::materialize(url));
            check(wav.open(QIODevice::ReadOnly) && wav.read(4) == "RIFF" && wav.size() > 44,
                  "real archive decodes accent to WAV");
        }
    }
    std::puts("Embedded Lingvo audio index checks passed.");
}
