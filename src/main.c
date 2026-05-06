#include "agent_session.h"
#include "bus.h"
#include "cJSON.h"
#include "log.h"
#include "termbox2.h"
#include "ui_dashboard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_AGENTS 8
#define SPAWN_MIN_INTERVAL_MS 3000  /* don't spawn cc more than once per 3s */
#define REDRAW_INTERVAL_MS    200

typedef struct {
    AgentSession *session;
    char          id[16];
    AgentBackend  backend;
} ManagedAgent;

typedef enum {
    UI_DASHBOARD,
    UI_INPUT_SPAWN_PROMPT,
} UiMode;

static int64_t now_ms_monotonic(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* --- key/event pretty-printer for the debug log ----------------------- */

static const char *known_key_name(uint16_t k) {
    switch (k) {
    case TB_KEY_F1:           return "F1";
    case TB_KEY_F2:           return "F2";
    case TB_KEY_F3:           return "F3";
    case TB_KEY_F4:           return "F4";
    case TB_KEY_F5:           return "F5";
    case TB_KEY_F6:           return "F6";
    case TB_KEY_F7:           return "F7";
    case TB_KEY_F8:           return "F8";
    case TB_KEY_F9:           return "F9";
    case TB_KEY_F10:          return "F10";
    case TB_KEY_F11:          return "F11";
    case TB_KEY_F12:          return "F12";
    case TB_KEY_INSERT:       return "Insert";
    case TB_KEY_DELETE:       return "Delete";
    case TB_KEY_HOME:         return "Home";
    case TB_KEY_END:          return "End";
    case TB_KEY_PGUP:         return "PgUp";
    case TB_KEY_PGDN:         return "PgDn";
    case TB_KEY_ARROW_UP:     return "Up";
    case TB_KEY_ARROW_DOWN:   return "Down";
    case TB_KEY_ARROW_LEFT:   return "Left";
    case TB_KEY_ARROW_RIGHT:  return "Right";
    case TB_KEY_BACK_TAB:     return "Shift+Tab";
    case TB_KEY_ESC:          return "Esc";
    case TB_KEY_ENTER:        return "Enter";
    case TB_KEY_TAB:          return "Tab";
    case TB_KEY_BACKSPACE:    return "Backspace";
    case TB_KEY_BACKSPACE2:   return "Backspace";
    case TB_KEY_SPACE:        return "Space";
    default: return NULL;
    }
}

static void describe_event(const struct tb_event *ev, char *out, size_t cap) {
    const char *prefix = "";
    bool show_mod = (ev->mod != 0)
                  && !(ev->key == TB_KEY_TAB   && ev->mod == TB_MOD_CTRL)
                  && !(ev->key == TB_KEY_ENTER && ev->mod == TB_MOD_CTRL);
    char modbuf[32] = "";
    if (show_mod) {
        size_t n = 0;
        if (ev->mod & TB_MOD_CTRL)  n += (size_t)snprintf(modbuf + n, sizeof(modbuf) - n, "Ctrl+");
        if (ev->mod & TB_MOD_ALT)   n += (size_t)snprintf(modbuf + n, sizeof(modbuf) - n, "Alt+");
        if (ev->mod & TB_MOD_SHIFT) n += (size_t)snprintf(modbuf + n, sizeof(modbuf) - n, "Shift+");
        prefix = modbuf;
    }

    if (ev->type == TB_EVENT_RESIZE) {
        snprintf(out, cap, "Resize w=%d h=%d", ev->w, ev->h);
        return;
    }
    if (ev->type == TB_EVENT_MOUSE) {
        snprintf(out, cap, "Mouse x=%d y=%d", ev->x, ev->y);
        return;
    }

    const char *name = known_key_name(ev->key);
    if (name) {
        snprintf(out, cap, "%s%s [key=0x%04x]", prefix, name, ev->key);
        return;
    }
    if (ev->ch != 0) {
        if (ev->ch >= 0x20 && ev->ch < 0x7f) {
            snprintf(out, cap, "%s'%c' [ch=0x%02x]", prefix, (char)ev->ch, ev->ch);
        } else {
            snprintf(out, cap, "%sU+%04X [ch=0x%08x]", prefix, ev->ch, ev->ch);
        }
        return;
    }
    snprintf(out, cap, "%sUnknown [key=0x%04x ch=0x%08x mod=0x%02x]",
             prefix, ev->key, ev->ch, ev->mod);
}

/* --- argv parsing ----------------------------------------------------- */

static const char *parse_debug_flag(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0) {
            if (i + 1 < argc && argv[i + 1][0] != '-') return argv[i + 1];
            return "/tmp/ac.log";
        }
        if (strncmp(argv[i], "--debug=", 8) == 0) return argv[i] + 8;
    }
    return NULL;
}

