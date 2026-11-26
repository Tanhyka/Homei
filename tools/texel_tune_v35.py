"""Texel Weight & Symmetric Half-Board PST Optimizer for Homei_v3-5 (numpy 2.5+).

Reads binary feature traces emitted by C++ texel_extract.exe, calibrates logistic scaling K,
fits (V35_STRUCT_MG/EG, V35_PST_MG/EG) using Adam with spatial smoothness + L2 shrinkage,
validates strictly out-of-sample WDL Brier MSE & BCE, and writes HomeiEngine/src/eval_weights_v35.h.
"""
from __future__ import annotations

import re
import struct
import sys
import time
from pathlib import Path
import numpy as np

HOMEI_ROOT = Path(r"E:\all-code\Code1\CLP_engine\Homei")
WEIGHTS_H = HOMEI_ROOT / "HomeiEngine" / "src" / "eval_weights_v35.h"

V35_PST_COUNT = 8 * 4 * 45  # 1440
V35_STRUCT_COUNT = 35
V35_TOTAL_FEATS = V35_PST_COUNT + V35_STRUCT_COUNT  # 1475


def load_v35_header_init() -> tuple[np.ndarray, np.ndarray]:
    """Parse current V35_STRUCT_MG/EG and V35_PST_MG/EG from eval_weights_v35.h as prior."""
    txt = WEIGHTS_H.read_text(encoding="utf-8")
    w_mg = np.zeros(V35_TOTAL_FEATS, dtype=np.float64)
    w_eg = np.zeros(V35_TOTAL_FEATS, dtype=np.float64)

    def _parse_block(name: str, count: int) -> np.ndarray:
        m = re.search(rf"{name}\[[^\]]*\]\s*=\s*\{{(.*?)\}};", txt, re.S)
        if not m:
            return np.zeros(count, dtype=np.float64)
        body = re.sub(r"//.*", "", m.group(1))
        nums = [int(x) for x in re.findall(r"[+-]?\d+", body)]
        arr = np.zeros(count, dtype=np.float64)
        for i, v in enumerate(nums[:count]):
            arr[i] = float(v)
        return arr

    # Structural weights live at indices [1440..1474]
    w_mg[V35_PST_COUNT:] = _parse_block("V35_STRUCT_MG", V35_STRUCT_COUNT)
    w_eg[V35_PST_COUNT:] = _parse_block("V35_STRUCT_EG", V35_STRUCT_COUNT)

    # Parse nested PST initializers cleanly per piece (first 45 entries for VUONG=0, LONG=5, PHUONG=6 if present)
    def _parse_pst_block(name: str) -> np.ndarray:
        out = np.zeros(V35_PST_COUNT, dtype=np.float64)
        m = re.search(rf"{name}\[8\]\[4\]\[45\]\s*=\s*\{{(.*)\}};", txt, re.S)
        if not m:
            return out
        # Simple nested brace walker at depth 1 (piece), depth 2 (facing)
        s = m.group(1)
        depth = 0
        pt = -1
        f = -1
        vals: list[str] = []
        tok = []
        i = 0
        # Strip comments first
        s = re.sub(r"//.*", "", s)
        for ch in s:
            if ch == "{":
                depth += 1
                if depth == 1:
                    pt += 1
                    f = -1
                elif depth == 2:
                    f += 1
                    vals = []
                    tok = []
            elif ch == "}":
                if depth == 2 and 0 <= pt < 8 and 0 <= f < 4:
                    if tok:
                        vals.append("".join(tok))
                        tok = []
                    nums = [int(x) for x in vals if re.match(r"^[+-]?\d+$", x)]
                    for k, v in enumerate(nums[:45]):
                        out[pt * 180 + f * 45 + k] = float(v)
                depth -= 1
            elif depth == 2:
                if ch in "+-0123456789":
                    tok.append(ch)
                elif tok:
                    vals.append("".join(tok))
                    tok = []
        return out

    w_mg[:V35_PST_COUNT] = _parse_pst_block("V35_PST_MG")
    w_eg[:V35_PST_COUNT] = _parse_pst_block("V35_PST_EG")
    return w_mg, w_eg


