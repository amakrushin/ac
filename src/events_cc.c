#include "events_cc.h"

#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

static const char *cstr_or_null(const cJSON *node) {
    if (!node || !cJSON_IsString(node)) return NULL;
    return node->valuestring;
}

static int parse_system(const cJSON *root, AgentEventEmit emit, void *user) {
    const char *subtype = cstr_or_null(cJSON_GetObjectItemCaseSensitive(root, "subtype"));
    if (!subtype) return 0;

    if (strcmp(subtype, "init") == 0) {
        AgentEvent ev = {
            .kind = AC_EV_SYSTEM_INIT,
            .as.system_init = {
                .model      = cstr_or_null(cJSON_GetObjectItemCaseSensitive(root, "model")),
                .session_id = cstr_or_null(cJSON_GetObjectItemCaseSensitive(root, "session_id")),
            },
        };
        emit(&ev, user);
        return 1;
    }

    if (strcmp(subtype, "hook_started") == 0 || strcmp(subtype, "hook_response") == 0) {
        AgentEvent ev = {
            .kind = AC_EV_HOOK,
            .as.hook = {
                .hook_name   = cstr_or_null(cJSON_GetObjectItemCaseSensitive(root, "hook_name")),
                .hook_event  = cstr_or_null(cJSON_GetObjectItemCaseSensitive(root, "hook_event")),
                .is_response = strcmp(subtype, "hook_response") == 0,
                .exit_code   = 0,
            },
        };
        if (ev.as.hook.is_response) {
            const cJSON *exit_code = cJSON_GetObjectItemCaseSensitive(root, "exit_code");
            if (cJSON_IsNumber(exit_code)) ev.as.hook.exit_code = (int)exit_code->valuedouble;
        }
        emit(&ev, user);
        return 1;
    }

    return 0;
}

static int parse_assistant(const cJSON *root, AgentEventEmit emit, void *user) {
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(root, "message");
    if (!cJSON_IsObject(message)) return 0;
    const cJSON *parent_id = cJSON_GetObjectItemCaseSensitive(root, "parent_tool_use_id");
    const char  *parent    = cstr_or_null(parent_id);

    const cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
    if (!cJSON_IsArray(content)) return 0;

    int emitted = 0;
    const cJSON *block = NULL;
    cJSON_ArrayForEach(block, content) {
        const char *btype = cstr_or_null(cJSON_GetObjectItemCaseSensitive(block, "type"));
        if (!btype) continue;

        if (strcmp(btype, "text") == 0) {
            const char *text = cstr_or_null(cJSON_GetObjectItemCaseSensitive(block, "text"));
            if (text) {
                AgentEvent ev = {
                    .kind = AC_EV_ASSISTANT_TEXT_DELTA,
                    .as.text_delta = { .text = text },
                };
                emit(&ev, user);
                emitted++;
            }
        } else if (strcmp(btype, "thinking") == 0) {
            /* Strip the (massive) `signature` field by reading only `thinking`. */
            const char *thinking = cstr_or_null(cJSON_GetObjectItemCaseSensitive(block, "thinking"));
            AgentEvent ev = {
                .kind = AC_EV_THINKING_DELTA,
                .as.thinking_delta = { .text = thinking ? thinking : "" },
            };
            emit(&ev, user);
            emitted++;
        } else if (strcmp(btype, "tool_use") == 0) {
            const char  *call_id = cstr_or_null(cJSON_GetObjectItemCaseSensitive(block, "id"));
            const char  *name    = cstr_or_null(cJSON_GetObjectItemCaseSensitive(block, "name"));
            const cJSON *input   = cJSON_GetObjectItemCaseSensitive(block, "input");
            char *args_json = input ? cJSON_PrintUnformatted(input) : NULL;
            AgentEvent ev = {
                .kind = AC_EV_TOOL_CALL_STARTED,
                .as.tool_call_started = {
                    .call_id            = call_id,
                    .name               = name,
                    .args_json          = args_json ? args_json : "",
                    .parent_tool_use_id = parent,
                },
            };
            emit(&ev, user);
            emitted++;
            if (args_json) cJSON_free(args_json);
        }
    }
    return emitted;
}

