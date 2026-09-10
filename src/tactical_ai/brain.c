#if defined(__SDCC)
#pragma bank 255
#include <gbdk/platform.h>
BANKREF(brain_bank)
#endif

#include "brain.h"

#define NO_AGENT 0xffu
#define PLAN_LEASE_TICKS 40u
#define MEMORY_TICKS 18u

typedef struct {
    uint16_t pre;
    uint16_t eff;
} ActionDef;

/*
 * Compact add-only symbolic model. Concrete target and position bindings live
 * beside the plan in Agent, matching the desktop prototype's architecture.
 */
static const ActionDef k_actions[ACTION_COUNT] = {
    {0u, 0u},                                                /* NONE */
    {0u, FACT_WEAPON_READY},                                 /* RELOAD */
    {FACT_TARGET_KNOWN, FACT_IN_SHOT_RANGE},                 /* MOVE_TO_TARGET */
    {(FACT_IN_SHOT_RANGE | FACT_WEAPON_READY), FACT_TARGET_DEAD}, /* SHOOT */
    {FACT_TARGET_KNOWN, FACT_IN_COVER},                      /* MOVE_TO_COVER */
    {FACT_TARGET_KNOWN, FACT_AT_LAST_SEEN},                  /* MOVE_TO_LAST */
    {0u, FACT_AT_PATROL},                                    /* MOVE_PATROL */
    {0u, FACT_WAITED}                                        /* WAIT */
};

static uint8_t choose_goal(const Sim *s, uint8_t id, uint8_t visible) {
    const Agent *a = &s->agents[id];
    uint8_t scores[GOAL_COUNT];
    uint8_t in_cover = sim_has_adjacent_cover(s, a->x, a->y);
    uint8_t memory_fresh = a->last_seen_valid && ((uint16_t)(s->tick - a->last_seen_tick) <= MEMORY_TICKS);
    uint8_t threat_known = (visible != NO_AGENT) || memory_fresh;
    uint8_t best = a->goal < GOAL_COUNT ? a->goal : GOAL_IDLE;
    uint8_t best_score;
    uint8_t g;

    scores[GOAL_ESCAPE_DANGER] = (threat_known && !in_cover && a->hp <= 1u) ? 100u : 0u;
    scores[GOAL_COVER] = (threat_known && !in_cover && (a->suppressed || a->hp <= 2u)) ? 90u : 0u;
    scores[GOAL_KILL_ENEMY] = (visible != NO_AGENT) ? 70u : 0u;
    scores[GOAL_SEARCH_LOST] = memory_fresh ? 50u : 0u;
    scores[GOAL_PATROL] = 20u;
    scores[GOAL_IDLE] = 1u;
    best_score = scores[best];

    for (g = 0u; g < GOAL_COUNT; ++g) {
        if (scores[g] > best_score) { best = g; best_score = scores[g]; }
    }
    return best;
}

static uint16_t desired_for_goal(uint8_t goal) {
    switch ((GoalKind)goal) {
        case GOAL_ESCAPE_DANGER:
        case GOAL_COVER:       return FACT_IN_COVER;
        case GOAL_KILL_ENEMY:  return FACT_TARGET_DEAD;
        case GOAL_SEARCH_LOST: return FACT_AT_LAST_SEEN;
        case GOAL_PATROL:      return FACT_AT_PATROL;
        case GOAL_IDLE:        return FACT_WAITED;
        default:               return FACT_WAITED;
    }
}

static uint8_t action_allowed(uint8_t goal, uint8_t action) {
    switch ((ActionKind)action) {
        case ACTION_RELOAD:
            return goal == GOAL_KILL_ENEMY;
        case ACTION_MOVE_TO_TARGET:
        case ACTION_SHOOT:
            return goal == GOAL_KILL_ENEMY;
        case ACTION_MOVE_TO_COVER:
            return goal == GOAL_COVER || goal == GOAL_ESCAPE_DANGER;
        case ACTION_MOVE_TO_LAST_SEEN:
            return goal == GOAL_SEARCH_LOST;
        case ACTION_MOVE_PATROL:
            return goal == GOAL_PATROL;
        case ACTION_WAIT:
            return goal == GOAL_IDLE;
        default:
            return 0u;
    }
}

static uint16_t compute_facts(const Sim *s, uint8_t id, const Agent *a) {
    uint16_t f = 0u;
    uint8_t target = a->plan_target;

    if (a->ammo) f |= FACT_WEAPON_READY;
    if (sim_has_adjacent_cover(s, a->x, a->y)) f |= FACT_IN_COVER;

    if (target != NO_AGENT && target < s->agent_count) {
        uint8_t visible = sim_target_visible(s, id, target);
        if (!s->agents[target].alive) f |= FACT_TARGET_DEAD;
        if ((a->last_seen_valid && a->last_seen_enemy == target) || visible) f |= FACT_TARGET_KNOWN;
        if (visible && sim_target_in_range(s, id, target)) f |= FACT_IN_SHOT_RANGE;
    } else if (a->last_seen_valid) {
        f |= FACT_TARGET_KNOWN;
    }

    if (a->x == a->plan_tx && a->y == a->plan_ty) {
        if (a->plan_goal == GOAL_SEARCH_LOST) f |= FACT_AT_LAST_SEEN;
        if (a->plan_goal == GOAL_PATROL) f |= FACT_AT_PATROL;
    }
    return f;
}

