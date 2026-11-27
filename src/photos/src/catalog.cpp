#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <signal.h>
#include <sqlite3.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "rummage/photos/api.h"

namespace {
using Json = nlohmann::json;
using Id = std::int64_t;
namespace fs = std::filesystem;
constexpr std::size_t kMaxExportPhotos = 1'000'000;
constexpr std::size_t kMaxExportBytes = std::size_t{256} * 1024 * 1024;

std::optional<Id> nanoseconds(Id seconds, Id fraction) {
    constexpr Id billion = 1'000'000'000;
    if (fraction < 0 || fraction >= billion)
        return std::nullopt;
    if (seconds >= 0) {
        if (seconds > (std::numeric_limits<Id>::max() - fraction) / billion)
            return std::nullopt;
        return seconds * billion + fraction;
    }
    // Shift a negative second toward zero before multiplying so INT64_MIN's partial
    // second remains representable without overflowing the intermediate product.
    const Id adjusted = seconds + 1;
    if (adjusted < std::numeric_limits<Id>::min() / billion)
        return std::nullopt;
    const Id base = adjusted * billion;
    const Id remainder = fraction - billion;
    if (base < std::numeric_limits<Id>::min() - remainder)
        return std::nullopt;
    return base + remainder;
}

class Statement {
public:
    Statement(sqlite3* db, const std::string& sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt_, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }

    ~Statement() { sqlite3_finalize(stmt_); }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    Statement& bind(int index, const Json& value) {
        int code = SQLITE_OK;
        if (value.is_null())
            code = sqlite3_bind_null(stmt_, index);
        else if (value.is_boolean())
            code = sqlite3_bind_int(stmt_, index, value.get<bool>() ? 1 : 0);
        else if (value.is_number_integer())
            code = sqlite3_bind_int64(stmt_, index, value.get<Id>());
        else if (value.is_string()) {
            const auto text = value.get<std::string>();
            code = sqlite3_bind_text(stmt_, index, text.c_str(), static_cast<int>(text.size()),
                                     SQLITE_TRANSIENT);
        } else
            throw std::runtime_error("Invalid catalog parameter");
        if (code != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db_));
        return *this;
    }

    bool row() {
        const int code = sqlite3_step(stmt_);
        if (code == SQLITE_ROW)
            return true;
        if (code == SQLITE_DONE)
            return false;
        throw std::runtime_error(sqlite3_errmsg(db_));
    }

    void run() { (void)row(); }

    Id number(int col) const { return sqlite3_column_int64(stmt_, col); }

    std::string text(int col) const {
        const auto* ptr = sqlite3_column_text(stmt_, col);
        return ptr ? reinterpret_cast<const char*>(ptr) : "";
    }

    Json value(int col) const {
        return sqlite3_column_type(stmt_, col) == SQLITE_NULL ? Json(nullptr) : Json(text(col));
    }

private:
    sqlite3* db_{};
    sqlite3_stmt* stmt_{};
};

// Schema initialization must be serialized before SQLite opens the new file or switches
// its journal mode. Separate descriptors make flock serialize threads as well as processes.
// A killed owner releases the lock automatically; every waiter rereads the schema version.
class SchemaLock {
public:
    explicit SchemaLock(const fs::path& catalog) {
        fd_ = ::open((catalog.string() + ".schema-lock").c_str(),
                     O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd_ < 0)
            throw std::runtime_error("Cannot lock photo catalog initialization");
        while (::flock(fd_, LOCK_EX) != 0) {
            if (errno == EINTR)
                continue;
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("Cannot lock photo catalog initialization");
        }
    }

    ~SchemaLock() {
        if (fd_ >= 0)
            ::close(fd_);
    }

    SchemaLock(const SchemaLock&) = delete;
    SchemaLock& operator=(const SchemaLock&) = delete;

private:
    int fd_{-1};
};

class Database {
public:
    explicit Database(const fs::path& path) : path_(path) {
        if (path.empty() || !path.is_absolute())
            throw std::runtime_error("Catalog path must be absolute");
        const bool new_directory = !fs::exists(path.parent_path());
        fs::create_directories(path.parent_path());
        if (new_directory)
            (void)::chmod(path.parent_path().c_str(), 0700);
        SchemaLock initialization_lock(path);
        const bool fresh = !fs::exists(path);
        if (sqlite3_open_v2(path.c_str(), &db_,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX |
                                SQLITE_OPEN_NOFOLLOW,
                            nullptr) != SQLITE_OK) {
            const std::string error = db_ ? sqlite3_errmsg(db_) : "Cannot open photo catalog";
            sqlite3_close(db_);
            db_ = nullptr;
            throw std::runtime_error(error);
        }
        sqlite3_busy_timeout(db_, 5000);
        if (fresh)
            (void)::chmod(path.c_str(), 0600);
        try {
            exec("PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;");
            Id schema = 0;
            {
                Statement version(db_, "PRAGMA user_version");
                (void)version.row();
                schema = version.number(0);
            }
            if (schema > 3)
                throw std::runtime_error(
                    "Photo catalog is newer than this add-on. Install a matching version; the "
                    "catalog has been preserved.");
            if (schema == 0) {
                if (!fresh)
                    backup(path.string() + ".before-schema-1.backup", true);
                initialize();
            } else if (schema == 1) {
                backup(path.string() + ".before-schema-2.backup", true);
                try {
                    exec(
                        "BEGIN IMMEDIATE; ALTER TABLE jobs ADD COLUMN refresh_again INTEGER NOT "
                        "NULL "
                        "DEFAULT 0; CREATE TABLE background_settings(id INTEGER PRIMARY KEY "
                        "CHECK(id=1),paused INTEGER NOT NULL DEFAULT 0,pace TEXT NOT NULL DEFAULT "
                        "'normal'); INSERT INTO background_settings(id) VALUES(1); CREATE TABLE "
                        "source_directories(id INTEGER PRIMARY KEY AUTOINCREMENT,source_id INTEGER "
                        "NOT "
                        "NULL REFERENCES sources(id) ON DELETE CASCADE,relative_path TEXT NOT "
                        "NULL,seen_job INTEGER NOT NULL DEFAULT "
                        "0,UNIQUE(source_id,relative_path)); "
                        "INSERT INTO source_directories(source_id,relative_path) SELECT id,'' FROM "
                        "sources; PRAGMA user_version=2; COMMIT;");
                    schema = 2;
                } catch (const std::exception& error) {
                    try {
                        exec("ROLLBACK");
                    } catch (const std::exception&) {
                    }
                    throw std::runtime_error(
                        "Photo catalog upgrade failed. The original catalog and pre-upgrade backup "
                        "were preserved; restore the backup or retry with a compatible add-on. " +
                        std::string(error.what()));
                }
            }
            if (schema == 2) {
                backup(path.string() + ".before-schema-3.backup", true);
                try {
                    exec(
                        "BEGIN IMMEDIATE; ALTER TABLE sources ADD COLUMN watch_enabled INTEGER "
                        "NOT NULL DEFAULT 1; CREATE INDEX photos_pending_metadata ON photos(id) "
                        "WHERE metadata_signature='' AND availability='online'; "
                        "CREATE INDEX photos_seen_job ON "
                        "photos(seen_job,availability,metadata_signature); "
                        "CREATE INDEX photos_modified ON photos(mtime,mtime_ns,id); "
                        "CREATE TABLE photo_receipts(photo_id INTEGER PRIMARY KEY REFERENCES "
                        "photos(id) ON DELETE CASCADE, operation_id INTEGER NOT NULL, path TEXT "
                        "NOT NULL, signature TEXT NOT NULL); CREATE INDEX photo_receipts_path ON "
                        "photo_receipts(path); "
                        "INSERT OR REPLACE INTO photo_receipts SELECT "
                        "CAST(json_extract(r.value,'$.id') AS INTEGER),o.id,"
                        "json_extract(r.value,'$.destination'),json_extract(r.value,'$.receipt_"
                        "signature') "
                        "FROM operations o,json_each(o.payload,'$.results') r "
                        "JOIN photos p ON p.id=json_extract(r.value,'$.id') "
                        "WHERE o.kind='trash' AND json_extract(r.value,'$.success')=1 "
                        "AND json_extract(r.value,'$.receipt_signature') IS NOT NULL "
                        "AND json_extract(r.value,'$.destination') IS NOT NULL ORDER BY o.id; "
                        "PRAGMA user_version=3; COMMIT;");
                } catch (...) {
                    try {
                        exec("ROLLBACK");
                    } catch (...) {
                    }
                    throw;
                }
            }
            // Additive album metadata is compatible with existing schema-three catalogs.
            // Keep the allocation counter separate so deleting an album can never retarget
            // a saved search or a stale UI reference to a later album with a reused ID.
            Statement album_metadata(
                db_,
                "SELECT count(*) FROM sqlite_master WHERE type='table' "
                "AND name IN ('album_covers','album_sequence','catalog_identity')");
            (void)album_metadata.row();
            if (album_metadata.number(0) != 3)
                exec(
                    "BEGIN IMMEDIATE; CREATE TABLE IF NOT EXISTS album_covers(album_id INTEGER "
                    "PRIMARY KEY REFERENCES albums(id) ON DELETE CASCADE,photo_id INTEGER NOT "
                    "NULL REFERENCES photos(id) ON DELETE CASCADE,FOREIGN KEY(photo_id,album_id) "
                    "REFERENCES memberships(photo_id,album_id) ON DELETE CASCADE); "
                    "CREATE TABLE IF NOT EXISTS album_sequence(id INTEGER PRIMARY KEY "
                    "CHECK(id=1),next_id INTEGER NOT NULL); "
                    "INSERT OR IGNORE INTO album_sequence VALUES(1,1); "
                    "CREATE TABLE IF NOT EXISTS catalog_identity(id INTEGER PRIMARY KEY "
                    "CHECK(id=1),identity TEXT NOT NULL); "
                    "INSERT OR IGNORE INTO catalog_identity VALUES(1,lower(hex(randomblob(16)))); "
                    "COMMIT;");
        } catch (...) {
            sqlite3_close(db_);
            db_ = nullptr;
            throw;
        }
    }

    ~Database() { sqlite3_close(db_); }

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    sqlite3* get() { return db_; }

    const fs::path& path() const { return path_; }

    void exec(const char* sql) {
        char* error = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
            std::string message = error ? error : sqlite3_errmsg(db_);
            sqlite3_free(error);
            throw std::runtime_error(message);
        }
    }

    Id last_id() const { return sqlite3_last_insert_rowid(db_); }

    void backup(fs::path destination, bool unique = false) {
        if (!destination.is_absolute())
            throw std::runtime_error("Backup path must be absolute");
        const auto original = destination;
        int fd = -1;
        for (unsigned suffix = 0;; ++suffix) {
            fd = ::open(destination.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW,
                        0600);
            if (fd >= 0 || !unique || errno != EEXIST)
                break;
            destination = original.string() + "." + std::to_string(suffix + 1);
        }
        if (fd < 0)
            throw std::runtime_error("Cannot create backup: " + destination.string() +
                                     ". Choose a new path; the original catalog was preserved.");
        ::close(fd);
        sqlite3* copy = nullptr;
        int result = sqlite3_open_v2(destination.c_str(), &copy,
                                     SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOFOLLOW, nullptr);
        if (result == SQLITE_OK) {
            auto* transfer = sqlite3_backup_init(copy, "main", db_, "main");
            if (transfer) {
                result = sqlite3_backup_step(transfer, -1);
                const int finish = sqlite3_backup_finish(transfer);
                if (result == SQLITE_DONE)
                    result = finish;
            } else
                result = sqlite3_errcode(copy);
        }
        const std::string error = copy ? sqlite3_errmsg(copy) : "Cannot open backup";
        sqlite3_close(copy);
        if (result != SQLITE_OK)
            throw std::runtime_error("Catalog backup failed: " + error +
                                     ". The original catalog was preserved.");
    }

