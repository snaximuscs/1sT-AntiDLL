#include "webhook_queue.h"
#include <cstdio>

WebhookQueue::~WebhookQueue()
{
    Stop();
}

void WebhookQueue::Start(const std::string& url)
{
    if (url.empty())
        return;
    if (m_running.load())
        Stop();

    m_url = url;
    m_running.store(true);
    m_worker = std::thread(&WebhookQueue::WorkerLoop, this);
}

void WebhookQueue::Stop()
{
    if (!m_running.load())
        return;

    m_running.store(false);
    m_cv.notify_all();
    if (m_worker.joinable())
        m_worker.join();

    // Drain anything still queued so a restart begins clean.
    std::lock_guard<std::mutex> lock(m_mutex);
    std::queue<std::string> empty;
    std::swap(m_queue, empty);
}

void WebhookQueue::Enqueue(const std::string& jsonPayload)
{
    if (!m_running.load() || m_url.empty())
        return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push(jsonPayload);
    }
    m_cv.notify_one();
}

void WebhookQueue::WorkerLoop()
{
    while (true)
    {
        std::string payload;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this]{ return !m_running.load() || !m_queue.empty(); });

            if (!m_running.load() && m_queue.empty())
                return;
            if (m_queue.empty())
                continue;

            payload = std::move(m_queue.front());
            m_queue.pop();
        }

        // Send via the curl CLI. JSON goes through stdin (--data-binary @-) so we never
        // have to shell-escape the payload. Failure is swallowed; the server keeps running.
        std::string cmd = "curl -s -m 10 -X POST -H 'Content-Type: application/json' --data-binary @- '"
            + m_url + "' > /dev/null 2>&1";
        FILE* pipe = popen(cmd.c_str(), "w");
        if (pipe)
        {
            fwrite(payload.c_str(), 1, payload.size(), pipe);
            pclose(pipe);
        }
    }
}
