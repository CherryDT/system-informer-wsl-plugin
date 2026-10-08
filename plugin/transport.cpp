#include "transport.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace wsl {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t MaxResponseBytes = 16 * 1024 * 1024;
constexpr std::size_t MaxDiagnosticBytes = 32 * 1024;
constexpr std::size_t MaxHelperBytes = 128 * 1024 * 1024;

// A valid error reply leaves the framing intact. A failed action should not
// disconnect the distro or cause a second execution on a fresh connection.
class RemoteError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(other.release()) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    HANDLE get() const { return value_; }
    explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
    HANDLE release() { return std::exchange(value_, nullptr); }
    void reset(HANDLE value = nullptr) {
        if (*this) CloseHandle(value_);
        value_ = value;
    }
private:
    HANDLE value_ = nullptr;
};

std::runtime_error winError(const char* operation, DWORD error = GetLastError()) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring detail = length ? std::wstring(buffer, length) : L"Unknown Windows error";
    if (buffer) LocalFree(buffer);
    while (!detail.empty() && (detail.back() == L'\r' || detail.back() == L'\n')) detail.pop_back();
    return std::runtime_error(std::string(operation) + ": " + utf8(detail) +
        " (" + std::to_string(error) + ")");
}

std::wstring wslExecutable() {
    wchar_t path[MAX_PATH];
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    if (!length || length >= MAX_PATH) throw winError("Find Windows system directory");
    return std::wstring(path, length) + L"\\wsl.exe";
}

struct Pipe {
    Handle parent;
    Handle child;
};

Pipe outputPipe() {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE reader = nullptr, writer = nullptr;
    if (!CreatePipe(&reader, &writer, &security, 64 * 1024)) throw winError("Create output pipe");
    Pipe result{Handle(reader), Handle(writer)};
    if (!SetHandleInformation(reader, HANDLE_FLAG_INHERIT, 0)) throw winError("Protect output pipe");
    return result;
}

Pipe inputPipe() {
    // Anonymous pipes do not support overlapped writes. A private named pipe
    // lets us cancel an upload if WSL fails to consume stdin during startup.
    static std::atomic<unsigned long> serial{0};
    const auto name = L"\\\\.\\pipe\\SystemInformerWsl-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++serial);
    Handle writer(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_OUTBOUND |
        FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 64 * 1024, 64 * 1024, 0, nullptr));
    if (!writer) throw winError("Create input pipe");
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle reader(CreateFileW(name.c_str(), GENERIC_READ, 0, &security,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!reader) throw winError("Open input pipe");
    // The synchronous client has already connected to this server instance.
    OVERLAPPED connection{};
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) throw winError("Create pipe connection event");
    connection.hEvent = event.get();
    if (!ConnectNamedPipe(writer.get(), &connection) && GetLastError() != ERROR_PIPE_CONNECTED)
        throw winError("Connect input pipe");
    return {std::move(writer), std::move(reader)};
}

struct Process {
    Handle process;
    Handle input;
    Handle output;
    Handle errors;
    std::string diagnostics;
    std::string pending;

    ~Process() {
        // Closing stdin lets a healthy helper exit and an unfinished installer
        // remove its temporary upload; the installed component is retained.
        input.reset();
        if (process && WaitForSingleObject(process.get(), 100) == WAIT_TIMEOUT) {
            // Only our wsl.exe launcher is terminated. Never use --terminate or
            // --shutdown: other sessions and the distro must remain untouched.
            TerminateProcess(process.get(), ERROR_CANCELLED);
            // TerminateProcess is asynchronous. Releasing our handle does not
            // require waiting for WSL or its service to acknowledge the exit.
        }
    }

