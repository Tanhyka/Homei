"""Comprehensive unit test suite for LPGF 2.0, Standard TXT, Variations, and ASCII diagram."""

import sys
import unittest
from pathlib import Path

ENGINE_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ENGINE_ROOT))

from ref.clp.board import Board
from ref.clp.pieces import Piece, PieceType, Player
from ref.clp import rules, lppn
from ref.clp.core_bridge import CoreBridge, MoveNode
from ref.clp.record_io import (
    RecordIO,
    ImportedRecord,
    format_ascii_board,
    parse_ascii_board,
    detect_ascii_board,
    strip_source_square,
    export_to_lpgf_v2_string,
    export_to_txt_string,
    _MOVE_PATTERN,
)


class TestLPGF2Pipeline(unittest.TestCase):

    def test_ascii_diagram_roundtrip(self):
        """Test 3: ASCII diagram conversion with test position and reverse parse back to LPPN."""
        test_lppn = "lppn1 1K2V21L24/X2S2T22t0P2S2X2/B2B2B2B1B2B22B2/9/3p0b0b13/1b07/b02b0b0b01K1[b0x1]/1l01s05/x02t0v0t0s02 c -/- KKBBB/- 36"
        res = lppn.from_lppn(test_lppn, strict=True)
        board = res.board
        turn = res.turn
        ply = res.ply

        # 1. Format to ASCII
        ascii_text = format_ascii_board(board, turn, base_ply=ply)
        self.assertTrue(detect_ascii_board(ascii_text))
        self.assertIn("Cam: B3 K2", ascii_text)
        self.assertIn("Xanh: không", ascii_text)
        self.assertIn("Lượt đi: Cam", ascii_text)
        self.assertIn("36 ...", ascii_text)
        self.assertIn("[BX>]", ascii_text)
        self.assertIn("cB>", ascii_text)
        self.assertIn("cK>", ascii_text)

        # 2. Parse back to Board, turn, ply
        parsed_board, parsed_turn, parsed_ply = parse_ascii_board(ascii_text)
        self.assertEqual(parsed_turn, turn)
        self.assertEqual(parsed_ply, ply)

        # 3. Canonical LPPN comparison
        re_lppn = lppn.to_lppn(parsed_board, parsed_turn, ply=parsed_ply)
        self.assertEqual(re_lppn, test_lppn)

    def test_roundtrip_lpgf2_and_txt(self):
        """Test 1: Roundtrip LPGF 2.0 <-> TXT <-> CoreBridge with (ô_nguồn) formatting."""
        bridge = CoreBridge()
        # Legal CLP opening moves:
        # 1. P*b7
        # 2. P*h3
        # 3. L*b8
        # 4. L*h8
        # 5. Sb3>(a3)
        # 6. Sh2(i3)
        record_content = """#lpgf version=2.0 encoding=UTF-8
Tên Phòng: Phòng thử nghiệm
Ngày Giờ Chơi: 14-11-2026 14:00
Ngày Giờ Lưu: 14-11-2026 14:30
Kỳ Thủ Thắng: Chưa kết thúc
Kỳ Thủ Cầm Cam: Nguyễn Văn A
Kỳ Thủ Cầm Xanh: Trần Văn B

1.P*b7
2.P*h3
3.L*b8
4.L*h8
5.Sb3>(a3)
6.Sh2(i3)
"""
        rec = ImportedRecord()
        rec.plainText = record_content
        applied, total, err = RecordIO.load_txt_moves_into_bridge(bridge, rec)
        self.assertIsNone(err)
        self.assertEqual(applied, 6)

        # Export LPGF 2.0
        meta = {
            "room_name": "Phòng thử nghiệm",
            "date": "14-11-2026 14:00",
            "save_date": "14-11-2026 14:30",
            "winner": "Chưa kết thúc",
            "player_cam": "Nguyễn Văn A",
            "player_xanh": "Trần Văn B",
        }
        lpgf2_str = export_to_lpgf_v2_string(bridge, meta)
        self.assertIn("#lpgf version=2.0 encoding=UTF-8", lpgf2_str)
        self.assertIn("1.P*b7", lpgf2_str)  # Drop has no source
        self.assertIn("3.L*b8", lpgf2_str)  # Drop has no source
        self.assertIn("5.Sb3>(a3)", lpgf2_str)  # Board move has (a3)
        self.assertIn("6.Sh2(i3)", lpgf2_str)   # Board move has (i3)

        # Export TXT
        txt_str = export_to_txt_string(bridge, meta)
        self.assertIn("Kỳ Phổ Game Đấu", txt_str)
        # Check column 18 chars and stripped source square
        self.assertIn(" 1.P*b7           ", txt_str)
        self.assertIn(" 5.Sb3>           ", txt_str)
        self.assertIn(" 6.Sh2", txt_str)
        self.assertNotIn("(a3)", txt_str)
        self.assertNotIn("(i3)", txt_str)

        # Re-import TXT into new bridge
        bridge2 = CoreBridge()
        rec_txt = ImportedRecord()
        rec_txt.plainText = txt_str
        app2, tot2, err2 = RecordIO.load_txt_moves_into_bridge(bridge2, rec_txt)
        self.assertIsNone(err2)
        self.assertEqual(app2, 6)

    def test_multiple_variations(self):
        """Test 2: Multiple nhánh: nước thứ N variation blocks."""
        record_with_branches = """#lpgf version=2.0 encoding=UTF-8
Tên Phòng: Phòng Biến Nhánh
Ngày Giờ Chơi: 14-11-2026 14:00
Ngày Giờ Lưu: 14-11-2026 14:30
Kỳ Thủ Thắng: Chưa kết thúc
Kỳ Thủ Cầm Cam: Cam Player
Kỳ Thủ Cầm Xanh: Xanh Player

1.P*b7
2.P*h3
3.L*b8
4.L*h8
5.Sb3>(a3)
6.Sh2(i3)

nhánh: nước thứ 5
5.Bd5(c5)
6.Sh2(i3)

nhánh: nước thứ 5
5.Kc9<(a8)
6.Sh2(i3)
"""
        bridge = CoreBridge()
        rec = ImportedRecord()
        rec.plainText = record_with_branches
        app, tot, err = RecordIO.load_txt_moves_into_bridge(bridge, rec)
        self.assertIsNone(err)
        self.assertEqual(app, 6)

        # Verify bridge root node has variation branches
        nodes = bridge.root_node.all_nodes()
        # Find node at ply 4 (after 4.L*h8)
        ply4_nodes = [n for n in nodes if n.ply == 4]
        self.assertEqual(len(ply4_nodes), 1)
        ply4 = ply4_nodes[0]
        # It should have 3 children: mainline Sb3>, and branches Bd5 and Kc9<!
        self.assertEqual(len(ply4.children), 3)

        child_notations = [c.notation_text for c in ply4.children]
        self.assertIn("Sb3>", child_notations)
        self.assertIn("Bd5", child_notations)
        self.assertIn("Kc9<", child_notations)

        # Export to LPGF 2.0 and verify both branches appear
        meta = {"room_name": "Phòng Biến Nhánh"}
        lpgf2_exported = export_to_lpgf_v2_string(bridge, meta)
        self.assertIn("nhánh: nước thứ 5", lpgf2_exported)
        self.assertIn("5 Bd5(c5)", lpgf2_exported)
        self.assertIn("5 Kc9<(a8)", lpgf2_exported)

        # Export to TXT and verify both branches appear without (source)
        txt_exported = export_to_txt_string(bridge, meta)
        self.assertIn("nhánh: nước thứ 5", txt_exported)
        self.assertIn(" 5.Bd5", txt_exported)
        self.assertIn(" 5.Kc9<", txt_exported)
        self.assertNotIn("(c5)", txt_exported)
        self.assertNotIn("(a8)", txt_exported)


if __name__ == "__main__":
    unittest.main()
