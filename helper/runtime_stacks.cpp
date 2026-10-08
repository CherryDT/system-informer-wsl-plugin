#include "observer.hpp"
#include "node_inspector_script.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <set>
#include <stdexcept>
#include <unistd.h>

namespace observer {
namespace {
constexpr int CaptureTimeoutMs = 15000;
constexpr size_t CaptureOutputLimit = 512 * 1024;
constexpr const char* JsSuccess = "__WSL_JS_CAPTURE_OK__";

std::string proc_file(int pid, const char* leaf) {
    return "/proc/" + std::to_string(pid) + "/" + leaf;
}

Json unavailable(const std::string& runtime, const std::string& message) {
    return {{"runtime", runtime}, {"supported", false}, {"success", false},
        {"text", ""}, {"message", message}};
}

CommandCredentials effective_credentials(int pid) {
    const auto status = read_text(proc_file(pid, "status"), 64 * 1024);
    auto effective = [&](const std::string& key) {
        std::istringstream lines(status);
        std::string line;
        while (std::getline(lines, line)) {
            if (line.compare(0, key.size(), key) != 0) continue;
            uint64_t real = 0, value = 0;
            std::istringstream values(line.substr(key.size()));
            if (values >> real >> value && value < std::numeric_limits<uint32_t>::max())
                return static_cast<uint32_t>(value);
        }
        throw std::runtime_error("Cannot read the JVM's effective user and group IDs");
    };
    return {effective("Uid:"), effective("Gid:")};
}

std::string llnode_plugin() {
    for (const auto* path : {"/usr/local/lib/llnode/llnode.so", "/usr/lib/lldb/plugins/llnode.so",
        "/usr/local/lib/node_modules/llnode/llnode.so", "/usr/lib/node_modules/llnode/llnode.so"}) {
        const auto trusted = trusted_command_path(path);
        if (!trusted.empty()) return trusted;
    }
    return {};
}

std::string lldb_quote(const std::string& value) {
    std::string result = "\"";
    for (const char c : value) {
        if (static_cast<unsigned char>(c) < 32) throw std::runtime_error("Invalid LLDB plugin path");
        if (c == '\\' || c == '"') result += '\\';
        result += c;
    }
    return result + '"';
}

std::vector<std::string> javascript_command(int pid, const std::string& plugin) {
    // This program is fixed text, not target/user-supplied Python. SBCommand's
    // result captures per-thread failures without aborting the finally/detach.
    // Keep frame and thread limits separate from the transport output limit.
    const std::string script = R"PY(import lldb
p = lldb.debugger.GetSelectedTarget().GetProcess()
ok = True
try:
    count = p.GetNumThreads()
    if not count:
        ok = False
    for i in range(min(count, 256)):
        t = p.GetThreadAtIndex(i)
        p.SetSelectedThread(t)
        print('\nThread %d (TID %d)' % (i + 1, t.GetThreadID()))
        result = lldb.SBCommandReturnObject()
        frames = min(64, t.GetNumFrames())
        if not frames:
            print('[No unwindable frames]')
            continue
        lldb.debugger.GetCommandInterpreter().HandleCommand('v8 bt %d' % frames, result)
        output = result.GetOutput() or ''
        error = result.GetError() or ''
        print(output)
        if error:
            print(error)
        ok = ok and result.Succeeded()
    if count > 256:
        print('[Thread limit reached: only the first 256 threads were captured]')
        ok = False
finally:
    detached = p.Detach()
    if detached.Fail():
        print('Detach failed: ' + str(detached))
        ok = False
if ok:
    print('__WSL_JS_CAPTURE_OK__')
)PY";
    return {"lldb", "--no-lldbinit", "--batch",
        "-o", "settings set target.load-script-from-symbol-file false",
        "-o", "settings set use-color false",
        "-o", "plugin load " + lldb_quote(plugin),
        "-o", "process attach --pid " + std::to_string(pid),
        "-o", "script exec(" + Json(script).dump() + ")"};
}

std::string clean_js_output(std::string output, bool& success) {
    // LLDB echoes the script command itself. Only a separate marker line proves
    // that the capture reached its successful end; never match the echoed code.
    std::istringstream lines(output);
    std::string line, cleaned;
    std::set<std::string> diagnostics;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == JsSuccess) { success = true; continue; }
        if (line.rfind("(lldb) script exec(", 0) == 0) continue;
        if (line.rfind("[llnode]", 0) == 0) {
            // llnode reports V8 decoding failures only in debug output and can
            // otherwise exit successfully with just native frame addresses.
            // Keep actual frame failures, not every optional-constant probe.
            if (line.rfind("[llnode][DoExecute ", 0) == 0) diagnostics.insert(line);
            continue;
        }
        cleaned += line + '\n';
    }
    for (const auto& diagnostic : diagnostics) cleaned += diagnostic + '\n';
    return cleaned;
}
} // namespace

