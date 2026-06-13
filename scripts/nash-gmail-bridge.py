#!/usr/bin/env python3
"""
nash-gmail-bridge.py — Gmail bridge for nash's file-based mailbox.

Watches outbox/ for agent messages and sends them as emails.
Polls IMAP for replies/new tasks and writes them to inbox/.
Pure Python3 stdlib — no pip dependencies.

Usage:
  nash-gmail-bridge.py run              Main loop (outbox watcher + IMAP poller)
  nash-gmail-bridge.py test             Test SMTP/IMAP connectivity
  nash-gmail-bridge.py send FILE        Send a specific outbox file as email
  nash-gmail-bridge.py check            One-shot: check IMAP for replies/tasks
  nash-gmail-bridge.py setup            Interactive config setup

Config: ~/.nash/gmail.conf (or $NASH_GMAIL_CONF)
  smtp_server = smtp.gmail.com
  smtp_port = 587
  imap_server = imap.gmail.com
  imap_port = 993
  email = your-agent@gmail.com
  password = xxxx-xxxx-xxxx-xxxx    (Gmail App Password)
  notify = your-human@example.com
  poll_interval = 30
  label = nash
"""

import sys
import os
import re
import time
import signal
import smtplib
import imaplib
import email
import email.utils
import logging
from email.mime.text import MIMEText
from email.header import decode_header
from pathlib import Path

# ── Logging ──────────────────────────────────────────────

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("gmail-bridge")

# ── Config ───────────────────────────────────────────────

DEFAULT_CONFIG = {
    "smtp_server": "smtp.gmail.com",
    "smtp_port": "587",
    "imap_server": "imap.gmail.com",
    "imap_port": "993",
    "email": "",
    "password": "",
    "notify": "",
    "poll_interval": "30",
    "label": "nash",
}


def load_config(path=None):
    """Load key=value config file. Lines starting with # are comments."""
    if path is None:
        path = os.environ.get("NASH_GMAIL_CONF",
                              os.path.expanduser("~/.nash/gmail.conf"))
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
    for key in ("email", "password", "notify"):
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


# ── Email helpers ────────────────────────────────────────

def make_subject(msg_type, msg_id, body):
    """Generate email subject from message type and content."""
    first_line = body.split("\n")[0][:80] if body else ""
    if msg_type == "ask":
        return f"[nash:ask:{msg_id}] {first_line}"
    elif msg_type == "result":
        return f"[nash:result:{msg_id}] {first_line}"
    elif msg_type == "status":
        return f"[nash:status] {first_line}"
    else:
        return f"[nash] {first_line}"


def send_email(config, subject, body):
    """Send an email via SMTP."""
    msg = MIMEText(body, "plain", "utf-8")
    msg["From"] = config["email"]
    msg["To"] = config["notify"]
    msg["Subject"] = subject
    msg["Date"] = email.utils.formatdate(localtime=True)
    msg["X-Nash-Bridge"] = "1"

    try:
        port = int(config["smtp_port"])
        if port == 465:
            server = smtplib.SMTP_SSL(config["smtp_server"], port, timeout=30)
        else:
            server = smtplib.SMTP(config["smtp_server"], port, timeout=30)
            server.ehlo()
            server.starttls()
            server.ehlo()
        server.login(config["email"], config["password"])
        server.sendmail(config["email"], [config["notify"]], msg.as_string())
        server.quit()
        return True
    except Exception as e:
        log.error("SMTP send failed: %s", e)
        return False


def decode_header_value(val):
    """Decode MIME-encoded header into plain string."""
    if val is None:
        return ""
    parts = decode_header(val)
    decoded = []
    for data, charset in parts:
        if isinstance(data, bytes):
            decoded.append(data.decode(charset or "utf-8", errors="replace"))
        else:
            decoded.append(data)
    return " ".join(decoded)


