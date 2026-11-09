#include "dict/databasemanager.h"
#include "dict/data/term.h"
#include "dict/yomidbbuilder.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <zip.h>
#include <cstring>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
static void check(bool ok, const char *message) {
    if (!ok) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
static void sql(sqlite3 *db, const char *text) {
    char *error=nullptr;
    const int rc=sqlite3_exec(db,text,nullptr,nullptr,&error);
    if(rc) {std::fprintf(stderr,"%s\n",error);sqlite3_free(error);}
    check(rc==SQLITE_OK,"fixture SQL failed");
}
static void addZipFile(zip_t *archive, const char *name, const char *data) {
    const zip_int64_t size = static_cast<zip_int64_t>(std::strlen(data));
    zip_source_t *source = zip_source_buffer(archive, data, size, 0);
    check(source != nullptr, "create dictionary archive entry");
    check(zip_file_add(archive, name, source, ZIP_FL_ENC_UTF_8) >= 0,
          "write dictionary archive entry");
}
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    QTemporaryDir temp;
    check(temp.isValid(),"temporary directory");
    const QString path=temp.filePath("dictionary.sqlite");
    sqlite3 *db=nullptr;
    check(yomi_prepare_db(path.toUtf8().constData(),&db)==0,"create v5");
    sql(db,"INSERT INTO directory VALUES(1,'First',3,'test',0),(2,'Second',3,'test',0);"
           "INSERT INTO term_bank VALUES(1,'Hope','','','',0,'[\"first\"]',0,''),"
           "(2,'Hope','','','',0,'[\"second\"]',0,'');"
           "DROP INDEX idx_term_bank_exp_nocase;"
           "DROP INDEX idx_term_bank_reading_nocase;PRAGMA user_version=4;");
    sqlite3_close(db);
    check(yomi_prepare_db(path.toUtf8().constData(),&db)==0,"migrate v4 to v5");
    sqlite3_stmt *stmt=nullptr;
    check(sqlite3_prepare_v2(db,"PRAGMA user_version",-1,&stmt,nullptr)==0,"read version");
    check(sqlite3_step(stmt)==SQLITE_ROW && sqlite3_column_int(stmt,0)==5,"version 5 after migration");
    sqlite3_finalize(stmt);
    check(sqlite3_prepare_v2(db,"SELECT count(*) FROM sqlite_master WHERE name IN ('idx_term_bank_exp_nocase','idx_term_bank_reading_nocase')",-1,&stmt,nullptr)==0,"read indexes");
    check(sqlite3_step(stmt)==SQLITE_ROW && sqlite3_column_int(stmt,0)==2,"both migration indexes");
    sqlite3_finalize(stmt);
    // A previous interrupted v4 migration may have created both indexes
    // without updating user_version. Preserve rows and complete the upgrade.
    sql(db,"PRAGMA user_version=4;");
    sqlite3_close(db);
    check(yomi_prepare_db(path.toUtf8().constData(),&db)==0,
          "resume v4 migration when both indexes already exist");
    check(sqlite3_prepare_v2(db,"PRAGMA user_version",-1,&stmt,nullptr)==0,
          "read resumed version");
    check(sqlite3_step(stmt)==SQLITE_ROW && sqlite3_column_int(stmt,0)==5,
          "resumed migration reaches version 5");
    sqlite3_finalize(stmt);
    check(sqlite3_prepare_v2(db,"SELECT count(*) FROM directory",-1,&stmt,nullptr)==0,
          "read dictionaries after resumed migration");
    check(sqlite3_step(stmt)==SQLITE_ROW && sqlite3_column_int(stmt,0)==2,
          "resumed migration preserves dictionaries");
    sqlite3_finalize(stmt);sqlite3_close(db);
    for(int pass=0;pass<2;++pass) {
        DatabaseManager manager(path,temp.path());
        for(const auto &query : {QString("Hope"),QString("hope"),QString("HOPE")}) {
            QObject owner;QString error;
            auto terms=manager.queryTerms(query,&owner,&error);
            check(error.isEmpty(),"query error");check(terms.size()==2,"two distinct dictionaries with identical heading");
            for(auto term:terms) check(term->definitions().size()==1,"definitions must not mix dictionaries");
        }
        QObject owner;QString error;
        check(manager.queryTerms("missingword",&owner,&error).isEmpty(),"no result");
    }
    check(sqlite3_open(path.toUtf8().constData(),&db)==0,"open fixture");
    sql(db,"INSERT INTO dict_disabled VALUES(2)");sqlite3_close(db);
    DatabaseManager manager(path,temp.path());QObject owner;QString error;
    check(manager.queryTerms("hope",&owner,&error).size()==1,"disabled dictionary excluded");

    const QString archivePath = temp.filePath("synthetic-dictionary.zip");
    int zipError = 0;
    zip_t *archive = zip_open(archivePath.toUtf8().constData(),
                              ZIP_CREATE | ZIP_TRUNCATE, &zipError);
    check(archive != nullptr, "create synthetic Yomitan archive");
    addZipFile(archive, "index.json",
        "{\"title\":\"Synthetic import\",\"format\":3,"
        "\"revision\":\"1\",\"sequenced\":false}");
    addZipFile(archive, "term_bank_1.json",
        "[[\"testword\",\"testword\",\"\",\"\",0,"
        "[\"synthetic dictionary definition\"],0,\"\"]]");
    check(zip_close(archive) == 0, "close synthetic Yomitan archive");

    DatabaseManager importManager(temp.filePath("import.sqlite"), temp.path());
    std::atomic_bool lockReady{false};
    std::thread shortWriteLock([&] {
        sqlite3 *lockDb = nullptr;
        check(sqlite3_open(temp.filePath("import.sqlite").toUtf8().constData(),
                           &lockDb) == SQLITE_OK,
              "open temporary lock connection");
        sql(lockDb, "BEGIN EXCLUSIVE");
        lockReady.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        sql(lockDb, "COMMIT");
        sqlite3_close(lockDb);
    });
    while (!lockReady.load()) {
        std::this_thread::yield();
    }
    const int importResult = importManager.addDictionary(archivePath);
    shortWriteLock.join();
    check(importResult == 0,
          "import waits for a brief SQLite write lock on the dictionary database");
    QObject importOwner;
    QString importError;
    const auto imported = importManager.queryTerms(
        "testword", &importOwner, &importError);
    check(importError.isEmpty() && imported.size() == 1,
          "query imported dictionary through the same manager");
    check(imported.front()->definitions().size() == 1,
          "imported dictionary definition is present");
    std::puts("Database migration, separation, and dictionary import tests passed.");
}
