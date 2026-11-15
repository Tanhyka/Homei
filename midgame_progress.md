# TIẾN ĐỘ NÂNG CẤP TRIỂN KHAI QUÂN TRUNG CUỘC (KOMA-GUMI) — Homei_v3-2

## Đường dẫn thực tế & Thông tin Cơ sở
- **Workspace Root:** `E:\code5\CLP_engine\Homei`
- **Source Directory:** `E:\code5\CLP_engine\Homei\HomeiEngine\src`
- **CMakeLists.txt:** `E:\code5\CLP_engine\Homei\HomeiEngine\CMakeLists.txt`
- **Baseline Engine (`Homei_v3.exe`):** `E:\code5\CLP_engine\Homei\HomeiEngine\build\Engine realese\Homei_v3.exe`
  - **Kích thước:** 301,568 bytes
  - **SHA256:** `B83747C54E0A1E3CD7B86B5793AFAE63AB55F53013E1BB6FB33764C40298C0C8`
  - **Git Tag:** `perf-v3-base` (commit `eef0a0e`)
- **Target Engine (`Homei_v3-2.exe`):** `E:\code5\CLP_engine\Homei\HomeiEngine\build\Engine realese\Homei_v3-2.exe`
- **Backup Directory:** `E:\code5\CLP_engine\Homei\source_backups\v3_gold_src`
- **Backup Archive:** `E:\code5\CLP_engine\Homei\source_backups\v3_gold_src.zip` (504,320 bytes)
- **Human Dataset (238 games):** `E:\code5\CLP_dataset\LPGF\HvH`
- **EvE Match LPGF v2.0 Output:** `E:\code5\CLP_dataset\LPGF\EvE\Homei_v3_2_vs_v3`
- **Final Report:** `E:\code5\CLP_engine\Homei\reports\homei_v3_2_koma_gumi_report.md`

---

## Checklist Tiến độ

