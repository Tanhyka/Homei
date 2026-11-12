"""All test positions from Section 6.3 (T01-T27) and Section 6.4 (S1-S8) of homei.md."""

from dataclasses import dataclass, field
from typing import List, Optional


@dataclass
class TacticalPosition:
    tid: str
    title: str
    lppn: str
    kind: str  # "EXPECT" | "MATE" | "MATE_UNVERIFIED"
    expected: List[str] = field(default_factory=list)
    max_mate_ply: int = 0


TACTICAL_POSITIONS: List[TacticalPosition] = [
    TacticalPosition(
        tid="T01",
        title="Ăn quân không được bảo vệ (Hình 1)",
        lppn="lppn1 X2K2S2T2V2T21K2X2/9/B2B2B2B2B21B2B2B2/4L21[B2P2]2/3S35/9/b0[b0l0]b0b0b0b0b0b0b0/3s0p0s0t02/x0k01t01v01k0x0 x -/- -/- 15",
        kind="EXPECT",
        expected=["Lc8", "[L]c8"],
    ),
    TacticalPosition(
        tid="T02",
        title="Ăn quân không được bảo vệ (Hình 2)",
        lppn="lppn1 X2K21T2V24/1S22T14/B2B2B2B2B21K1B2[B2X2]/4L24/3S32[B2P2B2]2/9/b0[b0l0]b0[b0p0]b0b0b0b0b0/3s02t02/x0k01t0k01s0v0x0 x -/- -/- 27",
        kind="EXPECT",
        expected=["Pc2", "[P]c2"],
    ),
    TacticalPosition(
        tid="T03",
        title="Ăn quân không được bảo vệ (Hình 3)",
        lppn="lppn1 X2K21T2V24/1S22T11p02/B2B2B2B2B21K11[B2X2]/4S22L21/6[B2P2]2/6B22/b0[b0l0]b0b0b0[b0k0]b0b0b0/3s02t02/x0k01t02s0v0x0 x -/- -/B 33",
        kind="EXPECT",
        expected=["Ke3<", "[K]e3<", "Ke3^", "Ke3>", "[K]e3^", "[K]e3>"],
    ),
    TacticalPosition(
        tid="T04",
        title="Lấy 1 quân đổi 2 quân (Hình 1)",
        lppn="lppn1 X2K21T2V24/1S22T14/B2B2B2B2B24/4[S2P2]2L2p0/6k32/6b02/b0[b0l0]b0b0b0b0t0b0b0/3s05/x0k01t02s0v0x0 x -/- B/KXBBB 45",
        kind="EXPECT",
        expected=["Kd5v"],
    ),
    TacticalPosition(
        tid="T05",
        title="Lấy 1 quân đổi 2 quân (Hình 2)",
        lppn="lppn1 1K2S31B2T2V2K21/4T22S21/B22B2B2B2B2B21/9/2P26/b01k01p01b02/1[b0l0][b0k3]b0b01b0v0b3/3s01L2x32/x04S1T02 c -/- -/TXXBBB 82",
        kind="EXPECT",
        expected=["Sh3^"],
    ),
    TacticalPosition(
        tid="T06",
        title="Đổi quân ít điểm lấy quân cao điểm (Hình 1)",
        lppn="lppn1 X2V2T2K0[B2K3]4/1S21T23L21/B2B2B2B2B3[B2S2]B32/5B23/5X23/1b01l01k0p02/b0b0b0b0b01b1b0b0/2[s0t0]1t12s01/[x0x0]1v06 c -/- K/P 58",
        kind="EXPECT",
        expected=["Xf4v", "Xf4<", "Xf4>"],
    ),
    TacticalPosition(
        tid="T07",
        title="Đổi quân ít điểm lấy quân cao điểm (Hình 2)",
        lppn="lppn1 X2V2T21K22K21/1S21T1P22S21/B2B2B2B2B2B2B2B21/7X2B2/3b05/5k23/b0b0b0b0b01b02/1v0p01t02l01/x0s11t0b11s0k01 c -/- LB/X 48",
        kind="EXPECT",
        expected=["Xh2v", "Xh2<", "Xh2>"],
    ),
    TacticalPosition(
        tid="T08",
        title="Bắt đôi (Hình 1)",
        lppn="lppn1 X2V2T2K0B24/1S21T25/B2B2B2B2B3B2[B3L2]2/2P2K31B23/9/b0b02b04/1b0b0b01s0B31b0/2s0t1x12s01/[x0x0]1v02l03 x -/- TKB/KP 87",
        kind="EXPECT",
        expected=["P*e1"],
    ),
    TacticalPosition(
        tid="T09",
        title="Bắt đôi (Hình 2)",
        lppn="lppn1 X2V2T21K22K21/1S21T1P22S21/B2B2B2B2B2B2B2B21/8B2/3b05/5k23/b0b0b0b0b01b02/1v0p01t02s01/x0s11t0b12k01 c -/- LLB/XX 50",
        kind="EXPECT",
        expected=["L*f3"],
    ),
    TacticalPosition(
        tid="T10",
        title="Đổi quân lớn lấy quân nhỏ rồi thả bắt lại",
        lppn="lppn1 X2K21T2V24/1S22T11p02/B2B2B2B2B21K1p01/4S22L2[B2X2]/6k32/6b02/b0[b0l0]b0b0b0b0t0b0b0/3s05/x0k01t02s0v0x0 x -/- B/BB 41",
        kind="EXPECT",
        expected=["Pd1"],
    ),
    TacticalPosition(
        tid="T11",
        title="Mở đường để bắt quân lớn (Hình 1)",
        lppn="lppn1 X2K21T2K2T22X2/2V2S2P22L21/B2B2B2B2B2B2B2B2B2/9/1b02S24/4[b0p0]4/b01[b0k0]1b0b0b0b0b0/3l05/x01s0t0v0t0s0k0x0 x -/- -/- 21",
        kind="EXPECT",
        expected=["Pc8"],
    ),
    TacticalPosition(
        tid="T12",
        title="Mở đường để bắt quân lớn (Hình 2)",
        lppn="lppn1 X2V2T2K0B24/1S21T23L21/B2B2B2B2B3B2B32/3K31B23/6S22/b0b03K03/1b0b0b0b0p0[b1s0]b0b0/2s0t0t14/[x0x0]1v02l03 c -/- K/XP 70",
        kind="EXPECT",
        expected=["Kh6>"],
    ),
    TacticalPosition(
        tid="T13",
        title="Mở đường để bắt quân lớn (Hình 3)",
        lppn="lppn1 X2V2T2K0B24/1S21T25/B2B2B2B2B3B2B32/3K31B23/6S22/b0b04s02/1b0b0b0b0p0b1L2b0/2s0t15/[x0x0]1v02l03 c -/- TKB/KXP 74",
        kind="EXPECT",
        expected=["Lg3"],
    ),
    TacticalPosition(
        tid="T14",
        title="Bảo vệ quân không có quân giữ (Hình 1)",
        lppn="lppn1 X21S22T22X2/1L2P21T2V21S21/B21B2B2B2B2[B2K2]B2B2/B28/4K04/9/b0b0b0b0b0b0b0b0b0/1l02p02s01/x0k0s0t0k01t0v0x0 x -/- -/- 19",
        kind="EXPECT",
        expected=["Sh6^", "Sh6>", "Sh6<", "Th6^", "Th6>", "Th5^", "Th5>", "Th5<"],
    ),
    TacticalPosition(
        tid="T15",
        title="Bảo vệ quân không có quân giữ (Hình 2)",
        lppn="lppn1 X21S22T22X2/1L22T2V21S21/2B2B2B2[B2K3]B2B2B2/2P22K03/[B2B2]8/3b0b04/1[b0b0]b0p01b0b0b0b0/1l01s01t01s01/x0k02k0t01v0x0 x -/- -/- 37",
        kind="EXPECT",
        expected=["Sg5^", "Sg5>", "Sg5<", "Bf7>", "Bf5<.f5"],
    ),
    TacticalPosition(
        tid="T16",
        title="Tăng quân bảo vệ, tránh 2 đổi 1 (Hình 1)",
        lppn="lppn1 X21S22T22X2/1L2P21T2V21S21/B21B2B2B2B2B2B2B2/B28/4K0K23/9/b0b0b0b0b0b0b0b0b0/1l01s0p02s01/x0k01t0k01t0v0x0 x -/- -/- 21",
        kind="EXPECT",
        expected=["Th5^", "Th5>", "Th5<", "Th6^", "Th6>", "Th6<"],
    ),
    TacticalPosition(
        tid="T17",
        title="Tăng quân bảo vệ (Hình 2)",
        lppn="lppn1 X21S22T22X2/1L22T2V21S21/B21B2B2B2B2B2B2B2/B28/3P2K0K23/9/b0b0b0b0b0b0b0b0b0/1l01s0p0t01s01/x0k02[k0t0]2v0x0 x -/- -/- 27",
        kind="EXPECT",
        expected=["Bg9>.g9", "Bg7<.g7"],
    ),
    TacticalPosition(
        tid="T18",
        title="Chạy quân (Hình 1)",
        lppn="lppn1 X2K21T21T2S21X2/2V2S2P22L21/B2B2B2B2B2B2[B2K2]B2B2/9/5[b0p0]3/9/b0b0[b0k0]1b0b0b0b0b0/3l05/x01s0t0v0t0s0k0x0 x -/- -/- 11",
        kind="EXPECT",
        expected=["Pf3", "[P]f3", "Pd3", "Pf5", "[P]f5"],
    ),
    TacticalPosition(
        tid="T19",
        title="Chạy quân (Hình 2)",
        lppn="lppn1 X2K21T2K2T2S21X2/2V2S2P22L21/B2B2B2B2B2B2B2B2B2/6[b0p0]2/9/9/b0b0[b0k0]1b0b0b0b0b0/3l05/x01s0t0v0t0s0k0x0 x -/- -/- 13",
        kind="EXPECT",
        expected=["Pe4", "[P]e4", "Pe2", "[P]e2", "Pf1", "[P]f1", "Pf5", "[P]f5"],
    ),
    TacticalPosition(
        tid="T20",
        title="Chạy quân (Hình 3)",
        lppn="lppn1 X2K21T2K2T22X2/P21V2S25/B21B21B2[B2P2]B21B2/1b13b03/3b0b02[B2L2]1/4b04/b0k23b0b0b0b0/3l02s02/x01s0t0v0t01k0x0 c -/- B/S 42",
        kind="EXPECT",
        expected=["Pd3", "[P]d3", "Pb5", "[P]b5", "Pb3", "[P]b3", "Pa2", "[P]a2"],
    ),
    TacticalPosition(
        tid="T21",
        title="Bảo vệ Vua (Hình 1)",
        lppn="lppn1 X2V2T24K2X2/3T21S13/B2s0B2B2B21B21B2/6B32/1P2L26/9/b03b0b0b0b0b0/4t3v0t0s01/x0k03b01k0x0 c -/- SLPBB/KBB 46",
        kind="EXPECT",
        expected=["B*b8^", "B*b8>", "B*b8v", "B*b8<"],
    ),
    TacticalPosition(
        tid="T22",
        title="Chiếu hết 3 nước (Hình 1)",
        lppn="lppn1 X2K21T2V24/1S22T11p02/B2B2B2B2B21K11[B2X2]/4S22L21/6k32/9/b0[b0l0]b0b0b0b0B2b0b0/3s02t02/x0k01t02s0v0x0 x -/- B/PBB 35",
        kind="MATE_UNVERIFIED",
        max_mate_ply=9,
    ),
    TacticalPosition(
        tid="T23",
        title="Chiếu hết 3 nước (Hình 2)",
        lppn="lppn1 X2K22T23X2/P21V26/B21B22[b3p0]B21B2/1[B2L2]7/9/9/b11T22b01b0b0/3x32s02/2v01b0t02x0 c -/- TSSKBBBBBBB/SKKL 104",
        kind="MATE",
        expected=["S*h7v", "T*h7^", "T*h7>", "T*h7v"],
        max_mate_ply=3,
    ),
    TacticalPosition(
        tid="T24",
        title="Chiếu hết 5 nước (Hình 1)",
        lppn="lppn1 1K2S31B2T2V2K21/4T22S21/B22B2[B2P2]B2B2B21/9/6b0v01/b03p01b02/1[b0l0][b0k3]b0b01b01b3/4k31S02/x05T02 c -/- SXB/TXXLB 94",
        kind="MATE",
        expected=["X*d2v"],
        max_mate_ply=5,
    ),
    TacticalPosition(
        tid="T25",
        title="Chiếu hết 5 nước (Hình 2)",
        lppn="lppn1 X2V2T2K0B21l02/1S21T21B13/B2B21B21B23/2B2b01B23/4b04/b0b04x02/1K0b0b0b03b0/l0v07/1S31p05 c -/- TTSKXBB/SKXP 154",
        kind="MATE",
        expected=["T*i8^"],
        max_mate_ply=5,
    ),
    TacticalPosition(
        tid="T26",
        title="Chiếu hết chuỗi dài (Hình 1)",
        lppn="lppn1 X2K27/1S22V2B12x3/B21B2B2B24/1B27/9/6b02/b0b0b0b0b0b0t0b0b0/3s05/x0k01t02s0v0x0 x -/- SKLP/TTKLPBBB 59",
        kind="MATE_UNVERIFIED",
        max_mate_ply=9,
    ),
    TacticalPosition(
        tid="T27",
        title="Chiếu hết chuỗi dài (Hình 2)",
        lppn="lppn1 X21V21S12l0X2/3T21S13/B2[t0k2]B2B2B21B21B2/1b04B32/2L26/9/b03b0b0b0b0b0/4P2v0t0s01/x0k03b01k0x0 c -/- TSKPBBB/- 68",
        kind="MATE",
        expected=["P*i5"],
        max_mate_ply=7,
    ),
]


