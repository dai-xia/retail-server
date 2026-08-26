#include "businessmanager.h"
#include "servermanager.h"
#include "databasemanager.h"
#include "crypto.h"
#include <QDebug>
#include <QMutexLocker>
#include <QDateTime>
#include <QFile>
#include <QCoreApplication>
#include <time.h>
#include <stdlib.h>
#include <cmath>
#include <cJSON.h>

/* Validate OTA version / filename to prevent path traversal */
static bool isValidOtaName(const QString &s) {
    if (s.isEmpty() || s.length() > 128) return false;
    for (auto c : s) {
        if (!c.isLetterOrNumber() && c != '.' && c != '-' && c != '_') return false;
    }
    return !s.contains("..");
}

// File-static helper: calculate cosine similarity between two feature JSON objects
double BusinessManager::calculateSimilarity(cJSON* feature1, cJSON* feature2)
{
    cJSON* featureArray1 = cJSON_GetObjectItem(feature1, "feature");
    cJSON* featureArray2 = cJSON_GetObjectItem(feature2, "feature");

    if(!cJSON_IsArray(featureArray1) || !cJSON_IsArray(featureArray2))
    {
        return 0.0;
    }

    int size1 = cJSON_GetArraySize(featureArray1);
    int size2 = cJSON_GetArraySize(featureArray2);

    if(size1 != size2)
    {
        return 0.0;
    }

    double dotProduct = 0.0;
    double norm1 = 0.0;
    double norm2 = 0.0;

    for(int i=0; i<size1; i++)
    {
        cJSON* item1 = cJSON_GetArrayItem(featureArray1, i);
        cJSON* item2 = cJSON_GetArrayItem(featureArray2, i);

        if(cJSON_IsNumber(item1) && cJSON_IsNumber(item2))
        {
            double val1 = item1->valuedouble;
            double val2 = item2->valuedouble;

            dotProduct += val1 * val2;
            norm1 += val1 * val1;
            norm2 += val2 * val2;
        }
    }

    if(norm1 == 0 || norm2 == 0)
    {
        return 0.0;
    }

    return dotProduct / (sqrt(norm1) * sqrt(norm2));
}

static QMutex s_businessMutex;
static BusinessManager* s_businessInstance = nullptr;

BusinessManager::BusinessManager(QObject *parent)
    : QObject(parent)
{
    initDispatch();
}

void BusinessManager::initDispatch()
{
    m_dispatch = {
        {"client_register",       &BusinessManager::processClientRegister},
        {"member_register",       &BusinessManager::processMemberRegister},
        {"member_query",          &BusinessManager::processMemberQuery},
        {"member_verify_password",&BusinessManager::processMemberVerifyPassword},
        {"member_recharge",       &BusinessManager::processMemberRecharge},
        {"balance_update",        &BusinessManager::processBalanceUpdate},
        {"goods_management_auth", &BusinessManager::processGoodsManagementAuth},
        {"goods_add",             &BusinessManager::processGoodsAdd},
        {"goods_update",          &BusinessManager::processGoodsUpdate},
        {"goods_delete",          &BusinessManager::processGoodsDelete},
        {"goods_sync_request",    &BusinessManager::processGoodsSync},
        {"goods_sync_report",     &BusinessManager::processGoodsSyncReport},
        {"stock_deduct",          &BusinessManager::processStockDeduct},
        {"order_create",          &BusinessManager::processOrderCreate},
        {"order_query",           &BusinessManager::processOrderQuery},
        {"face_verify",           &BusinessManager::processFaceVerify},
        {"ota_check",             &BusinessManager::processOtaCheck},
        {"ota_file_request",      &BusinessManager::processOtaFileRequest},
    };
}

void BusinessManager::initialize()
{
    /* DirectConnection: signals come from net thread-pool workers and must be
     * handled synchronously in this thread */
    connect(ServerManager::getInstance(), &ServerManager::signalClientData,
            this, &BusinessManager::slotProcessClientData, Qt::DirectConnection);
    connect(ServerManager::getInstance(), &ServerManager::signalClientDisconnected,
            this, &BusinessManager::slotClientDisconnected, Qt::DirectConnection);
    connect(ServerManager::getInstance(), &ServerManager::signalClientConnected,
            this, &BusinessManager::slotClientConnected, Qt::DirectConnection);
}

BusinessManager* BusinessManager::getInstance()
{
    if (s_businessInstance == nullptr) {
        QMutexLocker locker(&s_businessMutex);
        if (s_businessInstance == nullptr) {
            s_businessInstance = new BusinessManager();
        }
    }
    return s_businessInstance;
}

bool BusinessManager::initDatabase()
{
    return DatabaseManager::getInstance()->initDatabase();
}

bool BusinessManager::startServer(const QString& ip, int port)
{
    return ServerManager::getInstance()->startServer(ip, port);
}

void BusinessManager::stopServer()
{
    ServerManager::getInstance()->stopServer();
}

bool BusinessManager::isServerRunning()
{
    return ServerManager::getInstance()->isServerRunning();
}

/************************* UI-facing methods *************************/

QList<QPair<int, QString>> BusinessManager::getConnectedClients()
{
    QMutexLocker lock(&m_clientMapMutex);
    auto clients = ServerManager::getInstance()->getConnectedClients();
    QList<QPair<int, QString>> result;
    for (auto &p : clients) {
        QString id = m_clientIdMap.value(p.first, "");
        result.append({p.first, id.isEmpty() ? p.second : id});
    }
    return result;
}

void BusinessManager::kickClient(int fd)
{
    removeClient(fd);
    ServerManager::getInstance()->closeClient(fd);
}

void BusinessManager::sendToClient(int fd, const QString &jsonData)
{
    QByteArray plainData = jsonData.toUtf8();
    unsigned char *encData = nullptr;
    int encLen = 0;
    if (crypto_encrypt((const unsigned char*)plainData.constData(), plainData.size(), &encData, &encLen) != 0) {
        qDebug() << "encryption failed";
        return;
    }
    QByteArray encBuf((const char*)encData, encLen);
    free(encData);
    ServerManager::getInstance()->sendToClient(fd, encBuf);
}

