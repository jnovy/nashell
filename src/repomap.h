#ifndef REPOMAP_H
#define REPOMAP_H

#include <stddef.h>

/*
 * Repo Map — Structural codebase context for LLM consumption.
 *
 * Implements a simplified version of Aider's tree-sitter repo map:
 * 1. Symbol extraction via line-based C parsing (no tree-sitter dependency)
 * 2. Directed graph: referencing_file → defining_file (weighted edges)
 * 3. Personalized PageRank biased toward the user's query
 * 4. Rank distribution to individual symbols
 * 5. Budget fitting via greedy selection
 * 6. Scope-aware rendering with ⋮ elision
 *
 * The result is injected into the LLM context as read-only structural
 * information, helping the model understand the codebase architecture
 * without seeing full implementations.
 */

/* Build a repo map for the given directory.
 *
 * root_dir:    project root (NULL = current directory)
 * user_query:  the user's query (for relevance boosting, may be NULL)
 * chat_files:  array of file paths currently in the chat (for ×50 boost)
 * n_chat_files: number of chat_files entries
 * max_chars:   maximum characters in the output (0 = default 8000)
 *
 * Returns a malloc'd string with the rendered map, or NULL on failure.
 * Caller must free(). */
char *repomap_build(const char *root_dir, const char *user_query,
                    const char **chat_files, int n_chat_files,
                    int max_chars);

/* Extract definition symbols from in-memory file content.
 *
 * Reuses Phase 1 C/C++ parsing to extract function, type, and macro
 * definitions without any disk I/O.  Used by eviction breadcrumbs to
 * produce structural briefs instead of raw first-N-bytes.
 *
 * content:     file content (NUL-terminated)
 * content_len: length in bytes
 * filename:    original filename (extension used for language detection)
 * out:         output buffer (comma-separated symbol names)
 * out_cap:     output buffer capacity (including NUL)
 *
 * Returns chars written (0 if no symbols found or unsupported language).
 * Output is always NUL-terminated. */
int repomap_file_symbols(const char *content, int content_len,
                         const char *filename, char *out, int out_cap);

#endif /* REPOMAP_H */
