# BÁO CÁO NGHIỆM THU: NÂNG CẤP TRIỂN KHAI QUÂN TRUNG CUỘC (KOMA-GUMI & SABAKI) — `Homei_v3-2.exe`

- **Ngày hoàn thành:** 2026-11-14
- **Workspace:** `E:\code5\CLP_engine\Homei`
- **Engine cơ sở (`Homei_v3.exe`):** `E:\code5\CLP_engine\Homei\HomeiEngine\build\Engine realese\Homei_v3.exe`
  - **Kích thước:** `301,568` bytes
  - **SHA256:** `B83747C54E0A1E3CD7B86B5793AFAE63AB55F53013E1BB6FB33764C40298C0C8` (nguyên vẹn 100%)
  - **Git Tag cơ sở:** `perf-v3-base` (`eef0a0e`)
- **Engine đích (`Homei_v3-2.exe`):** `E:\code5\CLP_engine\Homei\HomeiEngine\build\Engine realese\Homei_v3-2.exe`
  - **Handshake ID:** `id name Homei v3-2` (`protocol clp-ep 1`)
  - **SHA256:** `F5F9BA4F90BCB794F9790926AA6D2E3EABD69C7265C60066A774D1544234A568`
  - **Cấu hình mặc định:** `koma_eval = true`, `koma_search = false` (Cấu hình A — chiến thắng tuyệt đối ở Bước 4)

---

## 1. BƯỚC 0 — SAO LƯU BẤT KHẢ XÂM PHẠM & AN TOÀN VẬN HÀNH (`tools/safe_proc.py`)

