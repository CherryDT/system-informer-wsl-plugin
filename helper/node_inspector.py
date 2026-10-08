"""Capture one Node main-thread stack through its own Inspector listener.

Embedded in the native observer and run with a trusted Python's -I -S flags.
All protocol commands are fixed here; the request supplies process identity and
whether the user chose to enable Inspector. No third-party modules are used.
"""

import base64
import hashlib
import ipaddress
import json
import os
import re
import shlex
import signal
import socket
import struct
import sys
import time
import urllib.parse

CAPTURE_SECONDS = 12.0
CLEANUP_SECONDS = 2.0
OUTPUT_LIMIT = 512 * 1024
FRAME_LIMIT = 1024 * 1024
HTTP_LIMIT = 64 * 1024
UUID_PATH = re.compile(r"/[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}\Z")

# Calling close directly is intentional: scheduling it with setImmediate would
# never run in a busy JS loop. close may wait for our connection to disconnect,
# so the client sends this expression and closes its socket without waiting for
# the usual reply. Older ESM runtimes may need the asynchronous import fallback;
# if their event loop cannot run, the caller reports incomplete cleanup.
CLOSE_EXPRESSION = """(() => {
  let inspector;
  if (typeof process.getBuiltinModule === 'function')
    inspector = process.getBuiltinModule('inspector');
  else if (typeof require === 'function')
    inspector = require('inspector');
  else if (process.mainModule && typeof process.mainModule.require === 'function')
    inspector = process.mainModule.require('inspector');
  const close = m => {
    if (!m || typeof m.close !== 'function') return {closeSupported:false};
    m.close();
    return {closeSupported:true};
  };
  if (inspector) return close(inspector);
  try {
    return new Function("return import('inspector')")().then(close, () => ({closeSupported:false}));
  } catch (error) { return {closeSupported:false}; }
})()"""


class CaptureError(Exception):
    pass


class DeadlineExpired(CaptureError):
    pass


class InspectorProbeError(CaptureError):
    """A listener advertised Node Inspector, but its endpoint was invalid."""


def remaining(deadline):
    seconds = deadline - time.monotonic()
    if seconds <= 0:
        raise DeadlineExpired("Node Inspector operation timed out")
    return seconds


def read_file(path, limit):
    with open(path, "rb") as source:
        data = source.read(limit + 1)
    if len(data) > limit:
        raise CaptureError("Process metadata exceeded the collection limit")
    return data


