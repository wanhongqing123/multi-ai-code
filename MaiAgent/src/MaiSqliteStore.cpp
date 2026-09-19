#include <sqlite3.h>

#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "MaiPathUtf8.h"
#include "MaiSessionStore.h"

// SQLite 落库。
//
// ── 为什么排序靠 id，而不是另开一个 seq 列 ──────────────────────
// MaiIdGenerator 产出的 id 是 前缀 + 10 位时间戳 + 3 位同毫秒序号 +
// 8 位随机，定长，而且用的 base32 字母表在 ASCII 里是递增的
// （0-9 然后 A..Z 去掉 ILOU）。定长 + 字母表递增 => 字典序等于生成顺序。
// 所以 `ORDER BY id` 就是插入顺序，和内存存储的行为一致。
//
// 这不是巧合，是 id 设计时就冲着这个去的（见 MaiIdGenerator.h）。
// 万一哪天换了 id 方案，这里的排序会跟着错——MaiStoreTests 里有一条
// 用例专门盯着"顺序和插入顺序一致"。
//
// ── 为什么整个连接一把锁 ────────────────────────────────────────
// 多个会话各跑各的线程，都会写这个库。SQLite 自己的 FULLMUTEX 只保证
// 单条语句安全，而 putMessage 是"删旧 part + 插新 part"好几条语句，
// mutateSession 更是读-改-写。这些必须整体原子，所以锁在我们这一层。
//
// 代价是写入串行化。可以接受：putMessage 的调用频率是"每个工具调用一次
// 加每轮一次"，不是每个 delta 一次（那是当初就定死的，见 MaiTurnRunner）。

namespace {

// 和 MaiRole / MaiToolState 的枚举值绑死。**不要改这些数字**——
// 库里已经存了的行不会跟着变。加新值只能往后追加。
int roleToColumn(MaiRole role) {
    return role == MaiRole::User ? 0 : 1;
}

MaiRole columnToRole(int value) {
    return value == 0 ? MaiRole::User : MaiRole::Assistant;
}

enum PartKind { kText = 0, kReasoning = 1, kTool = 2 };

int toolStateToColumn(MaiToolState state) {
    switch (state) {
        case MaiToolState::Pending: return 0;
        case MaiToolState::Running: return 1;
        case MaiToolState::Completed: return 2;
        case MaiToolState::Error: return 3;
    }
    return 0;
}

MaiToolState columnToToolState(int value) {
    switch (value) {
        case 1: return MaiToolState::Running;
        case 2: return MaiToolState::Completed;
        case 3: return MaiToolState::Error;
        default: return MaiToolState::Pending;
    }
}

std::string textColumn(sqlite3_stmt* statement, int index) {
    const unsigned char* text = sqlite3_column_text(statement, index);
    if (!text) return {};
    return std::string(reinterpret_cast<const char*>(text),
                       static_cast<std::size_t>(sqlite3_column_bytes(statement, index)));
}

// 绑定 std::string 时一律用 SQLITE_TRANSIENT：让 SQLite 自己拷一份。
// 用 STATIC 的话要保证字符串活到 step 之后，而这里好几处绑的是临时量，
// 那种错误不会立刻炸，会在某次 GC 时机变成读到垃圾。
void bindText(sqlite3_stmt* statement, int index, const std::string& value) {
    sqlite3_bind_text(statement, index, value.c_str(), static_cast<int>(value.size()),
                      SQLITE_TRANSIENT);
}

class SqliteStore final : public MaiSessionStore {
public:
    explicit SqliteStore(sqlite3* database) : mDatabase(database) {}

    ~SqliteStore() override {
        for (sqlite3_stmt* statement : mCached) sqlite3_finalize(statement);
        sqlite3_close(mDatabase);
    }

    void putSession(const MaiSession& session) override {
        std::lock_guard<std::mutex> lock(mMutex);
        writeSessionLocked(session);
    }

    bool getSession(const std::string& id, MaiSession& out) const override {
        std::lock_guard<std::mutex> lock(mMutex);
        sqlite3_stmt* statement = prepareLocked(
            "SELECT id, title, directory, model, agent, created, updated "
            "FROM sessions WHERE id = ?1");
        if (!statement) return false;
        Reset guard(statement);
        bindText(statement, 1, id);
        if (sqlite3_step(statement) != SQLITE_ROW) return false;
        out = readSession(statement);
        return true;
    }

