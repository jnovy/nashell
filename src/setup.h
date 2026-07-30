#ifndef SETUP_H
#define SETUP_H

/* Interactive first-time setup wizard.
 * Prompts the user to configure providers, routing, embedding, and
 * writes ~/.nash/config.toml + ~/.nash/credentials.toml.
 *
 * nash_dir: path to ~/.nash/ (already created by caller)
 * add_only: if non-zero, only add a provider to existing config
 *           (--setup --add-provider mode)
 *
 * Returns 0 on success, non-zero on error or user abort. */
int setup_run(const char *nash_dir, int add_only);

#endif
