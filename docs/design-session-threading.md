# Design: Session Threading in Matrix/Telegram Bridges

## Overview

A nash "session" is a sequence of queries within one react loop progression.
Each session should appear as a **thread** in the chat platform: the initial
query is the **thread root**, and all follow-up queries/results within the
same session are **replies in that thread**.

This applies to three sources of activity:

| Source | Thread root shows | Replies contain |
|--------|------------------|-----------------|
| **Bridge-originated** | User's query (verbatim) | Follow-up queries + results |
| **TUI-originated** | User's query (forwarded from TUI) | Result (as reply) |
| **Agent** | `[agent XXX executed]` | Result (as reply) |

## Current State

### What exists today

- **Telegram**: Forum topics map to workspaces via `thread_id` (topic-level
  routing).  All `tg_api_send_*` functions accept `thread_id` for topic
  routing.  No `reply_to_message_id` support -- messages within a topic
  are flat.

- **Matrix**: Rooms map to workspaces via `room_map`.  `mx_api_send_*`
  functions accept `room_id` for room-level routing.  Reply detection
  (`m.relates_to` / `m.in_reply_to`) exists for incoming messages but
  outgoing messages have no threading support.  `mx_api_send_message_inner()`
  parses `event_id` from the response but discards it.

- **Outbox protocol**: Result files carry `X-Route-Token` (room_id or
  thread_id for workspace routing), `X-Workspace`, `X-User-Query` headers.
  Only `result_*`, `ask_*`, `status_*` message types exist.

- **TUI -> bridge**: `route_to_outbox()` forwards only the **result** text
  (not the query) to the outbox when a daemon is running.

- **Agent -> bridge**: `agent_execute()` writes result with
  `[workspace: X] [agent: Y]` prefix via `mailbox_write_result_routed()`.

### What's missing

1. Send functions return `int` (0/-1), not message/event IDs -- no way to
   reference a sent message for threading.
2. No `reply_to_message_id` support in Telegram sends.
3. No `m.relates_to` / `m.thread` support in Matrix sends.
4. No "session thread" concept -- each outbox message is independent.
5. TUI-originated queries are not forwarded to the bridge at all (only
   results).
6. Agent mode sends full result text, not the compact notification the
   user wants.

## Design

### 1. Thread-aware send functions

#### Matrix

Change `mx_api_send_message_inner()` to:
- Accept optional `const char *thread_event_id` parameter.
- When non-NULL, add `m.relates_to` to the JSON body:
  ```json
  "m.relates_to": {
    "rel_type": "m.thread",
    "event_id": "<thread_event_id>",
    "m.in_reply_to": {
      "event_id": "<thread_event_id>"
    }
  }
  ```
- Return `char *event_id` (caller frees) instead of `int`.  NULL = error.

New wrapper signatures:
```c
char *mx_api_send_message_threaded(matrix_ctx_t *ctx, const char *room_id,
                                   const char *text, const char *html,
                                   const char *thread_event_id);
```

Existing wrappers (`mx_api_send_message`, `mx_api_send_to_room`,
`mx_api_send_markdown_to`) remain unchanged (pass NULL for thread, return
int via event_id != NULL check).

#### Telegram

Add `reply_to_message_id` parameter to the inner send function
`tg_api_send_message_inner()`:
```c
static long long tg_api_send_message_inner(telegram_ctx_t *ctx,
    const char *text, const char *parse_mode,
    long long thread_id, long long reply_to_message_id);
```

Return the sent message's `message_id` (from API response `result.message_id`)
instead of `int`.  Returns -1 on error.

### 2. Session thread tracking

Both bridge contexts gain a session-thread map that tracks the active
thread root per workspace session:

```c
/* Session thread: tracks the thread root message for the current session */
#define MX_MAX_SESSION_THREADS 32
typedef struct {
    char  room_id[256];          /* which room */
    char  thread_event_id[256];  /* event_id of the thread root message */
} mx_session_thread_t;
```

