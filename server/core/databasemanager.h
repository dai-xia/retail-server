#ifndef DATABASEMANAGER_H
#define DATABASEMANAGER_H

#include <QObject>
#include <QMutex>
#include <QString>
#include <QList>
#include <QQueue>
#include <QWaitCondition>
#include <QSqlDatabase>
#include "common.h"

class DatabaseManager : public QObject
{
    Q_OBJECT
public:
    static DatabaseManager* getInstance();

    bool initDatabase(const QString& host = "127.0.0.1", int port = 3306,
                      const QString& dbName = "retail_system",
                      const QString& user = "root",
                      const QString& password = "123",
                      int poolSize = 16);
    void closeDatabase();

    /************************* Member API ************************/
    int memberRegister(const QString& uid, const QString& name, const QString& phone,
                       double initBalance, const QString& password = QString(),
                       const QString& facePath = QString(), const QString& faceFeature = QString(),
                       int memberType = 0);
    int memberQueryByUid(const QString& uid, member_info_t* member);
    int memberQueryAll(member_info_t* list, int* count);
    int memberUpdateBalance(const QString& uid, double newBalance);
    int memberDelete(const QString& uid);
    int memberVerifyPassword(const QString& uid, const QString& password);
    int memberQueryType(const QString& uid, int* type);

    /************************* Goods & stock API ************************/
    int goodsAdd(const QString& clientId, const QString& goodsName, double price,
                 const QString& unit, int initStock);
    int goodsQueryById(int goodsId, goods_info_t* goods);
    int goodsQueryAll(goods_info_t* list, int* count);
    int goodsQueryByClientId(const QString& clientId, goods_info_t* list, int* count);
    int goodsDelete(const QString& clientId, const QString& goodsName);
    int stockDeduct(const QString& clientId, const QString& goodsName, int deductNum);

    /************************* Order API ************************/
    int orderQueryByCondition(const QString& condition, order_info_t* list, int* count);
    int orderQueryAll(order_info_t* list, int* count);
    int orderQueryById(int id, order_info_t* order);

    /************************* Composite business API ************************/
    int goodsUpdateWithStock(const QString& clientId, const QString& goodsName,
                             double price, const QString& unit, int stock);
    int syncClientGoods(const QString& clientId, const QList<goods_info_t>& goodsList);
    int getGoodsId(const QString& clientId, const QString& goodsName);

    /************************* Atomic transaction API ************************/
    int orderCreateAtomic(const QString& orderId, const QString& memberUid,
                          const QString& goodsList, double totalAmount,
                          QString& outMemberUid, double& outNewBalance);
    int balanceUpdateAtomic(const QString& memberUid, double amount, int type, double& outNewBalance);

    /************************* OTA management API ************************/
    int otaAddVersion(const QString& version, const QString& filename,
                      const QString& sha256, int fileSize,
                      const QString& description, int forceUpdate,
                      int type = 0 /* OTA_TYPE_APP / OTA_TYPE_SYSTEM */);
    int otaGetLatestVersion(ota_version_t* ver);
    int otaGetVersionByVersion(const QString& version, ota_version_t* ver);
    int otaGetAllVersions(ota_version_t* list, int* count);
    int otaDeleteVersion(int id);

    /************************* Balance log API ************************/
    int balanceLogQuery(const QString& memberUid, QList<QMap<QString, QVariant>>& logList,
                        int limit = 50);

private:
    explicit DatabaseManager(QObject *parent = nullptr);
    ~DatabaseManager();

    friend class DbConn;
    QSqlDatabase getConnection();
    void releaseConnection(QSqlDatabase& conn);

    QMutex m_poolMutex;
    QWaitCondition m_poolCond;
    QQueue<QString> m_connNames;   ///< connection-name queue (QSqlDatabase manages by name)
    int m_poolSize;
    bool m_initialized;

    QString m_host;
    int m_port;
    QString m_dbName;
    QString m_user;
    QString m_password;

    bool createTables();
};

#endif // DATABASEMANAGER_H
