#ifndef TOOLS_REGISTRY_H
#define TOOLS_REGISTRY_H

#include "cJSON.h"
#include "provider.h"

/* Shared tool definition — defined once, formatted per-provider.
 * Eliminates the 3x duplication of tool definitions across
 * provider_local.c, provider_openai.c, and provider_anthropic.c.
 * Also replaces llm.c:build_tools_array() — single source of truth. */

typedef struct {
    const char *name;
    const char *description;
    const char *params_json;   /* JSON schema string for parameters */
} tool_def_t;

/* The canonical tool registry — single source of truth for all tools. */
static const tool_def_t TOOL_REGISTRY[] = {
    {"shell_exec",
     "Execute a shell command (git, make, docker, gh, npm, etc.). "
     "For file reading use file_read, for content search use grep_search, "
     "for file search use glob_search, for URL fetching use web_fetch. "
     "Limit output: pipe through head -50, tail, jq, grep. "
     "For servers/daemons, set background=true.",
     "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\",\"description\":\"Shell command\"},\"background\":{\"type\":\"boolean\",\"description\":\"Start as background process (for servers/daemons). Returns immediately with PID.\",\"default\":false}},\"required\":[\"command\"]}"},

    {"file_read",
     "Read contents of a file. Supports line ranges to avoid reading entire large files. "
     "Use start_line/end_line for specific sections (1-based). "
     "Negative start_line reads from end (e.g., -20 = last 20 lines).",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"File path\"},\"start_line\":{\"type\":\"integer\",\"description\":\"First line to read (1-based, default: 1). Negative = from end (-20 = last 20 lines)\"},\"end_line\":{\"type\":\"integer\",\"description\":\"Last line to read (1-based inclusive, default: EOF)\"}},\"required\":[\"path\"]}"},

    {"file_write",
     "Write content to a file (under workspace dir).",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"File path\"},\"content\":{\"type\":\"string\",\"description\":\"File content\"}},\"required\":[\"path\",\"content\"]}"},

    {"file_edit",
     "Edit a file by replacing exact text. Always file_read first to copy exact text.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"File path\"},\"old_text\":{\"type\":\"string\",\"description\":\"Exact text to find (must match)\"},\"new_text\":{\"type\":\"string\",\"description\":\"Replacement text\"}},\"required\":[\"path\",\"old_text\",\"new_text\"]}"},

    {"grep_search",
     "Search file contents with a regex pattern.",
     "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\",\"description\":\"Regex pattern\"},\"path\":{\"type\":\"string\",\"description\":\"Directory or file to search in\"}},\"required\":[\"pattern\"]}"},

    {"web_fetch",
     "Fetch content from a URL.",
     "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\",\"description\":\"URL to fetch\"}},\"required\":[\"url\"]}"},

    {"web_search",
     "Search the web for information.",
     "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Search query\"}},\"required\":[\"query\"]}"},

    {"glob_search",
     "Search for files matching a glob pattern.",
     "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\",\"description\":\"Glob pattern (e.g. **/*.py)\"}},\"required\":[\"pattern\"]}"},

    {"memory_store",
     "Store reusable knowledge in long-term memory.",
     "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"description\":\"Memory key\"},\"value\":{\"type\":\"string\",\"description\":\"Content to store\"},\"tags\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Tags for search\"}},\"required\":[\"key\",\"value\"]}"},

    {"memory_recall",
     "Recall information from long-term memory.",
     "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"description\":\"Exact key to recall\"},\"query\":{\"type\":\"string\",\"description\":\"Search query\"}}}"},

    {"memory_pin",
     "Pin an existing memory so it is always injected into the system prompt.",
     "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"description\":\"Memory key to pin\"}},\"required\":[\"key\"]}"},

    {"memory_unpin",
     "Unpin a memory so it is no longer always injected into the system prompt.",
     "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"description\":\"Memory key to unpin\"}},\"required\":[\"key\"]}"},

    {"done",
     "Signal task completion. Include all concrete data (paths, numbers, URLs) in result.",
     "{\"type\":\"object\",\"properties\":{\"result\":{\"type\":\"string\",\"description\":\"Complete answer with details\"}},\"required\":[\"result\"]}"},

    {"plan",
     "Outline a numbered execution plan (3-8 steps) before starting work.",
     "{\"type\":\"object\",\"properties\":{\"result\":{\"type\":\"string\",\"description\":\"Numbered plan: 1. step (tool)\\n2. ...\"}},\"required\":[\"result\"]}"},

    {"notes",
     "Persistent scratchpad that survives context compaction. "
     "Supports section-based ops: notes(op=\"write\", section=\"name\", content=\"...\", priority=N) to write a section, "
     "notes(op=\"append\", section=\"name\", content=\"...\") to append, "
     "notes(op=\"clear\", section=\"name\") to delete a section, "
     "notes(op=\"list\") to list all sections. "
     "Legacy: notes(content=\"...\") still works (replaces all). Priority 1=highest, 9=lowest (default 5).",
     "{\"type\":\"object\",\"properties\":{\"content\":{\"type\":\"string\",\"description\":\"Full scratchpad content (legacy mode) or section content (with op).\"},\"op\":{\"type\":\"string\",\"description\":\"Operation: write, append, read, clear, list\"},\"section\":{\"type\":\"string\",\"description\":\"Section name for write/append/read/clear\"},\"priority\":{\"type\":\"integer\",\"description\":\"Section priority 1-9 (1=highest, default 5)\"}}}"},

    {NULL, NULL, NULL}  /* sentinel */
};

#define TOOL_REGISTRY_COUNT 15

/* Build a cJSON tools array from the registry, formatted for the given provider type.
 * Handles the structural differences between Local/OpenAI/Anthropic APIs.
 * For OpenAI: adds "strict":true and "additionalProperties":false recursively.
 * For Anthropic: uses "input_schema" instead of "parameters" and no "function" wrapper. */
cJSON *build_tools_from_registry(provider_type_t type);

#endif /* TOOLS_REGISTRY_H */