private:
    void initialize() {
        exec(R"SQL(BEGIN IMMEDIATE;
CREATE TABLE IF NOT EXISTS sources(id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE, recursive INTEGER NOT NULL, exclusions TEXT NOT NULL DEFAULT '[]', active INTEGER NOT NULL DEFAULT 1, availability TEXT NOT NULL DEFAULT 'online',device INTEGER NOT NULL,inode INTEGER NOT NULL, watch_enabled INTEGER NOT NULL DEFAULT 1);
CREATE TABLE IF NOT EXISTS photos(id INTEGER PRIMARY KEY AUTOINCREMENT, source_id INTEGER NOT NULL REFERENCES sources(id) ON DELETE CASCADE, relative_path TEXT NOT NULL, filename TEXT NOT NULL, device INTEGER NOT NULL, inode INTEGER NOT NULL, size INTEGER NOT NULL, mtime INTEGER NOT NULL, mtime_ns INTEGER NOT NULL, availability TEXT NOT NULL DEFAULT 'online', description TEXT NOT NULL DEFAULT '', rating INTEGER NOT NULL DEFAULT 0 CHECK(rating BETWEEN 0 AND 5), capture_date TEXT, date_override TEXT, width INTEGER NOT NULL DEFAULT 0, height INTEGER NOT NULL DEFAULT 0, camera TEXT NOT NULL DEFAULT '', metadata_signature TEXT NOT NULL DEFAULT '', fingerprint TEXT NOT NULL DEFAULT '', seen_job INTEGER NOT NULL DEFAULT 0);
CREATE UNIQUE INDEX IF NOT EXISTS photos_location ON photos(source_id,relative_path) WHERE availability<>'replaced';
CREATE INDEX IF NOT EXISTS photos_rating ON photos(rating,id);
CREATE INDEX IF NOT EXISTS photos_date ON photos(COALESCE(date_override,capture_date),id);
CREATE INDEX IF NOT EXISTS photos_name ON photos(filename COLLATE NOCASE,id);
CREATE INDEX IF NOT EXISTS photos_identity ON photos(source_id,device,inode);
CREATE INDEX IF NOT EXISTS photos_size ON photos(size);
CREATE INDEX IF NOT EXISTS photos_pending_metadata ON photos(id) WHERE metadata_signature='' AND availability='online';
CREATE INDEX IF NOT EXISTS photos_seen_job ON photos(seen_job,availability,metadata_signature);
CREATE INDEX IF NOT EXISTS photos_modified ON photos(mtime,mtime_ns,id);
CREATE TABLE IF NOT EXISTS photo_receipts(photo_id INTEGER PRIMARY KEY REFERENCES photos(id) ON DELETE CASCADE,operation_id INTEGER NOT NULL,path TEXT NOT NULL,signature TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS photo_receipts_path ON photo_receipts(path);
CREATE TABLE IF NOT EXISTS tags(photo_id INTEGER NOT NULL REFERENCES photos(id) ON DELETE CASCADE, tag TEXT NOT NULL, PRIMARY KEY(photo_id,tag));
CREATE INDEX IF NOT EXISTS tags_text ON tags(tag,photo_id);
CREATE TABLE IF NOT EXISTS albums(id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE);
CREATE TABLE IF NOT EXISTS memberships(photo_id INTEGER NOT NULL REFERENCES photos(id) ON DELETE CASCADE, album_id INTEGER NOT NULL REFERENCES albums(id) ON DELETE CASCADE, PRIMARY KEY(photo_id,album_id));
CREATE INDEX IF NOT EXISTS memberships_album ON memberships(album_id,photo_id);
CREATE TABLE IF NOT EXISTS filters(id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE, definition TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS jobs(id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT NOT NULL, source_id INTEGER REFERENCES sources(id) ON DELETE CASCADE, state TEXT NOT NULL DEFAULT 'queued', completed INTEGER NOT NULL DEFAULT 0, total INTEGER, error TEXT NOT NULL DEFAULT '', last_photo INTEGER NOT NULL DEFAULT 0,refresh_again INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS background_settings(id INTEGER PRIMARY KEY CHECK(id=1),paused INTEGER NOT NULL DEFAULT 0,pace TEXT NOT NULL DEFAULT 'normal');
INSERT OR IGNORE INTO background_settings(id) VALUES(1);
CREATE TABLE IF NOT EXISTS source_directories(id INTEGER PRIMARY KEY AUTOINCREMENT,source_id INTEGER NOT NULL REFERENCES sources(id) ON DELETE CASCADE,relative_path TEXT NOT NULL,seen_job INTEGER NOT NULL DEFAULT 0,UNIQUE(source_id,relative_path));
CREATE TABLE IF NOT EXISTS directories(job_id INTEGER NOT NULL REFERENCES jobs(id) ON DELETE CASCADE, relative_path TEXT NOT NULL, cookie INTEGER NOT NULL DEFAULT 0, PRIMARY KEY(job_id,relative_path));
CREATE TABLE IF NOT EXISTS outcomes(job_id INTEGER NOT NULL REFERENCES jobs(id) ON DELETE CASCADE, path TEXT NOT NULL, error TEXT NOT NULL, stage TEXT NOT NULL DEFAULT 'discovery', PRIMARY KEY(job_id,path));
CREATE TABLE IF NOT EXISTS metadata_claims(photo_id INTEGER PRIMARY KEY REFERENCES photos(id) ON DELETE CASCADE, token TEXT NOT NULL, owner TEXT NOT NULL, owner_start TEXT NOT NULL, expires INTEGER NOT NULL);
CREATE TABLE IF NOT EXISTS operations(id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT NOT NULL, state TEXT NOT NULL DEFAULT 'pending', payload TEXT NOT NULL, error TEXT NOT NULL DEFAULT '');
CREATE VIRTUAL TABLE IF NOT EXISTS photo_search USING fts5(filename,description,tags);
PRAGMA user_version=3;
COMMIT;)SQL");
    }

    sqlite3* db_{};
    fs::path path_;
};

class Transaction {
public:
    explicit Transaction(Database& db) : db_(db), nested_(sqlite3_get_autocommit(db.get()) == 0) {
        db_.exec(nested_ ? "SAVEPOINT photo_change" : "BEGIN IMMEDIATE");
    }

    ~Transaction() {
        if (!done_) {
            try {
                db_.exec(nested_ ? "ROLLBACK TO photo_change; RELEASE photo_change" : "ROLLBACK");
            } catch (...) {
            }
        }
    }

    void commit() {
        db_.exec(nested_ ? "RELEASE photo_change" : "COMMIT");
        done_ = true;
    }

private:
    Database& db_;
    bool nested_{};
    bool done_{};
};

bool contains(const fs::path& parent, const fs::path& child) {
    auto p = parent.begin();
    auto c = child.begin();
    while (p != parent.end() && c != child.end() && *p == *c) {
        ++p;
        ++c;
    }
    return p == parent.end();
}

std::string checked_text(const Json& request, const char* key, std::size_t maximum = 65536) {
    auto value = request.at(key).get<std::string>();
    if (value.size() > maximum || value.find('\0') != std::string::npos)
        throw std::runtime_error(std::string("Invalid ") + key);
    return value;
}

Json source_exclusions(const Json& request) {
    const auto exclusions = request.value("exclusions", Json::array());
    if (!exclusions.is_array() || exclusions.size() > 1000)
        throw std::runtime_error("Choose at most 1000 source-relative exclusions");
    for (const auto& item : exclusions) {
        if (!item.is_string())
            throw std::runtime_error("Exclusions must be source-relative text paths");
        const auto text = item.get<std::string>();
        const fs::path exclusion{text};
        if (exclusion.empty() || exclusion.is_absolute() || text.find('\0') != std::string::npos ||
            std::find(exclusion.begin(), exclusion.end(), "..") != exclusion.end())
            throw std::runtime_error("Exclusions must be source-relative paths without '..'");
    }
    return exclusions;
}

bool valid_date(const std::string& value) {
    if (value.size() < 10 || value.size() > 35 || value[4] != '-' || value[7] != '-')
        return false;
    const auto digits = [&](std::size_t start, std::size_t count) {
        return std::all_of(value.begin() + static_cast<std::ptrdiff_t>(start),
                           value.begin() + static_cast<std::ptrdiff_t>(start + count),
                           [](char c) { return c >= '0' && c <= '9'; });
    };
    if (!digits(0, 4) || !digits(5, 2) || !digits(8, 2))
        return false;
    const std::chrono::year_month_day day{
        std::chrono::year(std::stoi(value.substr(0, 4))),
        std::chrono::month(static_cast<unsigned>(std::stoi(value.substr(5, 2)))),
        std::chrono::day(static_cast<unsigned>(std::stoi(value.substr(8, 2))))};
    if (!day.ok())
        return false;
    if (value.size() == 10)
        return true;
    if (value.size() == 16)
        return (value[10] == 'T' || value[10] == ' ') && value[13] == ':' && digits(11, 2) &&
               digits(14, 2) && std::stoi(value.substr(11, 2)) <= 23 &&
               std::stoi(value.substr(14, 2)) <= 59;
    if (value.size() < 19 || (value[10] != 'T' && value[10] != ' ') || value[13] != ':' ||
        value[16] != ':' || !digits(11, 2) || !digits(14, 2) || !digits(17, 2))
        return false;
    if (std::stoi(value.substr(11, 2)) > 23 || std::stoi(value.substr(14, 2)) > 59 ||
        std::stoi(value.substr(17, 2)) > 59)
        return false;
    if (value.size() == 19 || (value.size() == 20 && value[19] == 'Z'))
        return true;
    return value.size() == 25 && (value[19] == '+' || value[19] == '-') && value[22] == ':' &&
           digits(20, 2) && digits(23, 2) && std::stoi(value.substr(20, 2)) <= 23 &&
           std::stoi(value.substr(23, 2)) <= 59;
}

std::string signature(const struct stat& st) {
    return std::to_string(st.st_dev) + ":" + std::to_string(st.st_ino) + ":" +
           std::to_string(st.st_size) + ":" + std::to_string(st.st_mtim.tv_sec) + ":" +
           std::to_string(st.st_mtim.tv_nsec);
}

const std::set<std::string>& photo_extensions() {
    static const std::set<std::string> formats{".jpg", ".jpeg", ".png", ".webp", ".gif",  ".bmp",
                                               ".tif", ".tiff", ".jxl", ".heif", ".heic", ".avif",
                                               ".dng", ".nef",  ".cr2", ".cr3",  ".arw",  ".orf",
                                               ".rw2", ".raf",  ".pef"};
    return formats;
}

bool photo_extension(const fs::path& path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return photo_extensions().contains(extension);
}

std::vector<fs::path> internal_exclusions(Database& db, const Json& request) {
    std::vector<fs::path> result{db.path().parent_path().lexically_normal()};
    const auto provided = request.value("internal_exclusions", Json::array());
    if (!provided.is_array() || provided.size() > 16)
        throw std::runtime_error("Invalid internal photo exclusions");
    for (const auto& item : provided) {
        const fs::path path = item.get<std::string>();
        if (!path.is_absolute())
            throw std::runtime_error("Internal photo exclusions must be absolute paths");
        const auto normalized = path.lexically_normal();
        if (std::find(result.begin(), result.end(), normalized) == result.end())
            result.push_back(normalized);
    }
    return result;
}

bool internally_excluded(const std::vector<fs::path>& exclusions, const fs::path& path) {
    return std::any_of(exclusions.begin(), exclusions.end(),
                       [&](const auto& exclusion) { return contains(exclusion, path); });
}

Json source_json(Statement& statement) {
    return {{"id", statement.number(0)},
            {"path", statement.text(1)},
            {"recursive", statement.number(2) != 0},
            {"exclusions", Json::parse(statement.text(3))},
            {"active", statement.number(4) != 0},
            {"availability", statement.text(5)},
            {"device", statement.number(6)},
            {"inode", statement.number(7)},
            {"watch_enabled", statement.number(8) != 0}};
}

Id queue_scan(Database& db, Id source_id, bool reconcile = false) {
    Statement existing(db.get(),
                       "SELECT id,state FROM jobs WHERE source_id=? AND kind='scan' AND state IN "
                       "('queued','running','paused','waiting') LIMIT 1");
    existing.bind(1, source_id);
    if (existing.row()) {
        const Id id = existing.number(0);
        if (reconcile && (existing.text(1) == "running" || existing.text(1) == "paused")) {
            Statement again(db.get(), "UPDATE jobs SET refresh_again=1 WHERE id=?");
            again.bind(1, id).run();
        }
        if (existing.text(1) == "waiting") {
            Statement retry(db.get(), "UPDATE jobs SET state='queued',error='' WHERE id=?");
            retry.bind(1, id).run();
            Statement root(
                db.get(),
                "INSERT OR REPLACE INTO directories(job_id,relative_path,cookie) VALUES(?,'',0)");
            root.bind(1, id).run();
        }
        return id;
    }
    Statement insert(db.get(), "INSERT INTO jobs(kind,source_id) VALUES('scan',?)");
    insert.bind(1, source_id).run();
    const Id id = db.last_id();
    Statement directory(db.get(), "INSERT INTO directories(job_id,relative_path) VALUES(?,'')");
    directory.bind(1, id).run();
    return id;
}

void check_sources(Database& db) {
    Statement sources(db.get(),
                      "SELECT id,path,device,inode,availability FROM sources WHERE active=1");
    while (sources.row()) {
        struct stat st{};
        const bool available = ::stat(sources.text(1).c_str(), &st) == 0 && S_ISDIR(st.st_mode) &&
                               static_cast<Id>(st.st_dev) == sources.number(2) &&
                               static_cast<Id>(st.st_ino) == sources.number(3);
        const std::string state = available ? "online" : "offline";
        if (state != sources.text(4)) {
            Statement update(db.get(), "UPDATE sources SET availability=? WHERE id=?");
            update.bind(1, state).bind(2, sources.number(0)).run();
        }
    }
}

Json sources(Database& db) {
    check_sources(db);
    Statement statement(
        db.get(),
        "SELECT id,path,recursive,exclusions,active,availability,device,inode,watch_enabled FROM "
        "sources ORDER BY path");
    Json items = Json::array();
    while (statement.row())
        items.push_back(source_json(statement));
    return {{"sources", items}};
}

Json add_source(Database& db, const Json& request) {
    const auto requested = fs::path(checked_text(request, "path"));
    if (!requested.is_absolute())
        throw std::runtime_error("Choose an absolute folder path");
    std::error_code error;
    const auto path = fs::canonical(requested, error);
    if (error || !fs::is_directory(path, error))
        throw std::runtime_error("The source folder is unavailable");
    if (internally_excluded(internal_exclusions(db, request), path))
        throw std::runtime_error(
            "Rummage's catalog and thumbnail-cache folders cannot be photo sources");
    const auto exclusions = source_exclusions(request);
    Transaction transaction(db);
    Statement overlap(db.get(), "SELECT id,path,active FROM sources");
    Id id = 0;
    while (overlap.row()) {
        const fs::path other = overlap.text(1);
        if (other == path) {
            id = overlap.number(0);
            continue;
        }
        if (contains(other, path) || contains(path, other))
            throw std::runtime_error("This folder overlaps an existing source: " + other.string());
    }
    if (id == 0) {
        Statement insert(
            db.get(),
            "INSERT INTO sources(path,recursive,exclusions,device,inode) VALUES(?,?,?,?,?)");
        struct stat root_stat{};
        if (::stat(path.c_str(), &root_stat) != 0)
            throw std::runtime_error("Source folder became unavailable");
        insert.bind(1, path.string())
            .bind(2, request.value("recursive", true))
            .bind(3, exclusions.dump())
            .bind(4, static_cast<Id>(root_stat.st_dev))
            .bind(5, static_cast<Id>(root_stat.st_ino))
            .run();
        id = db.last_id();
    } else {
        Statement activate(
            db.get(),
            "UPDATE sources SET "
            "active=1,availability='online',watch_enabled=1,recursive=?,exclusions=? WHERE id=?");
        activate.bind(1, request.value("recursive", true))
            .bind(2, exclusions.dump())
            .bind(3, id)
            .run();
    }
    Statement watch_root(
        db.get(), "INSERT OR IGNORE INTO source_directories(source_id,relative_path) VALUES(?,'')");
    watch_root.bind(1, id).run();
    const Id job = queue_scan(db, id);
    transaction.commit();
    Statement source(
        db.get(),
        "SELECT id,path,recursive,exclusions,active,availability,device,inode,watch_enabled FROM "
        "sources WHERE id=?");
    source.bind(1, id);
    (void)source.row();
    return {{"source", source_json(source)}, {"job_id", job}};
}

void update_search(Database& db, Id id) {
    // A normal FTS table stores previous terms, allowing safe replace on annotations.
    Statement remove(db.get(), "DELETE FROM photo_search WHERE rowid=?");
    remove.bind(1, id).run();
    Statement insert(db.get(),
                     "INSERT INTO photo_search(rowid,filename,description,tags) SELECT "
                     "id,filename,description,COALESCE((SELECT group_concat(tag,' ') FROM tags "
                     "WHERE photo_id=p.id),'') FROM photos p WHERE id=?");
    insert.bind(1, id).run();
}

Json photo_json(Database& db, Statement& row, bool annotations = true) {
    const Id id = row.number(0);
    const auto path = fs::path(row.text(2)) / row.text(3);
    Json result{{"id", id},
                {"source_id", row.number(1)},
                {"path", path.string()},
                {"relative_path", row.text(3)},
                {"filename", row.text(4)},
                {"size", row.number(5)},
                {"mtime", row.number(6)},
                {"availability", row.text(7)},
                {"description", row.text(8)},
                {"rating", row.number(9)},
                {"capture_date", row.value(10)},
                {"date_origin", row.text(11)},
                {"width", row.number(12)},
                {"height", row.number(13)},
                {"camera", row.text(14)},
                {"extracted_date", row.value(15)},
                {"date_override", row.value(16)},
                {"signature", row.text(17)},
                {"tags", Json::array()},
                {"albums", Json::array()}};
    const auto identity = row.text(17);
    result["device"] = std::stoll(identity.substr(0, identity.find(':')));
    result["inode"] = std::stoll(
        identity.substr(identity.find(':') + 1,
                        identity.find(':', identity.find(':') + 1) - identity.find(':') - 1));
    const auto timestamp =
        nanoseconds(row.number(6), std::stoll(identity.substr(identity.rfind(':') + 1)));
    result["mtime_ns"] = timestamp.value_or(0);
    result["mtime_ns_valid"] = timestamp.has_value();
    if (!annotations)
        return result;
    Statement tags(db.get(), "SELECT tag FROM tags WHERE photo_id=? ORDER BY tag");
    tags.bind(1, id);
    while (tags.row())
        result["tags"].push_back(tags.text(0));
    Statement albums(db.get(),
                     "SELECT a.id,a.name FROM albums a JOIN memberships m ON a.id=m.album_id WHERE "
                     "m.photo_id=? ORDER BY a.name");
    albums.bind(1, id);
    while (albums.row())
        result["albums"].push_back({{"id", albums.number(0)}, {"name", albums.text(1)}});
    return result;
}

const std::string photo_columns =
    "p.id,p.source_id,s.path,p.relative_path,p.filename,p.size,p.mtime,CASE WHEN "
    "s.active=0 THEN 'inactive' WHEN s.availability='offline' THEN 'offline' ELSE p.availability "
    "END,p.description,p.rating,COALESCE(p.date_override,p.capture_date),CASE WHEN p.date_override "
    "IS NOT NULL THEN 'user' WHEN p.capture_date IS NOT NULL THEN 'embedded' ELSE 'unknown' "
    "END,p.width,p.height,p.camera,p.capture_date,p.date_override,CAST(p.device AS "
    "TEXT)||':'||CAST(p.inode AS TEXT)||':'||CAST(p.size AS TEXT)||':'||CAST(p.mtime AS "
    "TEXT)||':'||CAST(p.mtime_ns AS TEXT)";

void enrich_photos(Database& db, Json& photos) {
    for (std::size_t first = 0; first < photos.size(); first += 1000) {
        const auto last = std::min(first + 1000, photos.size());
        std::map<Id, Json*> rows;
        std::string placeholders;
        for (auto index = first; index < last; ++index) {
            if (!placeholders.empty())
                placeholders += ',';
            placeholders += '?';
            rows.emplace(photos[index].at("id").get<Id>(), &photos[index]);
        }
        Statement tags(db.get(), "SELECT photo_id,tag FROM tags WHERE photo_id IN (" +
                                     placeholders + ") ORDER BY photo_id,tag");
        Statement albums(db.get(),
                         "SELECT m.photo_id,a.id,a.name FROM memberships m JOIN albums a ON "
                         "a.id=m.album_id WHERE m.photo_id IN (" +
                             placeholders + ") ORDER BY m.photo_id,a.name");
        int parameter = 1;
        for (auto index = first; index < last; ++index, ++parameter) {
            tags.bind(parameter, photos[index].at("id"));
            albums.bind(parameter, photos[index].at("id"));
        }
        while (tags.row())
            (*rows.at(tags.number(0)))["tags"].push_back(tags.text(1));
        while (albums.row())
            (*rows.at(albums.number(0)))["albums"].push_back(
                {{"id", albums.number(1)}, {"name", albums.text(2)}});
    }
}

Json query_ids(Database& db, const Json& request) {
    const auto ids = request.at("ids").get<std::vector<Id>>();
    if (ids.size() > 1000)
        throw std::runtime_error("Query at most 1000 photograph IDs per batch");
    Json photos = Json::array(), missing = Json::array();
    if (ids.empty())
        return {{"photos", photos}, {"missing_ids", missing}};
    std::string placeholders;
    for (std::size_t i = 0; i < ids.size(); ++i)
        placeholders += i == 0 ? "?" : ",?";
    const std::string visible = request.value("include_inactive", false)
                                    ? ""
                                    : " AND s.active=1 AND p.availability<>'replaced'";
    Statement rows(db.get(),
                   "SELECT " + photo_columns +
                       " FROM photos p JOIN sources s ON s.id=p.source_id WHERE p.id IN (" +
                       placeholders + ")" + visible + " ORDER BY p.id");
    for (std::size_t i = 0; i < ids.size(); ++i)
        rows.bind(static_cast<int>(i + 1), ids[i]);
    std::set<Id> found;
    while (rows.row()) {
        found.insert(rows.number(0));
        photos.push_back(photo_json(db, rows, false));
    }
    enrich_photos(db, photos);
    for (const auto id : ids)
        if (!found.contains(id))
            missing.push_back(id);
    return {{"photos", photos}, {"missing_ids", missing}};
}

Json query(Database& db, const Json& request) {
    check_sources(db);
    const Id limit = std::clamp(request.value("limit", Id{200}), Id{1}, Id{1000});
    const Id offset = std::max(request.value("offset", Id{0}), Id{0});
    std::string where = " WHERE 1=1";
    std::vector<Json> params;
    const auto filter = [&](const std::string& sql, Json value) {
        where += sql;
        params.push_back(std::move(value));
    };
    if (request.value("retained_only", false))
        where += " AND (s.active=0 OR p.availability='replaced')";
    else if (!request.value("include_inactive", false))
        where += " AND s.active=1 AND p.availability<>'replaced'";
    if (request.value("source_id", Id{0}) > 0)
        filter(" AND p.source_id=?", request["source_id"]);
    if (request.value("album_id", Id{0}) > 0)
        filter(" AND p.id IN (SELECT photo_id FROM memberships WHERE album_id=?)",
               request["album_id"]);
    if (request.value("rating_min", 0) > 0)
        filter(" AND p.rating>=?", request["rating_min"]);
    if (request.value("unknown_date", false))
        where += " AND COALESCE(p.date_override,p.capture_date) IS NULL";
    if (!request.value("date_from", std::string{}).empty())
        filter(" AND COALESCE(p.date_override,p.capture_date)>=?", request["date_from"]);
    if (!request.value("date_to", std::string{}).empty())
        filter(" AND substr(COALESCE(p.date_override,p.capture_date),1,10)<=?", request["date_to"]);
    if (!request.value("folder", std::string{}).empty()) {
        const auto folder = request["folder"].get<std::string>();
        filter(" AND p.relative_path>=?", folder + "/");
        filter(" AND p.relative_path<?", folder + "0");
    }
    const auto text = request.value("text", std::string{});
    if (!text.empty()) {
        // Quote the whole query as an FTS phrase. User syntax never becomes SQL/FTS operators.
        std::string phrase = "\"";
        for (const char c : text) {
            if (c == '"')
                phrase += '"';
            phrase += c;
        }
        phrase += "\"*";
        filter(" AND p.id IN (SELECT rowid FROM photo_search WHERE photo_search MATCH ?)", phrase);
    }
    auto bind = [&](Statement& statement) {
        for (std::size_t i = 0; i < params.size(); ++i)
            statement.bind(static_cast<int>(i + 1), params[i]);
    };
    Statement count(db.get(),
                    "SELECT count(*) FROM photos p JOIN sources s ON s.id=p.source_id" + where);
    bind(count);
    (void)count.row();
    const Id total = count.number(0);
    const auto sort = request.value("sort", std::string("name"));
    // Only fixed SQL expressions enter ORDER BY. Keep the historic default direction for
    // capture date and rating; browser file columns default ascending unless requested.
    const std::string direction =
        request.value("descending", sort == "date" || sort == "rating") ? " DESC" : " ASC";
    std::string order;
    if (sort == "name")
        order = "p.filename COLLATE NOCASE" + direction;
    else if (sort == "date")
        order = "COALESCE(p.date_override,p.capture_date)" + direction;
    else if (sort == "rating")
        order = "p.rating" + direction;
    else if (sort == "size")
        order = "p.size" + direction;
    else if (sort == "modified")
        order = "p.mtime" + direction + ",p.mtime_ns" + direction;
    else if (sort == "location")
        order =
            "(s.path||'/'||substr(p.relative_path,1,length(p.relative_path)-length(p.filename))) "
            "COLLATE NOCASE" +
            direction;
    else
        throw std::runtime_error("Unsupported photo sort order");
    order += ",p.id";  // stable tie-break across pages and repeated requests
    Statement statement(db.get(), "SELECT " + photo_columns +
                                      " FROM photos p JOIN sources s ON s.id=p.source_id" + where +
                                      " ORDER BY " + order + " LIMIT ? OFFSET ?");
    bind(statement);
    statement.bind(static_cast<int>(params.size() + 1), limit)
        .bind(static_cast<int>(params.size() + 2), offset);
    Json photos = Json::array();
    while (statement.row())
        photos.push_back(photo_json(db, statement, false));
    enrich_photos(db, photos);
    return {{"photos", photos}, {"total", total}, {"offset", offset}, {"limit", limit}};
}

Json edit(Database& db, const Json& request) {
    const auto ids = request.at("ids").get<std::vector<Id>>();
    if (ids.empty() || ids.size() > 100000)
        throw std::runtime_error("Choose photographs to edit");
    if (request.contains("rating") &&
        (request.at("rating").get<int>() < 0 || request.at("rating").get<int>() > 5))
        throw std::runtime_error("Rating must be between zero and five");
    if (request.contains("capture_date") && !request["capture_date"].is_null() &&
        !valid_date(request["capture_date"].get<std::string>()))
        throw std::runtime_error(
            "Use a valid ISO date, optionally including time and explicit timezone");
    if (request.contains("description"))
        (void)checked_text(request, "description");
    Transaction transaction(db);
    const std::set<Id> unique(ids.begin(), ids.end());
    for (Id id : unique) {
        Statement exists(db.get(), "SELECT id FROM photos WHERE id=?");
        exists.bind(1, id);
        if (!exists.row())
            throw std::runtime_error("A selected photograph no longer exists in the catalog");
        for (const auto* key : {"description", "rating", "capture_date"})
            if (request.contains(key)) {
                const auto column = std::string(key) == "capture_date" ? "date_override" : key;
                Statement update(db.get(),
                                 "UPDATE photos SET " + std::string(column) + "=? WHERE id=?");
                update.bind(1, request[key]).bind(2, id).run();
            }
        for (const auto& tag : request.value("add_tags", Json::array())) {
            const auto value = tag.get<std::string>();
            if (value.empty() || value.size() > 512 || value.find('\0') != std::string::npos)
                throw std::runtime_error("Tags must contain between one and 512 bytes");
            Statement add(db.get(), "INSERT OR IGNORE INTO tags(photo_id,tag) VALUES(?,?)");
            add.bind(1, id).bind(2, value).run();
        }
        for (const auto& tag : request.value("remove_tags", Json::array())) {
            Statement remove(db.get(), "DELETE FROM tags WHERE photo_id=? AND tag=?");
            remove.bind(1, id).bind(2, tag).run();
        }
        for (const auto& album : request.value("add_albums", Json::array())) {
            Statement add(db.get(),
                          "INSERT OR IGNORE INTO memberships(photo_id,album_id) VALUES(?,?)");
            add.bind(1, id).bind(2, album).run();
        }
        for (const auto& album : request.value("remove_albums", Json::array())) {
            Statement remove(db.get(), "DELETE FROM memberships WHERE photo_id=? AND album_id=?");
            remove.bind(1, id).bind(2, album).run();
        }
        update_search(db, id);
    }
    transaction.commit();
    return {{"changed", unique.size()}};
}

class WorkLock {
public:
    explicit WorkLock(const fs::path& catalog) {
        descriptor_ = ::open((catalog.string() + ".worker-lock").c_str(),
                             O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (descriptor_ < 0)
            throw std::runtime_error("Cannot lock photo catalog work");
        acquired_ = ::flock(descriptor_, LOCK_EX | LOCK_NB) == 0;
    }

    ~WorkLock() {
        if (descriptor_ >= 0)
            ::close(descriptor_);
    }

    bool acquired() const { return acquired_; }

    WorkLock(const WorkLock&) = delete;
    WorkLock& operator=(const WorkLock&) = delete;

private:
    int descriptor_{-1};
    bool acquired_{};
};

Json background_state(Database& db) {
    Statement state(db.get(), "SELECT paused,pace FROM background_settings WHERE id=1");
    (void)state.row();
    const auto pace = state.text(1);
    const bool gentle = pace == "gentle";
    return {{"paused", state.number(0) != 0},
            {"pace", pace},
            {"delay_ms", gentle ? 1000 : 250},
            {"metadata_limit", gentle ? 1 : 4}};
}

Json background_control(Database& db, const Json& request) {
    Transaction transaction(db);
    if (request.contains("paused")) {
        if (!request["paused"].is_boolean())
            throw std::runtime_error("Invalid background pause state");
        Statement update(db.get(), "UPDATE background_settings SET paused=? WHERE id=1");
        update.bind(1, request["paused"]).run();
    }
    if (request.contains("pace")) {
        const auto pace = request["pace"].get<std::string>();
        if (pace != "normal" && pace != "gentle")
            throw std::runtime_error("Choose normal or gentle background work");
        Statement update(db.get(), "UPDATE background_settings SET pace=? WHERE id=1");
        update.bind(1, pace).run();
    }
    const auto result = background_state(db);
    transaction.commit();
    return result;
}

std::string process_start(const std::string& owner) {
    if (owner.empty() ||
        !std::all_of(owner.begin(), owner.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return {};
    std::ifstream input("/proc/" + owner + "/stat");
    std::string line;
    std::getline(input, line);
    const auto end = line.rfind(')');
    if (end == std::string::npos)
        return {};
    std::istringstream fields(line.substr(end + 2));
    std::string value;
    for (int index = 0; index <= 19; ++index)
        if (!(fields >> value))
            return {};
    return value;
}

void reclaim_metadata(Database& db) {
    Statement claims(db.get(), "SELECT photo_id,owner,owner_start FROM metadata_claims");
    std::vector<Id> abandoned;
    while (claims.row()) {
        const auto owner = claims.text(1), start = claims.text(2);
        if (!start.empty() && process_start(owner) != start)
            abandoned.push_back(claims.number(0));
        else if (!owner.empty() && std::all_of(owner.begin(), owner.end(),
                                               [](char c) { return c >= '0' && c <= '9'; })) {
            try {
                const auto pid = std::stoll(owner);
                if (pid > 0 && pid <= 2147483647 && ::kill(static_cast<pid_t>(pid), 0) != 0 &&
                    errno == ESRCH)
                    abandoned.push_back(claims.number(0));
            } catch (const std::exception&) {
            }
        }
    }
    for (Id id : abandoned) {
        Statement remove(db.get(), "DELETE FROM metadata_claims WHERE photo_id=?");
        remove.bind(1, id).run();
    }
    db.exec("DELETE FROM metadata_claims WHERE expires<=unixepoch()");
}

Json metadata_pending(Database& db, Id limit) {
    reclaim_metadata(db);
    if (background_state(db).value("paused", false))
        return Json::array();
    Statement statement(
        db.get(),
        "SELECT p.id,s.path,p.relative_path,p.device,p.inode,p.size,p.mtime,p.mtime_ns FROM photos "
        "p INDEXED BY photos_pending_metadata JOIN sources s ON s.id=p.source_id WHERE "
        "s.active=1 AND s.availability='online' AND "
        "p.availability='online' AND p.metadata_signature='' AND NOT EXISTS(SELECT 1 FROM "
        "metadata_claims c WHERE c.photo_id=p.id AND c.expires>unixepoch()) AND p.seen_job IN "
        "(SELECT id FROM jobs WHERE state IN ('queued','running','completed')) ORDER BY p.id LIMIT "
        "?");
    statement.bind(1, std::clamp(limit, Id{1}, Id{1000}));
    Json pending = Json::array();
    while (statement.row()) {
        std::string current;
        for (int i = 3; i <= 7; ++i) {
            if (i > 3)
                current += ':';
            current += std::to_string(statement.number(i));
        }
        pending.push_back({{"id", statement.number(0)},
                           {"path", (fs::path(statement.text(1)) / statement.text(2)).string()},
                           {"signature", current},
                           {"size", statement.number(5)},
                           {"mtime", statement.number(6)}});
    }
    return pending;
}

Json metadata_claim(Database& db, const Json& request) {
    Transaction transaction(db);
    reclaim_metadata(db);
    if (background_state(db).value("paused", false)) {
        transaction.commit();
        return {{"allowed", false}};
    }
    const Id id = request.at("id").get<Id>();
    Statement candidate(
        db.get(),
        "SELECT p.device,p.inode,p.size,p.mtime,p.mtime_ns FROM photos p JOIN sources s ON "
        "s.id=p.source_id JOIN jobs j ON j.id=p.seen_job WHERE p.id=? AND p.metadata_signature='' "
        "AND p.availability='online' AND s.active=1 AND s.availability='online' AND j.state IN "
        "('queued','running','completed') AND NOT EXISTS(SELECT 1 FROM metadata_claims c WHERE "
        "c.photo_id=p.id AND c.expires>unixepoch())");
    candidate.bind(1, id);
    if (!candidate.row()) {
        transaction.commit();
        return {{"allowed", false}};
    }
    std::string expected;
    for (int i = 0; i < 5; ++i) {
        if (i > 0)
            expected += ':';
        expected += std::to_string(candidate.number(i));
    }
    if (expected != request.at("signature").get<std::string>()) {
        transaction.commit();
        return {{"allowed", false}};
    }
    Statement token(db.get(), "SELECT lower(hex(randomblob(16)))");
    (void)token.row();
    const auto value = token.text(0);
    Statement claim(db.get(),
                    "INSERT INTO metadata_claims(photo_id,token,owner,owner_start,expires) "
                    "VALUES(?,?,?,?,unixepoch()+120) ON CONFLICT(photo_id) DO UPDATE SET "
                    "token=excluded.token,owner=excluded.owner,owner_start=excluded.owner_start,"
                    "expires=excluded.expires");
    claim.bind(1, id)
        .bind(2, value)
        .bind(3, request.value("owner", std::string("browser")))
        .bind(4, process_start(request.value("owner", std::string("browser"))))
        .run();
    transaction.commit();
    return {{"allowed", true}, {"claim_token", value}};
}

Json metadata(Database& db, const Json& request) {
    const Id id = request.at("id").get<Id>();
    Transaction transaction(db);
    Statement claim(db.get(), "SELECT token FROM metadata_claims WHERE photo_id=?");
    claim.bind(1, id);
    if (claim.row() && claim.text(0) != request.value("claim_token", std::string{}))
        throw std::runtime_error("Metadata claim belongs to another worker");
    Statement entry(db.get(),
                    "SELECT s.path,p.relative_path,p.device,p.inode,p.size,p.mtime,p.mtime_ns FROM "
                    "photos p JOIN sources s ON s.id=p.source_id WHERE p.id=?");
    entry.bind(1, id);
    if (!entry.row())
        throw std::runtime_error("Photograph no longer exists");
    const auto path = fs::path(entry.text(0)) / entry.text(1);
    struct stat st{};
    std::string expected;
    for (int i = 2; i <= 6; ++i) {
        if (i > 2)
            expected += ':';
        expected += std::to_string(entry.number(i));
    }
    const auto claimed = request.at("signature").get<std::string>();
    if (expected != claimed || ::stat(path.c_str(), &st) != 0 || signature(st) != claimed) {
        Statement release(db.get(), "DELETE FROM metadata_claims WHERE photo_id=?");
        release.bind(1, id).run();
        if (expected == claimed) {
            Statement changed(db.get(), "UPDATE photos SET availability='changed' WHERE id=?");
            changed.bind(1, id).run();
        }
        transaction.commit();
        return {{"changed", 0}, {"stale", true}};
    }
    if (request.contains("capture_date") && !request["capture_date"].is_null() &&
        !valid_date(request["capture_date"].get<std::string>()))
        throw std::runtime_error("Invalid extracted capture date");
    if (request.value("width", Id{0}) < 0 || request.value("height", Id{0}) < 0)
        throw std::runtime_error("Invalid image dimensions");
    const auto error = request.value("error", std::string{});
    Statement update(
        db.get(),
        "UPDATE photos SET "
        "width=?,height=?,capture_date=?,camera=?,metadata_signature=?,availability=? WHERE id=?");
    update.bind(1, request.value("width", Id{0}))
        .bind(2, request.value("height", Id{0}))
        .bind(3, request.value("capture_date", Json(nullptr)))
        .bind(4, request.value("camera", std::string{}))
        .bind(5, claimed)
        .bind(6, error.empty() ? "online" : "unreadable")
        .bind(7, id)
        .run();
    Statement release(db.get(), "DELETE FROM metadata_claims WHERE photo_id=?");
    release.bind(1, id).run();
    if (!error.empty()) {
        Statement job(db.get(), "SELECT seen_job FROM photos WHERE id=?");
        job.bind(1, id);
        (void)job.row();
        Statement failure(
            db.get(),
            "INSERT OR REPLACE INTO outcomes(job_id,path,error,stage) VALUES(?,?,?,'metadata')");
        failure.bind(1, job.number(0)).bind(2, path.string()).bind(3, error).run();
    }
    transaction.commit();
    return {{"changed", 1}};
}

Json metadata_batch(Database& db, const Json& request) {
    const auto& items = request.at("items");
    if (!items.is_array() || items.size() > 4)
        throw std::runtime_error("Complete at most four metadata items per batch");
    Transaction transaction(db);
    Json results = Json::array(), changed = Json::array();
    for (const auto& item : items) {
        Id id = 0;
        try {
            id = item.at("id").get<Id>();
            auto result = metadata(db, item);
            result["id"] = id;
            result["ok"] = true;
            results.push_back(std::move(result));
            changed.push_back(id);
        } catch (const std::exception& error) {
            const std::string message = error.what();
            try {
                Json failure{{"id", id},
                             {"signature", item.at("signature")},
                             {"claim_token", item.value("claim_token", std::string{})},
                             {"error", "Invalid extracted metadata: " + message}};
                (void)metadata(db, failure);
                changed.push_back(id);
            } catch (const std::exception&) {
                // Never release another worker's claim when a stale/invalid result arrives.
                Statement release(db.get(),
                                  "DELETE FROM metadata_claims WHERE photo_id=? AND token=?");
                release.bind(1, id).bind(2, item.value("claim_token", std::string{})).run();
            }
            results.push_back({{"id", id}, {"ok", false}, {"error", message}});
        }
    }
    transaction.commit();
    return {{"results", results}, {"changed_ids", changed}, {"jobs_changed", !items.empty()}};
}

bool runnable(Database& db, Id id) {
    if (background_state(db).value("paused", false))
        return false;
    Statement state(db.get(), "SELECT state FROM jobs WHERE id=?");
    state.bind(1, id);
    return state.row() && (state.text(0) == "running" || state.text(0) == "queued");
}

void outcome(Database& db, Id job, const std::string& path, const std::string& error,
             const std::string& stage = "discovery") {
    Statement statement(
        db.get(),
        "INSERT INTO outcomes(job_id,path,error,stage) VALUES(?,?,?,?) ON "
        "CONFLICT(job_id,path) DO UPDATE SET error=excluded.error,stage=excluded.stage");
    statement.bind(1, job).bind(2, path).bind(3, error).bind(4, stage).run();
}

bool index_file(Database& db, Id source, Id job, const std::string& relative, const struct stat& st,
                Json& changed) {
    Transaction transaction(db);
    if (!runnable(db, job)) {
        transaction.commit();
        return false;
    }
    // A stable inode within the same registered source is useful only if its previous
    // location has actually disappeared. Hard links retain independent catalog identities.
    Statement current(db.get(),
                      "SELECT id,device,inode,size,mtime,mtime_ns,availability FROM photos WHERE "
                      "source_id=? AND relative_path=? "
                      "AND availability<>'replaced'");
    current.bind(1, source).bind(2, relative);
    Id id = current.row() ? current.number(0) : 0;
    if (id != 0 && (current.number(1) != static_cast<Id>(st.st_dev) ||
                    current.number(2) != static_cast<Id>(st.st_ino))) {
        Statement replace(db.get(), "UPDATE photos SET availability='replaced' WHERE id=?");
        replace.bind(1, id).run();
        changed.push_back(id);
        id = 0;
    }
    if (id != 0 && current.number(3) == static_cast<Id>(st.st_size) &&
        current.number(4) == static_cast<Id>(st.st_mtim.tv_sec) &&
        current.number(5) == static_cast<Id>(st.st_mtim.tv_nsec) &&
        (current.text(6) == "online" || current.text(6) == "unreadable")) {
        Statement seen(db.get(), "UPDATE photos SET seen_job=? WHERE id=?");
        seen.bind(1, job).bind(2, id).run();
        transaction.commit();
        return true;
    }
    if (id == 0) {
        Statement candidate(db.get(),
                            "SELECT p.id,s.path,p.relative_path FROM photos p JOIN sources s ON "
                            "s.id=p.source_id WHERE p.source_id=? AND p.device=? AND p.inode=? AND "
                            "p.size=? AND p.mtime=? AND p.mtime_ns=?");
        candidate.bind(1, source)
            .bind(2, static_cast<Id>(st.st_dev))
            .bind(3, static_cast<Id>(st.st_ino))
            .bind(4, static_cast<Id>(st.st_size))
            .bind(5, static_cast<Id>(st.st_mtim.tv_sec))
            .bind(6, static_cast<Id>(st.st_mtim.tv_nsec));
        std::vector<Id> matches;
        while (candidate.row()) {
            struct stat old{};
            if (::lstat((fs::path(candidate.text(1)) / candidate.text(2)).c_str(), &old) != 0 &&
                errno == ENOENT)
                matches.push_back(candidate.number(0));
        }
        if (matches.size() == 1) {
            id = matches.front();
            Statement move(db.get(), "UPDATE photos SET relative_path=?,filename=? WHERE id=?");
            move.bind(1, relative)
                .bind(2, fs::path(relative).filename().string())
                .bind(3, id)
                .run();
        }
    }
    Statement insert(
        db.get(),
        R"SQL(INSERT INTO photos(source_id,relative_path,filename,device,inode,size,mtime,mtime_ns,seen_job) VALUES(?,?,?,?,?,?,?,?,?)
ON CONFLICT(source_id,relative_path) WHERE availability<>'replaced' DO UPDATE SET device=excluded.device,inode=excluded.inode,size=excluded.size,mtime=excluded.mtime,mtime_ns=excluded.mtime_ns,seen_job=excluded.seen_job,
availability=CASE WHEN photos.availability='unreadable' AND photos.device=excluded.device AND photos.inode=excluded.inode AND photos.size=excluded.size AND photos.mtime=excluded.mtime AND photos.mtime_ns=excluded.mtime_ns THEN 'unreadable' ELSE 'online' END,
metadata_signature=CASE WHEN photos.device=excluded.device AND photos.inode=excluded.inode AND photos.size=excluded.size AND photos.mtime=excluded.mtime AND photos.mtime_ns=excluded.mtime_ns THEN photos.metadata_signature ELSE '' END,
fingerprint=CASE WHEN photos.device=excluded.device AND photos.inode=excluded.inode AND photos.size=excluded.size AND photos.mtime=excluded.mtime AND photos.mtime_ns=excluded.mtime_ns THEN photos.fingerprint ELSE '' END)SQL");
    insert.bind(1, source)
        .bind(2, relative)
        .bind(3, fs::path(relative).filename().string())
        .bind(4, static_cast<Id>(st.st_dev))
        .bind(5, static_cast<Id>(st.st_ino))
        .bind(6, static_cast<Id>(st.st_size))
        .bind(7, static_cast<Id>(st.st_mtim.tv_sec))
        .bind(8, static_cast<Id>(st.st_mtim.tv_nsec))
        .bind(9, job)
        .run();
    if (id == 0) {
        Statement find(db.get(),
                       "SELECT id,device,inode FROM photos WHERE source_id=? AND relative_path=? "
                       "AND availability<>'replaced'");
        find.bind(1, source).bind(2, relative);
        (void)find.row();
        id = find.number(0);
    }
    update_search(db, id);
    changed.push_back(id);
    transaction.commit();
    return true;
}

struct DirectoryCloser {
    void operator()(DIR* stream) const { ::closedir(stream); }
};

struct DirectoryCursor {
    Id job{};
    fs::path path;
    std::string stamp;
    std::unique_ptr<DIR, DirectoryCloser> stream;
};

std::mutex cursor_mutex;
std::map<std::string, DirectoryCursor> cursors;

Json scan_work(Database& db, Id job, Id source, Id budget, const std::vector<fs::path>& internal) {
    Json changed = Json::array();
    bool topology_changed = false, discovery_changed = false;
    Statement root(db.get(),
                   "SELECT path,recursive,exclusions,active,device,inode,availability FROM sources "
                   "WHERE id=?");
    root.bind(1, source);
    if (!root.row() || root.number(3) == 0) {
        Statement cancel(db.get(), "UPDATE jobs SET state='cancelled' WHERE id=?");
        cancel.bind(1, job).run();
        return {{"worked", true}};
    }
    const fs::path path = root.text(0);
    if (internally_excluded(internal, path)) {
        Statement fail(db.get(),
                       "UPDATE jobs SET state='failed',error='Source is inside an internal catalog "
                       "or thumbnail-cache folder' WHERE id=?");
        fail.bind(1, job).run();
        return {{"worked", true}};
    }
    const bool recursive = root.number(1) != 0;
    const auto exclusions = Json::parse(root.text(2)).get<std::vector<std::string>>();

    std::unique_ptr<DIR, DirectoryCloser> root_check(::opendir(path.c_str()));
    struct stat root_stat{};
    if (!root_check || ::fstat(::dirfd(root_check.get()), &root_stat) != 0 ||
        static_cast<Id>(root_stat.st_dev) != root.number(4) ||
        static_cast<Id>(root_stat.st_ino) != root.number(5)) {
        Transaction transaction(db);
        Statement offline(db.get(), "UPDATE sources SET availability='offline' WHERE id=?");
        offline.bind(1, source).run();
        Statement waiting(db.get(),
                          "UPDATE jobs SET state='waiting',error='Source is unavailable. Reconnect "
                          "it and retry.' WHERE id=?");
        waiting.bind(1, job).run();
        transaction.commit();
        return {{"worked", true},
                {"waiting", true},
                {"discovery_changed", true},
                {"topology_changed", true}};
    }
    root_check.reset();
    Statement watch_root(
        db.get(),
        "INSERT INTO source_directories(source_id,relative_path,seen_job) VALUES(?,'',?) ON "
        "CONFLICT(source_id,relative_path) DO UPDATE SET seen_job=excluded.seen_job");
    watch_root.bind(1, source).bind(2, job).run();
    if (root.text(6) != "online") {
        discovery_changed = true;
        topology_changed = true;
    }
    Statement online(db.get(), "UPDATE sources SET availability='online' WHERE id=?");
    online.bind(1, source).run();
    Statement mark(db.get(),
                   "UPDATE jobs SET state='running',error='' WHERE id=? AND state='queued'");
    mark.bind(1, job).run();
    // POSIX directory cookies belong to the open stream that produced them. Keep that
    // stream for subsequent batches. After restart or eviction replay only the unfinished
    // directory; stable signatures prevent repeated decoding/hashing. Directory mutations
    // rewind it as well, so renamed/inserted entries cannot be skipped by a stale cookie.
    std::lock_guard cursor_guard(cursor_mutex);
    const auto catalog_key = db.path().string();
    if (!cursors.contains(catalog_key) && cursors.size() >= 16)
        cursors.erase(cursors.begin());
    auto& cursor = cursors[catalog_key];
    Id processed = 0;
    while (processed < budget && runnable(db, job)) {
        Statement next(db.get(),
                       "SELECT relative_path,cookie FROM directories WHERE job_id=? ORDER BY "
                       "relative_path LIMIT 1");
        next.bind(1, job);
        if (!next.row()) {
            Statement metadata_left(db.get(),
                                    "SELECT count(*) FROM photos WHERE seen_job=? AND "
                                    "metadata_signature='' AND availability='online'");
            metadata_left.bind(1, job);
            (void)metadata_left.row();
            if (metadata_left.number(0) > 0)
                break;
            Transaction transaction(db);
            // Failure to read any subtree keeps unobserved records intact. A vanished drive
            // is never interpreted as a mass deletion.
            Statement problems(
                db.get(), "SELECT count(*) FROM outcomes WHERE job_id=? AND stage='discovery'");
            problems.bind(1, job);
            (void)problems.row();
            struct stat final_root{};
            const bool same_root = ::stat(path.c_str(), &final_root) == 0 &&
                                   static_cast<Id>(final_root.st_dev) == root.number(4) &&
                                   static_cast<Id>(final_root.st_ino) == root.number(5);
            if (!same_root) {
                Statement offline(db.get(), "UPDATE sources SET availability='offline' WHERE id=?");
                offline.bind(1, source).run();
                Statement waiting(db.get(),
                                  "UPDATE jobs SET state='waiting',error='Source disconnected. "
                                  "Reconnect and retry.' WHERE id=?");
                waiting.bind(1, job).run();
                discovery_changed = true;
                topology_changed = true;
                transaction.commit();
                break;
            }
            if (problems.number(0) == 0) {
                Statement missing(
                    db.get(),
                    "UPDATE photos SET availability='missing' WHERE source_id=? AND "
                    "seen_job<>? AND availability NOT IN ('trashed','replaced','missing')");
                missing.bind(1, source).bind(2, job).run();
                discovery_changed = discovery_changed || sqlite3_changes(db.get()) > 0;
                Statement prune(db.get(),
                                "DELETE FROM source_directories WHERE source_id=? AND "
                                "relative_path<>'' AND seen_job<>?");
                prune.bind(1, source).bind(2, job).run();
                topology_changed = topology_changed || sqlite3_changes(db.get()) > 0;
            }
            Statement again(db.get(), "SELECT refresh_again FROM jobs WHERE id=?");
            again.bind(1, job);
            (void)again.row();
            const bool follow_up = again.number(0) != 0;
            Statement finish(db.get(),
                             "UPDATE jobs SET state='completed',total=completed WHERE id=? AND "
                             "state IN ('running','queued')");
            finish.bind(1, job).run();
            if (follow_up) {
                Statement reset(db.get(), "UPDATE jobs SET refresh_again=0 WHERE id=?");
                reset.bind(1, job).run();
                (void)queue_scan(db, source);
            }
            transaction.commit();
            break;
        }
        const auto relative = next.text(0);
        const auto directory = path / relative;
        bool reopened = false;
        if (!cursor.stream || cursor.path != directory || cursor.job != job) {
            cursor.stream.reset(::opendir(directory.c_str()));
            cursor.path = directory;
            cursor.job = job;
            cursor.stamp.clear();
            reopened = true;
        }
        auto& stream = cursor.stream;
        if (!stream) {
            outcome(db, job, directory.string(), std::strerror(errno));
            Statement remove(db.get(),
                             "DELETE FROM directories WHERE job_id=? AND relative_path=?");
            remove.bind(1, job).bind(2, relative).run();
            ++processed;
            continue;
        }
        struct stat directory_stat{};
        if (::fstat(::dirfd(stream.get()), &directory_stat) != 0)
            throw std::runtime_error("Cannot inspect source directory");
        const auto stamp = signature(directory_stat) + ":" +
                           std::to_string(directory_stat.st_ctim.tv_sec) + ":" +
                           std::to_string(directory_stat.st_ctim.tv_nsec);
        if (reopened || stamp != cursor.stamp ||
            static_cast<Id>(::telldir(stream.get())) != next.number(1)) {
            ::rewinddir(stream.get());
            cursor.stamp = stamp;
            Statement reset(db.get(),
                            "UPDATE directories SET cookie=0 WHERE job_id=? AND relative_path=?");
            reset.bind(1, job).bind(2, relative).run();
        }
        bool finished = false;
        while (processed < budget && runnable(db, job)) {
            errno = 0;
            auto* entry = ::readdir(stream.get());
            if (!entry) {
                if (errno != 0)
                    outcome(db, job, directory.string(), std::strerror(errno));
                finished = true;
                break;
            }
            const std::string name = entry->d_name;
            const auto cookie = static_cast<Id>(::telldir(stream.get()));
            if (name == "." || name == "..")
                continue;
            const auto child = fs::path(relative) / name;
            bool excluded = internally_excluded(internal, path / child);
            try {
                (void)Json(child.string()).dump();
            } catch (const Json::type_error&) {
                std::ostringstream label;
                label << "[unsupported filename bytes ";
                for (std::size_t i = 0; i < std::min(name.size(), std::size_t{24}); ++i)
                    label << std::hex << std::setw(2) << std::setfill('0')
                          << static_cast<unsigned>(static_cast<unsigned char>(name[i]));
                if (name.size() > 24)
                    label << "...";
                label << "]";
                outcome(
                    db, job, (directory / label.str()).string(),
                    "Filename is not valid UTF-8; rename it using the file manager before retrying",
                    "filename");
                excluded = true;
            }
            for (const auto& exclusion : exclusions)
                if (contains(fs::path(exclusion).lexically_normal(), child)) {
                    excluded = true;
                    break;
                }
            if (!excluded) {
                struct stat st{};
                if (::lstat((path / child).c_str(), &st) != 0)
                    outcome(db, job, (path / child).string(), std::strerror(errno));
                else if (S_ISDIR(st.st_mode) && recursive) {
                    Statement add(
                        db.get(),
                        "INSERT OR IGNORE INTO directories(job_id,relative_path) VALUES(?,?)");
                    add.bind(1, job).bind(2, child.string()).run();
                    Statement present(
                        db.get(),
                        "SELECT 1 FROM source_directories WHERE source_id=? AND relative_path=?");
                    present.bind(1, source).bind(2, child.string());
                    if (!present.row())
                        topology_changed = true;
                    Statement known(
                        db.get(),
                        "INSERT INTO source_directories(source_id,relative_path,seen_job) "
                        "VALUES(?,?,?) ON CONFLICT(source_id,relative_path) DO UPDATE SET "
                        "seen_job=excluded.seen_job");
                    known.bind(1, source).bind(2, child.string()).bind(3, job).run();
                } else if (S_ISREG(st.st_mode) && photo_extension(child)) {
                    if (!index_file(db, source, job, child.string(), st, changed))
                        break;
                }
                // Symlinks are not followed: roots cannot escape or acquire cycles.
            }
            Transaction checkpoint(db);
            Statement position(
                db.get(), "UPDATE directories SET cookie=? WHERE job_id=? AND relative_path=?");
            position.bind(1, cookie).bind(2, job).bind(3, relative).run();
            Statement count(db.get(), "UPDATE jobs SET completed=completed+1 WHERE id=?");
            count.bind(1, job).run();
            checkpoint.commit();
            ++processed;
        }
        if (finished) {
            stream.reset();
            Statement remove(db.get(),
                             "DELETE FROM directories WHERE job_id=? AND relative_path=?");
            remove.bind(1, job).bind(2, relative).run();
        }
    }
    return {{"worked", true},
            {"job_id", job},
            {"processed", processed},
            {"changed_ids", changed},
            {"discovery_changed", discovery_changed || !changed.empty()},
            {"topology_changed", topology_changed}};
}

std::string fingerprint(const fs::path& path, const std::string& expected) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        throw std::runtime_error("Cannot read original: " + path.string());

    struct Close {
        int fd;

        ~Close() { ::close(fd); }
    } close{fd};
    struct stat before{};
    if (::fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) || signature(before) != expected)
        throw std::runtime_error("Original changed since indexing: " + path.string());
    // This signature only selects candidates. Destructive review always compares full
    // bytes again; a hash collision never authorizes trashing a file.
    std::uint64_t hash = 14695981039346656037ULL;
    std::array<unsigned char, 65536> buffer{};
    for (;;) {
        const auto count = ::read(fd, buffer.data(), buffer.size());
        if (count < 0) {
            if (errno == EINTR)
                continue;
            throw std::runtime_error("Cannot read original: " + path.string());
        }
        if (count == 0)
            break;
        for (ssize_t i = 0; i < count; ++i) {
            hash ^= buffer[static_cast<std::size_t>(i)];
            hash *= 1099511628211ULL;
        }
    }
    struct stat after{};
    if (::fstat(fd, &after) != 0 || signature(after) != expected)
        throw std::runtime_error("Original changed during comparison: " + path.string());
    std::ostringstream output;
    output << std::hex << std::setw(16) << std::setfill('0') << hash;
    return output.str();
}

bool equal_files(const fs::path& a, const fs::path& b) {
    std::ifstream left(a, std::ios::binary), right(b, std::ios::binary);
    if (!left || !right)
        return false;
    std::array<char, 65536> first{}, second{};
    for (;;) {
        left.read(first.data(), static_cast<std::streamsize>(first.size()));
        right.read(second.data(), static_cast<std::streamsize>(second.size()));
        if (left.bad() || right.bad() || left.gcount() != right.gcount())
            return false;
        if (left.gcount() == 0)
            return true;
        if (!std::equal(first.begin(), first.begin() + left.gcount(), second.begin()))
            return false;
    }
}

Json duplicate_work(Database& db, Id job, Id budget) {
    Statement last(db.get(), "SELECT last_photo FROM jobs WHERE id=?");
    last.bind(1, job);
    (void)last.row();
    Statement mark(db.get(), "UPDATE jobs SET state='running' WHERE id=? AND state='queued'");
    mark.bind(1, job).run();
    Statement candidates(
        db.get(),
        "SELECT " + photo_columns +
            ",p.fingerprint FROM photos p JOIN sources s ON s.id=p.source_id WHERE p.id>? AND "
            "s.active=1 AND s.availability='online' AND p.availability='online' AND p.size IN "
            "(SELECT size FROM photos GROUP BY size HAVING count(*)>1) ORDER BY p.id LIMIT ?");
    candidates.bind(1, last.number(0)).bind(2, budget);
    Json items = Json::array();
    while (candidates.row()) {
        auto item = photo_json(db, candidates);
        item["fingerprint"] = candidates.text(18);
        items.push_back(std::move(item));
    }
    Id processed = 0;
    for (const auto& item : items) {
        if (!runnable(db, job))
            break;
        try {
            if (item["fingerprint"].get<std::string>().empty()) {
                const auto hash = fingerprint(item["path"].get<std::string>(),
                                              item["signature"].get<std::string>());
                Statement update(db.get(), "UPDATE photos SET fingerprint=? WHERE id=?");
                update.bind(1, hash).bind(2, item["id"]).run();
            }
        } catch (const std::exception& error) {
            outcome(db, job, item["path"].get<std::string>(), error.what());
        }
        Statement checkpoint(db.get(),
                             "UPDATE jobs SET last_photo=?,completed=completed+1 WHERE id=?");
        checkpoint.bind(1, item["id"]).bind(2, job).run();
        ++processed;
    }
    if (items.empty()) {
        Statement finish(db.get(),
                         "UPDATE jobs SET state='completed',total=completed WHERE id=? AND state "
                         "IN ('queued','running')");
        finish.bind(1, job).run();
    }
    return {{"worked", true}, {"job_id", job}, {"processed", processed}};
}

Json duplicates(Database& db, const Json& request) {
    const Id limit = std::clamp(request.value("limit", Id{200}), Id{1}, Id{1000});
    const Id offset = std::max(request.value("offset", Id{0}), Id{0});
    const std::string grouped =
        "SELECT p.fingerprint,p.size FROM photos p JOIN sources s ON s.id=p.source_id WHERE "
        "p.fingerprint<>'' AND p.availability='online' AND s.active=1 AND s.availability='online' "
        "GROUP BY p.fingerprint,p.size HAVING count(*)>1";
    Statement count(db.get(), "SELECT count(*) FROM (" + grouped + ")");
    (void)count.row();
    const Id total = count.number(0);
    Statement groups(db.get(), grouped + " ORDER BY p.fingerprint,p.size LIMIT ? OFFSET ?");
    groups.bind(1, limit).bind(2, offset);
    Json result = Json::array();
    while (groups.row()) {
        Statement entries(db.get(), "SELECT " + photo_columns +
                                        " FROM photos p JOIN sources s ON s.id=p.source_id WHERE "
                                        "p.fingerprint=? AND p.size=? AND p.availability='online' "
                                        "AND s.active=1 AND s.availability='online' ORDER BY p.id");
        entries.bind(1, groups.text(0)).bind(2, groups.number(1));
        Json photos = Json::array();
        while (entries.row())
            photos.push_back(photo_json(db, entries, false));
        enrich_photos(db, photos);
        if (photos.size() > 1)
            result.push_back(
                {{"key", groups.text(0) + ":" + std::to_string(groups.number(1))},
                 {"photos", photos},
                 {"keeper_id", photos[0]["id"]},
                 {"reason", "First cataloged copy; compare original locations before choosing"},
                 {"verified", false}});
    }
    return {{"groups", result},
            {"total", total},
            {"offset", offset},
            {"limit", limit},
            {"more", offset + static_cast<Id>(result.size()) < total}};
}

Json verify_duplicates(Database& db, const Json& request) {
    const auto ids = request.at("ids").get<std::vector<Id>>();
    if (ids.size() < 2 || ids.size() > 1000)
        throw std::runtime_error(
            "Choose between 2 and 1000 duplicate candidates, including retained copies");
    Json photos = Json::array();
    for (Id id : ids) {
        Statement row(db.get(),
                      "SELECT " + photo_columns +
                          " FROM photos p JOIN sources s ON s.id=p.source_id WHERE p.id=? AND "
                          "s.active=1 AND s.availability='online' AND p.availability='online'");
        row.bind(1, id);
        if (!row.row())
            throw std::runtime_error("An original is unavailable; refresh duplicate review");
        auto item = photo_json(db, row);
        if (!item.value("mtime_ns_valid", true))
            throw std::runtime_error(
                "Original timestamp is outside the supported range for guarded file operations");
        (void)fingerprint(item["path"].get<std::string>(), item["signature"].get<std::string>());
        photos.push_back(std::move(item));
    }
    for (std::size_t i = 1; i < photos.size(); ++i)
        if (!equal_files(photos[0]["path"].get<std::string>(),
                         photos[i]["path"].get<std::string>()))
            throw std::runtime_error("Candidates are not byte-identical; no files were changed");
    // Check identities again after the comparisons to reject replacement races.
    for (const auto& photo : photos) {
        if (!photo.value("mtime_ns_valid", true))
            throw std::runtime_error(
                "Original timestamp is outside the supported range for guarded file operations");
        struct stat st{};
        if (::lstat(photo["path"].get<std::string>().c_str(), &st) != 0 ||
            signature(st) != photo["signature"].get<std::string>())
            throw std::runtime_error("A candidate changed during comparison; retry review");
    }
    return {{"verified", true}, {"photos", photos}};
}

Json work(Database& db, const Json& request) {
    const auto state = background_state(db);
    if (state.value("paused", false)) {
        auto result = state;
        result["worked"] = false;
        result["metadata"] = Json::array();
        return result;
    }
    WorkLock lock(db.path());
    if (!lock.acquired())
        return {{"worked", false}, {"busy", true}, {"metadata", Json::array()}};
    const Id maximum = state.value("pace", std::string{}) == "gentle" ? Id{8} : Id{256};
    const Id budget = std::clamp(request.value("budget", Id{64}), Id{1}, maximum);
    Statement job(db.get(),
                  "SELECT id,kind,COALESCE(source_id,0) FROM jobs WHERE state IN "
                  "('queued','running') ORDER BY id LIMIT 1");
    Json result{{"worked", false}};
    if (job.row())
        result = job.text(1) == "scan" ? scan_work(db, job.number(0), job.number(2), budget,
                                                   internal_exclusions(db, request))
                                       : duplicate_work(db, job.number(0), std::min(budget, Id{8}));
    result["metadata"] = metadata_pending(db, state.at("metadata_limit").get<Id>());
    if (!result.contains("changed_ids"))
        result["changed_ids"] = Json::array();
    if (!result.contains("discovery_changed"))
        result["discovery_changed"] = false;
    if (!result.contains("topology_changed"))
        result["topology_changed"] = false;
    result["jobs_changed"] = result.value("worked", false);
    result.update(background_state(db));
    return result;
}

Json jobs(Database& db, bool summary = false) {
    // Aggregate once over the displayed jobs, rather than scanning the photo catalog
    // separately for each historical job. Counts describe rows currently owned by each
    // scan (a later refresh may take ownership); completed job history is not a snapshot.
    const std::string selection =
        summary ? "SELECT id,kind,source_id,state,completed,total,error FROM jobs ORDER BY CASE "
                  "state WHEN 'running' THEN 0 WHEN 'queued' THEN 1 WHEN 'paused' THEN 2 WHEN "
                  "'waiting' THEN 3 ELSE 4 END,id DESC LIMIT 1"
                : "SELECT id,kind,source_id,state,completed,total,error FROM jobs ORDER BY id DESC "
                  "LIMIT 1000";
    // Summary polling uses index ranges. Evaluating three boolean SUM expressions
    // over every photo dominated each worker batch for large, mostly finished scans.
    // The third summary column is the full population; unavailable is its remainder.
    const std::string photo_progress = summary ? R"SQL(
    SELECT j.id AS job_id,
        (SELECT count(*) FROM photos WHERE seen_job=j.id
            AND availability IN ('online','unreadable') AND metadata_signature>'') AS done,
        (SELECT count(*) FROM photos WHERE seen_job=j.id
            AND availability='online' AND metadata_signature='') AS pending,
        (SELECT count(*) FROM photos WHERE seen_job=j.id) AS unavailable
    FROM listed j WHERE j.kind='scan'
)SQL"
                                               : R"SQL(
    SELECT p.seen_job AS job_id,
        sum(p.availability IN ('online','unreadable') AND p.metadata_signature<>'') AS done,
        sum(p.availability='online' AND p.metadata_signature='') AS pending,
        sum(p.availability NOT IN ('online','unreadable') OR
            (p.availability='unreadable' AND p.metadata_signature='')) AS unavailable
    FROM photos p JOIN listed j ON j.id=p.seen_job AND j.kind='scan' GROUP BY p.seen_job
)SQL";
    Statement statement(db.get(), "WITH listed AS (" + selection + "), photo_progress AS (" +
                                      photo_progress + R"SQL(
), directory_progress AS (
    SELECT d.job_id,count(*) AS pending FROM directories d
    JOIN listed j ON j.id=d.job_id GROUP BY d.job_id
), claims AS (
    SELECT p.seen_job AS job_id,count(*) AS active FROM metadata_claims c
    JOIN photos p ON p.id=c.photo_id JOIN listed j ON j.id=p.seen_job
    WHERE c.expires>unixepoch() GROUP BY p.seen_job
)
SELECT j.id,j.kind,COALESCE(j.source_id,0),j.state,j.completed,j.total,j.error,
    COALESCE(p.done,0),COALESCE(p.pending,0),COALESCE(p.unavailable,0),
    COALESCE(d.pending,0),COALESCE(c.active,0)
FROM listed j LEFT JOIN photo_progress p ON p.job_id=j.id
LEFT JOIN directory_progress d ON d.job_id=j.id LEFT JOIN claims c ON c.job_id=j.id
ORDER BY j.id DESC
)SQL");
    Json items = Json::array();
    while (statement.row()) {
        Json item{
            {"id", statement.number(0)},
            {"kind", statement.text(1)},
            {"source_id", statement.number(2)},
            {"state", statement.text(3)},
            {"completed", statement.number(4)},
            {"total", statement.value(5).is_null() ? Json(nullptr) : Json(statement.number(5))},
            {"error", statement.text(6)},
            {"failures", Json::array()}};
        item["metadata_completed"] = statement.number(7);
        item["metadata_pending"] = statement.number(8);
        item["metadata_total"] = statement.number(7) + statement.number(8);
        item["metadata_unavailable"] =
            statement.number(9) - (summary ? statement.number(7) + statement.number(8) : 0);
        item["pending_directories"] = statement.number(10);
        item["in_flight"] = statement.number(11);
        const auto state = statement.text(3);
        if (state == "completed")
            item["phase"] = "complete";
        else if (state == "failed" || state == "cancelled")
            item["phase"] = state;
        else if (statement.text(1) != "scan")
            item["phase"] = "fingerprinting";
        else if (statement.number(10) > 0)
            item["phase"] = "discovering";
        else
            item["phase"] = statement.number(8) > 0 ? "metadata" : "finishing";
        if (summary) {
            Statement count(db.get(), "SELECT count(*) FROM outcomes WHERE job_id=?");
            count.bind(1, statement.number(0));
            (void)count.row();
            item["failure_count"] = count.number(0);
        } else {
            Statement failures(db.get(),
                               "SELECT path,error,stage FROM outcomes WHERE job_id=? LIMIT 100");
            failures.bind(1, statement.number(0));
            while (failures.row())
                item["failures"].push_back({{"path", failures.text(0)},
                                            {"error", failures.text(1)},
                                            {"stage", failures.text(2)}});
        }
        items.push_back(std::move(item));
    }
    auto result = background_state(db);
    result["jobs"] = items;
    if (summary) {
        Json counts{{"paused", 0}, {"waiting", 0}, {"failed", 0}};
        Statement states(db.get(),
                         "SELECT state,count(*) FROM jobs WHERE state IN "
                         "('paused','waiting','failed') GROUP BY state");
        while (states.row())
            counts[states.text(0)] = states.number(1);
        result["job_counts"] = counts;
    }
    return result;
}

