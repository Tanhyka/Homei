#!/usr/bin/env python3
r"""
GPU SFNN / NNUE Trainer for Homei (CLP-HalfKA_hm, PyTorch CUDA on GTX 1060 6GB).

Usage (Python 3.12 with torch+cu124):
    py -3.12 train_nnue.py --data shard1.clpbin [shard2.clpbin ...] \
        --dll E:\all-code\Code1\CLP_engine\Homei\HomeiEngine\build_nnue\nnue_dataloader.dll \
        --out-nn E:\all-code\Code1\CLP_engine\Homei\networks\homei_v46_gen1.nn \
        --epochs 35 --batch-size 16384 --lr 1.5e-3 --wdl-lambda 0.25
"""

import argparse
import ctypes
import math
import os
import queue
import struct
import sys
import threading
import time
from pathlib import Path
from typing import List, Tuple

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

NNUE_MAGIC = 0x314E4D48  # "HMN1"
NNUE_VERSION = 1
KING_BUCKETS = 20
F_LOCAL = 5134
INPUT_SIZE = KING_BUCKETS * F_LOCAL  # 102,680
FT_HALF = 256
FT_TOTAL = FT_HALF * 2  # 512
NUM_STACKS = 8
L1_OUT = 16
L2_OUT = 32
QA = 128
QW0 = 128
QW = 64
EVAL_SCALE = 400.0
SAMPLE_BYTES = 104
MAX_ACTIVE_FEATS = 96


def load_dataloader_dll(dll_path: str) -> ctypes.CDLL:
    lib = ctypes.CDLL(dll_path)

    lib.clp_nnue_constants.argtypes = [ctypes.POINTER(ctypes.c_int32)]
    lib.clp_nnue_constants.restype = ctypes.c_int32

    lib.clp_decode_batch.argtypes = [
        ctypes.c_void_p,                  # base_samples
        ctypes.POINTER(ctypes.c_int64),   # indices
        ctypes.POINTER(ctypes.c_uint8),   # aug_mirror
        ctypes.c_int32,                   # batch_size
        ctypes.POINTER(ctypes.c_int32),   # stm_global
        ctypes.POINTER(ctypes.c_int32),   # stm_local
        ctypes.POINTER(ctypes.c_int32),   # stm_offsets
        ctypes.POINTER(ctypes.c_int32),   # opp_global
        ctypes.POINTER(ctypes.c_int32),   # opp_local
        ctypes.POINTER(ctypes.c_int32),   # opp_offsets
        ctypes.POINTER(ctypes.c_int32),   # stacks
        ctypes.POINTER(ctypes.c_float),   # scores
        ctypes.POINTER(ctypes.c_float),   # wdls
        ctypes.POINTER(ctypes.c_int32),   # out_totals[2]
    ]
    lib.clp_decode_batch.restype = ctypes.c_int32

    lib.clp_eval_packed_nnue.argtypes = [
        ctypes.c_char_p,
        ctypes.c_void_p,
        ctypes.c_int32,
        ctypes.POINTER(ctypes.c_int32),
    ]
    lib.clp_eval_packed_nnue.restype = ctypes.c_int32

    vals = (ctypes.c_int32 * 12)()
    lib.clp_nnue_constants(vals)
    assert vals[0] == KING_BUCKETS and vals[1] == F_LOCAL and vals[2] == INPUT_SIZE
    assert vals[3] == FT_HALF and vals[4] == NUM_STACKS and vals[5] == L1_OUT and vals[6] == L2_OUT
    assert vals[7] == QA and vals[8] == QW and vals[10] == SAMPLE_BYTES
    return lib


