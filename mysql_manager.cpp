#include "mysql_manager.h"
#include <dlfcn.h>
#include <cstring>
#include <cstdio>
#include <vector>

MySQLManager::~MySQLManager() { Stop(); }

bool MySQLManager::Init(const MySQLConfig& cfg)
{
    m_cfg = cfg;
    if (!cfg.enabled) return false;

    m_lib = dlopen("libmysqlclient.so", RTLD_NOW);
    if (!m_lib) m_lib = dlopen("libmysqlclient.so.21", RTLD_NOW);
    if (!m_lib) m_lib = dlopen("libmariadb.so", RTLD_NOW);
    if (!m_lib) m_lib = dlopen("libmariadb.so.3", RTLD_NOW);
    if (!m_lib)
    {
        fprintf(stderr, "[1sT-AntiDLL] MySQL: could not load libmysqlclient or libmariadb\n");
        return false;
    }

#define LOAD_SYM(field, name) \
    field = reinterpret_cast<decltype(field)>(dlsym(m_lib, name)); \
    if (!field) { fprintf(stderr, "[1sT-AntiDLL] MySQL: missing symbol %s\n", name); dlclose(m_lib); m_lib = nullptr; return false; }

    LOAD_SYM(fn_init,               "mysql_init");
    LOAD_SYM(fn_real_connect,       "mysql_real_connect");
    LOAD_SYM(fn_query,              "mysql_query");
    LOAD_SYM(fn_store_result,       "mysql_store_result");
    LOAD_SYM(fn_fetch_row,          "mysql_fetch_row");
    LOAD_SYM(fn_free_result,        "mysql_free_result");
    LOAD_SYM(fn_close,              "mysql_close");
    LOAD_SYM(fn_error,              "mysql_error");
    LOAD_SYM(fn_real_escape_string, "mysql_real_escape_string");
    LOAD_SYM(fn_num_rows,           "mysql_num_rows");
#undef LOAD_SYM

    m_running.store(true);
    m_worker = std::thread(&MySQLManager::WorkerLoop, this);
    return true;
}

void MySQLManager::Stop()
{
    if (!m_running.load()) return;
    m_running.store(false);
    m_cv.notify_all();
    if (m_worker.joinable()) m_worker.join();
    Disconnect();
    if (m_lib) { dlclose(m_lib); m_lib = nullptr; }
}

bool MySQLManager::Connect()
{
    if (m_connected.load()) return true;
    if (!fn_init) return false;

    m_conn = fn_init(nullptr);
    if (!m_conn) return false;

    void* ok = fn_real_connect(m_conn, m_cfg.host.c_str(), m_cfg.user.c_str(),
        m_cfg.password.c_str(), m_cfg.database.c_str(),
        static_cast<unsigned int>(m_cfg.port), nullptr, 0);
    if (!ok)
    {
        fprintf(stderr, "[1sT-AntiDLL] MySQL connect failed: %s\n", fn_error(m_conn));
        fn_close(m_conn);
        m_conn = nullptr;
        return false;
    }

    fn_query(m_conn, "SET NAMES utf8mb4");
    m_connected.store(true);
    return true;
}

void MySQLManager::Disconnect()
{
    if (m_conn && fn_close)
    {
        fn_close(m_conn);
        m_conn = nullptr;
    }
    m_connected.store(false);
}

bool MySQLManager::ExecSQL(const std::string& sql)
{
    if (!m_connected.load() && !Connect()) return false;

    int ret = fn_query(m_conn, sql.c_str());
    if (ret != 0)
    {
        fprintf(stderr, "[1sT-AntiDLL] MySQL query error: %s\n", fn_error(m_conn));
        Disconnect();
        if (!Connect()) return false;
        ret = fn_query(m_conn, sql.c_str());
        if (ret != 0) return false;
    }
    return true;
}

