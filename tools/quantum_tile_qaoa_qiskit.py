#!/usr/bin/env python3
"""Build/sample QAOA for a QUBO emitted by quantum_tile_dictionary.py.

Requires current Qiskit (tested against the Qiskit 2.x API shape documented in
2026):
    pip install 'qiskit>=2.5'

This file is deliberately optional: corpus construction, QUBO export, exact
small-instance controls and centroid refinement have no Qiskit dependency.
"""

import argparse
import json
import pathlib


def load_problem(path):
    p = json.loads(pathlib.Path(path).read_text())
    n = len(p["variables"])
    linear = {int(i): float(v) for i, v in p["linear"]}
    quadratic = {(int(i), int(j)): float(v) for i, j, v in p["quadratic"]}
    offset = float(p.get("offset", 0.0))
    return p, n, linear, quadratic, offset


def qubo_to_sparse_pauli(n, linear, quadratic, offset):
    """Return (SparsePauliOp, constant) for x=(1-Z)/2."""
    from qiskit.quantum_info import SparsePauliOp

    h = [0.0] * n
    jz = {}
    constant = offset
    for i, a in linear.items():
        constant += a / 2.0
        h[i] -= a / 2.0
    for (i, j), b in quadratic.items():
        constant += b / 4.0
        h[i] -= b / 4.0
        h[j] -= b / 4.0
        jz[(i, j)] = jz.get((i, j), 0.0) + b / 4.0

    terms = []
    for i, c in enumerate(h):
        if abs(c) > 1e-15:
            terms.append(("Z", [i], c))
    for (i, j), c in sorted(jz.items()):
        if abs(c) > 1e-15:
            terms.append(("ZZ", [i, j], c))
    if not terms:
        terms = [("I", [0], 0.0)]
    return SparsePauliOp.from_sparse_list(terms, num_qubits=n), constant


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("qubo")
    ap.add_argument("--reps", type=int, default=1)
    ap.add_argument("--params", type=float, nargs="*",
                    help="QAOA parameter values in circuit parameter order")
    ap.add_argument("--shots", type=int, default=4096)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--sample", action="store_true")
    ap.add_argument("--draw", action="store_true")
    args = ap.parse_args()

    from qiskit.circuit.library import qaoa_ansatz

    problem, n, linear, quadratic, offset = load_problem(args.qubo)
    cost, constant = qubo_to_sparse_pauli(n, linear, quadratic, offset)
    circuit = qaoa_ansatz(cost, reps=args.reps, flatten=True)
    print(f"QAOA_QUBITS {circuit.num_qubits}")
    print(f"QAOA_PARAMETERS {circuit.num_parameters}")
    print(f"QAOA_DEPTH {circuit.decompose().depth()}")
    print(f"ISING_CONSTANT {constant:.17g}")
    if args.draw:
        print(circuit.draw(output="text", fold=120))

    if not args.sample:
        print("QAOA_BUILD_PASS")
        return
    if args.params is None or len(args.params) != circuit.num_parameters:
        raise SystemExit(
            f"--sample requires exactly {circuit.num_parameters} --params values")

    from qiskit.primitives import StatevectorSampler

    measured = circuit.copy()
    measured.measure_all()
    sampler = StatevectorSampler(seed=args.seed)
    result = sampler.run([(measured, args.params)], shots=args.shots).result()[0]
    counts = result.data.meas.get_counts()
    ranked = sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))[:32]
    print("QAOA_COUNTS " + json.dumps(ranked))
    print("QAOA_SAMPLE_PASS")


if __name__ == "__main__":
    main()
