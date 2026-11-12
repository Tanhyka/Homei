# Tiến Độ Triển Khai & Kiểm Thử Engine Hōmei (Cờ Long Phượng)

## Checklist Lộ Trình (Bước 1 → Bước 5)

### Bước 1: Dữ liệu cốt lõi, Zobrist, LPPN, JSON, Base64URL, `clp.state/1` & `clp.move/1`
- [x] Bước 1: Hoàn thành `types`, `geometry`, `zobrist`, `json`, `base64url`, `lppn`, `state_json` (Mục 2.1, 2.2, 3.1, 3.2, 4.1) — PASS (Biên dịch C++20 Release sạch, Zobrist 64-bit SplitMix64, JSON/Base64URL/LPPN/`clp.state/1`/`clp.move/1` khớp tuyệt đối `ref\clp`)

### Bước 2: Sinh nước đi, Kiểm tra chiếu, Notation, Adjudicate & Test 0 (Parity)
- [x] Bước 2: Hoàn thành `attack`, `movegen`, `notation`, `adjudicate` (Mục 2.3 – 2.6) — PASS (`pytest test_parity.py`: 6/6 passed, 18.96s)
- [x] Test 0.1: Thế xuất phát (18 nước hợp lệ cho CAM, 18 cho XANH, `perft 1 = 18`, `perft 2 = 324`) — PASS (Khớp 100% trước và sau giai đoạn thả `reserve`)
- [x] Test 0.2: 200 thế ngẫu nhiên hợp lệ (0–150 nước, có Đội >= 3 quân, bị chiếu, tù binh mọi loại) — khớp 100% tập `legal` và 6 trường cấu trúc với `rules.legal_moves` — PASS (200/200 thế khớp 100% cả số lượng, notation và 6 trường cấu trúc)
- [x] Test 0.3: Apply parity trên mọi nước hợp lệ của 50 thế đầu (LPPN sau `apply` trùng `rules.apply_move` + `to_lppn`) — PASS (Khớp 100% trên toàn bộ nước hợp lệ của 50 thế)
- [x] Test 0.4: Adjudicate parity (lặp lần 3, chiếu hết, PAT, và lặp lần 3 đồng thời đối thủ hết nước -> `repetition3` ưu tiên trước) — PASS (Khớp 100% cả 4 kịch bản với `core_bridge._adjudicate_state_py`)
- [x] Test 0.5: LPPN round-trip trên mọi chuỗi Phần 6 + từ chối chuỗi hỏng (`error ...`, không crash) — PASS (36/36 chuỗi Phần 6 round-trip khớp từng ký tự; 8 mẫu LPPN hỏng trả `error ...` và không crash)
- [x] Test 0.6: Notation parity (khớp `Move.canonical_notation` cho mọi nước hợp lệ, gồm `.src`, `rotate`, `team`, `drop`) — PASS (Khớp 100% với `Move.canonical_notation`)

### Bước 3: Giao thức CLP-EP v1 & Test P (Protocol)
- [x] Bước 3: Hoàn thành vòng lặp lệnh `clp_ep.cpp` và hạ tầng điều khiển tìm kiếm (Phần 3) — PASS (`pytest test_protocol.py`: 9/9 passed, 4.97s)
- [x] Test P.1: Bắt tay (`clp 1` -> `clpok`, `isready` -> `readyok`, khớp `engine_client.EngineClient`) — PASS (`Engine ready (capabilities verified)`)
- [x] Test P.2: Trình tự `position state64` -> `positionok ply n` -> 3x `setoption` -> 3x `optionok` -> `go` — PASS (Đúng thứ tự phản hồi, trả `info64` và `bestmove` hợp lệ)
- [x] Test P.3: `movetime` nhỏ (`movetime = 1` trả `bestmove` hợp lệ; `movetime = 1000` kết thúc trong `1000 ± 150 ms`) — PASS (Hoàn thành trong khoảng `1000 ± 150 ms`, trả nước hợp lệ)
- [x] Test P.4: Giới hạn `Depth = 3` (dừng ở depth <= 3) và `Nodes = 20000` (dừng ở <= 1.05x số nút) — PASS (`depth == 3`, `nodes <= 20000`)
- [x] Test P.5: `MultiPV = 3` (phát `info` cho `multipv = 1, 2, 3` có nước đầu đôi một khác nhau, điểm hạng 1 >= 2 >= 3) — PASS (3 nhánh đầu đôi một khác nhau, điểm đơn điệu giảm)
- [x] Test P.6: `infinite` + `stop` (`searchstarted id N` ngay, `stop id N` trả `bestmove` trong <= 200 ms, bỏ qua id lạc `stop id 999`) — PASS (Phản hồi `stop` < 5 ms, bỏ qua `stop id 999`, chạy tiếp id mới bình thường)
- [x] Test P.7: State terminal / không có nước hợp lệ -> `bestmove id N none` — PASS (Trả `bestmove id N none` cho cả thế chiếu hết và state đánh dấu `terminal`)
- [x] Test P.8: Lịch sử lặp từ `repetition.entries` (`count = 2` -> không đi vào lặp lần 3) — PASS (Tránh nước lặp lần 3 `Vi5`)
- [x] Test P.9: Độ bền (dòng 2 MiB không crash, JSON thiếu khóa -> `error ...`, `quit` thoát sạch trong <= 1 s) — PASS (Bắt lỗi dòng > 2 MiB, JSON thiếu khóa, `quit` thoát mã 0 trong < 50 ms)

