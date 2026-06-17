#!/usr/bin/env python3
"""
nash-telegram-bridge.py — Telegram bridge for nash's file-based mailbox.

Watches outbox/ for agent messages and sends them as Telegram messages.
Uses long polling (getUpdates) for near-instant replies from inbox/.
Pure Python3 stdlib — no pip dependencies.

Usage:
  nash-telegram-bridge.py run              Main loop (outbox watcher + Telegram poller)
  nash-telegram-bridge.py test             Test bot connectivity, send test message
  nash-telegram-bridge.py send FILE        Send a specific outbox file as Telegram message
  nash-telegram-bridge.py check            One-shot: check for new Telegram messages
  nash-telegram-bridge.py setup            Interactive config setup

Config: ~/.nash/telegram.conf (or $NASH_TELEGRAM_CONF)
  bot_token = 123456:ABC-DEF1234ghIkl-zyx57W2v1u123ew11
  chat_id = 987654321
  poll_interval = 1
"""

import sys
import os
import re
import time
import json
import signal
import logging
import urllib.request
import urllib.error
import urllib.parse
from pathlib import Path

# ── Logging ──────────────────────────────────────────────

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("telegram-bridge")

# ── Constants ────────────────────────────────────────────

TELEGRAM_API = "https://api.telegram.org/bot{token}/{method}"
MAX_MESSAGE_LEN = 4096
LONG_POLL_TIMEOUT = 30  # seconds for getUpdates long polling

# ── Config ───────────────────────────────────────────────

DEFAULT_CONFIG = {
    "bot_token": "",
    "chat_id": "",
    "poll_interval": "1",
}


def load_config(path=None):
    """Load key=value config file. Lines starting with # are comments."""
    if path is None:
        path = os.environ.get("NASH_TELEGRAM_CONF",
                              os.path.expanduser("~/.nash/telegram.conf"))
    config = dict(DEFAULT_CONFIG)
    if not os.path.exists(path):
        return None, path
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if "=" not in line:
                continue
            key, val = line.split("=", 1)
            config[key.strip()] = val.strip()
    return config, path


def validate_config(config):
    """Check required fields are present."""
    missing = []
    for key in ("bot_token", "chat_id"):
        if not config.get(key):
            missing.append(key)
    if missing:
        log.error("Missing config keys: %s", ", ".join(missing))
        return False
    return True


# ── Mailbox paths ────────────────────────────────────────

def get_mailbox_dir():
    return os.environ.get("NASH_MAILBOX_DIR",
                          os.path.expanduser("~/.nash/mailbox"))


def ensure_dirs(mailbox_dir):
    """Create mailbox directories if needed."""
    for sub in ("inbox", "outbox", "outbox/.sent"):
        os.makedirs(os.path.join(mailbox_dir, sub), exist_ok=True)


# ── File helpers ─────────────────────────────────────────

def read_file(path):
    """Read file, strip trailing whitespace. Returns None on error."""
    try:
        with open(path) as f:
            return f.read().rstrip()
    except (OSError, IOError):
        return None


def atomic_write(path, content):
    """Write content atomically via tmp+rename."""
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        f.write(content)
        if not content.endswith("\n"):
            f.write("\n")
    os.rename(tmp, path)


def parse_outbox_file(filename):
    """Parse outbox filename → (type, id).
    Returns ('ask','abc123'), ('result','abc123'), ('status','abc123'), or None."""
    for prefix in ("ask_", "result_", "status_"):
        if filename.startswith(prefix):
            msg_id = filename[len(prefix):]
            msg_type = prefix.rstrip("_")
            return msg_type, msg_id
    return None


# ── Telegram API ─────────────────────────────────────────