def extract_reply_body(msg):
    """Extract the reply text from an email, stripping quoted content."""
    body = None
    if msg.is_multipart():
        for part in msg.walk():
            ct = part.get_content_type()
            if ct == "text/plain":
                payload = part.get_payload(decode=True)
                if payload:
                    body = payload.decode(
                        part.get_content_charset() or "utf-8",
                        errors="replace"
                    )
                    break
    else:
        payload = msg.get_payload(decode=True)
        if payload:
            body = payload.decode(
                msg.get_content_charset() or "utf-8",
                errors="replace"
            )

    if not body:
        return ""

    # Strip common reply quote markers
    # Stop at lines starting with "On ... wrote:" or "> "
    lines = body.split("\n")
    clean = []
    for line in lines:
        # Gmail-style "On Mon, Jun 13, 2026 at 2:00 PM ... wrote:"
        if re.match(r"^On .+ wrote:$", line.strip()):
            break
        # Outlook-style "From: ..."
        if re.match(r"^-+\s*Original Message\s*-+", line.strip(), re.I):
            break
        # Quoted line (but allow first level of quoting as actual content)
        if line.startswith("> ") and clean and any(
            l.startswith("> ") for l in clean[-3:]
        ):
            # Multiple consecutive quoted lines = we're in the quote section
            # But let's keep it simple: just stop at first "On ... wrote:"
            pass
        clean.append(line)

    return "\n".join(clean).strip()


def parse_subject_for_ask_id(subject):
    """Extract ask ID from subject like '[nash:ask:abc123] question text'.
    Returns the ID or None."""
    m = re.search(r"\[nash:ask:([^\]]+)\]", subject)
    return m.group(1) if m else None


def parse_subject_for_result_id(subject):
    """Extract result ID from subject like '[nash:result:abc123]'."""
    m = re.search(r"\[nash:result:([^\]]+)\]", subject)
    return m.group(1) if m else None


def is_nash_email(subject):
    """Check if this is a nash-originated email (to avoid processing our own)."""
    return bool(re.search(r"\[nash:", subject))


def is_new_task_email(subject, from_addr, config):
    """Check if email is a new task submission (not a reply to nash).
    A new task is from the notify address and has [nash:task] or [nash] in subject,
    but NOT [nash:ask:...] or [nash:result:...] (those are replies)."""
    if from_addr != config["notify"]:
        return False
    if re.search(r"\[nash:(ask|result|status):", subject):
        return False
    if re.search(r"\[nash(:task)?\]", subject):
        return True
    return False


# ── Outbox watcher ───────────────────────────────────────

class OutboxWatcher:
    """Watch outbox/ for new files and send them as emails."""

    def __init__(self, config, mailbox_dir):
        self.config = config
        self.outbox = os.path.join(mailbox_dir, "outbox")
        self.sent_dir = os.path.join(self.outbox, ".sent")
        self.seen = set()
        os.makedirs(self.sent_dir, exist_ok=True)
        # Track already-existing files on startup
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
        """Check for new outbox files and send them as emails."""
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

            subject = make_subject(msg_type, msg_id, body)
            log.info("Sending %s → %s", filename, self.config["notify"])

            if send_email(self.config, subject, body):
                log.info("Sent: %s", subject)
                # Move to .sent/
                try:
                    os.rename(filepath,
                              os.path.join(self.sent_dir, filename))
                except OSError:
                    pass
                self.seen.add(filename)
            else:
                log.error("Failed to send %s, will retry", filename)


# ── IMAP poller ──────────────────────────────────────────

