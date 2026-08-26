#include "databasemanager.h"
#include <QDebug>
#include <QSqlQuery>
#include <QSqlError>
#include <QCryptographicHash>
#include <QVariant>
#include <QMap>
#include <cstring>
#include <crypt.h>
#include <random>

// =============================================================================
//  RAII guard: auto acquire/release connection, auto rollback uncommitted txn
// =============================================================================
class DbConn {
    DatabaseManager *m_mgr;
    QSqlDatabase m_db;
public:
    DbConn(DatabaseManager *mgr) : m_mgr(mgr) { m_db = m_mgr->getConnection(); }
    ~DbConn() { if (m_db.isValid()) m_mgr->releaseConnection(m_db); }
    QSqlDatabase& db() { return m_db; }
    bool ok() const { return m_db.isOpen(); }
};

class Transaction {
    QSqlDatabase &m_db;
    bool m_active;
public:
    Transaction(QSqlDatabase &db) : m_db(db), m_active(db.transaction()) {}
    ~Transaction() { if (m_active) m_db.rollback(); }
    bool commit() { if (!m_active) return false; m_active = false; return m_db.commit(); }
    bool started() const { return m_active; }
};

// =============================================================================
//  Helpers
// =============================================================================
/*
 * bcrypt password hashing
 *
 * Security comparison:
 *   Old: SHA-256 unsalted -> rainbow tables crack instantly, GPU brute-force fast
 *   New: bcrypt ($2b$12$) -> built-in random salt, tunable cost factor (12),
 *        strong GPU resistance
 *
 * bcrypt format: $2b$12$xxxxxxxxxxxxxxxxxxxxxxx...
 *                 ^    ^   ^
 *                 |    |   22-char salt + 31-char hash = 53 chars
 *                 |    cost factor (10-14); higher = slower = safer
 *                 version (2b is the standard)
 *
 * Total length: 3(version) + 1($) + 2(cost) + 1($) + 53 = 60 chars
 */

static QString bcryptHash(const QString &password) {
    const int cost = 12;

    /* Generate a 22-char bcrypt base64 salt directly (simpler, no OOB risk) */
    const char b64[] = "./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    char salt_b64[23];
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 63);
    for (int i = 0; i < 22; i++)
        salt_b64[i] = b64[dis(gen)];
    salt_b64[22] = '\0';

    char salt_param[30];
    snprintf(salt_param, sizeof(salt_param), "$2b$%02d$%s", cost, salt_b64);

    char *hash = crypt(password.toUtf8().constData(), salt_param);
    if (!hash) return QString();

    return QString(hash);
}

static bool bcryptVerify(const QString &password, const QString &storedHash) {
    if (storedHash.isEmpty() || !storedHash.startsWith("$2b$"))
        return false;

    char *hash = crypt(password.toUtf8().constData(), storedHash.toUtf8().constData());
    if (!hash) return false;

    return strcmp(hash, storedHash.toUtf8().constData()) == 0;
}

static QString hashPassword(const QString &password) {
    return bcryptHash(password);
}

static void safeCopy(char *dst, const QString &src, int maxLen) {
    strncpy(dst, src.toUtf8().constData(), maxLen - 1);
    dst[maxLen - 1] = '\0';
}

