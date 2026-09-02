import importlib.util
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).parents[1] / "tools" / "doomguy_playable_pack_to_c.py"
SPEC = importlib.util.spec_from_file_location("doomguy_playable_pack_to_c", MODULE_PATH)
packer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(packer)


class DoomguyPlayablePackerTests(unittest.TestCase):
    def test_dispatcher_emits_one_bank_selector_for_all_operations(self):
        meta = {
            "grid_w": 1, "grid_h": 1, "yaws": 2, "positions": 1,
            "origin_x": 0, "origin_y": 0, "step": 8, "eye_z": 16,
            "pool_a": 3, "pool_b": 20, "pool_size": 17,
            "dict_base": 40, "dict_count": 1, "states": 2,
        }
        state = bytes(2 + packer.MAP_BYTES)
        chunks = [(0, [state]), (1, [state])]
        with tempfile.TemporaryDirectory() as td:
            out = Path(td)
            packer.emit_dispatch(out, chunks, meta, bytes(32), bytes((0,)))
            source = (out / "doomguy_playable_dispatch.c").read_text()
            header = (out / "doomguy_playable_meta.h").read_text()

        self.assertEqual(source.count("if(state<"), 1)
        self.assertIn("}else{", source)
        self.assertEqual(source.count("doom_play_bank0("), 2)  # declaration + call
        self.assertNotIn("doom_play_patterns_bank", source)
        self.assertNotIn("doom_play_upload_bank", source)
        self.assertNotIn("doom_play_name_bank", source)
        self.assertIn("#define doom_play_state_patterns", header)
        self.assertIn("DOOM_PLAY_OP_UPLOAD", header)

    def test_data_bank_uses_one_operation_coded_entry_point(self):
        meta = {"pool_a": 3, "pool_b": 20}
        with tempfile.TemporaryDirectory() as td:
            out = Path(td)
            packer.emit_bank(out, 0, 0, [bytes(2 + packer.MAP_BYTES)], meta)
            source = (out / "doomguy_playable_bank0.c").read_text()

        self.assertEqual(source.count("uint16_t doom_play_bank0("), 1)
        self.assertIn("op==DOOM_PLAY_OP_PATTERNS", source)
        self.assertIn("op==DOOM_PLAY_OP_UPLOAD", source)
        self.assertIn("op==DOOM_PLAY_OP_NAME", source)


if __name__ == "__main__":
    unittest.main()