    std::vector<MaiSession> listSessions() const override {
        std::lock_guard<std::mutex> lock(mMutex);
        std::vector<MaiSession> out;
        sqlite3_stmt* statement = prepareLocked(
            "SELECT id, title, directory, model, agent, created, updated "
            "FROM sessions ORDER BY updated DESC, id DESC");
        if (!statement) return out;
        Reset guard(statement);
        while (sqlite3_step(statement) == SQLITE_ROW) out.push_back(readSession(statement));
        return out;
    }

    bool removeSession(const std::string& id) override {
        std::lock_guard<std::mutex> lock(mMutex);
        if (!exec("BEGIN IMMEDIATE")) return false;

        // 显式逐级删，不靠 ON DELETE CASCADE：外键约束要连接上开了
        // PRAGMA foreign_keys 才生效，而那个 pragma 是每连接的，
        // 将来谁新开一个连接忘了设，删除就会只删一半、留下孤儿行。
        runWith(
            "DELETE FROM parts WHERE message_id IN "
            "(SELECT id FROM messages WHERE session_id = ?1)",
            id);
        runWith("DELETE FROM messages WHERE session_id = ?1", id);
        runWith("DELETE FROM sessions WHERE id = ?1", id);
        const int removed = sqlite3_changes(mDatabase);

        exec("COMMIT");
        return removed > 0;
    }

    void putMessage(const std::string& sessionId, const MaiMessage& message) override {
        std::lock_guard<std::mutex> lock(mMutex);
        if (!exec("BEGIN IMMEDIATE")) return;

        sqlite3_stmt* upsert = prepareLocked(
            "INSERT INTO messages(id, session_id, role, created, completed) "
            "VALUES(?1, ?2, ?3, ?4, ?5) "
            "ON CONFLICT(id) DO UPDATE SET role=excluded.role, created=excluded.created, "
            "completed=excluded.completed");
        if (upsert) {
            Reset guard(upsert);
            bindText(upsert, 1, message.id);
            bindText(upsert, 2, sessionId);
            sqlite3_bind_int(upsert, 3, roleToColumn(message.role));
            sqlite3_bind_int64(upsert, 4, message.created);
            sqlite3_bind_int64(upsert, 5, message.completed);
            step(upsert);
        }

        // part 整体重写而不是逐条 diff：一条消息的 part 个数是个位数，
        // 而 diff 要处理"改了/删了/顺序变了"三种情况，不值得。
        // putMessage 的调用频率是每个工具调用一次，不是每个 delta 一次。
        runWith("DELETE FROM parts WHERE message_id = ?1", message.id);
        for (const auto& part : message.parts) writePartLocked(message.id, part);

        exec("COMMIT");
    }

    std::vector<MaiMessage> listMessages(const std::string& sessionId) const override {
        std::lock_guard<std::mutex> lock(mMutex);
        std::vector<MaiMessage> out;

        sqlite3_stmt* statement = prepareLocked(
            "SELECT id, role, created, completed FROM messages "
            "WHERE session_id = ?1 ORDER BY id");
        if (!statement) return out;
        {
            Reset guard(statement);
            bindText(statement, 1, sessionId);
            while (sqlite3_step(statement) == SQLITE_ROW) {
                MaiMessage message;
                message.id = textColumn(statement, 0);
                message.role = columnToRole(sqlite3_column_int(statement, 1));
                message.created = sqlite3_column_int64(statement, 2);
                message.completed = sqlite3_column_int64(statement, 3);
                out.push_back(std::move(message));
            }
        }
        for (auto& message : out) message.parts = readPartsLocked(message.id);
        return out;
    }

    MaiError lastWriteError() const override {
        std::lock_guard<std::mutex> lock(mMutex);
        return mLastWriteError;
    }