void BusinessManager::sendToAllClients(const QString &jsonData)
{
    QByteArray plainData = jsonData.toUtf8();
    unsigned char *encData = nullptr;
    int encLen = 0;
    if (crypto_encrypt((const unsigned char*)plainData.constData(), plainData.size(), &encData, &encLen) != 0) {
        qDebug() << "encryption failed";
        return;
    }
    QByteArray encBuf((const char*)encData, encLen);
    free(encData);
    ServerManager::getInstance()->sendToAllClients(encBuf);
}

void BusinessManager::otaPushToClient(int fd, const QString &version)
{
    ota_version_t ver;
    if (otaGetVersionByVersion(version, &ver) != 0) {
        qDebug() << "OTA push failed: version" << version << "not found";
        return;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", "ota_push");
    cJSON_AddStringToObject(root, "version", ver.version);
    cJSON_AddStringToObject(root, "filename", ver.filename);
    cJSON_AddStringToObject(root, "sha256", ver.sha256);
    cJSON_AddNumberToObject(root, "file_size", ver.file_size);
    cJSON_AddStringToObject(root, "description", ver.description);
    cJSON_AddNumberToObject(root, "type", (int)ver.type);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return;
    sendToClient(fd, QString(json));
    free(json);
}

void BusinessManager::otaPushToAll(const QString &version)
{
    ota_version_t ver;
    if (otaGetVersionByVersion(version, &ver) != 0) {
        qDebug() << "OTA broadcast failed: version" << version << "not found";
        return;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", "ota_push");
    cJSON_AddStringToObject(root, "version", ver.version);
    cJSON_AddStringToObject(root, "filename", ver.filename);
    cJSON_AddStringToObject(root, "sha256", ver.sha256);
    cJSON_AddNumberToObject(root, "file_size", ver.file_size);
    cJSON_AddStringToObject(root, "description", ver.description);
    cJSON_AddNumberToObject(root, "type", (int)ver.type);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return;
    sendToAllClients(QString(json));
    free(json);
}

/************************* Monitor control (server push) *************************/

/**
 * @brief Push a start-monitor command to a client
 * @param fd      client connection fd
 * @param rtspUrl RTSP URL the client should stream to
 *
 * Flow: server UI picks a client -> send {"cmd":"monitor_start","rtsp_url":...}
 * -> client opens the camera + RTSP push -> server StreamReceiver pulls and
 * decodes the RTSP stream for display.
 */
void BusinessManager::monitorStart(int fd, const QString &rtspUrl)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", "monitor_start");
    cJSON_AddStringToObject(root, "rtsp_url", rtspUrl.toUtf8().constData());
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return;

    sendToClient(fd, QString(json));
    free(json);

    emit signalAddLog(QString("monitor command sent to client (fd=%1), stream url: %2").arg(fd).arg(rtspUrl));
}

/**
 * @brief Push a stop-monitor command to a client
 */
void BusinessManager::monitorStop(int fd)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", "monitor_stop");
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return;

    sendToClient(fd, QString(json));
    free(json);

    emit signalAddLog(QString("stop-monitor command sent to client (fd=%1)").arg(fd));
}

/************************* Member operations (for UI) *************************/

int BusinessManager::memberQueryAll(member_info_t *list, int *count)
{
    return DatabaseManager::getInstance()->memberQueryAll(list, count);
}

int BusinessManager::memberQueryByUid(const QString &uid, member_info_t *member)
{
    return DatabaseManager::getInstance()->memberQueryByUid(uid, member);
}

int BusinessManager::memberRegister(const QString &uid, const QString &name, const QString &phone,
                                    double balance, const QString &password, const QString &facePath,
                                    const QString &faceFeature, int memberType)
{
    int r = DatabaseManager::getInstance()->memberRegister(uid, name, phone, balance,
                                                           password, facePath, faceFeature, memberType);
    if (r == 0) emit signalMemberDataChanged();
    return r;
}

int BusinessManager::memberUpdateBalance(const QString &uid, double balance)
{
    int r = DatabaseManager::getInstance()->memberUpdateBalance(uid, balance);
    if (r == 0) emit signalMemberDataChanged();
    return r;
}

int BusinessManager::memberDelete(const QString &uid)
{
    int r = DatabaseManager::getInstance()->memberDelete(uid);
    if (r == 0) emit signalMemberDataChanged();
    return r;
}

int BusinessManager::balanceLogQuery(const QString &uid, QList<QMap<QString, QVariant>> &logList, int limit)
{
    return DatabaseManager::getInstance()->balanceLogQuery(uid, logList, limit);
}

/************************* Goods operations (for UI) *************************/

int BusinessManager::goodsQueryAll(goods_info_t *list, int *count)
{
    return DatabaseManager::getInstance()->goodsQueryAll(list, count);
}

int BusinessManager::goodsQueryByClientId(const QString &clientId, goods_info_t *list, int *count)
{
    return DatabaseManager::getInstance()->goodsQueryByClientId(clientId, list, count);
}

int BusinessManager::goodsQueryById(int id, goods_info_t *goods)
{
    return DatabaseManager::getInstance()->goodsQueryById(id, goods);
}

int BusinessManager::goodsAdd(const QString &clientId, const QString &name, double price,
                               const QString &unit, int stock)
{
    int r = DatabaseManager::getInstance()->goodsAdd(clientId, name, price, unit, stock);
    if (r == 0) emit signalGoodsDataChanged();
    return r;
}

int BusinessManager::goodsUpdate(const QString &clientId, const QString &origName,
                                  const QString &name, double price,
                                  const QString &unit, int stock)
{
    Q_UNUSED(name);
    int r = DatabaseManager::getInstance()->goodsUpdateWithStock(clientId, origName, price, unit, stock);
    if (r == 0) emit signalGoodsDataChanged();
    return r;
}

int BusinessManager::goodsDelete(const QString &clientId, const QString &name)
{
    int r = DatabaseManager::getInstance()->goodsDelete(clientId, name);
    if (r == 0) emit signalGoodsDataChanged();
    return r;
}

/************************* Order operations (for UI) *************************/

