import importlib.util
import struct
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = (Path(__file__).parents[1] / "tools" /
               "analyze_doomguy_dense_corpus.py")
SPEC = importlib.util.spec_from_file_location(
    "analyze_doomguy_dense_corpus", MODULE_PATH)
reader = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(reader)


def build_corpus(records, angles=2, bands=1, radii=(24.0,), version=1):
    """Assemble a minimal but structurally valid DHC image.

    Version 2 appends a per-pixel material-family plane after each record's
    shade plane. Both planes are always present in a v2 file -- an asset with
    no families writes zeros -- so there is no optional path for a reader to
    get wrong.
    """
    radius_words = [int(round(r * 256)) for r in radii] + [0] * (8 - len(radii))
    head = struct.pack(
        reader.HEADER_STRUCT, b"DHC1", version, 160, 144, angles, bands, 0x81,
        78 * 256, 24 * 256, 3 * 256, 16 * 256, 80, *radius_words)
    body = b""
    for rec in records:
        crop = rec["crop"]
        body += struct.pack(
            reader.RECORD_HEAD, rec["angle"], rec["band"], rec["yaw"],
            rec["cam_x"], rec["cam_y"], rec["anchor_x"], rec["anchor_y"],
            rec["x0"], rec["y0"], rec["x1"], rec["y1"],
            sum(1 for v in crop if v))
        body += bytes(crop)
        if version >= 2:
            body += bytes(rec.get("family", (0,) * len(crop)))
    return head + body


def sample_record(angle, crop=(0, 5, 5, 0), anchor_y=115 * 256):
    return {
        "angle": angle, "band": 0, "yaw": (angle + 128) & 255,
        "cam_x": 102 * 256, "cam_y": 24 * 256,
        "anchor_x": 80 * 256, "anchor_y": anchor_y,
        "x0": 78, "y0": 40, "x1": 79, "y1": 41, "crop": crop,
    }


class DenseCorpusReaderTests(unittest.TestCase):
    def read(self, blob):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "corpus.dhc"
            path.write_bytes(blob)
            return reader.Corpus(path)

    def test_reads_header_and_records(self):
        c = self.read(build_corpus([sample_record(0), sample_record(128)]))
        self.assertEqual(len(c.samples), 2)
        self.assertEqual(c.angles, 2)
        self.assertEqual(c.radii, [24.0])
        self.assertEqual(c.pivot, (78.0, 24.0, 3.0))
        self.assertEqual(c.eye_z, 16.0)
        self.assertEqual(c.samples[1].yaw, 0)

    def test_screen_lookup_is_bbox_relative(self):
        c = self.read(build_corpus([sample_record(0), sample_record(128)]))
        s = c.samples[0]
        self.assertEqual(s.at(78, 40), 0)
        self.assertEqual(s.at(79, 40), 5)
        self.assertEqual(s.at(78, 41), 5)
        # Outside the stored crop the hero simply is not there.
        self.assertEqual(s.at(0, 0), 0)
        self.assertEqual(s.at(200, 200), 0)

    def test_row_spans_report_first_and_last_owned_column(self):
        c = self.read(build_corpus([sample_record(0), sample_record(128)]))
        self.assertEqual(c.samples[0].row_spans(), [(79, 79), (78, 78)])

    def test_rejects_pixel_count_that_disagrees_with_crop(self):
        blob = bytearray(build_corpus([sample_record(0), sample_record(128)]))
        # The pixel count is the last field of the first record header.
        head = struct.calcsize(reader.HEADER_STRUCT)
        off = head + struct.calcsize(reader.RECORD_HEAD) - 2
        blob[off:off + 2] = struct.pack("<H", 99)
        with self.assertRaises(SystemExit):
            self.read(bytes(blob))

    def test_rejects_trailing_bytes(self):
        with self.assertRaises(SystemExit):
            self.read(build_corpus([sample_record(0), sample_record(128)]) +
                      b"\x00")

    def test_rejects_foreign_magic(self):
        blob = bytearray(build_corpus([sample_record(0), sample_record(128)]))
        blob[0:4] = b"DGP1"
        with self.assertRaises(SystemExit):
            self.read(bytes(blob))

    def test_rejects_truncated_payload(self):
        blob = build_corpus([sample_record(0), sample_record(128)])
        with self.assertRaises(SystemExit):
            self.read(blob[:-2])


if __name__ == "__main__":
    unittest.main()

class FamilyPlaneTests(unittest.TestCase):
    def read(self, blob):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "corpus.dhc"
            path.write_bytes(blob)
            return reader.Corpus(str(path))

    def test_version_one_has_no_family_plane(self):
        c = self.read(build_corpus([sample_record(0), sample_record(1)]))
        self.assertEqual(c.version, 1)
        self.assertFalse(c.has_family)
        # family_at() must still answer, so callers need no version branch.
        self.assertEqual(c.samples[0].family_at(78, 40), 0)

    def test_version_two_round_trips_the_family_plane(self):
        recs = [dict(sample_record(0), family=(0, 2, 1, 0)),
                dict(sample_record(1), family=(0, 1, 1, 0))]
        c = self.read(build_corpus(recs, version=2))
        self.assertEqual(c.version, 2)
        self.assertTrue(c.has_family)
        s = c.samples[0]
        # crop is (0,5,5,0) over the 2x2 box at (78,40).
        self.assertEqual(s.family_at(79, 40), 2)
        self.assertEqual(s.family_at(78, 41), 1)
        # Outside the bounding box is not the hero, so it has no material.
        self.assertEqual(s.family_at(0, 0), 0)

    def test_a_family_outside_the_silhouette_is_rejected(self):
        # The two planes must agree about where the hero is; if they can
        # disagree, every downstream measurement is comparing different shapes.
        recs = [dict(sample_record(0), family=(3, 2, 1, 0)),
                dict(sample_record(1), family=(0, 1, 1, 0))]
        with self.assertRaises(SystemExit):
            self.read(build_corpus(recs, version=2))

    def test_an_unknown_version_is_refused_rather_than_guessed(self):
        with self.assertRaises(SystemExit):
            self.read(build_corpus([sample_record(0), sample_record(1)],
                                   version=3))