class Target:
    def __init__(self, request):
        self.pid = request.get("pid")
        self.start_ticks = request.get("start_ticks")
        if (type(self.pid) is not int or self.pid <= 1 or self.pid == os.getpid()
                or type(self.start_ticks) is not int or self.start_ticks <= 0):
            raise CaptureError("A valid Node process ID and start time are required")
        self.path = "/proc/%d/" % self.pid
        self.executable_identity = None
        self.check()
        self.inner_pid = self.pid
        for line in read_file(self.path + "status", 64 * 1024).splitlines():
            if line.startswith(b"NSpid:"):
                values = line.split()[1:]
                if values:
                    self.inner_pid = int(values[-1])

    def check(self):
        try:
            stat = read_file(self.path + "stat", 16 * 1024)
            fields = stat[stat.rfind(b")") + 2:].split()
            if len(fields) < 20 or int(fields[19]) != self.start_ticks:
                raise CaptureError("The selected process exited or its PID was reused")
            self.state = fields[0]
            executable = os.readlink(self.path + "exe")
            if executable.endswith(" (deleted)"):
                executable = executable[:-10]
            if os.path.basename(executable) not in ("node", "nodejs"):
                raise CaptureError("The selected executable is no longer Node.js")
            metadata = os.stat(self.path + "exe")
            identity = (metadata.st_dev, metadata.st_ino)
            if self.executable_identity is not None and identity != self.executable_identity:
                raise CaptureError("The selected process changed executable")
            self.executable_identity = identity
            own_net = os.stat("/proc/self/ns/net")
            target_net = os.stat(self.path + "ns/net")
            if (own_net.st_dev, own_net.st_ino) != (target_net.st_dev, target_net.st_ino):
                raise CaptureError("Node Inspector capture requires the process to share the observer's network namespace")
        except (OSError, ValueError) as error:
            raise CaptureError("Cannot verify the selected Node process: %s" % error) from error

    def socket_inodes(self):
        self.check()
        inodes = set()
        try:
            with os.scandir(self.path + "fd") as entries:
                for count, entry in enumerate(entries):
                    if count >= 65536:
                        raise CaptureError("The process has too many file descriptors to safely locate Inspector")
                    try:
                        link = os.readlink(entry.path)
                    except FileNotFoundError:
                        continue  # A descriptor can close during collection.
                    if link.startswith("socket:[") and link.endswith("]"):
                        inodes.add(link[8:-1])
        except OSError as error:
            raise CaptureError("Cannot read the selected process's sockets: %s" % error) from error
        return inodes

    def listeners(self):
        inodes = self.socket_inodes()
        listeners = []
        for family, filename in ((socket.AF_INET, "tcp"), (socket.AF_INET6, "tcp6")):
            try:
                table = read_file("/proc/net/" + filename, 16 * 1024 * 1024)
            except FileNotFoundError:
                continue
            for line in table.splitlines()[1:]:
                fields = line.split()
                if len(fields) < 10 or fields[3] != b"0A" or fields[9].decode("ascii") not in inodes:
                    continue
                address, port = fields[1].decode("ascii").split(":")
                raw = b"".join(struct.pack("=I", int(address[i:i + 8], 16))
                               for i in range(0, len(address), 8))
                bound = ipaddress.ip_address(socket.inet_ntop(family, raw))
                route = ipaddress.ip_address("127.0.0.1" if family == socket.AF_INET else "::1") if bound.is_unspecified else bound
                # /proc/net does not supply the scope ID needed for link-local
                # IPv6 routing. Do not guess an interface or try another peer.
                if route.version == 6 and route.is_link_local:
                    continue
                listeners.append({"family": family, "bound": str(bound), "host": str(route),
                                  "port": int(port, 16), "inode": fields[9].decode("ascii")})
        return sorted(listeners, key=lambda item: (item["port"], item["family"]))

    def owns(self, listener):
        return any(item["inode"] == listener["inode"] and item["port"] == listener["port"]
                   and item["bound"] == listener["bound"] for item in self.listeners())

    def inspector_clients(self, listener):
        # Include other HTTP/WebSocket clients, not just debugger sessions. It
        # is safer to leave a listener enabled than block the target in close()
        # waiting for another client's connection. A new connection can still
        # race this check; the Inspector API offers no atomic close-if-alone.
        inodes = self.socket_inodes()
        accepted = set()
        for filename in ("tcp", "tcp6"):
            try:
                table = read_file("/proc/net/" + filename, 16 * 1024 * 1024)
            except FileNotFoundError:
                continue
            for line in table.splitlines()[1:]:
                fields = line.split()
                if len(fields) < 10 or fields[3] != b"01":
                    continue
                inode = fields[9].decode("ascii")
                port = int(fields[1].split(b":")[1], 16)
                if inode in inodes and port == listener["port"]:
                    accepted.add(inode)
        return len(accepted)

    def activation_allowed(self):
        # Conservative scanning also covers NODE_OPTIONS. A script argument
        # resembling an Inspector option may cause a refusal rather than risk
        # enabling a public listener. Unknown runtime changes to debugPort or
        # custom signal handlers cannot be inferred from /proc configuration.
        tokens = [part.decode("utf-8", "replace") for part in
                  read_file(self.path + "cmdline", 1024 * 1024).split(b"\0") if part]
        for entry in read_file(self.path + "environ", 1024 * 1024).split(b"\0"):
            if entry.startswith(b"NODE_OPTIONS="):
                try:
                    tokens += shlex.split(entry[13:].decode("utf-8", "replace"))
                except ValueError as error:
                    raise CaptureError("Cannot safely parse NODE_OPTIONS before enabling Inspector") from error
        options = ("--inspect", "--inspect-brk", "--inspect-wait", "--inspect-port", "--debug-port")
        for index, token in enumerate(tokens):
            option, separator, value = token.partition("=")
            if option not in options:
                continue
            if not separator:
                if option not in ("--inspect-port", "--debug-port"):
                    continue
                value = tokens[index + 1] if index + 1 < len(tokens) else ""
            if not value or value.isdecimal():
                continue
            if value.startswith("[") and "]" in value:
                host = value[1:value.index("]")]
            elif value.count(":") == 1:
                host = value.rsplit(":", 1)[0]
            else:
                host = value
            if host.lower() == "localhost":
                continue
            try:
                loopback = ipaddress.ip_address(host).is_loopback
            except ValueError:
                loopback = False
            if not loopback:
                raise CaptureError("Automatic Inspector activation was declined: %s configures a non-loopback or unverified host. Start a loopback Inspector explicitly, then retry." % option)

    def enable(self):
        self.activation_allowed()
        if not hasattr(os, "pidfd_open") or not hasattr(signal, "pidfd_send_signal"):
            raise CaptureError("Safe Inspector activation requires Python pidfd_open/pidfd_send_signal support; no numeric-PID signal fallback is used")
        self.check()
        descriptor = os.pidfd_open(self.pid, 0)
        try:
            self.check()
            if self.state in (b"T", b"t"):
                raise CaptureError("The process is stopped. Resume it before enabling Inspector, or use llnode.")
            signal.pidfd_send_signal(descriptor, signal.SIGUSR1, None, 0)
        finally:
            os.close(descriptor)