Json job_control(Database& db, const Json& request) {
    const auto action = request.at("action").get<std::string>();
    const Id id = request.at("id").get<Id>();
    Transaction transaction(db);
    Statement job(db.get(), "SELECT kind,source_id,state FROM jobs WHERE id=?");
    job.bind(1, id);
    if (!job.row())
        throw std::runtime_error("Job no longer exists");
    std::string state;
    if (action == "pause")
        state = "paused";
    else if (action == "cancel")
        state = "cancelled";
    else if (action == "resume" || action == "retry")
        state = "queued";
    else
        throw std::runtime_error("Unknown job action");
    const auto old = job.text(2);
    if ((action == "pause" || action == "cancel") && old == "completed")
        throw std::runtime_error("This job has already finished");
    if (action == "retry") {
        Statement clear(db.get(), "DELETE FROM outcomes WHERE job_id=?");
        clear.bind(1, id).run();
        if (job.text(0) == "scan") {
            Statement restart(
                db.get(),
                "INSERT OR REPLACE INTO directories(job_id,relative_path,cookie) VALUES(?,'',0)");
            restart.bind(1, id).run();
            Statement retry_metadata(
                db.get(),
                "UPDATE photos SET availability='online',metadata_signature='' WHERE seen_job=? "
                "AND availability='unreadable'");
            retry_metadata.bind(1, id).run();
        } else {
            Statement restart(db.get(),
                              "UPDATE jobs SET last_photo=0,completed=0,total=NULL WHERE id=?");
            restart.bind(1, id).run();
        }
    }
    Statement update(db.get(), "UPDATE jobs SET state=?,error='' WHERE id=?");
    update.bind(1, state).bind(2, id).run();
    transaction.commit();
    return {{"state", state}};
}