def tg_api(token, method, params=None, timeout=30):
    """Call Telegram Bot API. Returns parsed JSON or None on error."""
    url = TELEGRAM_API.format(token=token, method=method)
    if params:
        data = json.dumps(params).encode("utf-8")
        req = urllib.request.Request(
            url, data=data,
            headers={"Content-Type": "application/json"},
        )
    else:
        req = urllib.request.Request(url)

    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = resp.read().decode("utf-8")
            return json.loads(body)
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", errors="replace")
        log.error("Telegram API %s HTTP %d: %s", method, e.code, body[:200])
        return None
    except urllib.error.URLError as e:
        log.error("Telegram API %s network error: %s", method, e.reason)
        return None
    except Exception as e:
        log.error("Telegram API %s error: %s", method, e)
        return None


def tg_send_message(token, chat_id, text, reply_to=None):
    """Send a message. Splits long messages. Returns message_id or None."""
    # Split into chunks if too long
    chunks = split_message(text)
    last_msg_id = None

    for chunk in chunks:
        params = {
            "chat_id": chat_id,
            "text": chunk,
            "parse_mode": "HTML",
            "disable_web_page_preview": True,
        }
        if reply_to and last_msg_id is None:
            params["reply_to_message_id"] = reply_to

        result = tg_api(token, "sendMessage", params)
        if result and result.get("ok"):
            last_msg_id = result["result"]["message_id"]
        else:
            # Retry without HTML parse_mode in case of formatting errors
            params.pop("parse_mode", None)
            result = tg_api(token, "sendMessage", params)
            if result and result.get("ok"):
                last_msg_id = result["result"]["message_id"]
            else:
                log.error("Failed to send message chunk")
                return None

    return last_msg_id


def tg_get_updates(token, offset=None, timeout=LONG_POLL_TIMEOUT):
    """Get updates via long polling. Returns list of updates or empty list."""
    params = {"timeout": timeout}
    if offset is not None:
        params["offset"] = offset

    result = tg_api(token, "getUpdates", params,
                     timeout=timeout + 10)  # HTTP timeout > long poll timeout
    if result and result.get("ok"):
        return result.get("result", [])
    return []


def tg_get_me(token):
    """Get bot info. Returns result dict or None."""
    result = tg_api(token, "getMe")
    if result and result.get("ok"):
        return result["result"]
    return None


def split_message(text, max_len=MAX_MESSAGE_LEN):
    """Split text into chunks that fit Telegram's message limit."""
    if len(text) <= max_len:
        return [text]

    chunks = []
    while text:
        if len(text) <= max_len:
            chunks.append(text)
            break

        # Try to split at a newline
        split_at = text.rfind("\n", 0, max_len)
        if split_at < max_len // 2:
            # No good newline break, split at space
            split_at = text.rfind(" ", 0, max_len)
        if split_at < max_len // 2:
            # No good break point, hard split
            split_at = max_len

        chunks.append(text[:split_at])
        text = text[split_at:].lstrip("\n")

    return chunks


# ── Message formatting ───────────────────────────────────

def escape_html(text):
    """Escape HTML special characters for Telegram."""
    return (text
            .replace("&", "&amp;")
            .replace("<", "&lt;")
            .replace(">", "&gt;"))


def format_outbox_message(msg_type, msg_id, body, original_query=None):
    """Format an outbox message for Telegram."""
    escaped = escape_html(body)

    if msg_type == "ask":
        return f"❓ <b>Question</b> <code>[{msg_id}]</code>\n\n{escaped}\n\n<i>Reply to this message to answer.</i>"
    elif msg_type == "result":
        header = f"📋 <b>Result</b> <code>[{msg_id}]</code>"
        if original_query:
            # Truncate long queries for the header
            q_preview = original_query if len(original_query) <= 120 else original_query[:120] + "…"
            header += f"\n🔎 <i>{escape_html(q_preview)}</i>"
        return f"{header}\n\n{escaped}"
    elif msg_type == "status":
        return f"ℹ️ {escaped}"
    else:
        return escaped


# ── Ask ID ↔ Message ID mapping ─────────────────────────

