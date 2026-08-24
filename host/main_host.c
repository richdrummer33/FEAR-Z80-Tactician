#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/sim.h"

static char cell_char(const Sim *s, uint8_t x, uint8_t y) {
    uint8_t i;
    CellKind c;
    for (i = 0u; i < s->agent_count; ++i) {
        const Agent *a = &s->agents[i];
        if (a->alive && a->x == x && a->y == y) return sim_agent_team(i) == TEAM_BLUE ? 'B' : 'R';
    }
    c = (CellKind)sim_cell_kind(s, x, y);
    if (c == CELL_WALL) return '#';
    if (c == CELL_DOOR_CLOSED) return '+';
    if (c == CELL_DOOR_OPEN) return '-';
    return '.';
}

static void render(const Sim *s) {
    uint8_t y, x;
    printf("\x1b[Htick %u  BLUE alive=%u hp=%u   RED alive=%u hp=%u\n",
           (unsigned)s->tick,
           (unsigned)sim_team_alive(s, TEAM_BLUE), (unsigned)sim_team_hp(s, TEAM_BLUE),
           (unsigned)sim_team_alive(s, TEAM_RED), (unsigned)sim_team_hp(s, TEAM_RED));
    for (y = 0u; y < SIM_H; ++y) {
        for (x = 0u; x < SIM_W; ++x) putchar(cell_char(s, x, y));
        putchar('\n');
    }
}

int main(int argc, char **argv) {
    Sim s;
    uint16_t seed = 42u;
    unsigned max_ticks = SIM_TICK_CAP;
    int animate = 0;
    unsigned i;
    if (argc > 1) seed = (uint16_t)strtoul(argv[1], NULL, 0);
    if (argc > 2) max_ticks = (unsigned)strtoul(argv[2], NULL, 0);
    if (argc > 3 && strcmp(argv[3], "--animate") == 0) animate = 1;
    sim_init(&s, seed);
    if (animate) printf("\x1b[2J");
    for (i = 0u; i < max_ticks && !s.done; ++i) {
        sim_tick(&s);
        if (animate) render(&s);
    }
    render(&s);
    printf("result: %s at tick %u  replans=%u reused=%u\n",
           s.winner == TEAM_BLUE ? "BLUE" : s.winner == TEAM_RED ? "RED" : "DRAW",
           (unsigned)s.tick, (unsigned)s.total_replans, (unsigned)s.total_plan_reuses);
    return s.done ? 0 : 1;
}