    void drainErrors() {
        if (!errors) return;
        char buffer[4096];
        // Bound each drain so a noisy child cannot starve deadline checks.
        std::size_t drained = 0;
        while (drained < 64 * 1024) {
            DWORD available = 0;
            if (!PeekNamedPipe(errors.get(), nullptr, 0, nullptr, &available, nullptr) || !available) break;
            DWORD count = 0;
            if (!ReadFile(errors.get(), buffer, std::min<DWORD>(available, sizeof(buffer)), &count, nullptr) || !count) break;
            drained += count;
            if (diagnostics.size() < MaxDiagnosticBytes)
                diagnostics.append(buffer, std::min<std::size_t>(count, MaxDiagnosticBytes - diagnostics.size()));
        }
    }

    std::runtime_error failure(const std::string& message) {
        drainErrors();
        return std::runtime_error(message + (diagnostics.empty() ? "" : "\n" + diagnostics));
    }

    void check(HANDLE cancellation, Clock::time_point deadline) {
        drainErrors();
        if (cancellation && WaitForSingleObject(cancellation, 0) == WAIT_OBJECT_0)
            throw std::runtime_error("WSL operation cancelled.");
        if (Clock::now() >= deadline) throw failure("WSL operation timed out. Reconnect the distribution to retry.");
    }

    void write(const char* bytes, std::size_t length, HANDLE cancellation, Clock::time_point deadline) {
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event) throw winError("Create write event");
        while (length) {
            check(cancellation, deadline);
            OVERLAPPED operation{};
            operation.hEvent = event.get();
            ResetEvent(event.get());
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(length, 64 * 1024));
            DWORD count = 0;
            if (!WriteFile(input.get(), bytes, chunk, &count, &operation)) {
                if (GetLastError() != ERROR_IO_PENDING) throw failure("Could not write to the WSL helper.");
                try {
                    while (WaitForSingleObject(event.get(), 20) == WAIT_TIMEOUT) check(cancellation, deadline);
                } catch (...) {
                    // OVERLAPPED and its buffer must stay alive until the kernel
                    // acknowledges cancellation, even when the deadline expires.
                    CancelIoEx(input.get(), &operation);
                    GetOverlappedResult(input.get(), &operation, &count, TRUE);
                    throw;
                }
                if (!GetOverlappedResult(input.get(), &operation, &count, FALSE))
                    throw failure("The WSL helper closed its input pipe.");
            }
            if (!count) throw failure("The WSL helper stopped accepting requests.");
            bytes += count;
            length -= count;
        }
    }

    std::string readLine(HANDLE cancellation, Clock::time_point deadline) {
        for (;;) {
            const auto newline = pending.find('\n');
            if (newline != std::string::npos) {
                std::string line = pending.substr(0, newline);
                pending.erase(0, newline + 1);
                return line;
            }
            check(cancellation, deadline);
            DWORD available = 0;
            if (!PeekNamedPipe(output.get(), nullptr, 0, nullptr, &available, nullptr))
                throw failure("The WSL helper disconnected.");
            if (available) {
                char buffer[16 * 1024];
                DWORD count = 0;
                if (!ReadFile(output.get(), buffer, std::min<DWORD>(available, sizeof(buffer)), &count, nullptr) || !count)
                    throw failure("The WSL helper disconnected.");
                pending.append(buffer, count);
                if (pending.size() > MaxResponseBytes) throw failure("WSL response exceeds the 16 MiB safety limit.");
                continue;
            }
            if (WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0)
                throw failure("The WSL helper exited. Reconnect the distribution to retry.");
            if (cancellation) WaitForSingleObject(cancellation, 10);
            else Sleep(10);
        }
    }
};