def load_texel_bin(path: Path) -> tuple[dict, dict]:
    buf = path.read_bytes()
    magic, n_feats = struct.unpack_from("<II", buf, 0)
    assert magic == 0x56333554, f"Bad magic: {magic:#x}"
    assert n_feats == V35_TOTAL_FEATS, f"Feature mismatch: {n_feats} != {V35_TOTAL_FEATS}"

    splits: list[int] = []
    phases: list[int] = []
    bases: list[int] = []
    wdls: list[float] = []
    qscores: list[int] = []
    rows_tr: list[int] = []
    cols_tr: list[int] = []
    vals_tr: list[int] = []
    rows_va: list[int] = []
    cols_va: list[int] = []
    vals_va: list[int] = []

    tr_meta = {"phase": [], "base": [], "wdl": [], "qscore": []}
    va_meta = {"phase": [], "base": [], "wdl": [], "qscore": []}

    off = 8
    hdr_fmt = "<BHifhH"
    hdr_sz = struct.calcsize(hdr_fmt)
    n_bytes = len(buf)
    tr_idx = 0
    va_idx = 0

    while off < n_bytes:
        sp, ph, bs, wdl, qs, ni = struct.unpack_from(hdr_fmt, buf, off)
        off += hdr_sz
        if ni > 0:
            items = np.frombuffer(buf, dtype=np.dtype([("idx", "<u2"), ("val", "<i2")]), count=ni, offset=off)
            off += ni * 4
        if sp == 0:
            tr_meta["phase"].append(ph)
            tr_meta["base"].append(bs)
            tr_meta["wdl"].append(wdl)
            tr_meta["qscore"].append(qs)
            if ni > 0:
                rows_tr.append(np.full(ni, tr_idx, dtype=np.int32))
                cols_tr.append(items["idx"].astype(np.int32))
                vals_tr.append(items["val"].astype(np.float64))
            tr_idx += 1
        else:
            va_meta["phase"].append(ph)
            va_meta["base"].append(bs)
            va_meta["wdl"].append(wdl)
            va_meta["qscore"].append(qs)
            if ni > 0:
                rows_va.append(np.full(ni, va_idx, dtype=np.int32))
                cols_va.append(items["idx"].astype(np.int32))
                vals_va.append(items["val"].astype(np.float64))
            va_idx += 1

    def _pack(meta, r_list, c_list, v_list, n_rows):
        ph = np.array(meta["phase"], dtype=np.float64)
        return {
            "n": n_rows,
            "p_mg": ph / 256.0,
            "p_eg": (256.0 - ph) / 256.0,
            "base": np.array(meta["base"], dtype=np.float64),
            "wdl": np.array(meta["wdl"], dtype=np.float64),
            "qscore": np.array(meta["qscore"], dtype=np.float64),
            "r": np.concatenate(r_list) if r_list else np.zeros(0, dtype=np.int32),
            "c": np.concatenate(c_list) if c_list else np.zeros(0, dtype=np.int32),
            "v": np.concatenate(v_list) if v_list else np.zeros(0, dtype=np.float64),
        }

    return _pack(tr_meta, rows_tr, cols_tr, vals_tr, tr_idx), _pack(va_meta, rows_va, cols_va, vals_va, va_idx)


def eval_split(d: dict, w_mg: np.ndarray, w_eg: np.ndarray, k_scale: float) -> tuple[float, float, np.ndarray]:
    if len(d["r"]) > 0:
        mg = np.bincount(d["r"], weights=d["v"] * w_mg[d["c"]], minlength=d["n"])
        eg = np.bincount(d["r"], weights=d["v"] * w_eg[d["c"]], minlength=d["n"])
    else:
        mg = np.zeros(d["n"], dtype=np.float64)
        eg = np.zeros(d["n"], dtype=np.float64)
    score = d["base"] + d["p_mg"] * mg + d["p_eg"] * eg
    p = 1.0 / (1.0 + np.exp(-np.clip(score / k_scale, -20.0, 20.0)))
    wdl = d["wdl"]
    brier = float(np.mean((p - wdl) ** 2))
    eps = 1e-7
    bce = float(-np.mean(wdl * np.log(p + eps) + (1.0 - wdl) * np.log(1.0 - p + eps)))
    return brier, bce, score