std::string runtime_for_executable(std::string executable) {
    // Classify the resolved executable, never argv[0], the process title, a
    // script filename, or a substring in the command line. Shell/npm wrappers
    // therefore don't masquerade as their children. CPython versions/debug and
    // free-threaded build suffixes are accepted; PyPy is not supported by py-spy.
    constexpr const char* deleted = " (deleted)";
    if (executable.size() >= 10 && executable.compare(executable.size() - 10, 10, deleted) == 0)
        executable.resize(executable.size() - 10);
    const auto slash = executable.find_last_of('/');
    const auto name = executable.substr(slash == std::string::npos ? 0 : slash + 1);
    if (name == "node" || name == "nodejs") return "node";
    if (name == "java") return "java";
    if (name == "python") return "python";
    if (name.compare(0, 6, "python") == 0) {
        const auto version = name.substr(6);
        size_t i = 0;
        if (version.empty() || (version[0] != '2' && version[0] != '3')) return {};
        while (i < version.size() && std::isdigit(static_cast<unsigned char>(version[i]))) ++i;
        if (i < version.size() && version[i] == '.') {
            const auto begin = ++i;
            while (i < version.size() && std::isdigit(static_cast<unsigned char>(version[i]))) ++i;
            if (i == begin) return {};
        }
        while (i < version.size() && (version[i] == 'm' || version[i] == 'd' || version[i] == 't')) ++i;
        if (i == version.size()) return "python";
    }
    return {};
}