class MessageMap:
    """Persistent mapping between Telegram message IDs and ask IDs.
    Stored as a simple file so it survives restarts."""

    def __init__(self, mailbox_dir):
        self.path = os.path.join(mailbox_dir, ".tg_message_map")
        self.msg_to_ask = {}   # telegram_message_id → ask_id
        self.ask_to_msg = {}   # ask_id → telegram_message_id
        self._load()

    def _load(self):
        """Load mapping from file."""
        if not os.path.exists(self.path):
            return
        try:
            with open(self.path) as f:
                for line in f:
                    line = line.strip()
                    if not line or line.startswith("#"):
                        continue
                    parts = line.split(" ", 1)
                    if len(parts) == 2:
                        msg_id_str, ask_id = parts
                        try:
                            msg_id = int(msg_id_str)
                            self.msg_to_ask[msg_id] = ask_id
                            self.ask_to_msg[ask_id] = msg_id
                        except ValueError:
                            pass
        except (OSError, IOError):
            pass

    def _save(self):
        """Save mapping to file."""
        try:
            with open(self.path, "w") as f:
                f.write("# telegram_message_id ask_id\n")
                for msg_id, ask_id in self.msg_to_ask.items():
                    f.write(f"{msg_id} {ask_id}\n")
        except (OSError, IOError) as e:
            log.error("Failed to save message map: %s", e)

    def add(self, telegram_msg_id, ask_id):
        """Record a mapping."""
        self.msg_to_ask[telegram_msg_id] = ask_id
        self.ask_to_msg[ask_id] = telegram_msg_id
        self._save()

    def get_ask_id(self, telegram_msg_id):
        """Look up ask_id by Telegram message ID."""
        return self.msg_to_ask.get(telegram_msg_id)

    def remove_ask(self, ask_id):
        """Remove a mapping by ask_id (after answer received)."""
        msg_id = self.ask_to_msg.pop(ask_id, None)
        if msg_id is not None:
            self.msg_to_ask.pop(msg_id, None)
            self._save()

    def cleanup(self, max_age_entries=1000):
        """Keep only the last N entries to prevent unbounded growth."""
        if len(self.msg_to_ask) > max_age_entries:
            # Keep newest entries (highest message IDs)
            sorted_ids = sorted(self.msg_to_ask.keys())
            to_remove = sorted_ids[:len(sorted_ids) - max_age_entries]
            for mid in to_remove:
                aid = self.msg_to_ask.pop(mid, None)
                if aid:
                    self.ask_to_msg.pop(aid, None)
            self._save()


# ── Task ID → Query text mapping ────────────────────────

class QueryMap:
    """Persistent mapping between task IDs and original query text.
    Used to include the original query when presenting results back to user."""

    def __init__(self, mailbox_dir):
        self.path = os.path.join(mailbox_dir, ".tg_query_map")
        self.queries = {}  # task_id → query_text
        self._load()

    def _load(self):
        """Load mapping from file (tab-separated: task_id<TAB>query)."""
        if not os.path.exists(self.path):
            return
        try:
            with open(self.path) as f:
                for line in f:
                    line = line.rstrip("\n")
                    if not line or line.startswith("#"):
                        continue
                    parts = line.split("\t", 1)
                    if len(parts) == 2:
                        self.queries[parts[0]] = parts[1]
        except (OSError, IOError):
            pass

    def _save(self):
        """Save mapping to file."""
        try:
            with open(self.path, "w") as f:
                f.write("# task_id<TAB>query_text\n")
                for task_id, query in self.queries.items():
                    # Replace newlines in query to keep one-line-per-entry
                    safe_query = query.replace("\n", " ").replace("\t", " ")
                    f.write(f"{task_id}\t{safe_query}\n")
        except (OSError, IOError) as e:
            log.error("Failed to save query map: %s", e)

    def add(self, task_id, query_text):
        """Store the original query for a task."""
        self.queries[task_id] = query_text
        self._save()

    def get(self, task_id):
        """Look up original query by task_id. Returns None if not found."""
        return self.queries.get(task_id)

    def remove(self, task_id):
        """Remove a mapping (after result delivered)."""
        if task_id in self.queries:
            del self.queries[task_id]
            self._save()

    def cleanup(self, max_entries=1000):
        """Keep only the last N entries to prevent unbounded growth."""
        if len(self.queries) > max_entries:
            keys = list(self.queries.keys())
            for k in keys[:len(keys) - max_entries]:
                del self.queries[k]
            self._save()