class Wire:
    """A deadline-aware socket buffer used for HTTP upgrade and WebSocket frames."""
    def __init__(self, listener, deadline):
        self.socket = socket.socket(listener["family"], socket.SOCK_STREAM)
        self.buffer = bytearray()
        self.received = 0
        try:
            self.socket.settimeout(remaining(deadline))
            peer = (listener["host"], listener["port"])
            self.socket.connect(peer if listener["family"] == socket.AF_INET else peer + (0, 0))
        except Exception:
            self.socket.close()
            raise

    def send(self, data, deadline):
        self.socket.settimeout(remaining(deadline))
        self.socket.sendall(data)

    def receive(self, deadline):
        self.socket.settimeout(remaining(deadline))
        data = self.socket.recv(16384)
        if not data:
            raise CaptureError("Node Inspector disconnected")
        self.received += len(data)
        if self.received > 16 * 1024 * 1024:
            raise CaptureError("Node Inspector exceeded the protocol traffic limit")
        self.buffer.extend(data)

    def ensure(self, count, deadline):
        remaining(deadline)
        while len(self.buffer) < count:
            self.receive(deadline)

    def exact(self, count, deadline):
        self.ensure(count, deadline)
        result = bytes(self.buffer[:count])
        del self.buffer[:count]
        return result

    def until(self, delimiter, limit, deadline):
        remaining(deadline)
        while True:
            index = self.buffer.find(delimiter)
            if index >= 0:
                if index + len(delimiter) > limit:
                    raise CaptureError("Node Inspector HTTP header exceeded its limit")
                return self.exact(index + len(delimiter), deadline)
            if len(self.buffer) >= limit:
                raise CaptureError("Node Inspector HTTP header exceeded its limit")
            self.receive(deadline)

    def close(self):
        self.socket.close()


def authority(listener):
    host = listener["host"]
    return ("[%s]" % host if ":" in host else host) + ":" + str(listener["port"])


def headers(wire, deadline):
    raw = wire.until(b"\r\n\r\n", 16 * 1024, deadline)
    lines = raw[:-4].decode("iso-8859-1").split("\r\n")
    first = lines[0].split(" ", 2)
    if len(first) < 2 or first[0] not in ("HTTP/1.0", "HTTP/1.1"):
        raise CaptureError("Invalid Inspector HTTP response")
    values = {}
    for line in lines[1:]:
        name, separator, value = line.partition(":")
        if not separator or name.lower() in values:
            raise CaptureError("Invalid or duplicate Inspector HTTP header")
        values[name.lower()] = value.strip()
    return int(first[1]), values


def inspector_endpoints(listener, deadline):
    wire = Wire(listener, deadline)
    try:
        wire.send(("GET /json/list HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n" % authority(listener)).encode("ascii"), deadline)
        status, response_headers = headers(wire, deadline)
        if status != 200:
            return []
        length = response_headers.get("content-length")
        if response_headers.get("transfer-encoding", "").lower() == "chunked":
            data = bytearray()
            while True:
                size = int(wire.until(b"\r\n", 1024, deadline).split(b";", 1)[0].strip(), 16)
                if size == 0:
                    break
                if size < 0 or len(data) + size > HTTP_LIMIT:
                    raise CaptureError("Inspector discovery response exceeded its limit")
                data.extend(wire.exact(size, deadline))
                if wire.exact(2, deadline) != b"\r\n":
                    raise CaptureError("Invalid chunked Inspector discovery response")
        elif length is not None:
            length = int(length)
            if not 0 <= length <= HTTP_LIMIT:
                raise CaptureError("Inspector discovery response exceeded its limit")
            data = wire.exact(length, deadline)
        else:
            # Node normally supplies Content-Length. Read-to-close is bounded
            # too, for older versions which omit it.
            data = bytearray(wire.buffer)
            wire.buffer.clear()
            while len(data) <= HTTP_LIMIT:
                wire.socket.settimeout(remaining(deadline))
                part = wire.socket.recv(min(16384, HTTP_LIMIT + 1 - len(data)))
                if not part:
                    break
                data.extend(part)
            if len(data) > HTTP_LIMIT:
                raise CaptureError("Inspector discovery response exceeded its limit")
        try:
            listing = json.loads(data)
        except (ValueError, UnicodeError):
            return []  # An application's ordinary HTTP response is not Inspector.
        if not isinstance(listing, list):
            return []
        result = []
        for item in listing[:32]:
            if not isinstance(item, dict) or item.get("type") != "node":
                continue
            try:
                url = urllib.parse.urlsplit(item.get("webSocketDebuggerUrl", ""))
                if (url.scheme != "ws" or url.username is not None or url.password is not None
                        or url.port != listener["port"] or url.query or url.fragment
                        or not UUID_PATH.fullmatch(url.path)):
                    raise ValueError("invalid WebSocket URL")
                # The URL never chooses the connection destination. Only accept
                # our literal address (or localhost for a loopback route).
                valid_host = url.hostname in (listener["host"], listener["bound"])
                if url.hostname == "localhost" and ipaddress.ip_address(listener["host"]).is_loopback:
                    valid_host = True
                if not valid_host:
                    raise ValueError("WebSocket URL names another address")
            except (ValueError, TypeError, AttributeError) as error:
                raise InspectorProbeError("An owned listener advertised an invalid Node Inspector endpoint: " + str(error)) from error
            result.append((listener, url.path))
        return result
    finally:
        wire.close()


