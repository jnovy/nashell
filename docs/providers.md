# Multi-Provider Support

Nash supports four LLM provider types through a unified vtable interface.
All providers share the same tool registry, SSE streaming, and react loop.
Provider-specific differences (JSON format, auth headers, endpoint URLs)
are encapsulated in the vtable -- you switch providers by changing one line
in `[routing]`.

## Provider Types

| Type | Backend | Default Endpoint | Auth Method |
|------|---------|------------------|-------------|
| `local` | llama.cpp / any OpenAI-compatible server | `http://localhost:8080` | None |
| `openai` | OpenAI API (GPT-4o, GPT-5, etc.) | `https://api.openai.com/v1` | API key (Bearer token) |
| `anthropic` | Anthropic API (Claude) | `https://api.anthropic.com/v1` | API key (x-api-key header) |
| `vertex` | Anthropic models via Google Vertex AI | Google Cloud regional endpoint | `gcloud` OAuth2 token |

## Defining Providers

Providers are defined as named blocks in `~/.nash/config.toml`. Each block
creates a reusable provider that can be referenced by name in `[routing]`.

```toml
# Local llama.cpp server (no auth required)
[providers.local]
type = "local"
api_base = "http://localhost:8080"

# OpenAI API
[providers.openai-gpt5]
type = "openai"
model_id = "gpt-5"
api_key_env = "OPENAI_API_KEY"   # env var containing the key (default)

# Direct Anthropic API
[providers.anthropic-opus]
type = "anthropic"
model_id = "claude-opus-4-6"
api_key_env = "ANTHROPIC_API_KEY"  # env var containing the key (default)
caching = true                     # enable prompt caching

# Anthropic via Vertex AI
[providers.vertex-opus]
type = "vertex"
model_id = "claude-opus-4-6"
project_id = "my-gcp-project"
region = "global"                  # "global" or a region like "us-east5"
# caching is NOT supported on Vertex (rejected with HTTP 400)
```

### Configuration Fields

| Field | Required | Types | Description |
|-------|----------|-------|-------------|
| `type` | Yes | all | Provider type: `local`, `openai`, `anthropic`, `vertex` |
| `model_id` | No | all | Model identifier for API calls |
| `api_base` | No | local, openai, anthropic | Base URL (overrides default endpoint) |
| `api_key_env` | No | openai, anthropic | Name of env var holding the API key |
| `project_id` | No | vertex | Google Cloud project ID |
| `region` | No | vertex | Vertex AI region (default: `"global"`) |
| `context_size` | No | all | Context window size in tokens (0 = auto-detect) |
| `chars_per_token` | No | all | Characters per token ratio (default: 3.5) |
| `caching` | No | anthropic | Enable prompt caching (direct Anthropic only) |

## Routing

The `[routing]` section controls which named provider handles each role.
At minimum, set `default`. All other roles fall back to `default` if unset.

```toml
[routing]
default = "local"              # required when [providers.*] blocks exist
# planner = "vertex-opus"      # planning steps (step 0, replan)
# worker = "local"             # execution steps
# reflection = "vertex-opus"   # post-task reflection
# consolidation = "local"      # memory consolidation (dreaming)
```

This lets you use a powerful cloud model for planning while keeping a fast
local model for execution, or vice versa.

## Authentication

Nash supports three ways to provide API keys, checked in this order
(highest priority first):

1. **Environment variable** -- set the var named in `api_key_env` directly
2. **credentials.toml** -- store keys in `~/.nash/credentials.toml`
3. **Config fallback** -- provider-specific env var defaults (see below)

### credentials.toml (recommended)

Store API keys separately from config to avoid accidental commits:

```toml
# ~/.nash/credentials.toml (chmod 600!)
[providers.anthropic-opus]
api_key = "sk-ant-..."

[providers.openai-gpt5]
api_key = "sk-..."
```

Nash warns if this file has permissions more open than `0600`. Keys from
credentials.toml are set as temporary `NASH_CRED_*` environment variables,
then scrubbed from the environment after provider initialization to prevent
leaking to child processes (`shell_exec`, `git`).

### Provider-Specific Defaults

If no `api_key_env` is configured, Nash falls back to well-known env vars:

| Provider | Env Var Defaults |
|----------|-----------------|
| `openai` | `OPENAI_API_KEY` for auth, `OPENAI_MODEL` for model_id |
| `anthropic` | `ANTHROPIC_API_KEY` for auth, `ANTHROPIC_MODEL` for model_id |
| `vertex` | `CLOUD_ML_REGION` for region, `ANTHROPIC_VERTEX_PROJECT_ID` for project_id, `ANTHROPIC_MODEL` for model_id |
| `local` | No auth required |

### Vertex AI Authentication

Vertex AI does not use API keys. Instead, Nash calls
`gcloud auth print-access-token` to obtain an OAuth2 bearer token.
The token is cached for 10 minutes and refreshed automatically.

Prerequisites:

* Install the [Google Cloud CLI](https://cloud.google.com/sdk/docs/install)
* Run `gcloud auth login` and `gcloud config set project PROJECT_ID`
* The gcloud command must complete within 15 seconds or the request fails

## Provider-Specific Notes

**Local** -- Sends llama.cpp-specific extensions: `chat_template_kwargs`
(for thinking mode) and `reasoning_budget` (for thinking token budgets).
Auto-detects context size via the server's `/props` endpoint.

**OpenAI** -- Uses `max_completion_tokens` (not `max_tokens`) per OpenAI API
requirements. Sends strict mode (`additionalProperties: false`) in tool
schemas.

**Anthropic** -- Uses `input_schema` format for tool definitions (not
`parameters`). Supports extended thinking and prompt caching (via the
`anthropic-beta: prompt-caching-2024-07-31` header). System prompt is sent
as a separate parameter, not as a message.

**Vertex** -- Shares the Anthropic provider implementation internally.
Prompt caching is not available on Vertex (the `anthropic-beta` header is
rejected). Endpoints are constructed automatically from project_id, region,
and model_id. When region is `"global"`, uses
`aiplatform.googleapis.com`; otherwise uses
`{region}-aiplatform.googleapis.com`.

## See Also

* [Configuration Reference](configuration.md) -- full config.toml documentation
* [Model Profiles](model-profiles.md) -- per-model tuning (chars_per_token, thinking, tools)
* [Building & Usage](building.md) -- `nash --setup` runs an interactive provider wizard
