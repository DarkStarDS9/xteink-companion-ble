#!/usr/bin/env python3
"""
The single Python implementation of the Companion Display Protocol (v12).

`docs/companion-display-protocol.md` is authoritative for the wire format; this
module is its one Python transcription. Both Python clients in this repo —
`push_companion_content.py` (the bring-up pusher) and `companion_e2e_test.py`
(the on-hardware end-to-end harness) — import it and own no wire format of
their own.

That rule exists because the alternative was tried and failed. The two scripts
each carried their own copy of the UUIDs, opcodes, framing and handshake; the
pusher was kept current through v7-v11 while the harness silently froze at v6
and refused to start ("Device speaks protocol v11, this harness speaks v6") for
five consecutive protocol versions without anyone noticing. A protocol change
now has exactly one Python site to edit, and a version bump that is missed here
breaks both clients loudly rather than one of them quietly.

What lives here: GATT UUIDs, content/session opcodes, field ids, the
notification decoder, capability parsing, the HELLO/ACQUIRE/RELEASE handshake,
host-side token persistence, and the START/CHUNK/END framer (including the v9
image / v10 title-body sequence numbers, the v11 `pushId` on END, and v12's
mandatory declared content shape and its LIST_STATE pull).

What deliberately does not live here: anything a *particular* client decides —
which buttons it declares, what it prints, how it correlates its own pushes,
its argument parsing. This module never prints; callers pass hooks for the
notifications they want to surface.
"""

from __future__ import annotations

import asyncio
import hashlib
import json
import os
import struct
import uuid
from dataclasses import dataclass, field as dataclass_field
from pathlib import Path
from typing import Callable, Iterable, Sequence

from bleak import BleakClient

# The version this module implements. Checked against capability byte 0 by
# parse_capabilities(); bump it in the same commit as the doc and the firmware.
PROTOCOL_VERSION = 12

# --------------------------------------------------------------------------- #
# GATT
# --------------------------------------------------------------------------- #

SERVICE_UUID = "7c9c0000-3e4a-4b1a-9c1e-6d8a1f2b0001"
CONTENT_CHAR_UUID = "7c9c0001-3e4a-4b1a-9c1e-6d8a1f2b0001"
BUTTON_CHAR_UUID = "7c9c0002-3e4a-4b1a-9c1e-6d8a1f2b0001"
CAPABILITY_CHAR_UUID = "7c9c0003-3e4a-4b1a-9c1e-6d8a1f2b0001"
STATUS_CHAR_UUID = "7c9c0004-3e4a-4b1a-9c1e-6d8a1f2b0001"
SESSION_CHAR_UUID = "7c9c0005-3e4a-4b1a-9c1e-6d8a1f2b0001"

# --------------------------------------------------------------------------- #
# Content characteristic
# --------------------------------------------------------------------------- #

OP_START, OP_CHUNK, OP_END = 0x01, 0x02, 0x03

FIELD_TITLE, FIELD_BODY, FIELD_CONTENT_ID = 0x01, 0x02, 0x03
FIELD_IMAGE, FIELD_UI_DECL, FIELD_ICON, FIELD_TAG_STATE = 0x04, 0x05, 0x06, 0x07
# v12 (ToDo List Phase A): the whole document, revision + lists + groups +
# items, per docs/companion-todo-list-design.md §4. Reassembled in RAM like
# title/body, not streamed to SD like image -- it is small (cap 16 KiB, see
# MAX_LIST_DOC_LEN) and pushed rarely, as a full replace. Next free after
# this is 0x09.
FIELD_LIST_DOC = 0x08
FINAL_FLAG = 0x80

# docs/companion-todo-list-design.md §4: the whole list document is
# reassembled in RAM, capped well inside the ~166 KB of headroom measured
# there. This is the only size cap on a list document -- an earlier revision
# also capped total item count (MAX_LIST_ITEMS) to bound a device-side JSON
# read-back that no longer exists; storage and read-back both now move the
# verbatim wire bytes, which nothing further needs to bound. See
# docs/companion-todo-list-design.md §3.
MAX_LIST_DOC_LEN = 16 * 1024

# Fields whose CHUNKs carry the 2-byte little-endian sequence number: image
# since v9, title/body since v10. Every other field's CHUNK payload still
# starts at byte 2. Sending the sequence number to a field that doesn't expect
# it corrupts the payload silently (the two bytes land as data), so this tuple
# is the whole of that decision.
SEQ_CHUNK_FIELDS = (FIELD_IMAGE, FIELD_TITLE, FIELD_BODY)

# Framing overheads, in bytes, deducted from the ATT payload to size a CHUNK.
CHUNK_HEADER_LEN = 2  # opcode + sessionId
CHUNK_SEQ_LEN = 2  # + uint16 seq, for SEQ_CHUNK_FIELDS

# --------------------------------------------------------------------------- #
# Session characteristic
# --------------------------------------------------------------------------- #

SESS_HELLO, SESS_BYE, SESS_ACQUIRE, SESS_RELEASE = 0x01, 0x02, 0x03, 0x04
SESS_LIST_STATE_GET = 0x05  # v12

SESS_HELLO_OK = 0x81
SESS_HELLO_PENDING = 0x82
SESS_HELLO_DENIED = 0x83
SESS_FOREGROUND = 0x84
SESS_BACKGROUND = 0x85
SESS_ACQUIRE_DENIED = 0x86
SESS_ASSET_ACK = 0x87
# 0x88 was IMAGE_STATUS through v10; v11 renamed it RENDER_STATUS and appended
# pushId, because it now answers a title/body/content-id/tag batch as well as
# an image push. Deliberately not aliased to the old name: a caller reaching
# for IMAGE_STATUS is a caller still thinking in v10 terms.
SESS_RENDER_STATUS = 0x88
SESS_IMAGE_CHUNK_ACK = 0x89  # v9, diagnostic only
SESS_FIELD_SEQ_GAP = 0x8A  # v10
# v12: the ToDo List check-off sync-back. AVAIL is unsolicited (after HELLO_OK,
# alongside FOREGROUND, and live as the user ticks boxes) and only ever sent
# when the device is actually holding edits; LIST_STATE answers a GET. The pull
# is paginated into whole, independently-parseable notifications precisely so
# the session characteristic's "never chunked" rule still holds -- see
# docs/companion-display-protocol.md.
SESS_LIST_STATE_AVAIL = 0x8B
SESS_LIST_STATE = 0x8C