std::unique_ptr<Process> launch(const std::vector<std::wstring>& arguments) {
    auto input = inputPipe();
    auto output = outputPipe();
    auto errors = outputPipe();
    const std::wstring executable = wslExecutable();
    std::wstring command = quoteArg(executable);
    for (const auto& argument : arguments) command += L" " + quoteArg(argument);

    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<unsigned char> storage(attributeBytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes))
        throw winError("Initialize launcher handle list");
    struct AttributeGuard {
        LPPROC_THREAD_ATTRIBUTE_LIST value;
        ~AttributeGuard() { DeleteProcThreadAttributeList(value); }
    } guard{attributes};
    HANDLE inherited[]{input.child.get(), output.child.get(), errors.child.get()};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        inherited, sizeof(inherited), nullptr, nullptr)) throw winError("Set launcher handle list");

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.child.get();
    startup.StartupInfo.hStdOutput = output.child.get();
    startup.StartupInfo.hStdError = errors.child.get();
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION information{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
        &startup.StartupInfo, &information)) throw winError("Start wsl.exe");
    Handle thread(information.hThread);
    auto result = std::make_unique<Process>();
    result->process.reset(information.hProcess);
    result->input = std::move(input.parent);
    result->output = std::move(output.parent);
    result->errors = std::move(errors.parent);
    return result;
}

std::string runDiscovery(const std::function<bool()>& cancelled) {
    auto process = launch({L"--list", L"--running", L"--quiet"});
    process->input.reset();
    std::string result;
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    for (;;) {
        if (cancelled && cancelled()) throw std::runtime_error("WSL discovery cancelled.");
        process->check(nullptr, deadline);
        DWORD available = 0;
        const BOOL open = PeekNamedPipe(process->output.get(), nullptr, 0, nullptr, &available, nullptr);
        if (open && available) {
            char buffer[4096];
            DWORD count = 0;
            if (!ReadFile(process->output.get(), buffer, std::min<DWORD>(available, sizeof(buffer)), &count, nullptr))
                throw process->failure("Could not read the WSL distribution list.");
            result.append(buffer, count);
            if (result.size() > 1024 * 1024) throw process->failure("Unexpectedly large WSL distribution list.");
            continue;
        }
        if (WaitForSingleObject(process->process.get(), 0) == WAIT_OBJECT_0) break;
        Sleep(10);
    }
    DWORD status = 0;
    if (!GetExitCodeProcess(process->process.get(), &status)) throw winError("Read WSL exit status");
    if (status) throw process->failure("Could not enumerate WSL distributions (exit " + std::to_string(status) + ").");
    return result;
}

std::wstring decodeList(const std::string& bytes) {
    if (bytes.empty()) return {};
    // wsl.exe usually emits UTF-16LE to redirected output. Accept UTF-8 too,
    // since this behavior has varied between Windows and Store WSL releases.
    const bool utf16 = bytes.find('\0') != std::string::npos ||
        (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xff &&
            static_cast<unsigned char>(bytes[1]) == 0xfe);
    if (!utf16) return wide(bytes);
    if (bytes.size() % 2) throw std::runtime_error("WSL returned an incomplete UTF-16 distribution list.");
    std::wstring result;
    result.reserve(bytes.size() / 2);
    for (std::size_t i = 0; i < bytes.size(); i += 2) {
        const wchar_t character = static_cast<wchar_t>(static_cast<unsigned char>(bytes[i]) |
            (static_cast<unsigned char>(bytes[i + 1]) << 8));
        if (character && character != 0xfeff) result.push_back(character);
    }
    return result;
}

std::vector<std::wstring> enumerateRegisteredWsl2Distros() {
    HKEY root = nullptr;
    const LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss", 0, KEY_READ, &root);
    if (status == ERROR_FILE_NOT_FOUND) return {};
    if (status != ERROR_SUCCESS) throw winError("Read WSL registration", status);
    struct KeyGuard { HKEY value; ~KeyGuard() { RegCloseKey(value); } } rootGuard{root};
    std::vector<std::wstring> result;
    for (DWORD index = 0;; ++index) {
        wchar_t keyName[256];
        DWORD keyLength = 256;
        const LSTATUS next = RegEnumKeyExW(root, index, keyName, &keyLength, nullptr, nullptr, nullptr, nullptr);
        if (next == ERROR_NO_MORE_ITEMS) break;
        if (next != ERROR_SUCCESS) throw winError("Enumerate WSL registration", next);
        // Version describes the registration/filesystem schema, not WSL1 vs
        // WSL2. Microsoft's LXSS_DISTRO_FLAGS_VM_MODE selects the WSL2 VM.
        DWORD flags = 0, flagsBytes = sizeof(flags);
        if (RegGetValueW(root, keyName, L"Flags", RRF_RT_REG_DWORD, nullptr, &flags, &flagsBytes) != ERROR_SUCCESS || !(flags & 0x8))
            continue;
        DWORD nameBytes = 0;
        if (RegGetValueW(root, keyName, L"DistributionName", RRF_RT_REG_SZ, nullptr, nullptr, &nameBytes) != ERROR_SUCCESS)
            continue;
        std::vector<wchar_t> name(nameBytes / sizeof(wchar_t) + 1, L'\0');
        if (RegGetValueW(root, keyName, L"DistributionName", RRF_RT_REG_SZ, nullptr, name.data(), &nameBytes) == ERROR_SUCCESS)
            result.emplace_back(name.data());
    }
    return result;
}

