# BÁO CÁO NGHIỆM THU CHUYỂN ĐỔI KỲ PHỔ SANG LPGF 2.0 & TXT CHUẨN

- **Ngày thực hiện**: 2026-11-14 14:51:30
- **Trạng thái tổng thể**: **PASS**
- **Thời gian xử lý**: 843.55 giây

---

## 1. Thống Kê Tổng Quan

| Tiêu chí | Số lượng | Tỷ lệ |
| :--- | :--- | :--- |
| **Tổng số ván cờ nguồn** (`E:\code5\CLP_dataset\LPGF\HvH\`) | **238** | 100.0% |
| **Tổng số nước đi (plies)** | **17,784** | - |
| **Số nước trung bình / ván** | **74.72** | - |
| **Số ván xuất LPGF 2.0 thành công & Replay 100% PASS** | **238 / 238** | **100.0%** |
| **Số ván xuất TXT chuẩn thành công & Replay 100% PASS** | **238 / 238** | **100.0%** |
| **Số lỗi phát sinh (Failures)** | **0** | **0.0%** |

---

## 2. Tiêu Chuẩn Kỹ Thuật Đạt Được

### A. Định dạng LPGF 2.0 (`HvH_v2_draft\`)
1. **Header chuẩn hóa**:
   ```text
   #lpgf version=2.0 encoding=UTF-8
   Tên Phòng: <tên phòng>
   Ngày Giờ Chơi: <dd-mm-yyyy HH:MM>
   Ngày Giờ Lưu: <dd-mm-yyyy HH:MM>
   Kỳ Thủ Thắng: <người thắng>
   Kỳ Thủ Cầm Cam: <kỳ thủ Cam>
   Kỳ Thủ Cầm Xanh: <kỳ thủ Xanh>
   ```
   - Đã loại bỏ hoàn toàn `#source` và `#repair` thừa.
2. **Ký hiệu nước đi có `(ô_nguồn)`**:
   - Mọi nước đi quân từ bàn cờ được gắn kèm `(ô_nguồn)`: ví dụ `5.Sb8(a8)`, `9.Bd8.d9(c8)`, `17.Bc2<(c3)`, `19.[B]c6>(c6)`, `40.L=(c5)`.
   - Nước thả quân (`1.P*b7`) không kèm `(ô_nguồn)`.
   - Nước đi không xoay hướng không bị chèn ký hiệu hướng mặc định (`Sb8` thay vì `Sb8v`).
   - Xoay tại chỗ giữ nguyên ngoặc vuông: `19.[B]c6>(c6)`.
   - Nước kết thúc `#` hoặc `Xin thua` được bảo toàn chính xác.

### B. Định dạng TXT Chuẩn 238 Ván (`HvH_txt_draft\`)
1. **Quy cách bảng 6 cột**:
   - Mỗi dòng đúng 6 nước đi.
   - Cột cố định 18 ký tự (left-aligned):
     - Nước 1 đến 9 có 1 khoảng trắng ở đầu (` 1.P*b7           `).
     - Nước 10 trở lên không có khoảng trắng (`10.Vh8            `).
   - Tự động ẩn/loại bỏ toàn bộ `(ô_nguồn)` để giữ phong cách truyền thống.

### C. Khả năng tương thích Biến nhánh (Variations) & Sơ đồ ASCII 9x9
1. **Biến nhánh**: Cú pháp `nhánh: nước thứ N` được bộ đọc/ghi nạp và phân nhánh chính xác vào cây nước đi.
2. **Sơ đồ thế cờ ASCII 9x9**: Khi ván cờ bắt đầu từ thế cờ giữa trận hoặc chỉnh sửa, sơ đồ chuẩn 9x9 (kèm tù binh, lượt đi, ply) được chèn tự động giữa header và nước đi; parser phục hồi nguyên vẹn trạng thái bàn cờ.

---

## 3. Danh Sách Lỗi (Nếu Có)

> **Không có lỗi nào phát sinh. Toàn bộ 238/238 ván cờ chuyển đổi và kiểm thử đạt kết quả hoàn hảo 100%.**
