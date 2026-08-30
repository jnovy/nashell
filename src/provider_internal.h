#ifndef PROVIDER_INTERNAL_H
#define PROVIDER_INTERNAL_H

#include "provider.h"

/* Provider-specific vtable initializers.
 * Called by provider_new() to wire up the function pointers. */
void provider_local_init(provider_t *p);
void provider_openai_init(provider_t *p);
void provider_anthropic_init(provider_t *p);

#endif