static int parse_user(const cJSON *root, AgentEventEmit emit, void *user) {
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(root, "message");
    if (!cJSON_IsObject(message)) return 0;
    const cJSON *parent_id = cJSON_GetObjectItemCaseSensitive(root, "parent_tool_use_id");
    const char  *parent    = cstr_or_null(parent_id);

    const cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
    if (!cJSON_IsArray(content)) return 0;

    int emitted = 0;
    const cJSON *block = NULL;
    cJSON_ArrayForEach(block, content) {
        const char *btype = cstr_or_null(cJSON_GetObjectItemCaseSensitive(block, "type"));
        if (!btype || strcmp(btype, "tool_result") != 0) continue;

        const char *call_id = cstr_or_null(cJSON_GetObjectItemCaseSensitive(block, "tool_use_id"));
        const cJSON *is_error = cJSON_GetObjectItemCaseSensitive(block, "is_error");
        const cJSON *content_node = cJSON_GetObjectItemCaseSensitive(block, "content");

        const char *summary = NULL;
        char       *summary_owned = NULL;
        if (cJSON_IsString(content_node)) {
            summary = content_node->valuestring;
        } else if (content_node) {
            summary_owned = cJSON_PrintUnformatted(content_node);
            summary = summary_owned ? summary_owned : "";
        }

        AgentEvent ev = {
            .kind = AC_EV_TOOL_CALL_FINISHED,
            .as.tool_call_finished = {
                .call_id            = call_id,
                .ok                 = !(cJSON_IsBool(is_error) && cJSON_IsTrue(is_error)),
                .result_summary     = summary ? summary : "",
                .parent_tool_use_id = parent,
            },
        };
        emit(&ev, user);
        emitted++;
        if (summary_owned) cJSON_free(summary_owned);
    }
    return emitted;
}

static int parse_rate_limit(const cJSON *root, AgentEventEmit emit, void *user) {
    const cJSON *info = cJSON_GetObjectItemCaseSensitive(root, "rate_limit_info");
    if (!cJSON_IsObject(info)) return 0;
    const cJSON *resets = cJSON_GetObjectItemCaseSensitive(info, "resetsAt");
    AgentEvent ev = {
        .kind = AC_EV_RATE_LIMIT,
        .as.rate_limit = {
            .window   = cstr_or_null(cJSON_GetObjectItemCaseSensitive(info, "rateLimitType")),
            .reset_at = cJSON_IsNumber(resets) ? (uint64_t)resets->valuedouble : 0,
        },
    };
    emit(&ev, user);
    return 1;
}

static int parse_result(const cJSON *root, AgentEventEmit emit, void *user) {
    (void)root;
    AgentEvent ev = { .kind = AC_EV_TURN_DONE };
    emit(&ev, user);
    return 1;
}

int events_cc_parse_one(const char *json, size_t len,
                        AgentEventEmit emit, void *user) {
    if (!json || !emit) return -1;
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return -1;

    int n = 0;
    const char *type = cstr_or_null(cJSON_GetObjectItemCaseSensitive(root, "type"));
    if (type) {
        if      (strcmp(type, "system")           == 0) n = parse_system(root, emit, user);
        else if (strcmp(type, "assistant")        == 0) n = parse_assistant(root, emit, user);
        else if (strcmp(type, "user")             == 0) n = parse_user(root, emit, user);
        else if (strcmp(type, "rate_limit_event") == 0) n = parse_rate_limit(root, emit, user);
        else if (strcmp(type, "result")           == 0) n = parse_result(root, emit, user);
    }

    cJSON_Delete(root);
    return n;
}