int BusinessManager::orderQueryAll(order_info_t *list, int *count)
{
    return DatabaseManager::getInstance()->orderQueryAll(list, count);
}

int BusinessManager::orderQueryByCondition(const QString &condition, order_info_t *list, int *count)
{
    return DatabaseManager::getInstance()->orderQueryByCondition(condition, list, count);
}

int BusinessManager::orderQueryById(int orderId, order_info_t *order)
{
    return DatabaseManager::getInstance()->orderQueryById(orderId, order);
}

/************************* OTA operations (for UI) *************************/

int BusinessManager::otaAddVersion(const QString &version, const QString &filename, const QString &sha256,
                                    int fileSize, const QString &description, int forceUpdate, int type)
{
    int r = DatabaseManager::getInstance()->otaAddVersion(version, filename, sha256, fileSize, description, forceUpdate, type);
    if (r == 0) emit signalOtaDataChanged();
    return r;
}

int BusinessManager::otaGetAllVersions(ota_version_t *list, int *count)
{
    return DatabaseManager::getInstance()->otaGetAllVersions(list, count);
}

int BusinessManager::otaGetLatestVersion(ota_version_t *ver)
{
    return DatabaseManager::getInstance()->otaGetLatestVersion(ver);
}

int BusinessManager::otaGetVersionByVersion(const QString &version, ota_version_t *ver)
{
    return DatabaseManager::getInstance()->otaGetVersionByVersion(version, ver);
}

int BusinessManager::otaDeleteVersion(int id)
{
    int r = DatabaseManager::getInstance()->otaDeleteVersion(id);
    if (r == 0) emit signalOtaDataChanged();
    return r;
}

/************************* Core: sendResponse *************************/

void BusinessManager::sendResponse(int fd, const QString &cmd, int code, const QString &msg, cJSON *data)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", cmd.toUtf8().constData());
    cJSON_AddNumberToObject(root, "code", code);
    cJSON_AddStringToObject(root, "msg", msg.toUtf8().constData());
    if(data != nullptr)
    {
        cJSON_AddItemToObject(root, "data", data);
    }
    char *jsonStr = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!jsonStr) return;
    sendToClient(fd, QString::fromUtf8(jsonStr));
    free(jsonStr);
}

/************************* Core: slot / processClientData *************************/

void BusinessManager::slotProcessClientData(int fd, QByteArray rawData)
{
    unsigned char *plainData = nullptr;
    int plainLen = 0;
    if (crypto_decrypt((const unsigned char*)rawData.constData(), rawData.size(), &plainData, &plainLen) != 0) {
        qDebug() << "decryption failed";
        return;
    }
    QString jsonData = QString::fromUtf8((const char*)plainData, plainLen);
    free(plainData);
    processClientData(fd, jsonData);
}

void BusinessManager::slotClientDisconnected(int fd)
{
    removeClient(fd);
    emit signalClientListChanged();
}

void BusinessManager::slotClientConnected(int fd, QString ip)
{
    Q_UNUSED(fd);
    Q_UNUSED(ip);
    emit signalClientListChanged();
}

void BusinessManager::processClientData(int fd, const QString &jsonData)
{
    cJSON *root = cJSON_Parse(jsonData.toUtf8().constData());
    if(root == nullptr)
    {
        sendResponse(fd, "error", -1, "JSON解析失败");
        return;
    }

    cJSON *cmdItem = cJSON_GetObjectItem(root, "cmd");
    if(!cJSON_IsString(cmdItem))
    {
        cJSON_Delete(root);
        sendResponse(fd, "error", -1, "参数错误: 缺少cmd");
        return;
    }
    QString cmd = cmdItem->valuestring;
    qDebug() << "client command:" << cmd << " FD:" << fd;

    /* O(1) command dispatch table replaces the if-else chain */
    auto it = m_dispatch.find(cmd);
    if (it != m_dispatch.end()) {
        (this->*it.value())(fd, root);
    }
    /* inline commands */
    else if (cmd == "heartbeat") {
        sendResponse(fd, "heartbeat_ack", 0, "ok");
    }
    else if (cmd == "ping") {
        sendResponse(fd, "pong", 0, "ok");
    }
    else {
        qDebug() << "unknown command:" << cmd;
        sendResponse(fd, "error", -1, "未知命令");
    }

    cJSON_Delete(root);
}

/************************* Process methods *************************/

void BusinessManager::processMemberRegister(int fd, cJSON *root)
{
    cJSON *uidItem = cJSON_GetObjectItem(root, "uid");
    cJSON *nameItem = cJSON_GetObjectItem(root, "name");
    cJSON *phoneItem = cJSON_GetObjectItem(root, "phone");
    cJSON *balanceItem = cJSON_GetObjectItem(root, "balance");

    if(!uidItem || !cJSON_IsString(uidItem) ||
       !nameItem || !cJSON_IsString(nameItem) ||
       !phoneItem || !cJSON_IsString(phoneItem) ||
       !balanceItem || !cJSON_IsNumber(balanceItem))
    {
        sendResponse(fd, "member_register", -1, "参数错误: 缺少uid/name/phone/balance");
        return;
    }

    char *uid = uidItem->valuestring;
    char *name = nameItem->valuestring;
    char *phone = phoneItem->valuestring;
    double balance = balanceItem->valuedouble;

    if(balance > 0)
    {
        qDebug() << QString("warning: non-zero initial balance on member register: uid=%1, balance=%2").arg(uid).arg(balance);
    }

    /* optional: password */
    char *password = nullptr;
    cJSON *passwordItem = cJSON_GetObjectItem(root, "password");
    if(passwordItem != nullptr && cJSON_IsString(passwordItem))
    {
        password = passwordItem->valuestring;
    }

    /* optional: face path */
    char *face_path = nullptr;
    cJSON *facePathItem = cJSON_GetObjectItem(root, "face");
    if(facePathItem != nullptr && cJSON_IsString(facePathItem))
    {
        face_path = facePathItem->valuestring;
    }

    /* optional: face feature */
    char *face_feature = nullptr;
    cJSON *faceFeatureItem = cJSON_GetObjectItem(root, "feature");
    if(faceFeatureItem != nullptr && cJSON_IsString(faceFeatureItem))
    {
        face_feature = faceFeatureItem->valuestring;
    }

    /* optional: member type (default 0 = normal customer) */
    int memberType = 0;
    cJSON *typeItem = cJSON_GetObjectItem(root, "type");
    if(typeItem != nullptr && cJSON_IsNumber(typeItem))
    {
        memberType = typeItem->valueint;
    }

    QString pwd = password ? QString(password) : QString();
    QString face = face_path ? QString(face_path) : QString();
    QString feature = face_feature ? QString(face_feature) : QString();

    int ret = DatabaseManager::getInstance()->memberRegister(uid, name, phone, balance, pwd, face, feature, memberType);
    if(ret == 0)
    {
        emit signalAddLog(QString("member registered: UID=%1, name=%2").arg(uid).arg(name));
        emit signalMemberDataChanged();
        sendResponse(fd, "member_register", 0, "注册成功");
    }
    else
    {
        sendResponse(fd, "member_register", -1, "注册失败，UID或手机号已存在");
    }
}

