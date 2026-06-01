#ifndef NASH_LIMITS_H
#define NASH_LIMITS_H

/*
 * Central size/buffer constants for the Nash codebase.
 *
 * These are compile-time defaults for internal buffers and safety caps.
 * They are distinct from the runtime-configurable limits in config.h
 * (file_max_size, shell_max_output, etc.) which control user-facing
 * tool behavior.
 *
 * Rationale: eliminates magic number hardcodings (1048576, 65536, 4096,
 * 32768) scattered across dozens of files, making limits discoverable,
 * auditable, and changeable from one place.
 */

/* Max path buffer size for local file paths.
 * Used for stack-allocated path buffers (char path[NASH_PATH_MAX]).
 * 4096 matches Linux PATH_MAX and covers all practical paths. */
#define NASH_PATH_MAX       4096

/* Max single-line buffer for reading text line-by-line.
 * Used for fgets() buffers in journal parsing, react loop, UI state, etc.
 * 64KB accommodates even very long JSON-encoded lines. */
#define NASH_LINE_MAX       65536

/* Safety cap for reading entire files into memory (internal use).
 * Applied when reading checkpoint files, config files, markdown docs —
 * anything where we slurp the whole file. Prevents accidental multi-GB
 * allocations from corrupt/huge files.
 * 1MB is generous for any config/checkpoint/metadata file. */
#define NASH_FILE_READ_MAX  (1 * 1024 * 1024)

/* Initial allocation hint for dynamic string buffers (str_t).
 * Used when we expect moderate output (HTTP response bodies, etc.).
 * Not a hard limit — str_t grows as needed. */
#define NASH_INITIAL_BUF    32768

#endif /* NASH_LIMITS_H */
