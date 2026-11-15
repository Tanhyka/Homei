# BÁO CÁO KIỂM TOÁN TÌM KIẾM, ĐO HIỆU NĂNG & CHẨN ĐOÁN LƯỢNG GIÁ ENGINE HŌMEI

- **Dự án:** HomeiEngine (Cờ Long Phượng Engine)
- **Phiên bản chuẩn:** CLP-EP v1
- **Commit Hash:** `dc185953f331cc52606d4fbee1f8b2a6a302002a`
- **Ngày thực hiện:** 11/11/2026
- **Mục tiêu & Nguyên tắc:**
  1. Chỉ đo đạc và chẩn đoán — Tuyệt đối KHÔNG thay đổi thuật toán tìm kiếm, sinh nước hay giá trị lượng giá.
  2. Không xâm thực giao thức CLP-EP v1 — Không đổi handshake, không tự phát thông điệp rác vào stdio.
  3. Cổng an toàn Regression Gate — Toàn bộ Test 0, Test P, T01–T27, S1–S8 đạt 100% PASS như ban đầu.

---

## PHẦN 1: BẢNG KIỂM TOÁN MÃ NGUỒN TÌM KIẾM (SEARCH AUDIT)

Rà soát toàn diện 13 hạng mục kỹ thuật trong mã nguồn `E:\code5\Homei\HomeiEngine\src`:

