#include "controller.hpp"
#include "transport.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace wsl {
namespace {

struct Job {
    std::wstring distro;
    Json request;
    std::shared_ptr<Mailbox> mailbox;
    uintptr_t tag = 0;
};

constexpr std::size_t MaximumQueuedRequests = 128;
std::mutex queueMutex;
std::mutex lifecycleMutex;
std::condition_variable ready;
std::deque<Job> jobs;
std::thread worker;
bool stopping = true;
std::atomic<bool> cancellationRequested{true};
// Accessed under queueMutex. Shared ownership keeps cancellation safe while
// the worker finishes a request or removes a disconnected client from its map.
std::shared_ptr<Client> activeClient;

std::wstring helperPath() {
    std::wstring path(32768, L'\0');
    const DWORD size = GetModuleFileNameW(instance, path.data(), static_cast<DWORD>(path.size()));
    if (!size || size >= path.size())
        throw std::runtime_error("Could not locate the WSL Tools plugin directory.");
    path.resize(size);
    return path.substr(0, path.find_last_of(L"\\/")) + L"\\wsl-observer";
}

void deliver(const std::shared_ptr<Mailbox>& mailbox, std::unique_ptr<Reply> reply) {
    if (!mailbox) return;
    // Window teardown takes the same lock before clearing its HWND, then drains
    // queued replies. A worker can therefore never post after that final drain.
    std::lock_guard<std::mutex> lock(mailbox->gate);
    const HWND target = mailbox->window.load();
    if (target && PostMessageW(target, ReplyMessage, 0, reinterpret_cast<LPARAM>(reply.get())))
        reply.release();
}

void run() {
    std::map<std::wstring, std::shared_ptr<Client>> clients;
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            ready.wait(lock, [] { return stopping || !jobs.empty(); });
            if (stopping) break;
            job = std::move(jobs.front());
            jobs.pop_front();
        }
        const auto operation = job.request.value("op", "");
        if (operation == "disconnect") {
            clients.erase(job.distro);
            continue;
        }
        if (!job.mailbox || !job.mailbox->window.load()) continue;

        auto reply = std::make_unique<Reply>();
        reply->tag = job.tag;
        try {
            if (operation == "discover") {
                reply->data = Json::array();
                for (const auto& distro : runningDistros([] { return cancellationRequested.load(); }))
                    reply->data.push_back(utf8(distro.name));
            } else {
                auto& client = clients[job.distro];
                // Installation is the explicit action that may replace a
                // terminal missing-component connection with a fresh one.
                if (operation == "install_component") client.reset();
                if (!client) client = std::make_shared<Client>(job.distro, helperPath());
                {
                    std::lock_guard<std::mutex> lock(queueMutex);
                    if (stopping) break;
                    activeClient = client;
                }
                const auto timeout = operation == "service_details" || operation == "service_action" || operation == "stacks"
                    ? std::chrono::seconds(35) : std::chrono::seconds(20);
                reply->data = operation == "install_component"
                    ? client->installComponent() : client->request(job.request, timeout);
            }
        } catch (const ComponentMissing& error) {
            reply->componentMissing = true;
            reply->error = error.what();
        } catch (const std::exception& error) {
            reply->error = error.what();
            // A valid remote error leaves the connection usable. A transport
            // failure leaves Client terminal until an explicit disconnect;
            // neither case should silently launch a replacement root helper.
        }
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            activeClient.reset();
            if (stopping) break;
        }
        deliver(job.mailbox, std::move(reply));
    }
    // Ensure idle clients also observe EOF on shutdown, even if one request
    // was cancelled. Destruction only terminates our own Windows launchers.
    clients.clear();
}

} // namespace

void startController() {
    // Serialize start/join separately from the queue lock, which the worker
    // needs while exiting. Concurrent start cannot revive a stopping worker.
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex);
    std::lock_guard<std::mutex> lock(queueMutex);
    if (worker.joinable()) return;
    stopping = false;
    cancellationRequested = false;
    try {
        worker = std::thread(run);
    } catch (...) {
        stopping = true;
        cancellationRequested = true;
        throw;
    }
}

void submit(const std::wstring& distro, Json request, std::shared_ptr<Mailbox> mailbox, uintptr_t tag) {
    std::string rejection;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (stopping) rejection = "WSL Tools is shutting down.";
        else if (jobs.size() >= MaximumQueuedRequests)
            rejection = "Too many WSL requests are waiting. Wait for the current operation, then try again.";
        else jobs.push_back({distro, std::move(request), mailbox, tag});
    }
    if (!rejection.empty()) {
        auto reply = std::make_unique<Reply>();
        reply->tag = tag;
        reply->error = std::move(rejection);
        deliver(mailbox, std::move(reply));
        return;
    }
    ready.notify_one();
}

void disconnect(const std::wstring& distro) {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (stopping) return;
        // Preserve previously requested actions, but discard old polling work.
        // Coalesce control markers too: repeated reconnect clicks cannot create
        // an unbounded queue. Control markers have priority over the request cap
        // so a busy queue can always release a connection.
        jobs.erase(std::remove_if(jobs.begin(), jobs.end(), [&](const Job& job) {
            const auto operation = job.request.value("op", "");
            return job.distro == distro && (operation == "snapshot" || operation == "connections" ||
                operation == "services" || operation == "disconnect");
        }), jobs.end());
        jobs.push_back({distro, {{"op", "disconnect"}}, nullptr, 0});
    }
    ready.notify_one();
}

void stopController() {
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex);
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        stopping = true;
        cancellationRequested = true;
        jobs.clear();
        if (activeClient) activeClient->close();
    }
    ready.notify_one();
    if (worker.joinable()) worker.join();
    std::lock_guard<std::mutex> lock(queueMutex);
    activeClient.reset();
}

} // namespace wsl