```c
#define TG_MAX_SESSION_THREADS 32
typedef struct {
    long long thread_id;         /* which forum topic */
    long long root_message_id;   /* message_id of the thread root */
} tg_session_thread_t;
```

These are stored in `matrix_ctx_t` / `telegram_ctx_t` and cleared on
session reset (`cmd_new`).

### 3. Bridge-originated session flow

**Current flow** (flat):
```
User sends message -> bridge writes task_* -> daemon processes -> 
bridge sends result as flat message
```

**New flow** (threaded):
```
User sends NEW message (not a reply):
  1. Bridge writes cmd_new (session reset)
  2. Bridge writes task_*
  3. Bridge sends ack ("Processing...") -- this becomes the thread root
  4. Bridge stores ack's message_id/event_id as session thread root
  5. Daemon processes, writes result_* to outbox
  6. Bridge sends result as REPLY to thread root

User sends REPLY to bot message (follow-up):
  1. Bridge writes task_* (no cmd_new)
  2. Bridge sends ack ("Continuing...") as REPLY to existing thread root
  3. Daemon processes, writes result_* to outbox
  4. Bridge sends result as REPLY to thread root
```

The thread root is the **first acknowledgment message** of a new session.
All subsequent messages (results, asks, status, follow-up acks) are
replies to this root.

### 4. Outbox protocol extensions

#### New message type: `query_*`

A new outbox message type for TUI/agent-originated activity:

```
query_{id}
---
X-Workspace: rh
X-Source: tui|agent
X-Agent-Name: daily-report     (only for agent source)
X-Thread-Action: root|reply    (root = start new thread, reply = reply to existing)
---
actual query text (or "[agent XXX executed]" for agents)
```

When the bridge processes a `query_*` file:
- **root**: Send as a new message, store its message_id/event_id as
  the session thread root for this workspace.
- **reply**: Send as a reply to the existing session thread root.

#### Extended `result_*` with thread semantics

Result files gain a `X-Thread-Action: reply` header (always reply to
the current session thread root).  If no session thread exists, send
as a flat message (backward compat).

### 5. TUI -> bridge forwarding

Extend `route_to_outbox()` to also forward the **query text** before
the result.  New function:

```c
static void route_query_to_outbox(const char *nash_dir, const char *query,
                                  const char *ws_name, const char *agent_name,
                                  int is_new_session);
```

Called from:
- **Headless `-p` mode** (main.c ~L1469): Before `react_run()`, call
  `route_query_to_outbox(nash_dir, query, ws_name, NULL, 1)` (always
  new session).
- **TUI interactive** (main.c ~L1675): When inference starts, call
  `route_query_to_outbox(nash_dir, query, ws_name, NULL, is_first_query)`.
  `is_first_query` = true when `react_loop == 0` or after user typed
  a non-reply query.
- **TUI agent/playbook** (main.c ~L1590): Call
  `route_query_to_outbox(nash_dir, "[agent XXX executed]", ws_name, agent_name, 1)`.

This writes a `query_*` file to the outbox with appropriate headers.
The bridge picks it up and sends it to the chat platform, establishing
the thread root.

### 6. Agent mode specifics

When an agent executes (via daemon `agent_run_due()` or TUI playbook):

1. **Thread root**: `[agent daily-report executed]` -- posted to the
   workspace's room/topic.  This is a compact notification, not the
   full query/playbook content.

2. **Result**: Posted as a reply to the thread root.  Contains the
   actual agent output (which may be long).

Implementation in `agent_execute()` (agents.c ~L727):
```c
/* Write query notification first */
char query_id[256];
snprintf(query_id, sizeof(query_id), "agentq_%s", a->id);
mailbox_write_query(mailbox_dir, query_id, "[agent executed]",
                    a->workspace_name, a->id, 1 /* new thread */);

/* Then write result as reply */
mailbox_write_result_routed(mailbox_dir, task_id,
                            pargs.result_text, NULL,
                            a->workspace_name, NULL);
```