void BusinessManager::processGoodsManagementAuth(int fd, cJSON *root)
{
    cJSON *uidItem = cJSON_GetObjectItem(root, "uid");
    cJSON *passwordItem = cJSON_GetObjectItem(root, "password");

    if(!uidItem || !cJSON_IsString(uidItem) ||
       !passwordItem || !cJSON_IsString(passwordItem))
    {
        sendResponse(fd, "goods_management_auth_result", -1, "参数错误: 缺少uid/password");
        return;
    }

    QString uid = uidItem->valuestring;
    QString password = passwordItem->valuestring;

    /* 1. verify password */
    int ret = DatabaseManager::getInstance()->memberVerifyPassword(uid, password);
    if(ret != 0)
    {
        sendResponse(fd, "goods_management_auth_result", -1, "密码错误");
        return;
    }

    /* 2. check admin role */
    int memberType = 0;
    ret = DatabaseManager::getInstance()->memberQueryType(uid, &memberType);
    if(ret != 0 || memberType != 1)
    {
        sendResponse(fd, "goods_management_auth_result", -1, "权限不足，仅管理员可操作");
        return;
    }

    emit signalAddLog(QString("admin auth success: UID=%1").arg(uid));
    sendResponse(fd, "goods_management_auth_result", 0, "管理员认证成功");
}

void BusinessManager::processFaceVerify(int fd, cJSON *root)
{
    cJSON *uidItem = cJSON_GetObjectItem(root, "uid");
    cJSON *faceFeatureItem = cJSON_GetObjectItem(root, "face_feature");

    if(!uidItem || !cJSON_IsString(uidItem) ||
       !faceFeatureItem || !cJSON_IsString(faceFeatureItem))
    {
        sendResponse(fd, "face_verify", -1, "参数错误: 缺少uid/face_feature");
        return;
    }

    char *uid = uidItem->valuestring;
    char *face_feature = faceFeatureItem->valuestring;

    qDebug() << "face verify request - UID:" << uid;

    member_info_t member;
    int ret = DatabaseManager::getInstance()->memberQueryByUid(uid, &member);

    if(ret == 0 && strlen(member.face_feature) > 0)
    {
        /* Parse feature vectors (MobileFaceNet format) */
        cJSON *storedFeature = cJSON_Parse(member.face_feature);
        cJSON *currentFeature = cJSON_Parse(face_feature);

        if(storedFeature && currentFeature)
        {
            /* Detect algorithm version */
            const char *version = nullptr;
            cJSON *versionItem = cJSON_GetObjectItem(currentFeature, "version");
            if(cJSON_IsString(versionItem))
            {
                version = versionItem->valuestring;
                qDebug() << "client algorithm version:" << version;
            }

            /* Cosine similarity */
            double similarity = calculateSimilarity(storedFeature, currentFeature);

            qDebug() << "face compare result - UID:" << uid << " similarity:" << similarity;

            /* Cosine-similarity threshold for MobileFaceNet + L2 normalization */
            static constexpr double MOBILEFACENET_THRESHOLD = 0.65;  /* payment-grade threshold */

            if(similarity >= MOBILEFACENET_THRESHOLD)
            {
                sendResponse(fd, "face_verify", 0, "验证成功");
                emit signalAddLog(QString("✓ face verify success: UID=%1, similarity=%2")
                                  .arg(uid).arg(similarity, 0, 'f', 4));
            }
            else
            {
                sendResponse(fd, "face_verify", -1, QString("人脸不匹配（相似度：%1）").arg(similarity, 0, 'f', 4));
                emit signalAddLog(QString("✗ face verify failed: UID=%1, similarity=%2 < threshold %3")
                                  .arg(uid).arg(similarity, 0, 'f', 4).arg(MOBILEFACENET_THRESHOLD));
            }

            cJSON_Delete(storedFeature);
            cJSON_Delete(currentFeature);
        }
        else
        {
            qWarning() << "face feature JSON parse failed";
            sendResponse(fd, "face_verify", -1, "特征数据格式错误");
        }
    }
    else
    {
        qWarning() << "member not found or no face registered - UID:" << uid;
        sendResponse(fd, "face_verify", -1, "会员不存在或未注册人脸");
    }
}

void BusinessManager::processMemberQuery(int fd, cJSON *root)
{
    cJSON *uidItem = cJSON_GetObjectItem(root, "uid");
    if(!uidItem || !cJSON_IsString(uidItem))
    {
        sendResponse(fd, "member_query", -1, "参数错误: 缺少uid");
        return;
    }
    char *uid = uidItem->valuestring;

    member_info_t member;
    int ret = DatabaseManager::getInstance()->memberQueryByUid(uid, &member);
    if(ret == 0)
    {
        cJSON *data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "uid", member.uid);
        cJSON_AddStringToObject(data, "name", member.name);
        cJSON_AddStringToObject(data, "phone", member.phone);
        cJSON_AddNumberToObject(data, "balance", member.balance);
        if(strlen(member.face_path) > 0)
        {
            cJSON_AddStringToObject(data, "face_path", member.face_path);
        }
        if(strlen(member.face_feature) > 0)
        {
            cJSON_AddStringToObject(data, "face_feature", member.face_feature);
        }
        sendResponse(fd, "member_query", 0, "查询成功", data);
    }
    else
    {
        sendResponse(fd, "member_query", -1, "会员不存在");
    }
}

