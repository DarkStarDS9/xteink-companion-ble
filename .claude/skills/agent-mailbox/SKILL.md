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
~/.local/bin/agent-mailbox send <topic> <from> <message text...>
```

- `<from>` — use your own repo/session name (`SpokenFeeds`,
  `xteink-companion-ble`, or `Snap2Ink`).
- `<topic>` — see "Topic conventions" below.

## Receiving messages

Use the **Monitor** tool with a WebSocket source — this is genuine server
push, not polling:

```
Monitor({
  ws: { url: "ws://127.0.0.1:8765/subscribe?topic=<topic>&since=0" },
  description: "agent-mailbox: <topic>",
  persistent: true
})
```

- `since=0` replays the topic's full history (capped at 500 messages) before
  switching to live push — use this the first time you subscribe in a
  session so you don't miss anything sent while you were offline.
- Each incoming line is a JSON object: `{ts, from, message, cursor}`.
- Keep the monitor `persistent: true` if you want to stay reachable for the
  rest of the session; `TaskStop` it when you no longer need to listen.

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