# ── Outbox watcher ───────────────────────────────────────

class OutboxWatcher:
    """Watch outbox/ for new files and send them as Telegram messages."""

    def __init__(self, config, mailbox_dir, msg_map, query_map=None):
        self.config = config
        self.token = config["bot_token"]
        self.chat_id = config["chat_id"]
        self.outbox = os.path.join(mailbox_dir, "outbox")
        self.sent_dir = os.path.join(self.outbox, ".sent")
        self.msg_map = msg_map
        self.query_map = query_map
        self.seen = set()
        os.makedirs(self.sent_dir, exist_ok=True)
        self._scan_existing()

    def _scan_existing(self):
        """Record files already in outbox so we don't re-send them."""
        try:
            for f in os.listdir(self.outbox):
                if f.startswith(".") or f.endswith(".tmp"):
                    continue
                self.seen.add(f)
            if self.seen:
                log.info("Found %d existing outbox files (skipping)",
                         len(self.seen))
        except OSError:
            pass

    def check_and_send(self):
        """Check for new outbox files and send them as Telegram messages."""
        try:
            files = os.listdir(self.outbox)
        except OSError:
            return

        for filename in sorted(files):
            if filename.startswith(".") or filename.endswith(".tmp"):
                continue
            if filename in self.seen:
                continue

            parsed = parse_outbox_file(filename)
            if not parsed:
                log.warning("Unknown outbox file: %s", filename)
                self.seen.add(filename)
                continue

            msg_type, msg_id = parsed
            filepath = os.path.join(self.outbox, filename)
            body = read_file(filepath)
            if body is None:
                continue

            # Look up original query text for result messages
            original_query = None
            if msg_type == "result" and self.query_map:
                original_query = self.query_map.get(msg_id)

            text = format_outbox_message(msg_type, msg_id, body,
                                         original_query=original_query)
            log.info("Sending %s → Telegram", filename)

            tg_msg_id = tg_send_message(self.token, self.chat_id, text)
            if tg_msg_id:
                log.info("Sent: %s (msg_id=%d)", filename, tg_msg_id)

                # Track ask messages for reply matching
                if msg_type == "ask":
                    self.msg_map.add(tg_msg_id, msg_id)
                    log.info("Tracking ask_%s → msg %d for reply matching",
                             msg_id, tg_msg_id)

                # Clean up query map after result is delivered
                if msg_type == "result" and self.query_map:
                    self.query_map.remove(msg_id)

                # Move to .sent/
                try:
                    os.rename(filepath,
                              os.path.join(self.sent_dir, filename))
                except OSError:
                    pass
                self.seen.add(filename)
            else:
                log.error("Failed to send %s, will retry", filename)


# ── Telegram update handler ─────────────────────────────

