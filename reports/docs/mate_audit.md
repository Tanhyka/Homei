# Báo Cáo Audit Hiện Trạng Bộ Giải Chiếu Hết (Mate Audit)

**Engine:** HomeiEngine (Cờ Long Phượng)  
**Ngày thực hiện:** 12/11/2026  
**Mục tiêu:** Rà soát kiến trúc solver hiện tại trước khi nâng cấp kiến trúc tìm chiếu hết 3 tầng (Homei_v2-2).

---

## 1. Cơ Chế Hoạt Động của Bộ Giải Chiếu Hết (`src/mate.cpp`)
- **Thuật toán cốt lõi:** df-pn (Depth-First Proof-Number Search) trên cây AND/OR game hữu hạn.
  - **OR node (Bên tấn công / Attacker):** Cần chứng minh có ít nhất 1 nước chiếu dẫn tới chiếu hết.
    $$\text{pn} = \min_{m} (\text{child.pn}), \quad \text{dn} = \sum_{m} (\text{child.dn})$$
  - **AND node (Bên phòng thủ / Defender):** Cần thoát chiếu với mọi nước đi hợp lệ.
    $$\text{pn} = \sum_{m} (\text{child.pn}), \quad \text{dn} = \min_{m} (\text{child.dn})$$
- **Bảng băm Transposition Table (df-pn TT):**
  - Mảng tĩnh cấp phát 1 lần dung lượng $2^{20} = 1,048,576$ buckets (`DfpnBucket`).
  - Mỗi bucket gồm 2 slot (ưu tiên độ sâu chứng minh ngắn hơn và thế chỗ theo thế hệ).
  - Khóa Zobrist được phân tách giữa nút OR và AND bằng hằng số bitwise XOR `0xBF58476D1CE4E5B9ULL`.
  - Lưu trữ `proved_len` (chiều dài chuỗi chiếu hết), `disproved_rem` và `best_move`.
- **Xử lý lặp thế (Thiên Nhật Thủ):**
  - Hàm `count_reps(hash, ply)` kiểm tra lịch sử trạng thái:
    - Cộng dồn số lần xuất hiện trong `base_rep_` (từ lịch sử ván cờ gốc `repetition.entries`).
    - Quét dọc theo mảng `path_hashes_` từ gốc tìm kiếm đến độ sâu `ply`.
  - Nếu `count_reps >= 3`: Bên vừa đi vi phạm lặp lần 3 sẽ bị xử thua ngay lập tức theo luật Cờ Long Phượng, triệt tiêu nguy cơ solver lặp nước vĩnh viễn.
- **Ngân sách tìm kiếm hiện tại:**
  - `time_budget_ms`: 250 ms (hoặc $\text{clamp}(\text{hard\_limit} / 4, 50, 100)$).
  - `max_nodes`: 5,000,000 nodes.
  - `max_ply`: Hiện tại bị giới hạn cứng $\min(16, \text{max\_d})$.
- **Vị trí gọi hiện tại trong Engine:**
  - Chỉ được gọi duy nhất tại nút gốc `Searcher::search()` trong `src/search.cpp` (dòng 1000–1039) trước khi bước vào Iterative Deepening.
  - Chưa được tích hợp vào các tầng sâu của PVS (`pvs()`) hay tìm kiếm tĩnh (`qsearch()`).

---

## 2. Kiểm Tra `generate_checks`
- `generate_checks(Position& pos, MoveList& out)` đã sinh đầy đủ 4 loại nước đi theo luật Cờ Long Phượng:
  1. **Drop (Thả quân):** Thả quân từ `reserve` (Long, Phượng ở hàng 2/8) và từ `prisoners` (7 loại quân) gây chiếu trực tiếp hoặc chiếu ngầm.
  2. **Single (Đi lẻ):** Nước đi của quân trên đỉnh chồng quân tới ô trống hoặc ăn quân địch, kèm các góc xoay ($0^\circ, 90^\circ, 270^\circ$).
  3. **Team (Đi cả Đội):** Nước đi của toàn bộ chồng quân khi không chứa Vương, kèm các góc xoay.
  4. **Rotate (Xoay tại chỗ):** Xoay quân tại ô hiện tại để đổi hướng tia chiếu vào Vương đối phương.
- Hỗ trợ đầy đủ chiếu mở (discovered checks) qua tia trượt của Long, Phượng, Xa và chiếu lộ khi tách quân khỏi Đội (stack-split exposed checks).

---

## 3. Kiểm Tra Truyền Lịch Sử Lặp (`base_rep_` + `path_hashes_`)
- Tại nút gốc (`ply == 0`): `Searcher` đã truyền vector `mate_rep` (tương ứng `base_rep_`) vào `solve_mate`. Mảng `path_hashes_` được cập nhật tại mỗi ply trong quá trình duyệt df-pn.
- Điểm cần nâng cấp cho kiến trúc 3 tầng:
  - Khi gọi df-pn ở tầng sâu (như `mate_pvlight` tại `ply = 1, 2`): Cần truyền cả đường đi `path_hashes_[0..ply]` từ PVS vào `DfpnSolver` để tránh trường hợp nước mate ở độ sâu con tạo lặp lần 3 với các nước đi ở gốc PVS.

---

## 4. Cơ Chế Cờ Cấu Hình (`HOMEI_FEATURES`)
Để đảm bảo khả năng bật/tắt độc lập và kiểm định nghiêm ngặt từng tầng theo đúng nguyên tắc:
- Biến môi trường: `HOMEI_FEATURES`
- Cờ điều khiển:
  - `mate_root`: Bật/tắt df-pn 2 pha tại nút gốc.
  - `mate_threat`: Bật/tắt phát hiện đe dọa mate qua Null-Move và mở rộng phòng thủ trong PVS.
  - `mate_pvlight`: Bật/tắt df-pn siêu nhẹ trên nhánh PV.
  - `mate1_leaf`: Bật/tắt quét Mate-in-1 tại nút lá `qsearch`.
- Mặc định khi không đặt biến môi trường: Tất cả các tầng được bật (`true`).