void BusinessManager::processMemberVerifyPassword(int fd, cJSON *root)
{
    cJSON *uidItem = cJSON_GetObjectItem(root, "uid");
    cJSON *passwordItem = cJSON_GetObjectItem(root, "password");

    if(!uidItem || !cJSON_IsString(uidItem) ||
       !passwordItem || !cJSON_IsString(passwordItem))
    {
        sendResponse(fd, "member_verify_password", -1, "参数错误: 缺少uid/password");
        return;
    }

    char *uid = uidItem->valuestring;
    char *password = passwordItem->valuestring;

    int ret = DatabaseManager::getInstance()->memberVerifyPassword(uid, password);
    if(ret == 0)
    {
        sendResponse(fd, "member_verify_password", 0, "密码验证成功");
    }
    else
    {
        sendResponse(fd, "member_verify_password", -1, "密码错误或会员不存在");
    }
}

void BusinessManager::processMemberRecharge(int fd, cJSON *root)
{
    cJSON *uidItem = cJSON_GetObjectItem(root, "uid");
    cJSON *amountItem = cJSON_GetObjectItem(root, "amount");

    if(!uidItem || !cJSON_IsString(uidItem) ||
       !amountItem || !cJSON_IsNumber(amountItem))
    {
        sendResponse(fd, "member_recharge", -1, "参数错误: 缺少uid/amount");
        return;
    }

    char *uid = uidItem->valuestring;
    double amount = amountItem->valuedouble;

    if(amount <= 0)
    {
        sendResponse(fd, "member_recharge", -1, "充值金额必须大于0");
        return;
    }

    double newBalance = 0;
    int ret = DatabaseManager::getInstance()->balanceUpdateAtomic(uid, amount, 1, newBalance);
    if(ret == 0)
    {
        emit signalAddLog(QString("member recharge success: UID=%1, amount=%2").arg(uid).arg(amount));
        emit signalMemberDataChanged();

        cJSON *data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "uid", uid);
        cJSON_AddNumberToObject(data, "amount", amount);
        sendResponse(fd, "member_recharge", 0, "充值成功", data);
    }
    else
    {
        sendResponse(fd, "member_recharge", -1, "充值失败，会员不存在");
    }
}

void BusinessManager::processGoodsUpdate(int fd, cJSON *root)
{
    cJSON *goodsNameItem = cJSON_GetObjectItem(root, "goods_name");
    cJSON *priceItem = cJSON_GetObjectItem(root, "price");
    cJSON *unitItem = cJSON_GetObjectItem(root, "unit");
    cJSON *stockItem = cJSON_GetObjectItem(root, "stock");

    if(!goodsNameItem || !cJSON_IsString(goodsNameItem) ||
       !priceItem || !cJSON_IsNumber(priceItem) ||
       !unitItem || !cJSON_IsString(unitItem) ||
       !stockItem || !cJSON_IsNumber(stockItem))
    {
        sendResponse(fd, "goods_update", -1, "参数错误: 缺少goods_name/price/unit/stock");
        return;
    }

    char *goods_name = goodsNameItem->valuestring;
    double price = priceItem->valuedouble;
    char *unit = unitItem->valuestring;
    int stock = stockItem->valueint;

    QString clientId = getClientId(fd);

    int ret = DatabaseManager::getInstance()->goodsUpdateWithStock(clientId, goods_name, price, unit, stock);
    if(ret == 0)
    {
        emit signalAddLog(QString("goods updated: %1").arg(goods_name));
        emit signalGoodsDataChanged();
        sendResponse(fd, "goods_update", 0, "商品修改成功");
    }
    else
    {
        qDebug() << "goods update failed";
        sendResponse(fd, "goods_update", -1, "商品修改失败");
    }
}

void BusinessManager::processGoodsDelete(int fd, cJSON *root)
{
    cJSON *goodsNameItem = cJSON_GetObjectItem(root, "goods_name");
    if(!goodsNameItem || !cJSON_IsString(goodsNameItem))
    {
        sendResponse(fd, "goods_delete", -1, "参数错误: 缺少goods_name");
        return;
    }

    char *goods_name = goodsNameItem->valuestring;

    QString clientId = getClientId(fd);

    int ret = DatabaseManager::getInstance()->goodsDelete(clientId, goods_name);
    if(ret == 0)
    {
        emit signalAddLog(QString("goods deleted: %1").arg(goods_name));
        emit signalGoodsDataChanged();
        sendResponse(fd, "goods_delete", 0, "商品删除成功");
    }
    else
    {
        qDebug() << "goods delete failed";
        sendResponse(fd, "goods_delete", -1, "商品删除失败");
    }
}