class TelegramPoller:
    """Poll Telegram for replies and new messages, write to inbox."""

    def __init__(self, config, mailbox_dir, msg_map, query_map=None):
        self.config = config
        self.token = config["bot_token"]
        self.chat_id = str(config["chat_id"])
        self.inbox = os.path.join(mailbox_dir, "inbox")
        self.msg_map = msg_map
        self.query_map = query_map
        self.update_offset = self._load_offset(mailbox_dir)
        self.offset_path = os.path.join(mailbox_dir, ".tg_update_offset")

    def _load_offset(self, mailbox_dir):
        """Load last processed update offset."""
        path = os.path.join(mailbox_dir, ".tg_update_offset")
        try:
            with open(path) as f:
                return int(f.read().strip())
        except (OSError, ValueError):
            return None

    def _save_offset(self):
        """Save update offset to survive restarts."""
        try:
            with open(self.offset_path, "w") as f:
                f.write(str(self.update_offset))
        except OSError:
            pass

    def poll(self, timeout=LONG_POLL_TIMEOUT):
        """Long-poll for updates and process them. Returns True if any processed."""
        updates = tg_get_updates(self.token, self.update_offset, timeout)
        if not updates:
            return False

        for update in updates:
            update_id = update["update_id"]
            self.update_offset = update_id + 1

            msg = update.get("message")
            if msg:
                self._process_message(msg)

        self._save_offset()
        return True

    def check_once(self):
        """Non-blocking check for updates."""
        return self.poll(timeout=0)

    def _process_message(self, msg):
        """Process a single Telegram message."""
        # Only accept messages from our configured chat
        chat_id = str(msg.get("chat", {}).get("id", ""))
        if chat_id != self.chat_id:
            log.debug("Ignoring message from chat %s (expected %s)",
                      chat_id, self.chat_id)
            return

        text = msg.get("text", "").strip()
        if not text:
            return

        from_user = msg.get("from", {}).get("first_name", "unknown")
        log.info("Received from %s: %s", from_user, text[:80])

        # Case 1: Reply to an ask message
        reply_to = msg.get("reply_to_message", {})
        reply_msg_id = reply_to.get("message_id")
        if reply_msg_id:
            ask_id = self.msg_map.get_ask_id(reply_msg_id)
            if ask_id:
                answer_file = os.path.join(self.inbox, f"ask_{ask_id}")
                atomic_write(answer_file, text)
                log.info("Answer written: ask_%s = %s", ask_id, text[:80])
                self.msg_map.remove_ask(ask_id)
                # Send confirmation
                tg_send_message(self.token, self.chat_id,
                                f"✅ Answer recorded for <code>{ask_id}</code>",
                                reply_to=msg["message_id"])
                return

        # Case 2: /task command — explicit task submission
        if text.startswith("/task "):
            query = text[6:].strip()
            if query:
                task_id = f"tg_{int(time.time()):x}"
                task_file = os.path.join(self.inbox, f"task_{task_id}")
                atomic_write(task_file, query)
                if self.query_map:
                    self.query_map.add(task_id, query)
                log.info("Task written: task_%s = %s", task_id, query[:80])
                q_preview = query if len(query) <= 80 else query[:80] + "…"
                tg_send_message(self.token, self.chat_id,
                                f"📥 Task submitted: <code>{task_id}</code>\n"
                                f"🔎 <i>{escape_html(q_preview)}</i>",
                                reply_to=msg["message_id"])
                return

        # Case 3: /status command
        if text.strip() == "/status":
            self._send_status(msg["message_id"])
            return

        # Case 4: /help command
        if text.strip() == "/help":
            self._send_help(msg["message_id"])
            return

        # Case 5: /pending command — show pending questions
        if text.strip() == "/pending":
            self._send_pending(msg["message_id"])
            return

        # Case 6: Plain message — treat as task
        task_id = f"tg_{int(time.time()):x}"
        task_file = os.path.join(self.inbox, f"task_{task_id}")
        atomic_write(task_file, text)
        if self.query_map:
            self.query_map.add(task_id, text)
        log.info("Task (plain message): task_%s = %s", task_id, text[:80])
        q_preview = text if len(text) <= 80 else text[:80] + "…"
        tg_send_message(self.token, self.chat_id,
                        f"📥 Task submitted: <code>{task_id}</code>\n"
                        f"🔎 <i>{escape_html(q_preview)}</i>\n"
                        f"<i>Tip: use /task to be explicit, "
                        f"or reply to ❓ messages to answer questions.</i>",
                        reply_to=msg["message_id"])

    def _send_status(self, reply_to):
        """Send mailbox status."""
        mailbox_dir = os.path.dirname(self.inbox)
        outbox = os.path.join(mailbox_dir, "outbox")

        inbox_count = 0
        outbox_count = 0
        pending_asks = []

        try:
            for f in os.listdir(self.inbox):
                if not f.startswith(".") and not f.endswith(".tmp"):
                    inbox_count += 1
        except OSError:
            pass

        try:
            for f in os.listdir(outbox):
                if not f.startswith(".") and not f.endswith(".tmp"):
                    outbox_count += 1
                    if f.startswith("ask_"):
                        pending_asks.append(f)
        except OSError:
            pass

        text = (
            f"📊 <b>Mailbox Status</b>\n\n"
            f"Inbox:  {inbox_count} files\n"
            f"Outbox: {outbox_count} files\n"
            f"Pending questions: {len(pending_asks)}\n"
            f"Tracked ask mappings: {len(self.msg_map.msg_to_ask)}"
        )
        tg_send_message(self.token, self.chat_id, text, reply_to=reply_to)

    def _send_help(self, reply_to):
        """Send help text."""
        text = (
            "🤖 <b>Nash Telegram Bridge</b>\n\n"
            "<b>Commands:</b>\n"
            "/task &lt;query&gt; — Submit a task to nash\n"
            "/status — Show mailbox status\n"
            "/pending — Show pending questions\n"
            "/help — Show this help\n\n"
            "<b>How to interact:</b>\n"
            "• <b>Answer questions:</b> Reply to ❓ messages\n"
            "• <b>Submit tasks:</b> Send /task or just type a message\n"
            "• <b>Results</b> appear as 📋 messages\n"
            "• <b>Status updates</b> appear as ℹ️ messages"
        )
        tg_send_message(self.token, self.chat_id, text, reply_to=reply_to)

    def _send_pending(self, reply_to):
        """Show pending ask questions."""
        if not self.msg_map.msg_to_ask:
            tg_send_message(self.token, self.chat_id,
                            "No pending questions.",
                            reply_to=reply_to)
            return

        lines = ["❓ <b>Pending Questions</b>\n"]
        mailbox_dir = os.path.dirname(self.inbox)
        outbox = os.path.join(mailbox_dir, "outbox")
        sent = os.path.join(outbox, ".sent")

        for tg_msg_id, ask_id in sorted(self.msg_map.msg_to_ask.items()):
            # Try to read the question text
            question = None
            for d in (outbox, sent):
                q = read_file(os.path.join(d, f"ask_{ask_id}"))
                if q:
                    question = q
                    break
            preview = (question[:60] + "…") if question and len(question) > 60 else (question or "?")
            lines.append(f"• <code>{ask_id}</code>: {escape_html(preview)}")

        lines.append(f"\n<i>Reply to the ❓ message to answer.</i>")
        tg_send_message(self.token, self.chat_id,
                        "\n".join(lines), reply_to=reply_to)