/* --- send a user message as stream-json over the child's stdin -------- */

static int send_user_message(AgentSession *s, const char *text) {
    cJSON *root    = cJSON_CreateObject();
    cJSON *message = cJSON_CreateObject();
    cJSON *content = cJSON_CreateArray();
    cJSON *block   = cJSON_CreateObject();

    cJSON_AddStringToObject(root,    "type", "user");
    cJSON_AddStringToObject(message, "role", "user");
    cJSON_AddStringToObject(block,   "type", "text");
    cJSON_AddStringToObject(block,   "text", text);
    cJSON_AddItemToArray(content, block);
    cJSON_AddItemToObject(message, "content", content);
    cJSON_AddItemToObject(root, "message", message);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return -1;

    size_t len = strlen(json);
    int rc1 = (int)agent_session_send(s, json, len);
    int rc2 = (int)agent_session_send(s, "\n", 1);
    cJSON_free(json);
    if (rc1 < 0 || rc2 < 0) return -1;
    return 0;
}

/* --- spawn helpers ---------------------------------------------------- */

static int64_t g_last_spawn_at = 0;

static const char *spawn_cc_with_prompt(ManagedAgent *agents, size_t *count,
                                        EventBus *bus, const char *prompt) {
    int64_t now = now_ms_monotonic();
    if (g_last_spawn_at != 0 && now - g_last_spawn_at < SPAWN_MIN_INTERVAL_MS) {
        static char msg[64];
        snprintf(msg, sizeof(msg), "rate-limited (wait %lldms)",
                 (long long)(SPAWN_MIN_INTERVAL_MS - (now - g_last_spawn_at)));
        return msg;
    }
    if (*count >= MAX_AGENTS) return "too many agents (limit 8)";
    if (!prompt || !*prompt) return "empty prompt -- nothing to spawn";

    static char id[16];
    snprintf(id, sizeof(id), "cc-%zu", *count + 1);

    /* Interactive multi-turn stream-json mode: claude reads user-message
     * JSON lines from stdin and emits events on stdout until stdin EOF. */
    const char *argv[] = {
        "claude", "-p",
        "--input-format=stream-json",
        "--output-format=stream-json",
        "--verbose",
        "--permission-mode=dontAsk",
        NULL,
    };
    ProcSpawnOpts opts = { .argv = argv };

    /* Log the full argv exactly as it will be exec'd. Quote every arg with
     * single quotes so the log can be copy-pasted into a shell. */
    {
        char cmd[1024];
        size_t n = 0;
        for (int i = 0; argv[i] != NULL; i++) {
            int wrote = snprintf(cmd + n, sizeof(cmd) - n,
                                 "%s'%s'", i == 0 ? "" : " ", argv[i]);
            if (wrote < 0 || (size_t)wrote >= sizeof(cmd) - n) break;
            n += (size_t)wrote;
        }
        LOG("spawn %s: %s", id, cmd);
        LOG("first prompt to %s: %.200s", id, prompt);
    }

    AgentSession *s = agent_session_spawn(AC_BACKEND_CC, id, &opts, bus);
    if (!s) {
        LOG("agent_session_spawn failed");
        return "spawn failed (is `claude` on PATH?)";
    }

    if (send_user_message(s, prompt) != 0) {
        LOG("failed to write initial prompt to %s", id);
    }

    agents[*count].session = s;
    snprintf(agents[*count].id, sizeof(agents[*count].id), "%s", id);
    agents[*count].backend = AC_BACKEND_CC;
    (*count)++;
    g_last_spawn_at = now;

    static char ok[64];
    snprintf(ok, sizeof(ok), "spawned %s", id);
    return ok;
}

/* --- input-mode rendering --------------------------------------------- */