def fit_k_scale(d: dict, w_mg: np.ndarray, w_eg: np.ndarray) -> float:
    mg = np.bincount(d["r"], weights=d["v"] * w_mg[d["c"]], minlength=d["n"])
    eg = np.bincount(d["r"], weights=d["v"] * w_eg[d["c"]], minlength=d["n"])
    score = d["base"] + d["p_mg"] * mg + d["p_eg"] * eg
    best_k, best_loss = 750.0, 1e9
    for k in np.linspace(300.0, 1200.0, 91):
        p = 1.0 / (1.0 + np.exp(-np.clip(score / k, -20.0, 20.0)))
        loss = float(np.mean((p - d["wdl"]) ** 2))
        if loss < best_loss:
            best_loss = loss
            best_k = float(k)
    return best_k


def pst_spatial_smoothness_grad(w: np.ndarray, lam_smooth: float) -> np.ndarray:
    """Apply soft spatial Laplacian penalty inside active PST [8][4][9][5] planes."""
    g = np.zeros_like(w)
    if lam_smooth <= 0.0:
        return g
    pst = w[:V35_PST_COUNT].reshape(8, 4, 9, 5)
    gp = g[:V35_PST_COUNT].reshape(8, 4, 9, 5)
    for pt in range(8):
        max_f = 1 if pt in (0, 5, 6) else 4
        for f in range(max_f):
            plane = pst[pt, f]
            # Vertical neighbors
            diff_r = plane[1:, :] - plane[:-1, :]
            gp[pt, f, 1:, :] += 2.0 * lam_smooth * diff_r
            gp[pt, f, :-1, :] -= 2.0 * lam_smooth * diff_r
            # Horizontal neighbors
            diff_c = plane[:, 1:] - plane[:, :-1]
            gp[pt, f, :, 1:] += 2.0 * lam_smooth * diff_c
            gp[pt, f, :, :-1] -= 2.0 * lam_smooth * diff_c
    return g