# ── Main loop ────────────────────────────────────────────

def run_loop(config, mailbox_dir):
    """Main bridge loop: watch outbox + long-poll Telegram."""
    ensure_dirs(mailbox_dir)
    msg_map = MessageMap(mailbox_dir)
    query_map = QueryMap(mailbox_dir)
    outbox = OutboxWatcher(config, mailbox_dir, msg_map, query_map)
    poller = TelegramPoller(config, mailbox_dir, msg_map, query_map)

    log.info("Telegram bridge running")
    log.info("  Chat ID:  %s", config["chat_id"])
    log.info("  Mailbox:  %s", mailbox_dir)

    # Graceful shutdown
    running = [True]

    def handle_signal(sig, frame):
        log.info("Shutting down...")
        running[0] = False

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    # Drain old updates on first start (don't process messages from before bridge started)
    log.info("Draining old updates...")
    poller.check_once()

    while running[0]:
        # Check outbox for new files (fast, local)
        outbox.check_and_send()

        # Long-poll Telegram (blocks up to LONG_POLL_TIMEOUT seconds)
        # This is where we spend most of our time — efficient, no busy-waiting
        try:
            poller.poll(timeout=5)  # Shorter timeout so we check outbox frequently
        except Exception as e:
            log.error("Poll error: %s", e)
            time.sleep(5)