void BusinessManager::processOrderCreate(int fd, cJSON *root)
{
    cJSON *memberUidItem = cJSON_GetObjectItem(root, "member_uid");
    cJSON *goodsList = cJSON_GetObjectItem(root, "goods_list");
    cJSON *totalAmountItem = cJSON_GetObjectItem(root, "total_amount");

    if(!memberUidItem || !cJSON_IsString(memberUidItem) ||
       !goodsList || !cJSON_IsArray(goodsList) ||
       !totalAmountItem || !cJSON_IsNumber(totalAmountItem))
    {
        sendResponse(fd, "order_create", -1, "参数错误: 缺少member_uid/goods_list/total_amount");
        return;
    }

    char *member_uid = memberUidItem->valuestring;
    double total_amount = totalAmountItem->valuedouble;

    /* Order id supplied by the client (generated client-side) */
    char *client_order_id = nullptr;
    cJSON *orderIdItem = cJSON_GetObjectItem(root, "order_id");
    if(orderIdItem != nullptr && cJSON_IsString(orderIdItem))
    {
        client_order_id = orderIdItem->valuestring;
    }

    QString clientId = getClientId(fd);

    /* Use the client-supplied id, or generate a new one */
    char order_id[32] = {0};
    if(client_order_id != nullptr)
    {
        snprintf(order_id, sizeof(order_id), "%s", client_order_id);
    }
    else
    {
        time_t now = time(NULL);
        snprintf(order_id, sizeof(order_id), "ORD%ld%04d", now, rand() % 10000);
    }

    char *goods_list_str = cJSON_Print(goodsList);
    if (!goods_list_str) {
        sendResponse(fd, "order_create", -1, "订单创建失败");
        return;
    }

    QString outMemberUid;
    double outNewBalance = 0;
    int ret = DatabaseManager::getInstance()->orderCreateAtomic(
        order_id, member_uid, goods_list_str, total_amount,
        outMemberUid, outNewBalance);
    free(goods_list_str);

    if(ret == -2)
    {
        sendResponse(fd, "order_create", -1, "会员不存在");
        return;
    }
    else if(ret == -3)
    {
        sendResponse(fd, "order_create", -1, "余额不足");
        return;
    }
    else if(ret != 0)
    {
        sendResponse(fd, "order_create", -1, "订单创建失败");
        return;
    }

    emit signalAddLog(QString("order created: order_id=%1, member=%2, amount=%3, client=%4")
                      .arg(order_id).arg(member_uid).arg(total_amount).arg(clientId));
    emit signalOrderDataChanged();
    sendResponse(fd, "order_create", 0, "订单创建成功");
}

void BusinessManager::processOrderQuery(int fd, cJSON *root)
{
    cJSON *conditionItem = cJSON_GetObjectItem(root, "condition");
    if(!conditionItem || !cJSON_IsString(conditionItem))
    {
        sendResponse(fd, "order_query", -1, "参数错误: 缺少condition");
        return;
    }

    char *condition = conditionItem->valuestring;

    order_info_t list[100];
    int count = 0;

    int ret = DatabaseManager::getInstance()->orderQueryByCondition(condition, list, &count);
    if(ret != 0)
    {
        sendResponse(fd, "order_query", -1, "查询失败");
        return;
    }

    cJSON *orderArray = cJSON_CreateArray();
    for(int i = 0; i < count; i++)
    {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "order_id", list[i].order_id);
        cJSON_AddStringToObject(item, "member_uid", list[i].member_uid);
        cJSON_AddNumberToObject(item, "total_amount", list[i].total);
        cJSON_AddNumberToObject(item, "pay_status", list[i].pay_status);

        QDateTime time = QDateTime::fromSecsSinceEpoch(list[i].create_time);
        cJSON_AddStringToObject(item, "create_time", time.toString("yyyy-MM-dd hh:mm:ss").toUtf8().constData());

        cJSON_AddItemToArray(orderArray, item);
    }

    cJSON *data = cJSON_CreateObject();
    cJSON_AddItemToObject(data, "order_list", orderArray);
    sendResponse(fd, "order_query", 0, "查询成功", data);
    emit signalAddLog(QString("client order query done: condition=%1, %2 orders").arg(condition).arg(count));
}

void BusinessManager::processBalanceUpdate(int fd, cJSON *root)
{
    cJSON *memberUidItem = cJSON_GetObjectItem(root, "member_uid");
    cJSON *amountItem = cJSON_GetObjectItem(root, "amount");
    cJSON *typeItem = cJSON_GetObjectItem(root, "type");

    if(!memberUidItem || !cJSON_IsString(memberUidItem) ||
       !amountItem || !cJSON_IsNumber(amountItem) ||
       !typeItem || !cJSON_IsNumber(typeItem))
    {
        sendResponse(fd, "balance_update", -1, "参数错误: 缺少member_uid/amount/type");
        return;
    }

    char *member_uid = memberUidItem->valuestring;
    double amount = amountItem->valuedouble;
    int type = typeItem->valueint;

    double outNewBalance = 0;
    int ret = DatabaseManager::getInstance()->balanceUpdateAtomic(member_uid, amount, type, outNewBalance);
    if(ret == -2)
    {
        sendResponse(fd, "balance_update", -1, "会员不存在");
        return;
    }
    else if(ret == -3)
    {
        sendResponse(fd, "balance_update", -1, "余额不足");
        return;
    }
    else if(ret == 0)
    {
        QString action = (type == 1) ? "recharge" : "deduct";
        emit signalAddLog(QString("balance %1 success: member=%2, amount=%3, new balance=%4")
                          .arg(action).arg(member_uid).arg(amount).arg(outNewBalance));
        emit signalMemberDataChanged();
        sendResponse(fd, "balance_update", 0, "余额更新成功");
    }
    else
    {
        sendResponse(fd, "balance_update", -1, "余额更新失败");
    }
}

void BusinessManager::processClientRegister(int fd, cJSON *root)
{
    cJSON *clientIdItem = cJSON_GetObjectItem(root, "client_id");
    if(!clientIdItem || !cJSON_IsString(clientIdItem))
    {
        sendResponse(fd, "client_register", -1, "缺少client_id参数");
        return;
    }

    QString clientId = QString::fromUtf8(clientIdItem->valuestring);
    setClientId(fd, clientId);

    emit signalAddLog(QString("📱 client registered: FD=%1, ID=%2").arg(fd).arg(clientId));
    sendResponse(fd, "client_register", 0, "注册成功");

    emit signalClientListChanged();
}

/* ==================== Goods sync (filtered by client_id) ==================== */