class HomeiSFNN(nn.Module):
    """Exact float mirror of C++ CLP-HalfKA_hm SFNN v1 with Virtual Shared Bucket Factorization."""

    def __init__(self):
        super().__init__()
        self.ft_bucket = nn.EmbeddingBag(INPUT_SIZE, FT_HALF, mode="sum", sparse=False)
        self.ft_shared = nn.EmbeddingBag(F_LOCAL, FT_HALF, mode="sum", sparse=False)
        self.ft_bias = nn.Parameter(torch.zeros(FT_HALF))

        self.w0 = nn.Parameter(torch.empty(NUM_STACKS, L1_OUT, FT_TOTAL))
        self.b0 = nn.Parameter(torch.zeros(NUM_STACKS, L1_OUT))
        self.w1 = nn.Parameter(torch.empty(NUM_STACKS, L2_OUT, L1_OUT - 1))
        self.b1 = nn.Parameter(torch.zeros(NUM_STACKS, L2_OUT))
        self.w2 = nn.Parameter(torch.empty(NUM_STACKS, L2_OUT))
        self.b2 = nn.Parameter(torch.zeros(NUM_STACKS, 1))

        self._init_weights()

    def _init_weights(self):
        # Start bucket delta near zero so shared features generalize immediately across all 20 king buckets
        nn.init.zeros_(self.ft_bucket.weight)
        nn.init.normal_(self.ft_shared.weight, mean=0.0, std=0.04)
        nn.init.zeros_(self.ft_bias)
        nn.init.kaiming_uniform_(self.w0, a=math.sqrt(5))
        self.w0.data.mul_(0.5)
        nn.init.kaiming_uniform_(self.w1, a=math.sqrt(5))
        self.w1.data.mul_(0.5)
        nn.init.normal_(self.w2, mean=0.0, std=0.1)
        nn.init.zeros_(self.b0)
        nn.init.zeros_(self.b1)
        nn.init.zeros_(self.b2)

    @torch.no_grad()
    def clamp_weights_(self):
        max_w0 = 127.0 / float(QW0)  # 0.9921875 -> guaranteed zero int8 overflow
        max_w = 127.0 / float(QW)    # 1.984375  -> guaranteed zero int8 overflow
        self.w0.clamp_(-max_w0, max_w0)
        self.w1.clamp_(-max_w, max_w)
        self.w2.clamp_(-max_w, max_w)

    def forward(
        self,
        stm_g: torch.Tensor,
        stm_l: torch.Tensor,
        stm_off: torch.Tensor,
        opp_g: torch.Tensor,
        opp_l: torch.Tensor,
        opp_off: torch.Tensor,
        stacks: torch.Tensor,
        qat: bool = False,
    ) -> torch.Tensor:
        max_act = 127.0 / float(QA)
        acc_stm = self.ft_bucket(stm_g, stm_off) + self.ft_shared(stm_l, stm_off) + self.ft_bias
        acc_opp = self.ft_bucket(opp_g, opp_off) + self.ft_shared(opp_l, opp_off) + self.ft_bias

        ft_stm = torch.clamp(acc_stm, 0.0, max_act).pow(2)
        ft_opp = torch.clamp(acc_opp, 0.0, max_act).pow(2)
        x = torch.cat([ft_stm, ft_opp], dim=1)  # (B, 512)

        if qat:
            w0_use = self.w0 + (torch.round(self.w0 * QW0) / float(QW0) - self.w0).detach()
            w1_use = self.w1 + (torch.round(self.w1 * QW) / float(QW) - self.w1).detach()
            w2_use = self.w2 + (torch.round(self.w2 * QW) / float(QW) - self.w2).detach()
        else:
            w0_use, w1_use, w2_use = self.w0, self.w1, self.w2

        out = torch.empty((x.size(0), 1), device=x.device, dtype=x.dtype)
        for s in range(NUM_STACKS):
            mask = stacks == s
            if not mask.any():
                continue
            xs = x[mask]
            z0 = F.linear(xs, w0_use[s], self.b0[s])
            h0 = torch.clamp(z0[:, : L1_OUT - 1], 0.0, max_act)
            skip = z0[:, L1_OUT - 1 : L1_OUT]
            h1 = torch.clamp(F.linear(h0, w1_use[s], self.b1[s]), 0.0, max_act)
            y = torch.matmul(h1, w2_use[s]).unsqueeze(1) + self.b2[s] + skip
            out[mask] = y

        return out.squeeze(1) * EVAL_SCALE

    @torch.no_grad()
    def export_nn(self, out_path: str):
        """Fold Virtual Shared weights into all 20 king buckets and export quantized HMN1 .nn file."""
        p = Path(out_path)
        p.parent.mkdir(parents=True, exist_ok=True)

        wb = self.ft_bucket.weight.detach().cpu().numpy().reshape(KING_BUCKETS, F_LOCAL, FT_HALF)
        ws = self.ft_shared.weight.detach().cpu().numpy().reshape(1, F_LOCAL, FT_HALF)
        w_full = (wb + ws).reshape(INPUT_SIZE, FT_HALF)
        ft_b = self.ft_bias.detach().cpu().numpy()

        ft_w_i16 = np.clip(np.round(w_full * QA), -32000, 32000).astype("<i2")
        ft_b_i16 = np.clip(np.round(ft_b * QA), -32000, 32000).astype("<i2")

        w0_np = self.w0.detach().cpu().numpy()
        b0_np = self.b0.detach().cpu().numpy()
        w1_np = self.w1.detach().cpu().numpy()
        b1_np = self.b1.detach().cpu().numpy()
        w2_np = self.w2.detach().cpu().numpy()
        b2_np = self.b2.detach().cpu().numpy()

        with open(p, "wb") as f:
            hdr = struct.pack(
                "<8I",
                NNUE_MAGIC,
                NNUE_VERSION,
                KING_BUCKETS,
                F_LOCAL,
                FT_HALF,
                NUM_STACKS,
                L1_OUT,
                L2_OUT,
            )
            f.write(hdr)
            f.write(ft_w_i16.tobytes())
            f.write(ft_b_i16.tobytes())
            for s in range(NUM_STACKS):
                w0_i8 = np.clip(np.round(w0_np[s] * QW0), -127, 127).astype("<i1")
                b0_i32 = np.round(b0_np[s] * (QA * QW0)).astype("<i4")
                w1_pad = np.zeros((L2_OUT, L1_OUT), dtype=np.float32)
                w1_pad[:, : L1_OUT - 1] = w1_np[s]
                w1_i8 = np.clip(np.round(w1_pad * QW), -127, 127).astype("<i1")
                b1_i32 = np.round(b1_np[s] * (QA * QW)).astype("<i4")
                w2_i8 = np.clip(np.round(w2_np[s] * QW), -127, 127).astype("<i1")
                b2_i32 = np.round(b2_np[s, 0] * (QA * QW)).astype("<i4")
                f.write(w0_i8.tobytes())
                f.write(b0_i32.tobytes())
                f.write(w1_i8.tobytes())
                f.write(b1_i32.tobytes())
                f.write(w2_i8.tobytes())
                f.write(b2_i32.tobytes())


