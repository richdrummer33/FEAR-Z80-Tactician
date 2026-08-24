#include <assert.h>
#include <stdio.h>
#include "../src/sim.h"
#include "../src/brain.h"

static uint16_t alive_mask(const Sim *s) {
    uint8_t i;
    uint16_t m = 0u;
    for (i = 0u; i < s->agent_count; ++i)
        if (s->agents[i].alive) m |= (uint16_t)(1u << i);
    return m;
}

static void assert_invariants(const Sim *s) {
    unsigned i, j;
    assert(s->red_count >= SIM_RED_MIN && s->red_count <= SIM_RED_MAX);
    assert(s->agent_count == SIM_BLUE_COUNT + s->red_count);
    for (i = 0u; i < s->agent_count; ++i) {
        const Agent *a = &s->agents[i];
        assert(a->x < SIM_W && a->y < SIM_H);
        if (a->alive) {
            assert(a->hp > 0u && a->hp <= SIM_MAX_HP);
            assert(sim_cell_passable(s, a->x, a->y, 0u));
        } else assert(a->hp == 0u);
        assert(a->ammo <= SIM_MAX_AMMO);
        assert(a->route_len <= SIM_ROUTE_STEPS);
        assert(a->route_pos <= a->route_len);
        assert(a->plan_len <= SIM_PLAN_MAX);
        assert(a->plan_pos <= a->plan_len);
        for (j = i + 1u; j < s->agent_count; ++j) {
            const Agent *b = &s->agents[j];
            if (a->alive && b->alive) assert(!(a->x == b->x && a->y == b->y));
        }
    }
    assert(s->event_count <= SIM_EVENT_MAX);
}

static void assert_office_map(const Sim *s) {
    #define CELL(x,y) sim_cell_kind(s, (x), (y))
    assert(SIM_W == 46u && SIM_H == 24u);
    assert(CELL(9u,11u) == CELL_DOOR_CLOSED);
    assert(CELL(19u,8u) == CELL_DOOR_CLOSED);
    assert(CELL(19u,16u) == CELL_DOOR_CLOSED);
    assert(CELL(33u,8u) == CELL_DOOR_CLOSED);
    assert(CELL(29u,17u) == CELL_DOOR_CLOSED);
    assert(CELL(38u,13u) == CELL_DOOR_OPEN);
    assert(CELL(31u,13u) == CELL_DOOR_CLOSED);
    assert(CELL(14u,9u) == CELL_WALL);
    assert(CELL(14u,14u) == CELL_WALL);
    assert(CELL(25u,5u) == CELL_FLOOR); /* desk-bank gap */
    assert(CELL(23u,5u) == CELL_WALL);
    assert(CELL(22u,18u) == CELL_FLOOR); /* records shelf gap */
    assert(CELL(37u,7u) == CELL_WALL);
    assert(CELL(39u,7u) == CELL_FLOOR); /* security-console gap */
    #undef CELL
}


static void assert_goap_reload_chain(void) {
    Sim s;
    Agent *a;
    uint8_t enemy = SIM_BLUE_COUNT;
    sim_init(&s, 42u);

    /* Put one pair in a clean Lobby sightline and force an empty magazine. */
    a = &s.agents[0u];
    a->x = 10u; a->y = 7u; a->ammo = 0u; a->hp = SIM_MAX_HP; a->suppressed = 0u;
    s.agents[enemy].x = 12u; s.agents[enemy].y = 7u; s.agents[enemy].alive = 1u;
    s.agents[enemy].hp = SIM_MAX_HP;
    a->goal = GOAL_PATROL; a->plan_len = 0u; a->plan_pos = 0u;

    assert(sim_target_visible(&s, 0u, enemy));
    assert(sim_target_in_range(&s, 0u, enemy));
    brain_act_agent(&s, 0u);
    assert(a->goal == GOAL_KILL_ENEMY);
    assert(a->plan_len == 2u);
    assert(a->plan[0u] == ACTION_RELOAD);
    assert(a->plan[1u] == ACTION_SHOOT);
    assert(a->plan_pos == 1u);
    assert(a->ammo == SIM_MAX_AMMO);
    assert(a->replan_count == 1u);

    brain_act_agent(&s, 0u);
    assert(a->reuse_count >= 1u);
    assert(a->plan_pos == 2u);
    assert(a->ammo == (SIM_MAX_AMMO - 1u));
}

int main(void) {
    assert_goap_reload_chain();
    static const uint16_t seeds[] = {1u, 2u, 7u, 19u, 42u, 77u, 255u, 1024u, 65535u};
    unsigned si;
    for (si = 0u; si < sizeof(seeds)/sizeof(seeds[0]); ++si) {
        Sim s;
        unsigned guard = 0u;
        sim_init(&s, seeds[si]);
        assert_office_map(&s);
        assert_invariants(&s);
        if (seeds[si] == 42u) assert(s.red_count == 6u);

        while (!s.done && guard++ < SIM_TICK_CAP + 1u) {
            uint16_t before = alive_mask(&s);
            sim_tick(&s);
            assert_invariants(&s);
            if (!s.done) {
                uint16_t after = alive_mask(&s);
                assert((s.last_acted_mask & after) == after);
                assert((s.last_acted_mask & (uint16_t)~before) == 0u);
            }
        }
        assert(s.done);
        assert(s.tick <= SIM_TICK_CAP);
        assert(s.total_replans > 0u);
        assert(s.total_plan_reuses > 0u);
        printf("seed=%u red0=%u winner=%u tick=%u B=%u/%u R=%u/%u replans=%u reuse=%u\n",
               (unsigned)seeds[si], (unsigned)s.red_count, (unsigned)s.winner, (unsigned)s.tick,
               (unsigned)sim_team_alive(&s, TEAM_BLUE), (unsigned)sim_team_hp(&s, TEAM_BLUE),
               (unsigned)sim_team_alive(&s, TEAM_RED), (unsigned)sim_team_hp(&s, TEAM_RED),
               (unsigned)s.total_replans, (unsigned)s.total_plan_reuses);
    }
    puts("all office-loop full-population sim tests passed");
    return 0;
}