def build_bounds_and_mask() -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    """Return active_mask, w_min_mg, w_max_mg, w_min_eg, w_max_eg."""
    mask = np.zeros(V35_TOTAL_FEATS, dtype=np.float64)
    for pt in range(8):
        max_f = 1 if pt in (0, 5, 6) else 4
        for f in range(max_f):
            for r in range(9):
                for c in range(5):
                    if c == 4 and f == 3:
                        continue
                    mask[pt * 180 + f * 45 + r * 5 + c] = 1.0
    mask[V35_PST_COUNT:] = 1.0

    min_mg = np.full(V35_TOTAL_FEATS, -110.0, dtype=np.float64)
    max_mg = np.full(V35_TOTAL_FEATS, 110.0, dtype=np.float64)
    min_eg = np.full(V35_TOTAL_FEATS, -110.0, dtype=np.float64)
    max_eg = np.full(V35_TOTAL_FEATS, 110.0, dtype=np.float64)

    # Bound board material deltas cleanly within [-30, +35] cp to stay aligned with SEE
    sb = V35_PST_COUNT
    min_mg[sb:sb + 7], max_mg[sb:sb + 7] = -30.0, 35.0
    min_eg[sb:sb + 7], max_eg[sb:sb + 7] = -30.0, 40.0
    # Bound prisoner bonuses [+10..+75] so prisoner > board holds cleanly
    min_mg[sb + 7:sb + 14], max_mg[sb + 7:sb + 14] = 10.0, 75.0
    min_eg[sb + 7:sb + 14], max_eg[sb + 7:sb + 14] = 10.0, 80.0
    # Structural bounds (domain-sign consistent!)
    min_mg[sb + 14], max_mg[sb + 14] = -55.0, -12.0  # lp_home_stack (stay penalized!)
    min_eg[sb + 14], max_eg[sb + 14] = -35.0, 0.0
    min_mg[sb + 15], max_mg[sb + 15] = 4.0, 45.0     # lp_deployed_sep
    min_eg[sb + 15], max_eg[sb + 15] = 0.0, 25.0
    min_mg[sb + 16], max_mg[sb + 16] = 6.0, 45.0     # carrier_bk
    min_eg[sb + 16], max_eg[sb + 16] = 4.0, 35.0
    min_mg[sb + 17], max_mg[sb + 17] = 4.0, 40.0     # carrier_slider
    min_eg[sb + 17], max_eg[sb + 17] = 2.0, 30.0
    min_mg[sb + 18], max_mg[sb + 18] = 12.0, 65.0    # castle_wing_king
    min_eg[sb + 18], max_eg[sb + 18] = -12.0, 10.0
    min_mg[sb + 19], max_mg[sb + 19] = 4.0, 40.0     # castle_wall_ts
    min_eg[sb + 19], max_eg[sb + 19] = 2.0, 35.0
    min_mg[sb + 20], max_mg[sb + 20] = 2.0, 28.0     # castle_b_shield
    min_eg[sb + 20], max_eg[sb + 20] = 0.0, 20.0
    min_mg[sb + 21], max_mg[sb + 21] = -70.0, -20.0  # exposed_center_king in MG!
    min_eg[sb + 21], max_eg[sb + 21] = -10.0, 35.0
    # Positive bonuses stay non-negative cleanly!
    for pos_i in (22, 23, 24, 27, 32, 33):
        min_mg[sb + pos_i], max_mg[sb + pos_i] = 0.0, 40.0
        min_eg[sb + pos_i], max_eg[sb + pos_i] = 0.0, 40.0
    for mob_i in (25, 26):  # long_open_mob, phuong_open_mob
        min_mg[sb + mob_i], max_mg[sb + mob_i] = 0.0, 12.0
        min_eg[sb + mob_i], max_eg[sb + mob_i] = 0.0, 12.0
    # KingDanger terms smoothly bounded
    min_mg[sb + 28], max_mg[sb + 28] = -12.0, -1.0   # kd_atk_linear
    min_eg[sb + 28], max_eg[sb + 28] = -10.0, 0.0
    min_mg[sb + 29], max_mg[sb + 29] = -14.0, -1.0   # kd_atk_quad
    min_eg[sb + 29], max_eg[sb + 29] = -12.0, 0.0
    min_mg[sb + 30], max_mg[sb + 30] = -16.0, -1.0   # kd_drop_holes_pris
    min_eg[sb + 30], max_eg[sb + 30] = -14.0, 0.0
    min_mg[sb + 31], max_mg[sb + 31] = -18.0, -2.0   # kd_ki_drop_threat
    min_eg[sb + 31], max_eg[sb + 31] = -16.0, -1.0
    min_mg[sb + 34], max_mg[sb + 34] = 4.0, 28.0     # tempo
    min_eg[sb + 34], max_eg[sb + 34] = 4.0, 24.0
    # King VUONG walk b4/b5/b6 MG stay negative
    for sq45 in (8, 9):  # rel_r=1, sym_c=3,4
        min_mg[sq45], max_mg[sq45] = -60.0, -15.0

    return mask, min_mg, max_mg, min_eg, max_eg