### 7. Session reset semantics

When `cmd_new` is processed (session reset):
- Clear the session thread root for all workspaces.
- The next message starts a new thread.

When a new standalone message arrives in the bridge:
- Session reset fires (existing behavior).
- The acknowledgment message becomes the new thread root.

### 8. Platform-specific threading

#### Matrix threading
- Uses [MSC3440 threads](https://spec.matrix.org/v1.11/client-server-api/#threading):
  `m.relates_to.rel_type = "m.thread"` + `event_id` of thread root.
- All messages in a session reference the SAME thread root event_id
  (flat thread, not nested).
- Clients (Element) render these as collapsed threads.

#### Telegram threading
- Within a forum topic: use `reply_to_message_id` parameter in
  `sendMessage` to create reply chains.
- Telegram renders these as reply threads within the topic.
- `message_thread_id` continues to be used for topic routing (orthogonal
  to reply threading).

### 9. Data flow summary

```
TUI User types query
  |
  v
route_query_to_outbox() --> outbox/query_{id}  (X-Thread-Action: root)
  |                              |
  v                              v
react_run() processes      Bridge thread picks up query_*:
  |                          - Sends to room/topic
  v                          - Stores message_id as thread root
route_to_outbox() -------> outbox/result_{id}  (X-Thread-Action: reply)
                                 |
                                 v
                           Bridge thread picks up result_*:
                             - Sends as REPLY to thread root
```

```
Chat User sends message
  |
  v
Bridge receives via /sync or getUpdates
  |
  v
Bridge sends ack ("Processing...")  <-- THIS is the thread root
  |
  v
Bridge writes task_* to inbox
  |
  v
Daemon processes via react_run()
  |
  v
Daemon writes result_* to outbox
  |
  v
Bridge sends result as REPLY to thread root
```

```
Agent scheduled execution
  |
  v
agent_execute() runs playbook
  |
  v
Write query_* "[agent XXX executed]"  --> Bridge sends as thread root
  |
  v
Write result_*  --> Bridge sends as REPLY to thread root
```

## Files to modify

| File | Changes |
|------|---------|
| `src/matrix.h` | Add `mx_session_thread_t`, session thread array to `matrix_ctx_t` |
| `src/matrix.c` | Thread-aware sends, session thread tracking, `query_*` processing |
| `src/telegram.h` | Add `tg_session_thread_t`, session thread array to `telegram_ctx_t` |
| `src/telegram.c` | `reply_to_message_id` support, session thread tracking, `query_*` processing |
| `src/mailbox.h` | Add `mailbox_write_query()` declaration, `X-Thread-Action` / `X-Source` / `X-Agent-Name` headers |
| `src/mailbox.c` | Implement `mailbox_write_query()`, extend header parsing |
| `src/main.c` | Add `route_query_to_outbox()`, call it before react_run in TUI/headless paths |
| `src/agents.c` | Write `query_*` notification before result in `agent_execute()` |

## Backward compatibility

- Outbox files without `X-Thread-Action` headers are sent as flat
  messages (existing behavior).
- Bridges that don't support threading (older code) ignore the new
  header and function normally.
- `route_to_outbox()` continues to work unchanged -- `route_query_to_outbox()`
  is additive.

## Open questions

1. **Long messages split across multiple sends**: When a result is split
   into multiple chunks (>4096 chars), should all chunks be replies to
   the thread root, or should each chunk reply to the previous chunk?
   Recommendation: all reply to thread root (flat thread).

2. **user_ask in threads**: When the bot asks a clarifying question
   (`ask_*`), it should be sent as a reply to the thread root so it
   appears in context.  The user's answer (reply to the ask message)
   would need to be detected as an answer regardless of whether it's
   a reply to the thread root or the ask message specifically.

3. **Multiple concurrent sessions**: If two workspaces have active
   sessions simultaneously, each workspace maintains its own thread
   root independently (already supported by per-workspace routing).