Id create_album(Database& db, const std::string& name) {
    Statement existing(db.get(), "SELECT id FROM albums WHERE name=?");
    existing.bind(1, name);
    if (existing.row())
        throw std::runtime_error("An album with this name already exists");
    Statement sequence(db.get(),
                       "SELECT next_id,COALESCE((SELECT max(id) FROM albums),0) "
                       "FROM album_sequence WHERE id=1");
    if (!sequence.row() || sequence.number(0) >= std::numeric_limits<Id>::max() ||
        sequence.number(1) >= std::numeric_limits<Id>::max() - 1)
        throw std::runtime_error("No more album identifiers are available");
    const Id id = std::max(sequence.number(0), sequence.number(1) + 1);
    Statement insert(db.get(), "INSERT INTO albums(id,name) VALUES(?,?)");
    insert.bind(1, id).bind(2, name).run();
    Statement advance(db.get(), "UPDATE album_sequence SET next_id=? WHERE id=1");
    advance.bind(1, id + 1).run();
    return id;
}

Id find_or_create_album(Database& db, const std::string& name) {
    Statement find(db.get(), "SELECT id FROM albums WHERE name=?");
    find.bind(1, name);
    return find.row() ? find.number(0) : create_album(db, name);
}

Json export_data(Database& db, const Json& request) {
    // A read transaction captures one coherent snapshot across windows and writers.
    db.exec("BEGIN");
    try {
        Statement count(db.get(), "SELECT count(*) FROM photos");
        (void)count.row();
        if (static_cast<std::uint64_t>(count.number(0)) > kMaxExportPhotos)
            throw std::runtime_error(
                "Annotation export supports at most 1000000 photographs, matching restore "
                "capacity");
        Json data{{"version", 1},
                  {"sources", sources(db)["sources"]},
                  {"photos", Json::array()},
                  {"albums", Json::array()},
                  {"filters", Json::array()}};
        Statement photos(db.get(), "SELECT " + photo_columns +
                                       ",p.device,p.inode,p.mtime_ns,p.fingerprint FROM photos p "
                                       "JOIN sources s ON s.id=p.source_id ORDER BY p.id");
        while (photos.row()) {
            auto item = photo_json(db, photos, false);
            item["device"] = photos.number(18);
            item["inode"] = photos.number(19);
            item["mtime_subsecond"] = photos.number(20);
            item["fingerprint"] = photos.text(21);
            data["photos"].push_back(std::move(item));
        }
        enrich_photos(db, data["photos"]);
        Statement albums(db.get(),
                         "SELECT a.id,a.name,COALESCE(c.photo_id,0) FROM albums a "
                         "LEFT JOIN album_covers c ON c.album_id=a.id ORDER BY a.id");
        while (albums.row())
            data["albums"].push_back({{"id", albums.number(0)},
                                      {"name", albums.text(1)},
                                      {"cover_id", albums.number(2)}});
        Statement filters(db.get(), "SELECT name,definition FROM filters ORDER BY id");
        while (filters.row())
            data["filters"].push_back(
                {{"name", filters.text(0)}, {"filter", Json::parse(filters.text(1))}});
        const auto encoded = data.dump(2);
        if (encoded.size() > kMaxExportBytes)
            throw std::runtime_error(
                "Annotation export exceeds the 256 MiB restore capacity; no export was created");
        db.exec("COMMIT");
        if (request.value("encoded", false))
            return {{"json", encoded},
                    {"bytes", encoded.size()},
                    {"photo_count", data["photos"].size()}};
        return {{"data", data}};
    } catch (...) {
        db.exec("ROLLBACK");
        throw;
    }
}