@dataclass
class SyntheticPosition:
    sid: str
    title: str
    lppn: str
    kind: str  # "MATE1" | "SET" | "EXPECT" | "NOT_AND_EXPECT" | "REP_NOT"
    expected: List[str] = field(default_factory=list)
    forbidden: List[str] = field(default_factory=list)
    history_moves: List[str] = field(default_factory=list)
    current_lppn: Optional[str] = None


SYNTHETIC_POSITIONS: List[SyntheticPosition] = [
    SyntheticPosition(
        sid="S1",
        title="Chiếu hết 1 nước bằng tách Đội / Binh xoay",
        lppn="lppn1 v08/2B36/[L2T0]8/9/9/9/9/9/4V24 c -/- -/- 10",
        kind="MATE1",
        expected=["Bb8^", "Tb8<", "Tb8>", "Tb8^", "Tb9>", "Tb9^", "[T]b8<", "[T]b8>", "[T]b8^"],
    ),
    SyntheticPosition(
        sid="S2",
        title="Chiếu hết 1 nước bằng thả tù binh",
        lppn="lppn1 v01B36/9/T08/9/9/9/9/9/4V23b0 c -/- X/- 10",
        kind="MATE1",
        expected=["X*a7<", "X*a8<", "X*b9^"],
    ),
    SyntheticPosition(
        sid="S3",
        title="Bị chiếu bởi Long, phải thoát chiếu",
        lppn="lppn1 V23L24/9/9/9/9/9/5s03/3b01b03/3t0v0t03 x -/- -/B 10",
        kind="SET",
        expected=[
            "Sf5<", "Sf5>", "Sf5^",
            "Sh5<", "Sh5>", "Sh5^",
            "Th5<.i4", "Th5<.i6", "Th5>.i4", "Th5>.i6", "Th5^.i4", "Th5^.i6",
            "B*b5^", "B*b5>", "B*b5v", "B*b5<",
            "B*c5^", "B*c5>", "B*c5v", "B*c5<",
            "B*d5^", "B*d5>", "B*d5v", "B*d5<",
            "B*e5^", "B*e5>", "B*e5v", "B*e5<",
            "B*f5^", "B*f5>", "B*f5v", "B*f5<",
            "B*g5^", "B*g5>", "B*g5v", "B*g5<",
            "B*h5^", "B*h5>", "B*h5v", "B*h5<",
        ],
    ),
    SyntheticPosition(
        sid="S4",
        title="Ăn quân hở (Phượng ăn Long)",
        lppn="lppn1 3V25/9/9/9/4l04/9/2P26/9/3v05 c -/- -/- 10",
        kind="EXPECT",
        expected=["Pe5"],
    ),
    SyntheticPosition(
        sid="S5",
        title="Binh bị Long Xanh giữ (không ăn bằng Long)",
        lppn="lppn1 3V25/9/9/9/l03b04/9/4L24/9/3v05 c -/- -/- 10",
        kind="NOT_AND_EXPECT",
        forbidden=["Le5"],
        expected=[
            "Va7", "Va5", "Vb7", "Vb6", "Vb5",
            "Lf5", "Lg6", "Lg7", "Lg8", "Lg4", "Lg3", "Lg2", "Lg1",
        ],
    ),
    SyntheticPosition(
        sid="S6",
        title="Long bị Binh đe dọa, ăn Binh",
        lppn="lppn1 3V25/9/9/9/4L24/4b04/9/9/3v05 c -/- -/- 10",
        kind="EXPECT",
        expected=["Lf5"],
    ),
    SyntheticPosition(
        sid="S7",
        title="Cứu cả Đội bị Xa đe dọa (Kị hướng 0)",
        lppn="lppn1 3V25/9/9/9/4[B2T2K0]4/9/9/4x04/3v05 c -/- -/- 10",
        kind="EXPECT",
        expected=[
            "[K]c6^", "[K]c6>", "[K]c6<",
            "[K]c4^", "[K]c4>", "[K]c4<",
            "[K]g7^", "[K]g7>", "[K]g7<",
            "[K]g3^", "[K]g3>", "[K]g3<",
        ],
    ),
    SyntheticPosition(
        sid="S8",
        title="Không tự thua vì lặp thế lần 3 (Thiên Nhật Thủ)",
        lppn="lppn1 4V24/9/8B2/9/9/9/9/t07l0/4v04 c -/- -/- 20",
        kind="REP_NOT",
        forbidden=["Vi5"],
        history_moves=["Va6", "Vi6", "Va5", "Vi5", "Va6", "Vi6", "Va5"],
        current_lppn="lppn1 4V24/9/8B2/9/9/9/9/t07l0/3v05 x -/- -/- 27",
    ),
]


def all_section6_lppns() -> List[str]:
    out = [p.lppn for p in TACTICAL_POSITIONS]
    for s in SYNTHETIC_POSITIONS:
        out.append(s.lppn)
        if s.current_lppn:
            out.append(s.current_lppn)
    return out
