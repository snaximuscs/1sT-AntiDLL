#pragma once

#include <string>
#include <functional>

#define SQLMM_INTERFACE "SQLInterface"

class ISQLInterface;
class IMySQLConnection;

struct ServerIdentity
{
    std::string serverName  = "Unknown Server";
    std::string serverGroup;
    std::string region;
    std::string ip          = "0.0.0.0";
    int         port        = 27015;
    bool        mysqlMatched   = false;
    bool        mysqlConnected = false;
};

class SQLMMManager
{
public:
    bool Init(ISQLInterface* sqlInterface,
              const std::string& host, int port,
              const std::string& user, const std::string& pass,
              const std::string& database);
    void Shutdown();

    bool IsAvailable()  const { return m_available; }
    bool IsConnected()  const { return m_connected; }
    bool IsIdentityResolved() const { return m_identity.mysqlMatched || m_identityFailed; }

    ServerIdentity GetIdentity() const { return m_identity; }

    void SetIdentityQuery(const std::string& ip, int port,
                          const std::string& tableName,
                          const std::string& failName);

    void RefreshIdentity();

    void ExecuteSQL(const std::string& sql);
    std::string Escape(const std::string& s);

private:
    IMySQLConnection* m_conn = nullptr;
    bool m_available  = false;
    bool m_connected  = false;

    ServerIdentity m_identity;

    std::string m_host, m_user, m_pass, m_database;

    std::string m_idIp;
    int         m_idPort = 0;
    std::string m_idTable;
    std::string m_idFailName;
    bool        m_hasIdQuery = false;
    bool        m_identityFailed = false;
};
