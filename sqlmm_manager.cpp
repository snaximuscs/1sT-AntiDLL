#include "sqlmm_manager.h"
#include <sql_mm.h>
#include <mysql_mm.h>
#include <cstdio>

bool SQLMMManager::Init(ISQLInterface* sqlInterface,
                         const std::string& host, int port,
                         const std::string& user, const std::string& pass,
                         const std::string& database)
{
    if (!sqlInterface) return false;
    m_available = true;

    IMySQLClient* client = sqlInterface->GetMySQLClient();
    if (!client) { m_available = false; return false; }

    m_host = host;
    m_user = user;
    m_pass = pass;
    m_database = database;

    MySQLConnectionInfo info;
    info.host     = m_host.c_str();
    info.port     = port;
    info.user     = m_user.c_str();
    info.pass     = m_pass.c_str();
    info.database = m_database.c_str();
    info.timeout  = 30;

    m_conn = client->CreateMySQLConnection(info);
    if (!m_conn) return false;

    m_conn->Connect([this](bool success) {
        m_connected = success;
    });

    return true;
}

void SQLMMManager::Shutdown()
{
    if (m_conn)
    {
        m_conn->Destroy();
        m_conn = nullptr;
    }
    m_available  = false;
    m_connected  = false;
}

void SQLMMManager::SetIdentityQuery(const std::string& ip, int port,
                                     const std::string& tableName,
                                     const std::string& failName)
{
    m_idIp       = ip;
    m_idPort     = port;
    m_idTable    = tableName;
    m_idFailName = failName;
    m_hasIdQuery = true;

    m_identity.ip   = ip;
    m_identity.port = port;
    m_identity.serverName = failName;
}

void SQLMMManager::RefreshIdentity()
{
    if (!m_conn || !m_connected || !m_hasIdQuery) return;

    std::string escapedIp = Escape(m_idIp);
    std::string sql = "SELECT server_name, server_group, region FROM " + m_idTable +
        " WHERE ip = '" + escapedIp + "' AND port = " + std::to_string(m_idPort) +
        " AND enabled = 1 LIMIT 1";

    std::string failName = m_idFailName;
    std::string sIp = m_idIp;
    int sPort = m_idPort;

    m_conn->Query(sql.c_str(), [this, failName, sIp, sPort](ISQLQuery* query) {
        m_identity.ip             = sIp;
        m_identity.port           = sPort;
        m_identity.mysqlConnected = true;

        if (!query)
        {
            m_identity.serverName  = failName;
            m_identity.mysqlMatched = false;
            return;
        }

        ISQLResult* result = query->GetResultSet();
        if (!result || result->GetRowCount() == 0)
        {
            m_identity.serverName  = failName;
            m_identity.mysqlMatched = false;
            return;
        }

        result->FetchRow();
        const char* name   = result->GetString(0);
        const char* group  = result->GetString(1);
        const char* region = result->GetString(2);

        m_identity.serverName   = name   ? name   : failName;
        m_identity.serverGroup  = group  ? group  : "";
        m_identity.region       = region ? region : "";
        m_identity.mysqlMatched = true;
    });
}

void SQLMMManager::ExecuteSQL(const std::string& sql)
{
    if (!m_conn || !m_connected) return;

    m_conn->Query(sql.c_str(), [](ISQLQuery*) {});
}

std::string SQLMMManager::Escape(const std::string& s)
{
    if (m_conn)
        return m_conn->Escape(s.c_str());

    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s)
    {
        if (c == '\'') out += "''";
        else if (c == '\\') out += "\\\\";
        else if (c == '\0') { }
        else out.push_back(c);
    }
    return out;
}
