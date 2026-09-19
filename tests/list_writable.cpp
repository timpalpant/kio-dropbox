#include <KIO/ListJob>

#include <QCoreApplication>
#include <QUrl>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 2) {
        return 2;
    }

    bool foundWritableDirectory = false;
    KIO::ListJob *job = KIO::listDir(QUrl(QString::fromLocal8Bit(argv[1])), KIO::HideProgressInfo);
    QObject::connect(job, &KIO::ListJob::entries, [&foundWritableDirectory](KIO::Job *, const KIO::UDSEntryList &entries) {
        for (const KIO::UDSEntry &entry : entries) {
            if (entry.stringValue(KIO::UDSEntry::UDS_NAME) == QLatin1String(".") && (entry.numberValue(KIO::UDSEntry::UDS_ACCESS) & S_IWUSR)) {
                foundWritableDirectory = true;
            }
        }
    });
    QObject::connect(job, &KJob::result, [&app, &foundWritableDirectory](KJob *finishedJob) {
        app.exit(finishedJob->error() == 0 && foundWritableDirectory ? 0 : 1);
    });
    return app.exec();
}
