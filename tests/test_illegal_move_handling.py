"""Unit tests for handling records with illegal moves (prompt / truncate on illegal)."""
import os
import sys
import unittest

sys.path.insert(0, r"E:\code5\ColongphuongGUI_1")
from engine.records import load_record, IllegalRecordMoveError
from engine.notation import LPGFNotation
from engine.game import Game

# Authentic opening 4 plies: drops of Long and Phuong
LEGAL_LPGF_PREFIX = """#lpgf version=2.0 encoding=UTF-8
Tên Phòng: Test Room
Ngày Giờ Chơi: 15-11-2026 00:00
Ngày Giờ Lưu: 15-11-2026 00:05
Kỳ Thủ Thắng: 
Kỳ Thủ Cầm Cam: CamPlayer
Kỳ Thủ Cầm Xanh: XanhPlayer

1.P*b7
2.P*h3
3.L*b8
4.L*h8
"""

LEGAL_TXT_PREFIX = """Kỳ Phổ Game Đấu
Tên Phòng: Test Room
Kỳ Thủ Cầm Cam: CamPlayer
Kỳ Thủ Cầm Xanh: XanhPlayer
Lượt đi: Cam

 1.P*b7            2.P*h3            3.L*b8            4.L*h8           
"""

class TestIllegalMoveHandling(unittest.TestCase):
    def test_lpgf_illegal_move_raise(self):
        # Move 5 tries to move a non-existent or impossible piece
        lpgf_content = LEGAL_LPGF_PREFIX + "5.Sb8(a8)\n6.Sh2\n"
        with self.assertRaises(IllegalRecordMoveError) as ctx:
            load_record(lpgf_content, "game.lpgf")
        
        exc = ctx.exception
        self.assertEqual(exc.move_no, 5)
        self.assertIn("5.Sb8(a8)", exc.move_text)
        self.assertEqual(exc.applied_moves, 4)
        self.assertIsNotNone(exc.game)
        self.assertEqual(len(list(exc.game.nodes())) - 1, 4)
        self.assertEqual(exc.metadata.get("original_format"), "lpgf")

    def test_lpgf_illegal_move_truncate(self):
        lpgf_content = LEGAL_LPGF_PREFIX + "5.Sb8(a8)\n6.Sh2\n"
        game, meta = load_record(lpgf_content, "game.lpgf", truncate_on_illegal=True)
        self.assertEqual(len(list(game.nodes())) - 1, 4)
        self.assertEqual(meta.get("illegal_move_no"), 5)
        self.assertIn("5.Sb8(a8)", meta.get("truncated_illegal_move", ""))

    def test_txt_illegal_move_raise_and_truncate(self):
        txt_content = LEGAL_TXT_PREFIX + " 5.Se5             6.Sh2\n"
        # 1. Test raise
        with self.assertRaises(IllegalRecordMoveError) as ctx:
            load_record(txt_content, "game.txt")
        exc = ctx.exception
        self.assertEqual(exc.move_no, 5)
        self.assertEqual(exc.applied_moves, 4)
        self.assertIsNotNone(exc.game)
        self.assertEqual(len(list(exc.game.nodes())) - 1, 4)

        # 2. Test truncate
        game, meta = load_record(txt_content, "game.txt", truncate_on_illegal=True)
        self.assertEqual(len(list(game.nodes())) - 1, 4)
        self.assertEqual(meta.get("illegal_move_no"), 5)

    def test_txt_syntax_error_truncate(self):
        txt_content = LEGAL_TXT_PREFIX + " 5.INVALID_MOVE\n"
        with self.assertRaises(IllegalRecordMoveError) as ctx:
            load_record(txt_content, "game.txt")
        exc = ctx.exception
        self.assertEqual(exc.move_no, 5)
        self.assertEqual(exc.applied_moves, 4)

        game, meta = load_record(txt_content, "game.txt", truncate_on_illegal=True)
        self.assertEqual(len(list(game.nodes())) - 1, 4)
        self.assertEqual(meta.get("illegal_move_no"), 5)

    def test_legal_records_remain_unaffected(self):
        # A completely legal 6-ply sequence must load without truncation or exception
        legal_lpgf = LEGAL_LPGF_PREFIX + "5.Sb3>(a3)\n6.Sh2(i3)\n"
        game, meta = load_record(legal_lpgf, "game.lpgf")
        self.assertEqual(len(list(game.nodes())) - 1, 6)
        self.assertNotIn("truncated_illegal_move", meta)

if __name__ == "__main__":
    unittest.main()