/*
 * Backward regression: choose an action that can establish an unsatisfied fact,
 * replace that fact with the action preconditions, and recurse. The selected
 * actions are accumulated goal->start, then reversed for execution.
 */
static uint8_t regress(uint8_t goal, uint16_t facts, uint16_t desired,
                       uint8_t depth, uint8_t *reverse_plan, uint8_t *reverse_len) {
    uint16_t unsatisfied = (uint16_t)(desired & (uint16_t)~facts);
    uint8_t action;
    if (!unsatisfied) return 1u;
    if (depth >= SIM_PLAN_MAX) return 0u;

    for (action = 1u; action < ACTION_COUNT; ++action) {
        const ActionDef *d = &k_actions[action];
        uint16_t next_desired;
        if (!action_allowed(goal, action)) continue;
        if (!(d->eff & unsatisfied)) continue;
        next_desired = (uint16_t)((desired & (uint16_t)~d->eff) | d->pre);
        if (next_desired == desired) continue;
        reverse_plan[*reverse_len] = action;
        ++(*reverse_len);
        if (regress(goal, facts, next_desired, (uint8_t)(depth + 1u), reverse_plan, reverse_len))
            return 1u;
        --(*reverse_len);
    }
    return 0u;
}

static void bind_goal(Sim *s, uint8_t id, uint8_t visible, uint8_t goal) {
    Agent *a = &s->agents[id];
    uint8_t tx = a->x, ty = a->y;
    a->plan_target = NO_AGENT;
    a->plan_tx = a->x;
    a->plan_ty = a->y;

    if (goal == GOAL_KILL_ENEMY || goal == GOAL_COVER || goal == GOAL_ESCAPE_DANGER) {
        uint8_t target = visible;
        if (target == NO_AGENT && a->last_seen_valid) target = a->last_seen_enemy;
        a->plan_target = target;
        if ((goal == GOAL_COVER || goal == GOAL_ESCAPE_DANGER) && target != NO_AGENT &&
            sim_pick_cover_tile(s, id, target, &tx, &ty)) {
            a->plan_tx = tx;
            a->plan_ty = ty;
        }
    } else if (goal == GOAL_SEARCH_LOST) {
        a->plan_target = a->last_seen_enemy;
        a->plan_tx = a->last_seen_x;
        a->plan_ty = a->last_seen_y;
    } else if (goal == GOAL_PATROL) {
        sim_patrol_target(s, id, &tx, &ty);
        a->plan_tx = tx;
        a->plan_ty = ty;
    }
}

static void invalidate_plan(Agent *a) {
    a->plan_len = 0u;
    a->plan_pos = 0u;
    a->plan_age = 0u;
}

static uint8_t plan_still_valid(const Sim *s, uint8_t id, uint8_t goal) {
    const Agent *a = &s->agents[id];
    if (!a->plan_len || a->plan_pos >= a->plan_len) return 0u;
    if (a->plan_goal != goal) return 0u;
    if (a->plan_age >= PLAN_LEASE_TICKS) return 0u;
    if ((goal == GOAL_KILL_ENEMY || goal == GOAL_COVER || goal == GOAL_ESCAPE_DANGER) &&
        (a->plan_target == NO_AGENT || a->plan_target >= s->agent_count || !s->agents[a->plan_target].alive))
        return 0u;
    return 1u;
}

static uint8_t build_plan(Sim *s, uint8_t id, uint8_t visible, uint8_t goal) {
    Agent *a = &s->agents[id];
    uint8_t reverse_plan[SIM_PLAN_MAX];
    uint8_t reverse_len = 0u;
    uint8_t i;
    uint16_t facts, desired;

    bind_goal(s, id, visible, goal);
    a->plan_goal = goal;
    a->plan_age = 0u;
    a->plan_pos = 0u;
    a->plan_len = 0u;
    facts = compute_facts(s, id, a);
    a->facts = facts;
    desired = desired_for_goal(goal);

    if ((desired & (uint16_t)~facts) == 0u) return 0u;
    if (!regress(goal, facts, desired, 0u, reverse_plan, &reverse_len)) return 0u;
    if (!reverse_len || reverse_len > SIM_PLAN_MAX) return 0u;

    a->plan_len = reverse_len;
    for (i = 0u; i < reverse_len; ++i)
        a->plan[i] = reverse_plan[(uint8_t)(reverse_len - 1u - i)];

    ++a->replan_count;
    ++s->total_replans;
    return 1u;
}

