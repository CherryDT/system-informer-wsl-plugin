#include "controller.hpp"
#include "transport.hpp"
#include "process_rules.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace wsl
{
namespace
{

struct Job
{
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
std::wstring activeDistro;
bool activeAutomatic = false;
std::atomic<bool> automaticCapture{true};
bool backgroundCapture = true, captureVisible = false;
std::wstring captureDistro;
std::atomic<bool> discoveryPaused{false};

bool captureAllowed(const std::wstring &distro, const std::string &operation, bool automatic = false)
{
    if (automatic && !automaticCapture.load())
        return false;
    return backgroundCapture || (captureVisible && (operation == "discover" || distro == captureDistro));
}
constexpr auto CapturePaused =
    "Capture is paused. Open this distribution in the WSL tab, or enable background capture in WSL options.";

std::wstring helperPath()
{
    std::wstring path(32768, L'\0');
    const DWORD size = GetModuleFileNameW(instance, path.data(), static_cast<DWORD>(path.size()));
    if (!size || size >= path.size())
        throw std::runtime_error("Could not locate the WSL Tools plugin directory.");
    path.resize(size);
    return path.substr(0, path.find_last_of(L"\\/")) + L"\\wsl-observer";
}

void deliver(const std::shared_ptr<Mailbox> &mailbox, std::unique_ptr<Reply> reply)
{
    if (!mailbox)
        return;
    // Window teardown takes the same lock before clearing its HWND, then drains
    // queued replies. A worker can therefore never post after that final drain.
    std::lock_guard<std::mutex> lock(mailbox->gate);
    const HWND target = mailbox->window.load();
    if (target && PostMessageW(target, ReplyMessage, 0, reinterpret_cast<LPARAM>(reply.get())))
        reply.release();
}

void run()
{
    std::map<std::wstring, std::shared_ptr<Client>> clients;
    for (;;)
    {
        Job job;
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            ready.wait(lock, [] { return stopping || !jobs.empty(); });
            if (stopping)
                break;
            job = std::move(jobs.front());
            jobs.pop_front();
        }
        const auto operation = job.request.value("op", "");
        if (operation == "capture_policy")
        {
            // Also discard clients cancelled by a hide followed immediately by
            // a show: the current policy may already allow capture again.
            clients.clear();
            continue;
        }
        if (operation == "disconnect")
        {
            clients.erase(job.distro);
            continue;
        }
        const bool savedRule = job.request.value("_saved_scheduling", false);
        const bool automatic = savedRule || job.request.value("_automatic_capture", false);
        if (!job.mailbox || !job.mailbox->window.load())
        {
            if (savedRule)
                cancelSavedScheduling(job.distro, job.request);
            continue;
        }
        if (savedRule && !savedSchedulingCurrent(job.distro, job.request))
        {
            cancelSavedScheduling(job.distro, job.request);
            continue;
        }

        auto reply = std::make_unique<Reply>();
        reply->tag = job.tag;
        try
        {
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                if (!captureAllowed(job.distro, operation, automatic))
                    throw std::runtime_error(CapturePaused);
            }
            if (operation == "discover")
            {
                reply->data = Json::array();
                for (const auto &distro : runningDistros([automatic] {
                         return cancellationRequested.load() || discoveryPaused.load() ||
                                (automatic && !automaticCapture.load());
                     }))
                    reply->data.push_back(utf8(distro.name));
            }
            else
            {
                auto &client = clients[job.distro];
                // Installation is the explicit action that may replace a
                // terminal missing-component connection with a fresh one.
                if (operation == "install_component")
                    client.reset();
                if (!client)
                    client = std::make_shared<Client>(job.distro, helperPath());
                {
                    std::lock_guard<std::mutex> lock(queueMutex);
                    if (stopping)
                        break;
                    if (!captureAllowed(job.distro, operation, automatic))
                    {
                        clients.erase(job.distro);
                        throw std::runtime_error(CapturePaused);
                    }
                    activeDistro = job.distro;
                    activeAutomatic = automatic;
                    activeClient = client;
                }
                const auto timeout = operation == "service_details" || operation == "service_action" ||
                                             operation == "stacks" || operation == "script_stacks"
                                         ? std::chrono::seconds(35)
                                         : std::chrono::seconds(20);
                reply->data = operation == "install_component" ? client->installComponent()
                                                               : client->request(job.request, timeout);
            }
        }
        catch (const ComponentMissing &error)
        {
            reply->componentMissing = true;
            reply->error = error.what();
        }
        catch (const std::exception &error)
        {
            reply->error = error.what();
            // A valid remote error leaves the connection usable. A transport
            // failure leaves Client terminal until an explicit disconnect;
            // neither case should silently launch a replacement root helper.
        }
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            activeClient.reset();
            activeDistro.clear();
            activeAutomatic = false;
            if (stopping)
                break;
        }
        if (savedRule)
        {
            bool allowed;
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                allowed = captureAllowed(job.distro, operation, automatic);
            }
            if (!allowed)
                cancelSavedScheduling(job.distro, job.request);
            else
            {
                auto error = reply->error;
                if (error.empty() && !reply->data.value("complete", false))
                    error = reply->data.value("text", std::string("Could not apply all saved settings."));
                finishSavedScheduling(job.distro, job.request, error);
            }
            continue;
        }
        if (operation == "snapshot" && reply->error.empty() && automaticCapture.load())
        {
            if (auto request = nextSavedScheduling(job.distro, reply->data))
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                if (!stopping && captureAllowed(job.distro, "thread", true) &&
                    jobs.size() < MaximumQueuedRequests)
                    jobs.push_back({job.distro, std::move(*request), job.mailbox, 0});
                else
                    cancelSavedScheduling(job.distro, *request);
            }
        }
        deliver(job.mailbox, std::move(reply));
    }
    // Ensure idle clients also observe EOF on shutdown, even if one request
    // was cancelled. Destruction only terminates our own Windows launchers.
    clients.clear();
}

} // namespace

