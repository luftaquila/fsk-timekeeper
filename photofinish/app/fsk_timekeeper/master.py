"""The master's USB-CDC port: one reader thread, serialized writes, line subscribers; and the
camera command channel on top of it (protocol.py)."""

import asyncio
import logging
import threading

from . import protocol

log = logging.getLogger(__name__)

REPLY_TIMEOUT_S = 0.5
WAIT_RETRY_S = 0.02
FULL_RETRY_S = 0.1
WAIT_LIMIT_S = 2.0  # anchors past a line arrive within CAM_SAMPLE_MS on a healthy master
TIMEOUT_LIMIT = 20
SESSION_TIMEOUT_S = 3.0


def open_serial(path):
    """pyserial port (DTR asserted: the master reports only to an open console)."""
    import serial

    return serial.Serial(path, 115200, timeout=0.5)


class MasterLink:
    """Owns the port. Subscribers get every master line as protocol.Line, in order."""

    def __init__(self, open_port):
        self._open_port = open_port
        self._port = None
        self._lock = threading.Lock()
        self._loop = None
        self.subscribers = []
        self.identity = None  # (usb_proto, devid, role) of the last I line
        self.connected = False

    @property
    def camera_node(self):
        return protocol.camera_node(self.identity[1]) if self.identity else None

    def subscribe(self, callback):
        self.subscribers.append(callback)
        return lambda: self.subscribers.remove(callback)

    def write(self, line):
        with self._lock:
            port = self._port
            if port is None:
                return False
            try:
                port.write((line + "\n").encode("ascii"))
                return True
            except (OSError, ValueError) as error:
                log.warning("master write failed: %s", error)
                return False

    def _dispatch(self, text):
        line = protocol.Line(text)
        ident = line.identity()
        if ident:
            self.identity = ident
        for callback in list(self.subscribers):
            try:
                callback(line)
            except Exception:  # one subscriber never stops the others
                log.exception("line subscriber failed")

    def _read(self, port):
        buffer = b""
        while True:
            chunk = port.read(getattr(port, "in_waiting", 0) or 1)  # what is there, else wait for a byte
            if chunk is None:
                return
            if not chunk:
                if getattr(port, "closed", False):
                    return
                continue
            buffer += chunk
            while b"\n" in buffer:
                raw, buffer = buffer.split(b"\n", 1)
                text = raw.decode("ascii", "replace").rstrip("\r")
                if text:
                    self._loop.call_soon_threadsafe(self._dispatch, text)

    async def run(self, retry_s=1.0):
        """Open, read until the port fails, reopen; forever."""
        self._loop = asyncio.get_running_loop()
        while True:
            try:
                port = await self._loop.run_in_executor(None, self._open_port)
            except OSError as error:
                log.info("master port unavailable: %s", error)
                await asyncio.sleep(retry_s)
                continue
            with self._lock:
                self._port = port
            self.connected = True
            self.write("?ID")
            try:
                await self._loop.run_in_executor(None, self._read, port)
            except OSError as error:
                log.warning("master port lost: %s", error)
            finally:
                with self._lock:
                    self._port = None
                self.connected = False
                self.identity = None
                try:
                    port.close()
                except OSError:
                    pass
            await asyncio.sleep(retry_s)


class SessionLost(Exception):
    """The master no longer takes this session's records (reason: camoff, camline, ...)."""

    def __init__(self, reason):
        super().__init__(reason)
        self.reason = reason


class CameraLink:
    """Stop-and-wait V commands; tracks q and capture_seq of the current session."""

    def __init__(self, master):
        self.master = master
        self.q = 0
        self.seq = 0
        self.session = None
        self._reply = None
        self._diag = None
        self._busy = asyncio.Lock()
        master.subscribe(self._on_line)

    def _on_line(self, line):
        reply = line.camera_reply()
        if reply and self._reply and not self._reply.done():
            self._reply.set_result(reply)
            return
        node = self.master.camera_node
        diag = line.diag_session(node) if node else None
        if diag and self._diag and not self._diag.done():
            self._diag.set_result(diag)

    async def _command(self, text, timeout=REPLY_TIMEOUT_S):
        loop = asyncio.get_running_loop()
        self._reply = loop.create_future()
        if not self.master.write(text):
            return "timeout"
        try:
            return await asyncio.wait_for(self._reply, timeout)
        except asyncio.TimeoutError:
            return "timeout"

    async def start(self, period):
        """New session at period ticks; returns its id (the camera node's sensor_boot_id)."""
        async with self._busy:
            self.session = None
            reply = await self._command(protocol.start(period), timeout=1.0)
            if reply != "ok":
                raise SessionLost(reply)
            self.q = 0
            self.seq = 0
            loop = asyncio.get_running_loop()
            self._diag = loop.create_future()
            self.master.write("?STATUS")
            try:
                session, state = await asyncio.wait_for(self._diag, SESSION_TIMEOUT_S)
            except asyncio.TimeoutError:
                raise SessionLost("status") from None
            self.session = session
            return session

    async def stop(self):
        """No session: the master stops triggering. Its reply ("ok", "timeout", ...)."""
        async with self._busy:
            self.session = None
            return await self._command(protocol.stop())

    async def send(self, kind, *args):
        """One record ('C' line car conf | 'L' first last | 'K' line); its capture_seq (C, L)."""
        async with self._busy:
            if self.session is None:
                raise SessionLost("camoff")
            q = self.q + 1
            text = {"C": protocol.crossing, "L": protocol.loss, "K": protocol.checkpoint}[kind](q, *args)
            waited = 0.0
            timeouts = 0
            while True:
                reply = await self._command(text)
                if reply == "ok":
                    self.q = q
                    if kind == "K":
                        return None
                    self.seq += 1
                    return self.seq
                if reply in ("camwait", "camfull"):
                    delay = WAIT_RETRY_S if reply == "camwait" else FULL_RETRY_S
                    waited += delay
                    if reply == "camwait" and waited > WAIT_LIMIT_S:
                        raise SessionLost("camwait")
                    await asyncio.sleep(delay)
                    continue
                if reply == "timeout":
                    timeouts += 1
                    if timeouts > TIMEOUT_LIMIT:
                        raise SessionLost("timeout")
                    continue  # the same q: the master answers a resend without a second record
                self.session = None
                raise SessionLost(reply)