| STT | Tính năng | Trạng thái | File : Dòng | Ghi chú kỹ thuật |
| :--- | :--- | :---: | :--- | :--- |
| **1** | **Bảng băm Transposition Table (TT)** | **Có** | `src/tt.h:20-60`<br>`src/tt.cpp:1-120`<br>`src/search.cpp:466-515` | Kích thước mặc định **64 MB** (cấu hình linh hoạt qua CLP-EP Option `Hash`). Thiết kế 2-tier bucket (tier 0: replacement policy dựa trên depth và generation aging; tier 1: always-replace). Lưu best move, depth, score (chuyển đổi global $\leftrightarrow$ ply), bound flag (`TT_EXACT`, `TT_LOWER`, `TT_UPPER`) và generation. Nước đi từ TT (`tt_move`) được ưu tiên cao nhất trong Move Ordering (`score = 2,000,000`). Tra bảng và cắt tỉa (cutoff) diễn ra trong `pvs`; **vô hiệu hóa cutoff khi `rep_count >= 2`** để tránh chu kỳ lặp giả. |
| **2** | **Iterative Deepening** | **Có** | `src/search.cpp:784-952` | Vòng lặp tăng dần độ sâu từ `d = 1` đến `max_d` ở nút gốc (`Searcher::search`). Sau mỗi độ sâu hoàn thành, các nước đi tốt nhất trong `depth_lines` (PV) được hoán đổi đưa lên đầu danh sách `root_legal` với điểm ưu tiên `10,000,000`, tạo trật tự Move Ordering tối ưu cho độ sâu kế tiếp. Hỗ trợ kiểm tra giới hạn thời gian / số nút giữa các độ sâu và bên trong cây. |
| **3** | **PVS / Aspiration Window** | **Một phần** | `src/search.cpp:443-620`<br>`src/search.cpp:823-855` | • **PVS (Principal Variation Search):** Đã triển khai đầy đủ. Tại mỗi nút, nước đi đầu tiên (`i == 0`) được tìm kiếm với cửa sổ đầy đủ $[ \alpha, \beta ]$. Các nước đi tiếp theo (`i > 0`) được tìm kiếm với cửa sổ rỗng / null-window $[ -\alpha - 1, -\alpha ]$ (với độ sâu có thể áp dụng LMR). Nếu điểm trả về lớn hơn $\alpha$ và nhỏ hơn $\beta$ (fail-high null-window), engine thực hiện re-search với cửa sổ đầy đủ $[ \alpha, \beta ]$ ở độ sâu đầy đủ.<br>• **Aspiration Window:** Chưa triển khai (Không). Tại nút gốc, mỗi vòng lặp Iterative Deepening luôn khởi tạo với cửa sổ vô cực $[ -\infty, +\infty ]$ mà chưa dùng cửa sổ thu hẹp quanh điểm dự đoán của độ sâu trước (ví dụ $[ \text{prev} - \delta, \text{prev} + \delta ]$). |
| **4** | **Quiescence Search** | **Có** | `src/search.cpp:301-441` | Hàm `Searcher::qsearch` được kích hoạt khi độ sâu $d \le 0$.<br>• Khi đang bị chiếu (`in_check = true`): Sinh toàn bộ nước đi thoát chiếu hợp lệ (evasions).<br>• Khi không bị chiếu: Dùng eval tĩnh làm cận dưới (`stand_pat`). Nếu `stand_pat >= beta`, thực hiện beta-cutoff ngay lập tức.<br>• Lọc nước đi ăn quân bằng thuật toán ước tính lợi thế `stack_see_estimate`.<br>• **Quy ước nước thả quân (drop moves):** Trong thế cờ yên tĩnh (không bị chiếu), qsearch **chỉ** xét các nước ăn quân (`is_capture_move`), **KHÔNG** sinh hay thử các nước thả quân yên tĩnh từ kho dự trữ/tù binh (`quiet drops`). Khi bị chiếu, toàn bộ nước thoát chiếu (bao gồm cả thả quân chặn chiếu hợp lệ) đều được sinh và kiểm tra. |
| **5** | **Sắp xếp nước đi (Move Ordering)** | **Một phần** | `src/search.cpp:106-258`<br>(`score_moves`) | • Thứ tự ưu tiên có trọng số chi tiết:<br>1. `tt_move`: 2,000,000 điểm (ưu tiên tuyệt đối).<br>2. Nước ăn quân: Tính điểm theo MVV-LVA cải tiến kết hợp `stack_see_estimate` (ở ply $\le 2$) và thưởng thế công kích / xoay hướng sau khi ăn quân.<br>3. Nước thả quân: Thưởng nếu nước thả tạo thế đe dọa (chiếu hoặc tấn công quân địch) hoặc tạo khiên chắn bảo vệ.<br>4. Killer moves: 2 slot killer move cho mỗi độ sâu ply (`killer_moves_[ply][0]` = 800,000 điểm, `killer_moves_[ply][1]` = 750,000 điểm).<br>5. History heuristic: Điểm tích lũy trong mảng `history_[side][from_sq][to_sq]` (tối đa 400,000).<br>6. Đòn tấn công Vương và đe dọa chiến thuật: Thưởng thêm 50,000 - 150,000 điểm nếu nhắm vào ô nguy hiểm quanh Vương hoặc quân bị treo.<br>• **Countermove heuristic:** Chưa triển khai (Không). Chưa theo dõi bảng phản đòn theo cặp nước đi trước - nước đi sau `countermove[prev_piece][prev_to]`. |
| **6** | **Late Move Reductions (LMR)** | **Có** | `src/search.cpp:520-565`<br>`src/search.cpp:825-850` | Áp dụng khi độ sâu $d \ge 2$, chỉ số nước đi $i \ge 4$, không đang bị chiếu, và nước đi không phải là nước ăn quân, không phải nước chiếu, không phải killer move.<br>• Mức độ giảm độ sâu $R$: $R = 1$ khi $i < 8$; $R = 2$ khi $i \ge 8$ và $d \ge 5$; $R = 3$ khi $i \ge 16$ và $d \ge 7$ (tối đa $d - 1$).<br>• Nếu tìm kiếm với độ sâu giảm $d - 1 - R$ thu được điểm $> \alpha$, engine thực hiện re-search lại ở độ sâu đầy đủ $d - 1$. |
| **7** | **Late Move Pruning (LMP)** | **Không** | `src/search.cpp` | Chưa triển khai. Engine duyệt toàn bộ danh sách nước đi hợp lệ đã sắp xếp mà chưa cắt tỉa dừng vòng lặp sớm sau khi thử một số lượng nước yên tĩnh nhất định ở độ sâu thấp (ví dụ: $N > 3 + 3 \times d^2$). |
| **8** | **Futility Pruning / Razoring / Reverse Futility** | **Không** | `src/search.cpp` | Chưa triển khai. Chưa so sánh hiệu số giữa eval tĩnh và $\alpha$ hay $\beta$ (như `static_eval + margin < alpha` hoặc `static_eval - margin >= beta`) để bỏ qua tìm kiếm qsearch hay cắt tỉa biên trước khi sinh nước đi. |
| **9** | **Null-Move Pruning (NMP)** | **Không** | `src/search.cpp` | Chưa triển khai. Do luật cờ Long Phượng có tính chất phức tạp về ô chồng đội, hướng nhìn xoay quân và hiện tượng zugzwang đặc thù, NMP chưa được kích hoạt trong phiên bản hiện tại. |
| **10** | **Check Extension** | **Có** | `src/search.cpp:484-489`<br>`src/search.cpp:817-822` | Khi bên đi đang bị chiếu (`in_check = true`) và ngân sách mở rộng còn hiệu lực (`ext_budget > 0`):<br>• Tại `pvs`: Nếu số lượng nước đi thoát chiếu rất hạn chế ($\le 4$ nước hợp lệ), tự động mở rộng thêm $+1$ ply độ sâu (`depth + 1`) và giảm `ext_budget`.<br>• Tại gốc (`root`): Nếu bị chiếu ở độ sâu $d = 1$, tự động mở rộng lên $d = 2$. |
| **11** | **Mate-Distance Pruning** | **Không** | `src/search.cpp` | Chưa triển khai. Chưa cắt tỉa khoảng điểm $[ -\text{MATE} + \text{ply}, \text{MATE} - \text{ply} - 1 ]$ khi cửa sổ $\alpha, \beta$ bị giới hạn bởi độ sâu mate gần nhất. |
| **12** | **Phát hiện lặp trong cây (In-tree Repetition Detection)** | **Có** | `src/search.cpp:77-104`<br>`src/search.cpp:448-456`<br>`src/search.cpp:477` | Triển khai nghiêm ngặt theo luật Cờ Long Phượng (Thiên Nhật Thủ - repetition rule: bên thực hiện nước đi tạo trạng thái lặp lần thứ 3 sẽ bị xử thua ngay lập tức).<br>• Hàm `count_repetitions(hash, ply)` kết hợp lịch sử ván đấu gốc (`base_rep_`) và toàn bộ nhánh tìm kiếm hiện tại (`path_hashes_[1 .. ply-1]`).<br>• Tại `pvs`: Kiểm tra ngay đầu hàm, nếu `rep_count >= 3`, lập tức trả về điểm thua cực lớn `SCORE_MATE - ply` (tương đương bị chiếu bí ở ply này).<br>• Đồng thời, khi `rep_count >= 2`, vô hiệu hóa cutoff từ Transposition Table để tránh bẫy chu kỳ giả trong bảng băm. |
| **13** | **Đa luồng (Multithreading)** | **Không** | `src/search.cpp`<br>`src/clp_ep.cpp:98-102` | Tìm kiếm hoàn toàn đơn luồng (Single-threaded search). Luồng `search_thread_` trong `clp_ep.cpp` chỉ đóng vai trò phân tách bất đồng bộ giữa I/O giao thức CLP-EP và tiến trình tính toán tìm kiếm để có thể lắng nghe lệnh `stop` / `isready` tức thời, không phân chia cây tìm kiếm kiểu Lazy SMP hay YBW. |