std::string readHelper(const std::wstring& path) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) throw winError("Open wsl-observer helper beside the plugin");
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size)) throw winError("Read helper size");
    if (size.QuadPart <= 0 || static_cast<unsigned long long>(size.QuadPart) > MaxHelperBytes)
        throw std::runtime_error("The WSL helper is empty or exceeds 128 MiB. Reinstall the plugin package.");
    std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD count = 0;
    if (!ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) || count != bytes.size())
        throw winError("Read WSL helper");
    return bytes;
}

std::string sha256(const std::string& bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Windows could not initialize SHA256 for the WSL component.");
    struct AlgorithmGuard {
        BCRYPT_ALG_HANDLE handle;
        ~AlgorithmGuard() { BCryptCloseAlgorithmProvider(handle, 0); }
    } guard{algorithm};
    unsigned char digest[32];
    if (BCryptHash(algorithm, nullptr, 0,
        reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())), static_cast<ULONG>(bytes.size()),
        digest, sizeof(digest)) < 0)
        throw std::runtime_error("Windows could not hash the packaged WSL component.");
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const auto byte : digest) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 15]);
    }
    return result;
}

std::string componentBootstrap(std::size_t size, const std::string& digest, bool allowInstall) {
    // Only a decimal length, our computed hexadecimal SHA256 and a boolean are
    // inserted into this script. Distro names and user input never enter it.
    // Every parent is checked before trusting the persistent executable; root
    // ownership alone is insufficient if another account can replace a parent.
    return "expected=" + digest + "; count=" + std::to_string(size) +
        "; allow_install=" + (allowInstall ? "1" : "0") + R"SH(
set -eu
umask 077
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH
dir=/usr/local/lib/system-informer-wsl
target=$dir/wsl-observer
fail() { printf '%s\n' "$1" >&2; exit 1; }
missing() { printf '{"bootstrap":"missing"}\n'; exit 0; }
safe_owner_mode() {
    [ "$(stat -c %u -- "$1")" = 0 ] || fail "WSL component path is not owned by root: $1"
    mode=$(stat -c %a -- "$1")
    [ "$((0$mode & 022))" -eq 0 ] || fail "WSL component path is writable by another account: $1"
}
[ "$(id -u)" = 0 ] || fail 'The WSL component must run as root.'
for parent in / /usr /usr/local /usr/local/lib "$dir"; do
    [ ! -L "$parent" ] || fail "WSL component parent must not be a symlink: $parent"
    if [ ! -e "$parent" ]; then
        [ "$allow_install" = 1 ] || missing
        mkdir -m 755 -- "$parent"
    fi
    [ -d "$parent" ] || fail "WSL component parent is not a directory: $parent"
    safe_owner_mode "$parent"
done
[ ! -L "$target" ] || fail 'The installed WSL component must not be a symlink.'
if [ -e "$target" ]; then
    [ -f "$target" ] || fail 'The installed WSL component is not a regular file.'
    safe_owner_mode "$target"
    [ "$(stat -c %h -- "$target")" = 1 ] || fail 'The installed WSL component must not have hard links.'
    actual=$(sha256sum -- "$target")
    actual=${actual%% *}
else
    [ "$allow_install" = 1 ] || missing
    actual=
fi
if [ "$actual" != "$expected" ]; then
    temporary=$(mktemp "$dir/.observer.XXXXXX")
    trap 'rm -f -- "$temporary"' EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM
    printf '{"bootstrap":"upload"}\n'
    head -c "$count" > "$temporary"
    [ "$(wc -c < "$temporary")" -eq "$count" ] || fail 'The WSL component upload was incomplete.'
    actual=$(sha256sum -- "$temporary")
    actual=${actual%% *}
    [ "$actual" = "$expected" ] || fail 'The WSL component upload failed SHA256 verification.'
    chown 0:0 -- "$temporary"
    chmod 700 -- "$temporary"
    mv -fT -- "$temporary" "$target"
    trap - EXIT HUP INT TERM
fi
[ -x "$target" ] || fail 'The installed WSL component is not executable.'
printf '{"bootstrap":"ready"}\n'
exec "$target"
)SH";
}

} // namespace

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!length) throw std::runtime_error("Invalid UTF-16 text.");
    std::string result(length, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    // Diagnostics from a failed wsl.exe need not be valid UTF-8. Replace bad
    // sequences for display instead of throwing from a window error handler.
    const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (!length) throw std::runtime_error("Invalid UTF-8 text.");
    std::wstring result(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

std::wstring quoteArg(const std::wstring& value) {
    // wsl.exe recognizes option switches from its raw command line. Quoting a
    // switch such as "--list" makes some releases interpret it as a Linux
    // command, so leave arguments without special characters unquoted.
    if (!value.empty() && value.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return value;
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const auto character : value) {
        if (character == L'\\') { ++slashes; continue; }
        result.append(character == L'\"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result.push_back(character);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

std::vector<std::wstring> registeredWsl2Distros() {
    return enumerateRegisteredWsl2Distros();
}

std::vector<Distro> runningDistros(const std::function<bool()>& cancelled) {
    if (cancelled && cancelled()) throw std::runtime_error("WSL discovery cancelled.");
    const auto registered = registeredWsl2Distros();
    const auto list = decodeList(runDiscovery(cancelled));
    std::vector<Distro> result;
    std::size_t start = 0;
    while (start < list.size()) {
        const auto end = list.find(L'\n', start);
        auto name = list.substr(start, end == std::wstring::npos ? end : end - start);
        if (!name.empty() && name.back() == L'\r') name.pop_back();
        if (!name.empty() && name.front() == 0xfeff) name.erase(name.begin());
        if (!name.empty() && std::any_of(registered.begin(), registered.end(), [&](const auto& candidate) {
            return CompareStringOrdinal(candidate.c_str(), -1, name.c_str(), -1, TRUE) == CSTR_EQUAL;
        })) result.push_back({std::move(name)});
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return result;
}

struct Client::Impl {
    std::wstring distro;
    std::wstring helperPath;
    Handle cancellation{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::unique_ptr<Process> process;
    std::uint64_t nextId = 1;
    bool failed = false;
    bool componentMissing = false;

    Impl(std::wstring name, std::wstring path) : distro(std::move(name)), helperPath(std::move(path)) {
        if (!cancellation) throw winError("Create cancellation event");
    }

    Json exchange(const Json& payload, Clock::time_point deadline) {
        Json request = payload;
        const auto id = nextId++;
        request["id"] = id;
        const auto line = request.dump() + "\n";
        process->write(line.data(), line.size(), cancellation.get(), deadline);
        const auto response = Json::parse(process->readLine(cancellation.get(), deadline));
        if (!response.is_object() || response.value("id", std::uint64_t{0}) != id)
            throw std::runtime_error("WSL helper returned an unexpected response ID.");
        if (!response.value("ok", false)) {
            const auto error = response.find("error");
            throw RemoteError(error != response.end() && error->is_string()
                ? error->get<std::string>() : "The WSL helper could not complete this request.");
        }
        return response.at("data");
    }

    Json connect(bool allowInstall = false) {
        if (WaitForSingleObject(cancellation.get(), 0) == WAIT_OBJECT_0)
            throw std::runtime_error("WSL connection cancelled.");
        // Verify immediately before launch. WSL has no public atomic 'attach only'
        // launch; a distribution stopping between this check and launch is a
        // documented platform race. Failed clients are never restarted here.
        const auto running = runningDistros([this] {
            return WaitForSingleObject(cancellation.get(), 0) == WAIT_OBJECT_0;
        });
        if (WaitForSingleObject(cancellation.get(), 0) == WAIT_OBJECT_0)
            throw std::runtime_error("WSL connection cancelled.");
        if (std::none_of(running.begin(), running.end(), [&](const Distro& item) {
            return CompareStringOrdinal(item.name.c_str(), -1, distro.c_str(), -1, TRUE) == CSTR_EQUAL;
        })) throw std::runtime_error("This WSL2 distribution is no longer running.");
        const auto helper = readHelper(helperPath);
        const auto digest = sha256(helper);
        const auto deadline = Clock::now() + std::chrono::seconds(30);
        const auto script = componentBootstrap(helper.size(), digest, allowInstall);
        if (WaitForSingleObject(cancellation.get(), 0) == WAIT_OBJECT_0)
            throw std::runtime_error("WSL connection cancelled.");
        process = launch({L"--distribution", distro, L"--user", L"root", L"--exec", L"/bin/sh", L"-c", wide(script)});
        auto bootstrap = Json::parse(process->readLine(cancellation.get(), deadline));
        auto state = bootstrap.value("bootstrap", "");
        if (state == "missing") throw ComponentMissing();
        if (state == "upload") {
            process->write(helper.data(), helper.size(), cancellation.get(), deadline);
            bootstrap = Json::parse(process->readLine(cancellation.get(), deadline));
            state = bootstrap.value("bootstrap", "");
        }
        if (state != "ready") throw std::runtime_error("Unexpected WSL component installation response.");
        const auto hello = exchange(Json{{"op", "hello"}}, deadline);
        if (hello.value("protocol", 0) != 1)
            throw std::runtime_error("Unsupported WSL helper protocol. Reinstall matching plugin and helper files.");
        return hello;
    }
};

Client::Client(std::wstring distro, std::wstring helperPath)
    : impl_(std::make_unique<Impl>(std::move(distro), std::move(helperPath))) {}

Client::~Client() { close(); }

void Client::close() noexcept { SetEvent(impl_->cancellation.get()); }

Json Client::installComponent() {
    if (impl_->failed) throw std::runtime_error("Create a fresh connection before installing the WSL component.");
    try {
        if (impl_->process) return impl_->exchange(Json{{"op", "hello"}}, Clock::now() + std::chrono::seconds(20));
        return impl_->connect(true);
    } catch (...) {
        impl_->failed = true;
        impl_->process.reset();
        throw;
    }
}

Json Client::request(const Json& payload, std::chrono::milliseconds timeout) {
    if (impl_->componentMissing) throw ComponentMissing();
    if (impl_->failed) throw std::runtime_error("The WSL connection is closed. Reconnect to try again.");
    if (!payload.is_object() || !payload.contains("op") || !payload["op"].is_string())
        throw std::invalid_argument("A WSL request must contain a string op field.");
    if (!impl_->process) {
        try {
            impl_->connect();
        } catch (const ComponentMissing&) {
            impl_->componentMissing = true;
            impl_->failed = true;
            impl_->process.reset();
            throw;
        } catch (...) {
            impl_->failed = true;
            impl_->process.reset();
            throw;
        }
    }
    try {
        return impl_->exchange(payload, Clock::now() + timeout);
    } catch (const RemoteError&) {
        throw;
    } catch (...) {
        // Do not replay a failed action: the guest may have completed it before
        // the connection failed, and silently starting a stopped distro is wrong.
        impl_->failed = true;
        impl_->process.reset();
        throw;
    }
}

} // namespace wsl