void BusinessManager::processGoodsSync(int fd, cJSON *root)
{
    QString clientId = getClientId(fd);

    cJSON *clientIdItem = cJSON_GetObjectItem(root, "client_id");
    if(clientIdItem && cJSON_IsString(clientIdItem) && strlen(clientIdItem->valuestring) > 0)
    {
        clientId = clientIdItem->valuestring;
    }

    qDebug() << "goods sync request - client_id:" << clientId;

    goods_info_t list[100];
    int count = 0;

    int ret = DatabaseManager::getInstance()->goodsQueryByClientId(clientId, list, &count);
    if(ret != 0)
    {
        sendResponse(fd, "goods_sync", -1, "同步失败");
        return;
    }

    cJSON *goodsArray = cJSON_CreateArray();
    for(int i = 0; i < count; i++)
    {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "goods_id", list[i].id);
        cJSON_AddStringToObject(item, "goods_name", list[i].name);
        cJSON_AddNumberToObject(item, "price", list[i].price);
        cJSON_AddStringToObject(item, "unit", list[i].unit);

        QDateTime time = QDateTime::fromSecsSinceEpoch(list[i].create_time);
        cJSON_AddStringToObject(item, "create_time", time.toString("yyyy-MM-dd hh:mm:ss").toUtf8().constData());

        cJSON_AddNumberToObject(item, "stock_num", list[i].stock);
        cJSON_AddNumberToObject(item, "version", list[i].version);

        cJSON_AddItemToArray(goodsArray, item);
    }

    cJSON *data = cJSON_CreateObject();
    cJSON_AddItemToObject(data, "goods_list", goodsArray);
    cJSON_AddStringToObject(data, "client_id", clientId.toUtf8().constData());
    sendResponse(fd, "goods_sync", 0, "同步成功", data);
    emit signalAddLog(QString("client [%1] goods sync done, %2 goods").arg(clientId).arg(count));
}

void BusinessManager::processGoodsSyncReport(int fd, cJSON *root)
{
    QString clientId = getClientId(fd);
    qDebug() << "client goods sync report - client_id:" << clientId;

    cJSON *goodsList = cJSON_GetObjectItem(root, "goods_list");
    if(goodsList == nullptr || !cJSON_IsArray(goodsList))
    {
        sendResponse(fd, "goods_sync_report", -1, "无效的商品数据");
        return;
    }

    QList<goods_info_t> goodsData;
    int size = cJSON_GetArraySize(goodsList);
    for(int i = 0; i < size; i++)
    {
        cJSON *item = cJSON_GetArrayItem(goodsList, i);

        goods_info_t goods;
        memset(&goods, 0, sizeof(goods));
        cJSON *goodsIdItem = cJSON_GetObjectItem(item, "goods_id");
        goods.id = (goodsIdItem && cJSON_IsNumber(goodsIdItem)) ? goodsIdItem->valueint : 0;
        cJSON *goodsNameItem = cJSON_GetObjectItem(item, "goods_name");
        if(goodsNameItem && cJSON_IsString(goodsNameItem))
            strncpy(goods.name, goodsNameItem->valuestring, sizeof(goods.name)-1);
        cJSON *priceItem = cJSON_GetObjectItem(item, "price");
        goods.price = (priceItem && cJSON_IsNumber(priceItem)) ? priceItem->valuedouble : 0.0;
        cJSON *unitItem = cJSON_GetObjectItem(item, "unit");
        if(unitItem && cJSON_IsString(unitItem))
            strncpy(goods.unit, unitItem->valuestring, sizeof(goods.unit)-1);
        cJSON *stockItem = cJSON_GetObjectItem(item, "stock_num");
        if(!stockItem) stockItem = cJSON_GetObjectItem(item, "stock");
        goods.stock = (stockItem && cJSON_IsNumber(stockItem)) ? stockItem->valueint : 0;

        goodsData.append(goods);
    }

    /* transaction-protected atomic operation */
    int ret = DatabaseManager::getInstance()->syncClientGoods(clientId, goodsData);
    if(ret == 0)
    {
        sendResponse(fd, "goods_sync_report", 0, "同步成功");
        emit signalAddLog(QString("client [%1] goods sync report, %2 goods").arg(clientId).arg(size));
        emit signalGoodsDataChanged();
    }
    else
    {
        sendResponse(fd, "goods_sync_report", -1, "同步失败，数据库错误");
    }
}

/* ==================== Goods add (isolated by client_id, name as key) ==================== */

void BusinessManager::processGoodsAdd(int fd, cJSON *root)
{
    cJSON *goodsNameItem = cJSON_GetObjectItem(root, "goods_name");
    cJSON *priceItem = cJSON_GetObjectItem(root, "price");
    cJSON *unitItem = cJSON_GetObjectItem(root, "unit");
    cJSON *stockItem = cJSON_GetObjectItem(root, "stock");

    if(!goodsNameItem || !cJSON_IsString(goodsNameItem) ||
       !priceItem || !cJSON_IsNumber(priceItem) ||
       !unitItem || !cJSON_IsString(unitItem) ||
       !stockItem || !cJSON_IsNumber(stockItem))
    {
        sendResponse(fd, "goods_add", -1, "参数错误: 缺少goods_name/price/unit/stock");
        return;
    }

    char *goods_name = goodsNameItem->valuestring;
    double price = priceItem->valuedouble;
    char *unit = unitItem->valuestring;
    int stock = stockItem->valueint;

    QString clientId = getClientId(fd);

    /* 1. does this client already have this goods? */
    int goods_id = DatabaseManager::getInstance()->getGoodsId(clientId, goods_name);

    /* 2. create the record if missing */
    if(goods_id == -1)
    {
        int ret = DatabaseManager::getInstance()->goodsAdd(clientId, goods_name, price, unit, stock);
        if(ret != 0)
        {
            qDebug() << "goods create failed";
            sendResponse(fd, "goods_add", -1, "商品创建失败");
            return;
        }
    }
    else
    {
        /* exists already: update goods + stock */
        int ret = DatabaseManager::getInstance()->goodsUpdateWithStock(clientId, goods_name, price, unit, stock);
        if(ret != 0)
        {
            qDebug() << "goods update failed";
            sendResponse(fd, "goods_add", -1, "商品更新失败");
            return;
        }
    }

    emit signalAddLog(QString("✅ goods added [%1]: %2, price: %3, stock: %4")
                      .arg(clientId).arg(goods_name).arg(price).arg(stock));
    emit signalGoodsDataChanged();
    sendResponse(fd, "goods_add", 0, "商品添加成功");
}

/* ==================== Stock deduct (by client_id) ==================== */