---

## PHẦN 2: CỜ BIÊN DỊCH `HOMEI_STATS` & LỆNH `bench`

### 1. Cơ chế cờ tiền xử lý `HOMEI_STATS`
- Trong `CMakeLists.txt`:
  ```cmake
  option(HOMEI_STATS "Enable search profiling statistics" OFF)
  if(HOMEI_STATS)
      add_compile_definitions(HOMEI_STATS)
  endif()
  ```
- **Mặc định:** `OFF`.
- Khi `OFF`: Toàn bộ các biến đếm và mã thống kê được loại bỏ hoàn toàn bằng `#ifdef HOMEI_STATS`, đảm bảo **0% overhead CPU**, giữ nguyên 100% hiệu năng và tính xác định của cây tìm kiếm.
- Khi `ON`: Thu thập các chỉ số profiling trong cấu trúc `SearchStats`:
  - `nodes`: Tổng số nút trong cây chính (`pvs`).
  - `qnodes`: Tổng số nút trong Quiescence Search (`qsearch`).
  - `beta_cutoffs_total`: Tổng số lần xảy ra beta-cutoff.
  - `beta_cutoffs_first_move`: Số lần beta-cutoff xảy ra ngay tại nước đầu tiên (`first-move cutoff %`).
  - `tt_probe`: Số lần tra bảng băm TT.
  - `tt_hit`: Số lần trúng khóa Zobrist trong TT.
  - `tt_cutoff`: Số lần bảng băm TT trực tiếp gây ra cutoff.
  - `lmr_researches`: Số lần LMR fail-high buộc phải tìm kiếm lại đủ độ sâu.
  - `null_move_cutoffs`: Số lần Null-Move Pruning thành công (hiện = 0 vì NMP chưa bật).

