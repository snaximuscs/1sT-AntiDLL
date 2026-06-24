#pragma once

#include <string>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

struct MySQLConfig
{
    bool        enabled        = false;
    std::string host           = "127.0.0.1";
    int         port           = 3306;
    std::string user           = "root";
    std::string password;
    std::string database       = "antidll";
    std::string serverTable    = "antidll_servers";
    std::string detectionTable = "antidll_detections";
    std::string failName       = "Unknown Server";
};

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

class MySQLManager
{
public:
    MySQLManager() = default;
    ~MySQLManager();

    bool Init(const MySQLConfig& cfg);
    void Stop();
    bool IsConnected() const { return m_connected.load(); }
    bool IsLoaded()    const { return m_lib != nullptr; }

    ServerIdentity GetIdentity() const;
    void RequestIdentityRefresh(const std::string& ip, int port);
    void QueueSQL(const std::string& sql);

private:
    struct Task
    {
        enum Type { SQL_EXEC, IDENTITY_REFRESH };
        Type        type = SQL_EXEC;
        std::string sql;
        std::string refreshIp;
        int         refreshPort = 0;
    };

    void WorkerLoop();
    bool Connect();
    void Disconnect();
    bool ExecSQL(const std::string& sql);
    void DoRefreshIdentity(const std::string& ip, int port);
    std::string Escape(const std::string& s);

    void*  m_lib  = nullptr;
    void*  m_conn = nullptr;

    void*          (*fn_init)(void*)                                                                              = nullptr;
    void*          (*fn_real_connect)(void*, const char*, const char*, const char*, const char*, unsigned int, const char*, unsigned long) = nullptr;
    int            (*fn_query)(void*, const char*)                                                                = nullptr;
    void*          (*fn_store_result)(void*)                                                                      = nullptr;
    char**         (*fn_fetch_row)(void*)                                                                         = nullptr;
    void           (*fn_free_result)(void*)                                                                       = nullptr;
    void           (*fn_close)(void*)                                                                             = nullptr;
    const char*    (*fn_error)(void*)                                                                             = nullptr;
    unsigned long  (*fn_real_escape_string)(void*, char*, const char*, unsigned long)                              = nullptr;
    uint64_t       (*fn_num_rows)(void*)                                                                          = nullptr;

    MySQLConfig    m_cfg;
    ServerIdentity m_identity;
    mutable std::mutex m_identityMutex;

    std::queue<Task>        m_queue;
    std::mutex              m_queueMutex;
    std::condition_variable m_cv;
    std::thread             m_worker;
    std::atomic<bool>       m_running{false};
    std::atomic<bool>       m_connected{false};
};