    bool mutateSession(const std::string& id,
                       const std::function<void(MaiSession&)>& mutator) override {
        std::lock_guard<std::mutex> lock(mMutex);
        if (!exec("BEGIN IMMEDIATE")) return false;

        MaiSession session;
        bool found = false;
        sqlite3_stmt* statement = prepareLocked(
            "SELECT id, title, directory, model, agent, created, updated "
            "FROM sessions WHERE id = ?1");
        if (statement) {
            Reset guard(statement);
            bindText(statement, 1, id);
            if (sqlite3_step(statement) == SQLITE_ROW) {
                session = readSession(statement);
                found = true;
            }
        }
        if (!found) {
            exec("ROLLBACK");
            return false;
        }

        mutator(session);
        writeSessionLocked(session);
        exec("COMMIT");
        return true;
    }

private:
    // sqlite3_stmt 用完必须 reset，否则下次 bind 会接着上一次的状态，
    // 而且会一直占着读锁。用 RAII 保证提前 return 时也复位。
    struct Reset {
        sqlite3_stmt* statement;
        explicit Reset(sqlite3_stmt* s) : statement(s) {}
        ~Reset() {
            sqlite3_reset(statement);
            sqlite3_clear_bindings(statement);
        }
    };

    MaiSession readSession(sqlite3_stmt* statement) const {
        MaiSession session;
        session.id = textColumn(statement, 0);
        session.title = textColumn(statement, 1);
        session.directory = textColumn(statement, 2);
        session.model = textColumn(statement, 3);
        session.agent = textColumn(statement, 4);
        session.created = sqlite3_column_int64(statement, 5);
        session.updated = sqlite3_column_int64(statement, 6);
        return session;
    }

    void writeSessionLocked(const MaiSession& session) {
        sqlite3_stmt* statement = prepareLocked(
            "INSERT INTO sessions(id, title, directory, model, agent, created, updated) "
            "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7) "
            "ON CONFLICT(id) DO UPDATE SET title=excluded.title, directory=excluded.directory, "
            "model=excluded.model, agent=excluded.agent, updated=excluded.updated");
        if (!statement) return;
        Reset guard(statement);
        bindText(statement, 1, session.id);
        bindText(statement, 2, session.title);
        bindText(statement, 3, session.directory);
        bindText(statement, 4, session.model);
        bindText(statement, 5, session.agent);
        sqlite3_bind_int64(statement, 6, session.created);
        sqlite3_bind_int64(statement, 7, session.updated);
        step(statement);
    }

    void writePartLocked(const std::string& messageId, const MaiMessagePart& part) {
        sqlite3_stmt* statement = prepareLocked(
            "INSERT INTO parts(id, message_id, kind, created, text, tool, call_id, input, "
            "output, error, state) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)");
        if (!statement) return;
        Reset guard(statement);

        // part 不落 JSON，每一种展开成列。这样 sqlite3 命令行能直接查——
        // 排障时能看见"那次 write 的 input 到底是什么"，而不是一坨转义过的
        // 字符串。也省掉了把 nlohmann 拖进核心。
        int kind = kText;
        std::string text, tool, callId, input, output, error;
        int state = 0;
        if (const auto* textBody = std::get_if<MaiTextPart>(&part.body)) {
            kind = kText;
            text = textBody->text;
        } else if (const auto* reasoningBody = std::get_if<MaiReasoningPart>(&part.body)) {
            kind = kReasoning;
            text = reasoningBody->text;
        } else if (const auto* toolBody = std::get_if<MaiToolPart>(&part.body)) {
            kind = kTool;
            tool = toolBody->tool;
            callId = toolBody->callId;
            input = toolBody->input;
            output = toolBody->output;
            error = toolBody->error;
            state = toolStateToColumn(toolBody->state);
        }

        bindText(statement, 1, part.id);
        bindText(statement, 2, messageId);
        sqlite3_bind_int(statement, 3, kind);
        sqlite3_bind_int64(statement, 4, part.created);
        bindText(statement, 5, text);
        bindText(statement, 6, tool);
        bindText(statement, 7, callId);
        bindText(statement, 8, input);
        bindText(statement, 9, output);
        bindText(statement, 10, error);
        sqlite3_bind_int(statement, 11, state);
        step(statement);
    }

