#ifndef AC_EVENTS_H
#define AC_EVENTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    AC_EV_SYSTEM_INIT,
    AC_EV_HOOK,
    AC_EV_ASSISTANT_TEXT_DELTA,
    AC_EV_ASSISTANT_TEXT_DONE,
    AC_EV_THINKING_DELTA,
    AC_EV_TOOL_CALL_STARTED,
    AC_EV_TOOL_CALL_FINISHED,
    AC_EV_RATE_LIMIT,
    AC_EV_STATUS_CHANGE,
    AC_EV_ERROR,
    AC_EV_TURN_DONE,
} AgentEventKind;

typedef struct {
    AgentEventKind kind;
    union {
        struct {
            const char *model;
            const char *session_id;
        } system_init;
        struct {
            const char *hook_name;
            const char *hook_event;
            bool        is_response;
            int         exit_code;
        } hook;
        struct {
            const char *text;
        } text_delta;
        struct {
            const char *text;       /* thinking text; signature stripped */
        } thinking_delta;
        struct {
            const char *call_id;
            const char *name;
            const char *args_json;  /* compact JSON of input object */
            const char *parent_tool_use_id;
        } tool_call_started;
        struct {
            const char *call_id;
            bool        ok;
            const char *result_summary;
            const char *parent_tool_use_id;
        } tool_call_finished;
        struct {
            const char *window;     /* e.g. "five_hour" */
            uint64_t    reset_at;
        } rate_limit;
        struct {
            const char *new_status;
        } status_change;
        struct {
            const char *message;
        } error_;
    } as;
} AgentEvent;

typedef void (*AgentEventEmit)(const AgentEvent *ev, void *user);

#endif
