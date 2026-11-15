"""Step 3 Linear Weight Optimizer (tools/tune_koma.py).

Optimizes evaluation weights on leaf features extracted in Step 2 (data/koma_features.jsonl)
without invoking the engine inside the optimization loop.

Objective:
    Loss(w) = MarginLoss(w) + a * MSE(V_root(w), score_d6) + lambda * ||w - w_default||^2
where:
    MarginLoss(w) = mean( max(0, V_w(m_neg) - V_w(m_pos) + 25)^2 )
    MSE(w)        = mean( (V_root_w - score_d6)^2 )
    L2(w)         = mean( (w - w_default)^2 )

Grid searches (a, lambda) over 3x3 values on validation set.
Accepts tuned integer weights only if validation MarginLoss decreases AND validation MSE
increases by <= 2% relative to w_default. Outputs params_koma.txt and data/tune_summary.json.
"""

from __future__ import annotations

import json
from pathlib import Path
import sys
import time
from typing import Dict, List, Tuple
import numpy as np

HOMEI_ROOT = Path(__file__).resolve().parents[1]
if str(HOMEI_ROOT) not in sys.path:
    sys.path.insert(0, str(HOMEI_ROOT))

DATA_JSONL = HOMEI_ROOT / "data" / "koma_features.jsonl"
PARAMS_OUT = HOMEI_ROOT / "params_koma.txt"
TUNE_SUMMARY_JSON = HOMEI_ROOT / "data" / "tune_summary.json"

TERM_NAMES = [
    "stack_base",
    "stack_battery",
    "binh_rel_row",
    "binh_forward",
    "binh_guard_long_king",
    "binh_king_shield_front",
    "binh_king_shield_side",
    "tuong_si_ki_rel_row",
    "center_col_weight",
    "step_attack_enemy",
    "long_phuong_active_rank",
    "xa_forward",
    "koma_dead_all_out",
    "koma_dead_part_vec",
    "koma_dead_binh_home",
    "koma_buried_home",
    "koma_buried_no_ray",
    "koma_mobile_battery",
    "koma_slider_mob_base",
    "koma_slider_mob_prime",
    "koma_xa_rel_row",
    "koma_undeveloped_ki_si",
]

W_DEFAULT = np.array(
    [
        10,  # stack_base
        20,  # stack_battery
        3,   # binh_rel_row
        6,   # binh_forward
        14,  # binh_guard_long_king
        36,  # binh_king_shield_front
        10,  # binh_king_shield_side
        3,   # tuong_si_ki_rel_row
        2,   # center_col_weight
        12,  # step_attack_enemy
        18,  # long_phuong_active_rank
        6,   # xa_forward
        24,  # koma_dead_all_out
        5,   # koma_dead_part_vec
        22,  # koma_dead_binh_home
        45,  # koma_buried_home
        30,  # koma_buried_no_ray
        8,   # koma_mobile_battery
        2,   # koma_slider_mob_base
        2,   # koma_slider_mob_prime
        2,   # koma_xa_rel_row
        10,  # koma_undeveloped_ki_si
    ],
    dtype=np.float64,
)

# Bounds for each weight so core positional structure stays sound and penalties stay positive
W_MIN = np.array(
    [
        8, 15, 2, 4, 10, 28, 8, 2, 1, 8, 12, 4,
        10, 2, 10, 20, 15, 2, 1, 1, 1, 4,
    ],
    dtype=np.float64,
)
W_MAX = np.array(
    [
        16, 30, 5, 10, 20, 45, 16, 5, 4, 18, 24, 10,
        60, 18, 55, 85, 65, 24, 6, 6, 5, 25,
    ],
    dtype=np.float64,
)


class SplitArrays:
    def __init__(
        self,
        root_base: np.ndarray,
        root_raw: np.ndarray,
        score_d6: np.ndarray,
        pair_base_diff: np.ndarray,
        pair_raw_diff: np.ndarray,
        pair_cats: List[str],
    ):
        self.root_base = root_base            # shape (N_pos,)
        self.root_raw = root_raw              # shape (N_pos, D)
        self.score_d6 = score_d6              # shape (N_pos,)
        # V(m_neg) - V(m_pos) = (neg_base - pos_base) + (neg_raw - pos_raw) @ w
        self.pair_base_diff = pair_base_diff  # shape (N_pairs,)
        self.pair_raw_diff = pair_raw_diff    # shape (N_pairs, D)
        self.pair_cats = pair_cats