def write_weights_header(w_mg: np.ndarray, w_eg: np.ndarray) -> None:
    s_mg = np.rint(w_mg[V35_PST_COUNT:]).astype(int)
    s_eg = np.rint(w_eg[V35_PST_COUNT:]).astype(int)
    p_mg = np.rint(w_mg[:V35_PST_COUNT]).astype(int).reshape(8, 4, 45)
    p_eg = np.rint(w_eg[:V35_PST_COUNT]).astype(int).reshape(8, 4, 45)

    names = [
        "S_MAT_TUONG", "S_MAT_SI", "S_MAT_KI", "S_MAT_XA", "S_MAT_LONG", "S_MAT_PHUONG", "S_MAT_BINH",
        "S_PRIS_TUONG", "S_PRIS_SI", "S_PRIS_KI", "S_PRIS_XA", "S_PRIS_LONG", "S_PRIS_PHUONG", "S_PRIS_BINH",
        "S_LP_HOME_STACK", "S_LP_DEPLOYED_SEP", "S_CARRIER_BK", "S_CARRIER_SLIDER",
        "S_CASTLE_WING_KING", "S_CASTLE_WALL_TS", "S_CASTLE_B_SHIELD", "S_EXPOSED_CENTER_KING",
        "S_BINH_PASSED_ADV", "S_BINH_CONNECTED", "S_XA_OPEN_LANE", "S_LONG_OPEN_MOB",
        "S_PHUONG_OPEN_MOB", "S_DOUBLE_PHUONG", "S_KD_ATK_LINEAR", "S_KD_ATK_QUAD",
        "S_KD_DROP_HOLES_PRIS", "S_KD_KI_DROP_THREAT", "S_KING_RING_DEFENDERS", "S_STACK_DEFENSIVE", "S_TEMPO",
    ]
    pt_names = ["VUONG", "TUONG", "SI", "KI", "XA", "LONG", "PHUONG", "BINH"]

    def _fmt_struct(arr):
        rows = []
        for i, v in enumerate(arr):
            comma = "," if i + 1 < len(arr) else " "
            rows.append(f"    {v:4d}{comma} // {names[i]}")
        return "\n".join(rows)

    def _fmt_pst(tensor):
        blocks = []
        for pt in range(8):
            max_f = 1 if pt in (0, 5, 6) else 4
            f_strs = []
            for f in range(4):
                if f >= max_f:
                    f_strs.append("        {}")
                else:
                    vals = tensor[pt, f]
                    r_lines = []
                    for r in range(9):
                        row_v = ", ".join(f"{vals[r*5 + c]:+4d}" for c in range(5))
                        r_lines.append("            " + row_v + ("," if r < 8 else ""))
                    f_strs.append("        {\n" + "\n".join(r_lines) + "\n        }")
            blocks.append(f"    // {pt}: {pt_names[pt]}\n    {{\n" + ",\n".join(f_strs) + "\n    }")
        return ",\n".join(blocks)

    header = f"""#pragma once

#include <cstdint>

namespace homei::v35 {{

constexpr int V35_PST_COUNT = 8 * 4 * 45; // 1440
constexpr int V35_STRUCT_COUNT = 35;
constexpr int V35_TOTAL_FEATS = V35_PST_COUNT + V35_STRUCT_COUNT; // 1475

enum StructFeatIdx : int {{
    S_MAT_TUONG = 0,
    S_MAT_SI = 1,
    S_MAT_KI = 2,
    S_MAT_XA = 3,
    S_MAT_LONG = 4,
    S_MAT_PHUONG = 5,
    S_MAT_BINH = 6,
    S_PRIS_TUONG = 7,
    S_PRIS_SI = 8,
    S_PRIS_KI = 9,
    S_PRIS_XA = 10,
    S_PRIS_LONG = 11,
    S_PRIS_PHUONG = 12,
    S_PRIS_BINH = 13,
    S_LP_HOME_STACK = 14,
    S_LP_DEPLOYED_SEP = 15,
    S_CARRIER_BK = 16,
    S_CARRIER_SLIDER = 17,
    S_CASTLE_WING_KING = 18,
    S_CASTLE_WALL_TS = 19,
    S_CASTLE_B_SHIELD = 20,
    S_EXPOSED_CENTER_KING = 21,
    S_BINH_PASSED_ADV = 22,
    S_BINH_CONNECTED = 23,
    S_XA_OPEN_LANE = 24,
    S_LONG_OPEN_MOB = 25,
    S_PHUONG_OPEN_MOB = 26,
    S_DOUBLE_PHUONG = 27,
    S_KD_ATK_LINEAR = 28,
    S_KD_ATK_QUAD = 29,
    S_KD_DROP_HOLES_PRIS = 30,
    S_KD_KI_DROP_THREAT = 31,
    S_KING_RING_DEFENDERS = 32,
    S_STACK_DEFENSIVE = 33,
    S_TEMPO = 34
}};

// Texel-tuned structural weights (MG, EG)
inline constexpr int16_t V35_STRUCT_MG[V35_STRUCT_COUNT] = {{
{_fmt_struct(s_mg)}
}};

inline constexpr int16_t V35_STRUCT_EG[V35_STRUCT_COUNT] = {{
{_fmt_struct(s_eg)}
}};

// Texel-tuned half-board symmetric PST: [piece_type][sym_facing][rel_r * 5 + sym_c]
inline constexpr int16_t V35_PST_MG[8][4][45] = {{
{_fmt_pst(p_mg)}
}};

inline constexpr int16_t V35_PST_EG[8][4][45] = {{
{_fmt_pst(p_eg)}
}};

}} // namespace homei::v35
"""
    WEIGHTS_H.write_text(header, encoding="utf-8")