# Entries per LIST_STATE notification. Fixed by the protocol, not derived from
# the negotiated MTU, so this constant is the contract and not a guess.
LIST_STATE_ENTRIES_PER_NOTIFY = 30
# 11-byte header + 30 * 3. The assertion that this never grows is what keeps
# the pull inside the 103-byte session-characteristic floor.
LIST_STATE_MAX_NOTIFY_LEN = 11 + 3 * LIST_STATE_ENTRIES_PER_NOTIFY

DENIED_REASONS = {
    0x00: "user rejected",
    0x01: "timed out",
    0x02: "no session slots",
    0x03: "malformed HELLO",
    0x04: "storage failure",
    0x05: "another pairing prompt is up",
    0x06: "protocol version mismatch",  # v12
}
HELLO_DENIED_PROTOCOL_MISMATCH = 0x06
ACQUIRE_DENIED_REASONS = {0x00: "no UI declaration stored", 0x01: "unknown session"}
BACKGROUND_REASONS = {0x00: "preempted", 0x01: "released", 0x02: "link lost"}

ASSET_STORED = 0x00
ASSET_REJECTED_SIZE = 0x01
ASSET_REJECTED_FORMAT = 0x02
ASSET_REJECTED_STORAGE = 0x03
# v12: the declaration parsed, but its mandatory content-shape byte was absent
# or not one of the three known values. Distinct from RejectedFormat on purpose
# -- during the v11->v12 migration "your declaration is missing its shape byte"
# is a far better thing to read than "malformed".
ASSET_REJECTED_NO_SHAPE = 0x04
ASSET_RESULTS = {
    ASSET_STORED: "stored",
    ASSET_REJECTED_SIZE: "rejected: size",
    ASSET_REJECTED_FORMAT: "rejected: format",
    ASSET_REJECTED_STORAGE: "rejected: storage",
    ASSET_REJECTED_NO_SHAPE: "rejected: no content shape declared",
}

RENDER_DISPLAYED = 0x00
RENDER_DECODE_FAILED = 0x01
RENDER_REJECTED_SIZE = 0x02
RENDER_STORAGE_FAILED = 0x03
RENDER_SEQUENCE_GAP = 0x04
RENDER_SUPERSEDED = 0x05
# v12: the pushed field does not match the shape this peer declared. Latched at
# START and answered at END, so a whole batch of mismatched fields is answered
# exactly once -- on its final-flagged field, and only when pushId != 0.
RENDER_REJECTED_SHAPE = 0x06
RENDER_RESULTS = {
    RENDER_DISPLAYED: "displayed",
    RENDER_DECODE_FAILED: "decode failed",
    RENDER_REJECTED_SIZE: "rejected: size",
    RENDER_STORAGE_FAILED: "storage failed",
    RENDER_SEQUENCE_GAP: "sequence gap (dropped/reordered chunk, or a batch that lost a field)",
    RENDER_SUPERSEDED: "superseded (a later push took the screen first)",
    RENDER_REJECTED_SHAPE: "rejected: field does not match this peer's declared content shape",
}

# --------------------------------------------------------------------------- #
# Declared content shape (v12)
#
# A mandatory byte in the UI declaration, at asset offset 4 / body offset 0 --
# immediately after the digest, before the button count. It says which content
# fields this peer is permitted to push for the whole of its enrollment, and it
# is what the firmware consults instead of inferring a mode from whatever field
# happened to arrive last.
#
# 0x00 and anything above 0x03 are INVALID, not reserved: an unknown shape is
# refused (ASSET_REJECTED_NO_SHAPE) rather than tolerated, because tolerating
# it would mean falling back to the reactive model v12 replaces.
# --------------------------------------------------------------------------- #

SHAPE_TEXT = 0x01  # title (0x01), body (0x02), content-id (0x03), tag-state (0x07)
SHAPE_IMAGE = 0x02  # image (0x04), tag-state (0x07)
SHAPE_LIST = 0x03  # todo-list document (0x08), no overlay
SHAPE_NAMES = {SHAPE_TEXT: "TEXT", SHAPE_IMAGE: "IMAGE", SHAPE_LIST: "LIST"}

# Which content fields each shape permits. Asset fields (0x05 declaration,
# 0x06 icon) are never shape-checked -- a peer of any shape declares itself and
# supplies its icon. Tag state (0x07) is exempt in a different way: it is an
# overlay drawn over whatever content is on screen, not content of its own, so
# both TEXT and IMAGE permit it and an IMAGE peer can push image + tag as a
# single atomic batch. LIST permits only its own document (0x08) -- 0x07
# included in what it does NOT permit, per
# docs/companion-declared-shape-design.md §5: there is no overlay on
# Screen::List.
SHAPE_FIELDS = {
    SHAPE_TEXT: (FIELD_TITLE, FIELD_BODY, FIELD_CONTENT_ID, FIELD_TAG_STATE),
    SHAPE_IMAGE: (FIELD_IMAGE, FIELD_TAG_STATE),
    SHAPE_LIST: (FIELD_LIST_DOC,),
}

# Capability-block flag bits (byte 5). Bit 4 is v12's: the device enforces
# declared shapes, so a client can tell before pushing anything whether its
# declaration needs a shape byte.
CAP_FLAG_IMAGE = 0x01
CAP_FLAG_BUTTON_MAP = 0x02
CAP_FLAG_ICONS = 0x04
CAP_FLAG_SESSIONS = 0x08
CAP_FLAG_SHAPE_AWARE = 0x10
CAP_FLAG_LIST_STATE_SYNC = 0x20  # v12

