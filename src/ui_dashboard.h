#ifndef AC_UI_DASHBOARD_H
#define AC_UI_DASHBOARD_H

#include "agent.h"
#include "bus.h"
#include "termbox2.h"

#include <stdbool.h>
#include <stddef.h>

/* A snapshot of one agent's state, supplied by the caller for rendering. */
typedef struct {
    const char *id;
    AgentBackend backend;
    AgentStatus  status;
    const char  *current_tool;   /* may be NULL */
} DashboardAgent;

typedef struct {
    const DashboardAgent *agents;
    size_t                agent_count;
    EventBus             *bus;
    size_t                focused_agent;  /* index into agents[] */
} DashboardState;

/* Render one frame. Should be called between tb_clear() and tb_present(). */
void ui_dashboard_render(const DashboardState *state);

/* UTF-8-aware text rendering helper exposed for callers that overlay on top
 * of the dashboard (e.g. status line in main.c). */
void ui_put_str(int x, int y, int max_cols,
                uintattr_t fg, uintattr_t bg, const char *s);

#endif
