# Multi-Provider Support

Nash supports four LLM providers through a unified vtable interface:

| Provider | Endpoint | Features |
|----------|----------|----------|
| **Local** (llama.cpp) | Any OpenAI-compatible server | `chat_template_kwargs`, `reasoning_budget` |
| **OpenAI** | `api.openai.com` | Strict mode (`additionalProperties: false`), prompt caching |
| **Anthropic** | `api.anthropic.com` | `input_schema` format, extended thinking, prompt caching |
| **Vertex AI** | Google Cloud | Anthropic-on-Vertex via `gcloud auth` token |

All providers share the same tool registry and SSE streaming infrastructure. Provider-specific differences (JSON structure, auth headers, error formats) are encapsulated in the vtable.