def discover(target, deadline):
    candidates = target.listeners()
    endpoints = []
    failures = []
    for listener in candidates[:32]:
        remaining(deadline)
        try:
            endpoints += inspector_endpoints(listener, min(deadline, time.monotonic() + 0.2))
        except InspectorProbeError as error:
            failures.append(str(error))
        except (CaptureError, OSError, ValueError, UnicodeError, TypeError):
            # Node apps also own IPC/TCP servers which close, reset or ignore an
            # HTTP request. Those failures do not identify an Inspector. The
            # Inspector HTTP server runs independently of the JS event loop.
            # Activation separately remembers *all* existing listener inodes,
            # so even a temporarily unresponsive existing Inspector is never
            # mistaken for a listener we may close after sending SIGUSR1.
            continue
    if not endpoints:
        if failures:
            raise CaptureError(failures[0])
        if len(candidates) > 32:
            raise CaptureError("The process owns more than 32 listeners; Inspector discovery was incomplete")
    return endpoints

def debugger_event(session, message):
    method = message.get("method")
    params = message.get("params", {})
    if not isinstance(params, dict):
        raise CaptureError("Invalid Node Inspector event")
    if method == "Debugger.scriptParsed" and len(session.scripts) < 4096:
        session.scripts[str(params.get("scriptId", ""))] = str(params.get("url", ""))[:2048]
    elif method == "Debugger.paused":
        session.paused = params
    elif method == "Debugger.resumed":
        session.paused = None


def debugger_stack(session, heading, deadline):
    session.capture_note = ""
    session.command("Debugger.enable", {}, deadline)
    session.debugger_enabled = True
    # Enable replays an existing pause. Capture it without sending pause/resume
    # commands which would interfere with another debugger's stopped thread.
    session.preexisting_pause = session.paused is not None
    if not session.preexisting_pause:
        session.pause_requested = True
        pause_deadline = min(deadline - 0.15, time.monotonic() + 1.0)
        try:
            session.command("Debugger.pause", {}, pause_deadline)
            while session.paused is None:
                session.pump(pause_deadline)
        except (socket.timeout, DeadlineExpired):
            if session.paused is None:
                # An event-loop thread without JS activity may never reach the
                # requested pause point. Cancel our pending request before
                # inspecting another thread; do not manufacture JS frames or
                # leave a surprise pause waiting for the next application event.
                resume_debugger(session, deadline)
                session.capture_note = "No JavaScript pause point was reached within the sampling window."
                return heading + "\n\n" + session.capture_note
    frames = session.paused.get("callFrames", [])
    if not isinstance(frames, list):
        raise CaptureError("Invalid JavaScript stack response")
    lines = [heading, ""]
    for index, frame in enumerate(frames[:256]):
        location = frame.get("location", {})
        url = frame.get("url") or session.scripts.get(str(location.get("scriptId", ""))) or "<unknown script>"
        name = frame.get("functionName") or "<anonymous>"
        line = int(location.get("lineNumber", 0)) + 1
        column = int(location.get("columnNumber", 0)) + 1
        lines.append("#%d %s at %s:%d:%d" % (index, str(name)[:500], str(url)[:2048], line, column))
    if len(frames) > 256:
        lines.append("[Only the first 256 JavaScript frames are shown]")
    if not frames:
        lines.append("No JavaScript frames were present at the pause point.")
    if session.preexisting_pause:
        lines.append("[Already paused by another debugger; this session did not resume it]")
    return "\n".join(lines)


def resume_debugger(session, deadline):
    if session.pause_requested and not session.preexisting_pause:
        try:
            session.command("Debugger.resume", {}, deadline)
        except CaptureError as error:
            if session.paused is not None or "Can only perform operation while paused" not in str(error):
                raise
            # V8 accepted pause-on-next-statement but no statement ran yet.
            # V8 disable() alone can leave the shared pause-on-next-statement
            # flag set when another debugger is active. First deactivate this
            # session's breakpoints: V8 explicitly cancels its pending pause in
            # that operation. Other sessions retain their own breakpoint state.
            session.command("Debugger.setBreakpointsActive", {"active": False}, deadline)
            session.command("Debugger.disable", {}, deadline)
            session.debugger_enabled = False
        session.pause_requested = False