static time_t parseTimeString(const QString &s) {
    struct tm tm = {};
    sscanf(s.toUtf8().constData(), "%d-%d-%d %d:%d:%d",
           &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
           &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
    tm.tm_year -= 1900; tm.tm_mon -= 1;
    return mktime(&tm);
}

// =============================================================================
//  Singleton / ctor / dtor
// =============================================================================
DatabaseManager::DatabaseManager(QObject *parent)
    : QObject(parent), m_poolSize(16), m_initialized(false), m_port(3306) {}

DatabaseManager::~DatabaseManager() { closeDatabase(); }

DatabaseManager *DatabaseManager::getInstance() {
    static QMutex s_mutex;
    static DatabaseManager* s_instance = nullptr;
    if (!s_instance) {
        QMutexLocker locker(&s_mutex);
        if (!s_instance) s_instance = new DatabaseManager();
    }
    return s_instance;
}

// =============================================================================
//  Connection pool
// =============================================================================
QSqlDatabase DatabaseManager::getConnection() {
    QMutexLocker lock(&m_poolMutex);
    while (m_connNames.isEmpty()) {
        if (!m_poolCond.wait(&m_poolMutex, 5000)) {
            qDebug() << "get db connection timed out";
            return QSqlDatabase();
        }
    }
    QString name = m_connNames.dequeue();
    QSqlDatabase db = QSqlDatabase::database(name);
    if (!db.isOpen()) {
        db = QSqlDatabase::addDatabase("QMYSQL", name);
        db.setHostName(m_host); db.setPort(m_port);
        db.setDatabaseName(m_dbName); db.setUserName(m_user); db.setPassword(m_password);
        /* MYSQL_OPT_RECONNECT is deprecated in MySQL 8.0+; rely on auto-reconnect */
        if (!db.open()) {
            qDebug() << "reconnect MySQL failed:" << db.lastError().text();
            m_connNames.enqueue(name);
            return QSqlDatabase();
        }
    }
    return db;
}

void DatabaseManager::releaseConnection(QSqlDatabase &conn) {
    if (!conn.isValid()) return;
    QMutexLocker lock(&m_poolMutex);
    m_connNames.enqueue(conn.connectionName());
    m_poolCond.wakeOne();
    conn = QSqlDatabase();
}

// =============================================================================
//  Init / table creation / close
// =============================================================================
bool DatabaseManager::initDatabase(const QString &host, int port, const QString &dbName,
                                   const QString &user, const QString &password, int poolSize) {
    if (m_initialized) return true;
    QMutexLocker lock(&m_poolMutex);
    if (m_initialized) return true;

    m_host = host; m_port = port; m_dbName = dbName;
    m_user = user; m_password = password; m_poolSize = poolSize;

    for (int i = 0; i < m_poolSize; i++) {
        QString name = QString("db_conn_%1").arg(i);
        QSqlDatabase db = QSqlDatabase::addDatabase("QMYSQL", name);
        db.setHostName(host); db.setPort(port);
        db.setDatabaseName(dbName); db.setUserName(user); db.setPassword(password);
        /* MYSQL_OPT_RECONNECT is deprecated in MySQL 8.0+; rely on auto-reconnect */
        if (!db.open()) {
            qDebug() << "MySQL connect failed:" << db.lastError().text();
            QSqlDatabase::removeDatabase(name);
            continue;
        }
        m_connNames.enqueue(name);
    }
    if (m_connNames.isEmpty()) { qDebug() << "no MySQL connection could be established"; return false; }

    lock.unlock();
    if (!createTables()) {
        qDebug() << "create tables failed";
        QMutexLocker relock(&m_poolMutex);
        while (!m_connNames.isEmpty())
            QSqlDatabase::removeDatabase(m_connNames.dequeue());
        return false;
    }

    QMutexLocker relock(&m_poolMutex);
    m_initialized = true;
    qDebug() << QString("MySQL init done (pool: %1/%2)").arg(m_connNames.size()).arg(m_poolSize);
    return true;
}

bool DatabaseManager::createTables() {
    DbConn c(this);
    if (!c.ok()) return false;

    QStringList tables = {
        "CREATE TABLE IF NOT EXISTS member ("
        "member_uid VARCHAR(32) PRIMARY KEY NOT NULL,"
        "name VARCHAR(64) NOT NULL,phone VARCHAR(20) UNIQUE NOT NULL,"
        "balance DECIMAL(12,2) DEFAULT 0.00,password VARCHAR(60),"
        "face_path VARCHAR(256),face_feature TEXT,"
        "member_type TINYINT DEFAULT 0,register_time TIMESTAMP DEFAULT CURRENT_TIMESTAMP"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",

        "CREATE TABLE IF NOT EXISTS goods ("
        "goods_id INT AUTO_INCREMENT PRIMARY KEY,"
        "client_id VARCHAR(64) NOT NULL DEFAULT 'default',"
        "goods_name VARCHAR(128) NOT NULL,price DECIMAL(12,2) NOT NULL,"
        "unit VARCHAR(16) NOT NULL DEFAULT '件',"
        "create_time TIMESTAMP DEFAULT CURRENT_TIMESTAMP,version INT DEFAULT 1,"
        "UNIQUE KEY uk_client_goods (client_id,goods_name)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",

        "CREATE TABLE IF NOT EXISTS stock ("
        "stock_id INT AUTO_INCREMENT PRIMARY KEY,"
        "client_id VARCHAR(64) NOT NULL DEFAULT 'default',"
        "goods_name VARCHAR(128) NOT NULL,stock_num INT NOT NULL DEFAULT 0,"
        "version INT DEFAULT 1,"
        "update_time TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
        "UNIQUE KEY uk_client_goods (client_id,goods_name)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",

        "CREATE TABLE IF NOT EXISTS orders ("
        "id INT AUTO_INCREMENT PRIMARY KEY,order_id VARCHAR(64) NOT NULL,"
        "member_uid VARCHAR(32) NOT NULL,goods_list TEXT,"
        "total_amount DECIMAL(12,2) NOT NULL,pay_status TINYINT DEFAULT 0,"
        "create_time TIMESTAMP DEFAULT CURRENT_TIMESTAMP,pay_time TIMESTAMP NULL,"
        "KEY idx_order_member (member_uid),KEY idx_order_id (order_id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",

        "CREATE TABLE IF NOT EXISTS ota_versions ("
        "id INT AUTO_INCREMENT PRIMARY KEY,version VARCHAR(32) NOT NULL UNIQUE,"
        "filename VARCHAR(128) NOT NULL,sha256 VARCHAR(64) NOT NULL,"
        "file_size INT NOT NULL,description VARCHAR(256) DEFAULT '',"
        "force_update TINYINT DEFAULT 0,upload_time TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
        "type TINYINT DEFAULT 0 COMMENT '0=APP 1=SYSTEM'"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",

        "CREATE TABLE IF NOT EXISTS balance_log ("
        "id INT AUTO_INCREMENT PRIMARY KEY,member_uid VARCHAR(32) NOT NULL,"
        "type TINYINT NOT NULL,amount DECIMAL(12,2) NOT NULL,"
        "old_balance DECIMAL(12,2) NOT NULL,new_balance DECIMAL(12,2) NOT NULL,"
        "operator_name VARCHAR(64) DEFAULT '',remark VARCHAR(255) DEFAULT '',"
        "create_time TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
        "INDEX idx_blog_uid (member_uid),INDEX idx_blog_time (create_time)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4"
    };

    bool ok = true;
    for (const auto &sql : tables) {
        QSqlQuery q(c.db());
        if (!q.exec(sql)) { qDebug() << "create table failed:" << q.lastError().text(); ok = false; }
    }

    /* Legacy DB upgrade: add the type column to ota_versions if missing (skip if present) */
    {
        QSqlQuery check(c.db());
        check.exec("SELECT COLUMN_NAME FROM INFORMATION_SCHEMA.COLUMNS "
                   "WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='ota_versions' AND COLUMN_NAME='type'");
        if (!check.next()) {
            QSqlQuery alter(c.db());
            if (!alter.exec("ALTER TABLE ota_versions ADD COLUMN type TINYINT DEFAULT 0 COMMENT '0=APP 1=SYSTEM'")) {
                qDebug() << "ota_versions add-column failed:" << alter.lastError().text();
            }
        }
    }

    return ok;
}

void DatabaseManager::closeDatabase() {
    QMutexLocker lock(&m_poolMutex);
    while (!m_connNames.isEmpty())
        QSqlDatabase::removeDatabase(m_connNames.dequeue());
    m_initialized = false;
}

// =============================================================================
//  Atomic transactions
// =============================================================================
int DatabaseManager::orderCreateAtomic(const QString &orderId, const QString &memberUid,
                                       const QString &goodsList, double totalAmount,
                                       QString &outMemberUid, double &outNewBalance) {
    DbConn c(this);
    if (!c.ok()) return -1;
    Transaction t(c.db());
    if (!t.started()) return -1;

    QSqlQuery q(c.db());
    q.prepare("SELECT balance FROM member WHERE member_uid=? FOR UPDATE");
    q.addBindValue(memberUid);
    if (!q.exec() || !q.next()) return q.exec() ? -2 : -1;  // member not found=-2, SQL error=-1

    double cur = q.value(0).toDouble();
    if (cur < totalAmount) return -3;
    outNewBalance = cur - totalAmount;

    q.prepare("UPDATE member SET balance=? WHERE member_uid=?");
    q.addBindValue(outNewBalance); q.addBindValue(memberUid);
    if (!q.exec()) return -1;

    q.prepare("INSERT INTO orders(order_id,member_uid,goods_list,total_amount,pay_status,pay_time)"
              "VALUES(?,?,?,?,1,NOW())");
    q.addBindValue(orderId); q.addBindValue(memberUid);
    q.addBindValue(goodsList); q.addBindValue(totalAmount);
    if (!q.exec()) return -1;

    q.prepare("INSERT INTO balance_log(member_uid,type,amount,old_balance,new_balance,remark)"
              "VALUES(?,?,?,?,?,?)");
    q.addBindValue(memberUid); q.addBindValue(2);
    q.addBindValue(totalAmount); q.addBindValue(cur);
    q.addBindValue(outNewBalance); q.addBindValue(orderId);
    q.exec();  // log failure must not abort the main flow

    if (!t.commit()) return -1;
    outMemberUid = memberUid;
    return 0;
}

int DatabaseManager::balanceUpdateAtomic(const QString &memberUid, double amount, int type,
                                         double &outNewBalance) {
    DbConn c(this);
    if (!c.ok()) return -1;
    Transaction t(c.db());
    if (!t.started()) return -1;

    QSqlQuery q(c.db());
    q.prepare("SELECT balance FROM member WHERE member_uid=? FOR UPDATE");
    q.addBindValue(memberUid);
    if (!q.exec() || !q.next()) return q.exec() ? -2 : -1;

    double cur = q.value(0).toDouble();
    outNewBalance = cur;
    if (type == 1) outNewBalance += amount;
    else if (type == 2) {
        if (cur < amount) return -3;
        outNewBalance -= amount;
    }

    q.prepare("UPDATE member SET balance=? WHERE member_uid=?");
    q.addBindValue(outNewBalance); q.addBindValue(memberUid);
    if (!q.exec()) return -1;

    q.prepare("INSERT INTO balance_log(member_uid,type,amount,old_balance,new_balance)"
              "VALUES(?,?,?,?,?)");
    q.addBindValue(memberUid); q.addBindValue(type);
    q.addBindValue(amount); q.addBindValue(cur); q.addBindValue(outNewBalance);
    if (!q.exec()) return -1;

    if (!t.commit()) return -1;
    return 0;
}

// =============================================================================
//  Member management
// =============================================================================
int DatabaseManager::memberRegister(const QString &uid, const QString &name, const QString &phone,
                                    double initBalance, const QString &password,
                                    const QString &facePath, const QString &faceFeature, int memberType) {
    DbConn c(this);
    if (!c.ok()) return -1;
    QSqlQuery q(c.db());
    q.prepare("INSERT INTO member(member_uid,name,phone,balance,password,face_path,face_feature,member_type)"
              "VALUES(?,?,?,?,?,?,?,?)");
    q.addBindValue(uid); q.addBindValue(name); q.addBindValue(phone); q.addBindValue(initBalance);
    q.addBindValue(password.isEmpty() ? QVariant() : hashPassword(password));
    q.addBindValue(facePath.isEmpty() ? QVariant() : facePath);
    q.addBindValue(faceFeature.isEmpty() ? QVariant() : faceFeature);
    q.addBindValue(memberType);
    return q.exec() ? 0 : -1;
}

int DatabaseManager::memberQueryByUid(const QString &uid, member_info_t *m) {
    memset(m, 0, sizeof(member_info_t));
    DbConn c(this);
    if (!c.ok()) return -1;
    QSqlQuery q(c.db());
    q.prepare("SELECT member_uid,name,phone,balance,password,face_path,face_feature,"
              "member_type,UNIX_TIMESTAMP(register_time) FROM member WHERE member_uid=?");
    q.addBindValue(uid);
    if (!q.exec() || !q.next()) return -1;
    safeCopy(m->uid, q.value(0).toString(), sizeof(m->uid));
    safeCopy(m->name, q.value(1).toString(), sizeof(m->name));
    safeCopy(m->phone, q.value(2).toString(), sizeof(m->phone));
    m->balance = q.value(3).toDouble();
    safeCopy(m->password, q.value(4).toString(), sizeof(m->password));
    safeCopy(m->face_path, q.value(5).toString(), sizeof(m->face_path));
    safeCopy(m->face_feature, q.value(6).toString(), sizeof(m->face_feature));
    m->member_type = q.value(7).toInt();
    m->create_time = (time_t)q.value(8).toLongLong();
    return 0;
}

int DatabaseManager::memberQueryAll(member_info_t *list, int *count) {
    memset(list, 0, sizeof(member_info_t) * 100);
    *count = 0;
    DbConn c(this);
    if (!c.ok()) return -1;
    QSqlQuery q(c.db());
    if (!q.exec("SELECT member_uid,name,phone,balance,password,face_path,face_feature,"
                "member_type,UNIX_TIMESTAMP(register_time) FROM member")) return -1;
    while (q.next() && *count < 100) {
        member_info_t *m = &list[*count];
        safeCopy(m->uid, q.value(0).toString(), sizeof(m->uid));
        safeCopy(m->name, q.value(1).toString(), sizeof(m->name));
        safeCopy(m->phone, q.value(2).toString(), sizeof(m->phone));
        m->balance = q.value(3).toDouble();
        safeCopy(m->password, q.value(4).toString(), sizeof(m->password));
        safeCopy(m->face_path, q.value(5).toString(), sizeof(m->face_path));
        safeCopy(m->face_feature, q.value(6).toString(), sizeof(m->face_feature));
        m->member_type = q.value(7).toInt();
        m->create_time = (time_t)q.value(8).toLongLong();
        (*count)++;
    }
    return 0;
}

int DatabaseManager::memberUpdateBalance(const QString &uid, double newBalance) {
    DbConn c(this);
    if (!c.ok()) return -1;

    QSqlQuery q(c.db());
    q.prepare("SELECT balance FROM member WHERE member_uid=?");
    q.addBindValue(uid);
    if (!q.exec() || !q.next()) return -1;
    double oldBalance = q.value(0).toDouble();

    Transaction t(c.db());
    if (!t.started()) return -1;

    q.prepare("UPDATE member SET balance=? WHERE member_uid=?");
    q.addBindValue(newBalance); q.addBindValue(uid);
    if (!q.exec()) return -1;

    q.prepare("INSERT INTO balance_log(member_uid,type,amount,old_balance,new_balance,remark)"
              "VALUES(?,?,?,?,?,?)");
    q.addBindValue(uid); q.addBindValue(3);
    q.addBindValue(newBalance - oldBalance);
    q.addBindValue(oldBalance); q.addBindValue(newBalance);
    q.addBindValue("管理员调整余额");
    if (!q.exec()) return -1;

    return t.commit() ? 0 : -1;
}

int DatabaseManager::memberDelete(const QString &uid) {
    DbConn c(this);
    if (!c.ok()) return -1;
    QSqlQuery q(c.db());
    q.prepare("DELETE FROM member WHERE member_uid=?");
    q.addBindValue(uid);
    return q.exec() ? 0 : -1;
}

int DatabaseManager::memberVerifyPassword(const QString &uid, const QString &password) {
    DbConn c(this);
    if (!c.ok()) return -1;
    QSqlQuery q(c.db());
    q.prepare("SELECT password FROM member WHERE member_uid=?");
    q.addBindValue(uid);
    if (!q.exec() || !q.next()) return -1;

    QString storedHash = q.value(0).toString();

    /* Backward compat: detect legacy unsalted SHA-256 hashes */
    if (storedHash.size() == 64 && !storedHash.startsWith("$")) {
        QString sha256 = QString(QCryptographicHash::hash(password.toUtf8(), QCryptographicHash::Sha256).toHex());
        if (sha256 == storedHash) {
            /* Legacy password verified: auto-upgrade to bcrypt */
            QString newHash = bcryptHash(password);
            QSqlQuery u(c.db());
            u.prepare("UPDATE member SET password=? WHERE member_uid=?");
            u.addBindValue(newHash);
            u.addBindValue(uid);
            u.exec();
            return 0;
        }
        return -1;
    }

    return bcryptVerify(password, storedHash) ? 0 : -1;
}

int DatabaseManager::memberQueryType(const QString &uid, int *type) {
    *type = 0;
    DbConn c(this);
    if (!c.ok()) return -1;
    QSqlQuery q(c.db());
    q.prepare("SELECT member_type FROM member WHERE member_uid=?");
    q.addBindValue(uid);
    if (!q.exec() || !q.next()) return -1;
    *type = q.value(0).toInt();
    return 0;
}

// =============================================================================
//  Goods & stock
// =============================================================================
int DatabaseManager::goodsAdd(const QString &clientId, const QString &goodsName, double price,
                              const QString &unit, int initStock) {
    DbConn c(this);
    if (!c.ok()) return -1;
    Transaction t(c.db());
    if (!t.started()) return -1;

    QSqlQuery q(c.db());
    q.prepare("INSERT INTO goods(client_id,goods_name,price,unit) VALUES(?,?,?,?)");
    q.addBindValue(clientId); q.addBindValue(goodsName);
    q.addBindValue(price); q.addBindValue(unit);
    if (!q.exec()) return -1;

    q.prepare("INSERT INTO stock(client_id,goods_name,stock_num) VALUES(?,?,?)");
    q.addBindValue(clientId); q.addBindValue(goodsName); q.addBindValue(initStock);
    if (!q.exec()) return -1;

    return t.commit() ? 0 : -1;
}

int DatabaseManager::goodsQueryById(int goodsId, goods_info_t *g) {
    memset(g, 0, sizeof(goods_info_t));
    DbConn c(this);
    if (!c.ok()) return -1;
    QSqlQuery q(c.db());
    q.prepare("SELECT g.goods_id,g.goods_name,g.price,g.unit,"
              "IFNULL(s.stock_num,0),UNIX_TIMESTAMP(g.create_time),g.version "
              "FROM goods g LEFT JOIN stock s ON g.client_id=s.client_id AND g.goods_name=s.goods_name "
              "WHERE g.goods_id=?");
    q.addBindValue(goodsId);
    if (!q.exec() || !q.next()) return -1;
    g->id = q.value(0).toInt();
    safeCopy(g->name, q.value(1).toString(), sizeof(g->name));
    g->price = q.value(2).toDouble();
    safeCopy(g->unit, q.value(3).toString(), sizeof(g->unit));
    g->stock = q.value(4).toInt();
    g->create_time = (time_t)q.value(5).toLongLong();
    g->version = q.value(6).toInt();
    return 0;
}

static void fillGoods(QSqlQuery &q, goods_info_t *g) {
    g->id = q.value(0).toInt();
    safeCopy(g->name, q.value(1).toString(), sizeof(g->name));
    g->price = q.value(2).toDouble();
    safeCopy(g->unit, q.value(3).toString(), sizeof(g->unit));
    g->stock = q.value(4).toInt();
    g->create_time = (time_t)q.value(5).toLongLong();
    g->version = q.value(6).toInt();
}

int DatabaseManager::goodsQueryAll(goods_info_t *list, int *count) {
    memset(list, 0, sizeof(goods_info_t) * 100);
    *count = 0;
    DbConn c(this);
    if (!c.ok()) return -1;
    QSqlQuery q(c.db());
    if (!q.exec("SELECT g.goods_id,g.goods_name,g.price,g.unit,"
                "IFNULL(s.stock_num,0),UNIX_TIMESTAMP(g.create_time),g.version "
                "FROM goods g LEFT JOIN stock s ON g.client_id=s.client_id AND g.goods_name=s.goods_name"))
        return -1;
    while (q.next() && *count < 100) fillGoods(q, &list[(*count)++]);
    return 0;
}

int DatabaseManager::goodsQueryByClientId(const QString &clientId, goods_info_t *list, int *count) {
    memset(list, 0, sizeof(goods_info_t) * 100);
    *count = 0;
    DbConn c(this);
    if (!c.ok()) return -1;
    QSqlQuery q(c.db());
    q.prepare("SELECT g.goods_id,g.goods_name,g.price,g.unit,"
              "IFNULL(s.stock_num,0),UNIX_TIMESTAMP(g.create_time),g.version "
              "FROM goods g LEFT JOIN stock s ON g.client_id=s.client_id AND g.goods_name=s.goods_name "
              "WHERE g.client_id=?");
    q.addBindValue(clientId);
    if (!q.exec()) return -1;
    while (q.next() && *count < 100) fillGoods(q, &list[(*count)++]);
    return 0;
}

int DatabaseManager::goodsDelete(const QString &clientId, const QString &goodsName) {
    DbConn c(this);
    if (!c.ok()) return -1;
    Transaction t(c.db());
    if (!t.started()) return -1;
    QSqlQuery q(c.db());
    q.prepare("DELETE FROM stock WHERE client_id=? AND goods_name=?");
    q.addBindValue(clientId); q.addBindValue(goodsName);
    if (!q.exec()) return -1;
    q.prepare("DELETE FROM goods WHERE client_id=? AND goods_name=?");
    q.addBindValue(clientId); q.addBindValue(goodsName);
    if (!q.exec()) return -1;
    return t.commit() ? 0 : -1;
}

int DatabaseManager::stockDeduct(const QString &clientId, const QString &goodsName, int deductNum) {
    DbConn c(this);
    if (!c.ok()) return -1;
    Transaction t(c.db());
    if (!t.started()) return -1;
    QSqlQuery q(c.db());
    q.prepare("SELECT stock_num FROM stock WHERE client_id=? AND goods_name=? FOR UPDATE");
    q.addBindValue(clientId); q.addBindValue(goodsName);
    if (!q.exec() || !q.next()) return -1;
    if (q.value(0).toInt() < deductNum) return -1;
    q.prepare("UPDATE stock SET stock_num=stock_num-?,version=version+1 WHERE client_id=? AND goods_name=?");
    q.addBindValue(deductNum); q.addBindValue(clientId); q.addBindValue(goodsName);
    if (!q.exec()) return -1;
    return t.commit() ? 0 : -1;
}

// =============================================================================
//  Order management
// =============================================================================
static void fillOrder(QSqlQuery &q, order_info_t *o) {
    o->id = q.value(0).toInt();
    safeCopy(o->order_id, q.value(1).toString(), sizeof(o->order_id));
    safeCopy(o->member_uid, q.value(2).toString(), sizeof(o->member_uid));
    o->total = q.value(3).toDouble();
    o->pay_status = q.value(4).toInt();
    o->create_time = parseTimeString(q.value(5).toString());
    safeCopy(o->goods_list, q.value(6).toString(), sizeof(o->goods_list));
}

int DatabaseManager::orderQueryByCondition(const QString &condition, order_info_t *list, int *count) {
    memset(list, 0, sizeof(order_info_t) * 100);
    *count = 0;
    DbConn c(this);
    if (!c.ok()) return -1;
    QSqlQuery q(c.db());
    q.prepare("SELECT id,order_id,member_uid,total_amount,pay_status,"
              "DATE_FORMAT(create_time,'%Y-%m-%d %H:%i:%s'),goods_list FROM orders "
              "WHERE member_uid=? OR EXISTS"
              "(SELECT 1 FROM member m WHERE m.member_uid=orders.member_uid AND m.phone=?) "
              "ORDER BY create_time DESC");
    q.addBindValue(condition); q.addBindValue(condition);
    if (!q.exec()) return -1;
    while (q.next() && *count < 100) fillOrder(q, &list[(*count)++]);
    return 0;
}

int DatabaseManager::orderQueryAll(order_info_t *list, int *count) {
    memset(list, 0, sizeof(order_info_t) * 100);
    *count = 0;
    DbConn c(this);
    if (!c.ok()) return -1;
    QSqlQuery q(c.db());
    if (!q.exec("SELECT id,order_id,member_uid,total_amount,pay_status,"
                "DATE_FORMAT(create_time,'%Y-%m-%d %H:%i:%s'),goods_list FROM orders "
                "ORDER BY create_time DESC")) return -1;
    while (q.next() && *count < 100) fillOrder(q, &list[(*count)++]);
    return 0;
}

int DatabaseManager::orderQueryById(int id, order_info_t *o) {
    memset(o, 0, sizeof(order_info_t));
    DbConn c(this);
    if (!c.ok()) return -1;
    QSqlQuery q(c.db());
    q.prepare("SELECT id,order_id,member_uid,total_amount,pay_status,"
              "DATE_FORMAT(create_time,'%Y-%m-%d %H:%i:%s'),goods_list FROM orders WHERE id=?");
    q.addBindValue(id);
    if (!q.exec() || !q.next()) return -1;
    fillOrder(q, o);
    return 0;
}

// =============================================================================
//  Composite business
// =============================================================================
int DatabaseManager::goodsUpdateWithStock(const QString &clientId, const QString &goodsName,
                                          double price, const QString &unit, int stock) {
    DbConn c(this);
    if (!c.ok()) return -1;
    Transaction t(c.db());
    if (!t.started()) return -1;
    QSqlQuery q(c.db());
    q.prepare("UPDATE goods SET price=?,unit=? WHERE client_id=? AND goods_name=?");
    q.addBindValue(price); q.addBindValue(unit);
    q.addBindValue(clientId); q.addBindValue(goodsName);
    if (!q.exec()) return -1;
    q.prepare("UPDATE stock SET stock_num=?,version=version+1 WHERE client_id=? AND goods_name=?");
    q.addBindValue(stock); q.addBindValue(clientId); q.addBindValue(goodsName);
    if (!q.exec()) return -1;
    return t.commit() ? 0 : -1;
}

int DatabaseManager::syncClientGoods(const QString &clientId, const QList<goods_info_t> &goodsList) {
    DbConn c(this);
    if (!c.ok()) return -1;
    Transaction t(c.db());
    if (!t.started()) return -1;
    QSqlQuery q(c.db());
    q.prepare("DELETE FROM stock WHERE client_id=?");
    q.addBindValue(clientId);
    if (!q.exec()) return -1;
    q.prepare("DELETE FROM goods WHERE client_id=?");
    q.addBindValue(clientId);
    if (!q.exec()) return -1;

    QSqlQuery qi(c.db()), qs(c.db());
    qi.prepare("INSERT INTO goods(client_id,goods_name,price,unit) VALUES(?,?,?,?)");
    qs.prepare("INSERT INTO stock(client_id,goods_name,stock_num) VALUES(?,?,?)");
    for (const auto &g : goodsList) {
        qi.addBindValue(clientId); qi.addBindValue(QString::fromUtf8(g.name));
        qi.addBindValue(g.price); qi.addBindValue(QString::fromUtf8(g.unit));
        if (!qi.exec()) return -1;
        qs.addBindValue(clientId); qs.addBindValue(QString::fromUtf8(g.name));
        qs.addBindValue(g.stock);
        if (!qs.exec()) return -1;
    }
    return t.commit() ? 0 : -1;
}

int DatabaseManager::getGoodsId(const QString &clientId, const QString &goodsName) {
    DbConn c(this);
    if (!c.ok()) return -1;
    QSqlQuery q(c.db());
    q.prepare("SELECT goods_id FROM goods WHERE client_id=? AND goods_name=?");
    q.addBindValue(clientId); q.addBindValue(goodsName);
    if (!q.exec() || !q.next()) return -1;
    return q.value(0).toInt();
}

// =============================================================================
//  OTA
// =============================================================================
int DatabaseManager::otaAddVersion(const QString &version, const QString &filename,
                                   const QString &sha256, int fileSize,
                                   const QString &description, int forceUpdate, int type) {
    DbConn c(this);
    if (!c.ok()) return -1;
    QSqlQuery q(c.db());
    q.prepare("INSERT INTO ota_versions(version,filename,sha256,file_size,description,force_update,type)"
              "VALUES(?,?,?,?,?,?,?)");
    q.addBindValue(version); q.addBindValue(filename); q.addBindValue(sha256);
    q.addBindValue(fileSize); q.addBindValue(description); q.addBindValue(forceUpdate);
    q.addBindValue(type);
    return q.exec() ? 0 : -1;
}

static void fillOta(QSqlQuery &q, ota_version_t *v) {
    v->id = q.value(0).toInt();
    safeCopy(v->version, q.value(1).toString(), sizeof(v->version));
    safeCopy(v->filename, q.value(2).toString(), sizeof(v->filename));
    safeCopy(v->sha256, q.value(3).toString(), sizeof(v->sha256));
    v->file_size = q.value(4).toInt();
    safeCopy(v->description, q.value(5).toString(), sizeof(v->description));
    safeCopy(v->upload_time, q.value(6).toString(), sizeof(v->upload_time));
    v->force_update = q.value(7).toInt();
    v->type = (ota_type_t)q.value(8).toInt();
}

int DatabaseManager::otaGetLatestVersion(ota_version_t *ver) {
    memset(ver, 0, sizeof(ota_version_t));
    DbConn c(this);
    if (!c.ok()) return -1;
    QSqlQuery q(c.db());
    if (!q.exec("SELECT id,version,filename,sha256,file_size,description,"
                "DATE_FORMAT(upload_time,'%Y-%m-%d %H:%i:%s'),force_update,type "
                "FROM ota_versions ORDER BY id DESC LIMIT 1")) return -1;
    if (!q.next()) return -1;
    fillOta(q, ver);
    return 0;
}

int DatabaseManager::otaGetVersionByVersion(const QString &version, ota_version_t *ver) {
    memset(ver, 0, sizeof(ota_version_t));
    DbConn c(this);
    if (!c.ok()) return -1;
    QSqlQuery q(c.db());
    q.prepare("SELECT id,version,filename,sha256,file_size,description,"
              "DATE_FORMAT(upload_time,'%Y-%m-%d %H:%i:%s'),force_update,type "
              "FROM ota_versions WHERE version=?");
    q.addBindValue(version);
    if (!q.exec() || !q.next()) return -1;
    fillOta(q, ver);
    return 0;
}

int DatabaseManager::otaGetAllVersions(ota_version_t *list, int *count) {
    *count = 0;
    DbConn c(this);
    if (!c.ok()) return -1;
    QSqlQuery q(c.db());
    if (!q.exec("SELECT id,version,filename,sha256,file_size,description,"
                "DATE_FORMAT(upload_time,'%Y-%m-%d %H:%i:%s'),force_update,type "
                "FROM ota_versions ORDER BY id DESC")) return -1;
    while (q.next() && *count < 50) fillOta(q, &list[(*count)++]);
    return 0;
}

int DatabaseManager::otaDeleteVersion(int id) {
    DbConn c(this);
    if (!c.ok()) return -1;
    QSqlQuery q(c.db());
    q.prepare("DELETE FROM ota_versions WHERE id=?");
    q.addBindValue(id);
    return q.exec() ? 0 : -1;
}

// =============================================================================
//  Balance log
// =============================================================================
int DatabaseManager::balanceLogQuery(const QString &memberUid, QList<QMap<QString, QVariant>> &logList, int limit) {
    logList.clear();
    DbConn c(this);
    if (!c.ok()) return -1;
    QSqlQuery q(c.db());
    q.prepare("SELECT id,member_uid,type,amount,old_balance,new_balance,"
              "operator_name,remark,DATE_FORMAT(create_time,'%Y-%m-%d %H:%i:%s') "
              "FROM balance_log WHERE member_uid=? ORDER BY id DESC LIMIT ?");
    q.addBindValue(memberUid); q.addBindValue(limit);
    if (!q.exec()) return -1;
    while (q.next()) {
        logList.append({
            {"id", q.value(0)}, {"member_uid", q.value(1)}, {"type", q.value(2)},
            {"amount", q.value(3)}, {"old_balance", q.value(4)}, {"new_balance", q.value(5)},
            {"operator_name", q.value(6)}, {"remark", q.value(7)}, {"create_time", q.value(8)}
        });
    }
    return 0;
}
