#include "ui_dashboard.h"

#include "termbox2.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* Decode one UTF-8 codepoint at *s into *cp; return advanced byte count.
 * Returns 1 on malformed bytes (treated as Latin-1 fallback). */
static int utf8_next(const char *s, uint32_t *cp) {
    int n = tb_utf8_char_length(*s);
    if (n <= 0 || n > 4) { *cp = (unsigned char)*s; return 1; }
    if (tb_utf8_char_to_unicode(cp, s) < 0) { *cp = (unsigned char)*s; return 1; }
    return n;
}

static void put_str(int x, int y, uintattr_t fg, uintattr_t bg, const char *s) {
    while (*s) {
        uint32_t cp;
        int n = utf8_next(s, &cp);
        tb_set_cell(x++, y, cp, fg, bg);
        s += n;
    }
}

static void put_str_capped(int x, int y, int max_cols,
                           uintattr_t fg, uintattr_t bg, const char *s) {
    int written = 0;
    while (*s && written < max_cols) {
        uint32_t cp;
        int n = utf8_next(s, &cp);
        tb_set_cell(x + written, y, cp, fg, bg);
        s += n;
        written++;
    }
}

void ui_put_str(int x, int y, int max_cols,
                uintattr_t fg, uintattr_t bg, const char *s) {
    put_str_capped(x, y, max_cols, fg, bg, s);
}

static const char *backend_glyph(AgentBackend b) {
    switch (b) {
    case AC_BACKEND_CC:    return "cc";
    case AC_BACKEND_CODEX: return "cdx";
    }
    return "?";
}

static const char *short_status(AgentStatus s) {
    switch (s) {
    case AC_AGENT_IDLE:           return "idle";
    case AC_AGENT_THINKING:       return "thinking";
    case AC_AGENT_TOOL:           return "tool";
    case AC_AGENT_STREAMING:      return "streaming";
    case AC_AGENT_AWAITING_INPUT: return "awaiting";
    case AC_AGENT_DONE:           return "done";
    case AC_AGENT_ERROR:          return "error";
    }
    return "?";
}

static void render_agent_buttons(const DashboardState *st, int width) {
    int x = 0;
    for (size_t i = 0; i < st->agent_count && x < width; i++) {
        const DashboardAgent *a = &st->agents[i];
        bool focused = (i == st->focused_agent);

        uintattr_t fg = focused ? TB_BLACK : TB_DEFAULT;
        uintattr_t bg = focused ? TB_WHITE : TB_DEFAULT;

        char chip[64];
        const char *tool = a->current_tool ? a->current_tool : "";
        if (a->status == AC_AGENT_TOOL && *tool) {
            snprintf(chip, sizeof(chip), " %zu:%s tool:%s ", i + 1,
                     a->id ? a->id : backend_glyph(a->backend), tool);
        } else {
            snprintf(chip, sizeof(chip), " %zu:%s %s ", i + 1,
                     a->id ? a->id : backend_glyph(a->backend),
                     short_status(a->status));
        }

        put_str_capped(x, 0, width - x, fg, bg, chip);
        x += (int)strlen(chip);
        if (x < width) {
            tb_set_cell(x, 0, ' ', TB_DEFAULT, TB_DEFAULT);
            x++;
        }
    }
}

static void render_activity_log(const DashboardState *st, int top, int height, int width) {
    if (!st->bus || height <= 0) return;
    size_t total = bus_size(st->bus);
    size_t show  = total < (size_t)height ? total : (size_t)height;
    size_t start = total - show;

    for (size_t i = 0; i < show; i++) {
        LogEntry e;
        if (!bus_get(st->bus, start + i, &e)) continue;

        char line[256];
        time_t secs = (time_t)(e.timestamp_ms / 1000);
        struct tm tm;
        localtime_r(&secs, &tm);
        char ts[16];
        strftime(ts, sizeof(ts), "%H:%M:%S", &tm);

        snprintf(line, sizeof(line), "%s  %-10s %s", ts,
                 e.agent_id[0] ? e.agent_id : "?",
                 e.oneline);

        uintattr_t fg = (e.kind == AC_LOG_ERROR) ? TB_RED : TB_DEFAULT;
        put_str_capped(0, top + (int)i, width, fg, TB_DEFAULT, line);
    }
}

static void render_fkey_bar(int row, int width) {
    static const char *labels[10] = {
        "Help", "Spawn", "Drill", "Filter", "Paste",
        "Curate", "Inspect", "Kill", "Menu", "Quit"
    };
    int x = 0;
    for (int i = 0; i < 10 && x < width; i++) {
        char num[4];
        snprintf(num, sizeof(num), "%d", i + 1);
        put_str_capped(x, row, width - x, TB_WHITE, TB_BLACK, num);
        x += (int)strlen(num);
        if (x >= width) break;
        char lbl[16];
        snprintf(lbl, sizeof(lbl), "%-7s", labels[i]);
        put_str_capped(x, row, width - x, TB_DEFAULT, TB_DEFAULT, lbl);
        x += (int)strlen(lbl);
    }
}

void ui_dashboard_render(const DashboardState *state) {
    int width  = tb_width();
    int height = tb_height();
    if (width <= 0 || height <= 0) return;

    /* Layout: row 0 = agent buttons; rows 1..h-2 = activity log; h-1 = F-keys. */
    render_agent_buttons(state, width);
    render_activity_log(state, 1, height - 2, width);
    render_fkey_bar(height - 1, width);
}