BTN_BACK, BTN_CONFIRM, BTN_LEFT, BTN_RIGHT, BTN_UP, BTN_DOWN, BTN_POWER = range(7)
BUTTON_NAMES = {0: "BACK", 1: "CONFIRM", 2: "LEFT", 3: "RIGHT", 4: "UP", 5: "DOWN", 6: "POWER"}

ROUTING_NONE, ROUTING_REMOTE, ROUTING_PAGE_PREV, ROUTING_PAGE_NEXT, ROUTING_SLEEP = range(5)

TAG_HIDDEN, TAG_OUTLINE, TAG_FILLED = 0, 1, 2


# --------------------------------------------------------------------------- #
# Payload codecs
# --------------------------------------------------------------------------- #


def asset_tag(body: bytes) -> bytes:
    """First 4 bytes of SHA-256 — a content hash, so downgrades re-push correctly.

    All-zero means "no asset stored" on the wire, so a hash that lands there is
    nudged off it rather than being read back as "nothing here".
    """
    tag = hashlib.sha256(body).digest()[:4]
    return b"\x00\x00\x00\x01" if tag == b"\x00\x00\x00\x00" else tag


def encode_ui_declaration(
    entries: Iterable[tuple[int, int, str]],
    tags: Iterable[tuple[int, str]] = (),
    style: int | None = None,
    capabilities: int | None = None,
    *,
    shape: int | None,
) -> bytes:
    """Field 0x05: the digest, the content shape, the button map, the tag labels.

    `shape` (v12) is mandatory on the wire and therefore mandatory here — one of
    SHAPE_TEXT / SHAPE_IMAGE / SHAPE_LIST. It is keyword-only and has no
    default on purpose: a default would let a caller ship a shape it never
    chose, and a v11-era positional call (`encode_ui_declaration(MAP, TAGS)`)
    fails loudly here instead of silently reading TAGS as the shape.

    `shape=None` emits a v11-shaped declaration with no shape byte at all. No
    real client wants that, and no real (correct-protocolVersion) client can
    even reach this anymore -- a stale client is refused at HELLO now, before
    it has a session to push a declaration on (see HELLO's protocolVersion
    field). This exists to prove a v11-shaped buffer from a *compliant*
    session is still just refused as malformed (ASSET_REJECTED_FORMAT), same
    as any other corrupt asset.

    `style` (v6) and `capabilities` (v8) are the optional trailing bytes and
    are positional, not tagged — asking for capabilities without a style byte
    is impossible on the wire, so passing `capabilities` alone emits the
    default style (BORDERED) ahead of it. The shape byte deliberately is *not*
    one of these: a mandatory field cannot sit behind optional ones whose
    absence is signalled by the buffer running out, which is why it goes at the
    front rather than the end.
    """
    entries = list(entries)
    tags = list(tags)
    if shape is not None and shape not in SHAPE_NAMES:
        raise ValueError(f"content shape {shape:#04x} is not one of {sorted(SHAPE_NAMES)}")
    body = b"" if shape is None else bytes([shape])
    body += bytes([len(entries)])
    for button, routing, label in entries:
        encoded = label.encode("utf-8")
        body += bytes([button, routing, len(encoded)]) + encoded
    body += bytes([len(tags)])
    for tag_id, label in tags:
        encoded = label.encode("utf-8")[:12]
        body += bytes([tag_id, len(encoded)]) + encoded
    if style is not None or capabilities is not None:
        body += bytes([style or 0])
    if capabilities is not None:
        body += bytes([capabilities])
    return asset_tag(body) + body


def encode_tag_state(states: Iterable[tuple[int, int]]) -> bytes:
    """Field 0x07: (tagId, state) pairs. state 0 hidden / 1 outline / 2 filled."""
    states = list(states)
    out = bytes([len(states)])
    for tag_id, state in states:
        out += bytes([tag_id, state])
    return out


def encode_icon_bits(bits: bytes) -> bytes:
    """Field 0x06: the digest followed by the 1-bpp bitmap."""
    return asset_tag(bits) + bits


def encode_list_doc(lists: Iterable[dict], revision: int = 1) -> bytes:
    """Field 0x08 (v12, ToDo List Phase A): revision, then lists, groups, items.

    docs/companion-todo-list-design.md §4 is the authoritative layout; every
    multi-byte value (revision, listId, groupId, itemId) is little-endian, the
    same convention `encode_hello`/`push_field` already use elsewhere in this
    module (`struct.pack("<H"/"<I", ...)`).

        revision : u32
        count L : u8, then L times:
            listId : u16, titleLen : u8, title
            group count G : u8, then G times:
                groupId : u16, labelLen : u8, label   (empty label = ungrouped)
                item count I : u8, then I times:
                    itemId : u16, checked : u8, textLen : u8, text

    `lists` is the ergonomic Python shape this is built from -- a sequence of
    dicts, nesting groups and items the same way the wire format does:

        encode_list_doc([
            {"id": 1, "title": "Groceries", "groups": [
                {"id": 1, "label": "", "items": [
                    {"id": 1, "text": "Milk", "checked": 0},
                    {"id": 2, "text": "Eggs", "checked": 1},
                ]},
            ]},
        ], revision=7)

    Deliberately permissive on `checked`: any int that fits in one byte is
    packed as-is, with no 0/1 validation here. The wire format says "checked
    (0 or 1 ONLY)"; a client that wants to prove the *firmware* enforces that
    constraint needs an encoder that will build the illegal byte instead of
    silently coercing it, so `encode_list_doc([...{"checked": 2}...])`
    produces exactly that. A truncated document needs no special support
    either -- slice the returned bytes, e.g. `encode_list_doc(GOOD)[:20]`.
    """
    lists = list(lists)
    out = struct.pack("<I", revision) + bytes([len(lists)])
    for lst in lists:
        title = lst["title"].encode("utf-8")
        groups = list(lst.get("groups", ()))
        out += struct.pack("<H", lst["id"]) + bytes([len(title)]) + title + bytes([len(groups)])
        for grp in groups:
            label = grp.get("label", "").encode("utf-8")
            items = list(grp.get("items", ()))
            out += struct.pack("<H", grp["id"]) + bytes([len(label)]) + label + bytes([len(items)])
            for item in items:
                text = item["text"].encode("utf-8")
                out += (
                    struct.pack("<H", item["id"])
                    + bytes([item.get("checked", 0)])
                    + bytes([len(text)])
                    + text
                )
    return out