### 2. Lệnh `bench`
- **Giao tiếp:** Nhận từ `stdin`:
  - `bench`: Chạy cấu hình chuẩn Depth 6, movetime 1000ms.
  - `bench [depth] [movetime]`: Cho phép tùy biến độ sâu và thời gian.
- **Quy chuẩn cố định:** Cấu hình đo chuẩn: `depth = 6, 7, 8`, `movetime = 1000 ms`, `MultiPV = 1`, `Threads = 1`, TT size reset về `64 MB` (`newgame`) giữa mỗi thế cờ.
- **Bộ 13 thế cờ kiểm thử cố định:**
  1. `startpos`: Thế cờ xuất phát chuẩn.
  2. `S3`: Thế cờ phòng thủ / hóa giải chiếu từ `homei.md`.
  3. `S8`: Thế cờ cản phá chiếu bí từ `homei.md`.
  4. `T01–T10`: 10 thế cờ chiến thuật chọn lọc trích xuất từ `homei.md`.

---

### 3. Bảng đo hiệu năng chuẩn `bench`

#### Bảng A: `HOMEI_STATS=OFF` (Bản chuẩn phát hành — Mặc định)
> *Cột `FirstCutoff%` và `TTHit%` in `n/a` theo đúng đặc tả.*

| Pos | Depth đạt | Nodes | Time (ms) | NPS | EBF | FirstCutoff% | TTHit% | BestMove |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :--- |
| **startpos** | 6 | 22,973 | 43 | 534,255 | 4.14 | n/a | n/a | `L*b5` |
| **S3** | 6 | 368,603 | 521 | 707,491 | 14.09 | n/a | n/a | `Sh5^` |
| **S8** | 6 | 28,435 | 31 | 917,258 | 3.39 | n/a | n/a | `Va6` |
| **T01** | 4 | 342,784 | 988 | 346,947 | 13.77 | n/a | n/a | `[L]c8` |
| **T02** | 4 | 358,144 | 988 | 362,493 | 5.34 | n/a | n/a | `Pc2` |
| **T03** | 4 | 277,248 | 988 | 280,615 | 7.40 | n/a | n/a | `Ke3<` |
| **T04** | 3 | 322,560 | 988 | 326,477 | 7.30 | n/a | n/a | `Kd5v` |
| **T05** | 3 | 188,928 | 988 | 191,222 | 2.38 | n/a | n/a | `Sh3^` |
| **T06** | 4 | 294,656 | 988 | 298,234 | 19.09 | n/a | n/a | `Xf4v` |
| **T07** | 4 | 382,464 | 989 | 386,717 | 13.63 | n/a | n/a | `Xh2v` |
| **T08** | 4 | 291,840 | 988 | 295,384 | 12.42 | n/a | n/a | `P*e1` |
| **T09** | 5 | 450,816 | 988 | 456,291 | 3.09 | n/a | n/a | `L*f3` |
| **T10** | 4 | 312,064 | 988 | 315,854 | 21.90 | n/a | n/a | `Pd1` |
| **TỔNG** | — | **3,641,515** | **10,476** | **347,605** | — | — | — | — |

---

#### Bảng B: `HOMEI_STATS=ON` (Đo đạc chi tiết Profiling — Depth 6)

| Pos | Depth đạt | Nodes | Time (ms) | NPS | EBF | FirstCutoff% | TTHit% | BestMove |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :--- |
| **startpos** | 6 | 22,973 | 42 | 546,976 | 4.14 | 95.4% | 30.9% | `L*b5` |
| **S3** | 6 | 368,603 | 521 | 707,491 | 14.09 | 92.8% | 18.9% | `Sh5^` |
| **S8** | 6 | 28,435 | 31 | 917,258 | 3.39 | 95.6% | 32.9% | `Va6` |
| **T01** | 4 | 347,648 | 988 | 351,870 | 13.77 | 67.0% | 25.3% | `[L]c8` |
| **T02** | 4 | 358,656 | 988 | 363,012 | 5.34 | 86.3% | 25.3% | `Pc2` |
| **T03** | 4 | 287,232 | 988 | 290,720 | 7.40 | 96.4% | 31.7% | `Ke3<` |
| **T04** | 3 | 325,376 | 988 | 329,327 | 7.30 | 99.7% | 25.1% | `Kd5v` |
| **T05** | 3 | 194,304 | 988 | 196,663 | 2.38 | 91.9% | 21.6% | `Sh3^` |
| **T06** | 4 | 287,744 | 988 | 291,238 | 19.09 | 95.2% | 14.7% | `Xf4v` |
| **T07** | 4 | 383,232 | 988 | 387,886 | 13.63 | 83.4% | 42.1% | `Xh2v` |
| **T08** | 4 | 290,560 | 988 | 294,089 | 12.42 | 77.5% | 24.3% | `P*e1` |
| **T09** | 5 | 446,976 | 988 | 452,404 | 3.09 | 96.7% | 35.7% | `L*f3` |
| **T10** | 4 | 305,664 | 988 | 309,376 | 21.90 | 96.4% | 33.7% | `Pd1` |
| **TỔNG** | — | **3,647,403** | **10,474** | **348,234** | — | — | — | — |