class WorkerSession:
    """One NodeWorker session multiplexed over the verified parent connection."""
    def __init__(self, parent, identifier, info):
        self.parent = parent
        self.identifier = identifier
        self.title = str(info.get("title", ""))[:500]
        self.worker_id = str(info.get("workerId", identifier))[:100]
        self.scripts = {}
        self.paused = None
        self.preexisting_pause = False
        self.pause_requested = False
        self.debugger_enabled = False
        self.capture_note = ""
        self.detached = False
        self.next_id = 1
        self.responses = {}

    def event(self, message):
        debugger_event(self, message)
        if "id" in message:
            # One command is outstanding per worker. Retain a few late replies
            # after a timeout without allowing unsolicited replies to grow memory.
            if len(self.responses) >= 8:
                self.responses.pop(next(iter(self.responses)))
            self.responses[message["id"]] = message

    def pump(self, deadline):
        if self.detached:
            raise CaptureError("Worker exited or its Inspector session detached")
        self.parent.pump(deadline)

    def command(self, method, params, deadline):
        if self.detached:
            raise CaptureError("Worker exited or its Inspector session detached")
        identifier = self.next_id
        self.next_id += 1
        request = json.dumps({"id": identifier, "method": method, "params": params}, separators=(",", ":"))
        self.parent.command("NodeWorker.sendMessageToWorker", {"sessionId": self.identifier, "message": request}, deadline)
        while identifier not in self.responses:
            self.pump(deadline)
        message = self.responses.pop(identifier)
        if "error" in message:
            raise CaptureError("Worker %s failed: %s" % (method, str(message["error"].get("message", "protocol error"))[:500]))
        result = message.get("result", {})
        if not isinstance(result, dict):
            raise CaptureError("Invalid worker Inspector command result")
        return result

    def detach(self, deadline):
        if not self.detached:
            self.parent.command("NodeWorker.detach", {"sessionId": self.identifier}, deadline)
            self.detached = True

