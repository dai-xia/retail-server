#ifndef BUSINESSMANAGER_H
#define BUSINESSMANAGER_H

#include <QObject>
#include <QMap>
#include <QHash>
#include <QMutex>
#include <QList>
#include <QPair>
#include <QVariant>
#include <cJSON.h>
#include "common.h"

class ServerManager;
class DatabaseManager;

class BusinessManager : public QObject
{
    Q_OBJECT
public:
    static BusinessManager* getInstance();
    void initialize();

    // Infrastructure (delegated to ServerManager / DatabaseManager)
    bool initDatabase();
    bool startServer(const QString& ip = "127.0.0.1", int port = 9090);
    void stopServer();
    bool isServerRunning();

    // Client management (for UI)
    QList<QPair<int, QString>> getConnectedClients();  // (fd, clientId or ip)
    QString getClientId(int fd);
    void kickClient(int fd);
    void sendToClient(int fd, const QString &jsonData);
    void sendToAllClients(const QString &jsonData);

    // OTA push (moved from OtaWidget)
    void otaPushToClient(int fd, const QString &version);
    void otaPushToAll(const QString &version);

    // Monitor control (server pushes monitor commands to clients)
    void monitorStart(int fd, const QString &rtspUrl);
    void monitorStop(int fd);

    // Member operations (for UI, delegate to DatabaseManager)
    int memberQueryAll(member_info_t *list, int *count);
    int memberQueryByUid(const QString &uid, member_info_t *member);
    int memberRegister(const QString &uid, const QString &name, const QString &phone,
                       double balance, const QString &password, const QString &facePath,
                       const QString &faceFeature, int memberType);
    int memberUpdateBalance(const QString &uid, double balance);
    int memberDelete(const QString &uid);
    int balanceLogQuery(const QString &uid, QList<QMap<QString, QVariant>> &logList, int limit = 50);

    // Goods operations (for UI)
    int goodsQueryAll(goods_info_t *list, int *count);
    int goodsQueryByClientId(const QString &clientId, goods_info_t *list, int *count);
    int goodsQueryById(int id, goods_info_t *goods);
    int goodsAdd(const QString &clientId, const QString &name, double price,
                 const QString &unit, int stock);
    int goodsUpdate(const QString &clientId, const QString &origName, const QString &name,
                    double price, const QString &unit, int stock);
    int goodsDelete(const QString &clientId, const QString &name);

    // Order operations (for UI)
    int orderQueryAll(order_info_t *list, int *count);
    int orderQueryByCondition(const QString &condition, order_info_t *list, int *count);
    int orderQueryById(int orderId, order_info_t *order);

    // OTA operations (for UI)
    int otaAddVersion(const QString &version, const QString &filename, const QString &sha256,
                      int fileSize, const QString &description, int forceUpdate, int type);
    int otaGetAllVersions(ota_version_t *list, int *count);
    int otaGetLatestVersion(ota_version_t *ver);
    int otaGetVersionByVersion(const QString &version, ota_version_t *ver);
    int otaDeleteVersion(int id);

private slots:
    void slotProcessClientData(int fd, QByteArray rawData);
    void slotClientDisconnected(int fd);
    void slotClientConnected(int fd, QString ip);

signals:
    void signalMemberDataChanged();
    void signalGoodsDataChanged();
    void signalOrderDataChanged();
    void signalOtaDataChanged();
    void signalAddLog(QString log);
    void signalClientListChanged();

private:
    explicit BusinessManager(QObject *parent = nullptr);

    /* Command dispatch table: O(1) routing replaces the if-else chain */
    typedef void (BusinessManager::*CmdHandler)(int fd, cJSON *root);
    QHash<QString, CmdHandler> m_dispatch;
    void initDispatch();

    QMap<int, QString> m_clientIdMap;  // fd -> clientId
    QMutex m_clientMapMutex;

    void processClientData(int fd, const QString &jsonData);
    void processMemberRegister(int fd, cJSON *root);
    void processMemberQuery(int fd, cJSON *root);
    void processMemberVerifyPassword(int fd, cJSON *root);
    void processMemberRecharge(int fd, cJSON *root);
    void processBalanceUpdate(int fd, cJSON *root);
    void processGoodsManagementAuth(int fd, cJSON *root);
    void processGoodsAdd(int fd, cJSON *root);
    void processGoodsUpdate(int fd, cJSON *root);
    void processGoodsDelete(int fd, cJSON *root);
    void processGoodsSync(int fd, cJSON *root);
    void processGoodsSyncReport(int fd, cJSON *root);
    void processStockDeduct(int fd, cJSON *root);
    void processOrderCreate(int fd, cJSON *root);
    void processOrderQuery(int fd, cJSON *root);
    void processFaceVerify(int fd, cJSON *root);
    void processOtaCheck(int fd, cJSON *root);
    void processOtaFileRequest(int fd, cJSON *root);
    void processClientRegister(int fd, cJSON *root);

    void sendResponse(int fd, const QString &cmd, int code, const QString &msg, cJSON* data = nullptr);

    double calculateSimilarity(cJSON* feature1, cJSON* feature2);

    void setClientId(int fd, const QString &clientId);
    void removeClient(int fd);
};

#endif
