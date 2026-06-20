#include "tools_registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The canonical tool registry — single source of truth for all tools. */
const tool_def_t TOOL_REGISTRY[] = {
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
     "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"description\":\"Memory key\"},\"value\":{\"type\":\"string\",\"description\":\"Content to store\"},\"refs\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Related memory keys for cross-references\"},\"supersedes\":{\"type\":\"string\",\"description\":\"Key of the memory this entry replaces (lesson lineage tracking)\"},\"global\":{\"type\":\"boolean\",\"description\":\"Store in global memory instead of workspace (default: false)\"}},\"required\":[\"key\",\"value\"]}"},

    {"memory_recall",
     "Recall information from long-term memory. Use when the task may depend on user preferences, prior decisions, or historical context.",
     "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"description\":\"Exact key to recall\"},\"query\":{\"type\":\"string\",\"description\":\"Search query\"}}}"},

    {"memory_pin",
     "Pin an existing memory so it is always injected into the system prompt.",
     "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"description\":\"Memory key to pin\"}},\"required\":[\"key\"]}"},

    {"memory_unpin",
     "Unpin a memory so it is no longer always injected into the system prompt.",
     "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"description\":\"Memory key to unpin\"}},\"required\":[\"key\"]}"},

    {"done",
     "Signal task completion. Include all concrete data (paths, numbers, URLs) in result. "
     "The user CANNOT see notes/scratchpad — never say \"see above\" or reference data only in notes. "
     "Copy all relevant content (tables, lists, data) directly into the result text.",
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

    {"user_ask",
     "Ask the user a clarifying question. Use when you need information "
     "that cannot be determined from the codebase or context. The react loop "
     "pauses until the user responds. "
     "Prefer calling this EARLY (step 0-2) when the task is ambiguous, rather "
     "than guessing and discovering the wrong assumption later.",
     "{\"type\":\"object\",\"properties\":{\"question\":{\"type\":\"string\",\"description\":\"Question to ask the user\"}},\"required\":[\"question\"]}"},

    {"memory_delete",
     "Delete a memory entry by key.",
     "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"description\":\"Memory key to delete\"}},\"required\":[\"key\"]}"},

    {"memory_list",
     "List all memory keys grouped by type. Returns key names with descriptions. "
     "Use to browse available memories when memory_recall semantic search is too narrow.",
     "{\"type\":\"object\",\"properties\":{\"type\":{\"type\":\"string\",\"description\":\"Filter by type: lesson, strategy, skill, fact, task, anti-pattern, other. Omit for all.\"}}}"},

    {"image_analyze",
     "Analyze an image file using the LLM's vision capabilities. "
     "Reads the image, base64-encodes it, and sends it to the provider "
     "for multimodal analysis. Returns a textual description/analysis. "
     "Supports: png, jpg/jpeg, gif, webp, bmp, svg, tiff. Max 20 MB.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Path to the image file\"},\"question\":{\"type\":\"string\",\"description\":\"What to analyze or ask about the image (default: describe in detail)\"}},\"required\":[\"path\"]}"},

    {"session_grep",
     "Search past session journals for exact/substring matches. "
     "Use for finding specific function names, error codes, file paths, "
     "commands, or identifiers in historical sessions. Complements "
     "memory_recall (semantic search) with precise pattern matching. "
     "Searches journal.jsonl files (agent thoughts, tool calls, params) "
     "across recent sessions, newest first. Case-insensitive.",
     "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\",\"description\":\"Substring pattern to search for (case-insensitive)\"},\"max_results\":{\"type\":\"integer\",\"description\":\"Maximum matches to return (default: 20, max: 100)\"},\"days\":{\"type\":\"integer\",\"description\":\"Only search sessions up to N days old (default: all)\"}},\"required\":[\"pattern\"]}"},

    {NULL, NULL, NULL}  /* sentinel */
};

/* FIX #14: TOOL_REGISTRY_COUNT is now a #define in tools_registry.h */

/* Build a comma-separated list of all tool names from the registry.
 * Caller must free() the returned string. */
char *tool_registry_names_csv(void) {
    size_t total = 0;
    for (int i = 0; i < TOOL_REGISTRY_COUNT; i++) {
        total += strlen(TOOL_REGISTRY[i].name) + 2; /* ", " */
    }
    char *buf = malloc(total + 1);
    if (!buf) return NULL;
    buf[0] = '\0';
    for (int i = 0; i < TOOL_REGISTRY_COUNT; i++) {
        if (i > 0) strcat(buf, ", ");
        strcat(buf, TOOL_REGISTRY[i].name);
    }
    return buf;
}