### Bước 4: Lượng giá HCE, Stack-SEE, Sắp xếp nước đi, PVS + Quiescence, TT
- [x] Bước 4: Hoàn thành `eval`, `see`, `tt`, `search` (Phần 4, kèm kiểm tra đối xứng `mirror_horizontal` và `flip_colors_180`) — PASS (`test_step4_eval_symmetry` đạt 100% trên toàn bộ 36 thế cờ Phần 6)

### Bước 5: Bộ Test Thực Chiến T01–T27 (Phần 6.3) & Test Bổ Sung S1–S8 (Phần 6.4)
#### Phần 6.3 — Test Thực Chiến (T01–T27)
- [x] T01 — Ăn quân không được bảo vệ (Hình 1) | EXPECT: `[Lc8, [L]c8]` — PASS (`bestmove = [L]c8`, `score = cp 1195`)
- [x] T02 — Ăn quân không được bảo vệ (Hình 2) | EXPECT: `[Pc2, [P]c2]` — PASS (`bestmove = Pc2`, `score = cp 5000`)
- [x] T03 — Ăn quân không được bảo vệ (Hình 3) | EXPECT: `[Ke3<, [K]e3<, Ke3^, Ke3>, [K]e3^, [K]e3>]` — PASS (`bestmove = [K]e3<`, `score = cp 472`)
- [x] T04 — Lấy 1 quân đổi 2 quân (Hình 1) | EXPECT: `[Kd5v]` — PASS (`bestmove = Kd5v`, `score = cp 5000`)
- [x] T05 — Lấy 1 quân đổi 2 quân (Hình 2) | EXPECT: `[Sh3^]` — PASS (`bestmove = Sh3^`, `score = cp 1075`)
- [x] T06 — Đổi quân ít điểm lấy quân cao điểm (Hình 1) | EXPECT: `[Xf4v, Xf4<, Xf4>]` — PASS (`bestmove = Xf4v`, `score = cp 5000`)
- [x] T07 — Đổi quân ít điểm lấy quân cao điểm (Hình 2) | EXPECT: `[Xh2v, Xh2<, Xh2>]` — PASS (`bestmove = Xh2v`, `score = cp 572`)
- [x] T08 — Bắt đôi (Hình 1) | EXPECT: `[P*e1]` — PASS (`bestmove = P*e1`, `score = cp 5000`)
- [x] T09 — Bắt đôi (Hình 2) | EXPECT: `[L*f3]` — PASS (`bestmove = L*f3`, `score = cp 5000`)
- [x] T10 — Đổi quân lớn lấy quân nhỏ rồi thả bắt lại | EXPECT: `[Pd1]` — PASS (`bestmove = Pd1`, `score = cp 5000`)
- [x] T11 — Mở đường để bắt quân lớn (Hình 1) | EXPECT: `[Pc8]` — PASS (`bestmove = Pc8`, `score = cp 5000`)
- [x] T12 — Mở đường để bắt quân lớn (Hình 2) | EXPECT: `[Kh6>]` — PASS (`bestmove = Kh6>`, `score = cp 1045`)
- [x] T13 — Mở đường để bắt quân lớn (Hình 3) | EXPECT: `[Lg3]` — PASS (`bestmove = Lg3`, `score = cp 177`)
- [x] T14 — Bảo vệ quân không có quân giữ (Hình 1) | EXPECT: `[Sh6^, Sh6>, Sh6<, Th6^, Th6>, Th5^, Th5>, Th5<]` — PASS (`bestmove = Sh6^`, `score = cp 5000`)
- [x] T15 — Bảo vệ quân không có quân giữ (Hình 2) | EXPECT: `[Sg5^, Sg5>, Sg5<, Bf7>, Bf5<.f5]` — PASS (`bestmove = Bf5<.f5`, `score = cp 283`)
- [x] T16 — Tăng quân bảo vệ, tránh 2 đổi 1 (Hình 1) | EXPECT: `[Th5^, Th5>, Th5<, Th6^, Th6>, Th6<]` — PASS (`bestmove = Th5^`, `score = cp 5000`)
- [x] T17 — Tăng quân bảo vệ (Hình 2) | EXPECT: `[Bg9>.g9, Bg7<.g7]` — PASS (`bestmove = Bg9>.g9`, `score = cp 5000`)
- [x] T18 — Chạy quân (Hình 1) | EXPECT: `[Pf3, [P]f3, Pd3, Pf5, [P]f5]` — PASS (`bestmove = Pf5`, `score = cp -72`)
- [x] T19 — Chạy quân (Hình 2) | EXPECT: `[Pe4, [P]e4, Pe2, [P]e2, Pf1, [P]f1, Pf5, [P]f5]` — PASS (`bestmove = [P]f5`, `score = cp -100`)
- [x] T20 — Chạy quân (Hình 3) | EXPECT: `[Pd3, [P]d3, Pb5, [P]b5, Pb3, [P]b3, Pa2, [P]a2]` — PASS (`bestmove = [P]d3`, `score = cp 85`)
- [x] T21 — Bảo vệ Vua (Hình 1) | EXPECT: `[B*b8^, B*b8>, B*b8v, B*b8<]` — PASS (`bestmove = B*b8<`, `score = cp 1120`)
- [x] T22 — Chiếu hết 3 nước (Hình 1) | MATE(3 ply) / UNVERIFIED — SKIPPED per spec (LPPN thiếu quân trong tay, không tính FAIL)
- [x] T23 — Chiếu hết 3 nước (Hình 2) | MATE(3 ply) — PASS (`bestmove = S*h7v`, `score = mate 3`)
- [x] T24 — Chiếu hết 5 nước (Hình 1) | MATE(5 ply) — PASS (`bestmove = X*d2v`, `score = mate 5`)
- [x] T25 — Chiếu hết 5 nước (Hình 2) | MATE(5 ply) — PASS (`bestmove = T*i8^`, `score = mate 3`)
- [x] T26 — Chiếu hết chuỗi dài (Hình 1) | MATE(<= 9 ply) / UNVERIFIED — SKIPPED per spec (chưa có lời giải xác nhận độc lập)
- [x] T27 — Chiếu hết chuỗi dài (Hình 2) | MATE(7 ply) — PASS (`bestmove = P*i5`, `score = mate 7`)
- [x] T28 — Chiếu hết chuỗi dài 15 ply (Lượt: XANH, `ply = 119`) | MATE(15 ply) — PASS (`bestmove = K*a3<`, `score = mate 15`, `pv_len = 15`, `time = 160 ms`, `Homei_v2-2.exe`)
  - LPPN: `lppn1 1B2B26/2S2S2V21T21P2/2B2B2B2B21B21/6b02/3B21P23/9/L24b0b0b0b0/3t0x11B11x0/8v0 x -/- TKKXBBB/TSSKKXLB 119`
  - Kết quả đo thực tế (`Homei_v2-2.exe`, 5s):
    - Tại thế gốc (ply 119, lượt XANH): `bestmove = K*a3<`, `score = mate 15`, `pv_len = 15`, thời gian 160 ms. Chuỗi PV: `K*a3< Va4 T*b5^ Va3 S*a4> Va2 Sb3> Vb3 T*a3> Va3 L*b3 Va2 S*b2^ Vb3 X*b4>#`
    - Nhánh phòng thủ Ta3< (`Ta3< -> Kc3< -> Va4/Va5/Va6/Vb4`): Đều giải ra đường thắng/sát cục tiếp theo đúng luật.
    - Sau nước vàng `9. Sb1v` (`ply = 128`, lượt CAM): Cả 4 nước chạy Vương của CAM (`Vd2`, `Vd1`, `Vb1`, `Vb2`) đều được XANH chiếu hết chính xác (`11. P*c1`, `11. K*f1v`, `11. L*a1`, `11. T*a1<#`).
  - Đáp án trong ảnh (đã đối chiếu `ref.clp.rules` và `homei.exe`):
    - Mở đầu (nước 1–9): `1. K*a3<` *(màu xanh)* → `2. Va4` *(hoặc `2. Vb4` đều gặp `3. S*b5>`; còn thiếu `2. Va6` → `3. S*b5^#` và `2. Va5` → `3. T*b5^#`; nếu `2. Ta3< / Ta3> / Ta3v` → `3. K*c3<` → `4. Va6` [`S*b5^#`] / `4. Va5` [`T*b5^#`] / `4. Va4` [`T*b5> Vb3 S*c3^ Va2 S*b2^#`] / `4. Vb4` \[vàng, chưa đi tiếp; `homei.exe` 4s: `L*b2`, `-504 cp`\] / ~~`4. Vb5`~~ \[bất hợp lệ\])* → `3. S*b5>` → `4. Va3` *(còn thiếu `4. Va5` → `5. S*a4<#`)* → `5. X*a4>` → `6. Vb2` → `7. S*a1v` → `8. Vc1` → `9. Sb1v` *(màu vàng, nước đi từ `a1` lên `b1`)*.
    - Các nhánh sau `9. Sb1v` (nước 10–15):
      - `10. Vd2` → `11. P*c1`:
        - `12. Vd3` → `13. T*e3^/<` → `14. Vc3` → `15. K*e2^#`
        - `12. Vd1` → `13. K*f1v` → `14. Ve2` → `15. T*f2^/#>` | hoặc `13. K*f2^` → `14. Ve2` → `15. T*f1^/#<`
        - `12. Ve1` → `13. T*f1^/<` → `14. Vd1` → `15. K*f2^#` | hoặc `13. T*f2^/>` → `14. Vd1` → `15. K*f1v#`
        - `12. Ve2` → `13. L*e3` → `14. Vd1` → `15. K*f2^#` / `15. K*f1v#` / `15. T*e1#`
      - `10. Vd1` → `11. K*f1v`:
        - `12. Vd2` → `13. T*d1v/<` → `14. Ve3` *(ảnh ghi `14. Ve2` là bất hợp lệ)* → `15. P*f2#` | hoặc `13. P*c1` → `14. Ve2` → `15. T*e3>` → `16. Ve1` → `17. Tf2>` *(hoặc `Tf2^#`)* | `14. Ve1` *(ảnh thiếu)* → `15. T*d1v#`
        - `12. Ve1` → `13. T*f2^/>` → `14. Vd2` → `15. L*e2#`
        - `12. Ve2` → `13. T*d1v/<` → `14. Ve3` → `15. P*f2#` *(ảnh ghi `13. T*d1#` là thiếu 2 ply cuối)*
      - `10. Vb1` → `11. L*a1` → `12. Vb2` → `13. T*c1^/#<`
      - `10. Vb2` → `11. T*a1<#` *(hoặc `T*a1v#`)*

