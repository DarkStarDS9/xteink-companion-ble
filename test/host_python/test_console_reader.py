#!/usr/bin/env python3
"""Host tests for scripts/companion_e2e_test.py's Console serial reader.

No hardware, no BLE, no pyserial: Console's read loop only needs an object with
readline()/read()/write()/flush()/reset_input_buffer()/close(), so the whole
seam is exercisable with a fake.

What is under test is the thing that produced a long-lived "known flake" in the
BLE harness: pyserial's Serial.readline() comes from io.IOBase, which stops at a
newline *or* at an empty read -- and pyserial's read() returns b'' on timeout.
So a readline() straddling the 0.2s port timeout returns a PARTIAL line. The old
reader treated the fragment as a whole reply (it still contained "CT:") and
silently dropped the remainder (which contained neither "CT:" nor "[ERR]"),
which is exactly how state() came back missing screen= or sessions=.

Run:  python3 test/host_python/test_console_reader.py
      (scripts/run_companion_tests.sh runs it every loop)
"""

import os
import queue
import random
import sys
import threading
import time
import types
import unittest

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO_ROOT, "scripts"))

# companion_e2e_test imports bleak/pyserial at module scope. Neither is touched
# by the reader seam, so stub whatever is missing rather than making a host unit
# test depend on a BLE stack being installed.
for _name in ("serial", "bleak"):
    if _name not in sys.modules:
        try:
            __import__(_name)
        except ImportError:
            sys.modules[_name] = types.ModuleType(_name)
_serial_stub = sys.modules["serial"]
for _attr, _obj in (("Serial", object), ("SerialException", Exception)):
    if not hasattr(_serial_stub, _attr):
        setattr(_serial_stub, _attr, _obj)
_bleak_stub = sys.modules["bleak"]
for _attr in ("BleakClient", "BleakScanner"):
    if not hasattr(_bleak_stub, _attr):
        setattr(_bleak_stub, _attr, type(_attr, (), {}))
if not hasattr(_bleak_stub, "BleakError"):
    _bleak_stub.BleakError = type("BleakError", (Exception,), {})
for _mod, _members in (
    ("bleak.exc", {"BleakError": _bleak_stub.BleakError}),
    ("bleak.backends", {}),
    ("bleak.backends.device", {"BLEDevice": type("BLEDevice", (), {})}),
    ("bleak.backends.scanner", {"AdvertisementData": type("AdvertisementData", (), {})}),
):
    if _mod not in sys.modules:
        module = types.ModuleType(_mod)
        for _k, _v in _members.items():
            setattr(module, _k, _v)
        sys.modules[_mod] = module

import companion_e2e_test  # noqa: E402

TIMEOUT = object()  # a scripted read() that returns b'' -- pyserial's timeout


class FakeSerial:
    """A serial port whose readline() has pyserial's exact timeout semantics.

    read() serves a script of chunks; a TIMEOUT entry yields b"" the way a
    pyserial read() does when its timeout expires. readline() is io.IOBase's
    algorithm verbatim -- accumulate until b"\\n" *or* an empty read -- which is
    the whole point: that is what returns a partial line.

    `responder` turns it into a device: each full `CMD:...` line written is
    passed to it, and whatever chunks it returns are appended to the script.
    """

    def __init__(self, script=(), responder=None):
        self._script = list(script)
        self._lock = threading.Lock()
        self._responder = responder
        self._pending_write = bytearray()
        self.commands: list[str] = []
        self.closed = False
        self.resets = 0
        self.exhausted = threading.Event()
        if not self._script:
            self.exhausted.set()

    def _next_chunk(self):
        with self._lock:
            if not self._script:
                self.exhausted.set()
                empty = True
            else:
                empty = False
        if empty:
            # A real port blocks for its timeout here. Returning instantly would
            # spin the reader thread hot and starve the test thread of the GIL.
            time.sleep(0.004)
            return b""
        with self._lock:
            if not self._script:
                return b""
            item = self._script.pop(0)
            if not self._script:
                self.exhausted.set()
        if item is TIMEOUT:
            time.sleep(0.004)  # stand-in for the 0.2s port timeout
            return b""
        if callable(item):
            return item()
        return item

    def read(self, size=1):
        return self._next_chunk()

    def readline(self):
        line = bytearray()
        while True:
            chunk = self.read(1)
            if not chunk:
                break
            line += chunk
            if line.endswith(b"\n"):
                break
        return bytes(line)

    def write(self, data):
        self._pending_write.extend(data)
        while b"\n" in self._pending_write:
            raw, _, rest = bytes(self._pending_write).partition(b"\n")
            self._pending_write = bytearray(rest)
            command = raw.decode().strip()
            self.commands.append(command)
            if self._responder is not None:
                chunks = self._responder(command) or []
                with self._lock:
                    self._script.extend(chunks)
                    if self._script:
                        self.exhausted.clear()
        return len(data)

    def flush(self):
        pass

    def reset_input_buffer(self):
        self.resets += 1

    def close(self):
        self.closed = True