static void render_input_box(const char *label, const char *buf, size_t cursor_pos) {
    int row = tb_height() - 2;
    int width = tb_width();
    /* Clear the row first with the input-box bg colour. */
    for (int x = 0; x < width; x++) tb_set_cell(x, row, ' ', TB_BLACK, TB_GREEN);

    char display[1200];
    snprintf(display, sizeof(display), "%s%s", label, buf);
    ui_put_str(0, row, width, TB_BLACK, TB_GREEN, display);

    int cursor_x = (int)strlen(label) + (int)cursor_pos;
    if (cursor_x < width) {
        tb_set_cell(cursor_x, row, ' ', TB_BLACK, TB_WHITE);
    }
}

/* --- main ------------------------------------------------------------- */

int main(int argc, char **argv) {
    const char *debug_path = parse_debug_flag(argc, argv);
    if (debug_path) {
        if (!log_open(debug_path)) {
            fprintf(stderr, "warning: could not open debug log %s\n", debug_path);
        } else {
            fprintf(stderr, "debug log: %s\n", debug_path);
            LOG("=== ac started ===");
        }
    }

    EventBus *bus = bus_create(512);
    if (!bus) { fprintf(stderr, "bus_create failed\n"); return 1; }

    ManagedAgent agents[MAX_AGENTS] = {0};
    size_t       agent_count = 0;

    DashboardAgent dash_agents[MAX_AGENTS];
    DashboardState state = {
        .agents = dash_agents,
        .agent_count = 0,
        .bus = bus,
        .focused_agent = 0,
    };

    int rc = tb_init();
    if (rc != 0) {
        fprintf(stderr, "tb_init failed: %d\n", rc);
        bus_free(bus);
        return 1;
    }

    UiMode ui_mode = UI_DASHBOARD;
    char input_buf[1024] = "";
    size_t input_len = 0;

    char status_msg[160] = "Press F2 to spawn a cc agent. F1 for help.";
    bool running = true;

    while (running) {
        for (size_t i = 0; i < agent_count; i++) {
            dash_agents[i].id = agents[i].id;
            dash_agents[i].backend = agents[i].backend;
            dash_agents[i].status = agent_session_status(agents[i].session);
            dash_agents[i].current_tool = agent_session_current_tool(agents[i].session);
        }
        state.agent_count = agent_count;
        if (state.focused_agent >= agent_count && agent_count > 0)
            state.focused_agent = agent_count - 1;

        tb_clear();
        ui_dashboard_render(&state);

        if (ui_mode == UI_INPUT_SPAWN_PROMPT) {
            render_input_box("prompt> ", input_buf, input_len);
        } else if (status_msg[0]) {
            ui_put_str(0, tb_height() - 2, tb_width(),
                       TB_YELLOW, TB_DEFAULT, status_msg);
        }
        tb_present();

        struct tb_event ev;
        int pe = tb_peek_event(&ev, REDRAW_INTERVAL_MS);
        if (pe == TB_ERR_NO_EVENT) continue;
        if (pe != TB_OK) break;

        char ev_desc[96];
        describe_event(&ev, ev_desc, sizeof(ev_desc));
        if (ev.type != TB_EVENT_KEY) {
            LOG("%-28s -> ignored (non-key)", ev_desc);
            continue;
        }

        /* Input mode: route everything to the prompt buffer. */
        if (ui_mode == UI_INPUT_SPAWN_PROMPT) {
            const char *act = "input";
            if (ev.key == TB_KEY_ESC) {
                ui_mode = UI_DASHBOARD;
                input_buf[0] = '\0'; input_len = 0;
                snprintf(status_msg, sizeof(status_msg), "spawn cancelled");
                act = "spawn cancelled";
            } else if (ev.key == TB_KEY_ENTER) {
                if (input_len > 0) {
                    const char *r = spawn_cc_with_prompt(agents, &agent_count, bus, input_buf);
                    snprintf(status_msg, sizeof(status_msg), "%s", r);
                    act = r;
                } else {
                    snprintf(status_msg, sizeof(status_msg),
                             "empty prompt -- nothing to spawn");
                    act = "empty prompt";
                }
                ui_mode = UI_DASHBOARD;
                input_buf[0] = '\0'; input_len = 0;
            } else if (ev.key == TB_KEY_BACKSPACE || ev.key == TB_KEY_BACKSPACE2) {
                if (input_len > 0) input_buf[--input_len] = '\0';
                act = "backspace";
            } else if (ev.ch >= 0x20 && ev.ch < 0x7f
                       && input_len < sizeof(input_buf) - 1) {
                input_buf[input_len++] = (char)ev.ch;
                input_buf[input_len] = '\0';
                act = "typed";
            }
            LOG("%-28s -> [input] %s (buf len=%zu)", ev_desc, act, input_len);
            continue;
        }

        status_msg[0] = '\0';
        const char *action = "unmapped";

        if (ev.key == TB_KEY_ESC || ev.ch == 'q' || ev.key == TB_KEY_F10) {
            running = false;
            action = "quit";
        } else if (ev.key == TB_KEY_TAB || ev.ch == '\t' || ev.ch == 9) {
            if (agent_count > 0) {
                size_t prev = state.focused_agent;
                state.focused_agent = (state.focused_agent + 1) % agent_count;
                snprintf(status_msg, sizeof(status_msg),
                         "focus -> agent %zu", state.focused_agent + 1);
                static char act[48];
                snprintf(act, sizeof(act), "focus %zu->%zu", prev + 1, state.focused_agent + 1);
                action = act;
            } else {
                snprintf(status_msg, sizeof(status_msg),
                         "no agents yet -- press F2 to spawn");
                action = "no agents";
            }
        } else if (ev.ch >= '1' && ev.ch <= '9') {
            size_t idx = (size_t)(ev.ch - '1');
            if (idx < agent_count) {
                state.focused_agent = idx;
                snprintf(status_msg, sizeof(status_msg),
                         "focus -> agent %zu", idx + 1);
                static char act[32];
                snprintf(act, sizeof(act), "focus -> %zu", idx + 1);
                action = act;
            } else {
                snprintf(status_msg, sizeof(status_msg), "no agent %zu", idx + 1);
                action = "no such agent";
            }
        } else if (ev.key == TB_KEY_F1) {
            snprintf(status_msg, sizeof(status_msg),
                     "F2 spawn (prompt)  Tab/1-9 focus  F10/q quit  (drilldown coming)");
            action = "help";
        } else if (ev.key == TB_KEY_F2) {
            ui_mode = UI_INPUT_SPAWN_PROMPT;
            input_buf[0] = '\0'; input_len = 0;
            action = "spawn dialog";
        } else if (ev.key == TB_KEY_F3) {
            snprintf(status_msg, sizeof(status_msg), "F3 drill -- not implemented yet");
            action = "drill (stub)";
        } else if (ev.key == TB_KEY_F4) {
            snprintf(status_msg, sizeof(status_msg), "F4 filter -- not implemented yet");
            action = "filter (stub)";
        } else if (ev.key == TB_KEY_F5) {
            snprintf(status_msg, sizeof(status_msg), "F5 paste -- not implemented yet");
            action = "paste (stub)";
        } else if (ev.key == TB_KEY_F6) {
            snprintf(status_msg, sizeof(status_msg), "F6 curate -- not implemented yet");
            action = "curate (stub)";
        } else if (ev.key == TB_KEY_F7) {
            snprintf(status_msg, sizeof(status_msg), "F7 inspect -- not implemented yet");
            action = "inspect (stub)";
        } else if (ev.key == TB_KEY_F8) {
            snprintf(status_msg, sizeof(status_msg), "F8 kill -- not implemented yet");
            action = "kill (stub)";
        } else if (ev.key == TB_KEY_F9) {
            snprintf(status_msg, sizeof(status_msg), "F9 menu -- deferred to v2");
            action = "menu (v2)";
        } else {
            snprintf(status_msg, sizeof(status_msg), "%s", ev_desc);
        }

        LOG("%-28s -> %s", ev_desc, action);
    }

    for (size_t i = 0; i < agent_count; i++) {
        if (agents[i].session) {
            agent_session_kill(agents[i].session, 9 /*SIGKILL*/);
            agent_session_free(agents[i].session);
        }
    }

    tb_shutdown();
    LOG("=== ac stopped ===");
    log_close();
    bus_free(bus);
    return 0;
}