#### Phần 6.4 — Test Bổ Sung (S1–S8)
- [x] S1 — Chiếu hết 1 nước bằng tách Đội / Binh xoay | MATE1: `[Bb8^, Tb8<, Tb8>, Tb8^, Tb9>, Tb9^, [T]b8<, [T]b8>, [T]b8^]` — PASS (`bestmove = Tb9^`, `score = mate 1`, `time = 2 ms`)
- [x] S2 — Chiếu hết 1 nước bằng thả tù binh | MATE1: `[X*a7<, X*a8<, X*b9^]` — PASS (`bestmove = X*b9^`, `score = mate 1`, `time = 7 ms`)
- [x] S3 — Bị chiếu bởi Long, phải thoát chiếu và không bị mate 1 | SET (40 nước thoát chiếu, không bị mate 1) — PASS (`bestmove = Sh5^`, `score = cp 975`, `time = 4989 ms`)
- [x] S4 — Ăn quân hở (Phượng ăn Long) | EXPECT: `[Pe5]` — PASS (`bestmove = Pe5`, `score = cp 1205`, `time = 4989 ms`)
- [x] S5 — Binh bị Long Xanh giữ (không ăn bằng Long) | NOT `Le5` + EXPECT — PASS (`bestmove = Vb5`, `score = cp -122`, `time = 4989 ms`)
- [x] S6 — Long bị Binh đe dọa, ăn Binh | EXPECT: `[Lf5]` — PASS (`bestmove = Lf5`, `score = cp 785`, `time = 4988 ms`)
- [x] S7 — Cứu cả Đội bị Xa đe dọa | EXPECT: `[[K]c6^, [K]c6>, [K]c6<, [K]c4^, [K]c4>, [K]c4<, [K]g7^, [K]g7>, [K]g7<, [K]g3^, [K]g3>, [K]g3<]` — PASS (`bestmove = [K]g3<`, `score = cp 661`, `time = 4989 ms`)
- [x] S8 — Không tự thua vì lặp thế lần 3 (Thiên Nhật Thủ) | NOT `Vi5` (và kiểm chứng đảo chiều khi bỏ lịch sử lặp) — PASS (`bestmove = Lc1`, `score = cp 1213`, `time = 214 ms`)