def encode_hello(hello_tag: int, app_id: bytes, install_id: bytes, token: bytes | None,
                 name: str, user_name: str = "", protocol_version: int = PROTOCOL_VERSION) -> bytes:
    """The v12 HELLO shape: ..., protocolVersion, appId, installId, ..., nameLen/name,
    then userNameLen/userName.

    `protocol_version` (v12) defaults to this module's own PROTOCOL_VERSION —
    override it only to deliberately provoke HELLO_DENIED(PROTOCOL_MISMATCH),
    e.g. to test that a stale client is refused here rather than only once its
    (now-unreachable) declaration push fails to parse. It sits right after
    helloTag and is checked before anything else in the payload, by design
    (docs/companion-display-protocol.md's Session characteristic section) — a
    device building against a different protocol version could in principle
    lay out everything after it differently, so it is the one field a client
    can always rely on being read from the same offset.

    Both names are length-prefixed and may be empty, but the length byte is
    never optional — a v7-shaped HELLO (no userName field at all) is one field
    short of what the device parses and is rejected as malformed.
    """
    payload = bytes([SESS_HELLO]) + struct.pack("<H", hello_tag) + bytes([protocol_version]) + app_id + install_id
    payload += bytes([len(token) if token else 0]) + (token or b"")
    encoded_name = name.encode("utf-8")[:24]
    payload += bytes([len(encoded_name)]) + encoded_name
    encoded_user = user_name.encode("utf-8")[:24]
    payload += bytes([len(encoded_user)]) + encoded_user
    return payload


def parse_capabilities(raw: bytes, minimum_version: int = PROTOCOL_VERSION, who: str = "this script") -> dict:
    """The 23-byte capability block, readable before the handshake.

    `minimum_version` is the caller's floor, not necessarily this module's: the
    pusher only exercises the stable-since-v6 subset for most of what it sends
    and gates the newer fields individually, while the e2e harness asserts on
    v12 behaviour throughout and has no reason to run against anything older.

    v12 is a clean break rather than an additive version: a client that pushes a
    declaration with no content shape is refused whatever it can otherwise
    speak, so `flags & CAP_FLAG_SHAPE_AWARE` (or simply version >= 12) is worth
    reading before the first push rather than after the first refusal.
    """
    if len(raw) < 23:
        raise SystemExit(
            f"Capability characteristic is {len(raw)} bytes, expected 23. "
            "This device is running pre-v6 firmware; flash a v6+ build first."
        )
    caps = {
        "version": raw[0],
        "chars_wide": raw[1],
        "chars_high": raw[2],
        "max_text": struct.unpack_from("<H", raw, 3)[0],
        "flags": raw[5],
        "max_image": struct.unpack_from("<I", raw, 6)[0],
        "max_sessions": raw[10],
        "icon_w": raw[11],
        "icon_h": raw[12],
        "device_id": raw[13:17].hex(),
        "px_wide": struct.unpack_from("<H", raw, 17)[0],
        "px_high": struct.unpack_from("<H", raw, 19)[0],
        "max_content_id": raw[21],
        "gray_levels": raw[22],
        "raw": bytes(raw),
    }
    if caps["version"] < minimum_version:
        raise SystemExit(f"Device speaks protocol v{caps['version']}, {who} needs v{minimum_version}+.")
    return caps


@dataclass
class ButtonEvent:
    session_id: int
    button: int
    is_final: bool
    ticks: int  # hold duration in 100ms ticks since the initial press
    content_id: bytes

    @property
    def name(self) -> str:
        return BUTTON_NAMES.get(self.button, "?")

    @property
    def seconds(self) -> float:
        return self.ticks * 0.1


def decode_button_event(data: bytes) -> ButtonEvent | None:
    """None for a runt frame; the button characteristic's payload is >= 4 bytes."""
    if len(data) < 4:
        return None
    return ButtonEvent(
        session_id=data[0],
        button=data[1] & 0x0F,
        is_final=bool(data[1] & 0x80),
        ticks=struct.unpack_from("<H", data, 2)[0],
        content_id=bytes(data[4:]),
    )


@dataclass
class SessionNotification:
    """One decoded Session-characteristic notification.

    Only the fields that opcode actually carries are set; the rest stay None,
    so a caller that reads `note.push_id` on a HELLO_OK gets None rather than a
    plausible-looking byte from somewhere else in the frame.
    """

    opcode: int
    raw: bytes
    hello_tag: int | None = None
    session_id: int | None = None
    reason: int | None = None
    result: int | None = None
    push_id: int | None = None
    field_id: int | None = None
    seq: int | None = None
    token: bytes | None = None
    asset_tags: dict[int, bytes] | None = None
    revision: int | None = None
    count: int | None = None
    offset: int | None = None
    total: int | None = None
    entries: list[tuple[int, bool]] | None = None

    @property
    def is_hello_reply(self) -> bool:
        return self.opcode in (SESS_HELLO_OK, SESS_HELLO_PENDING, SESS_HELLO_DENIED)