    std::vector<MaiMessagePart> readPartsLocked(const std::string& messageId) const {
        std::vector<MaiMessagePart> out;
        sqlite3_stmt* statement = prepareLocked(
            "SELECT id, kind, created, text, tool, call_id, input, output, error, state "
            "FROM parts WHERE message_id = ?1 ORDER BY id");
        if (!statement) return out;
        Reset guard(statement);
        bindText(statement, 1, messageId);
        while (sqlite3_step(statement) == SQLITE_ROW) {
            MaiMessagePart part;
            part.id = textColumn(statement, 0);
            const int kind = sqlite3_column_int(statement, 1);
            part.created = sqlite3_column_int64(statement, 2);
            if (kind == kReasoning) {
                part.body = MaiReasoningPart{textColumn(statement, 3)};
            } else if (kind == kTool) {
                MaiToolPart tool;
                tool.tool = textColumn(statement, 4);
                tool.callId = textColumn(statement, 5);
                tool.input = textColumn(statement, 6);
                tool.output = textColumn(statement, 7);
                tool.error = textColumn(statement, 8);
                tool.state = columnToToolState(sqlite3_column_int(statement, 9));
                part.body = std::move(tool);
            } else {
                part.body = MaiTextPart{textColumn(statement, 3)};
            }
            out.push_back(std::move(part));
        }
        return out;
    }

    // 语句缓存：同一条 SQL 只 prepare 一次。prepare 要解析 SQL 并生成
    // 字节码，放在每次写入的路径上是浪费。
    sqlite3_stmt* prepareLocked(const char* sql) const {
        auto it = mStatements.find(sql);
        if (it != mStatements.end()) return it->second;
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(mDatabase, sql, -1, &statement, nullptr) != SQLITE_OK) {
            noteError("prepare failed: " + std::string(sqlite3_errmsg(mDatabase)));
            return nullptr;
        }
        mStatements.emplace(sql, statement);
        mCached.push_back(statement);
        return statement;
    }

    void step(sqlite3_stmt* statement) {
        const int code = sqlite3_step(statement);
        if (code != SQLITE_DONE && code != SQLITE_ROW)
            noteError("write failed: " + std::string(sqlite3_errmsg(mDatabase)));
    }

    void runWith(const char* sql, const std::string& argument) {
        sqlite3_stmt* statement = prepareLocked(sql);
        if (!statement) return;
        Reset guard(statement);
        bindText(statement, 1, argument);
        step(statement);
    }

    bool exec(const char* sql) {
        char* message = nullptr;
        if (sqlite3_exec(mDatabase, sql, nullptr, nullptr, &message) == SQLITE_OK) return true;
        noteError(std::string("exec failed: ") + (message ? message : sql));
        sqlite3_free(message);
        return false;
    }

    // 只记住第一个错误。后面的多半是它的连锁反应，覆盖掉会让最有用的
    // 那条信息消失。
    void noteError(std::string what) const {
        if (!mLastWriteError.hasError())
            mLastWriteError = MaiError::make(MaiErrorCode::Internal, std::move(what));
    }

    sqlite3* mDatabase = nullptr;
    mutable std::mutex mMutex;
    mutable std::map<std::string, sqlite3_stmt*> mStatements;
    mutable std::vector<sqlite3_stmt*> mCached;
    mutable MaiError mLastWriteError;
};

// v1 建表。以后改结构就往下追加 if (version < 2) { ... }，
// 不要改这一段——已经落地的库是按 v1 建的。
const char* kSchemaV1 =
    "CREATE TABLE IF NOT EXISTS sessions("
    "  id TEXT PRIMARY KEY,"
    "  title TEXT NOT NULL DEFAULT '',"
    "  directory TEXT NOT NULL DEFAULT '',"
    "  model TEXT NOT NULL DEFAULT '',"
    "  agent TEXT NOT NULL DEFAULT 'build',"
    "  created INTEGER NOT NULL DEFAULT 0,"
    "  updated INTEGER NOT NULL DEFAULT 0);"
    "CREATE INDEX IF NOT EXISTS sessions_updated ON sessions(updated DESC);"
    "CREATE TABLE IF NOT EXISTS messages("
    "  id TEXT PRIMARY KEY,"
    "  session_id TEXT NOT NULL,"
    "  role INTEGER NOT NULL DEFAULT 0,"
    "  created INTEGER NOT NULL DEFAULT 0,"
    "  completed INTEGER NOT NULL DEFAULT 0);"
    "CREATE INDEX IF NOT EXISTS messages_session ON messages(session_id, id);"
    "CREATE TABLE IF NOT EXISTS parts("
    "  id TEXT PRIMARY KEY,"
    "  message_id TEXT NOT NULL,"
    "  kind INTEGER NOT NULL DEFAULT 0,"
    "  created INTEGER NOT NULL DEFAULT 0,"
    "  text TEXT NOT NULL DEFAULT '',"
    "  tool TEXT NOT NULL DEFAULT '',"
    "  call_id TEXT NOT NULL DEFAULT '',"
    "  input TEXT NOT NULL DEFAULT '',"
    "  output TEXT NOT NULL DEFAULT '',"
    "  error TEXT NOT NULL DEFAULT '',"
    "  state INTEGER NOT NULL DEFAULT 0);"
    "CREATE INDEX IF NOT EXISTS parts_message ON parts(message_id, id);";