def make_console(script=(), responder=None):
    """A Console wired to a FakeSerial, bypassing __init__'s real port open."""
    fake = FakeSerial(script, responder)
    console = companion_e2e_test.Console(port="fake", serial_port=fake)
    return console, fake


def collect(console, fake, expected=1, timeout=1.0):
    """Waits for the script to drain, then returns every queued CT: reply."""
    fake.exhausted.wait(timeout)
    deadline = time.time() + timeout
    replies: list[str] = []
    while len(replies) < expected and time.time() < deadline:
        try:
            replies.append(console._ct_queue.get(timeout=0.05))
        except queue.Empty:
            pass
    time.sleep(0.05)  # settle: a duplicate or a late fragment would land here
    while True:
        try:
            replies.append(console._ct_queue.get_nowait())
        except Exception:
            break
    return replies


STATE_LINE = b"CT:state screen=list connected=1 sessions=2 heap=38424\n"
STATE_REPLY = "state screen=list connected=1 sessions=2 heap=38424"


class PartialReadTest(unittest.TestCase):
    def test_whole_line_in_one_read(self):
        console, fake = make_console([STATE_LINE, TIMEOUT])
        try:
            self.assertEqual(collect(console, fake), [STATE_REPLY])
        finally:
            console.close()

    def test_every_split_offset_of_a_ct_line(self):
        """A CT: reply split at *any* byte offset must arrive once and intact."""
        for offset in range(1, len(STATE_LINE)):
            with self.subTest(offset=offset):
                script = [STATE_LINE[:offset], TIMEOUT, STATE_LINE[offset:], TIMEOUT]
                console, fake = make_console(script)
                try:
                    self.assertEqual(collect(console, fake), [STATE_REPLY])
                finally:
                    console.close()

    def test_three_way_split(self):
        """More than one timeout inside one line -- the buffer must survive both."""
        a, b = 6, 30
        script = [STATE_LINE[:a], TIMEOUT, STATE_LINE[a:b], TIMEOUT, STATE_LINE[b:], TIMEOUT]
        console, fake = make_console(script)
        try:
            self.assertEqual(collect(console, fake), [STATE_REPLY])
        finally:
            console.close()

    def test_every_split_offset_of_an_err_line(self):
        err = b"[ERR] CompanionBle: peer rejected\n"
        for offset in range(1, len(err)):
            with self.subTest(offset=offset):
                console, fake = make_console([err[:offset], TIMEOUT, err[offset:], TIMEOUT])
                try:
                    collect(console, fake, expected=0)
                    self.assertEqual(console.pop_errors(), ["[ERR] CompanionBle: peer rejected"])
                finally:
                    console.close()

    def test_log_noise_prefixing_a_reply_is_still_stripped(self):
        line = b"12:00:00 [DBG] main: dispatch CT:pong v12\n"
        for offset in (1, 10, 26, 30, len(line) - 1):
            with self.subTest(offset=offset):
                console, fake = make_console([line[:offset], TIMEOUT, line[offset:], TIMEOUT])
                try:
                    self.assertEqual(collect(console, fake), ["pong v12"])
                finally:
                    console.close()

    def test_inf_and_dbg_lines_are_still_dropped(self):
        noise = b"[INF] boot ok\n[DBG] heap=38424\n"
        console, fake = make_console([noise[:9], TIMEOUT, noise[9:], TIMEOUT])
        try:
            self.assertEqual(collect(console, fake, expected=0), [])
            self.assertEqual(console.pop_errors(), [])
        finally:
            console.close()

    def test_several_lines_in_one_read(self):
        console, fake = make_console([STATE_LINE + b"CT:peers count=0\n", TIMEOUT])
        try:
            self.assertEqual(collect(console, fake, expected=2), [STATE_REPLY, "peers count=0"])
        finally:
            console.close()

    def test_unterminated_tail_is_flushed_at_close(self):
        """A last reply with no trailing newline must not be swallowed by close()."""
        console, fake = make_console([b"CT:pong v12", TIMEOUT])
        fake.exhausted.wait(2.0)
        time.sleep(0.05)
        console.close()
        replies = []
        while True:
            try:
                replies.append(console._ct_queue.get_nowait())
            except Exception:
                break
        self.assertEqual(replies, ["pong v12"])

    def test_a_huge_newlineless_run_cannot_grow_the_buffer_without_bound(self):
        blob = b"x" * 200000
        console, fake = make_console([blob, TIMEOUT, b"CT:pong v12\n", TIMEOUT])
        try:
            self.assertEqual(collect(console, fake, timeout=5.0), ["pong v12"])
            self.assertLessEqual(len(console._rx_buffer), companion_e2e_test.Console.MAX_LINE_BYTES)
        finally:
            console.close()

    def test_pausing_the_reader_drops_a_partial_line(self):
        """_reader_paused hands the port to read_screenshot()'s raw reads, which
        start with reset_input_buffer(); a half-line held here would otherwise be
        stitched onto whatever follows the dump."""
        console, fake = make_console([b"CT:sta", TIMEOUT])
        try:
            fake.exhausted.wait(2.0)
            time.sleep(0.05)
            console._reader_paused.set()
            time.sleep(0.1)
            self.assertEqual(console._rx_buffer, b"")
        finally:
            console._reader_paused.clear()
            console.close()