class ImapPoller:
    """Poll IMAP for replies to ask questions and new task submissions."""

    def __init__(self, config, mailbox_dir):
        self.config = config
        self.inbox = os.path.join(mailbox_dir, "inbox")
        self.label = config.get("label", "nash")

    def check(self):
        """Connect to IMAP, check for relevant emails, write to inbox."""
        try:
            imap = imaplib.IMAP4_SSL(
                self.config["imap_server"],
                int(self.config["imap_port"]),
                timeout=30,
            )
            imap.login(self.config["email"], self.config["password"])
        except Exception as e:
            log.error("IMAP login failed: %s", e)
            return

        try:
            self._process_mailbox(imap)
        finally:
            try:
                imap.logout()
            except Exception:
                pass

    def _process_mailbox(self, imap):
        """Search for unread emails from the notify address."""
        imap.select("INBOX")

        # Search for unread emails from the configured notify address
        notify = self.config["notify"]
        status, data = imap.search(None, "UNSEEN", f'FROM "{notify}"')
        if status != "OK" or not data[0]:
            return

        msg_ids = data[0].split()
        log.info("Found %d unread emails from %s", len(msg_ids), notify)

        for msg_id in msg_ids:
            try:
                self._process_message(imap, msg_id)
            except Exception as e:
                log.error("Error processing message %s: %s", msg_id, e)

    def _process_message(self, imap, msg_id):
        """Process a single IMAP message."""
        status, data = imap.fetch(msg_id, "(RFC822)")
        if status != "OK":
            return

        raw = data[0][1]
        msg = email.message_from_bytes(raw)
        subject = decode_header_value(msg.get("Subject", ""))
        from_addr = email.utils.parseaddr(msg.get("From", ""))[1]

        log.info("Processing: %s (from %s)", subject, from_addr)

        # Case 1: Reply to a nash ask question
        ask_id = parse_subject_for_ask_id(subject)
        if ask_id:
            body = extract_reply_body(msg)
            if body:
                answer_file = os.path.join(self.inbox, f"ask_{ask_id}")
                atomic_write(answer_file, body)
                log.info("Answer written: ask_%s = %s",
                         ask_id, body[:80])
                # Mark as read (already is since we fetched, but ensure)
                imap.store(msg_id, "+FLAGS", "\\Seen")
                return
            else:
                log.warning("Empty reply body for ask_%s", ask_id)

        # Case 2: New task submission
        if is_new_task_email(subject, from_addr, self.config):
            body = extract_reply_body(msg)
            if body:
                task_id = f"gmail_{int(time.time()):x}"
                task_file = os.path.join(self.inbox, f"task_{task_id}")
                atomic_write(task_file, body)
                log.info("Task written: task_%s = %s",
                         task_id, body[:80])
                imap.store(msg_id, "+FLAGS", "\\Seen")
                return

        # Case 3: Unrecognized but from notify address
        # Could be a plain email task (no [nash] tag needed)
        # Treat any email from notify as a task if not a reply
        if from_addr == self.config["notify"] and not is_nash_email(subject):
            body = extract_reply_body(msg)
            if body:
                task_id = f"gmail_{int(time.time()):x}"
                task_file = os.path.join(self.inbox, f"task_{task_id}")
                # Include subject as context
                full_query = f"{subject}\n\n{body}" if subject else body
                atomic_write(task_file, full_query)
                log.info("Task (plain email): task_%s = %s",
                         task_id, subject[:80])
                imap.store(msg_id, "+FLAGS", "\\Seen")
                return

        log.debug("Skipping email: %s", subject)


# ── Main loop ────────────────────────────────────────────

def run_loop(config, mailbox_dir):
    """Main bridge loop: watch outbox + poll IMAP."""
    ensure_dirs(mailbox_dir)
    outbox = OutboxWatcher(config, mailbox_dir)
    poller = ImapPoller(config, mailbox_dir)
    interval = int(config.get("poll_interval", "30"))

    log.info("Gmail bridge running")
    log.info("  Email:    %s", config["email"])
    log.info("  Notify:   %s", config["notify"])
    log.info("  Mailbox:  %s", mailbox_dir)
    log.info("  Interval: %ds", interval)

    # Graceful shutdown
    running = [True]

    def handle_signal(sig, frame):
        log.info("Shutting down...")
        running[0] = False

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    last_imap_check = 0

    while running[0]:
        # Check outbox for new files (fast, every second)
        outbox.check_and_send()

        # Poll IMAP on interval
        now = time.time()
        if now - last_imap_check >= interval:
            poller.check()
            last_imap_check = now

        # Sleep 1s between outbox checks
        time.sleep(1)


# ── Commands ─────────────────────────────────────────────

def cmd_run(config, mailbox_dir):
    """Run the main bridge loop."""
    run_loop(config, mailbox_dir)