int readUserVersion(sqlite3* database) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, "PRAGMA user_version", -1, &statement, nullptr) != SQLITE_OK)
        return -1;
    int version = -1;
    if (sqlite3_step(statement) == SQLITE_ROW) version = sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement);
    return version;
}

}  // namespace

MaiResult<std::unique_ptr<MaiSessionStore>> makeMaiSqliteStore(
    const std::string& databasePathUtf8) {
    if (databasePathUtf8.empty())
        return {MaiErrorCode::InvalidInput, "database path must not be empty"};

    // 父目录不存在就建出来。少了这一步，第一次跑的人会看到一句
    // "unable to open database file"，完全猜不到是目录的问题。
    // 走 MaiPathUtf8：MSVC 的 fs::path 会把 narrow 字符串按 ANSI 代码页
    // 解释，中文路径会出错（见 docs 里那条踩坑记录）。
    if (databasePathUtf8 != ":memory:") {
        const std::filesystem::path path = MaiPathUtf8::fromUtf8(databasePathUtf8);
        if (path.has_parent_path()) {
            std::error_code errorCode;
            std::filesystem::create_directories(path.parent_path(), errorCode);
        }
    }

    sqlite3* database = nullptr;
    // NOMUTEX：SQLite 自己不加锁，我们在 SqliteStore 里整体加。
    // 让它也加一层等于同一件事做两遍。
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX;
    if (sqlite3_open_v2(databasePathUtf8.c_str(), &database, flags, nullptr) != SQLITE_OK) {
        const std::string message = database ? sqlite3_errmsg(database) : "out of memory";
        sqlite3_close(database);
        return {MaiErrorCode::Internal, "cannot open database: " + message};
    }

    // WAL：读不挡写。界面在拉历史的同时，跑着的那一轮还在往里写。
    // synchronous=NORMAL 而不是 FULL：FULL 每次提交都 fsync，而 putMessage
    // 在工具循环里一轮要调好几次。NORMAL 在 WAL 下的代价是"机器掉电可能
    // 丢最后几次提交"，丢几条消息比每次都等磁盘划算。
    //
    // :memory: 上 WAL 不适用，SQLite 会忽略，不用特判。
    //
    // **WAL 的操作陷阱**：开了 WAL 之后，已提交的数据先落在 <db>-wal 里，
    // 要到 checkpoint 才并回主文件。实测跑完一轮对话，agent.db 还是 4096
    // 字节（一页），78KB 数据全在 agent.db-wal 里。
    //
    // 所以：**只拷 agent.db 不算备份**，会拷到一个几乎空的库。要么三个
    // 文件（db / -wal / -shm）一起拷，要么先让进程干净退出——sqlite3_close
    // 在最后一个连接关闭时会做 checkpoint 并删掉 -wal。
    //
    // 进程被硬杀不会丢数据：下次打开时 SQLite 会从 -wal 恢复。
    // 这条是实测过的（tests/e2e/m5_persistence.py 就是 terminate() 杀的）。
    sqlite3_exec(database, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
    sqlite3_exec(database, "PRAGMA synchronous=NORMAL", nullptr, nullptr, nullptr);
    // 被别的写者占着时等 5 秒再报 busy，而不是立刻失败。
    sqlite3_busy_timeout(database, 5000);

    char* message = nullptr;
    if (sqlite3_exec(database, kSchemaV1, nullptr, nullptr, &message) != SQLITE_OK) {
        const std::string what = message ? message : "unknown error";
        sqlite3_free(message);
        sqlite3_close(database);
        // 最常见的原因是这个文件根本不是数据库（指错了路径）。
        return {MaiErrorCode::Internal, "cannot create schema: " + what};
    }
    if (readUserVersion(database) < 1)
        sqlite3_exec(database, "PRAGMA user_version=1", nullptr, nullptr, nullptr);

    return std::unique_ptr<MaiSessionStore>(new SqliteStore(database));
}