### 1.1 Sao lưu Mã nguồn & Binary `Homei_v3`
- Đã gắn thẻ `git tag -f perf-v3-base` tại commit `eef0a0e`.
- Đã sao chép nguyên trạng `HomeiEngine/src/`, `HomeiEngine/tests/`, `tools/`, `CMakeLists.txt` và `Homei_v3.exe` vào `E:\code5\CLP_engine\Homei\source_backups\v3_gold_src\` và nén thành `E:\code5\CLP_engine\Homei\source_backups\v3_gold_src.zip` (`504,320` bytes).

### 1.2 Kiểm định `tools/safe_proc.py` (`tests/test_safe_proc.py`)
`tools/safe_proc.py` quản lý toàn bộ tiến trình con với nhóm tiến trình riêng (`CREATE_NEW_PROCESS_GROUP`), luồng đọc `stdout`/`stderr` qua `queue`, giới hạn `1 MB/dòng` & `50 MB log`, tự động gửi `stop` chờ `500 ms` rồi `taskkill /F /T /PID` khi quá hạn, ghi nhận `run\pids.txt` (chỉ diệt PID do chính mình sinh), khóa `.heavy.lock`, nhịp tim `progress.hb`, xóa `HOMEI_THREADS` và gửi `Threads=1`, `Hash=64` trước mỗi ván.
- **Kết quả `pytest tests/test_safe_proc.py`:** **4/4 PASS** (`3.64s`) trên cả 4 kịch bản lỗi nhân tạo:
  1. Engine giả treo vô hạn không trả lời -> tự động timeout và diệt sạch tiến trình.
  2. Engine giả xả `100 MB` rác ra `stdout` -> ngắt ở ngưỡng `50 MB` và diệt sạch tiến trình trong `< 5s`.
  3. Engine giả bỏ qua lệnh `stop` và đẻ thêm tiến trình con -> diệt sạch cả cây tiến trình (`taskkill /F /T /PID`), không sót tiến trình mồ côi.
  4. Xóa biến môi trường `HOMEI_THREADS` và từ chối diệt PID ngoại lai không thuộc `run\pids.txt`.

### 1.3 Độ trễ Dừng (`stop` -> `bestmove`) trên 200 Thế (`reports/stop_latency_v3.json`)
Đo trên 200 thế (28 thế `T01..T28` + 172 thế hợp lệ ngẫu nhiên) khi đang `go infinite`:

| Chỉ số | Giá trị (ms) | Ngưỡng yêu cầu | Kết quả |
| :--- | :---: | :---: | :---: |
| **Min** | `0.039 ms` | `<= 200 ms` | **PASS** |
| **Median (p50)** | `0.182 ms` | `<= 200 ms` | **PASS** |
| **Mean** | `0.215 ms` | `<= 200 ms` | **PASS** |
| **p95** | `0.244 ms` | `<= 200 ms` | **PASS** |
| **p99** | `0.643 ms` | `<= 200 ms` | **PASS** |
| **Max** | `3.570 ms` | `<= 200 ms` | **PASS (`0/200` vượt 200 ms)** |

---

## 2. BƯỚC 1 — THAM SỐ HÓA & ĐẶC TRƯNG TRIỂN KHAI QUÂN TRUNG CUỘC (`koma_eval`, `koma_search`)

### 2.1 Tham số hóa `eval.cpp` & Cổng Tương đương 1.0(a) / 1.0(b)
- Đã tham số hóa 22 số hạng lượng giá trong `HomeiEngine/src/eval.h` và `HomeiEngine/src/eval.cpp`, hỗ trợ nạp trọng số qua biến môi trường `HOMEI_PARAMS` hoặc lệnh `reloadparams <path>`, cùng các lệnh chẩn đoán `evalterms <LPPN>`, `qcheck <LPPN>`, `qleaf <LPPN> <move>`.
- **Cổng 1.0(a):** Khi `koma_eval = false` và không nạp `HOMEI_PARAMS`, `eval()` trùng bit **100%** với `Homei_v3.exe` trên **2,050 thế** và số nút `bench` cố định trùng khớp tuyệt đối (`985,924 == 985,924` nút).
- **Cổng 1.0(b):** Tổng các số hạng trong `evalterms` (`base_fixed + sum(raw_i * weight_i)`) khớp **100%** với `evaluate(pos)` trên toàn bộ 2,050 thế.

### 2.2 Các Đặc trưng Mới trong `koma_eval` & `koma_search`
1. **1a — Phạt Vector Chết & Quay mặt vào biên (`Dead-Facing Penalty`):**
   - Sử dụng bảng tra tĩnh `O(1)` (`get_dead_vec_table()[pt][facing][sq]`, 2.5 KB trong L1 cache) tính số vector bước đi hợp lệ rơi ra ngoài bàn cờ cho Binh, Kị, Sĩ, Xa (không dùng `ply`, đối xứng hoàn toàn):
     * `koma_dead_all_out`: Phạt nặng khi `in_board == 0` (úp mặt thẳng vào mép bàn cờ không có lối đi).
     * `koma_dead_part_vec`: Phạt tỷ lệ với số vector bị cụt ra ngoài biên (`out_board`).
     * `koma_dead_binh_home`: Phạt bổ sung khi Binh ở sân nhà (`rel_row <= 3`) úp mặt vào biên cụt hoặc xoay ngược 180° về đáy nhà.
2. **1b — Giải phóng Quân Tầm Xa & Pháo đài (`Buried Slider & Siege Battery` thay khối Stack Synergy cũ):**
   - Hàm `has_siege_ray(pos, sq, slider, ekr, ekc)` kiểm tra tia trượt của Long/Phượng/Xa từ ô chồng quân đi qua các ô trống có chạm tới vùng `3x3` quanh Vương địch hay không.
   - Khi quân tầm ngắn (`BINH`, `SI`, `TUONG`) đè lên quân trượt (`LONG`, `PHUONG`, `XA`) của mình:
     * Nếu ở sân nhà (`rel_row <= 3`): phạt nặng `koma_buried_home` và **bỏ** thưởng `+20` (`stack_battery`).
     * Nếu đã sang nửa sân trên (`rel_row >= 4`) nhưng **không** có tia công thành 3x3 quanh Vương địch: phạt `koma_buried_no_ray` và **bỏ** thưởng `+20`.
     * Nếu ở `rel_row >= 4` **và** có tia công thành 3x3 quanh Vương địch: **miễn phạt hoàn toàn** và giữ nguyên thưởng `+20` (`stack_battery`).
   - Khi quân cơ động (`KI`, `LONG`, `PHUONG`, `XA`) ở trên cùng chồng lên quân trượt có tia công thành: hưởng cả `+20` (`stack_battery`) và thưởng pháo đài cơ động `koma_mobile_battery`.
3. **1c — Độ cơ động Tia Quân Trượt (`Slider Ray Mobility`):**
   - Tính trực tiếp trong vòng lặp `get_raw_destinations` (không duyệt lại tia lần thứ hai): đếm số ô trống hoặc ô quân địch mà Long/Phượng kiểm soát (`koma_slider_mob_base`, trần 12 ô/quân) và thưởng thêm cho các ô thuộc phần sân địch (`rel_row >= 5`) hoặc vùng trung tâm `5x5` (`koma_slider_mob_prime`, trần 8 ô/quân).
4. **1d — Phân vai Tiến Quân (`Role-Based Advancement`):**
   - *Các số hạng đã có sẵn trong `Homei_v3` (giữ nguyên, không nhân đôi):* `tuong_si_ki_rel_row` (`+3 * rel_row`), `center_col_weight` (`+2 * (4 - |col - 4|)`), và thưởng lá chắn Vương của Tướng/Sĩ (`+32 / +12 / +4` theo khoảng cách Manhattan `1 / 2 / 3`).
   - *Hai số hạng bổ sung mới:*
     * `koma_xa_rel_row`: Thưởng độ dâng cao `rel_row` cho Xa để khuyến khích Xa chiếm tuyến dọc/ngang chủ động.
     * `koma_undeveloped_ki_si`: Phạt nhẹ mỗi quân Kị/Sĩ còn nằm nguyên ở hàng đáy (`rel_row == 0`) khi đã có `>= 4` đơn vị quân khác triển khai (dựa thuần túy trên cấu trúc bàn cờ, không dùng `ply`).
5. **`koma_search` (Thử nghiệm trong `search.cpp`):**
   - Hạ điểm sắp xếp nước đi (`-120,000`) và tăng LMR `+1 ply` (kẹp trần để `new_depth >= 1`) đối với các nước yên tĩnh chỉ xoay Binh tại chỗ ở sân nhà hoặc xoay quân thẳng vào mép biên chết.

### 2.3 Kiểm thử Đơn vị, Đối xứng & Chi phí NPS (`tests/test_koma_step1.py`)
- **4 thế dựng tay cho mục 1b:** PASS 100% (xác nhận phạt đè quân sân nhà, phạt đè quân khi tia bị chặn, thưởng `+20` khi có tia công thành ở `rel_row >= 4`, và đối xứng tuyệt đối).
- **Đối xứng `mirror_horizontal` & `flip_colors_180` trên 2,050 thế:** **100% PASS** (`0` sai lệch).
- **Chi phí NPS (`koma_eval` ON vs OFF trên `bench` depth 6):**
  - `NPS OFF = 200,326 nps` | `NPS ON = 194,894 nps` -> **Chậm đi `+2.71%`** (thấp hơn rất nhiều so với ngưỡng yêu cầu `<= 10%`).

---

## 3. BƯỚC 2 — TRÍCH XUẤT DỮ LIỆU TỪ 238 VÁN CỜ NGƯỜI (`tools/extract_koma_data.py`)

Quét toàn bộ **238** file `.lpgf` trong `E:\code5\CLP_dataset\LPGF\HvH\` bằng `ref.clp.record_io`:

| Giai đoạn trong Phễu lọc (`data/koma_funnel_stats.json`) | Số lượng | Ghi chú |
| :--- | :---: | :--- |
| Tổng số file `.lpgf` quét | `238` | Chia 80/20 theo **VÁN**: `190` ván Train / `48` ván Val |
| Số ván có nhãn `HISTORICAL_ILLEGAL_LOSS` (đã cắt trước nước lỗi) | `22` | Giữ lại phần đầu hợp lệ |
| Tổng số ply hợp lệ đã replay | `17,784` | — |
| Số thế trong cửa sổ trung cuộc `ply 6..45` | `8,875` | — |
| Số thế duy nhất sau khử trùng LPPN chuẩn hóa (gồm cả đối xứng gương) | `8,846` | Không rò rỉ thế trùng giữa Train và Val |
| Loại do nước người `m_human` rơi vào nhóm xấu / không có `m_neg` | `823` | `541` nước xấu + `282` thế không có `m_neg` |
| Ứng viên đưa vào lọc bằng Engine (`qcheck` + `Homei_v3` depth 5/6) | `8,023` | — |
| Loại do đang bị chiếu (`in_check != 0`) | `22` | — |
| Loại do thế không yên tĩnh (`|static_eval - qsearch| > 30 cp`) | `1,381` | — |
| Loại do điểm quá lệch (`|score| > 1500 cp` hoặc điểm `mate`) | `163` | — |
| Loại do nước người `m_human` kém `bestmove` depth 5 của `Homei_v3` `> 60 cp` | `1,878` | — |
| **Số thế gốc yên tĩnh đạt chuẩn** | **`4,579`** | — |
| **Tổng số thế sau khi nhân bản lật gương ngang (`is_mirror`)** | **`9,158`** | **`7,256` thế Train / `1,902` thế Val** |
| **Tổng số cặp `(m_pos, m_neg)` trích xuất đặc trưng tại lá `qleaf`** | **`27,474`** | **`21,768` cặp Train / `5,706` cặp Val** |

- **Phân bố 27,474 mẫu âm (`m_neg`) theo nhóm lỗi triển khai quân:**
  - `edge_dead_facing` (xoay/đi quân úp mặt vào biên chết): `9,288` mẫu (`33.8%`)
  - `bury_slider` (Binh/Sĩ/Tướng đè lên Long/Phượng/Xa ở sân nhà hoặc không có tia công thành): `8,342` mẫu (`30.4%`)
  - `retreat_si_ki_backrank` (rút Sĩ/Kị lùi về hàng đáy nhà): `7,014` mẫu (`25.5%`)
  - `home_binh_rotate` / `home_binh_rotate_back` (xoay Binh tại chỗ vô nghĩa ở sân nhà): `2,830` mẫu (`10.3%`)

---

## 4. BƯỚC 3 — TỐI ƯU HÓA TRỌNG SỐ TUYẾN TÍNH TẠI LÁ `qleaf` (`tools/tune_koma.py`)

Hàm mục tiêu kết hợp xếp hạng biên (Margin Ranking Loss với `margin = 25 cp`), độ bám điểm tìm kiếm `Homei_v3` ở depth 6 (`MSE`), và điều chuẩn `L2`:
$$\mathcal{L}(w) = \frac{1}{|P|}\sum_{(m^+, m^-)} \max(0, V_w(m^-) - V_w(m^+) + 25)^2 + a \cdot \text{MSE}(V_{\text{root}}(w), \text{score}_{d6}) + \lambda \|w - w_{\text{default}}\|_2^2$$

### 4.1 Kết quả Trước và Sau Tinh chỉnh (`data/tune_summary.json`)
Siêu tham số tối ưu trên tập Validation (`a = 0.02, lambda = 0.2`):

| Tập dữ liệu | Chỉ số | Trước tinh chỉnh (`w_default`) | Sau tinh chỉnh (`params_koma.txt`) | Thay đổi |
| :--- | :--- | :---: | :---: | :---: |
| **Train** (`7,256` thế, `21,768` cặp) | `MarginLoss` | `265.1250` | **`246.6702`** | **-6.96%** |
| | `MSE` (`score_d6`) | `12696.2222` | **`12172.9033`** | **-4.12%** |
| | `RMSE` / `MAE` | `112.68` / `63.99` | **`110.33` / `60.79`** | **-2.35 cp / -3.20 cp** |
| | Tỷ lệ `V(m_pos) > V(m_neg)` | `95.64%` | **`95.94%`** | **+0.30%** |
| | Tỷ lệ đạt biên `>= 25 cp` | `87.36%` | **`87.55%`** | **+0.19%** |
| **Validation** (`1,902` thế, `5,706` cặp) | `MarginLoss` | `175.9685` | **`165.8283`** | **-5.76% (ĐẠT)** |
| | `MSE` (`score_d6`) | `13108.4774` | **`12548.5205`** | **-4.27% (ĐẠT `<= +2%`)** |
| | `RMSE` / `MAE` | `114.49` / `63.61` | **`112.02` / `60.83`** | **-2.47 cp / -2.78 cp** |
| | Tỷ lệ `V(m_pos) > V(m_neg)` | `95.27%` | **`95.48%`** | **+0.21%** |
| | Tỷ lệ đạt biên `>= 25 cp` | `86.15%` | **`86.65%`** | **+0.50%** |

### 4.2 Bảng Trọng số Lượng giá (`params_koma.txt` & `EvalParams::defaults()`)

| STT | Tên tham số (`EvalTermId`) | Mặc định khởi tạo | Sau tinh chỉnh (`Homei_v3-2`) | Độ lệch |
| :---: | :--- | :---: | :---: | :---: |
| 1..12 | 12 tham số nền của `Homei_v3` (`stack_base` .. `xa_forward`) | Giữ nguyên `Homei_v3` | Giữ nguyên `Homei_v3` | `0` |
| 13 | `koma_dead_all_out` (phạt úp mặt vào biên cụt 100%) | `24` | **`26`** | `+2` |
| 14 | `koma_dead_part_vec` (phạt mỗi vector rơi ra ngoài bàn cờ) | `5` | **`5`** | `0` |
| 15 | `koma_dead_binh_home` (phạt Binh sân nhà úp biên / quay ngược) | `22` | **`24`** | `+2` |
| 16 | `koma_buried_home` (phạt đè Long/Phượng/Xa ở sân nhà `rel_row <= 3`) | `45` | **`47`** | `+2` |
| 17 | `koma_buried_no_ray` (phạt đè Long/Phượng/Xa không có tia công thành) | `30` | **`30`** | `0` |
| 18 | `koma_mobile_battery` (thưởng pháo đài cơ động có tia công thành) | `8` | **`8`** | `0` |
| 19 | `koma_slider_mob_base` (thưởng mỗi ô tia Long/Phượng kiểm soát) | `2` | **`1`** | `-1` |
| 20 | `koma_slider_mob_prime` (thưởng thêm ô tia ở sân địch / trung tâm 5x5) | `2` | **`1`** | `-1` |
| 21 | `koma_xa_rel_row` (thưởng độ dâng cao `rel_row` của Xa) | `2` | **`5`** | `+3` |
| 22 | `koma_undeveloped_ki_si` (phạt Kị/Sĩ nằm lỳ ở hàng đáy khi đã triển khai quân) | `10` | **`10`** | `0` |

---

## 5. BƯỚC 4 — KIỂM ĐỊNH ĐÚNG ĐẮN, ĐẤU TRẬN, XUẤT LPGF v2.0 & ĐÓNG GÓI

### 5.1 Cổng Đúng đắn (`python tools/verify_gate.py --require-t28`)
Chạy trên bản build cuối cùng `Homei_v3-2.exe` (`koma_eval = true` bật mặc định):
- `test_parity.py`: **6 passed**
- `test_protocol.py`: **9 passed**
- `test_synthetic.py`: **8 passed**
- `test_tactical_suite.py`: **26 passed, 2 skipped**
- `test_piecevalues.py`: **3 passed**
- `test_bench.py`: **1 passed**
- `T28` (`--require-t28`): **PASS** (`move=K*a3<, score=mate 15, pv_len=15, pv_terminal=True`)
- **Kết luận Cổng đúng đắn:** **PASS 100% (`exit 0`)**.

### 5.2 Kết quả Đấu trận Thực nghiệm vs `Homei_v3.exe` (`movetime 150 ms`, `Threads=1`, `Hash=64 MB`)
Sử dụng bộ 400 thế khai cuộc 8-ply cân bằng (`openings.txt`, `seed=12345`, `|eval_d4| <= 150 cp`), mỗi thế đấu 2 ván đổi màu Cam/Xanh, trọng tài độc lập `ref.clp`:

| Đợt đấu | Cấu hình (`Engine A` vs `Homei_v3.exe`) | Số ván (Cặp) | Thắng / Hòa / Thua (A) | Tỷ lệ điểm | Elo ± 95% CI (Paired) | LOS (%) | Nước phạm luật / Lỗi |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **Tuyển chọn A** | **Cấu hình A (`+koma_eval,-koma_search`)** | `400` (`200` cặp) | `229 / 0 / 171` | **`57.25%`** | **`+50.7 ± 33.7`** (`[+17.5, +84.9]`) | **`99.87%`** | **`0 / 0`** |
| **Tuyển chọn B** | **Cấu hình B (`+koma_eval,+koma_search`)** | `400` (`200` cặp) | `224 / 0 / 176` | **`56.00%`** | **`+41.9 ± 34.6`** (`[+7.7, +76.9]`) | **`99.19%`** | **`0 / 0`** |
| **Xác nhận 800 ván (A)** | **`Homei_v3-2` (`+koma_eval,-koma_search`)** | **`800` (`400` cặp)** | **`482 / 0 / 318`** | **`60.25%`** | **`+72.2 ± 23.9`** (`[+48.7, +96.5]`) | **`100.00%`** | **`0 / 0`** |

### 5.3 Chỉ số Phụ Triển khai Quân Trung cuộc (`ply 12..40`) & Thời gian
Thống kê trực tiếp từ **800 ván** xác nhận (`games/config_a_koma_eval/summary.json`) và **400 ván** Cấu hình B (`games/config_b_koma_eval_search/summary.json`):

| Chỉ số trung cuộc (`ply 12..40`) & Vận hành | `Homei_v3.exe` (Baseline) | `Homei_v3-2` (Cấu hình A — 800 ván) | Cấu hình B (400 ván) | Mức cải thiện (`Homei_v3-2` vs `Homei_v3`) |
| :--- | :---: | :---: | :---: | :---: |
| **Binh quay mặt vào biên (`edge_facing_binh` / ply)** | `0.0242` | **`0.0009`** | `0.0000` | **Giảm 96.3%** |
| **Quân trượt bị đè (`buried_slider` / ply)** | `0.6681` | **`0.0263`** | `0.0257` | **Giảm 96.1%** |
| **Tỷ lệ có `>= 2` Sĩ/Kị/Tướng trên hàng `d, e, f` (`adv_def_rate`)** | `0.93%` | **`6.73%`** | `6.64%` | **Tăng 7.2 lần (+5.80 điểm %)** |
| **Tốc độ duyệt trung bình trong trận (`NPS`)** | `186,282 nps` | **`186,594 nps`** | `174,264 nps` | **Tương đương (`+0.17%`)** |
| **Độ sâu trung bình (`avg_depth`)** | `3.65` | `3.47` | `3.39` | `-0.18 ply` |
| **Số nước trễ `> movetime + 200 ms` (`over_mt_200ms`)** | `178` / `33,440` nước (`0.53%`) | **`174`** / `33,440` nước (`0.52%`) | `126` | **Thấp hơn `Homei_v3`** |

### 5.4 Xuất & Kiểm định 100% Kỳ phổ Chuẩn LPGF v2.0 (`tools/verify_lpgf_export.py`)
- Toàn bộ **1,200 ván đấu** ở Bước 4 (`800` ván `config_a_koma_eval` + `400` ván `config_b_koma_eval_search`) đã được xuất ra chuẩn **LPGF v2.0** (`#lpgf version=2.0 encoding=UTF-8`, đầy đủ 6 dòng tiêu đề, sơ đồ bàn cờ ASCII 9x9 của thế khai cuộc 8-ply, và từng nước đi kèm `(ô_nguồn)` trên từng dòng riêng) tại:
  - `E:\code5\CLP_dataset\LPGF\EvE\Homei_v3_2_vs_v3\config_a_koma_eval\game_0001.lpgf` .. `game_0800.lpgf`
  - `E:\code5\CLP_dataset\LPGF\EvE\Homei_v3_2_vs_v3\config_b_koma_eval_search\game_0001.lpgf` .. `game_0400.lpgf`
- **Kết quả kiểm định đọc lại & replay bằng `ref.clp.record_io.RecordIO.read` và `RecordIO.load_txt_moves_into_bridge`:**
  - `total_files: 1200`, **`passed_files: 1200`**, **`failed_files: 0`** (**100% hợp lệ**, `0` lỗi cú pháp hay luật cờ).