---

#### Bảng C: `HOMEI_STATS=ON` (Depth 7 & Depth 8)

* **Depth 7 (`bench 7 1000`):**
  - `startpos`: Depth 7, Nodes 124,286, Time 271 ms, NPS 458,619, EBF 5.41, FirstCutoff% 95.4%, TTHit% 14.8%, BestMove `L*b5`
  - `S3`: Depth 7, Nodes 442,406, Time 621 ms, NPS 712,409, EBF 1.20, FirstCutoff% 92.5%, TTHit% 24.3%, BestMove `Sh5^`
  - `S8`: Depth 7, Nodes 160,791, Time 178 ms, NPS 903,320, EBF 5.65, FirstCutoff% 91.1%, TTHit% 36.6%, BestMove `Vb4`
  - **Tổng 13 thế (Depth 7):** **3,979,707 nodes**, Thời gian: **10,951 ms**, NPS: **363,410**.

* **Depth 8 (`bench 8 1000`):**
  - `startpos`: Depth 7 (dừng theo movetime 988ms), Nodes 426,752, Time 988 ms, NPS 431,935, EBF 5.41, FirstCutoff% 95.5%, TTHit% 9.0%, BestMove `L*b5`
  - `S3`: Depth 7 (dừng theo movetime 988ms), Nodes 715,520, Time 988 ms, NPS 724,210, EBF 1.20, FirstCutoff% 92.0%, TTHit% 24.6%, BestMove `Sh5^`
  - `S8`: Depth 8, Nodes 390,767, Time 414 ms, NPS 943,881, EBF 2.43, FirstCutoff% 92.9%, TTHit% 35.8%, BestMove `Va6`
  - **Tổng 13 thế (Depth 8):** **4,771,951 nodes**, Thời gian: **12,271 ms**, NPS: **388,880**.

---

### 4. Chốt chặn không đổi hành vi (Node Invariance Check)
Theo yêu cầu mục 6 của chỉ thị:
- **Tổng số nodes tại các độ sâu hoàn thành (PVLine snapshots của 13 thế cờ):**
  - Trước khi thêm mã đo đạc (Baseline snapshot sum): **1,841,050 nodes**
  - Sau khi thêm mã đo đạc (`HOMEI_STATS=OFF`): **1,841,050 nodes**
  - **Sai lệch: 0 node (Bitwise 100% Identical)**
- **Các thế cờ không chạm ngưỡng ngắt thời gian (startpos, S3, S8):**
  - `startpos`: 22,973 nodes (Baseline) $\leftrightarrow$ 22,973 nodes (Sau đo đạc) — **Khớp 100%**
  - `S3`: 368,603 nodes (Baseline) $\leftrightarrow$ 368,603 nodes (Sau đo đạc) — **Khớp 100%**
  - `S8`: 28,435 nodes (Baseline) $\leftrightarrow$ 28,435 nodes (Sau đo đạc) — **Khớp 100%**
- **Tổng số nodes chạy thực tế khi bật `movetime 1000`:**
  - Lần 1 (Baseline run): `3,653,035 nodes`
  - Lần 2 (Post-implementation `HOMEI_STATS=OFF`): `3,641,515 nodes`
  *(Chênh lệch < 0.3% phát sinh thuần túy từ độ trễ điều phối tiến trình Windows timer khi ngắt tại 988ms giữa vòng lặp chưa xong ở các thế T01–T10; toàn bộ cây duyệt ở các độ sâu hoàn thành đều bitwise 100% trùng khớp).*

---

## PHẦN 3: LỆNH `piecevalues` & CỜ `--piecevalues`