class RandomSplitTest(unittest.TestCase):
    """A whole transcript chopped at random offsets, the way a real port does it."""

    TRANSCRIPT = (
        b"[INF] boot ok\n"
        b"CT:pong v12\n"
        b"CT:state screen=waiting_app connected=1 sessions=1 heap=38424\n"
        b"CT:peers count=2\n"
        b"CT:peer id=0 app=spokenfeeds tag=a1b2\n"
        b"CT:peer id=1 app=todo tag=c3d4\n"
        b"[ERR] CompanionBle: refused image\n"
        b"CT:state screen=list connected=1 sessions=2 heap=38000\n"
    )
    EXPECTED_CT = [
        "pong v12",
        "state screen=waiting_app connected=1 sessions=1 heap=38424",
        "peers count=2",
        "peer id=0 app=spokenfeeds tag=a1b2",
        "peer id=1 app=todo tag=c3d4",
        "state screen=list connected=1 sessions=2 heap=38000",
    ]

    def test_random_chopping_never_loses_or_corrupts_a_reply(self):
        rng = random.Random(20260810)
        for trial in range(15):
            with self.subTest(trial=trial):
                cuts = sorted(rng.sample(range(1, len(self.TRANSCRIPT)), 12))
                script = []
                previous = 0
                for cut in cuts + [len(self.TRANSCRIPT)]:
                    script.append(self.TRANSCRIPT[previous:cut])
                    script.append(TIMEOUT)
                    previous = cut
                console, fake = make_console(script)
                try:
                    replies = collect(console, fake, expected=len(self.EXPECTED_CT), timeout=2.0)
                    self.assertEqual(replies, self.EXPECTED_CT)
                    self.assertEqual(console.pop_errors(), ["[ERR] CompanionBle: refused image"])
                finally:
                    console.close()


class StaleReplyTest(unittest.TestCase):
    """send() must never attribute a previous command's reply to a new command."""

    STALE = b"CT:state screen=waiting_app connected=0 sessions=2 heap=1\n"
    FRESH = b"CT:state screen=list connected=1 sessions=1 heap=2\n"
    FRESH_REPLY = "state screen=list connected=1 sessions=1 heap=2"

    def _responder(self, delay_before_pong=0.0):
        def respond(command):
            if command == "CMD:CPING":
                if delay_before_pong:
                    return [lambda: (time.sleep(delay_before_pong), self.STALE)[1], b"CT:pong v12\n"]
                return [b"CT:pong v12\n"]
            if command == "CMD:CSTATE":
                return [self.FRESH, TIMEOUT]
            return [TIMEOUT]

        return respond

    def test_a_line_already_queued_is_not_attributed_to_the_next_command(self):
        console, fake = make_console([self.STALE, TIMEOUT], self._responder())
        try:
            time.sleep(0.05)  # let the stale line reach the queue first
            replies = console.send("CSTATE", expect="state", timeout=3.0)
            self.assertEqual(replies, [self.FRESH_REPLY])
        finally:
            console.close()

    def test_a_line_landing_after_the_drain_is_still_discarded(self):
        """The old drain-then-write order lost this one: the reader is concurrent,
        so a straggler from the previous command could be queued after the drain
        had already run and be read back as this command's answer."""
        console, fake = make_console([TIMEOUT], self._responder(delay_before_pong=0.05))
        try:
            replies = console.send("CSTATE", expect="state", timeout=3.0)
            self.assertEqual(replies, [self.FRESH_REPLY])
        finally:
            console.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