/* Returns 1 when the current primitive has completed, 0 when it persists. */
static uint8_t execute_action(Sim *s, uint8_t id, uint8_t action) {
    Agent *a = &s->agents[id];
    uint8_t target = a->plan_target;

    switch ((ActionKind)action) {
        case ACTION_RELOAD:
            a->ammo = SIM_MAX_AMMO;
            sim_record_reload(s, id);
            return 1u;

        case ACTION_MOVE_TO_TARGET:
            if (target == NO_AGENT || target >= s->agent_count || !s->agents[target].alive) return 1u;
            if (sim_target_visible(s, id, target) && sim_target_in_range(s, id, target)) return 1u;
            sim_do_move_or_door(s, id, s->agents[target].x, s->agents[target].y);
            return (sim_target_visible(s, id, target) && sim_target_in_range(s, id, target)) ? 1u : 0u;

        case ACTION_SHOOT:
            if (target == NO_AGENT || target >= s->agent_count || !s->agents[target].alive) return 1u;
            if (!a->ammo) return 1u; /* plan validation will force a reload next tick */
            if (!sim_target_visible(s, id, target) || !sim_target_in_range(s, id, target)) return 1u;
            --a->ammo;
            sim_do_shot(s, id, target);
            return 1u;

        case ACTION_MOVE_TO_COVER:
            if (a->x == a->plan_tx && a->y == a->plan_ty) return 1u;
            sim_do_move_or_door(s, id, a->plan_tx, a->plan_ty);
            return (a->x == a->plan_tx && a->y == a->plan_ty) ? 1u : 0u;

        case ACTION_MOVE_TO_LAST_SEEN:
            if (a->x == a->plan_tx && a->y == a->plan_ty) {
                a->last_seen_valid = 0u;
                return 1u;
            }
            sim_do_move_or_door(s, id, a->plan_tx, a->plan_ty);
            if (a->x == a->plan_tx && a->y == a->plan_ty) {
                a->last_seen_valid = 0u;
                return 1u;
            }
            return 0u;

        case ACTION_MOVE_PATROL:
            if (a->x == a->plan_tx && a->y == a->plan_ty) return 1u;
            sim_do_move_or_door(s, id, a->plan_tx, a->plan_ty);
            return (a->x == a->plan_tx && a->y == a->plan_ty) ? 1u : 0u;

        case ACTION_WAIT:
            return 1u;
        default:
            return 1u;
    }
}

static void brain_act_agent_impl(Sim *s, uint8_t id) {
    Agent *a = &s->agents[id];
    uint8_t visible;
    uint8_t selected_goal;
    uint8_t reused = 0u;
    uint8_t action;

    if (!a->alive) return;
    s->last_acted_mask |= (uint16_t)(1u << id);

    visible = sim_nearest_visible_enemy(s, id);
    sim_perceive(s, id, visible);
    selected_goal = choose_goal(s, id, visible);

    if (selected_goal != a->goal) {
        a->goal = selected_goal;
        invalidate_plan(a);
    }

    if (plan_still_valid(s, id, selected_goal)) {
        reused = 1u;
        ++a->reuse_count;
        ++s->total_plan_reuses;
    } else {
        invalidate_plan(a);
        if (!build_plan(s, id, visible, selected_goal)) {
            /* A satisfied SearchLost plan clears stale memory; patrol then wins next tick. */
            if (selected_goal == GOAL_SEARCH_LOST && a->x == a->plan_tx && a->y == a->plan_ty)
                a->last_seen_valid = 0u;
            a->facts = compute_facts(s, id, a);
            return;
        }
    }

    (void)reused;
    if (a->plan_pos >= a->plan_len) return;
    action = a->plan[a->plan_pos];
    if (execute_action(s, id, action)) ++a->plan_pos;
    ++a->plan_age;
    a->facts = compute_facts(s, id, a);
}

void brain_act_agent(Sim *s, uint8_t id) BANKED {
    brain_act_agent_impl(s, id);
}

void brain_run_actor_round(Sim *s, uint8_t first_team, uint8_t slots) BANKED {
    uint8_t slot;
    for (slot = 0u; slot < slots && !s->done; ++slot) {
        uint8_t bvalid = (slot < SIM_BLUE_COUNT);
        uint8_t rvalid = (slot < s->red_count);
        uint8_t b = slot;
        uint8_t r = (uint8_t)(SIM_BLUE_COUNT + slot);
        if (first_team == TEAM_BLUE) {
            if (bvalid) brain_act_agent_impl(s, b);
            if (!s->done && rvalid) brain_act_agent_impl(s, r);
        } else {
            if (rvalid) brain_act_agent_impl(s, r);
            if (!s->done && bvalid) brain_act_agent_impl(s, b);
        }
    }
}