Json restore(Database& db, const Json& request) {
    const auto& data = request.at("data");
    if (data.at("version") != 1)
        throw std::runtime_error("Unsupported photo annotation export version");
    if (!data.at("photos").is_array() || data["photos"].size() > kMaxExportPhotos)
        throw std::runtime_error("Photo restore supports at most 1000000 photographs");
    if (data.dump().size() > kMaxExportBytes)
        throw std::runtime_error("Annotation restore supports at most 256 MiB per export");
    Transaction transaction(db);
    Json restored = Json::array(), missing = Json::array(), ambiguous = Json::array(),
         unresolved_filters = Json::array();
    std::map<Id, Id> album_mapping, photo_mapping;
    for (const auto& album : data.value("albums", Json::array())) {
        const auto name = checked_text(album, "name", 512);
        if (name.empty())
            throw std::runtime_error("Invalid exported album name");
        album_mapping[album.at("id").get<Id>()] = find_or_create_album(db, name);
    }
    const auto map_path = [&](const fs::path& original) {
        for (const auto& mapping : request.value("source_mappings", Json::array())) {
            const fs::path from = mapping.at("from").get<std::string>();
            const fs::path to = mapping.at("to").get<std::string>();
            if (!from.is_absolute() || !to.is_absolute())
                throw std::runtime_error("Source mappings must use absolute paths");
            if (contains(from, original))
                return to / original.lexically_relative(from);
        }
        return original;
    };
    std::map<Id, Id> source_mapping;
    Json sources_requiring_refresh = Json::array();
    for (const auto& source : data.at("sources")) {
        const auto exclusions = source_exclusions(source);
        const auto path = map_path(fs::path(source.at("path").get<std::string>()));
        Statement find(db.get(), "SELECT id FROM sources WHERE path=?");
        find.bind(1, path.string());
        if (find.row())
            source_mapping[source.at("id").get<Id>()] = find.number(0);
        else {
            if (!path.is_absolute() || path.lexically_normal() != path)
                throw std::runtime_error("Invalid source path in export");
            Statement roots(db.get(), "SELECT path FROM sources");
            bool overlap = false;
            while (roots.row())
                if (contains(fs::path(roots.text(0)), path) ||
                    contains(path, fs::path(roots.text(0))))
                    overlap = true;
            if (overlap)
                continue;
            struct stat st{};
            const bool available = ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
            Statement add(
                db.get(),
                "INSERT INTO "
                "sources(path,recursive,exclusions,active,availability,device,inode,watch_enabled) "
                "VALUES(?,?,?,?,?,?,?,0)");
            add.bind(1, path.string())
                .bind(2, source.value("recursive", true))
                .bind(3, exclusions.dump())
                .bind(4, source.value("active", true))
                .bind(5, available ? "online" : "offline")
                .bind(6, available ? static_cast<Id>(st.st_dev) : source.value("device", Id{0}))
                .bind(7, available ? static_cast<Id>(st.st_ino) : source.value("inode", Id{0}))
                .run();
            source_mapping[source.at("id").get<Id>()] = db.last_id();
            sources_requiring_refresh.push_back(db.last_id());
        }
    }
    db.exec(
        "INSERT OR IGNORE INTO source_directories(source_id,relative_path) SELECT id,'' FROM "
        "sources");
    // Restore only established identity matches; filename/hash alone never attaches edits.
    // Registration restores the explicit source map without starting discovery. Only
    // exact identity matches are imported; missing originals remain in the restore report.
    for (const auto& photo : data["photos"]) {
        const auto original = fs::path(photo.at("path").get<std::string>());
        const fs::path path = map_path(original);
        if (!path.is_absolute() || path.lexically_normal() != path ||
            path.string().find('\0') != std::string::npos)
            throw std::runtime_error("Invalid original path in export");
        const Id exported_source = photo.at("source_id").get<Id>();
        if (!source_mapping.contains(exported_source)) {
            missing.push_back(original.string());
            continue;
        }
        const Id source_id = source_mapping.at(exported_source);
        Statement root_query(db.get(), "SELECT path FROM sources WHERE id=?");
        root_query.bind(1, source_id);
        (void)root_query.row();
        const fs::path root = root_query.text(0);
        if (!contains(root, path) || path == root)
            throw std::runtime_error("Exported photograph is outside its source");
        const auto relative = path.lexically_relative(root).string();
        Statement candidate(db.get(),
                            "SELECT id,device,inode,size,mtime,mtime_ns FROM photos WHERE "
                            "source_id=? AND relative_path=? AND availability<>'replaced'");
        candidate.bind(1, source_id).bind(2, relative);
        Id id = 0;
        if (candidate.row()) {
            id = candidate.number(0);
            const bool same = candidate.number(1) == photo.at("device").get<Id>() &&
                              candidate.number(2) == photo.at("inode").get<Id>() &&
                              candidate.number(3) == photo.at("size").get<Id>() &&
                              candidate.number(4) == photo.at("mtime").get<Id>() &&
                              candidate.number(5) == photo.at("mtime_subsecond").get<Id>();
            if (!same) {
                ambiguous.push_back(original.string());
                continue;
            }
        } else {
            struct stat st{};
            if (::lstat(path.c_str(), &st) != 0) {
                missing.push_back(original.string());
                continue;
            }
            if (!S_ISREG(st.st_mode) ||
                static_cast<Id>(st.st_dev) != photo.at("device").get<Id>() ||
                static_cast<Id>(st.st_ino) != photo.at("inode").get<Id>() ||
                static_cast<Id>(st.st_size) != photo.at("size").get<Id>() ||
                static_cast<Id>(st.st_mtim.tv_sec) != photo.at("mtime").get<Id>() ||
                static_cast<Id>(st.st_mtim.tv_nsec) != photo.at("mtime_subsecond").get<Id>()) {
                ambiguous.push_back(original.string());
                continue;
            }
            const auto date = photo.value("extracted_date", Json(nullptr));
            if (!date.is_null() && !valid_date(date.get<std::string>()))
                throw std::runtime_error("Invalid extracted date in export");
            Statement insert(
                db.get(),
                "INSERT INTO "
                "photos(source_id,relative_path,filename,device,inode,size,mtime,mtime_ns,capture_"
                "date,width,height,camera,metadata_signature) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)");
            insert.bind(1, source_id)
                .bind(2, relative)
                .bind(3, path.filename().string())
                .bind(4, photo.at("device"))
                .bind(5, photo.at("inode"))
                .bind(6, photo.at("size"))
                .bind(7, photo.at("mtime"))
                .bind(8, photo.at("mtime_subsecond"))
                .bind(9, date)
                .bind(10, photo.value("width", Id{0}))
                .bind(11, photo.value("height", Id{0}))
                .bind(12, photo.value("camera", std::string{}))
                .bind(13, signature(st))
                .run();
            id = db.last_id();
        }
        Json changes{{"ids", Json::array({id})},
                     {"description", photo.at("description")},
                     {"rating", photo.at("rating")},
                     {"capture_date", photo.value("date_override", Json(nullptr))},
                     {"add_tags", photo.at("tags")},
                     {"add_albums", Json::array()}};
        for (const auto& album : photo.at("albums")) {
            changes["add_albums"].push_back(
                find_or_create_album(db, checked_text(album, "name", 512)));
        }
        (void)edit(db, changes);
        restored.push_back(id);
        photo_mapping[photo.at("id").get<Id>()] = id;
    }
    for (const auto& album : data.value("albums", Json::array())) {
        const Id cover = album.value("cover_id", Id{});
        if (cover <= 0 || !photo_mapping.contains(cover))
            continue;  // Old exports and unresolved identities preserve the existing choice.
        Statement save(db.get(),
                       "INSERT INTO album_covers(album_id,photo_id) SELECT album_id,photo_id "
                       "FROM memberships WHERE album_id=? AND photo_id=? ON CONFLICT(album_id) "
                       "DO UPDATE SET photo_id=excluded.photo_id");
        save.bind(1, album_mapping.at(album.at("id").get<Id>()))
            .bind(2, photo_mapping.at(cover))
            .run();
    }
    for (const auto& filter : data.value("filters", Json::array())) {
        const auto name = checked_text(filter, "name", 512);
        auto definition = filter.at("filter");
        if (name.empty() || !definition.is_object())
            throw std::runtime_error("Invalid saved filter in export");
        bool resolved = true;
        const Id source = definition.value("source_id", Id{0}),
                 album = definition.value("album_id", Id{0});
        if (source > 0) {
            if (source_mapping.contains(source))
                definition["source_id"] = source_mapping.at(source);
            else
                resolved = false;
        }
        if (album > 0) {
            if (album_mapping.contains(album))
                definition["album_id"] = album_mapping.at(album);
            else
                resolved = false;
        }
        if (!resolved) {
            unresolved_filters.push_back(name);
            continue;
        }
        Statement save(db.get(),
                       "INSERT INTO filters(name,definition) VALUES(?,?) ON CONFLICT(name) DO "
                       "UPDATE SET definition=excluded.definition");
        save.bind(1, name).bind(2, definition.dump()).run();
    }
    transaction.commit();
    return {{"restored", restored.size()},
            {"restored_ids", restored},
            {"missing", missing},
            {"ambiguous", ambiguous},
            {"unresolved_filters", unresolved_filters},
            {"sources_requiring_refresh", sources_requiring_refresh},
            {"merge_policy", "replace_scalar_annotations_merge_tags_and_albums"}};
}