Json script_stacks(const Json& request) {
    const auto identity = request_identity(request);
    if (identity.pid <= 1 || identity.pid == getpid())
        throw std::runtime_error("Stack capture for this process is protected");
    require_identity(identity);
    const auto executable = read_link(proc_file(identity.pid, "exe"));
    const auto runtime = runtime_for_executable(executable);
    if (runtime.empty()) return unavailable(runtime, "This executable is not a recognized Node.js, CPython or Java runtime.");

    const auto backend = request.value("backend", "auto");
    if (backend != "auto" && backend != "inspector" && backend != "llnode")
        throw std::runtime_error("Unknown JavaScript stack capture backend");
    if (runtime != "node" && backend != "auto")
        throw std::runtime_error("Backend selection is only available for Node.js stacks");
    if (runtime == "node" && backend != "llnode") {
        if (find_command("python3").empty()) {
            auto response = unavailable(runtime, "Node Inspector capture requires Python 3 in this distribution. "
                "On Ubuntu/Debian install it with apt install python3. No extra Python packages are required.");
            response["choice_required"] = true;
            response["inspector_unavailable"] = true;
            return response;
        }
        const Json configuration{{"pid", identity.pid}, {"start_ticks", identity.start_ticks},
            {"enable_inspector", backend == "inspector" && request.value("enable_inspector", false)}};
        // Ship the small stdlib client inside the observer, so updating the
        // observer also updates Inspector support. Isolated Python ignores user
        // startup/site packages; only the fixed embedded source is executed.
        const auto result = run_command({"python3", "-I", "-S", "-c", NodeInspectorScript, configuration.dump()},
            20000, 2 * 1024 * 1024);
        require_identity(identity);
        if (read_link(proc_file(identity.pid, "exe")) != executable)
            throw std::runtime_error("The process changed executable during capture; reopen its properties");
        if (result.timed_out)
            return {{"runtime", "node"}, {"tool", "Node Inspector"}, {"supported", true}, {"success", false},
                {"text", ""}, {"message", "Inspector capture timed out. The diagnostic client was stopped; "
                    "check whether the target is paused or its Inspector remains enabled."}};
        const auto data = Json::parse(result.output, nullptr, false);
        if (result.exit_code != 0 || !data.is_object())
            throw std::runtime_error("The Inspector client could not complete the request: " + result.output);
        if (!data.value("success", false) && !data.value("choice_required", false) &&
            data.value("fallback_safe", false)) {
            // Discovery/capture failures can fall back, but never attach a
            // second debugger when Inspector could not confirm its cleanup.
            Json fallbackRequest = request;
            fallbackRequest["backend"] = "llnode";
            fallbackRequest.erase("enable_inspector");
            const auto diagnostic = data.value("message", "Inspector capture failed");
            auto fallback = script_stacks(fallbackRequest);
            fallback["inspector_error"] = diagnostic;
            fallback["fallback"] = true;
            fallback["text"] = "Node Inspector: " + diagnostic + "\n\n" + data.value("text", "") +
                "\n\nllnode fallback\n" + fallback.value("text", "");
            fallback["message"] = "Inspector failed; tried llnode. " + fallback.value("message", "");
            return fallback;
        }
        return data;
    }

    std::vector<std::string> arguments;
    std::optional<CommandCredentials> credentials;
    std::string java_tool, tool, note;
    if (runtime == "node") {
        const auto plugin = llnode_plugin();
        if (find_command("lldb").empty() || plugin.empty())
            return unavailable(runtime,
                "JavaScript stacks require LLDB and the llnode plugin built for that LLDB version. "
                "On Ubuntu/Debian install lldb and its matching liblldb development package, then "
                "build llnode as your normal user: npm install llnode. Copy the built llnode.so as root "
                "to /usr/local/lib/llnode/llnode.so (root-owned, not group/world writable). "
                "Do not run npm with sudo. See https://github.com/nodejs/llnode#install-instructions. "
                "Node/V8 version and postmortem-symbol support are required.");
        arguments = javascript_command(identity.pid, plugin);
        tool = "LLDB + llnode";
        note = "Captured OS-thread stacks (up to 256 threads, 64 frames each). Idle threads may have no JavaScript frames; "
            "this is not an asynchronous promise/task history. llnode must support the target Node/V8 version and its postmortem symbols.";
    } else if (runtime == "python") {
        if (find_command("py-spy").empty())
            return unavailable(runtime,
                "Python stacks require py-spy. Install an upstream py-spy release executable as "
                "/usr/local/bin/py-spy, owned by root and not group/world writable. Alternatively build "
                "or install it as your user with pip install py-spy, then have an administrator copy the "
                "binary there. See https://github.com/benfred/py-spy. The installed version must support this CPython version.");
        arguments = {"py-spy", "dump", "--pid", std::to_string(identity.pid), "--full-filenames"};
        tool = "py-spy";
        note = "Captured Python thread stacks. Local variable values are not collected.";
    } else {
        credentials = effective_credentials(identity.pid);
        const auto directory = executable.substr(0, executable.find_last_of('/'));
        // Use the target's own JDK, including a securely owned user installation.
        // Arbitrary system jcmd versions are not interchangeable across JDKs.
        java_tool = trusted_command_path(directory + "/jcmd", credentials->uid);
        if (java_tool.empty())
            return unavailable(runtime,
                "Java stacks require jcmd from the target JVM's matching JDK. Install that JDK's "
                "headless development package (for example apt install openjdk-21-jdk-headless for Java 21). "
                "For a custom JRE, use the matching full JDK with jcmd next to bin/java. "
                "The JDK must be owned by root or the target user, with no group/world-writable path components.");
        arguments = {"jcmd", std::to_string(identity.pid), "Thread.print", "-l"};
        tool = "jcmd";
        note = "Captured JVM platform-thread stacks and locks using the target's effective user/group. "
            "Traditional Thread.print dumps do not include every unmounted virtual thread.";
    }

    // These tools attach by numeric PID, not pidfd. Identity checks reduce but
    // cannot eliminate PID reuse between validation and the tool's attachment.
    require_identity(identity);
    if (read_link(proc_file(identity.pid, "exe")) != executable)
        throw std::runtime_error("The process changed executable before stack capture; reopen its properties");
    auto result = run_command(arguments, CaptureTimeoutMs, CaptureOutputLimit, credentials, java_tool);
    require_identity(identity);
    if (read_link(proc_file(identity.pid, "exe")) != executable)
        throw std::runtime_error("The process changed executable during stack capture; the capture was discarded");
    bool success = result.exit_code == 0 && !result.timed_out;
    if (runtime == "node") {
        bool captured = false;
        result.output = clean_js_output(std::move(result.output), captured);
        success = success && captured;
    }
    const bool incomplete_v8 = runtime == "node" && result.output.find("[llnode][DoExecute ") != std::string::npos;
    const bool truncated = result.output.find("[Output truncated]") != std::string::npos;
    success = success && !incomplete_v8 && !truncated;
    std::string message;
    if (result.timed_out)
        message = tool + " timed out after 15 seconds; its command group was terminated. See partial output below.";
    else if (incomplete_v8)
        message = "llnode could not decode some V8 frames. JavaScript names or frames may be missing because "
            "this Node/V8 build is not supported by the installed llnode version. Partial native/V8 frames follow.";
    else if (truncated)
        message = "The stack output reached the 512 KiB limit; this is a partial capture.";
    else if (!success)
        message = tool + " could not capture every requested stack. See diagnostics below. " +
            (runtime == "java" ? "The JVM may disable attachment; the JDK version and target credentials must match."
                               : "Ptrace permissions, runtime version support, and debugging metadata may be required.");
    else
        message = note;
    if (result.timed_out) {
        const auto state = process_stat(identity.pid).state;
        if (state == "T" || state == "t")
            message += " The target is still stopped. Debugger cleanup may be incomplete; inspect its state before resuming it.";
    }
    return {{"runtime", runtime}, {"tool", tool}, {"supported", true}, {"success", success},
        {"timed_out", result.timed_out}, {"exit_code", result.exit_code},
        {"message", message}, {"text", result.output}};
}
} // namespace observer