void BusinessManager::processStockDeduct(int fd, cJSON *root)
{
    cJSON *goodsNameItem = cJSON_GetObjectItem(root, "goods_name");
    cJSON *deductNumItem = cJSON_GetObjectItem(root, "deduct_num");

    if(!goodsNameItem || !cJSON_IsString(goodsNameItem) ||
       !deductNumItem || !cJSON_IsNumber(deductNumItem))
    {
        sendResponse(fd, "stock_deduct", -1, "参数错误: 缺少goods_name/deduct_num");
        return;
    }

    char *goods_name = goodsNameItem->valuestring;
    int deduct_num = deductNumItem->valueint;

    QString clientId = getClientId(fd);

    cJSON *clientIdItem = cJSON_GetObjectItem(root, "client_id");
    if(clientIdItem && cJSON_IsString(clientIdItem) && strlen(clientIdItem->valuestring) > 0)
    {
        clientId = clientIdItem->valuestring;
    }

    int ret = DatabaseManager::getInstance()->stockDeduct(clientId, goods_name, deduct_num);
    if(ret != 0)
    {
        qDebug() << "stock deduct failed: insufficient stock or goods not found";
        sendResponse(fd, "stock_deduct", -1, "库存不足或扣减失败");
        return;
    }

    emit signalAddLog(QString("stock deducted [%1]: %2, num=%3")
                      .arg(clientId).arg(goods_name).arg(deduct_num));
    sendResponse(fd, "stock_deduct", 0, "库存扣减成功");
}

/************************* OTA file transfer (encrypted TCP channel) *************************/

void BusinessManager::processOtaFileRequest(int fd, cJSON *root)
{
    cJSON *versionItem = cJSON_GetObjectItem(root, "version");
    cJSON *filenameItem = cJSON_GetObjectItem(root, "filename");

    if (!versionItem || !cJSON_IsString(versionItem) ||
        !filenameItem || !cJSON_IsString(filenameItem)) {
        sendResponse(fd, "ota_file_request", -1, "参数错误: 缺少version/filename");
        return;
    }

    QString version = versionItem->valuestring;
    QString filename = filenameItem->valuestring;

    /* Validate version / filename to prevent path traversal */
    if (!isValidOtaName(version) || !isValidOtaName(filename)) {
        sendResponse(fd, "ota_file_request", -1, "参数错误: 版本号或文件名不合法");
        return;
    }

    QString filePath = QString("%1/ota_packages/%2_%3")
        .arg(QCoreApplication::applicationDirPath())
        .arg(version).arg(filename);

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        sendResponse(fd, "ota_file_request", -1, "文件不存在");
        return;
    }

    qint64 fileSize = file.size();
    static const int CHUNK_SIZE = 32768;
    int totalChunks = (fileSize + CHUNK_SIZE - 1) / CHUNK_SIZE;

    cJSON *ack = cJSON_CreateObject();
    cJSON_AddStringToObject(ack, "cmd", "ota_file_request");
    cJSON_AddNumberToObject(ack, "code", 0);
    cJSON_AddStringToObject(ack, "msg", "准备传输");
    cJSON_AddNumberToObject(ack, "total_chunks", totalChunks);
    cJSON_AddNumberToObject(ack, "file_size", fileSize);

    char *ackStr = cJSON_PrintUnformatted(ack);
    cJSON_Delete(ack);
    if (!ackStr) { file.close(); return; }
    sendToClient(fd, QString::fromUtf8(ackStr));
    free(ackStr);

    int chunkIndex = 0;
    while (!file.atEnd()) {
        QByteArray chunk = file.read(CHUNK_SIZE);
        QByteArray b64 = chunk.toBase64();

        cJSON *chunkMsg = cJSON_CreateObject();
        cJSON_AddStringToObject(chunkMsg, "cmd", "ota_chunk");
        cJSON_AddNumberToObject(chunkMsg, "chunk_index", chunkIndex);
        cJSON_AddNumberToObject(chunkMsg, "total_chunks", totalChunks);
        cJSON_AddStringToObject(chunkMsg, "data", b64.constData());

        char *chunkStr = cJSON_PrintUnformatted(chunkMsg);
        cJSON_Delete(chunkMsg);
        if (chunkStr) {
            sendToClient(fd, QString::fromUtf8(chunkStr));
            free(chunkStr);
        }
        chunkIndex++;

        QCoreApplication::processEvents(); // keep UI responsive
    }
    file.close();

    emit signalAddLog(QString("OTA file transfer done: %1 v%2 (%3 bytes, %4 chunks)")
                      .arg(filename).arg(version).arg(fileSize).arg(totalChunks));
}

void BusinessManager::processOtaCheck(int fd, cJSON *root)
{
    Q_UNUSED(root);

    ota_version_t ver;
    if (otaGetLatestVersion(&ver) != 0) {
        sendResponse(fd, "ota_check", -1, "暂无更新");
        return;
    }

    /* Respond in ota_push format; the client handles it via processOtaPush */
    cJSON *push = cJSON_CreateObject();
    cJSON_AddStringToObject(push, "cmd", "ota_push");
    cJSON_AddStringToObject(push, "version", ver.version);
    cJSON_AddStringToObject(push, "filename", ver.filename);
    cJSON_AddStringToObject(push, "sha256", ver.sha256);
    cJSON_AddNumberToObject(push, "file_size", ver.file_size);
    cJSON_AddStringToObject(push, "description", ver.description);
    cJSON_AddNumberToObject(push, "type", (int)ver.type);
    char *json = cJSON_PrintUnformatted(push);
    cJSON_Delete(push);
    if (!json) return;
    sendToClient(fd, QString(json));
    free(json);
}

/************************* Client ID management *************************/

QString BusinessManager::getClientId(int fd)
{
    QMutexLocker lock(&m_clientMapMutex);
    return m_clientIdMap.value(fd, QString());
}

void BusinessManager::setClientId(int fd, const QString &clientId)
{
    QMutexLocker lock(&m_clientMapMutex);
    m_clientIdMap.insert(fd, clientId);
}

void BusinessManager::removeClient(int fd)
{
    QMutexLocker lock(&m_clientMapMutex);
    m_clientIdMap.remove(fd);
}