class BatchDecoder:
    """Zero-copy C++ DLL batch decoder with reusable pre-allocated buffers."""

    def __init__(self, lib: ctypes.CDLL, samples_arr: np.ndarray, max_batch: int):
        self.lib = lib
        self.samples_arr = np.ascontiguousarray(samples_arr)
        self.base_ptr = self.samples_arr.ctypes.data
        self.max_batch = max_batch
        cap = max_batch * MAX_ACTIVE_FEATS

        self.stm_g = np.empty(cap, dtype=np.int32)
        self.stm_l = np.empty(cap, dtype=np.int32)
        self.stm_off = np.empty(max_batch + 1, dtype=np.int32)
        self.opp_g = np.empty(cap, dtype=np.int32)
        self.opp_l = np.empty(cap, dtype=np.int32)
        self.opp_off = np.empty(max_batch + 1, dtype=np.int32)
        self.stacks = np.empty(max_batch, dtype=np.int32)
        self.scores = np.empty(max_batch, dtype=np.float32)
        self.wdls = np.empty(max_batch, dtype=np.float32)
        self.totals = np.empty(2, dtype=np.int32)

    def decode(self, indices: np.ndarray, aug_mirror: np.ndarray):
        bs = int(len(indices))
        idx_c = np.ascontiguousarray(indices, dtype=np.int64)
        aug_c = np.ascontiguousarray(aug_mirror, dtype=np.uint8)
        self.lib.clp_decode_batch(
            ctypes.c_void_p(self.base_ptr),
            idx_c.ctypes.data_as(ctypes.POINTER(ctypes.c_int64)),
            aug_c.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)),
            ctypes.c_int32(bs),
            self.stm_g.ctypes.data_as(ctypes.POINTER(ctypes.c_int32)),
            self.stm_l.ctypes.data_as(ctypes.POINTER(ctypes.c_int32)),
            self.stm_off.ctypes.data_as(ctypes.POINTER(ctypes.c_int32)),
            self.opp_g.ctypes.data_as(ctypes.POINTER(ctypes.c_int32)),
            self.opp_l.ctypes.data_as(ctypes.POINTER(ctypes.c_int32)),
            self.opp_off.ctypes.data_as(ctypes.POINTER(ctypes.c_int32)),
            self.stacks.ctypes.data_as(ctypes.POINTER(ctypes.c_int32)),
            self.scores.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
            self.wdls.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
            self.totals.ctypes.data_as(ctypes.POINTER(ctypes.c_int32)),
        )
        n_stm = int(self.totals[0])
        n_opp = int(self.totals[1])
        # Copy compact slices so background thread can immediately reuse buffers
        return (
            torch.from_numpy(self.stm_g[:n_stm].copy()),
            torch.from_numpy(self.stm_l[:n_stm].copy()),
            torch.from_numpy(self.stm_off[:bs].copy()),
            torch.from_numpy(self.opp_g[:n_opp].copy()),
            torch.from_numpy(self.opp_l[:n_opp].copy()),
            torch.from_numpy(self.opp_off[:bs].copy()),
            torch.from_numpy(self.stacks[:bs].copy()),
            torch.from_numpy(self.scores[:bs].copy()),
            torch.from_numpy(self.wdls[:bs].copy()),
        )


