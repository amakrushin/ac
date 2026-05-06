#include "greatest.h"

GREATEST_MAIN_DEFS();

extern SUITE_EXTERN(ring);
extern SUITE_EXTERN(proc);
extern SUITE_EXTERN(events_cc);
extern SUITE_EXTERN(agent);
extern SUITE_EXTERN(bus);
extern SUITE_EXTERN(agent_session);

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(ring);
    RUN_SUITE(proc);
    RUN_SUITE(events_cc);
    RUN_SUITE(agent);
    RUN_SUITE(bus);
    RUN_SUITE(agent_session);
    GREATEST_MAIN_END();
}