def decode_session_notification(data: bytes) -> SessionNotification:
    data = bytes(data)
    opcode = data[0]
    note = SessionNotification(opcode=opcode, raw=data)

    if opcode in (SESS_HELLO_OK, SESS_HELLO_PENDING, SESS_HELLO_DENIED):
        # HELLO replies are correlated by helloTag, not sessionId — there is no
        # session yet to name, and two apps can have handshakes in flight on
        # one link at the same time.
        note.hello_tag = struct.unpack_from("<H", data, 1)[0]
        if opcode == SESS_HELLO_DENIED:
            note.reason = data[3]
        elif opcode == SESS_HELLO_OK:
            note.session_id = data[3]
            note.token = data[4:20]
            note.asset_tags = {}
            for i in range(data[20]):
                offset = 21 + i * 5
                note.asset_tags[data[offset]] = data[offset + 1 : offset + 5]
        return note

    note.session_id = data[1]
    if opcode == SESS_BACKGROUND:
        note.reason = data[2]
    elif opcode == SESS_ACQUIRE_DENIED:
        note.reason = data[2]
    elif opcode == SESS_ASSET_ACK:
        note.field_id = data[2]
        note.result = data[3]
        note.token = data[4:8]  # the 4-byte asset tag the device now holds
    elif opcode == SESS_RENDER_STATUS:
        note.result = data[2]
        # v11: the pushId the client put on the answered push's END, echoed
        # verbatim. For one day of the same release this byte was a field id
        # instead; nothing shipped against that shape.
        note.push_id = data[3]
    elif opcode == SESS_IMAGE_CHUNK_ACK:
        note.seq = struct.unpack_from("<H", data, 2)[0]
    elif opcode == SESS_FIELD_SEQ_GAP:
        note.field_id = data[2]
    elif opcode == SESS_LIST_STATE_AVAIL:
        note.revision = struct.unpack_from("<I", data, 2)[0]
        note.count = struct.unpack_from("<H", data, 6)[0]
    elif opcode == SESS_LIST_STATE:
        note.revision = struct.unpack_from("<I", data, 2)[0]
        note.offset = struct.unpack_from("<H", data, 6)[0]
        note.total = struct.unpack_from("<H", data, 8)[0]
        n = data[10]
        note.entries = [
            (struct.unpack_from("<H", data, 11 + i * 3)[0], bool(data[13 + i * 3])) for i in range(n)
        ]
    return note


# --------------------------------------------------------------------------- #
# Host-side identity and token storage
#
# A real app keeps these in the Keychain; a dev machine keeps them in one
# JSON file so the second run onward needs no on-device confirmation. Both
# scripts share the file, keyed by appId, because they deliberately present
# different app identities to the device.
# --------------------------------------------------------------------------- #

TOKEN_STORE = Path.home() / ".crosspoint_companion_tokens.json"


def load_store() -> dict:
    if TOKEN_STORE.exists():
        try:
            return json.loads(TOKEN_STORE.read_text())
        except (OSError, ValueError):
            pass
    return {}


def save_store(store: dict) -> None:
    TOKEN_STORE.write_text(json.dumps(store, indent=2))
    os.chmod(TOKEN_STORE, 0o600)


def install_id() -> bytes:
    """Persisted like a real app would: regenerating it makes a brand new peer."""
    store = load_store()
    if "install_id" not in store:
        store["install_id"] = os.urandom(16).hex()
        save_store(store)
    return bytes.fromhex(store["install_id"])


def stored_token(device_id: str) -> bytes | None:
    token = load_store().get("tokens", {}).get(device_id)
    return bytes.fromhex(token) if token else None


def remember_token(device_id: str, token: bytes) -> None:
    store = load_store()
    store.setdefault("tokens", {})[device_id] = token.hex()
    save_store(store)


# --------------------------------------------------------------------------- #
# Link and Session
# --------------------------------------------------------------------------- #


@dataclass
class HelloResult:
    ok: bool
    session_id: int = 0
    token: bytes | None = None
    asset_tags: dict[int, bytes] = dataclass_field(default_factory=dict)
    reason: int | None = None
    raw: bytes = b""

    @property
    def reason_text(self) -> str:
        return DENIED_REASONS.get(self.reason, f"unknown ({self.reason:#04x})")


class Link:
    """One BLE connection, carrying one or more Sessions.

    Notifications arrive per-link, not per-session — both apps sharing a phone's
    single link see every frame — so decode and routing live here and the
    Session objects only see what is addressed to them.
    """

    def __init__(self, client: BleakClient, caps: dict):
        self.client = client
        self.caps = caps
        self.loop = asyncio.get_running_loop()
        self.sessions: list[Session] = []
        # Every decoded notification, in arrival order. Cheap, and the only way
        # to assert on something *not* arriving (a pushId-0 push must produce no
        # RENDER_STATUS at all).
        self.notifications: list[SessionNotification] = []
        self.button_events: list[ButtonEvent] = []
        self.on_notification: Callable[[SessionNotification], None] | None = None
        self.on_button: Callable[[ButtonEvent], None] | None = None

        # bleak's mtu_size mirrors what CoreBluetooth actually negotiated for
        # this link (client.mtu_size == ATT MTU, the same value CompanionKit
        # derives maximumWriteValueLength(for:) from on a real iPhone) — not the
        # 185 the firmware merely requests. Minus 3 for ATT overhead, then minus
        # the CHUNK framing, same as CompanionKit's ContentFramer.
        self.att_payload = max(1, client.mtu_size - 3)

    def chunk_payload_size(self, field_id: int) -> int:
        overhead = CHUNK_HEADER_LEN + (CHUNK_SEQ_LEN if field_id in SEQ_CHUNK_FIELDS else 0)
        return max(1, self.att_payload - overhead)

    async def start_notify(self) -> None:
        await self.client.start_notify(SESSION_CHAR_UUID, self._handle_session)
        await self.client.start_notify(BUTTON_CHAR_UUID, self._handle_button)

    def attach(self, session: "Session") -> "Session":
        session._attach(self)
        self.sessions.append(session)
        return session

    # -- notification fan-out ----------------------------------------------- #

    def _handle_session(self, _sender, data: bytearray) -> None:
        note = decode_session_notification(data)
        self.notifications.append(note)
        if self.on_notification:
            self.on_notification(note)
        for session in self.sessions:
            if session._owns(note):
                session._dispatch(note)
                return

    def _handle_button(self, _sender, data: bytearray) -> None:
        event = decode_button_event(data)
        if event is None:
            return
        self.button_events.append(event)
        if self.on_button:
            self.on_button(event)

    def resolve(self, future: asyncio.Future | None, value) -> None:
        """Resolve a future from the BLE callback thread bleak notifies on."""
        if future and not future.done():
            self.loop.call_soon_threadsafe(future.set_result, value)


