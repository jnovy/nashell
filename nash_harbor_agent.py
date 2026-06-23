"""
Nash agent adapter for Harbor framework (Terminal-Bench 2.0).

Nash is a compiled C binary that supports headless mode via `-p "QUERY"`.
This adapter installs nash inside the Docker container, configures Vertex AI
provider credentials, and runs tasks in headless mode.

Usage:
    harbor run -d "terminal-bench/terminal-bench@2.0" \
        --agent-import-path nash_harbor_agent:NashAgent \
        -m vertex/claude-opus-4-6
"""

import json
import os
import shlex
import tempfile
from pathlib import Path
from typing import override

from harbor.agents.installed.base import BaseInstalledAgent, with_prompt_template
from harbor.environments.base import BaseEnvironment
from harbor.models.agent.context import AgentContext


class NashAgent(BaseInstalledAgent):
    """
    Nash agent — a compiled C binary that runs as a ReAct loop with 18 built-in
    tools (shell_exec, file_read, file_write, file_edit, grep_search, etc.).
    """

    # Path to the nash binary on the host
    NASH_BINARY = os.environ.get(
        "NASH_BINARY", os.path.expanduser("~/agents/nash/nash")
    )
    # Path to nash config on the host
    NASH_CONFIG = os.environ.get(
        "NASH_CONFIG", os.path.expanduser("~/.nash/config.toml")
    )
    # Path to Google Cloud ADC credentials
    GOOGLE_ADC = os.environ.get(
        "GOOGLE_APPLICATION_CREDENTIALS",
        os.path.expanduser("~/.config/gcloud/application_default_credentials.json"),
    )
    # Path to ONNX embedding model on the host
    NASH_EMBEDDING_MODEL = os.environ.get(
        "NASH_EMBEDDING_MODEL", os.path.expanduser("~/models/all-MiniLM-L6-v2")
    )

    # Remote paths inside container
    _REMOTE_NASH_BIN = "/usr/local/bin/nash"
    _REMOTE_NASH_DATA = "/tmp/nash-data"
    _REMOTE_CONFIG = "/root/.nash/config.toml"  # must be $HOME/.nash/config.toml
    _REMOTE_ADC = "/tmp/nash-data/adc.json"
    _REMOTE_EMBEDDING = "/tmp/nash-data/embedding-model"

    @staticmethod
    @override
    def name() -> str:
        return "nash"

    @override
    def get_version_command(self) -> str | None:
        return "nash --help 2>&1 | head -1 || true"

    @override
    def parse_version(self, stdout: str) -> str:
        return "dev"

    def _build_config_toml(self) -> str:
        """Build a config.toml for nash inside the container.

        Uses the model from self.model_name if provided, otherwise reads
        the host config for project/region settings.
        """
        # Parse model: format is "vertex/claude-opus-4-6" or just "claude-opus-4-6"
        model_id = self.model_name or "claude-opus-4-6"
        provider_type = "vertex"
        if "/" in model_id:
            parts = model_id.split("/", 1)
            provider_type = parts[0]
            model_id = parts[1]

        # Read project_id and region from host config
        project_id = os.environ.get("VERTEX_PROJECT_ID", "")
        region = os.environ.get("VERTEX_REGION", "global")

        # Try to read from host config if not in env
        if not project_id and os.path.exists(self.NASH_CONFIG):
            try:
                with open(self.NASH_CONFIG) as f:
                    for line in f:
                        line = line.strip()
                        if line.startswith("project_id"):
                            project_id = line.split("=", 1)[1].strip().strip('"')
                        elif line.startswith("region") and "=" in line:
                            region = line.split("=", 1)[1].strip().strip('"')
            except Exception:
                pass

        config = f"""# Nash config for Terminal-Bench (auto-generated)
[provider]
type = "{provider_type}"
model_id = "{model_id}"
project_id = "{project_id}"
region = "{region}"

[client]
temperature = 0
max_tokens = 16384
stream = false

[thinking]
mode = "yes"

[embedding]
type = "none"

[limits]
shell_timeout = 300
shell_max_output = 512000
file_max_size = 52428800
llm_timeout = 600
max_react_steps = 200

[paths]
data_dir = "{self._REMOTE_NASH_DATA}"

[search]
engine = "duckduckgo"
"""
        return config

    @override
    async def install(self, environment: BaseEnvironment) -> None:
        """Install nash binary and dependencies in the container."""

        # Install system library dependencies
        await self.exec_as_root(
            environment,
            command=(
                "apt-get update && apt-get install -y --no-install-recommends "
                "libcurl4-openssl-dev libssl-dev libreadline-dev "
                "libncursesw5-dev ca-certificates curl git "
                "|| (yum install -y libcurl-devel openssl-devel readline-devel "
                "ncurses-devel ca-certificates curl git) "
                "|| true"
            ),
            env={"DEBIAN_FRONTEND": "noninteractive"},
        )

        # Create nash data directory
        await self.exec_as_root(
            environment,
            command=f"mkdir -p {self._REMOTE_NASH_DATA}/memory {self._REMOTE_NASH_DATA}/sessions {self._REMOTE_NASH_DATA}/store",
        )

        # Upload nash binary
        if not os.path.exists(self.NASH_BINARY):
            raise RuntimeError(f"Nash binary not found at {self.NASH_BINARY}")
        await environment.upload_file(self.NASH_BINARY, self._REMOTE_NASH_BIN)
        await self.exec_as_root(
            environment,
            command=f"chmod +x {self._REMOTE_NASH_BIN}",
        )

        # Upload libonnxruntime shared library
        onnx_lib_dir = os.path.expanduser(
            "~/.local/lib/python3.14/site-packages/onnxruntime/capi"
        )
        for lib_name in ["libonnxruntime.so.1.24.4"]:
            lib_path = os.path.join(onnx_lib_dir, lib_name)
            if os.path.exists(lib_path):
                await environment.upload_file(lib_path, f"/usr/local/lib/{lib_name}")
        await self.exec_as_root(
            environment,
            command=(
                "ln -sf /usr/local/lib/libonnxruntime.so.1.24.4 /usr/local/lib/libonnxruntime.so.1 && "
                "ln -sf /usr/local/lib/libonnxruntime.so.1.24.4 /usr/local/lib/libonnxruntime.so && "
                "ldconfig"
            ),
        )

        # Upload config.toml to $HOME/.nash/ (nash loads config from $HOME/.nash/config.toml)
        await self.exec_as_root(
            environment,
            command="mkdir -p /root/.nash",
        )
        config_content = self._build_config_toml()
        with tempfile.NamedTemporaryFile(mode="w", suffix=".toml", delete=False) as f:
            f.write(config_content)
            tmp_config = f.name
        try:
            await environment.upload_file(tmp_config, self._REMOTE_CONFIG)
        finally:
            os.unlink(tmp_config)

        # Upload Google Cloud ADC credentials
        if os.path.exists(self.GOOGLE_ADC):
            await environment.upload_file(self.GOOGLE_ADC, self._REMOTE_ADC)

        # Create a fake 'gcloud' wrapper that exchanges ADC refresh token for
        # an access token. Nash calls `gcloud auth print-access-token` internally
        # (provider_anthropic.c:85) and gcloud CLI is not installed in containers.
        # Uses only bash/grep/sed/curl — no python3 dependency.
        gcloud_script = """#!/bin/bash
# Fake gcloud - only supports 'auth print-access-token'
json_val() { grep -o '"'"$1"'"[[:space:]]*:[[:space:]]*"[^"]*"' "$2" | head -1 | sed 's/.*"\\([^"]*\\)"$/\\1/'; }
if [ "$1" = "auth" ] && [ "$2" = "print-access-token" ]; then
    ADC=""" + shlex.quote(self._REMOTE_ADC) + """
    [ -f "$ADC" ] || { echo "ERROR: No ADC at $ADC" >&2; exit 1; }
    CID=$(json_val client_id "$ADC")
    CSE=$(json_val client_secret "$ADC")
    RTK=$(json_val refresh_token "$ADC")
    [ -n "$CID" ] && [ -n "$RTK" ] || { echo "ERROR: parse ADC" >&2; exit 1; }
    RESP=$(curl -s -X POST https://oauth2.googleapis.com/token \\
        -d "client_id=$CID" -d "client_secret=$CSE" \\
        -d "refresh_token=$RTK" -d "grant_type=refresh_token")
    TOK=$(echo "$RESP" | grep -o '"access_token"[[:space:]]*:[[:space:]]*"[^"]*"' | sed 's/.*"\\([^"]*\\)"$/\\1/')
    [ -n "$TOK" ] && echo "$TOK" || { echo "ERROR: token exchange: $RESP" >&2; exit 1; }
else
    echo "ERROR: unsupported gcloud command" >&2; exit 1
fi
"""
        with tempfile.NamedTemporaryFile(mode="w", suffix=".sh", delete=False) as f:
            f.write(gcloud_script)
            tmp_gcloud = f.name
        try:
            await environment.upload_file(tmp_gcloud, "/usr/local/bin/gcloud")
        finally:
            os.unlink(tmp_gcloud)
        await self.exec_as_root(
            environment,
            command="chmod +x /usr/local/bin/gcloud",
        )

        # Fix ownership so agent user can access everything
        if environment.default_user is not None:
            await self.exec_as_root(
                environment,
                command=(
                    f"chown -R {environment.default_user} {self._REMOTE_NASH_DATA} "
                    f"/root/.nash "
                    f"&& chown {environment.default_user} {self._REMOTE_NASH_BIN}"
                ),
            )

    # Max number of retry attempts when nash exits with a non-zero code
    # (transient Vertex AI API errors are recoverable on retry)
    _MAX_RETRIES = 2

    @with_prompt_template
    @override
    async def run(
        self, instruction: str, environment: BaseEnvironment, context: AgentContext
    ) -> None:
        """Run nash in headless/pipeline mode."""

        escaped_instruction = shlex.quote(instruction)

        env = {
            "GOOGLE_APPLICATION_CREDENTIALS": self._REMOTE_ADC,
            "HOME": "/root",
            "TERM": "dumb",
        }

        # Build the nash command.
        # Fix 3: stderr is NOT merged (no 2>&1) — flows to Harbor's stderr capture
        # separately, so trial.log shows both stdout and stderr independently.
        # Stderr is also tee'd to /tmp/nash-stderr.txt for artifact preservation.
        nash_cmd = (
            f"nash "
            f"--data-dir {shlex.quote(self._REMOTE_NASH_DATA)} "
            f"-p {escaped_instruction} "
            f"2> >(tee /tmp/nash-stderr.txt >&2) "
            f"| tee /tmp/nash-output.txt"
        )

        # Fix 4: Retry on failure — transient Vertex AI API errors often resolve
        # on a second attempt. Nash exits with code 1 when its 5-tier error
        # recovery exhausts all retries (react_error.c:176-410).
        last_error: Exception | None = None
        for attempt in range(1, self._MAX_RETRIES + 1):
            try:
                # Clean up previous attempt's session data so nash starts fresh
                if attempt > 1:
                    self.logger.debug(
                        f"Nash retry {attempt}/{self._MAX_RETRIES} — "
                        f"clearing previous session data"
                    )
                    await self.exec_as_agent(
                        environment,
                        command=(
                            f"rm -rf {self._REMOTE_NASH_DATA}/sessions/* "
                            f"/tmp/nash-output.txt /tmp/nash-stderr.txt "
                            f"|| true"
                        ),
                        env=env,
                    )

                await self.exec_as_agent(
                    environment,
                    command=f"bash -c {shlex.quote(nash_cmd)}",
                    env=env,
                    timeout_sec=3600,  # 1 hour timeout for complex tasks
                )
                # Success — no need to retry
                last_error = None
                break
            except Exception as e:
                last_error = e
                self.logger.debug(
                    f"Nash attempt {attempt}/{self._MAX_RETRIES} failed: {e}"
                )
                if attempt >= self._MAX_RETRIES:
                    self.logger.debug(
                        f"Nash exhausted all {self._MAX_RETRIES} attempts"
                    )

        # Fix 1 & 2: Save session journal and output as artifacts.
        # These files are critical for post-mortem diagnosis when nash crashes.
        # Copy them to /logs/ so they persist alongside Harbor's trial.log.
        await self._save_artifacts(environment, env)

        if last_error is not None:
            self.logger.debug(
                f"Nash execution error (non-fatal after retries): {last_error}"
            )

    async def _save_artifacts(
        self, environment: BaseEnvironment, env: dict[str, str]
    ) -> None:
        """Copy nash session artifacts to /logs/artifacts/ for post-mortem diagnosis.

        Harbor collects files from /logs/artifacts/ inside the container and
        saves them to the trial directory at artifacts/logs/artifacts/.

        Saves:
        - /tmp/nash-output.txt → /logs/artifacts/nash-stdout.txt  (full stdout)
        - /tmp/nash-stderr.txt → /logs/artifacts/nash-stderr.txt  (full stderr)
        - session journal.jsonl → /logs/artifacts/nash-journal.jsonl
        - session checkpoint.json → /logs/artifacts/nash-checkpoint.json
        """
        try:
            await self.exec_as_agent(
                environment,
                command=(
                    "mkdir -p /logs/artifacts && "
                    "cp /tmp/nash-output.txt /logs/artifacts/nash-stdout.txt 2>/dev/null; "
                    "cp /tmp/nash-stderr.txt /logs/artifacts/nash-stderr.txt 2>/dev/null; "
                    # Find the most recent session directory and copy key files
                    "SESS=$(ls -td " + self._REMOTE_NASH_DATA + "/sessions/*/ 2>/dev/null | head -1); "
                    "if [ -n \"$SESS\" ]; then "
                    "  cp \"${SESS}journal.jsonl\" /logs/artifacts/nash-journal.jsonl 2>/dev/null; "
                    "  cp \"${SESS}checkpoint.json\" /logs/artifacts/nash-checkpoint.json 2>/dev/null; "
                    "fi; "
                    "true"
                ),
                env=env,
            )
            self.logger.debug("Nash artifacts saved to /logs/artifacts/")
        except Exception as e:
            self.logger.debug(f"Failed to save nash artifacts (non-fatal): {e}")

    @override
    def populate_context_post_run(self, context: AgentContext) -> None:
        """Parse nash output and populate context."""
        # Nash outputs results via the done() tool; the actual work happens
        # inside the container via tool calls. No additional parsing needed
        # since the benchmark verifier evaluates the container state directly.
        pass
