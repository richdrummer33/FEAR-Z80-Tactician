#!/usr/bin/env python3
"""Transpile the real N96/K48 trapped two-slot Grover demonstrator against IBM fake hardware targets."""

from __future__ import annotations

import itertools
import json
import math
from collections import Counter

from qiskit import QuantumCircuit, QuantumRegister, ClassicalRegister
from qiskit.transpiler import generate_preset_pass_manager
from qiskit_ibm_runtime.fake_provider import FakeFez, FakeNighthawk

AVAILABLE_50 = [
    4, 6, 7, 10, 11, 12, 14, 16, 18, 20, 22, 23, 25, 26, 29, 31,
    34, 36, 39, 40, 41, 43, 44, 45, 46, 49, 51, 52, 53, 55, 56, 57,
    58, 59, 61, 62, 63, 65, 67, 69, 71, 73, 74, 75, 76, 77, 86, 88,
    89, 91,
]
CURRENT_PAIR = (6, 23)
UNIQUE_IMPROVING_PAIR = (6, 52)


def candidate_subset(n: int) -> list[int]:
    chosen = {6, 23, 52}
    for c in AVAILABLE_50:
        if len(chosen) >= n:
            break
        chosen.add(c)
    return sorted(chosen)


def pair_rank(pool: list[int], pair: tuple[int, int]) -> int:
    p = tuple(sorted(pair))
    for r, q in enumerate(itertools.combinations(pool, 2)):
        if q == p:
            return r
    raise ValueError(pair)


def build_tree_mcz(qc, controls, ancillas):
    if len(controls) == 2:
        qc.cz(controls[0], controls[1])
        return
    nodes = list(controls[:-1])
    used = []
    aidx = 0
    while len(nodes) > 1:
        nxt = []
        i = 0
        while i + 1 < len(nodes):
            a = ancillas[aidx]
            aidx += 1
            qc.rccx(nodes[i], nodes[i + 1], a)
            used.append((nodes[i], nodes[i + 1], a))
            nxt.append(a)
            i += 2
        if i < len(nodes):
            nxt.append(nodes[i])
        nodes = nxt
    root = nodes[0]
    qc.cz(root, controls[-1])
    for x, y, a in reversed(used):
        qc.rccx(x, y, a)


def phase_mark_rank(qc, search, ancillas, target_rank: int):
    for i, q in enumerate(search):
        if ((target_rank >> i) & 1) == 0:
            qc.x(q)
    build_tree_mcz(qc, search, ancillas)
    for i, q in enumerate(search):
        if ((target_rank >> i) & 1) == 0:
            qc.x(q)


def diffusion(qc, search, ancillas):
    for q in search:
        qc.h(q)
        qc.x(q)
    build_tree_mcz(qc, search, ancillas)
    for q in search:
        qc.x(q)
        qc.h(q)


def build_circuit(n_candidates: int, rounds: int):
    pool = candidate_subset(n_candidates)
    legal_pairs = math.comb(len(pool), 2)
    m = math.ceil(math.log2(legal_pairs))
    n_anc = max(0, m - 2)
    target = pair_rank(pool, UNIQUE_IMPROVING_PAIR)

    q_search = QuantumRegister(m, "rank")
    q_work = QuantumRegister(n_anc, "work") if n_anc else None
    c = ClassicalRegister(m, "c")
    regs = [q_search] + ([q_work] if q_work is not None else []) + [c]
    qc = QuantumCircuit(*regs)
    search = list(q_search)
    anc = list(q_work) if q_work else []

    qc.h(search)
    for _ in range(rounds):
        phase_mark_rank(qc, search, anc, target)
        diffusion(qc, search, anc)
    qc.measure(search, c)

    return qc, {
        "candidate_pool": pool,
        "legal_pairs": legal_pairs,
        "padded_states": 1 << m,
        "search_qubits": m,
        "work_ancillas": n_anc,
        "logical_qubits": m + n_anc,
        "target_pair": list(UNIQUE_IMPROVING_PAIR),
        "target_rank": target,
        "current_pair": list(CURRENT_PAIR),
        "current_rank": pair_rank(pool, CURRENT_PAIR),
    }


def two_qubit_depth(circuit) -> int:
    qindex = {q: i for i, q in enumerate(circuit.qubits)}
    depth = [0] * circuit.num_qubits
    maxd = 0
    for inst in circuit.data:
        qs = list(inst.qubits)
        if len(qs) == 2:
            a, b = qindex[qs[0]], qindex[qs[1]]
            d = max(depth[a], depth[b]) + 1
            depth[a] = depth[b] = d
            maxd = max(maxd, d)
    return maxd


def active_qubits(circuit):
    qindex = {q: i for i, q in enumerate(circuit.qubits)}
    active = set()
    for inst in circuit.data:
        if inst.operation.name in {"barrier", "delay"}:
            continue
        for q in inst.qubits:
            active.add(qindex[q])
    return sorted(active)


def ideal_one_mark_probability(padded_states: int, rounds: int) -> float:
    theta = math.asin(1.0 / math.sqrt(padded_states))
    return math.sin((2 * rounds + 1) * theta) ** 2


def compile_one(backend, candidates, rounds):
    qc, info = build_circuit(candidates, rounds)
    pm = generate_preset_pass_manager(
        backend=backend,
        optimization_level=3,
        seed_transpiler=42,
    )
    isa = pm.run(qc)
    ops = Counter(isa.count_ops())
    twoq_names = {"cz", "cx", "ecr", "rzz"}
    twoq = sum(v for k, v in ops.items() if k in twoq_names)
    return {
        "backend": backend.name,
        "backend_qubits": backend.num_qubits,
        "candidates": candidates,
        "rounds": rounds,
        **info,
        "ideal_target_probability": ideal_one_mark_probability(info["padded_states"], rounds),
        "isa_width": isa.num_qubits,
        "active_physical_qubits": active_qubits(isa),
        "active_physical_qubit_count": len(active_qubits(isa)),
        "isa_depth": isa.depth(),
        "two_qubit_count": twoq,
        "two_qubit_depth": two_qubit_depth(isa),
        "swap_count": int(ops.get("swap", 0)),
        "ops": dict(sorted(ops.items())),
    }


def main():
    backends = [FakeFez(), FakeNighthawk()]
    candidate_sizes = [8, 11, 16, 23, 32, 45, 50]
    rounds_list = [0, 1, 2, 3]
    out = {
        "qiskit_note": "Qiskit preset pass manager level 3, seed_transpiler=42",
        "results": [],
    }
    for backend in backends:
        for candidates in candidate_sizes:
            for rounds in rounds_list:
                print(f"Compiling {backend.name}: candidates={candidates} rounds={rounds}", flush=True)
                r = compile_one(backend, candidates, rounds)
                out["results"].append(r)
                print(json.dumps({
                    "backend": r["backend"],
                    "candidates": candidates,
                    "rounds": rounds,
                    "logical_qubits": r["logical_qubits"],
                    "active_physical_qubit_count": r["active_physical_qubit_count"],
                    "isa_depth": r["isa_depth"],
                    "two_qubit_count": r["two_qubit_count"],
                    "two_qubit_depth": r["two_qubit_depth"],
                    "swap_count": r["swap_count"],
                }), flush=True)

    with open("ibm_hardware_v1_qiskit_results.json", "w") as f:
        json.dump(out, f, indent=2)


if __name__ == "__main__":
    main()