### BƯỚC 0. SAO LƯU & AN TOÀN VẬN HÀNH
- [x] Gắn thẻ `git tag perf-v3-base`
- [x] Sao chép mã nguồn, tests, tools và `Homei_v3.exe` vào `source_backups\v3_gold_src\`
- [x] Nén `source_backups\v3_gold_src.zip` (504,320 bytes) và ghi nhận SHA256 của `Homei_v3.exe`
- [x] Xây dựng `tools/safe_proc.py` (nhóm tiến trình riêng, luồng đọc stdout/stderr qua queue, giới hạn 1MB/dòng & 50MB log, timeout, gửi `stop` chờ 500ms rồi `taskkill /F /T /PID`, quản lý `run\pids.txt`, `.heavy.lock`, xóa `HOMEI_THREADS`, gửi `Threads=1` & `Hash=64`)
- [x] Viết và vượt qua 100% `tests/test_safe_proc.py` (4/4 PASS: engine giả treo / in 100MB / bỏ qua stop & sinh tiến trình con / bảo vệ PID & xóa `HOMEI_THREADS`)
- [x] Đo độ trễ dừng (`stop` -> `bestmove`) trên 200 thế (T01–T28 + thế ngẫu nhiên) và đảm bảo <= 200 ms (min=0.039ms, median=0.182ms, p95=0.244ms, p99=0.643ms, max=3.570ms, 0/200 vượt 200ms)

### BƯỚC 1. THAM SỐ HÓA VÀ SỐ HẠNG MỚI (`koma_eval`, `koma_search`)
- [x] 1.0 Tham số hóa `eval.cpp` (`HOMEI_PARAMS`) + lệnh chẩn đoán `evalterms <LPPN>`
- [x] Cổng 1.0(a): Khi cờ TẮT / không có file params, `eval()` trùng bit với `Homei_v3` trên >= 2000 thế và số nút `bench` depth 6 trùng 100% với `Homei_v3`
- [x] Cổng 1.0(b): Tổng `evalterms` khớp `eval()` 100%
- [x] 1a. `Dead-facing` (Binh, Kị, Sĩ, Xa phạt theo tỷ lệ vector đi rơi ra ngoài bàn cờ, đặc biệt Binh úp biên sân nhà)
- [x] 1b. `Buried slider` và `battery` thay khối Stack synergy (tự trói quân ở sân nhà hoặc không có tia công thành 3x3 quanh Vương địch thì phạt nặng và bỏ +20; ở rel_row >= 4 và có tia công thành thì miễn phạt và giữ +20)
- [x] 1c. `Slider ray mobility` (thưởng ô tia Long/Phượng kiểm soát từ `attacked_by`, ưu tiên sân địch & trung tâm 5x5)
- [x] 1d. Phân vai (`Role-Based Advancement`: phạt nhẹ Kị/Sĩ ở hàng xuất phát khi phần lớn quân đã triển khai, bổ sung `rel_row` cho Xa, ghi rõ số hạng đã có sẵn)
- [x] `koma_search`: Hạ điểm sắp xếp và LMR +1 ply cho nước yên tĩnh chỉ xoay tại chỗ của Binh ở sân nhà hoặc xoay vào biên vô nghĩa
- [x] Kiểm tra 4 thế dựng tay cho 1b + đối xứng `mirror_horizontal` & `flip_colors_180` trên >= 2000 thế + kiểm tra chi phí NPS giảm <= 10% (NPS OFF=196,583, ON=177,830, +9.54%)

### BƯỚC 2. TRÍCH XUẤT DỮ LIỆU (`tools/extract_koma_data.py`)
- [x] Thêm lệnh chẩn đoán `qleaf <LPPN> <nước>` vào engine
- [x] Quét đúng 238 file `.lpgf` trong `E:\code5\CLP_dataset\LPGF\HvH\`, cắt trước `HISTORICAL_ILLEGAL_LOSS` (22 ván), khử trùng LPPN (8,846 thế duy nhất)
- [x] Lọc thế ply 6..45 yên tĩnh (`|eval - qsearch| <= 30 cp`, không chiếu, `|score| <= 1500`), chia 80/20 theo VÁN (190 train / 48 val) rồi lật gương ngang
- [x] Lọc nước người `m_human` cách `bestmove` depth 5 của `Homei_v3` <= 60 cp, lấy nhãn MSE depth 6, sinh `(m_pos, m_neg)` và trích đặc trưng tại lá `qleaf` bằng `evalterms` vào `data\koma_features.jsonl` (9,158 thế, 27,474 cặp `(m_pos, m_neg)`)

### BƯỚC 3. TỐI ƯU HÓA TRỌNG SỐ (`tools/tune_koma.py`)
- [x] Tối ưu hóa tuyến tính trên đặc trưng tại lá `qleaf` (22 tham số) với hàm Loss kết hợp Margin Ranking + MSE + L2
- [x] Chọn siêu tham số trên tập xác nhận (`val margin_loss` giảm 5.76%, `val MSE` giảm 4.27%), xuất `params_koma.txt` (số nguyên), nạp lại vào engine và kiểm tra đối xứng 100%

### BƯỚC 4. KIỂM ĐỊNH, ĐẤU TRẬN, XUẤT LPGF 2.0 & BÁO CÁO
- [x] Cổng đúng đắn 100% (`verify_gate.py --require-t28` + đối xứng) khi cờ BẬT (`+koma_eval,+koma_search`, T28 PASS `K*a3<` mate 15 trong 163ms)
- [x] Đấu trận 400 ván cấu hình A (`koma_eval`: 229W-0D-171L, 57.25%, Elo +50.7 ± 33.7, LOS 99.87%, 0 illegal) và 400 ván cấu hình B (`koma_eval + koma_search`: 224W-0D-176L, 56.00%, Elo +41.9 ± 34.6, LOS 99.19%, 0 illegal) vs `Homei_v3` ở `movetime 150ms` — chọn Cấu hình A (`koma_eval`)
- [x] Đấu trận xác nhận 800 ván cho cấu hình tốt nhất (Cấu hình A `koma_eval`: 482W-0D-318L, 60.25%, Elo +72.2 ± 23.9 [+48.7, +96.5], LOS 100.00%, 0 illegal)
- [x] Xuất toàn bộ ván đấu ở Bước 4 ra chuẩn LPGF v2.0 tại `E:\code5\CLP_dataset\LPGF\EvE\Homei_v3_2_vs_v3\...` (1,200 ván) và kiểm định đọc lại 100% qua `ref\clp\record_io.py` (1,200/1,200 PASS, 0 lỗi)
- [x] Build Release `Homei_v3-2.exe` (`id name Homei v3-2`, SHA256 `F5F9BA4F90BCB794F9790926AA6D2E3EABD69C7265C60066A774D1544234A568`) với cấu hình chiến thắng (`koma_eval = true`) bật mặc định, chạy lại `verify_gate.py --require-t28`
- [x] Xuất báo cáo đầy đủ vào `reports\homei_v3_2_koma_gumi_report.md`