def iter_batches_async(
    lib: ctypes.CDLL,
    samples_arr: np.ndarray,
    indices: np.ndarray,
    batch_size: int,
    augment: bool,
    rng: np.random.Generator,
):
    """Overlap C++ batch decoding on a worker thread with GPU compute."""
    q: queue.Queue = queue.Queue(maxsize=4)
    sentinel = object()
    err_holder: List[BaseException] = []

    def producer():
        try:
            dec = BatchDecoder(lib, samples_arr, batch_size)
            n = len(indices)
            for start in range(0, n, batch_size):
                end = min(n, start + batch_size)
                b_idx = indices[start:end]
                if augment:
                    aug = rng.integers(0, 2, size=len(b_idx), dtype=np.uint8)
                else:
                    aug = np.zeros(len(b_idx), dtype=np.uint8)
                q.put(dec.decode(b_idx, aug))
        except BaseException as exc:
            err_holder.append(exc)
        finally:
            q.put(sentinel)

    th = threading.Thread(target=producer, daemon=True)
    th.start()
    while True:
        item = q.get()
        if item is sentinel:
            break
        yield item
    th.join()
    if err_holder:
        raise err_holder[0]


def load_clpbin_files(paths: List[str]) -> np.ndarray:
    chunks = []
    total = 0
    for p_str in paths:
        p = Path(p_str)
        if not p.exists():
            raise FileNotFoundError(f"Missing dataset file: {p}")
        sz = p.stat().st_size
        n = sz // SAMPLE_BYTES
        if n == 0:
            print(f"[WARN] Skipping empty file: {p}")
            continue
        raw = np.fromfile(p, dtype=np.uint8, count=n * SAMPLE_BYTES).reshape(n, SAMPLE_BYTES)
        chunks.append(raw)
        total += n
        print(f"  [DATA] {p.name}: {n:,} samples ({sz / (1024*1024):.1f} MB)")
    if not chunks:
        raise RuntimeError("No training samples found!")
    if len(chunks) == 1:
        return chunks[0]
    return np.concatenate(chunks, axis=0)