class Session:
    """One app's session: the handshake, and everything that needs a sessionId.

    Identity (appId/installId) outlives any one connection, so a Session is
    constructed once and re-attached to each new Link — that is what a
    reconnect-with-a-stored-token test, or an app that drops and redials, is
    actually doing.
    """

    def __init__(self, app_id: bytes, name: str, install_id_bytes: bytes | None = None,
                 user_name: str = "", token: bytes | None = None):
        self.app_id = app_id
        self.name = name
        self.install_id = install_id_bytes if install_id_bytes is not None else os.urandom(16)
        self.user_name = user_name
        self.token = token
        self.session_id = 0
        self.asset_tags: dict[int, bytes] = {}

        # Which fields' CHUNKs go over Write Without Response. Empty by
        # default: bleak has no cross-platform equivalent of CoreBluetooth's
        # canSendWriteWithoutResponse flow control, so an unthrottled WWR loop
        # can drop its own writes, and a Python client that values a push
        # landing over a push being fast is right to stay on Write. Only
        # SEQ_CHUNK_FIELDS belong in here — every other field has no sequence
        # number and depends on Write's link-layer ack for correctness.
        self.wwr_fields: tuple[int, ...] = ()

        self.link: Link | None = None
        self.hello_tag = 0
        self._hello_future: asyncio.Future | None = None
        self._pending_future: asyncio.Future | None = None
        self._acquire_future: asyncio.Future | None = None
        self._background_future: asyncio.Future | None = None
        self._asset_futures: dict[int, asyncio.Future] = {}
        self._render_futures: dict[int, asyncio.Future] = {}

        # Everything the device says that nothing is awaiting, kept for
        # after-the-fact assertions and for a caller's own logging.
        self.render_statuses: list[tuple[int, int]] = []  # (result, pushId)
        self.field_seq_gaps: list[int] = []  # field ids the device dropped
        self.chunk_acks: list[int] = []  # IMAGE_CHUNK_ACK seq numbers
        # v12: every LIST_STATE_AVAIL this session was sent, as (revision,
        # count). Recorded rather than only awaited because the interesting
        # assertions are about *when* one arrives and about one NOT arriving.
        self.list_state_avails: list[tuple[int, int]] = []
        self.on_list_state_avail: Callable[[int, int], None] | None = None
        self._list_state_futures: dict[int, asyncio.Future] = {}

        # Optional hooks, so a client can print without this module doing so.
        self.on_pending: Callable[[], None] | None = None
        self.on_background: Callable[[int], None] | None = None
        self.on_chunk_ack: Callable[[int], None] | None = None
        self.on_field_seq_gap: Callable[[int], None] | None = None

    # -- link binding -------------------------------------------------------- #

    def _attach(self, link: Link) -> None:
        self.link = link
        self.session_id = 0
        self._hello_future = None
        self._pending_future = None
        self._acquire_future = None
        self._background_future = None
        self._asset_futures.clear()
        self._render_futures.clear()
        self._list_state_futures.clear()

    @property
    def _client(self) -> BleakClient:
        assert self.link is not None, "Session is not attached to a Link"
        return self.link.client

    # -- routing ------------------------------------------------------------- #

    def _owns(self, note: SessionNotification) -> bool:
        if note.is_hello_reply:
            return note.hello_tag == self.hello_tag and self.hello_tag != 0
        return note.session_id == self.session_id and self.session_id != 0

    def _dispatch(self, note: SessionNotification) -> None:
        assert self.link is not None
        if note.opcode == SESS_HELLO_PENDING:
            if self.on_pending:
                self.on_pending()
            self.link.resolve(self._pending_future, True)
        elif note.opcode == SESS_HELLO_OK:
            self.link.resolve(
                self._hello_future,
                HelloResult(True, note.session_id, bytes(note.token), dict(note.asset_tags), raw=note.raw),
            )
        elif note.opcode == SESS_HELLO_DENIED:
            self.link.resolve(self._hello_future, HelloResult(False, reason=note.reason, raw=note.raw))
        elif note.opcode == SESS_FOREGROUND:
            self.link.resolve(self._acquire_future, ("foreground", note.session_id))
        elif note.opcode == SESS_ACQUIRE_DENIED:
            self.link.resolve(self._acquire_future, ("denied", note.reason))
        elif note.opcode == SESS_BACKGROUND:
            if self.on_background:
                self.on_background(note.reason)
            self.link.resolve(self._background_future, note.reason)
        elif note.opcode == SESS_ASSET_ACK:
            self.link.resolve(self._asset_futures.pop(note.field_id, None), (note.result, bytes(note.token)))
        elif note.opcode == SESS_RENDER_STATUS:
            self.render_statuses.append((note.result, note.push_id))
            self.link.resolve(self._render_futures.pop(note.push_id, None), note.result)
        elif note.opcode == SESS_IMAGE_CHUNK_ACK:
            self.chunk_acks.append(note.seq)
            if self.on_chunk_ack:
                self.on_chunk_ack(note.seq)
        elif note.opcode == SESS_FIELD_SEQ_GAP:
            self.field_seq_gaps.append(note.field_id)
            if self.on_field_seq_gap:
                self.on_field_seq_gap(note.field_id)
        elif note.opcode == SESS_LIST_STATE_AVAIL:
            self.list_state_avails.append((note.revision, note.count))
            if self.on_list_state_avail:
                self.on_list_state_avail(note.revision, note.count)
        elif note.opcode == SESS_LIST_STATE:
            # Keyed by offset, not FIFO: the device is stateless across a pull,
            # so a client is free to have several GETs outstanding, and a reply
            # is identified by the offset it echoes.
            self.link.resolve(self._list_state_futures.pop(note.offset, None), note)

    # -- handshake ----------------------------------------------------------- #

    async def send_hello(self, token: bytes | None = None, protocol_version: int = PROTOCOL_VERSION) -> None:
        """Arm the reply futures and write HELLO. Does not wait for the answer.

        Split from wait_hello() because the interesting case has something to do
        in between: an unknown peer's HELLO answers HELLO_PENDING and then
        nothing at all until somebody presses CONFIRM on the device, which is
        what the e2e harness is there to do.

        `protocol_version` defaults to this module's own version; pass a
        different value to provoke HELLO_DENIED(PROTOCOL_MISMATCH) -- see
        encode_hello()'s doc comment.
        """
        assert self.link is not None
        if token is not None:
            self.token = token
        self.hello_tag = int.from_bytes(os.urandom(2), "little") or 1
        self._hello_future = self.link.loop.create_future()
        self._pending_future = self.link.loop.create_future()
        payload = encode_hello(self.hello_tag, self.app_id, self.install_id, self.token, self.name, self.user_name,
                               protocol_version)
        await self._client.write_gatt_char(SESSION_CHAR_UUID, payload, response=True)

    async def wait_hello(self, timeout: float = 40.0) -> HelloResult:
        """Wait for HELLO_OK/HELLO_DENIED and adopt the session on success.

        The 40s default outlives the device's own ~30s pairing-prompt timeout,
        so an unattended run reports HELLO_DENIED(TIMEOUT) — what the device
        actually did — instead of this side giving up first and blaming itself.
        """
        result = await asyncio.wait_for(self._hello_future, timeout=timeout)
        if result.ok:
            self.session_id = result.session_id
            self.token = result.token
            self.asset_tags = result.asset_tags
        return result

    async def hello(self, token: bytes | None = None, timeout: float = 40.0) -> HelloResult:
        await self.send_hello(token)
        return await self.wait_hello(timeout)

    def pending_future(self) -> asyncio.Future:
        """Resolves when HELLO_PENDING arrives — i.e. the prompt is on screen.

        Only valid after send_hello(); HELLO_PENDING is not a terminal answer,
        so this and wait_hello() are both live at once.
        """
        return self._pending_future

    async def acquire(self, timeout: float = 10.0) -> tuple:
        """('foreground', sessionId) or ('denied', reason).

        ACQUIRE is asynchronous by design: the answer is a notification, not a
        write response, because the device may have to tear down another app's
        screen first.
        """
        assert self.link is not None
        self._acquire_future = self.link.loop.create_future()
        await self._client.write_gatt_char(SESSION_CHAR_UUID, bytes([SESS_ACQUIRE, self.session_id]), response=True)
        return await asyncio.wait_for(self._acquire_future, timeout=timeout)

    async def release(self) -> None:
        await self._client.write_gatt_char(SESSION_CHAR_UUID, bytes([SESS_RELEASE, self.session_id]), response=True)

    async def bye(self) -> None:
        await self._client.write_gatt_char(SESSION_CHAR_UUID, bytes([SESS_BYE, self.session_id]), response=True)

    # -- v12: ToDo List check-off sync-back ---------------------------------- #

    async def list_state_get(self, offset: int, timeout: float = 5.0) -> SessionNotification:
        """One LIST_STATE page starting at entry `offset`.

        `offset` past the end, an unknown session and a peer with no stored diff
        all answer n = 0. That is the terminator of a normal walk, not an error,
        and the device logs nothing for it -- which is what lets check_no_errors
        run over a test that deliberately over-reads.
        """
        assert self.link is not None
        # Hold the future locally rather than re-reading the dict after the
        # write. The notification handler *pops* the offset key when the reply
        # lands, and the reply can land during the awaited write_gatt_char --
        # a response=True write only completes once the device has ACKed, which
        # is ample time for a notification on the same link to arrive first.
        # Re-indexing the dict afterwards then raised KeyError instead of
        # returning the answer that had already arrived.
        pending = self.link.loop.create_future()
        self._list_state_futures[offset] = pending
        await self._client.write_gatt_char(
            SESSION_CHAR_UUID,
            bytes([SESS_LIST_STATE_GET, self.session_id]) + struct.pack("<H", offset),
            response=True,
        )
        return await asyncio.wait_for(pending, timeout=timeout)

    async def pull_list_state(self, timeout: float = 5.0) -> tuple[int, int, list[tuple[int, bool]], list[int]]:
        """Walk the whole diff: (revision, total, entries, page_lengths).

        `page_lengths` is the raw byte length of each notification, so a caller
        can assert the pull stayed inside the session characteristic's floor --
        the property that makes this design legal at all.
        """
        entries: list[tuple[int, bool]] = []
        page_lengths: list[int] = []
        revision, total = 0, 0
        offset = 0
        while True:
            note = await self.list_state_get(offset, timeout=timeout)
            page_lengths.append(len(note.raw))
            revision, total = note.revision, note.total
            if not note.entries:
                return revision, total, entries, page_lengths
            entries.extend(note.entries)
            offset += len(note.entries)

    def expect_background(self) -> asyncio.Future:
        """Arm before doing whatever should preempt this session."""
        assert self.link is not None
        self._background_future = self.link.loop.create_future()
        return self._background_future

    def expect_render(self, push_id: int) -> asyncio.Future:
        """Arm before the push whose RENDER_STATUS you intend to await.

        Keyed by pushId, which is exactly what v11 introduced it for: the
        device answers each push with the id that push chose, so two pushes in
        flight cannot have their answers confused. Arm this *before* pushing —
        the answer for a text batch can arrive ~2.2s later, but a batch
        discarded for a sequence gap is answered immediately.
        """
        assert self.link is not None
        future = self.link.loop.create_future()
        self._render_futures[push_id & 0xFF] = future
        return future

    # -- content ------------------------------------------------------------- #

    async def push_field(
        self,
        field_id: int,
        data: bytes,
        final: bool = False,
        push_id: int = 0,
        progress: Callable[[int, int], None] | None = None,
        seq_gap_at: int | None = None,
        max_chunks: int | None = None,
        chunk_response: bool | None = None,
    ) -> None:
        """One field: START, its CHUNKs, then END.

        `push_id` is v11's third END byte, echoed back on RENDER_STATUS. 0
        means "I am not awaiting an answer" and the device stays silent — the
        right default for most pushes, and a trap for anyone who awaits a
        render without setting one, since the wait can only ever time out.

        `chunk_response` defaults to Write With Response unless this field is
        in `wwr_fields` (see the attribute's comment) — even for the fields the
        protocol recommends pushing over Write Without Response (image since
        v9, title/body since v10). That recommendation is a throughput one and
        is explicitly "not enforced at the GATT level": the device parses the
        sequence number regardless of which ATT write type carried the CHUNK.
        The sequence numbers below are what the wire format requires; the write
        type is the caller's throughput trade.

        `seq_gap_at` deliberately skips one sequence number at that chunk
        index, which is how a test provokes the drop-detection path without
        needing an actually-lossy link. Only meaningful for SEQ_CHUNK_FIELDS.

        `max_chunks` stops after that many CHUNKs and sends END anyway,
        declaring more in START than it delivers. No real client wants this;
        it exists so a test can provoke the drop-detection path on a large
        field (an image is a couple of hundred chunks) without paying for the
        whole transfer first, since the device latches the gap the moment it
        sees it and ignores everything after.
        """
        assert self.link is not None
        if chunk_response is None:
            chunk_response = field_id not in self.wwr_fields
        field_byte = field_id | (FINAL_FLAG if final else 0)
        start = bytes([OP_START, field_byte, self.session_id]) + struct.pack("<I", len(data))
        await self._client.write_gatt_char(CONTENT_CHAR_UUID, start, response=True)

        has_seq = field_id in SEQ_CHUNK_FIELDS
        chunk_payload = self.link.chunk_payload_size(field_id)
        total = max(1, (len(data) + chunk_payload - 1) // chunk_payload)
        seq = 0
        for index, offset in enumerate(range(0, len(data), chunk_payload)):
            if max_chunks is not None and index >= max_chunks:
                break
            header = bytes([OP_CHUNK, self.session_id])
            if has_seq:
                if seq_gap_at is not None and index == seq_gap_at:
                    seq += 1  # the chunk the device will never see
                header += struct.pack("<H", seq)
                seq += 1
            await self._client.write_gatt_char(
                CONTENT_CHAR_UUID, header + data[offset : offset + chunk_payload], response=chunk_response
            )
            if progress:
                progress(index, total)

        await self._client.write_gatt_char(
            CONTENT_CHAR_UUID, bytes([OP_END, self.session_id, push_id & 0xFF]), response=True
        )

    async def push_asset(self, field_id: int, payload: bytes, timeout: float = 15.0) -> tuple[int, bytes]:
        """Push field 0x05/0x06 and wait for its ASSET_ACK. Returns (result, tag).

        Assets are accepted from any live session, foreground or not — they have
        to be, since ACQUIRE is gated on a stored UI declaration and enrollment
        would otherwise deadlock.
        """
        assert self.link is not None
        future = self.link.loop.create_future()
        self._asset_futures[field_id] = future
        await self.push_field(field_id, payload)
        return await asyncio.wait_for(future, timeout=timeout)

    async def push_image(
        self,
        raw_bitmap: bytes,
        push_id: int = 1,
        timeout: float = 180.0,
        progress: Callable[[int, int], None] | None = None,
    ) -> int:
        """Push field 0x04 and wait for RENDER_STATUS. Returns the result byte.

        The timeout is generous because the answer only comes once the panel
        has actually developed the image — decode plus the two-pass grayscale
        settle, not merely the wire transfer.
        """
        future = self.expect_render(push_id)
        await self.push_field(FIELD_IMAGE, raw_bitmap, final=True, push_id=push_id, progress=progress)
        return await asyncio.wait_for(future, timeout=timeout)

    async def push_list_doc(self, doc: bytes, push_id: int = 1, timeout: float = 30.0) -> int:
        """Push field 0x08 and wait for RENDER_STATUS. Returns the result byte.

        A list document is CONTENT, answered by RENDER_STATUS like title/body/
        image -- not an asset like the UI declaration/icon, which answer
        ASSET_ACK (see `push_asset`). Unlike `push_image` there is no panel
        settle to wait out (no grayscale two-pass here), so the default
        timeout is much shorter; pass a larger one for a document large enough
        that the transfer itself, not the render, dominates.
        """
        future = self.expect_render(push_id)
        await self.push_field(FIELD_LIST_DOC, doc, final=True, push_id=push_id)
        return await asyncio.wait_for(future, timeout=timeout)

    async def set_tag(self, tag_id: int, state: int) -> None:
        """State-only change to one declared tag, without re-pushing content."""
        await self._client.write_gatt_char(
            STATUS_CHAR_UUID, bytes([self.session_id, tag_id, state]), response=True
        )


# --------------------------------------------------------------------------- #
# Raw packed 2bpp helpers (field 0x04's wire format, v7+)
# --------------------------------------------------------------------------- #


def raw_image_length(px_wide: int, px_high: int) -> int:
    """Exactly what field 0x04 must be: ceil(width/4) bytes per row, no header."""
    return ((px_wide + 3) // 4) * px_high


def pack_2bpp(levels: Sequence[int], width: int, height: int) -> bytes:
    """Pack per-pixel levels 0-3 (row-major) into field 0x04's layout.

    bytesPerRow = ceil(width/4), 4 samples per byte, MSB-first (sample 0 in
    bits 7-6), rows top to bottom. Each 2-bit sample already *is* the final
    display level — 0 black, 3 white — with no gray-level math device-side.
    """
    row_bytes = (width + 3) // 4
    out = bytearray(row_bytes * height)
    for y in range(height):
        base = y * row_bytes
        for x in range(width):
            shift = 6 - (x % 4) * 2
            out[base + x // 4] |= (levels[y * width + x] & 0x03) << shift
    return bytes(out)


def new_app_id() -> bytes:
    """A fresh random appId, for a caller that wants a throwaway identity."""
    return uuid.uuid4().bytes