Json operation_prepare(Database& db, const Json& request) {
    if (request.at("kind") != "trash")
        throw std::runtime_error("Unsupported photo operation");
    auto ids = request.at("ids").get<std::vector<Id>>();
    const auto keepers = request.at("keeper_ids").get<std::vector<Id>>();
    if (ids.empty() || keepers.empty())
        throw std::runtime_error("Choose copies to keep and copies to trash");
    const std::set<Id> trash(ids.begin(), ids.end());
    for (Id id : keepers) {
        if (trash.contains(id))
            throw std::runtime_error("A keeper cannot be trashed");
        ids.push_back(id);
    }
    if (std::set<Id>(ids.begin(), ids.end()).size() != ids.size())
        throw std::runtime_error("Duplicate review contains repeated photo identities");
    auto verified = verify_duplicates(db, {{"ids", ids}});
    if (request.contains("reviewed")) {
        const auto& reviewed = request.at("reviewed");
        if (!reviewed.is_array() || reviewed.size() > 1000)
            throw std::runtime_error("Invalid duplicate-review snapshot");
        for (const auto& photo : verified.at("photos")) {
            const auto expected =
                std::find_if(reviewed.begin(), reviewed.end(), [&](const Json& row) {
                    return row.is_object() && row.contains("id") && row.at("id") == photo.at("id");
                });
            if (expected == reviewed.end())
                throw std::runtime_error(
                    "The reviewed photo selection changed; reopen duplicate review");
            for (const auto* field : {"path", "signature", "device", "inode", "size", "mtime_ns"})
                if (!expected->contains(field) || expected->at(field) != photo.at(field))
                    throw std::runtime_error(
                        "An original changed since duplicate review was displayed; refresh and "
                        "review it again");
        }
    }
    Json captured = Json::array(), keeper_rows = Json::array();
    for (const auto& photo : verified["photos"]) {
        if (trash.contains(photo["id"].get<Id>()))
            captured.push_back(photo);
        else
            keeper_rows.push_back(photo);
    }
    Json payload{{"photos", captured}, {"keepers", keeper_rows}, {"results", Json::array()}};
    Statement insert(db.get(), "INSERT INTO operations(kind,payload) VALUES('trash',?)");
    insert.bind(1, payload.dump()).run();
    return {{"operation_id", db.last_id()}, {"photos", captured}, {"keepers", keeper_rows}};
}

void remember_receipt(Database& db, Id photo, Id operation, const std::string& path,
                      const std::string& identity) {
    Statement save(db.get(),
                   "INSERT INTO photo_receipts(photo_id,operation_id,path,signature) "
                   "VALUES(?,?,?,?) ON CONFLICT(photo_id) DO UPDATE SET "
                   "operation_id=excluded.operation_id,path=excluded.path,signature=excluded."
                   "signature WHERE excluded.operation_id>=photo_receipts.operation_id");
    save.bind(1, photo).bind(2, operation).bind(3, path).bind(4, identity).run();
}

Json operation_restore_prepare(Database& db, const Json& request) {
    const auto ids = request.at("ids").get<std::vector<Id>>();
    if (ids.empty())
        throw std::runtime_error("Choose trashed photographs to restore");
    Json captured = Json::array();
    for (const auto id : ids) {
        Statement photo(db.get(), "SELECT " + photo_columns +
                                      " FROM photos p JOIN sources s ON s.id=p.source_id WHERE "
                                      "p.id=? AND p.availability='trashed'");
        photo.bind(1, id);
        if (!photo.row())
            throw std::runtime_error("A selected photograph is not in the catalog trash");
        auto item = photo_json(db, photo);
        Statement receipt(db.get(), "SELECT path,signature FROM photo_receipts WHERE photo_id=?");
        receipt.bind(1, id);
        if (!receipt.row())
            throw std::runtime_error("No confirmed trash receipt is available for this photograph");
        const auto destination = receipt.text(0), expected = receipt.text(1);
        struct stat st{};
        if (destination.empty() || expected.empty() || ::lstat(destination.c_str(), &st) != 0 ||
            !S_ISREG(st.st_mode) || signature(st) != expected)
            throw std::runtime_error(
                "The trash receipt no longer matches the original; restore requires review");
        const auto timestamp =
            nanoseconds(static_cast<Id>(st.st_mtim.tv_sec), static_cast<Id>(st.st_mtim.tv_nsec));
        if (!timestamp)
            throw std::runtime_error(
                "Original timestamp is outside the supported range for guarded file operations");
        item["original_path"] = item["path"];
        item["path"] = destination;
        item["signature"] = expected;
        item["device"] = static_cast<Id>(st.st_dev);
        item["inode"] = static_cast<Id>(st.st_ino);
        item["size"] = static_cast<Id>(st.st_size);
        item["mtime"] = static_cast<Id>(st.st_mtim.tv_sec);
        item["mtime_ns"] = *timestamp;
        captured.push_back(std::move(item));
    }
    Json payload{{"photos", captured}, {"keepers", Json::array()}, {"results", Json::array()}};
    Statement insert(db.get(), "INSERT INTO operations(kind,payload) VALUES('restore',?)");
    insert.bind(1, payload.dump()).run();
    return {{"operation_id", db.last_id()}, {"photos", captured}};
}