@torch.no_grad()
def verify_cpp_parity(
    model: HomeiSFNN,
    lib: ctypes.CDLL,
    samples_arr: np.ndarray,
    val_indices: np.ndarray,
    nn_path: str,
    device: torch.device,
    n_check: int = 512,
) -> Tuple[float, int]:
    model.eval()
    k = min(n_check, len(val_indices))
    sub_idx = val_indices[:k]
    sub_samples = np.ascontiguousarray(samples_arr[sub_idx])
    dec = BatchDecoder(lib, sub_samples, k)
    batch = dec.decode(np.arange(k, dtype=np.int64), np.zeros(k, dtype=np.uint8))
    stm_g, stm_l, stm_off, opp_g, opp_l, opp_off, stacks, _, _ = [t.to(device) for t in batch]
    py_cp = model(stm_g, stm_l, stm_off, opp_g, opp_l, opp_off, stacks, qat=True).detach().cpu().numpy()

    cpp_cp = np.empty(k, dtype=np.int32)
    rc = lib.clp_eval_packed_nnue(
        nn_path.encode("utf-8"),
        ctypes.c_void_p(sub_samples.ctypes.data),
        ctypes.c_int32(k),
        cpp_cp.ctypes.data_as(ctypes.POINTER(ctypes.c_int32)),
    )
    if rc != k:
        raise RuntimeError(f"clp_eval_packed_nnue failed with rc={rc}")
    diffs = np.abs(py_cp - cpp_cp.astype(np.float32))
    return float(diffs.mean()), int(np.ceil(diffs.max()))


