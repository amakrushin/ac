#ifndef AC_EVENTS_CC_H
#define AC_EVENTS_CC_H

#include "events.h"
#include <stddef.h>

/*
 * Parse a single top-level JSON object from cc's --output-format stream-json.
 * Synchronously invokes `emit` for each AgentEvent produced (zero or more),
 * passing `user` through. Strings inside the AgentEvent are valid only for
 * the duration of the callback.
 *
 * Returns the number of events emitted on success, or -1 on JSON parse error.
 */
int events_cc_parse_one(const char *json, size_t len,
                        AgentEventEmit emit, void *user);

#endif