Json operation_complete(Database& db, const Json& request) {
    const Id id = request.at("operation_id").get<Id>();
    Transaction transaction(db);
    Statement operation(db.get(), "SELECT payload,state,kind FROM operations WHERE id=?");
    operation.bind(1, id);
    if (!operation.row())
        throw std::runtime_error("Unknown photo operation");
    auto payload = Json::parse(operation.text(0));
    if (operation.text(1) == "completed") {
        transaction.commit();
        return {{"changed", 0}, {"partial", false}};
    }
    const bool restoring = operation.text(2) == "restore";
    auto merged = payload.value("results", Json::array());
    for (auto result : request.at("results")) {
        const Id photo_id = result.at("id").get<Id>();
        const auto found = std::find_if(payload["photos"].begin(), payload["photos"].end(),
                                        [&](const Json& p) { return p.at("id") == photo_id; });
        if (found == payload["photos"].end())
            throw std::runtime_error("Operation result does not match its captured selection");
        auto previous = std::find_if(merged.begin(), merged.end(),
                                     [&](const Json& r) { return r.at("id") == photo_id; });
        if (previous != merged.end() && previous->value("success", false))
            continue;
        if (result.value("success", false)) {
            const auto destination = result.value("destination", std::string{});
            struct stat st{};
            if (destination.empty() || ::lstat(destination.c_str(), &st) != 0 ||
                !S_ISREG(st.st_mode) ||
                signature(st) != found->at("signature").get<std::string>() ||
                static_cast<Id>(st.st_size) != found->at("size").get<Id>() ||
                nanoseconds(static_cast<Id>(st.st_mtim.tv_sec),
                            static_cast<Id>(st.st_mtim.tv_nsec)) !=
                    std::optional<Id>{found->at("mtime_ns").get<Id>()}) {
                result["success"] = false;
                result["error"] =
                    "Filesystem completed but destination identity changed; review the operation";
            } else {
                result["receipt_signature"] = signature(st);
                if (restoring) {
                    Statement source(db.get(),
                                     "SELECT s.path FROM photos p JOIN sources s ON "
                                     "s.id=p.source_id WHERE p.id=?");
                    source.bind(1, photo_id);
                    (void)source.row();
                    const fs::path root = source.text(0);
                    if (!contains(root, fs::path(destination))) {
                        result["success"] = false;
                        result["error"] = "Restored outside its source; review the destination";
                    } else {
                        Statement update(db.get(),
                                         "UPDATE photos SET "
                                         "relative_path=?,filename=?,device=?,inode=?,size=?,mtime="
                                         "?,mtime_ns=?,availability='online' WHERE id=?");
                        update.bind(1, fs::path(destination).lexically_relative(root).string())
                            .bind(2, fs::path(destination).filename().string())
                            .bind(3, static_cast<Id>(st.st_dev))
                            .bind(4, static_cast<Id>(st.st_ino))
                            .bind(5, static_cast<Id>(st.st_size))
                            .bind(6, static_cast<Id>(st.st_mtim.tv_sec))
                            .bind(7, static_cast<Id>(st.st_mtim.tv_nsec))
                            .bind(8, photo_id)
                            .run();
                        update_search(db, photo_id);
                    }
                } else {
                    Statement update(db.get(),
                                     "UPDATE photos SET availability='trashed' WHERE id=?");
                    update.bind(1, photo_id).run();
                    remember_receipt(db, photo_id, id, destination, signature(st));
                }
            }
        }
        if (previous != merged.end())
            *previous = std::move(result);
        else
            merged.push_back(std::move(result));
    }
    const bool failed = std::any_of(merged.begin(), merged.end(),
                                    [](const Json& r) { return !r.value("success", false); });
    const bool partial = merged.size() != payload["photos"].size();
    payload["results"] = merged;
    Statement update(db.get(), "UPDATE operations SET state=?,payload=?,error=? WHERE id=?");
    update.bind(1, partial  ? "pending"
                   : failed ? "failed"
                            : "completed")
        .bind(2, payload.dump())
        .bind(3, partial  ? "Some filesystem results are still missing"
                 : failed ? "One or more filesystem results require review"
                          : "")
        .bind(4, id)
        .run();
    transaction.commit();
    return {{"changed", merged.size()}, {"partial", partial}, {"failed", failed}};
}

Json operations(Database& db) {
    Statement statement(
        db.get(), "SELECT id,kind,state,payload,error FROM operations ORDER BY id DESC LIMIT 1000");
    Json result = Json::array();
    while (statement.row()) {
        auto row = Json::parse(statement.text(3));
        row["id"] = statement.number(0);
        row["kind"] = statement.text(1);
        row["state"] = statement.text(2);
        row["error"] = statement.text(4);
        result.push_back(std::move(row));
    }
    return {{"operations", result}};
}

Json recovery_operations(Database& db, const Json& request) {
    const Id after = std::max(request.value("after_id", Id{0}), Id{0});
    const Id limit = std::clamp(request.value("limit", Id{128}), Id{1}, Id{1000});
    Statement statement(db.get(),
                        "SELECT id,kind,state,payload,error FROM operations WHERE id>? AND "
                        "state IN ('pending','needs_review') AND kind IN ('trash','restore') "
                        "ORDER BY id LIMIT ?");
    statement.bind(1, after).bind(2, limit + 1);
    Json result = Json::array();
    Id next = after;
    bool more = false;
    while (statement.row()) {
        if (static_cast<Id>(result.size()) == limit) {
            more = true;
            break;
        }
        auto row = Json::parse(statement.text(3));
        next = statement.number(0);
        row["id"] = next;
        row["kind"] = statement.text(1);
        row["state"] = statement.text(2);
        row["error"] = statement.text(4);
        result.push_back(std::move(row));
    }
    // ID cursors remain stable as each recovered operation leaves the pending set.
    return {{"operations", result}, {"next_after_id", next}, {"more", more}};
}

Json reconcile_locations(Database& db, const Json& request) {
    const auto kind = request.at("kind").get<std::string>();
    if (kind != "move" && kind != "rename" && kind != "trash" && kind != "restore")
        throw std::runtime_error("Unsupported location operation");
    Json changed = Json::array(), review = Json::array(), unresolved = Json::array();
    for (const auto& item : request.at("items")) {
        const fs::path from = item.at("from").get<std::string>(),
                       to = item.at("to").get<std::string>();
        if (!from.is_absolute() || !to.is_absolute())
            throw std::runtime_error("Confirmed locations must be absolute");
        if (kind == "move" || kind == "rename") {
            Transaction root_change(db);
            Statement known(db.get(), "SELECT id,path,device,inode FROM sources");
            while (known.row()) {
                const fs::path old_root = known.text(1);
                if (!contains(from, old_root))
                    continue;
                const fs::path destination =
                    old_root == from ? to : to / old_root.lexically_relative(from);
                struct stat st{};
                if (::stat(destination.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) ||
                    static_cast<Id>(st.st_dev) != known.number(2) ||
                    static_cast<Id>(st.st_ino) != known.number(3))
                    continue;
                Statement others(db.get(), "SELECT path FROM sources WHERE id<>?");
                others.bind(1, known.number(0));
                bool overlaps = false;
                while (others.row()) {
                    const fs::path other = others.text(0);
                    if (!contains(from, other) &&
                        (contains(other, destination) || contains(destination, other)))
                        overlaps = true;
                }
                if (overlaps)
                    continue;
                Statement update(db.get(),
                                 "UPDATE sources SET path=?,availability='online' WHERE id=?");
                update.bind(1, destination.string()).bind(2, known.number(0)).run();
                Statement photos(
                    db.get(),
                    "SELECT id FROM photos WHERE source_id=? AND availability<>'replaced'");
                photos.bind(1, known.number(0));
                while (photos.row())
                    changed.push_back(photos.number(0));
            }
            root_change.commit();
        }
        Json matches = Json::array();
        Statement roots(db.get(), "SELECT id,path FROM sources");
        while (roots.row()) {
            const fs::path root = roots.text(1);
            if (!contains(root, from) && !contains(from, root))
                continue;
            const bool whole_source = contains(from, root);
            const auto relative = from.lexically_relative(root).string();
            const std::string restriction =
                whole_source
                    ? ""
                    : " AND (p.relative_path=? OR (p.relative_path>=? AND p.relative_path<?))";
            Statement candidates(db.get(),
                                 "SELECT " + photo_columns +
                                     " FROM photos p JOIN sources s ON s.id=p.source_id WHERE "
                                     "p.source_id=? AND p.availability<>'replaced'" +
                                     restriction);
            candidates.bind(1, roots.number(0));
            if (!whole_source)
                candidates.bind(2, relative).bind(3, relative + "/").bind(4, relative + "0");
            while (candidates.row())
                matches.push_back(photo_json(db, candidates));
        }
        if (kind == "restore") {
            Statement receipts(db.get(),
                               "SELECT photo_id,signature FROM photo_receipts WHERE path=?");
            receipts.bind(1, from.string());
            while (receipts.row()) {
                Statement photo(db.get(), "SELECT " + photo_columns +
                                              " FROM photos p JOIN sources s ON s.id=p.source_id "
                                              "WHERE p.id=? AND p.availability='trashed'");
                photo.bind(1, receipts.number(0));
                if (photo.row()) {
                    auto value = photo_json(db, photo);
                    value["path"] = from.string();
                    value["signature"] = receipts.text(1);
                    matches.push_back(std::move(value));
                }
            }
        }
        for (const auto& photo : matches) {
            const fs::path old = photo.at("path").get<std::string>();
            const fs::path destination = old == from ? to : to / old.lexically_relative(from);
            struct stat st{};
            if (::lstat(destination.c_str(), &st) != 0 || !S_ISREG(st.st_mode) ||
                signature(st) != photo.at("signature").get<std::string>()) {
                review.push_back(
                    {{"id", photo["id"]},
                     {"path", destination.string()},
                     {"reason", "Destination identity differs from the indexed original"}});
                unresolved.push_back(photo);
                continue;
            }
            Transaction transaction(db);
            if (kind == "trash") {
                Json captured = photo;
                Json result{{"id", photo["id"]},
                            {"success", true},
                            {"destination", destination.string()},
                            {"receipt_signature", signature(st)}};
                Json payload{{"photos", Json::array({captured})},
                             {"keepers", Json::array()},
                             {"results", Json::array({result})}};
                Statement receipt(
                    db.get(),
                    "INSERT INTO operations(kind,state,payload) VALUES('trash','completed',?)");
                receipt.bind(1, payload.dump()).run();
                remember_receipt(db, photo.at("id").get<Id>(), db.last_id(), destination.string(),
                                 signature(st));
                Statement update(db.get(), "UPDATE photos SET availability='trashed' WHERE id=?");
                update.bind(1, photo["id"]).run();
            } else {
                Statement source(
                    db.get(),
                    "SELECT id,path FROM sources WHERE active=1 ORDER BY length(path) DESC");
                Id source_id = 0;
                fs::path root;
                while (source.row())
                    if (contains(fs::path(source.text(1)), destination)) {
                        source_id = source.number(0);
                        root = source.text(1);
                        break;
                    }
                if (source_id == 0) {
                    review.push_back({{"id", photo["id"]},
                                      {"path", destination.string()},
                                      {"reason", "Destination is outside registered sources"}});
                    unresolved.push_back(photo);
                    transaction.commit();
                    continue;
                }
                Statement update(
                    db.get(),
                    "UPDATE photos SET "
                    "source_id=?,relative_path=?,filename=?,availability='online' WHERE id=?");
                update.bind(1, source_id)
                    .bind(2, destination.lexically_relative(root).string())
                    .bind(3, destination.filename().string())
                    .bind(4, photo["id"])
                    .run();
                update_search(db, photo["id"].get<Id>());
            }
            transaction.commit();
            changed.push_back(photo["id"]);
        }
    }
    if (!review.empty()) {
        Json results = Json::array();
        for (const auto& item : review)
            results.push_back({{"id", item.at("id")},
                               {"success", false},
                               {"destination", item.at("path")},
                               {"error", item.at("reason")}});
        const Json payload{{"photos", unresolved},
                           {"keepers", Json::array()},
                           {"results", results},
                           {"location_kind", kind}};
        Statement journal(db.get(),
                          "INSERT INTO operations(kind,state,payload,error) "
                          "VALUES('location','needs_review',?,'One or more moved originals need "
                          "identity review')");
        journal.bind(1, payload.dump()).run();
    }
    return {{"changed", changed.size()}, {"review", review}};
}

Json reconcile_operations(Database& db) {
    Statement pending(db.get(), "SELECT id,payload FROM operations WHERE state='pending'");
    Json reports = Json::array();
    while (pending.row()) {
        const auto payload = Json::parse(pending.text(1));
        Json report{
            {"id", pending.number(0)}, {"state", "needs_review"}, {"photos", Json::array()}};
        for (const auto& photo : payload.at("photos")) {
            struct stat st{};
            const auto path = photo.at("path").get<std::string>();
            std::string state;
            if (::lstat(path.c_str(), &st) == 0)
                state = signature(st) == photo.at("signature").get<std::string>()
                            ? "original_present"
                            : "original_changed";
            else
                state = errno == ENOENT ? "original_missing" : "unavailable";
            report["photos"].push_back({{"id", photo.at("id")}, {"path", path}, {"state", state}});
        }
        // Missing is not proof of trash completion. Never repeat filesystem operations;
        // a receipt or explicit confirmed transfer result must settle the pending intent.
        reports.push_back(std::move(report));
    }
    return {{"operations", reports}};
}

Json relink(Database& db, const Json& request) {
    const Id source = request.at("source_id").get<Id>();
    const auto new_root = fs::canonical(fs::path(checked_text(request, "path")));
    if (!fs::is_directory(new_root))
        throw std::runtime_error("Choose an available source folder");
    Statement overlap(db.get(), "SELECT path FROM sources WHERE id<>?");
    overlap.bind(1, source);
    while (overlap.row())
        if (contains(fs::path(overlap.text(0)), new_root) ||
            contains(new_root, fs::path(overlap.text(0))))
            throw std::runtime_error("Relink would overlap another source");
    Statement photos(
        db.get(),
        "SELECT id,relative_path,device,inode,size,mtime,mtime_ns FROM photos WHERE source_id=?");
    photos.bind(1, source);
    Json matched = Json::array(), missing = Json::array(), ambiguous = Json::array();
    while (photos.row()) {
        struct stat st{};
        const auto path = new_root / photos.text(1);
        if (::lstat(path.c_str(), &st) != 0) {
            missing.push_back(photos.number(0));
            continue;
        }
        if (static_cast<Id>(st.st_dev) == photos.number(2) &&
            static_cast<Id>(st.st_ino) == photos.number(3) &&
            static_cast<Id>(st.st_size) == photos.number(4) &&
            static_cast<Id>(st.st_mtim.tv_sec) == photos.number(5) &&
            static_cast<Id>(st.st_mtim.tv_nsec) == photos.number(6))
            matched.push_back(photos.number(0));
        else
            ambiguous.push_back(photos.number(0));
    }
    if (request.value("confirm", false)) {
        if (!ambiguous.empty() || matched.empty())
            throw std::runtime_error(
                "Relink has ambiguous identities; original source mapping was preserved");
        struct stat root{};
        if (::stat(new_root.c_str(), &root) != 0)
            throw std::runtime_error("Relink folder became unavailable");
        Statement update(
            db.get(),
            "UPDATE sources SET path=?,availability='online',device=?,inode=? WHERE id=?");
        update.bind(1, new_root.string())
            .bind(2, static_cast<Id>(root.st_dev))
            .bind(3, static_cast<Id>(root.st_ino))
            .bind(4, source)
            .run();
    }
    return {{"matched", matched},
            {"missing", missing},
            {"ambiguous", ambiguous},
            {"applied", request.value("confirm", false)}};
}

Json watch_paths(Database& db, const Json& request) {
    const auto source_rows = sources(db)["sources"];
    const auto exclusions = internal_exclusions(db, request);
    const Id limit = std::clamp(request.value("limit", Id{128}), Id{1}, Id{512});
    const Id after = std::max(request.value("after_id", Id{0}), Id{0});
    Statement statement(db.get(),
                        "SELECT d.id,d.source_id,s.path,d.relative_path FROM source_directories d "
                        "JOIN sources s ON s.id=d.source_id WHERE d.id>? AND s.active=1 AND "
                        "s.availability='online' AND s.watch_enabled=1 ORDER BY d.id LIMIT ?");
    statement.bind(1, after).bind(2, limit + 1);
    Json paths = Json::array();
    Id next = after, consumed = 0;
    bool more = false;
    while (statement.row()) {
        if (consumed == limit) {
            more = true;
            break;
        }
        ++consumed;
        next = statement.number(0);
        const fs::path path = statement.text(3).empty()
                                  ? fs::path(statement.text(2))
                                  : fs::path(statement.text(2)) / statement.text(3);
        if (internally_excluded(exclusions, path))
            continue;
        paths.push_back({{"id", next},
                         {"source_id", statement.number(1)},
                         {"path", path.lexically_normal().string()}});
    }
    Json excluded = Json::array();
    for (const auto& path : exclusions)
        excluded.push_back(path.string());
    return {
        {"paths", paths}, {"sources", source_rows},          {"next_after_id", next},
        {"more", more},   {"internal_exclusions", excluded}, {"extensions", photo_extensions()}};
}