def cmd_test(config):
    """Test SMTP and IMAP connectivity."""
    print(f"Testing with email: {config['email']}")
    print(f"Notify address:     {config['notify']}")
    print()

    # Test SMTP
    print("Testing SMTP...", end=" ", flush=True)
    try:
        port = int(config["smtp_port"])
        if port == 465:
            server = smtplib.SMTP_SSL(config["smtp_server"], port, timeout=10)
        else:
            server = smtplib.SMTP(config["smtp_server"], port, timeout=10)
            server.ehlo()
            server.starttls()
            server.ehlo()
        server.login(config["email"], config["password"])
        server.quit()
        print("OK ✓")
    except Exception as e:
        print(f"FAILED ✗\n  {e}")
        return False

    # Test IMAP
    print("Testing IMAP...", end=" ", flush=True)
    try:
        imap = imaplib.IMAP4_SSL(
            config["imap_server"],
            int(config["imap_port"]),
            timeout=10,
        )
        imap.login(config["email"], config["password"])
        status, folders = imap.list()
        imap.select("INBOX")
        status, data = imap.search(None, "ALL")
        total = len(data[0].split()) if data[0] else 0
        imap.logout()
        print(f"OK ✓ ({total} messages in inbox)")
    except Exception as e:
        print(f"FAILED ✗\n  {e}")
        return False

    # Test sending
    print()
    yn = input("Send a test email to yourself? [y/N] ").strip().lower()
    if yn == "y":
        subject = "[nash:status] Gmail bridge test"
        body = (
            "This is a test email from nash-gmail-bridge.\n"
            "If you received this, the bridge is working correctly.\n"
            f"\nTimestamp: {time.strftime('%Y-%m-%d %H:%M:%S %Z')}\n"
        )
        if send_email(config, subject, body):
            print("Test email sent! ✓")
        else:
            print("Failed to send test email ✗")

    return True


def cmd_send(config, filepath):
    """Send a specific outbox file as email."""
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
    subject = make_subject(msg_type, msg_id, body)
    print(f"Sending: {subject}")
    if send_email(config, subject, body):
        print("Sent ✓")
    else:
        print("Failed ✗")
        sys.exit(1)


def cmd_check(config, mailbox_dir):
    """One-shot IMAP check."""
    ensure_dirs(mailbox_dir)
    poller = ImapPoller(config, mailbox_dir)
    poller.check()
    print("Done.")


def cmd_setup():
    """Interactive setup — create ~/.nash/gmail.conf."""
    conf_path = os.path.expanduser("~/.nash/gmail.conf")
    os.makedirs(os.path.dirname(conf_path), exist_ok=True)

    if os.path.exists(conf_path):
        print(f"Config already exists: {conf_path}")
        yn = input("Overwrite? [y/N] ").strip().lower()
        if yn != "y":
            return

    print()
    print("Nash Gmail Bridge Setup")
    print("=" * 40)
    print()
    print("You need a Gmail App Password (not your regular password).")
    print("Go to: https://myaccount.google.com/apppasswords")
    print("Create one for 'Mail' on 'Other (nash)'.")
    print()

    addr = input("Gmail address for the agent: ").strip()
    pw = input("App password (xxxx-xxxx-xxxx-xxxx): ").strip()
    notify = input("Your email (where to send notifications): ").strip()
    interval = input("IMAP poll interval in seconds [30]: ").strip() or "30"

    content = f"""# Nash Gmail Bridge Configuration
# Created: {time.strftime('%Y-%m-%d %H:%M:%S %Z')}

# SMTP settings (for sending)
smtp_server = smtp.gmail.com
smtp_port = 587

# IMAP settings (for receiving)
imap_server = imap.gmail.com
imap_port = 993

# Gmail account (use App Password, not regular password)
# Get app password: https://myaccount.google.com/apppasswords
email = {addr}
password = {pw}

# Where to send notifications (your human email)
notify = {notify}

# How often to check for replies (seconds)
poll_interval = {interval}

# Gmail label for nash emails (optional)
label = nash
"""

    with open(conf_path, "w") as f:
        f.write(content)
    os.chmod(conf_path, 0o600)
    print()
    print(f"Config written to: {conf_path}")
    print(f"Permissions set to 600 (owner-only)")
    print()
    print("Test with: nash-gmail-bridge.py test")
    print("Run with:  nash-gmail-bridge.py run")


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
        print("Run: nash-gmail-bridge.py setup")
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
            print("Usage: nash-gmail-bridge.py send FILE")
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