void startController()
{
    // Serialize start/join separately from the queue lock, which the worker
    // needs while exiting. Concurrent start cannot revive a stopping worker.
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex);
    std::lock_guard<std::mutex> lock(queueMutex);
    if (worker.joinable())
        return;
    stopping = false;
    cancellationRequested = false;
    try
    {
        worker = std::thread(run);
    }
    catch (...)
    {
        stopping = true;
        cancellationRequested = true;
        throw;
    }
}

void submit(const std::wstring &distro, Json request, std::shared_ptr<Mailbox> mailbox, uintptr_t tag)
{
    std::string rejection;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (stopping)
            rejection = "WSL Tools is shutting down.";
        else if (!captureAllowed(distro, request.value("op", ""),
                                 request.value("_automatic_capture", false) ||
                                     request.value("_saved_scheduling", false)))
            rejection = CapturePaused;
        else if (jobs.size() >= MaximumQueuedRequests)
            rejection = "Too many WSL requests are waiting. Wait for the current operation, then try again.";
        else
            jobs.push_back({distro, std::move(request), mailbox, tag});
    }
    if (!rejection.empty())
    {
        auto reply = std::make_unique<Reply>();
        reply->tag = tag;
        reply->error = std::move(rejection);
        deliver(mailbox, std::move(reply));
        return;
    }
    ready.notify_one();
}

void setCapturePolicy(bool background, bool visible, const std::wstring &distro, bool automatic)
{
    std::deque<Job> cancelled;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (backgroundCapture == background && captureVisible == visible && captureDistro == distro &&
            automaticCapture.load() == automatic)
            return;
        const bool previouslyBackground = backgroundCapture;
        const bool previouslyAutomatic = automaticCapture.exchange(automatic);
        backgroundCapture = background;
        captureVisible = visible;
        captureDistro = distro;
        discoveryPaused = !background && !visible;
        if (stopping || (background && previouslyBackground && automatic == previouslyAutomatic))
            return;
        for (auto it = jobs.begin(); it != jobs.end();)
        {
            const auto op = it->request.value("op", "");
            if (op == "capture_policy")
                it = jobs.erase(it);
            else if (op != "disconnect" && !captureAllowed(it->distro, op,
                                                           it->request.value("_automatic_capture", false) ||
                                                               it->request.value("_saved_scheduling", false)))
            {
                cancelled.push_back(std::move(*it));
                it = jobs.erase(it);
            }
            else
                ++it;
        }
        if (activeClient && !captureAllowed(activeDistro, "snapshot", activeAutomatic))
            activeClient->close();
        // The worker owns idle clients. Give its cleanup priority over new work.
        jobs.push_front({{}, {{"op", "capture_policy"}}, nullptr, 0});
    }
    for (const auto &job : cancelled)
    {
        if (job.request.value("_saved_scheduling", false))
        {
            cancelSavedScheduling(job.distro, job.request);
            continue;
        }
        auto reply = std::make_unique<Reply>();
        reply->tag = job.tag;
        reply->error = CapturePaused;
        deliver(job.mailbox, std::move(reply));
    }
    ready.notify_one();
}

void disconnect(const std::wstring &distro)
{
    std::deque<Job> cancelled;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (stopping)
            return;
        // Preserve explicit actions, but retire old polls and coalesce control
        // markers. Every cancelled poll still needs a reply so its view can
        // release its pending-request state.
        for (auto it = jobs.begin(); it != jobs.end();)
        {
            const auto operation = it->request.value("op", "");
            if (it->distro == distro &&
                (it->request.value("_saved_scheduling", false) || operation == "snapshot" ||
                 operation == "connections" || operation == "services" || operation == "disconnect"))
            {
                cancelled.push_back(std::move(*it));
                it = jobs.erase(it);
            }
            else
                ++it;
        }
        jobs.push_back({distro, {{"op", "disconnect"}}, nullptr, 0});
    }
    for (const auto &job : cancelled)
    {
        if (job.request.value("_saved_scheduling", false))
            cancelSavedScheduling(distro, job.request);
        else if (job.mailbox)
        {
            auto reply = std::make_unique<Reply>();
            reply->tag = job.tag;
            reply->error = "Capture was interrupted by a refresh or capture-policy change.";
            deliver(job.mailbox, std::move(reply));
        }
    }
    ready.notify_one();
}

void stopController()
{
    std::lock_guard<std::mutex> lifecycle(lifecycleMutex);
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        stopping = true;
        cancellationRequested = true;
        jobs.clear();
        if (activeClient)
            activeClient->close();
    }
    ready.notify_one();
    if (worker.joinable())
        worker.join();
    std::lock_guard<std::mutex> lock(queueMutex);
    activeClient.reset();
}

} // namespace wsl