std::string MySQLManager::Escape(const std::string& s)
{
    if (!m_conn || !fn_real_escape_string) return s;
    std::vector<char> buf(s.size() * 2 + 1);
    unsigned long len = fn_real_escape_string(m_conn, buf.data(), s.c_str(),
        static_cast<unsigned long>(s.size()));
    return std::string(buf.data(), len);
}

ServerIdentity MySQLManager::GetIdentity() const
{
    std::lock_guard<std::mutex> lock(m_identityMutex);
    return m_identity;
}

void MySQLManager::RequestIdentityRefresh(const std::string& ip, int port)
{
    Task t;
    t.type      = Task::IDENTITY_REFRESH;
    t.refreshIp = ip;
    t.refreshPort = port;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queue.push(std::move(t));
    }
    m_cv.notify_one();
}

void MySQLManager::QueueSQL(const std::string& sql)
{
    if (!m_running.load()) return;
    Task t;
    t.type = Task::SQL_EXEC;
    t.sql  = sql;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queue.push(std::move(t));
    }
    m_cv.notify_one();
}

void MySQLManager::DoRefreshIdentity(const std::string& ip, int port)
{
    bool connected = m_connected.load() || Connect();

    if (!connected)
    {
        std::lock_guard<std::mutex> lock(m_identityMutex);
        m_identity.serverName    = m_cfg.failName;
        m_identity.ip            = ip;
        m_identity.port          = port;
        m_identity.mysqlMatched  = false;
        m_identity.mysqlConnected = false;
        return;
    }

    std::string sql = "SELECT server_name, server_group, region FROM " + m_cfg.serverTable +
        " WHERE ip = '" + Escape(ip) + "' AND port = " + std::to_string(port) +
        " AND enabled = 1 LIMIT 1";

    if (fn_query(m_conn, sql.c_str()) != 0)
    {
        fprintf(stderr, "[1sT-AntiDLL] MySQL identity query error: %s\n", fn_error(m_conn));
        std::lock_guard<std::mutex> lock(m_identityMutex);
        m_identity.serverName    = m_cfg.failName;
        m_identity.ip            = ip;
        m_identity.port          = port;
        m_identity.mysqlMatched  = false;
        m_identity.mysqlConnected = true;
        return;
    }

    void* res = fn_store_result(m_conn);
    if (!res)
    {
        std::lock_guard<std::mutex> lock(m_identityMutex);
        m_identity.serverName    = m_cfg.failName;
        m_identity.ip            = ip;
        m_identity.port          = port;
        m_identity.mysqlMatched  = false;
        m_identity.mysqlConnected = true;
        return;
    }

    char** row = fn_fetch_row(res);
    {
        std::lock_guard<std::mutex> lock(m_identityMutex);
        m_identity.ip              = ip;
        m_identity.port            = port;
        m_identity.mysqlConnected  = true;
        if (row)
        {
            m_identity.serverName  = row[0] ? row[0] : m_cfg.failName;
            m_identity.serverGroup = row[1] ? row[1] : "";
            m_identity.region      = row[2] ? row[2] : "";
            m_identity.mysqlMatched = true;
        }
        else
        {
            m_identity.serverName  = m_cfg.failName;
            m_identity.serverGroup.clear();
            m_identity.region.clear();
            m_identity.mysqlMatched = false;
        }
    }
    fn_free_result(res);
}

void MySQLManager::WorkerLoop()
{
    Connect();

    while (true)
    {
        Task task;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_cv.wait(lock, [this] { return !m_running.load() || !m_queue.empty(); });
            if (!m_running.load() && m_queue.empty()) return;
            if (m_queue.empty()) continue;
            task = std::move(m_queue.front());
            m_queue.pop();
        }

        switch (task.type)
        {
        case Task::SQL_EXEC:
            ExecSQL(task.sql);
            break;
        case Task::IDENTITY_REFRESH:
            DoRefreshIdentity(task.refreshIp, task.refreshPort);
            break;
        }
    }
}
