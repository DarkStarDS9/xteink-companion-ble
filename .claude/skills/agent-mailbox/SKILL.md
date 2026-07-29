---
name: agent-mailbox
description: Send or receive messages to/from Claude Code sessions running in sibling repos (SpokenFeeds, xteink-companion-ble, Snap2Ink) via the local agent-mailbox daemon. Use when the user asks you to notify, coordinate with, or message another repo's session, or when you're told to watch for a message from one.
---

## What this is

A tiny local-only pub/sub daemon (Node, no deps) that lets Claude Code sessions
running in different repos on this machine talk to each other. It's
independent of any one repo — each `claude remote-control` session for
SpokenFeeds / xteink-companion-ble / Snap2Ink runs in its own launchd job with
its own working directory, so subagents in one can't normally see another.
This daemon is the bridge.

- Runs under launchd as `com.local.agent-mailbox`, always up, listens on
  `127.0.0.1:8765` only (not exposed off-machine, no auth — trust model is
  single-user/single-machine).
- Source: `~/.local/share/agent-mailbox/server.js`. Plist:
  `~/Library/LaunchAgents/com.local.agent-mailbox.plist`.
- Messages persist to `~/.agent-mailbox/<topic>.jsonl` (last 500 per topic).

## Sending a message

```bash
~/.local/bin/agent-mailbox send [--to <recipient>] <topic> <from> <message text...>
```

- `<from>` — use your own repo/session name (`SpokenFeeds`,
  `xteink-companion-ble`, or `Snap2Ink`), or a more specific per-agent name
  when several agents share a topic (see "1:1 vs. group messaging" below).
- `--to <recipient>` — optional. Addresses the message at one specific
  subscriber's `self` name instead of everyone on the topic. Omit it to
  broadcast to all subscribers.
- `<topic>` — see "Topic conventions" below.

## Receiving messages

Use the **Monitor** tool with a WebSocket source — this is genuine server
push, not polling. Always pass `self=<your name>` so the daemon can filter
correctly for you (see below):

```
Monitor({
  ws: { url: "ws://127.0.0.1:8765/subscribe?topic=<topic>&since=0&self=<your-name>" },
  description: "agent-mailbox: <topic>",
  persistent: true
})
```

- `since=0` replays the topic's full history (capped at 500 messages) before
  switching to live push — use this the first time you subscribe in a
  session so you don't miss anything sent while you were offline.
- `self=<your-name>` must match the `<from>` you send with. The daemon uses
  it for two things:
  - **No self-echo:** messages you sent yourself are never delivered back to
    a socket whose `self` matches their `from` — saves tokens, no more
    seeing your own message appear in your own Monitor feed.
  - **Direct-message filtering:** if a message was sent with `--to X`, only
    the socket subscribed with `self=X` receives it; every other subscriber
    on the topic doesn't see it at all (filtered server-side, not just
    client-side, so it costs no tokens for bystanders).
- Omitting `self` still works (backward compatible) but you'll see your own
  messages echoed back, and you won't receive any `--to`-addressed message
  (server can't know who you are, so it excludes you from targeted
  deliveries).
- Each incoming line is a JSON object: `{ts, from, message, cursor}`, plus
  `to` when the message was directed at a specific recipient.
- Keep the monitor `persistent: true` if you want to stay reachable for the
  rest of the session; `TaskStop` it when you no longer need to listen.

## Announcing yourself when you join a topic

There's no server-side presence tracking — the daemon doesn't know who's
subscribed or notify anyone when a new socket connects. If you're joining a
topic where other agents (or a coordinator) are already listening, **broadcast
a join message right after subscribing** so they know you're live and what
you do:

```bash
~/.local/bin/agent-mailbox send <topic> <your-self-name> "joined as <role> — <one-line purpose>"
```

Example: `agent-mailbox send xteink-firmware-release release-worker-2 "joined as release-worker-2 — flashing X3 units, awaiting build"`.

- Do this once, right after your Monitor subscription is up (so you don't
  miss the replies) — not on every message.
- If you're the coordinator and want to know who's already on a topic before
  you've sent anything yourself, scan the topic's history file instead of
  guessing: `tail -n 500 ~/.agent-mailbox/<topic>.jsonl | grep '"joined as'`.
  This is best-effort (an agent that joined and later exited without saying
  so will still show up) — treat it as "who has announced themselves," not
  "who is definitely still alive."
- Keep role names stable and descriptive within one coordination thread
  (e.g. `release-coord`, `release-worker-1`) so join messages and later
  `--to`-addressed traffic use the same identity.

## 1:1 vs. group messaging

For a coordinator working with multiple agents on one topic: give each agent
a distinct `self`/`from` name (e.g. the topic name plus a role suffix, like
`release-coord` and `release-worker-1`). Default to `--to <name>` for
routine 1:1 traffic (status reports back to the coordinator, task hand-offs)
so uninvolved agents aren't woken up for messages that don't concern them.
Drop `--to` only for announcements genuinely meant for the whole group.

## Topic conventions

- Default/repo-wide topic = the repo name (`SpokenFeeds`,
  `xteink-companion-ble`, `Snap2Ink`) — use this for messages meant for
  "whoever is working in that repo right now."
- Ad-hoc topics (e.g. `xteink-firmware-release`) are for a specific
  coordination thread across repos. There's no discovery mechanism — the
  user (or a message on a well-known topic) has to tell you what topic name
  to use. Don't invent a topic name and expect the other side to already be
  listening on it.

## Health check

```bash
curl -sS http://127.0.0.1:8765/health
```

If this fails, the daemon isn't running —
`launchctl print gui/$(id -u)/com.local.agent-mailbox` to check status;
`launchctl bootout` + `launchctl bootstrap` the plist above to restart it
(prefer this over `launchctl kickstart -k`, which has been observed to hang
on this machine).
