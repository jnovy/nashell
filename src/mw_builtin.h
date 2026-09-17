/* mw_builtin.h - Built-in middleware hooks for tool dispatch.
 * Registered once at react_run() startup; run on every tool dispatch
 * via the pre/post hook chain in dispatch_handler() (tools.c). */
#ifndef MW_BUILTIN_H
#define MW_BUILTIN_H

/* Register all built-in middleware hooks.
 * Safe to call multiple times - skips if already registered. */
void mw_builtin_init(void);

#endif /* MW_BUILTIN_H */