Json dispatch(Database& db, const Json& request) {
    const auto op = request.at("op").get<std::string>();
    if (op == "catalog_identity") {
        Statement identity(db.get(), "SELECT identity FROM catalog_identity WHERE id=1");
        if (!identity.row())
            throw std::runtime_error("Photo catalog identity is unavailable");
        return {{"identity", identity.text(0)}};
    }
    if (op == "backup") {
        const auto path = request.value("path", db.path().string() + ".backup");
        db.backup(path);
        return {{"path", path}};
    }
    if (op == "background_state")
        return background_state(db);
    if (op == "background_control")
        return background_control(db, request);
    if (op == "watch_paths")
        return watch_paths(db, request);
    if (op == "sources")
        return sources(db);
    if (op == "add_source")
        return add_source(db, request);
    if (op == "query")
        return query(db, request);
    if (op == "query_ids")
        return query_ids(db, request);
    if (op == "validate_selection") {
        const auto& ids = request.at("ids");
        if (!ids.is_array() || ids.size() > 100000)
            throw std::runtime_error("Validate at most 100000 photograph IDs");
        std::set<Id> unique;
        for (const auto& value : ids) {
            if (!value.is_number_integer() ||
                (value.is_number_unsigned() &&
                 value.get<std::uint64_t>() >
                     static_cast<std::uint64_t>(std::numeric_limits<Id>::max())) ||
                value.get<Id>() <= 0)
                throw std::runtime_error("Photo selections must contain valid photograph IDs");
            unique.insert(value.get<Id>());
        }
        // One read statement gives a coherent snapshot and indexed primary-key probes even
        // for a selection spanning many pages. Retained/offline identities still exist;
        // only forgotten rows are stale. No original paths or metadata are read here.
        Statement missing(db.get(),
                          "SELECT selected.value FROM json_each(?) selected LEFT JOIN photos p "
                          "ON p.id=selected.value WHERE p.id IS NULL ORDER BY selected.value");
        missing.bind(1, Json(unique).dump());
        Json missing_ids = Json::array();
        while (missing.row())
            missing_ids.push_back(missing.number(0));
        return {{"count", unique.size() - missing_ids.size()}, {"missing_ids", missing_ids}};
    }
    if (op == "edit")
        return edit(db, request);
    if (op == "work")
        return work(db, request);
    if (op == "jobs")
        return jobs(db);
    if (op == "progress")
        return jobs(db, true);
    if (op == "job_control")
        return job_control(db, request);
    if (op == "metadata")
        return metadata(db, request);
    if (op == "metadata_batch")
        return metadata_batch(db, request);
    if (op == "metadata_claim")
        return metadata_claim(db, request);
    if (op == "metadata_pending")
        return {{"metadata", metadata_pending(db, request.value("limit", Id{64}))}};
    if (op == "export")
        return export_data(db, request);
    if (op == "restore")
        return restore(db, request);
    if (op == "duplicates")
        return duplicates(db, request);
    if (op == "verify_duplicates")
        return verify_duplicates(db, request);
    if (op == "operation_prepare")
        return operation_prepare(db, request);
    if (op == "operation_complete")
        return operation_complete(db, request);
    if (op == "operation_restore_prepare")
        return operation_restore_prepare(db, request);
    if (op == "operations")
        return operations(db);
    if (op == "recovery_operations")
        return recovery_operations(db, request);
    if (op == "reconcile_locations")
        return reconcile_locations(db, request);
    if (op == "reconcile_operations")
        return reconcile_operations(db);
    if (op == "relink_source")
        return relink(db, request);
    if (op == "refresh") {
        Transaction transaction(db);
        const Id id = request.value("source_id", Id{0});
        const bool reconcile = request.value("reconcile", false);
        const std::string eligible = reconcile ? " AND watch_enabled=1" : "";
        Statement all(db.get(), "SELECT id FROM sources WHERE active=1" + eligible +
                                    (id > 0 ? " AND id=?" : ""));
        if (id > 0)
            all.bind(1, id);
        Json ids = Json::array();
        while (all.row()) {
            if (!reconcile) {
                Statement enable(db.get(), "UPDATE sources SET watch_enabled=1 WHERE id=?");
                enable.bind(1, all.number(0)).run();
            }
            ids.push_back(queue_scan(db, all.number(0), reconcile));
        }
        transaction.commit();
        return {{"job_ids", ids}};
    }
    if (op == "remove_source" || op == "forget_source") {
        const Id id = request.value("source_id", request.value("id", Id{0}));
        Transaction transaction(db);
        Statement cancel(db.get(),
                         "UPDATE jobs SET state='cancelled' WHERE source_id=? AND state NOT IN "
                         "('completed','failed','cancelled')");
        cancel.bind(1, id).run();
        if (op == "remove_source") {
            Statement remove(db.get(), "UPDATE sources SET active=0 WHERE id=?");
            remove.bind(1, id).run();
        } else {
            std::set<Id> forgotten;
            Statement ids(db.get(), "SELECT id FROM photos WHERE source_id=?");
            ids.bind(1, id);
            while (ids.row())
                forgotten.insert(ids.number(0));
            Statement operations(db.get(), "SELECT id,payload FROM operations");
            while (operations.row()) {
                auto payload = Json::parse(operations.text(1));
                bool changed = false;
                for (const auto* key : {"photos", "keepers", "results"})
                    if (payload.contains(key)) {
                        auto& rows = payload[key];
                        const auto before = rows.size();
                        std::erase_if(rows.get_ref<Json::array_t&>(), [&](const Json& row) {
                            return row.is_number_integer()
                                       ? forgotten.contains(row.get<Id>())
                                       : row.contains("id") &&
                                             forgotten.contains(row["id"].get<Id>());
                        });
                        changed = changed || before != rows.size();
                    }
                if (!changed)
                    continue;
                if (payload.value("photos", Json::array()).empty()) {
                    Statement remove(db.get(), "DELETE FROM operations WHERE id=?");
                    remove.bind(1, operations.number(0)).run();
                } else {
                    Statement update(db.get(), "UPDATE operations SET payload=? WHERE id=?");
                    update.bind(1, payload.dump()).bind(2, operations.number(0)).run();
                }
            }
            Statement remove_search(db.get(),
                                    "DELETE FROM photo_search WHERE rowid IN (SELECT id FROM "
                                    "photos WHERE source_id=?)");
            remove_search.bind(1, id).run();
            Statement remove(db.get(), "DELETE FROM sources WHERE id=? AND active=0");
            remove.bind(1, id).run();
            if (sqlite3_changes(db.get()) == 0)
                throw std::runtime_error("Remove the source before forgetting its data");
        }
        transaction.commit();
        return {{"changed", 1}};
    }
    if (op == "albums") {
        Statement all(db.get(),
                      "SELECT a.id,a.name,sum(CASE WHEN s.active=1 AND p.availability<>'replaced' "
                      "THEN 1 ELSE 0 END),"
                      "sum(CASE WHEN p.id IS NOT NULL AND (s.active=0 OR "
                      "p.availability='replaced') THEN 1 ELSE 0 END),"
                      "COALESCE((SELECT photo_id FROM album_covers WHERE album_id=a.id),0) "
                      "FROM albums a LEFT JOIN memberships m ON m.album_id=a.id LEFT JOIN photos p "
                      "ON p.id=m.photo_id "
                      "LEFT JOIN sources s ON s.id=p.source_id GROUP BY a.id ORDER BY a.name");
        Json items = Json::array();
        while (all.row()) {
            // Snapshot previews remain usable when a source is offline. Changed, replaced,
            // missing and trashed identities must not become covers of different originals.
            const auto read_cover = [&](bool chosen) -> Json {
                Statement cover(db.get(),
                                "SELECT " + photo_columns +
                                    " FROM memberships m JOIN photos p ON p.id=m.photo_id "
                                    "JOIN sources s ON s.id=p.source_id WHERE m.album_id=? "
                                    "AND s.active=1 AND p.availability='online' " +
                                    (chosen ? "AND m.photo_id=? " : "") +
                                    "ORDER BY m.photo_id LIMIT 1");
                cover.bind(1, all.number(0));
                if (chosen)
                    cover.bind(2, all.number(4));
                return cover.row() ? photo_json(db, cover, false) : Json{};
            };
            Json photo = all.number(4) > 0 ? read_cover(true) : Json{};
            if (photo.is_null())
                photo = read_cover(false);
            items.push_back({{"id", all.number(0)},
                             {"name", all.text(1)},
                             {"count", all.number(2)},
                             {"retained_count", all.number(3)},
                             {"custom_cover_id", all.number(4)},
                             {"cover_id", photo.is_null() ? Id{} : photo.at("id").get<Id>()},
                             {"cover", std::move(photo)}});
        }
        return {{"albums", items}};
    }
    if (op == "album_add_paths") {
        const Id album = request.at("id").get<Id>();
        const auto& paths = request.at("paths");
        if (!paths.is_array() || paths.empty() || paths.size() > 100000)
            throw std::runtime_error("Copy between 1 and 100000 photo files to add to the album");
        Transaction transaction(db);
        Statement exists(db.get(), "SELECT id FROM albums WHERE id=?");
        exists.bind(1, album);
        if (album <= 0 || !exists.row())
            throw std::runtime_error("Album no longer exists");
        std::map<fs::path, Id> roots;
        Statement sources(db.get(), "SELECT path,id FROM sources WHERE active=1");
        while (sources.row())
            roots.emplace(fs::path{sources.text(0)}.lexically_normal(), sources.number(1));
        std::set<Id> ids;
        for (const auto& value : paths) {
            if (!value.is_string())
                throw std::runtime_error("Copy photo files to add to an album");
            const fs::path path = fs::path{value.get<std::string>()}.lexically_normal();
            if (!path.is_absolute())
                throw std::runtime_error("Photo paths must be absolute");
            auto root = path.parent_path();
            while (!roots.contains(root) && root != root.parent_path())
                root = root.parent_path();
            if (!roots.contains(root))
                throw std::runtime_error(
                    "A copied photo is not in Photo Library. Add its source folder first.");
            Statement photo(db.get(),
                            "SELECT id FROM photos WHERE source_id=? AND relative_path=? "
                            "AND availability NOT IN ('replaced','trashed')");
            photo.bind(1, roots.at(root)).bind(2, path.lexically_relative(root).generic_string());
            if (!photo.row())
                throw std::runtime_error(
                    "A copied photo is not in Photo Library. Refresh or add its source folder "
                    "first.");
            ids.insert(photo.number(0));
        }
        for (const Id id : ids) {
            Statement add(db.get(),
                          "INSERT OR IGNORE INTO memberships(photo_id,album_id) VALUES(?,?)");
            add.bind(1, id).bind(2, album).run();
        }
        transaction.commit();
        return {{"id", album}, {"count", ids.size()}};
    }
    if (op == "rename_album" || op == "delete_album" || op == "set_album_cover") {
        const Id id = request.at("id").get<Id>();
        Transaction transaction(db);
        Statement exists(db.get(), "SELECT id FROM albums WHERE id=?");
        exists.bind(1, id);
        if (id <= 0 || !exists.row())
            throw std::runtime_error("Album no longer exists");
        if (op == "rename_album") {
            const auto name = checked_text(request, "name", 512);
            if (name.empty())
                throw std::runtime_error("Album name cannot be empty");
            Statement rename(db.get(), "UPDATE albums SET name=? WHERE id=?");
            rename.bind(1, name).bind(2, id).run();
        } else if (op == "delete_album") {
            // Advance even for albums created by an older add-on before deleting their IDs.
            db.exec(
                "UPDATE album_sequence SET next_id=max(next_id,"
                "COALESCE((SELECT max(id)+CASE WHEN max(id)<9223372036854775807 THEN 1 "
                "ELSE 0 END FROM albums),1)) WHERE id=1");
            Statement remove(db.get(), "DELETE FROM albums WHERE id=?");
            remove.bind(1, id).run();
        } else {
            const auto value = request.at("photo_id");
            if (!value.is_null() && !value.is_number_integer())
                throw std::runtime_error("Choose a photograph from this album");
            const Id photo = value.is_null() ? 0 : value.get<Id>();
            if (photo < 0)
                throw std::runtime_error("Choose a photograph from this album");
            if (photo == 0) {
                Statement clear(db.get(), "DELETE FROM album_covers WHERE album_id=?");
                clear.bind(1, id).run();
            } else {
                Statement member(db.get(),
                                 "SELECT p.id FROM memberships m JOIN photos p ON p.id=m.photo_id "
                                 "JOIN sources s ON s.id=p.source_id WHERE m.album_id=? AND "
                                 "m.photo_id=? AND s.active=1 AND p.availability='online'");
                member.bind(1, id).bind(2, photo);
                if (!member.row())
                    throw std::runtime_error("Choose an available photograph from this album");
                Statement save(db.get(),
                               "INSERT INTO album_covers(album_id,photo_id) VALUES(?,?) "
                               "ON CONFLICT(album_id) DO UPDATE SET photo_id=excluded.photo_id");
                save.bind(1, id).bind(2, photo).run();
            }
        }
        transaction.commit();
        return {{"id", id}, {"changed", 1}};
    }
    if (op == "create_album") {
        const auto name = checked_text(request, "name", 512);
        if (name.empty())
            throw std::runtime_error("Album name cannot be empty");
        const auto ids = request.value("ids", Json::array());
        if (!ids.is_array() || ids.size() > 100000)
            throw std::runtime_error("Choose at most 100000 photographs for the album");
        std::set<Id> unique;
        for (const auto& value : ids) {
            if (!value.is_number_integer() || value.get<Id>() <= 0)
                throw std::runtime_error("Album selections must contain valid photograph IDs");
            unique.insert(value.get<Id>());
        }
        Transaction transaction(db);
        const auto album = create_album(db, name);
        for (const auto id : unique) {
            Statement exists(db.get(), "SELECT id FROM photos WHERE id=?");
            exists.bind(1, id);
            if (!exists.row())
                throw std::runtime_error("A selected photograph no longer exists in the catalog");
            Statement add(db.get(), "INSERT INTO memberships(photo_id,album_id) VALUES(?,?)");
            add.bind(1, id).bind(2, album).run();
        }
        transaction.commit();
        return {{"id", album}, {"name", name}, {"count", unique.size()}};
    }
    if (op == "save_filter") {
        const auto name = checked_text(request, "name", 512);
        if (name.empty() || !request.at("filter").is_object())
            throw std::runtime_error("Invalid saved filter");
        Statement save(db.get(),
                       "INSERT INTO filters(name,definition) VALUES(?,?) ON CONFLICT(name) DO "
                       "UPDATE SET definition=excluded.definition");
        save.bind(1, name).bind(2, request.at("filter").dump()).run();
        return {{"saved", true}};
    }
    if (op == "filters") {
        Statement all(db.get(), "SELECT id,name,definition FROM filters ORDER BY name");
        Json items = Json::array();
        while (all.row())
            items.push_back({{"id", all.number(0)},
                             {"name", all.text(1)},
                             {"filter", Json::parse(all.text(2))}});
        return {{"filters", items}};
    }
    if (op == "find_duplicates") {
        Transaction transaction(db);
        Statement existing(db.get(),
                           "SELECT id FROM jobs WHERE kind='duplicates' AND state IN "
                           "('queued','running','paused')");
        if (existing.row()) {
            const Id id = existing.number(0);
            transaction.commit();
            return {{"job_id", id}};
        }
        db.exec("INSERT INTO jobs(kind) VALUES('duplicates')");
        const Id id = db.last_id();
        transaction.commit();
        return {{"job_id", id}};
    }
    throw std::runtime_error("Unknown photo catalog request");
}
}  // namespace

extern "C" int rummage_photos_version() { return 1; }

extern "C" char* rummage_photos_request(const char* catalog_path, const char* request_json) {
    std::string response;
    try {
        if (!catalog_path || !request_json)
            throw std::runtime_error("Missing catalog request");
        const auto request = Json::parse(request_json);
        if (!request.is_object())
            throw std::runtime_error("Catalog request must be an object");
        Database db(catalog_path);
        auto result = dispatch(db, request);
        result["ok"] = true;
        response = result.dump();
    } catch (const std::exception& error) {
        try {
            response = Json({{"ok", false}, {"error", error.what()}}).dump();
        } catch (...) {
            return nullptr;
        }
    } catch (...) {
        return nullptr;
    }
    auto* result = static_cast<char*>(std::malloc(response.size() + 1));
    if (result)
        std::memcpy(result, response.c_str(), response.size() + 1);
    return result;
}

extern "C" void rummage_photos_free(char* response) { std::free(response); }
