#include <assert.h>
#include <stdio.h>
#include "vfx_lab.h"

static void test_beam(void) {
    Sim sim;
    VfxLab lab;
    uint8_t before;
    sim_init(&sim, 2u);
    vfx_lab_init(&lab, 0x1234u);
    vfx_lab_fire_beam(&lab, 90, 46, 126, 14);
    assert(lab.beam.cell_count > 0u);
    before = lab.beam.formed_count;
    vfx_lab_tick(&lab, &sim);
    assert(lab.beam.formed_count == (uint8_t)(before + 1u));
}

static void test_grenade_ai_budget(void) {
    Sim sim;
    VfxLab lab;
    unsigned i;
    sim_init(&sim, 2u);
    vfx_lab_init(&lab, 0x1234u);
    vfx_lab_detonate_grenade(&lab, 100, 60, -4, 2);
    vfx_lab_tick(&lab, &sim);
    assert(VFX_AI_IS_HELD(&lab));
    assert(lab.work_units_hint == 8u);
    assert(vfx_lab_active_debris(&lab) > 0u);

    for (i = 0u; i < 20u; ++i) vfx_lab_tick(&lab, &sim);
    assert(lab.ai_tick_div_hint == 4u || lab.ai_tick_div_hint == 0u);
    for (i = 0u; i < 40u; ++i) vfx_lab_tick(&lab, &sim);
    assert(lab.ai_tick_div_hint == 2u || lab.ai_tick_div_hint == 0u);
}

static void test_scripted_positions(void) {
    Sim sim;
    VfxLab lab;
    uint8_t scene, i;
    sim_init(&sim, 2u);
    vfx_lab_init(&lab, 0xBEEFu);
    for (scene = 0u; scene < VFX_SCENE_COUNT; ++scene) {
        vfx_lab_set_scene(&lab, scene);
        vfx_lab_apply_demo_agents(&lab, &sim);
        for (i = 0u; i < sim.agent_count; ++i) {
            if (!sim.agents[i].alive) continue;
            assert(sim_cell_passable(&sim, sim.agents[i].x, sim.agents[i].y, 0u));
        }
    }
}

static void test_full_loop(void) {
    Sim sim;
    VfxLab lab;
    unsigned i;
    sim_init(&sim, 2u);
    vfx_lab_init(&lab, 0xBEEFu);
    for (i = 0u; i < 1560u; ++i) vfx_lab_target_step(&lab, &sim);
    assert(lab.scene == VFX_SCENE_BULLETS);
    assert(lab.scene_frame == 0u);
    assert(lab.total_frame == 1560u);
}

int main(void) {
    test_beam();
    test_grenade_ai_budget();
    test_scripted_positions();
    test_full_loop();
    puts("vfx lab tests: ok");
    return 0;
}