def main():
    ap = argparse.ArgumentParser(description="Train Homei CLP-HalfKA_hm SFNN/NNUE on CUDA GPU")
    ap.add_argument("--data", nargs="+", required=True, help="One or more .clpbin dataset shards")
    ap.add_argument("--dll", required=True, help="Path to nnue_dataloader.dll")
    ap.add_argument("--out-nn", required=True, help="Output quantized .nn file path")
    ap.add_argument("--checkpoint", default="", help="Optional PyTorch .pt checkpoint path")
    ap.add_argument("--resume", default="", help="Optional .pt checkpoint to resume from")
    ap.add_argument("--epochs", type=int, default=35)
    ap.add_argument("--batch-size", type=int, default=16384)
    ap.add_argument("--lr", type=float, default=1.5e-3)
    ap.add_argument("--min-lr", type=float, default=3e-5)
    ap.add_argument("--wdl-lambda", type=float, default=0.05, help="Weight on game WDL vs search score (0..1)")
    ap.add_argument("--bucket-wd", type=float, default=0.10, help="Weight decay on king-bucket delta embeddings")
    ap.add_argument("--bucket-lr-mult", type=float, default=0.15, help="LR multiplier on king-bucket delta embeddings")
    ap.add_argument("--shared-warmup", type=int, default=10, help="Epochs to freeze ft_bucket and train ft_shared + stacks first")
    ap.add_argument("--shared-wd", type=float, default=1e-3, help="Weight decay on shared local embeddings")
    ap.add_argument("--early-stop", type=int, default=6, help="Stop after N epochs without val improvement (0=off)")
    ap.add_argument("--val-frac", type=float, default=0.03)
    ap.add_argument("--seed", type=int, default=20261126)
    ap.add_argument("--no-aug", action="store_true", help="Disable 50%% horizontal mirror augmentation")
    args = ap.parse_args()

    torch.manual_seed(args.seed)
    np.random.seed(args.seed)
    rng = np.random.default_rng(args.seed)

    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    print(f"[TRAIN] PyTorch {torch.__version__} | Device: {device}", flush=True)
    if device.type == "cuda":
        print(f"        GPU: {torch.cuda.get_device_name(0)}", flush=True)

    lib = load_dataloader_dll(args.dll)
    samples = load_clpbin_files(args.data)
    n_total = len(samples)
    print(f"[TRAIN] Total loaded: {n_total:,} samples ({samples.nbytes / (1024*1024):.1f} MB RAM)", flush=True)

    # Contiguous game-level train/val split (ZERO position leakage between adjacent plies of same game!)
    n_val = max(min(50000, n_total // 10), int(n_total * args.val_frac))
    n_val = min(n_val, max(1, n_total // 5))
    train_idx = np.arange(0, n_total - n_val, dtype=np.int64)
    val_idx = np.arange(n_total - n_val, n_total, dtype=np.int64)
    print(f"[TRAIN] Contiguous unseen-game split: train={len(train_idx):,} | val={len(val_idx):,} | aug_mirror={not args.no_aug}", flush=True)

    model = HomeiSFNN().to(device)
    if args.resume and Path(args.resume).exists():
        ckpt = torch.load(args.resume, map_location=device, weights_only=True)
        model.load_state_dict(ckpt["model"])
        print(f"[TRAIN] Resumed weights from {args.resume}", flush=True)

    param_groups = [
        {"params": [model.ft_bucket.weight], "weight_decay": args.bucket_wd, "lr": args.lr * args.bucket_lr_mult},
        {"params": [model.ft_shared.weight], "weight_decay": args.shared_wd, "lr": args.lr},
        {
            "params": [
                model.ft_bias,
                model.w0,
                model.b0,
                model.w1,
                model.b1,
                model.w2,
                model.b2,
            ],
            "weight_decay": 1e-4,
            "lr": args.lr,
        },
    ]
    optimizer = torch.optim.AdamW(param_groups, lr=args.lr, betas=(0.9, 0.999), eps=1e-8)
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=max(1, args.epochs), eta_min=args.min_lr)

    best_val_loss = float("inf")
    no_improve_epochs = 0
    t_start = time.time()
    lam = float(args.wdl_lambda)
    qat_start_epoch = max(1, int(args.epochs * 0.50))

    for epoch in range(1, args.epochs + 1):
        ep_t0 = time.time()
        use_qat = (epoch >= qat_start_epoch)
        # Stage 1 (epoch <= shared_warmup): freeze ft_bucket so ft_shared + 8 stacks generalize rock-solid first!
        model.ft_bucket.weight.requires_grad_(epoch > args.shared_warmup)
        model.train()
        rng.shuffle(train_idx)

        tr_loss_sum = 0.0
        tr_count = 0

        for batch in iter_batches_async(lib, samples, train_idx, args.batch_size, not args.no_aug, rng):
            stm_g, stm_l, stm_off, opp_g, opp_l, opp_off, stacks, scores, wdls = [
                t.to(device, non_blocking=True) for t in batch
            ]
            bs = stacks.size(0)
            pred_cp = model(stm_g, stm_l, stm_off, opp_g, opp_l, opp_off, stacks, qat=use_qat)
            pred_p = torch.sigmoid(pred_cp / EVAL_SCALE)
            target_p = (1.0 - lam) * torch.sigmoid(scores / EVAL_SCALE) + lam * wdls
            loss = (pred_p - target_p).pow(2).mean()

            optimizer.zero_grad(set_to_none=True)
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 2.0)
            optimizer.step()
            model.clamp_weights_()

            tr_loss_sum += float(loss.item()) * bs
            tr_count += bs

        scheduler.step()

        # Validation on 100% unseen games (always qat=True so val tracks true quantized C++ accuracy!)
        model.eval()
        val_loss_sum = 0.0
        val_sc_sum = 0.0
        val_wdl_sum = 0.0
        val_mae_sum = 0.0
        val_count = 0
        with torch.no_grad():
            for batch in iter_batches_async(lib, samples, val_idx, args.batch_size, False, rng):
                stm_g, stm_l, stm_off, opp_g, opp_l, opp_off, stacks, scores, wdls = [
                    t.to(device, non_blocking=True) for t in batch
                ]
                bs = stacks.size(0)
                pred_cp = model(stm_g, stm_l, stm_off, opp_g, opp_l, opp_off, stacks, qat=True)
                pred_p = torch.sigmoid(pred_cp / EVAL_SCALE)
                sc_p = torch.sigmoid(scores / EVAL_SCALE)
                target_p = (1.0 - lam) * sc_p + lam * wdls
                v_loss = (pred_p - target_p).pow(2).mean()
                v_sc = (pred_p - sc_p).pow(2).mean()
                v_wdl = (pred_p - wdls).pow(2).mean()
                v_mae = (pred_cp - scores).abs().mean()

                val_loss_sum += float(v_loss.item()) * bs
                val_sc_sum += float(v_sc.item()) * bs
                val_wdl_sum += float(v_wdl.item()) * bs
                val_mae_sum += float(v_mae.item()) * bs
                val_count += bs

        tr_loss = tr_loss_sum / max(1, tr_count)
        val_loss = val_loss_sum / max(1, val_count)
        val_sc = val_sc_sum / max(1, val_count)
        val_wdl = val_wdl_sum / max(1, val_count)
        val_mae = val_mae_sum / max(1, val_count)
        ep_dt = time.time() - ep_t0
        sps = tr_count / max(0.01, ep_dt)
        lr_now = optimizer.param_groups[1]["lr"]

        marker = ""
        if val_loss < best_val_loss:
            best_val_loss = val_loss
            no_improve_epochs = 0
            model.export_nn(args.out_nn)
            if args.checkpoint:
                Path(args.checkpoint).parent.mkdir(parents=True, exist_ok=True)
                torch.save({"model": model.state_dict(), "epoch": epoch, "val_loss": val_loss}, args.checkpoint)
            marker = " *BEST*"
        else:
            no_improve_epochs += 1

        print(
            f"[EP {epoch:02d}/{args.epochs:02d}] "
            f"tr={tr_loss:.6f} val={val_loss:.6f} sc={val_sc:.6f} wdl={val_wdl:.6f} mae={val_mae:5.1f}cp "
            f"lr={lr_now:.2e} {ep_dt:4.1f}s ({sps/1000:5.0f}k samp/s){marker}",
            flush=True,
        )
        if args.early_stop > 0 and no_improve_epochs >= args.early_stop and epoch >= qat_start_epoch:
            print(f"[EARLY STOP] No val improvement for {no_improve_epochs} epochs (best_val={best_val_loss:.6f}).", flush=True)
            break

    # Reload best weights if checkpoint saved, or verify exported .nn directly against final/best
    if args.checkpoint and Path(args.checkpoint).exists():
        ckpt = torch.load(args.checkpoint, map_location=device, weights_only=True)
        model.load_state_dict(ckpt["model"])

    mean_d, max_d = verify_cpp_parity(model, lib, samples, val_idx, args.out_nn, device)
    total_dt = time.time() - t_start
    nn_mb = Path(args.out_nn).stat().st_size / (1024 * 1024)
    print(
        f"[DONE] total={total_dt:.1f}s best_val={best_val_loss:.6f} "
        f"out={args.out_nn} ({nn_mb:.2f} MB) | "
        f"PyTorch<->C++ parity (512 val): mean_diff={mean_d:.3f}cp max_diff={max_d}cp",
        flush=True,
    )


if __name__ == "__main__":
    main()