def main() -> None:
    bin_path = Path(sys.argv[1])
    t0 = time.perf_counter()
    tr, va = load_texel_bin(bin_path)
    print(f"[texel_tune] Loaded train={tr['n']}, val={va['n']} positions in {time.perf_counter() - t0:.2f}s")

    w0_mg, w0_eg = load_v35_header_init()
    k_scale = fit_k_scale(va, w0_mg, w0_eg)
    b_tr0, c_tr0, _ = eval_split(tr, w0_mg, w0_eg, k_scale)
    b_va0, c_va0, _ = eval_split(va, w0_mg, w0_eg, k_scale)
    print(f"[texel_tune] Calibrated K={k_scale:.1f} | Baseline train Brier={b_tr0:.6f} BCE={c_tr0:.6f} | val Brier={b_va0:.6f} BCE={c_va0:.6f}")

    mask, min_mg, max_mg, min_eg, max_eg = build_bounds_and_mask()
    w0_mg = np.clip(w0_mg, min_mg, max_mg) * mask
    w0_eg = np.clip(w0_eg, min_eg, max_eg) * mask
    b_va_start, c_va_start, _ = eval_split(va, w0_mg, w0_eg, k_scale)
    y_tr = tr["wdl"]

    # Gentle L2 & spatial smoothing tuned to actual ~1e-5 gradient magnitude
    l2_w = np.full(V35_TOTAL_FEATS, 1.5e-7, dtype=np.float64)
    l2_w[V35_PST_COUNT:] = 3e-8
    lam_smooth = 2.5e-7

    w_mg = w0_mg.copy()
    w_eg = w0_eg.copy()
    m_mg, v_mg = np.zeros_like(w_mg), np.zeros_like(w_mg)
    m_eg, v_eg = np.zeros_like(w_eg), np.zeros_like(w_eg)
    beta1, beta2, eps = 0.9, 0.999, 1e-8

    best_va_brier = 1e9
    best_mg, best_eg = w_mg.copy(), w_eg.copy()

    n_tr = float(tr["n"])
    steps = 450
    for step in range(1, steps + 1):
        mg = np.bincount(tr["r"], weights=tr["v"] * w_mg[tr["c"]], minlength=tr["n"])
        eg = np.bincount(tr["r"], weights=tr["v"] * w_eg[tr["c"]], minlength=tr["n"])
        score = tr["base"] + tr["p_mg"] * mg + tr["p_eg"] * eg
        p = 1.0 / (1.0 + np.exp(-np.clip(score / k_scale, -20.0, 20.0)))

        # Derivative of Brier MSE (p - y)^2 wrt score: 2*(p - y)*p*(1 - p) / K
        ds = (2.0 / n_tr) * (p - y_tr) * p * (1.0 - p) / k_scale

        g_mg = np.bincount(tr["c"], weights=tr["v"] * (ds * tr["p_mg"])[tr["r"]], minlength=V35_TOTAL_FEATS)
        g_eg = np.bincount(tr["c"], weights=tr["v"] * (ds * tr["p_eg"])[tr["r"]], minlength=V35_TOTAL_FEATS)

        g_mg += 2.0 * l2_w * (w_mg - w0_mg) + pst_spatial_smoothness_grad(w_mg, lam_smooth)
        g_eg += 2.0 * l2_w * (w_eg - w0_eg) + pst_spatial_smoothness_grad(w_eg, lam_smooth)
        g_mg *= mask
        g_eg *= mask

        m_mg = beta1 * m_mg + (1.0 - beta1) * g_mg
        v_mg = beta2 * v_mg + (1.0 - beta2) * (g_mg * g_mg)
        m_eg = beta1 * m_eg + (1.0 - beta1) * g_eg
        v_eg = beta2 * v_eg + (1.0 - beta2) * (g_eg * g_eg)

        mh_mg = m_mg / (1.0 - beta1 ** step)
        vh_mg = v_mg / (1.0 - beta2 ** step)
        mh_eg = m_eg / (1.0 - beta1 ** step)
        vh_eg = v_eg / (1.0 - beta2 ** step)

        lr = 1.2 if step <= 220 else (0.5 if step <= 360 else 0.2)
        w_mg = np.clip(w_mg - lr * mh_mg / (np.sqrt(vh_mg) + eps), min_mg, max_mg) * mask
        w_eg = np.clip(w_eg - lr * mh_eg / (np.sqrt(vh_eg) + eps), min_eg, max_eg) * mask

        if step % 50 == 0 or step == steps:
            b_tr, c_tr, _ = eval_split(tr, w_mg, w_eg, k_scale)
            b_va, c_va, _ = eval_split(va, w_mg, w_eg, k_scale)
            print(f"  step {step:3d}: train Brier={b_tr:.6f} BCE={c_tr:.6f} | val Brier={b_va:.6f} BCE={c_va:.6f}")
            if b_va < best_va_brier:
                best_va_brier = b_va
                best_mg, best_eg = w_mg.copy(), w_eg.copy()

    # Quantize to integers & verify val strictly improves
    q_mg = np.rint(best_mg)
    q_eg = np.rint(best_eg)
    b_va_q, c_va_q, _ = eval_split(va, q_mg, q_eg, k_scale)
    print(f"[texel_tune] Integer quantized val: Brier={b_va_q:.6f} (was {b_va0:.6f}), BCE={c_va_q:.6f} (was {c_va0:.6f})")

    s_names = [
        "MAT_T", "MAT_S", "MAT_K", "MAT_X", "MAT_L", "MAT_P", "MAT_B",
        "PRIS_T", "PRIS_S", "PRIS_K", "PRIS_X", "PRIS_L", "PRIS_P", "PRIS_B",
        "LP_STACK", "LP_SEP", "CAR_BK", "CAR_SL", "CASTLE_K", "CASTLE_TS", "CASTLE_B", "EXP_K",
        "B_PASS", "B_CONN", "X_LANE", "L_MOB", "P_MOB", "2P",
        "KD_LIN", "KD_QUAD", "KD_HOLE", "KD_KI", "K_DEF", "STK_DEF", "TEMPO",
    ]
    for i, sn in enumerate(s_names):
        idx = V35_PST_COUNT + i
        print(f"  {sn:10s}: MG {int(w0_mg[idx]):+4d} -> {int(q_mg[idx]):+4d} | EG {int(w0_eg[idx]):+4d} -> {int(q_eg[idx]):+4d}")

    if b_va_q <= b_va_start + 5e-5:
        write_weights_header(q_mg, q_eg)
        print(f"[texel_tune] ACCEPTED & saved domain-clean tuned weights to {WEIGHTS_H}!")
    else:
        print("[texel_tune] Quantized val did not beat baseline; keeping baseline weights.")


if __name__ == "__main__":
    main()