# ── Commands ─────────────────────────────────────────────

def cmd_run(config, mailbox_dir):
    """Run the main bridge loop."""
    run_loop(config, mailbox_dir)


def cmd_test(config):
    """Test bot connectivity."""
    print(f"Testing bot token...")
    print()

    # Get bot info
    bot = tg_get_me(config["bot_token"])
    if bot:
        print(f"Bot:      @{bot.get('username', '?')} ({bot.get('first_name', '?')})")
        print(f"Bot ID:   {bot.get('id', '?')}")
        print(f"Chat ID:  {config['chat_id']}")
        print(f"Status:   OK ✓")
    else:
        print("Failed to connect to Telegram API ✗")
        print("Check your bot_token in the config.")
        return False

    # Test sending
    print()
    yn = input("Send a test message? [y/N] ").strip().lower()
    if yn == "y":
        text = (
            f"🤖 <b>Nash Telegram Bridge Test</b>\n\n"
            f"If you see this, the bridge is working correctly.\n"
            f"Timestamp: {time.strftime('%Y-%m-%d %H:%M:%S %Z')}"
        )
        msg_id = tg_send_message(config["bot_token"], config["chat_id"], text)
        if msg_id:
            print(f"Test message sent! ✓ (message_id={msg_id})")
        else:
            print("Failed to send test message ✗")
            print("Check your chat_id in the config.")
            return False

    return True


def cmd_send(config, filepath):
    """Send a specific outbox file as a Telegram message."""
    filename = os.path.basename(filepath)
    parsed = parse_outbox_file(filename)
    if not parsed:
        print(f"Error: cannot parse filename '{filename}'")
        print("Expected: ask_ID, result_ID, or status_ID")
        sys.exit(1)

    body = read_file(filepath)
    if body is None:
        print(f"Error: cannot read '{filepath}'")
        sys.exit(1)

    msg_type, msg_id = parsed
    text = format_outbox_message(msg_type, msg_id, body)
    print(f"Sending: {filename}")
    msg_id_tg = tg_send_message(config["bot_token"], config["chat_id"], text)
    if msg_id_tg:
        print(f"Sent ✓ (message_id={msg_id_tg})")
    else:
        print("Failed ✗")
        sys.exit(1)


def cmd_check(config, mailbox_dir):
    """One-shot check for Telegram updates."""
    ensure_dirs(mailbox_dir)
    msg_map = MessageMap(mailbox_dir)
    poller = TelegramPoller(config, mailbox_dir, msg_map)
    poller.check_once()
    print("Done.")