def load_dataset(jsonl_path: Path) -> Tuple[SplitArrays, SplitArrays]:
    if not jsonl_path.is_file():
        raise FileNotFoundError(f"Missing dataset file: {jsonl_path}")

    splits: Dict[str, dict] = {
        "train": {"r_base": [], "r_raw": [], "d6": [], "p_bdiff": [], "p_rdiff": [], "cats": []},
        "val": {"r_base": [], "r_raw": [], "d6": [], "p_bdiff": [], "p_rdiff": [], "cats": []},
    }

    with jsonl_path.open("r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            obj = json.loads(line)
            sp = obj["split"]
            if sp not in splits:
                continue
            s = splits[sp]
            s["r_base"].append(float(obj["root_base_fixed"]))
            s["r_raw"].append([float(x) for x in obj["root_raw"]])
            s["d6"].append(float(obj["score_d6"]))

            pos_base = float(obj["m_pos"]["base_fixed"])
            pos_raw = np.array(obj["m_pos"]["raw"], dtype=np.float64)
            for neg in obj["m_negs"]:
                neg_base = float(neg["base_fixed"])
                neg_raw = np.array(neg["raw"], dtype=np.float64)
                s["p_bdiff"].append(neg_base - pos_base)
                s["p_rdiff"].append(neg_raw - pos_raw)
                s["cats"].append(str(neg.get("category", "unknown")))

    def _to_arrays(d: dict) -> SplitArrays:
        return SplitArrays(
            root_base=np.array(d["r_base"], dtype=np.float64),
            root_raw=np.array(d["r_raw"], dtype=np.float64),
            score_d6=np.array(d["d6"], dtype=np.float64),
            pair_base_diff=np.array(d["p_bdiff"], dtype=np.float64),
            pair_raw_diff=np.array(d["p_rdiff"], dtype=np.float64),
            pair_cats=list(d["cats"]),
        )

    return _to_arrays(splits["train"]), _to_arrays(splits["val"])


def compute_metrics(data: SplitArrays, w: np.ndarray, margin: float = 25.0) -> Dict[str, float]:
    # V(m_neg) - V(m_pos) + margin
    diff = data.pair_base_diff + data.pair_raw_diff @ w
    viol = np.maximum(0.0, diff + margin)
    margin_loss = float(np.mean(viol ** 2)) if len(viol) > 0 else 0.0
    pair_acc = float(np.mean(diff < 0.0)) if len(diff) > 0 else 0.0
    margin_satisfied = float(np.mean(diff <= -margin)) if len(diff) > 0 else 0.0

    v_root = data.root_base + data.root_raw @ w
    err = v_root - data.score_d6
    mse = float(np.mean(err ** 2)) if len(err) > 0 else 0.0
    mae = float(np.mean(np.abs(err))) if len(err) > 0 else 0.0

    return {
        "margin_loss": round(margin_loss, 4),
        "mse": round(mse, 4),
        "rmse": round(float(np.sqrt(mse)), 4),
        "mae": round(mae, 4),
        "pair_acc_pct": round(100.0 * pair_acc, 2),
        "margin25_acc_pct": round(100.0 * margin_satisfied, 2),
    }


def optimize_weights_adam_lbfgs(
    train: SplitArrays,
    a_mse: float,
    lam_l2: float,
    tune_only_koma: bool = False,
    margin: float = 25.0,
    steps: int = 1500,
    lr: float = 0.15,
) -> np.ndarray:
    w = W_DEFAULT.copy()
    m_adam = np.zeros_like(w)
    v_adam = np.zeros_like(w)
    beta1, beta2, eps = 0.9, 0.999, 1e-8

    n_pairs = max(1, len(train.pair_base_diff))
    n_pos = max(1, len(train.root_base))

    # Stronger anchor on base Homei_v3 weights (indices 0..11) so existing tactical balance is preserved
    l2_scale = np.ones_like(w)
    l2_scale[:12] = 10.0

    for t in range(1, steps + 1):
        # Margin gradient
        diff = train.pair_base_diff + train.pair_raw_diff @ w
        viol = np.maximum(0.0, diff + margin)
        grad_margin = (2.0 / n_pairs) * (train.pair_raw_diff.T @ viol)

        # MSE gradient
        v_root = train.root_base + train.root_raw @ w
        err = v_root - train.score_d6
        grad_mse = (2.0 / n_pos) * (train.root_raw.T @ err)

        # L2 gradient
        grad_l2 = 2.0 * lam_l2 * l2_scale * (w - W_DEFAULT)

        grad = grad_margin + a_mse * grad_mse + grad_l2
        if tune_only_koma:
            grad[:12] = 0.0

        m_adam = beta1 * m_adam + (1.0 - beta1) * grad
        v_adam = beta2 * v_adam + (1.0 - beta2) * (grad * grad)
        m_hat = m_adam / (1.0 - beta1 ** t)
        v_hat = v_adam / (1.0 - beta2 ** t)

        step_lr = lr * (0.5 if t > steps * 0.7 else 1.0)
        w = w - step_lr * m_hat / (np.sqrt(v_hat) + eps)
        w = np.clip(w, W_MIN, W_MAX)
        if tune_only_koma:
            w[:12] = W_DEFAULT[:12]

    return np.rint(w).astype(np.float64)


def run_tuning() -> dict:
    t0 = time.perf_counter()
    train, val = load_dataset(DATA_JSONL)
    print(
        f"[tune] Loaded train: {len(train.root_base)} pos ({len(train.pair_base_diff)} pairs) | "
        f"val: {len(val.root_base)} pos ({len(val.pair_base_diff)} pairs)"
    )

    train_base_metrics = compute_metrics(train, W_DEFAULT)
    val_base_metrics = compute_metrics(val, W_DEFAULT)
    print(f"[tune] Default weights on TRAIN: {train_base_metrics}")
    print(f"[tune] Default weights on VAL  : {val_base_metrics}")

    # Grid search at most 3 values along each axis (a_mse x lam_l2)
    a_grid = [0.02, 0.05, 0.15]
    lam_grid = [0.2, 1.0, 4.0]

    grid_results: List[dict] = []
    best_candidate: Optional[dict] = None

    for a_val in a_grid:
        for lam_val in lam_grid:
            for koma_only in (True,):
                w_int = optimize_weights_adam_lbfgs(
                    train, a_mse=a_val, lam_l2=lam_val, tune_only_koma=koma_only
                )
                tr_m = compute_metrics(train, w_int)
                va_m = compute_metrics(val, w_int)

                mse_change_pct = (
                    100.0 * (va_m["mse"] - val_base_metrics["mse"]) / max(1e-9, val_base_metrics["mse"])
                )
                margin_change_pct = (
                    100.0
                    * (va_m["margin_loss"] - val_base_metrics["margin_loss"])
                    / max(1e-9, val_base_metrics["margin_loss"])
                )
                eligible = (
                    va_m["margin_loss"] < val_base_metrics["margin_loss"]
                    and mse_change_pct <= 2.0
                )
                entry = {
                    "a_mse": a_val,
                    "lam_l2": lam_val,
                    "koma_only": koma_only,
                    "train": tr_m,
                    "val": va_m,
                    "val_margin_change_pct": round(margin_change_pct, 2),
                    "val_mse_change_pct": round(mse_change_pct, 2),
                    "eligible": eligible,
                    "weights": [int(x) for x in w_int],
                }
                grid_results.append(entry)
                if eligible:
                    if best_candidate is None or va_m["margin_loss"] < best_candidate["val"]["margin_loss"]:
                        best_candidate = entry

    if best_candidate is not None:
        accepted = True
        w_final = [int(x) for x in best_candidate["weights"]]
        final_train = best_candidate["train"]
        final_val = best_candidate["val"]
        chosen_hyper = {
            "a_mse": best_candidate["a_mse"],
            "lam_l2": best_candidate["lam_l2"],
            "koma_only": best_candidate["koma_only"],
        }
        reason = (
            f"Accepted tuned weights: val margin_loss decreased by "
            f"{-best_candidate['val_margin_change_pct']:.2f}% and val MSE changed by "
            f"{best_candidate['val_mse_change_pct']:+.2f}% (<= +2.0%)."
        )
    else:
        accepted = False
        w_final = [int(x) for x in W_DEFAULT]
        final_train = train_base_metrics
        final_val = val_base_metrics
        chosen_hyper = {"a_mse": None, "lam_l2": None, "koma_only": None}
        reason = "No grid candidate met both criteria (val margin_loss decrease AND val MSE increase <= 2%); retained default weights."

    # Write params_koma.txt
    lines = ["# Homei_v3-2 Koma-gumi Evaluation Parameters (integer centipawn weights)"]
    param_table = []
    for i, name in enumerate(TERM_NAMES):
        d_val = int(W_DEFAULT[i])
        f_val = int(w_final[i])
        lines.append(f"{name} {f_val}")
        param_table.append({"name": name, "default": d_val, "tuned": f_val, "delta": f_val - d_val})
    PARAMS_OUT.write_text("\n".join(lines) + "\n", encoding="utf-8")

    summary = {
        "accepted_tuned": accepted,
        "reason": reason,
        "chosen_hyper": chosen_hyper,
        "train_positions": int(len(train.root_base)),
        "train_pairs": int(len(train.pair_base_diff)),
        "val_positions": int(len(val.root_base)),
        "val_pairs": int(len(val.pair_base_diff)),
        "train_before": train_base_metrics,
        "train_after": final_train,
        "val_before": val_base_metrics,
        "val_after": final_val,
        "parameters": param_table,
        "grid_results": grid_results,
        "elapsed_sec": round(time.perf_counter() - t0, 2),
    }
    TUNE_SUMMARY_JSON.write_text(json.dumps(summary, indent=2, ensure_ascii=False), encoding="utf-8")
    print(f"[tune] {reason}")
    print(f"[tune] Saved {PARAMS_OUT} and {TUNE_SUMMARY_JSON}")
    return summary


if __name__ == "__main__":
    run_tuning()