class Inspector:
    def __init__(self, target, endpoint, deadline):
        self.listener, self.path = endpoint
        if not target.owns(self.listener):
            raise CaptureError("The Inspector listener changed ownership before connection")
        self.wire = Wire(self.listener, deadline)
        self.next_id = 1
        self.scripts = {}
        self.fragments = bytearray()
        self.fragmented = False
        self.paused = None
        self.preexisting_pause = False
        self.pause_requested = False
        self.debugger_enabled = False
        self.verified = False
        self.workers = {}
        self.worker_domain = False
        self.worker_limit = False
        self.worker_notes = []
        self.captured_workers = 0
        self.capture_note = ""
        self.sampling_notes = []
        try:
            nonce = base64.b64encode(os.urandom(16)).decode("ascii")
            request = ("GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                       "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n") % (self.path, authority(self.listener), nonce)
            self.wire.send(request.encode("ascii"), deadline)
            status, response_headers = headers(self.wire, deadline)
            accept = base64.b64encode(hashlib.sha1((nonce + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")).digest()).decode("ascii")
            if (status != 101 or response_headers.get("sec-websocket-accept") != accept
                    or response_headers.get("upgrade", "").lower() != "websocket"
                    or "upgrade" not in [value.strip().lower() for value in response_headers.get("connection", "").split(",")]
                    or "sec-websocket-extensions" in response_headers):
                raise CaptureError("Node Inspector WebSocket handshake was rejected")
            response = self.command("Runtime.evaluate", {"expression": "process.pid", "returnByValue": True,
                                                        "silent": True, "throwOnSideEffect": True}, deadline)
            remote_pid = response.get("result", {}).get("value")
            if response.get("exceptionDetails") or type(remote_pid) is not int or remote_pid != target.inner_pid:
                raise CaptureError("The Inspector endpoint did not identify the selected Node process")
            target.check()
            if not target.owns(self.listener):
                raise CaptureError("The Inspector listener changed during verification")
            self.verified = True
        except Exception:
            self.wire.close()
            raise

    def frame(self, opcode, payload, deadline):
        mask = os.urandom(4)
        size = len(payload)
        if size < 126:
            prefix = bytes((0x80 | opcode, 0x80 | size))
        elif size < 65536:
            prefix = bytes((0x80 | opcode, 0x80 | 126)) + struct.pack("!H", size)
        else:
            prefix = bytes((0x80 | opcode, 0x80 | 127)) + struct.pack("!Q", size)
        masked = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
        self.wire.send(prefix + mask + masked, deadline)

    def send(self, method, params, deadline):
        identifier = self.next_id
        self.next_id += 1
        self.frame(1, json.dumps({"id": identifier, "method": method, "params": params}, separators=(",", ":")).encode("utf-8"), deadline)
        return identifier

    def message(self, deadline):
        while True:
            # Do not consume a partial frame. A capture timeout can occur in
            # the middle of a packet, and cleanup must still parse the stream
            # correctly while waiting for Debugger.resume's reply.
            self.wire.ensure(2, deadline)
            first, second = self.wire.buffer[:2]
            opcode, final = first & 15, bool(first & 128)
            if first & 0x70 or second & 128:
                raise CaptureError("Unsupported Inspector WebSocket frame flags")
            size = second & 127
            prefix = 2
            if size == 126:
                prefix = 4
                self.wire.ensure(prefix, deadline)
                size = struct.unpack("!H", self.wire.buffer[2:prefix])[0]
            elif size == 127:
                prefix = 10
                self.wire.ensure(prefix, deadline)
                size = struct.unpack("!Q", self.wire.buffer[2:prefix])[0]
            if size > FRAME_LIMIT or len(self.fragments) + size > FRAME_LIMIT:
                raise CaptureError("Node Inspector WebSocket message exceeded its limit")
            if opcode >= 8 and (not final or size > 125):
                raise CaptureError("Invalid Inspector WebSocket control frame")
            payload = self.wire.exact(prefix + size, deadline)[prefix:]
            if opcode == 8:
                raise CaptureError("Node Inspector closed the WebSocket session")
            if opcode == 9:
                self.frame(10, payload, deadline)
                continue
            if opcode == 10:
                continue
            if opcode == 1 and not self.fragmented:
                self.fragments.extend(payload)
                self.fragmented = not final
            elif opcode == 0 and self.fragmented:
                self.fragments.extend(payload)
            else:
                raise CaptureError("Invalid Inspector WebSocket fragmentation")
            if final:
                data = bytes(self.fragments)
                self.fragments.clear()
                self.fragmented = False
                message = json.loads(data.decode("utf-8"))
                if not isinstance(message, dict):
                    raise CaptureError("Invalid Node Inspector protocol message")
                return message

    def event(self, message):
        debugger_event(self, message)
        method = message.get("method")
        params = message.get("params", {})
        if method == "NodeWorker.attachedToWorker":
            identifier = params.get("sessionId")
            info = params.get("workerInfo", {})
            if not isinstance(identifier, str) or not isinstance(info, dict):
                raise CaptureError("Invalid Node worker attachment event")
            if info.get("type") != "worker":
                return
            if len(self.workers) >= 32:
                self.worker_limit = True
                return
            self.workers[identifier] = WorkerSession(self, identifier, info)
        elif method == "NodeWorker.detachedFromWorker":
            worker = self.workers.get(params.get("sessionId"))
            if worker is not None:
                worker.detached = True
                # An exited worker no longer needs a resume command.
                worker.pause_requested = False
        elif method == "NodeWorker.receivedMessageFromWorker":
            worker = self.workers.get(params.get("sessionId"))
            if worker is not None:
                nested = params.get("message")
                if not isinstance(nested, str) or len(nested) > FRAME_LIMIT:
                    raise CaptureError("Invalid Node worker protocol message")
                decoded = json.loads(nested)
                if not isinstance(decoded, dict):
                    raise CaptureError("Invalid Node worker protocol message")
                worker.event(decoded)

    def pump(self, deadline):
        self.event(self.message(deadline))

    def command(self, method, params, deadline):
        identifier = self.send(method, params, deadline)
        while True:
            message = self.message(deadline)
            self.event(message)
            if message.get("id") == identifier:
                if "error" in message:
                    raise CaptureError("%s failed: %s" % (method, str(message["error"].get("message", "protocol error"))[:500]))
                result = message.get("result", {})
                if not isinstance(result, dict):
                    raise CaptureError("Invalid Node Inspector command result")
                return result

    def capture(self, target, deadline):
        target.check()
        try:
            # This subscription belongs to our CDP session. It never asks Node
            # to pause newly created workers or changes another client's policy.
            self.command("NodeWorker.enable", {"waitForDebuggerOnStart": False}, min(deadline, time.monotonic() + 0.5))
            self.worker_domain = True
        except (CaptureError, OSError, ValueError, TypeError) as error:
            self.worker_notes.append("Worker stacks unavailable: " + str(error))
        sections = [debugger_stack(self, "Node Inspector: main JavaScript thread", deadline)]
        if self.capture_note:
            self.sampling_notes.append("Main thread: " + self.capture_note)
        # Resume the main thread promptly rather than holding it stopped while
        # potentially slower worker captures consume the rest of the deadline.
        resume_debugger(self, min(deadline, time.monotonic() + 0.5))
        for worker in list(self.workers.values()):
            if time.monotonic() + 0.4 >= deadline:
                self.worker_notes.append("Worker capture reached the time limit; remaining workers were not captured.")
                break
            heading = "Worker %s%s" % (worker.worker_id, " (" + worker.title + ")" if worker.title else "")
            try:
                sections.append(debugger_stack(worker, heading, min(deadline - 0.3, time.monotonic() + 1.0)))
                if worker.capture_note:
                    self.sampling_notes.append(heading + ": " + worker.capture_note)
                else:
                    self.captured_workers += 1
            except (CaptureError, OSError, ValueError, TypeError) as error:
                self.worker_notes.append(heading + ": " + str(error))
            try:
                resume_debugger(worker, min(deadline, time.monotonic() + 0.3))
                worker.detach(min(deadline, time.monotonic() + 0.2))
            except (CaptureError, OSError, ValueError, TypeError) as error:
                self.worker_notes.append(heading + " cleanup: " + str(error))
                # Keep at most one possibly paused worker outstanding. Final
                # cleanup retries it using the separate two-second reserve.
                break
        if self.worker_limit:
            self.worker_notes.append("Only the first 32 attached workers were considered.")
        if self.worker_domain and not self.workers:
            sections.append("No existing worker threads were reported by Node Inspector.")
        sections.extend(self.worker_notes)
        sections.append("Threads are sampled sequentially, not at one simultaneous instant. Native frames and asynchronous task history are not included.")
        target.check()
        return "\n\n".join(sections)

    def resume_ours(self, deadline):
        resume_debugger(self, deadline)
        for worker in self.workers.values():
            if worker.pause_requested and not worker.detached:
                resume_debugger(worker, deadline)

    def detach_workers(self, deadline):
        if self.worker_domain:
            for worker in self.workers.values():
                if not worker.detached:
                    worker.detach(deadline)
            # NodeWorker.disable affects only this client's auto-attach handle.
            # Socket disconnect also releases any untracked, never-paused workers.
            self.command("NodeWorker.disable", {}, deadline)
            self.worker_domain = False

    def disconnect(self, deadline):
        try:
            self.frame(8, struct.pack("!H", 1000), deadline)
        except (CaptureError, OSError):
            pass
        self.wire.close()


def connect_matching(target, endpoints, deadline):
    last_error = "No matching Node Inspector endpoint was found"
    for endpoint in endpoints:
        try:
            return Inspector(target, endpoint, min(deadline, time.monotonic() + 1.0))
        except (CaptureError, OSError, ValueError, UnicodeError, TypeError) as error:
            last_error = str(error)
    raise CaptureError(last_error)


def original_listener_present(target, session, deadline):
    if not target.owns(session.listener):
        return False
    try:
        endpoints = inspector_endpoints(session.listener, min(deadline, time.monotonic() + 0.15))
        return any(path == session.path for _, path in endpoints)
    except (CaptureError, OSError, ValueError, UnicodeError, TypeError):
        # A listener which still belongs to the target but cannot be probed is
        # not proof of closure. Preserve the warning in that case.
        return True


def capture(request):
    started = time.monotonic()
    capture_deadline = started + CAPTURE_SECONDS
    final_deadline = capture_deadline + CLEANUP_SECONDS
    result = {"runtime": "node", "tool": "Node Inspector", "supported": True,
              "success": False, "message": "", "text": "", "choice_required": False,
              "fallback_safe": True}
    target = None
    session = None
    enabled_by_us = False
    activation_requested = False
    previous_listeners = set()
    cleanup_notes = []
    cleanup_failed = False
    try:
        target = Target(request)
        endpoints = discover(target, min(capture_deadline, time.monotonic() + 7.0))
        if not endpoints:
            if request.get("enable_inspector") is not True:
                result.update(choice_required=True, message="No Inspector listener owned by this Node process was found. Choose whether to enable Inspector for this capture or use llnode instead.")
                return result
            previous_listeners = {listener["inode"] for listener in target.listeners()}
            target.enable()
            activation_requested = True
            while not endpoints:
                remaining(capture_deadline)
                endpoints = discover(target, capture_deadline)
                if not endpoints:
                    time.sleep(min(0.05, remaining(capture_deadline)))
        session = connect_matching(target, endpoints, capture_deadline)
        enabled_by_us = activation_requested and session.listener["inode"] not in previous_listeners
        result["text"] = session.capture(target, capture_deadline)
        result["success"] = not session.worker_notes and (not session.capture_note or session.captured_workers > 0)
        if session.worker_notes:
            result["message"] = "Captured main-thread JavaScript stacks; worker capture was partial. " + session.worker_notes[0]
        elif session.captured_workers:
            prefix = "the main thread and " if not session.capture_note else ""
            result["message"] = "Captured JavaScript stacks for %s%d worker%s." % (prefix, session.captured_workers, "" if session.captured_workers == 1 else "s")
        elif session.capture_note:
            result["message"] = session.capture_note
        else:
            result["message"] = "Captured main-thread JavaScript stacks."
        if session.sampling_notes and result["success"]:
            result["message"] += " Some threads did not reach a JavaScript pause point; see the capture notes."
        if session.preexisting_pause:
            result["message"] += " The process was already paused; this session did not resume it."
        if activation_requested and not enabled_by_us:
            result["message"] += " The Inspector listener already existed and was left enabled."
    except (CaptureError, OSError, ValueError, UnicodeError, TypeError, KeyError) as error:
        result["message"] = str(error) or "Node Inspector capture failed"
        if activation_requested and session is None:
            result["message"] += " Inspector activation was requested, but no verified session became available (the configured port may be occupied)."
    finally:
        # Cleanup has its own reserve, even after the capture deadline. Never
        # resume an existing pause and never close a pre-existing Inspector.
        cleanup_deadline = min(final_deadline, time.monotonic() + CLEANUP_SECONDS)
        if session is not None:
            try:
                session.resume_ours(min(cleanup_deadline, time.monotonic() + 0.6))
            except (CaptureError, OSError, ValueError, TypeError) as error:
                cleanup_failed = True
                cleanup_notes.append("Could not confirm the process was resumed after capture: %s" % error)
            try:
                session.detach_workers(min(cleanup_deadline, time.monotonic() + 0.25))
            except (CaptureError, OSError, ValueError, TypeError) as error:
                cleanup_failed = True
                cleanup_notes.append("Could not confirm worker Inspector sessions detached: %s" % error)
            skip_close = False
            if enabled_by_us and session.verified:
                try:
                    target.check()
                    if target.inspector_clients(session.listener) > 1:
                        skip_close = True
                        cleanup_failed = True
                        cleanup_notes.insert(0, "Inspector remains enabled because another client connected.")
                    else:
                        identifier = session.send("Runtime.evaluate", {"expression": CLOSE_EXPRESSION, "silent": True,
                                                                        "returnByValue": True, "awaitPromise": False},
                                                  min(cleanup_deadline, time.monotonic() + 0.2))
                        # Unsupported close() can reply immediately. Supported
                        # close() may block until we disconnect, so only allow
                        # a short acknowledgement window and never require one.
                        acknowledgement = min(cleanup_deadline, time.monotonic() + 0.05)
                        try:
                            while True:
                                message = session.message(acknowledgement)
                                session.event(message)
                                if message.get("id") == identifier:
                                    value = message.get("result", {}).get("result", {}).get("value")
                                    if isinstance(value, dict) and value.get("closeSupported") is False:
                                        skip_close = True
                                        cleanup_failed = True
                                        cleanup_notes.insert(0, "Inspector remains enabled because this Node version does not provide inspector.close().")
                                    break
                        except (CaptureError, OSError, ValueError, TypeError):
                            pass
                except (CaptureError, OSError, ValueError, TypeError) as error:
                    cleanup_failed = True
                    cleanup_notes.append("Could not request Inspector closure: %s" % error)
            elif session.debugger_enabled and not session.preexisting_pause:
                try:
                    session.command("Debugger.disable", {}, min(cleanup_deadline, time.monotonic() + 0.2))
                except (CaptureError, OSError, ValueError, TypeError):
                    pass
            session.disconnect(min(cleanup_deadline, time.monotonic() + 0.1))
            if enabled_by_us and not skip_close:
                try:
                    while original_listener_present(target, session, cleanup_deadline):
                        time.sleep(min(0.05, remaining(cleanup_deadline)))
                    cleanup_notes.append("Inspector disabled.")
                except (CaptureError, OSError, ValueError, TypeError):
                    cleanup_failed = True
                    cleanup_notes.insert(0, "Could not verify Inspector was disabled. Its listener may remain enabled; older ESM runtimes or a busy event loop can prevent automatic closure.")
        elif activation_requested:
            cleanup_failed = True
            cleanup_notes.append("Could not verify Inspector was disabled. No verified session was available for cleanup; its listener may remain enabled.")
        if cleanup_failed:
            # Native fallback must not attach another debugger while resume or
            # Inspector cleanup from this attempt remains uncertain.
            result["fallback_safe"] = False
        if cleanup_notes:
            cleanup_text = " ".join(cleanup_notes)
            if cleanup_failed:
                result["message"] = cleanup_text + " " + result["message"]
                result["text"] = cleanup_text + "\n\n" + result["text"]
            else:
                result["message"] += " " + cleanup_text
    return result


def main():
    try:
        if len(sys.argv) != 2 or len(sys.argv[1]) > 64 * 1024:
            raise CaptureError("Expected one bounded JSON capture request")
        request = json.loads(sys.argv[1])
        if not isinstance(request, dict):
            raise CaptureError("The capture request must be a JSON object")
        result = capture(request)
    except Exception as error:
        # stdout belongs solely to the native observer protocol, including
        # unexpected parser/platform failures. Never emit a Python traceback.
        result = {"runtime": "node", "tool": "Node Inspector", "supported": True,
                  "success": False, "choice_required": False, "fallback_safe": False,
                  "text": "", "message": str(error)}
    encoded = json.dumps(result, ensure_ascii=True, separators=(",", ":")).encode("ascii")
    while len(encoded) + 1 > OUTPUT_LIMIT and result.get("text"):
        result["text"] = result["text"][:max(0, int(len(result["text"]) * (OUTPUT_LIMIT - 2048) / len(encoded)) - 128)]
        result["success"] = False
        result["message"] = "Stack output exceeded the 512 KiB limit; this is a partial capture. " + result.get("message", "")[:2048]
        encoded = json.dumps(result, ensure_ascii=True, separators=(",", ":")).encode("ascii")
    sys.stdout.buffer.write(encoded + b"\n")
    sys.stdout.buffer.flush()


if __name__ == "__main__":
    main()