def cmd_setup():
    """Interactive setup — create ~/.nash/telegram.conf."""
    conf_path = os.path.expanduser("~/.nash/telegram.conf")
    os.makedirs(os.path.dirname(conf_path), exist_ok=True)

    if os.path.exists(conf_path):
        print(f"Config already exists: {conf_path}")
        yn = input("Overwrite? [y/N] ").strip().lower()
        if yn != "y":
            return

    print()
    print("Nash Telegram Bridge Setup")
    print("=" * 40)
    print()
    print("Step 1: Create a Telegram Bot")
    print("  1. Open Telegram, search for @BotFather")
    print("  2. Send /newbot")
    print("  3. Choose a name (e.g., 'My Nash Agent')")
    print("  4. Choose a username (e.g., 'my_nash_bot')")
    print("  5. Copy the bot token")
    print()

    token = input("Bot token: ").strip()
    if not token:
        print("Error: bot token is required")
        return

    # Verify token
    print("Verifying token...", end=" ", flush=True)
    bot = tg_get_me(token)
    if not bot:
        print("FAILED ✗")
        print("Invalid token. Check and try again.")
        return
    print(f"OK ✓ — @{bot.get('username', '?')}")

    print()
    print("Step 2: Get your Chat ID")
    print("  1. Open Telegram, find your new bot")
    print("  2. Send it any message (e.g., 'hello')")
    print()
    input("Press Enter after sending a message to the bot...")

    # Try to auto-detect chat_id from recent messages
    print("Detecting chat ID...", end=" ", flush=True)
    updates = tg_get_updates(token, timeout=2)
    chat_id = None
    if updates:
        # Take the most recent message's chat ID
        for update in reversed(updates):
            msg = update.get("message", {})
            cid = msg.get("chat", {}).get("id")
            if cid:
                chat_id = str(cid)
                from_name = msg.get("from", {}).get("first_name", "?")
                print(f"OK ✓ — chat_id={chat_id} (from {from_name})")
                break

    if not chat_id:
        print("Could not auto-detect.")
        chat_id = input("Enter chat_id manually: ").strip()
        if not chat_id:
            print("Error: chat_id is required")
            return

    content = f"""# Nash Telegram Bridge Configuration
# Created: {time.strftime('%Y-%m-%d %H:%M:%S %Z')}

# Bot token from @BotFather
bot_token = {token}

# Your Telegram chat ID (auto-detected)
chat_id = {chat_id}

# How often to check outbox between Telegram polls (seconds)
# Note: Telegram updates use long polling so replies are near-instant
poll_interval = 1
"""

    with open(conf_path, "w") as f:
        f.write(content)
    os.chmod(conf_path, 0o600)
    print()
    print(f"Config written to: {conf_path}")
    print(f"Permissions set to 600 (owner-only)")

    # Send confirmation
    print()
    print("Sending confirmation message...", end=" ", flush=True)
    text = (
        "🤖 <b>Nash Telegram Bridge Connected!</b>\n\n"
        "Your nash agent can now communicate through this chat.\n\n"
        "<b>Commands:</b>\n"
        "/task &lt;query&gt; — Submit a task\n"
        "/status — Mailbox status\n"
        "/pending — Pending questions\n"
        "/help — Help\n\n"
        "<i>Reply to ❓ messages to answer questions.</i>"
    )
    msg_id = tg_send_message(token, chat_id, text)
    if msg_id:
        print("OK ✓")
        # Drain the update so we don't process it as a task later
        tg_get_updates(token, timeout=1)
    else:
        print("FAILED (but config is saved)")

    print()
    print("Test with: nash-telegram-bridge.py test")
    print("Run with:  nash-telegram-bridge.py run")


# ── Entry point ──────────────────────────────────────────

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    cmd = sys.argv[1]

    if cmd == "setup":
        cmd_setup()
        return

    config, conf_path = load_config()
    if config is None:
        print(f"Config not found: {conf_path}")
        print("Run: nash-telegram-bridge.py setup")
        sys.exit(1)

    if not validate_config(config):
        print(f"Fix config: {conf_path}")
        sys.exit(1)

    mailbox_dir = get_mailbox_dir()

    if cmd == "run":
        cmd_run(config, mailbox_dir)
    elif cmd == "test":
        cmd_test(config)
    elif cmd == "send":
        if len(sys.argv) < 3:
            print("Usage: nash-telegram-bridge.py send FILE")
            sys.exit(1)
        cmd_send(config, sys.argv[2])
    elif cmd == "check":
        cmd_check(config, mailbox_dir)
    else:
        print(f"Unknown command: {cmd}")
        print(__doc__)
        sys.exit(1)


if __name__ == "__main__":
    main()