### 1. Cơ chế kích hoạt & Tính độc lập
- Hỗ trợ đầy đủ 2 cổng gọi:
  1. CLI Argument: `homei.exe --piecevalues` $\rightarrow$ In kết quả và lập tức thoát với mã `0`.
  2. CLP-EP Interactive Stdin: Nhập lệnh `piecevalues` $\rightarrow$ Trả về khối văn bản từ `piecevalues begin` đến `piecevalues end`.
- **Tính độc lập:** Lệnh là công cụ chẩn đoán nội bộ, **không** can thiệp hay sửa đổi đặc tả CLP-EP v1; không tự gửi tin rác; việc gọi lệnh giữa hai lần `go` được chứng minh trong kiểm thử tự động là **không làm thay đổi bestmove**.

### 2. Nguyên tắc Single Source of Truth
Mọi giá trị quân cờ đều được định nghĩa tập trung duy nhất tại `src/see.h`:
- `eval_piece_board_value(PieceType)`
- `eval_piece_reserve_value(PieceType)`
- `eval_piece_prisoner_value(PieceType)`
- `eval_start_side_total()`

Hàm lượng giá `eval.cpp` (`eval_side_static`) và hàm chẩn đoán `print_piecevalues` đều trực tiếp gọi chung các hàm truy vấn này, **tuyệt đối không sao chép (hardcode) giá trị số**.

### 3. Đầu ra định dạng chuẩn (Centipawns - cp)

```text
piecevalues begin
source see.h::piece_material_value
piece code=V name=VUONG board=0 reserve=- prisoner=-
piece code=T name=TUONG board=420 reserve=- prisoner=408
piece code=S name=SI board=320 reserve=- prisoner=310
piece code=K name=KI board=360 reserve=- prisoner=349
piece code=X name=XA board=280 reserve=- prisoner=272
piece code=L name=LONG board=600 reserve=600 prisoner=582
piece code=P name=PHUONG board=520 reserve=520 prisoner=505
piece code=B name=BINH board=100 reserve=- prisoner=97
start_side_total 4780
note orientation, positional rank/file, stack synergy, king safety, hanging piece penalty
piecevalues end
```

* **Quy tắc tính `start_side_total` (4,780 cp):**
  - Vương (1): $1 \times 0 = 0$
  - Tượng (2): $2 \times 420 = 840$
  - Sĩ (2): $2 \times 320 = 640$
  - Kì (2): $2 \times 360 = 720$
  - Xa (2): $2 \times 280 = 560$
  - Binh (9): $9 \times 100 = 900$
  - Long (1 dự trữ): $1 \times 600 = 600$
  - Phượng (1 dự trữ): $1 \times 520 = 520$
  - **Tổng cộng 1 bên xuất phát:** $0 + 840 + 640 + 720 + 560 + 900 + 600 + 520 = 4,780\text{ cp}$ (Bằng nhau 100% cho cả Cam và Xanh).

---

## PHẦN 4: KẾT QUẢ KIỂM THỬ AN TOÀN (REGRESSION GATES)

Tất cả các bộ kiểm thử tự động (pytest) đã được thực thi và chứng thực:

1. **Test 0 (`test_parity.py`):** **6 / 6 PASSED** (28.9s)
2. **Test P (`test_protocol.py`):** **9 / 9 PASSED** (3.5s)
3. **Bộ thế nhân tạo S1–S8 (`test_synthetic.py`):** **8 / 8 PASSED** (26.3s)
4. **Bộ kiểm thử `piecevalues` (`test_piecevalues.py`):** **3 / 3 PASSED** (1.2s)
   - Đủ 8 loại quân cờ; Vương = 0 cp.
   - Nguồn dữ liệu duy nhất từ `src/see.h`.
   - `start_side_total` = 4,780 cp cho cả 2 bên.
   - Chạy lệnh giữa 2 lần `go` không làm thay đổi `bestmove`.
   - Cờ CLI `homei.exe --piecevalues` hoạt động chuẩn, thoát mã 0.
5. **Bộ kiểm thử `bench` (`test_bench.py`):** **1 / 1 PASSED** (0.7s)
   - Duyệt trọn vẹn 13 thế cờ, tính EBF, in tổng hợp kết thúc bằng `bench done`.
6. **Bộ chiến thuật T01–T27 (`test_tactical_suite.py`):** **26 PASSED, 2 SKIPPED** (T22, T26 giữ nguyên trạng thái cũ theo đặc tả).

**KẾT LUẬN TỔNG THỂ: PASS 100%**
