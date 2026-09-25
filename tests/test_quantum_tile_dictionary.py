import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

# Stub renderer-corpus imports: these tests exercise pure optimizer functions.
import types
m = types.ModuleType("analyze_doomguy_dense_corpus"); m.Corpus = object
sys.modules.setdefault("analyze_doomguy_dense_corpus", m)
m = types.ModuleType("analyze_resident_lod_dictionary"); m.parse_csv_ints = lambda s: [int(x) for x in s.split(',')]
sys.modules.setdefault("analyze_resident_lod_dictionary", m)
m = types.ModuleType("analyze_sprite_resident_lod"); m.build_groups = lambda *a, **k: ([], [])
sys.modules.setdefault("analyze_sprite_resident_lod", m)

# Minimal perceptual metric compatible with the production module interface.
m = types.ModuleType("resident_tile_dictionary")
class TileWeights:
    def __init__(self, silhouette=12.0, shade=1.0):
        self.silhouette=silhouette; self.shade=shade

def pixel_cost(got, wanted, w):
    if bool(got) != bool(wanted): return w.silhouette
    return w.shade * abs(int(got)-int(wanted)) if got else 0.0

def pattern_cost(got, wanted, w):
    return sum(pixel_cost(a,b,w) for a,b in zip(got,wanted))
m.TileWeights=TileWeights; m.pixel_cost=pixel_cost; m.pattern_cost=pattern_cost
sys.modules.setdefault("resident_tile_dictionary", m)

from quantum_tile_dictionary import (
    TargetClass, Qubo, add_cardinality_penalty, build_compact_qubo,
    build_weighted_targets, centroid_refine, cost_matrix,
    qubo_to_ising, weighted_centroid,
)


class QuantumTileDictionaryTests(unittest.TestCase):
    def test_view_weighting_gives_each_view_unit_mass(self):
        a = bytes([1] * 64); b = bytes([2] * 64)
        groups = [{"demands": [
            {"pattern": a, "view_index": 0},
            {"pattern": b, "view_index": 0},
            {"pattern": a, "view_index": 1},
        ]}]
        ts = build_weighted_targets(groups, [{}, {}], "view")
        self.assertAlmostEqual(sum(t.weight for t in ts), 2.0)
        wa = next(t.weight for t in ts if t.pattern == a)
        wb = next(t.weight for t in ts if t.pattern == b)
        self.assertAlmostEqual(wa, 1.5)
        self.assertAlmostEqual(wb, 0.5)

    def test_weighted_centroid_is_not_forced_to_observed_whole_tile(self):
        w = TileWeights(12, 1)
        a = bytearray([1] * 64); b = bytearray([3] * 64)
        c = weighted_centroid([bytes(a), bytes(b)], [1.0, 3.0], w)
        self.assertEqual(c[0], 3)

    def test_qubo_to_ising_preserves_every_energy(self):
        q = Qubo(["a","b","c"], {0:-1.5, 2:0.7},
                 {(0,1):2.0,(1,2):-0.25}, 0.3)
        z = qubo_to_ising(q)
        for mask in range(8):
            bits=[(mask>>i)&1 for i in range(3)]
            spins=[1-2*b for b in bits]
            e=z["offset"]
            for i,h in z["h"].items(): e += h*spins[i]
            for (i,j),c in z["J"].items(): e += c*spins[i]*spins[j]
            self.assertAlmostEqual(e, q.energy(bits), places=10)

    def test_cardinality_penalty_prefers_exact_k(self):
        q=Qubo(["a","b","c"],{},{}); add_cardinality_penalty(q, range(3), 2, 5.0)
        energies={sum(bits):q.energy(bits) for bits in ([0,0,0],[1,0,0],[1,1,0],[1,1,1])}
        self.assertAlmostEqual(energies[2], 0.0)
        self.assertGreater(energies[1], energies[2])
        self.assertGreater(energies[3], energies[2])

    def test_centroid_refinement_nonincreasing(self):
        w=TileWeights(12,1)
        p1=bytes([1]*64); p2=bytes([2]*64); p3=bytes([7]*64)
        targets=[TargetClass(p1,1,1,1),TargetClass(p2,1,1,1),TargetClass(p3,1,1,1)]
        final, score, hist=centroid_refine(targets,[p1,p3],w,iterations=3)
        costs=[h["weighted_cost"] for h in hist]
        self.assertTrue(all(b <= a + 1e-9 for a,b in zip(costs,costs[1:])))

    def test_compact_qubo_has_one_variable_per_candidate(self):
        w=TileWeights(12,1)
        pats=[bytes([v]*64) for v in (1,2,3)]
        targets=[TargetClass(p,1,1,1) for p in pats]
        mtx=cost_matrix(targets,pats,w)
        q=build_compact_qubo(targets,pats,2,mtx,w)
        self.assertEqual(len(q.variables),3)
        self.assertEqual(q.metadata["k"],2)

if __name__ == "__main__":
    unittest.main()
