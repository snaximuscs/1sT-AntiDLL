#pragma once

// DiscordWebhookQueue
// Async, non-blocking Discord webhook delivery. HTTP never runs on the game thread:
// messages are queued and a single worker thread sends them via the `curl` CLI.
// Safe shutdown joins the worker on plugin unload. A failed send is logged, never fatal.

#include <string>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

class WebhookQueue
{
public:
    WebhookQueue() = default;
    ~WebhookQueue();

    // Start the worker thread for the given webhook URL. No-op if url is empty.
    void Start(const std::string& url);

    // Stop and join the worker thread. Safe to call multiple times.
    void Stop();

    // Queue a raw Discord JSON payload for delivery. Returns immediately.
    void Enqueue(const std::string& jsonPayload);

    bool IsRunning() const { return m_running.load(); }

private:
    void WorkerLoop();

    std::string             m_url;
    std::queue<std::string> m_queue;
    std::mutex              m_mutex;
    std::condition_variable m_cv;
    std::thread             m_worker;
    std::atomic<bool>       m_running{false};
};