---

## Ghi Chú Mâu Thuẫn Giữa `homei.md` Và Mã Python Tham Chiếu (`ref\clp`)

1. **Mục 3.3 (dòng 162) vs Mục 3.1 & `lppn.py`:**
   - Trong `homei.md` dòng 162 viết lệnh mở rộng: `position lppn <8 trường LPPN>`.
   - Tuy nhiên, Mục 3.1 (dòng 121) và `ref\clp\lppn.py` (`from_lppn`, dòng 204) quy định rõ LPPN chỉ có **6 trường** cách nhau bằng khoảng trắng (`lppn1 BOARD TURN RESERVE PRISONERS PLY`).
   - **Xử lý (Python thắng):** Engine đọc phần chuỗi sau `position lppn ` và truyền vào bộ giải mã LPPN 6 trường chuẩn khớp `lppn.py`.
2. **Mục 2.1 (`validate()`) vs `lppn.py` (`_validate_board`):**
   - Mục 2.1 nêu tổng kho quân cả 2 bên là 40 quân cố định từng loại, nhưng `lppn.py` (`_validate_board(board, strict=True)`) chỉ kiểm tra mỗi bên có đúng 1 `VUONG`, tổng số quân `<= 40`, mỗi stack `<= 38`, không có stack khác màu hoặc chứa `VUONG`, không có `VUONG` trong tay, `reserve` chỉ chứa `L`/`P` không trùng lặp (cho phép các thế cờ bài tập S1–S8 có ít hơn 40 quân).
   - **Xử lý (Python thắng):** `validate()` trong C++ kiểm tra đúng các điều kiện của `lppn._validate_board(strict=True)` (và `cpp-CLP/core/core.cpp::validate`).
3. **Mục 2.2 (Chuẩn hóa D-001 cho `facing` của `V, L, P`) vs `lppn.py`:**
   - Trong `lppn.py`, khi đọc một quân từ chuỗi LPPN (`_decode_pair`), `facing` được giữ nguyên theo ký tự số `0..3` trong chuỗi LPPN và xuất lại nguyên vẹn trong `to_lppn` (ví dụ nếu test có `V0` cho CAM hoặc `v2` cho XANH thì round-trip vẫn bảo toàn, dù khi sinh nước thả mới thì dùng `initial_facing(player)`). Trong khi di chuyển, `pieces.py` (`board_moves_for`) dùng `effective_facing = 0` cho `NON_ROTATABLE`.
   - **Xử lý (Python thắng):** Lưu đúng `facing` 2-bit trong `Piece` khi đọc LPPN/JSON để `to_lppn` round-trip khớp tuyệt đối `lppn.py`, dùng `effective_facing = 0` khi tính nước đi của `V, L, P`, và dùng `initial_facing(player)` khi sinh nước `drop` cho `V, L, P`.

---

## Các Điểm Cần Xác Nhận (Sẽ cập nhật trong quá trình chạy test)
- (Chưa có)
