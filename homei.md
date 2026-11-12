# MASTER PROMPT DÀNH CHO ANTIGRAVITY: XÂY DỰNG ENGINE CỜ LONG PHƯỢNG (HŌMEI) — v2

> **Hướng dẫn sử dụng:** Đặt file này (`homei.md`) vào workspace của Antigravity cùng với **bộ file nguồn sự thật** của GUI: `pieces.py`, `board.py`, `rules.py`, `lppn.py`, `core_bridge.py`, `native.py`, `engine_client.py`. Rồi ra lệnh:
> *"Hãy đọc kỹ toàn bộ đặc tả trong `homei.md`. Thực thi tuần tự Bước 1 đến Bước 5 (Phần 0) bằng C++20, khớp từng bit với `rules.py` / `lppn.py` / `core_bridge.py` và giao thức CLP-EP v1 trong `engine_client.py`. Không bỏ qua ràng buộc nào, và chỉ báo hoàn thành khi Test 0, Test P, Bộ thực chiến (Phần 6) và các test bổ sung S1–S8 đều đạt tiêu chí PASS."*

**Thay đổi so với v1 (để Antigravity không dùng nhầm bản cũ):**
- Định dạng thế cờ là **LPPN** (`lppn1 ...`) và **state JSON `clp.state/1`**, không còn `CLP1` / `UCLP`. Nước đi là **JSON `clp.move/1`** kèm `notation` chuẩn (`Move.canonical_notation` trong `rules.py`), không còn `S:34:04:L:-1:0`.
- Giao thức engine ↔ GUI là **CLP-EP v1** (đúng như `engine_client.py` đang nói chuyện), có MultiPV, Depth, Nodes, `infinite` + `stop`.
- Bộ test thực chiến được **thay bằng các thế trong PDF "Phương pháp training engine Cờ Long Phượng (Hōmei)" cùng đáp án do chủ dự án nhập** (dạng LPPN thật từ GUI). Mọi đáp án đã được đối chiếu với `rules.py` (hợp lệ, và với chiếu hết thì chạy lại nước đi để xác minh). Các thế tổng hợp cũ (CLP1) đã được dựng lại bằng LPPN và kiểm chứng bằng `rules.py` thành các test bổ sung S1–S8.
- Sửa các lỗi đặc tả đã phát hiện: Kị bị định giá thấp hơn Xa/Sĩ, công thức `stack_see_estimate` mơ hồ, điểm lặp thế không nhất quán, `piece_code_44` không định nghĩa, tham chiếu "Bước 1–5" không tồn tại.

---

## PHẦN 0. LỘ TRÌNH THỰC THI (BƯỚC 1 → BƯỚC 5)

Không sang bước sau khi test của bước trước chưa PASS.

| Bước | Nội dung | Mục / Test cổng |
|---|---|---|
| 1 | Kiểu dữ liệu, hình học, `Position`, Zobrist, codec LPPN, parse/serialize `clp.state/1` & `clp.move/1`, JSON + base64url tự cài (không phụ thuộc thư viện ngoài, trừ khi được phép) | 2.1, 2.2, 3.1, 3.2, 4.1 |
| 2 | `attack`, `movegen`, `apply/undo`, ký hiệu `notation`, adjudicate (lặp thế/chiếu hết/PAT), perft | 2.3 – 2.6 · **Test 0 (Parity)** |
| 3 | Giao thức CLP-EP v1 (handshake, `position state64`, `setoption`, `go`, `info64`, `bestmove`, `stop`) | Phần 3 · **Test P (Protocol)** |
| 4 | Lượng giá HCE, Stack-SEE, sắp xếp nước đi, PVS + Quiescence, TT, MultiPV, thời gian | Phần 4 |
| 5 | Chạy toàn bộ bộ test thực chiến và test bổ sung | Phần 6 · **Test T, M, S1–S8** |

---

## PHẦN 1. VAI TRÒ & MỤC TIÊU TỐI THƯỢNG

Bạn là Kỹ sư trưởng phát triển AI Cờ Long Phượng (**Project Hōmei**). Xây một Engine C++20 độc lập, tốc độ cao, tương thích 100% với bộ luật tham chiếu Python (`pieces.py`, `board.py`, `rules.py`, `lppn.py`, `core_bridge.py`) và nói đúng giao thức **CLP-EP v1** của GUI.

**Mục đích sử dụng:** (1) **phân tích ván** kiểu Lichess/ShogiGUI (mũi tên nước tốt nhất, điểm, MultiPV, phân tích vô hạn có `stop`), (2) bot đủ mạnh để thắng người chơi nghiệp dư (tầm "cờ vua ~1500": giữ quân, ăn quân hở, đọc chiếu hết, không nộp quân vô cớ). Sức mạnh bot là thứ yếu so với độ **chính xác luật** và độ **tin cậy khi phân tích**.

Bộ kiểm thử bám theo 4 nhóm năng lực trong PDF của dự án:

1. **Ăn quân (tấn công):** ăn quân không được bảo vệ; lấy 1 quân đổi 2; đổi quân ít điểm lấy quân cao điểm; bắt đôi; chuỗi 3–5 nước (đổi quân lớn lấy quân nhỏ rồi thả quân nhỏ để bắt lại, hoặc mở đường để bắt quân lớn).
2. **Bảo vệ quân:** bảo vệ quân bị nhắm mà chưa có quân giữ (thường là Binh); tăng quân bảo vệ để tránh 2 đổi 1 / 3 đổi 1; chạy quân.
3. **Bảo vệ Vương:** chạy Vương, chấp nhận mất quân để không bị chiếu hết, chặn/dập đòn chiếu hết của đối phương (kể cả thả tù binh).
4. **Chiếu hết:** mate 1 nước, 3 nước, 5 nước, chuỗi dài (kể cả chiếu bằng thả tù binh, xoay quân tại chỗ, đi Đội, hoặc tách Đội mở tia chiếu bật).
5. **Tuân thủ Thiên Nhật Thủ (lặp thế lần 3 = người vừa đi THUA)** và **PAT = thua**: không bao giờ tự đi vào lặp lần 3 trừ khi mọi nước khác đều thua nhanh hơn.

---

## PHẦN 2. ĐẶC TẢ LUẬT CỜ LONG PHƯỢNG (NGUỒN SỰ THẬT DUY NHẤT)

> Đã đối chiếu từng điều khoản với `rules.py`, `board.py`, `pieces.py` và `core_bridge.py`. Khi có mâu thuẫn giữa file này và các file Python đó, **file Python thắng**, và phải báo lại chỗ mâu thuẫn.

### 2.1. Bàn cờ, Tọa độ, Hướng và Kho quân
* **Bàn cờ:** lưới $9 \times 9$. Hàng `r` từ `0..8` (nhãn `a..i` từ trên xuống), cột `c` từ `0..8` (nhãn `9..1` từ trái sang phải). Chỉ số ô phẳng `sq = r * 9 + c`. Tên ô trong ký hiệu: `"abcdefghi"[r] + "987654321"[c]` (ví dụ `(0,3)` = `a6`).
* **Hai phe:** `CAM` (đi trước, khởi đầu hàng `0`, 9 Binh ở hàng `2`, `initial_facing = 2` nhìn xuống) và `XANH` (đi sau, hàng `8`, Binh hàng `6`, `initial_facing = 0` nhìn lên).
* **Xếp quân hàng đầu (`c = 0..8`):** `[XA, KI, SI, TUONG, VUONG, TUONG, SI, KI, XA]`.
* **Quân dự bị tiền trận (`reserve`):** mỗi bên khởi đầu `[LONG, PHUONG]`.
* **Kho quân bất biến (cả 2 bên, trên bàn + `reserve` + `prisoners`):** 40 quân = `2 VUONG, 4 TUONG, 4 SI, 4 KI, 4 XA, 2 LONG, 2 PHUONG, 18 BINH`. Quân bị bắt vào `prisoners` của người bắt và khi thả sẽ mang **màu người thả** (D-005), nên một bên có thể có hơn 9 Binh. `validate()` kiểm theo **tổng số mỗi loại của cả hai bên cộng lại**.
* **4 hướng nhìn (`facing`):** `0` lên `(-1,0)`, `1` phải `(0,+1)`, `2` xuống `(+1,0)`, `3` trái `(0,-1)`. Xoay 90° theo chiều kim đồng hồ `rotate_cw((dr,dc), t)`: `t=0 → (dr,dc)`, `t=1 → (dc,-dr)`, `t=2 → (-dr,-dc)`, `t=3 → (-dc,dr)`.

### 2.2. Vector di chuyển gốc (ứng với `facing = 0`, tiến là `(-1, 0)`)
* **Nhóm không xoay `NON_ROTATABLE = {VUONG, LONG, PHUONG}`:** luôn dùng `effective_facing = 0`, không sinh nước `rotate`, `rotate_after = 0`.
  * **Chuẩn hóa D-001:** `facing` lưu trong quân `V, L, P` luôn là hướng mặc định của chủ (`CAM = 2`, `XANH = 0`). LPPN do GUI sinh ra đã theo quy ước này (ví dụ Long Cam `L2`, Long Xanh `l0`). Engine phải giữ nguyên quy ước khi thả quân và khi băm Zobrist.
* **Bảng vector (`MOVES_LOCAL`):**
  1. `VUONG (V)` — step 8 hướng: `(-1,-1),(-1,0),(-1,1),(0,-1),(0,1),(1,-1),(1,0),(1,1)`.
  2. `TUONG (T)` — step 6 hướng: `(-1,0),(-1,-1),(-1,1),(0,-1),(0,1),(1,0)`.
  3. `SI (S)` — step 5 hướng: `(-1,0),(-1,-1),(-1,1),(1,-1),(1,1)`.
  4. `KI (K)` — jump 5 điểm đáp (nhảy qua mọi quân): `(-2,-1),(-2,1),(2,0),(2,-2),(2,2)`.
  5. `XA (X)` — slide 1 tia thẳng phía trước: `(-1,0)`.
  6. `LONG (L)` — slide 4 tia ngang dọc: `(-1,0),(1,0),(0,-1),(0,1)`.
  7. `PHUONG (P)` — slide 4 tia chéo: `(-1,-1),(-1,1),(1,-1),(1,1)`.
  8. `BINH (B)` — step 1 bước thẳng phía trước: `(-1,0)`.
* Tia trượt dừng ở quân đầu tiên gặp (ô đó vẫn là đích hợp lệ nếu là quân địch, và bị chặn nếu là quân mình).

### 2.3. Cấu trúc Đội (Stack) và sinh nước đi (`generate_moves`)
* Mỗi ô là một ngăn xếp từ đáy lên đỉnh (`0..38` quân, `MAX_STACK_SIZE = 38`). Mọi quân trong một ô cùng chủ. **Quân trên cùng (`top`)** quyết định chủ ô và cách đi.
* **Vương không bao giờ bị bắt và không bao giờ nằm trong Đội** (không chồng lên quân khác, không bị chồng lên, không đi theo Đội).
* **Giai đoạn Tiền trận (`reserve[player]` còn quân):** chỉ được sinh nước thả từ `reserve`, thứ tự loại quân theo `V,T,S,K,X,L,P,B`.
  * Hàng thả bắt buộc: `CAM` hàng `1`, `XANH` hàng `7`. Ô đích: ô trống, hoặc ô của mình không có Vương và `stack.size() < 38`.
  * Hướng thả: loại không xoay chỉ có `initial_facing(player)`; loại xoay được sinh đủ `0,1,2,3`.
* **Giai đoạn Chính trận (`reserve[player]` rỗng)** — 4 loại nước, **thứ tự sinh y như `rules.generate_moves`** (thả tù binh trước, rồi duyệt ô theo `r` rồi `c`, với mỗi đích và mỗi `rot` thì `single` trước `team`, cuối cùng `rotate`):
  1. **Thả tù binh (`drop`):** mỗi loại quân phân biệt trong `prisoners[player]` (sắp theo `V,T,S,K,X,L,P,B`), thả vào mọi ô trống, HOẶC ô của mình không có Vương và `size < 38`. Hướng: `[initial_facing]` nếu không xoay được, `[0,1,2,3]` nếu xoay được. Thả lên ô của đối phương là **cấm**.
  2. **Đi quân lẻ (`single`):** quân `top` của ô nguồn đi tới ô đích theo `_raw_destinations`.
     * Đích trống hoặc của địch (không chứa Vương địch): hợp lệ, kèm `rotate_after ∈ {0,1,3}` nếu `top` xoay được, ngược lại `{0}`.
     * Đích là ô của mình (tạo Đội): **chỉ hợp lệ khi** ô nguồn đứng lẻ (`size == 1`), quân nguồn không phải `VUONG`, đích không có `VUONG`, `dst.size < 38`. *(Quân vừa tách khỏi một Đội `size ≥ 2` KHÔNG được tạo Đội mới trong cùng nước đi.)*
  3. **Đi cả Đội (`team`):** khi `src.size ≥ 2` và không chứa `VUONG`, cả chồng đi theo vector của `top` tới ô trống hoặc ô địch (không chứa Vương địch). **Đội không bao giờ được đi vào ô của quân mình.** Kèm `rotate_after ∈ {0,1,3}` cho `top` nếu xoay được.
  4. **Xoay tại chỗ (`rotate`):** nếu `top` xoay được, sinh 2 nước với `rotate_after ∈ {1,3}` (cấm xoay 180° tại chỗ).

### 2.4. Thực thi nước đi (`apply_move`) và nước đi hợp lệ
1. `single`: `pop()` quân top khỏi `src`. `team`: lấy toàn bộ `src.stack`, làm rỗng `src`.
2. Nếu `dst` thuộc đối phương: toàn bộ quân trong `dst.stack` vào `prisoners[player]` (theo thứ tự đáy lên đỉnh), rồi xóa sạch `dst.stack`.
3. Nếu `rotate_after != 0`: quân trên cùng vừa di chuyển có `facing = (facing + rotate_after) % 4`.
4. Nối quân (hoặc cả Đội) lên đỉnh `dst.stack`.
5. Nước `drop`: bốc 1 quân từ `reserve` (nếu có loại đó trong `reserve`) nếu không thì từ `prisoners`; đặt lên đỉnh ô đích với màu của người thả và `facing` của nước đi.
* **Nước hợp lệ (`legal_moves`):** nước trong `generate_moves` mà sau khi thực thi, Vương của người vừa đi **không** bị tấn công. Tấn công = quân `top` của bất kỳ ô địch nào có `raw_destination` trùng ô Vương (kể cả khi đó là ô `capture`).

### 2.5. Ký hiệu nước đi chuẩn (`notation`) — phải khớp từng ký tự `Move.canonical_notation(board_before)`
```
L        = chữ quân: V T S K X L P B   (single/team/rotate: quân TOP của ô nguồn; drop: move.ptype)
piece    = "[" + L + "]" nếu kind == team, ngược lại L
drop     = "*" nếu kind == drop, ngược lại ""
dst_sq   = square(dst) ; với rotate thì dst_sq = square(src)
dir      = chỉ khi quân XOAY ĐƯỢC: "^>v<"[(facing_hiện_tại + rotate_after) % 4]
           (facing_hiện_tại = move.facing nếu drop, ngược lại facing của quân top ô nguồn)
suffix   = "." + square(src) nếu kind == rotate
           hoặc nếu kind ∈ {single, team} và
             [ số ô có quân top CÙNG CHỦ, CÙNG LOẠI với quân đang đi và có raw_destination == dst ] > 1
             hoặc [ ô dst đang là quân top CÙNG CHỦ, CÙNG LOẠI và xoay được ]
notation = piece + drop + dst_sq + dir + suffix
```
Ví dụ: `Se6<` (Sĩ tới e6, quay trái), `P*d5` (thả Phượng xuống d5), `K*c8v` (thả Kị xuống c8 quay xuống), `[B]e3^` (cả Đội đi tới e3, top là Binh quay lên), `Bb5>.b5` (Binh xoay tại chỗ).

### 2.6. Kết thúc ván & Thiên Nhật Thủ (khớp `core_bridge._adjudicate_state_py`)
* **Khóa trạng thái (`position identity`):** toàn bộ `position` (81 ô với từng quân `type/owner/facing` theo thứ tự đáy lên đỉnh, `reserve`, `prisoners`, `to_move`), **không** gồm `notation` và `ply`.
* Sau mỗi nước đi tạo ra trạng thái `S'` (bên tới lượt là $P'$): tăng bộ đếm của `S'`.
  1. Nếu đếm `>= 3`: **bên vừa đi ($P$) THUA**, `reason = "repetition3"` — xét **trước cả chiếu hết**.
  2. Ngược lại, nếu $P'$ không còn nước hợp lệ: **$P'$ THUA**. `reason = "checkmate"` nếu $P'$ đang bị chiếu, `"pat"` nếu không. **Không có kết quả hòa.**
  3. Ngược lại: `ongoing`.

---

## PHẦN 3. ĐỊNH DẠNG DỮ LIỆU & GIAO THỨC (CLP-EP v1)

> Nguồn: `lppn.py` (LPPN), `core_bridge.py` (`clp.state/1`, `clp.move/1`, adjudicate), `engine_client.py` (CLP-EP v1). Engine là **tiến trình riêng** đọc stdin / ghi stdout, mỗi lệnh một dòng UTF-8 kết thúc `\n`. Dòng tối đa 2 MiB. Tên file thực thi mặc định `clp_core_cli.exe` (cho GUI cũ) và có thể thêm bản sao `homei.exe`. Mọi log phụ ghi ra **stderr**, tuyệt đối không ghi dòng lạ ra stdout ngoài danh sách dưới đây.

### 3.1. LPPN v0.2 (`lppn1 BOARD TURN RESERVE PRISONERS PLY`, 6 trường cách nhau bằng khoảng trắng)
* `BOARD`: 9 hàng (`r = 0..8`) ngăn cách bằng `/`; mỗi hàng đi từ cột `c = 0` đến `8`. Chữ số `1..9` = số ô trống liên tiếp. Quân = `<chữ><facing 0..3>`, **chữ HOA = CAM, chữ thường = XANH**, chữ quân `V T S K X L P B`. Đội = `[` quân… `]` liệt kê **từ đáy lên đỉnh**, ít nhất 2 quân.
* `TURN`: `c` (CAM) hoặc `x` (XANH).
* `RESERVE`, `PRISONERS`: dạng `CAM/XANH`, mỗi nửa là chuỗi chữ HOA theo thứ tự `V,T,S,K,X,L,P,B` (mỗi quân một ký tự, lặp nếu có nhiều) hoặc `-` nếu rỗng. `RESERVE` chỉ chứa `L`/`P`, mỗi loại tối đa 1. Không có `V` trong tay.
* `PLY`: số nguyên không có số 0 đứng đầu. Strict: mỗi bên đúng 1 Vương, tổng ≤ 40 quân, mỗi Đội ≤ 38, Đội không chứa Vương, mọi quân trong Đội cùng chủ.
* Engine phải có `lppn_to_state()` và `state_to_lppn()` (để lệnh debug, test, log). Round-trip phải khớp `lppn.to_lppn(from_lppn(x))`.

### 3.2. Trạng thái `clp.state/1` và nước đi `clp.move/1` (JSON, khóa đúng như dưới, thứ tự khóa tùy ý)
```json
{"schema":"clp.state/1","notation":null,"ply":15,
 "position":{"board":[ /*81 ô, mỗi ô = mảng quân đáy→đỉnh*/ [{"facing":2,"owner":"CAM","type":"XA"}], [], ... ],
             "prisoners":{"CAM":["BINH"],"XANH":[]},
             "reserve":{"CAM":[],"XANH":[]},
             "to_move":"XANH"},
 "repetition":{"complete":true,"entries":[{"count":1,"position":{ /*cùng schema position*/ }}]},
 "result":{"status":"ongoing"}}
```
* `owner` ∈ `CAM|XANH`; `type` ∈ `VUONG TUONG SI KI XA LONG PHUONG BINH`; ô `i` = hàng `i // 9`, cột `i % 9`.
* `repetition.entries`: **toàn bộ lịch sử trạng thái đã xuất hiện** và số lần (tính cả trạng thái hiện tại). Engine nạp chúng làm bảng đếm khởi tạo cho kiểm tra lặp thế (mục 4.2). Nếu `complete == false`, engine vẫn tìm kiếm nhưng phát một dòng `info ... string64` cảnh báo.
* `result.status == "terminal"`: engine trả `bestmove id N none`, không tìm kiếm.
* **Nước đi** (đúng 8 khóa, không thừa, không thiếu):
```json
{"schema":"clp.move/1","kind":"single","src":[4,3],"dst":[3,3],"ptype":"SI","facing":null,"rotate_after":1,"notation":"Sf6^"}
```
  `kind ∈ single|team|rotate|drop`; `src`/`dst` là `[r,c]` hoặc `null` (`drop`: `src = null`; `rotate`: `dst = null`); `ptype` là tên loại quân (với `single/team/rotate` là loại quân `top` ô nguồn); `facing` là `null` trừ `drop` (số `0..3`); `rotate_after ∈ {0,1,3}`; `notation` theo mục 2.5.
* **Mã hóa vận chuyển:** `state64` và `move64` = JSON UTF-8 nén chuẩn `base64url` **không có `=` padding**. Phải chấp nhận cả có và không có padding khi đọc (`_b64url_decode` tự thêm). State JSON tối đa 1 048 576 byte.

### 3.3. Bắt tay & lệnh CLP-EP v1 (khớp `engine_client.py`)
| GUI gửi | Engine phải phản hồi (mỗi thứ một dòng) |
|---|---|
| `clp 1` | `id name Homei`, `protocol clp-ep 1`, `capability search legal`, `capability clocks movetime`, `capability search infinite`, `capability control stop`, `option name Depth type spin default 12 min 1 max 32`, `option name Nodes type spin default 50000000 min 1 max 50000000`, `option name MultiPV type spin default 1 min 1 max 5`, rồi **`clpok`** |
| `isready` | `readyok` (GUI chỉ chấp nhận khi đã thấy đủ protocol, MultiPV, `search legal` + `clocks … movetime`, hoặc Depth + Nodes) |
| `position state64 <b64url>` | `positionok ply <n>` (**bắt buộc có ít nhất một token sau `positionok `**). Sai định dạng: `error <lý do ngắn, ≤ 300 ký tự>` |
| `setoption name Depth value D` | `optionok name Depth value D` (tương tự `Nodes`, `MultiPV`). `D ∈ 1..32`, `Nodes ∈ 1..50 000 000`, `MultiPV ∈ 1..5` |
| `go id N movetime MS` | tìm kiếm tối đa `MS` ms (`1..86 400 000`) hoặc đến khi chạm `Depth`/`Nodes`, gì tới trước. Xem 3.4 |
| `go id N infinite` | phát `searchstarted id N` **ngay** khi bắt đầu (GUI chờ dòng này). Chỉ dùng MultiPV `1..3`, không đặt Depth/Nodes |
| `stop id N` | dừng trong **≤ 200 ms** và phát `bestmove id N …` |
| `quit` | thoát tiến trình |

* GUI luôn gửi theo thứ tự: `position state64` → (nhận `positionok`) → `setoption Depth` → `setoption Nodes` → `setoption MultiPV` (chế độ `infinite`: chỉ `MultiPV`) → đủ `optionok` → `go`. **Engine không được tự bắt đầu tìm kiếm trước `go`.**
* `id N` là số nguyên dương; mọi dòng `info`, `searchstarted`, `bestmove` phải mang đúng `N` đang hoạt động.
* **Lệnh mở rộng cho test (GUI bỏ qua dòng lạ nên an toàn):** sau `position`, `legal` → `legal <count> <notation1> <notation2> …` (sắp xếp từ điển); `perft <d>` → `perft <d> <số_nút>` (đếm theo `legal_moves`, chưa áp adjudicate); `lppn` → `lppn <chuỗi LPPN của vị trí hiện tại>`; `apply <notation>` → áp một nước hợp lệ (theo `notation` chuẩn), cập nhật bộ đếm lặp thế, rồi trả `positionok ply <n>`, hoặc `error illegal move` nếu không hợp lệ; `position lppn <8 trường LPPN>` (tạo state gốc `count = 1`) → `positionok ply <n>`.

### 3.4. Phản hồi tìm kiếm
* **`info id N info64 <b64url JSON>`**, mỗi lần hoàn tất một độ sâu cho từng hạng MultiPV `1..K`:
```json
{"schema":"clp.info/1","id":N,"multipv":1,"depth":9,"time_ms":812,"nodes":1540220,
 "score":{"kind":"cp","perspective":"root","value":120},
 "pv":[ /*mảng clp.move/1, nước đầu là nước của người đang tới lượt ở root*/ ]}
```
  * `score.kind = "cp"` với `perspective = "root"` (điểm theo bên đang đi tại root, đơn vị centipawn). `score.kind = "mate"`: `value > 0` = bên root chiếu hết sau `value` nước của chính nó (mate in `value`), `value < 0` = bên root bị chiếu hết sau `|value|` nước của mình. Các test trong Phần 6 đếm độ dài chiếu hết theo **ply** (số nước của cả hai bên): PDF ghi "3 nước" = Xanh–Cam–Xanh = 3 ply. Test kiểm bằng độ dài `pv`, không phụ thuộc cách đặt `value`. *(Ngữ nghĩa `value` chưa xác nhận với GUI, xem Phần 7.)*
  * `pv` phải là chuỗi nước **hợp lệ liên tiếp** (replay được bằng `rules.py`); với điểm `mate`, `pv` phải kết thúc bằng thế mà bên tới lượt không còn nước đi hợp lệ.
  * Có thể phát `info id N string64 <b64url JSON {"message":"..."}>` để báo thông điệp cho GUI.
* **`bestmove id N move64 <b64url của JSON clp.move/1>`** hoặc **`bestmove id N none`** (không còn nước hợp lệ). Nước phải thuộc tập `legal_moves` của state, và là `pv[0]` của hạng 1.
* Bắt buộc: dù `movetime` rất nhỏ (kể cả 1 ms) vẫn trả về **ít nhất một nước hợp lệ** (độ sâu 1 hoàn tất không kiểm tra thời gian).

---

## PHẦN 4. KIẾN TRÚC ENGINE (ALPHA-BETA + QUIESCENCE + HCE)

### 4.1. Biểu diễn bộ nhớ (hiệu năng cao, không cấp phát động trong search)
```cpp
struct Piece { uint8_t type:3, owner:1, facing:2; };     // 1 byte
struct Cell  { uint8_t size; Piece stack[38]; };          // ~40 byte
struct Position {
  Cell cell[81];
  uint8_t prisoners[2][8];        // đếm theo loại quân, chỉ số theo PieceType
  uint8_t reserve[2];             // bitmask: bit0 = LONG, bit1 = PHUONG
  uint8_t king_sq[2];             // vị trí 2 Vương
  uint8_t side_to_move;           // 0 = CAM, 1 = XANH
  uint64_t zobrist;
};
```
* **Zobrist:** `zobrist_cell[depth_in_stack][sq][piece_code]` với `piece_code = (owner * 8 + type) * 4 + facing` (**64 mã**, chỉ dùng 46 mã vì quân không xoay luôn có facing chuẩn D-001), `depth_in_stack ∈ 0..37`. Cộng thêm `zobrist_hand[owner][is_prisoner][type][count]`, `zobrist_reserve[owner][mask]`, `zobrist_side`. Cập nhật **tăng dần** (XOR) khi apply/undo.
* `UndoState` lưu đủ để hoàn tác mọi loại nước: cả stack nguồn, cả stack đích bị bắt (mảng cố định `captured[38]`), `prisoners`, `reserve`, hash cũ, `king_sq`.
* Hợp lệ hóa nước đi: sinh nước giả-hợp-lệ rồi lọc bằng "Vương người đi có bị tấn công sau nước đi không". `is_attacked(sq, by_side)` quét ngược theo từng loại quân (step/jump/slide), **không** bằng cách duyệt 81 ô trong đường nóng.

### 4.2. Phát hiện lặp thế (Thiên Nhật Thủ) và điểm kết thúc
* Bảng đếm khởi tạo từ `repetition.entries` của state (mục 3.2): mỗi `position` được băm Zobrist và cộng `count`. Khóa tính theo **multiset** của `prisoners` (xem rủi ro R-2, Phần 7). Trong cây tìm kiếm duy trì thêm bảng đếm theo đường đi.
* Khi một nước đưa vị trí đến đếm `>= 3`: bên vừa đi thua, điểm `-(SCORE_MATE - (ply + 1))` từ góc nhìn bên vừa đi (nhất quán với test), và xét **trước** chiếu hết/PAT.
* Không còn nước hợp lệ: `-(SCORE_MATE - ply)` cho bên tới lượt (chiếu hết **và** PAT đều thua). `SCORE_MATE = 30000`, điểm `|v| ≥ SCORE_MATE - 128` được xuất ra dạng `kind = "mate"`.
* Với nút đã lặp 2 lần trên đường đi hiện tại khi đang cân nhắc, TT không được dùng để cắt nhánh (kết quả phụ thuộc đường đi): chỉ dùng TT khi đếm lặp của nút = 1.

### 4.3. Lượng giá tĩnh (HCE), điểm theo bên đang đi
1. **Giá trị vật chất (khởi điểm, sẽ tinh chỉnh bằng ván thật):** `BINH 100, XA 280, SI 320, KI 360, TUONG 420, PHUONG 520, LONG 600`. *(Kị và Tướng thực chiến rất mạnh nên đặt cao hơn Xa/Sĩ; bảng v1 để Kị 260 là quá thấp.)* Giá trị Đội = tổng giá trị các quân cộng thưởng cộng hưởng (mục 3).
2. **Tù binh/kho thả:** quân trong `prisoners` của mình = `0.9 ×` giá trị quân đó (có thể thả bất cứ lúc nào), của đối phương trừ tương ứng.
3. **Cộng hưởng Đội:** +20 cp nếu quân thứ 2 của Đội là quân trượt (LONG, PHUONG, XA) đang hướng tia về phía Vương địch.
4. **Treo quân (hanging):** phạt `0.75 × V_stack` cho mỗi ô của bên **không** nắm lượt mà bị bắt được bởi quân giá trị nhỏ hơn nhiều hoặc không có quân bảo vệ (dùng `stack_see_estimate`, mục 4.4).
5. **An toàn Vương:** phạt mỗi ô quanh Vương (và các ô chiếu thẳng) bị đối phương kiểm soát hoặc bỏ trống khi đối phương có `prisoners ≥ 2`: `-25 cp × số_tù_binh_địch`.
6. **Hướng quân:** thưởng nhỏ khi Binh/Xa/Sĩ/Tướng/Kị hướng về phía địch có thể ăn; phạt nhẹ quay lưng.
7. **Đối xứng bắt buộc:** `eval(P) == eval(mirror_horizontal(P))` và `eval(P) == -eval(flip_colors_180(P))` (tự động hóa trong test đơn vị; mirror đổi facing `1 ↔ 3`, flip_colors đổi màu, quay bàn 180° và cộng 2 vào facing của quân xoay được).

### 4.4. Stack-SEE ước lượng (`stack_see_estimate`), dùng cho sắp xếp nước đi và lọc Quiescence
Cho nước ăn quân `m` vào ô `dst` có đội địch với giá trị `V(dst)`:
```
if dst không được bảo vệ (sau nước đi, địch không có nước ăn lại dst):   gain = V(dst)
else if m.kind == single:                                               gain = V(dst) - V(top của nguồn)
else /* m.kind == team */:                                              gain = V(dst) - V(cả Đội nguồn)
```
Nước đi phù hợp Quiescence khi `gain >= 0` (hoặc đang bị chiếu: sinh mọi nước thoát chiếu).

### 4.5. Tìm kiếm
* **PVS (Negamax + alpha-beta)**, độ sâu lặp dần (iterative deepening) có aspiration window. **Transposition Table** 2 tầng (mặc định 64 MB, tùy chọn `Hash`).
* **Sắp xếp nước đi:** (1) nước từ TT, (2) ăn quân theo `10 × V(dst) - V(attacker)` kèm `gain` SEE, (3) nước chiếu, (4) killer moves, (5) history heuristic; **nước thả tù binh** chấm theo `V(quân thả)` và độ gần Vương địch.
* **Check Extension** (+1 ply khi nước đi gây chiếu). **Null-move** chỉ dùng khi bên đi còn ≥ 3 quân không phải Binh/Vương và không bị chiếu. **LMR** cho nước tĩnh muộn. **Futility/razoring** thận trọng ở độ sâu nhỏ.
* **Quiescence Search:** `stand_pat` + nước ăn quân có `gain >= 0` + (ở `qply == 0`) nước chiếu; nếu bị chiếu thì xét mọi nước thoát chiếu. Quiescence phải giữ được các thế "đang bị đe dọa" (treo quân) thông qua phạt `hanging` ở mục 4.3.
* **Tìm chiếu hết:** khi có nước chiếu ép (đối phương chỉ còn ≤ 3 nước hợp lệ) mở rộng thêm; trả điểm `mate` chính xác. Phải tìm được chiếu hết bằng thả tù binh, xoay quân tại chỗ và tách Đội.
* **MultiPV:** chạy `K` lần root, loại trừ các nước đầu đã chọn, phát `info` cho từng hạng mỗi độ sâu hoàn tất.
* **Thời gian:** `movetime` là hạn cứng (kiểm tra mỗi ~1024 nút); hoàn tất độ sâu 1 trước khi kiểm tra thời gian. `stop` phải phản hồi ≤ 200 ms. `Depth`/`Nodes` là giới hạn thêm.
* **Chế độ bot:** nước trả về = `pv[0]` hạng 1. Có thể thêm tùy chọn nhiễu nhẹ ở root để bot không lặp ván (mặc định tắt, tuyệt đối không làm bể test).

---

## PHẦN 5. CẤU TRÚC DỰ ÁN

```
HomeiEngine/
├── CMakeLists.txt                 (C++20, -O3 -march=x86-64-v3, mục tiêu x64 Windows + Linux)
├── src/
│   ├── types.h                    # Piece, Cell, Position, hằng số, SCORE_MATE
│   ├── zobrist.h/.cpp
│   ├── geometry.h/.cpp            # rotate_cw, bảng vector, bảng tia
│   ├── attack.h/.cpp
│   ├── movegen.h/.cpp             # generate/legal/apply/undo, thứ tự sinh như rules.py
│   ├── notation.h/.cpp            # canonical_notation (mục 2.5)
│   ├── adjudicate.h/.cpp          # lặp thế, chiếu hết, PAT (mục 2.6)
│   ├── lppn.h/.cpp                # LPPN <-> Position
│   ├── state_json.h/.cpp          # clp.state/1, clp.move/1, repetition entries
│   ├── json.h/.cpp                # parser/printer JSON tối giản
│   ├── base64url.h/.cpp
│   ├── eval.h/.cpp
│   ├── see.h/.cpp
│   ├── tt.h/.cpp
│   ├── search.h/.cpp              # PVS, quiescence, MultiPV, thời gian, stop
│   └── clp_ep.cpp                 # vòng lặp lệnh CLP-EP v1 + lệnh mở rộng (main)
└── tests/                         # harness Python, chạy bằng pytest
    ├── clp_ep_client.py           # client CLP-EP tối giản (subprocess) + dựng state từ LPPN
    ├── positions.py               # các thế trong Phần 6
    ├── oracle.py                  # (tùy chọn) oracle vật chất, chỉ để sinh lại tập đáp án S4–S7
    ├── test_parity.py             # Test 0
    ├── test_protocol.py           # Test P
    ├── test_tactical_suite.py     # Test T, M
    └── test_synthetic.py          # Test S1–S8
```
`tests/` đặt cạnh các file `rules.py`, `board.py`, `pieces.py`, `lppn.py`, `core_bridge.py` (import như một package Python) để làm **oracle luật**.


---

## PHẦN 6. BỘ KIỂM THỬ (CÁC THẾ VÀ ĐÁP ÁN TRONG PDF, KÈM TEST BỔ SUNG)

### 6.0. Hạ tầng và quy ước PASS
* Toàn bộ test là **pytest** chạy engine như tiến trình con qua CLP-EP v1 (đúng như GUI), dùng `rules.py` / `lppn.py` / `core_bridge.py` làm oracle luật. `tests/clp_ep_client.py` phải: (a) bắt tay (`clp 1` → `clpok` → `isready` → `readyok`); (b) dựng state từ LPPN theo cách `core_bridge.importLppnStrict` làm (`position` + `repetition.entries = [{count:1, position}]`, `ply` lấy từ LPPN, `result` qua adjudicate), hoặc từ LPPN gốc + danh sách nước (dùng `rules.apply_move` và cộng bộ đếm lặp thế như `_apply_move_to_state`); (c) gửi `position state64`, `setoption`, `go id N movetime MS`; (d) giải mã `info64` và `move64`.
* **Mọi nước engine trả về phải thuộc `rules.legal_moves`** (so khớp `kind, src, dst, ptype, facing, rotate_after`) và có `notation == Move.canonical_notation(board)`. Vi phạm = FAIL ngay.
* **Các kiểu kiểm tra:**
  * **EXPECT:** `bestmove.notation` thuộc danh sách `expected` của thế. Danh sách này đã được **mở sẵn thành ký hiệu chuẩn** (ví dụ đáp án `Xf4` không ghi hướng đã thành `Xf4v, Xf4<, Xf4>`), nên so sánh là **bằng nhau tuyệt đối từng ký tự**.
  * **MATE(N ply):** điểm `kind = "mate"`, `value > 0`, **PV phát lại bằng `rules.py` kết thúc ở thế mà bên bị chiếu không còn nước hợp lệ**, độ dài PV ≤ N ply, và `bestmove == pv[0]`. Không bắt buộc đúng nước đầu như đáp án (có thể có nhiều lời giải); đáp án chỉ là gợi ý đã kiểm chứng.
  * **MATE1 / SET / NOT:** xem từng test bổ sung ở 6.4. `MATE1`: nước thuộc tập chiếu hết 1 nước. `SET`: thuộc tập cho sẵn. `NOT`: khác nước cho sẵn.
* **Ngân sách:** mỗi thế `movetime = 5000 ms` (MATE: `15000 ms`), `Depth = 32`, `Nodes = 50 000 000`, `MultiPV = 1`, i7-8700K hoặc tương đương. Thế FAIL ở 5 s phải chạy lại ở 30 s và báo cả hai kết quả (để phân biệt "chậm" với "sai").
* **Cổng hoàn thành:** Test 0 và Test P đạt 100%. **Tất cả thế EXPECT (T01–T21) đạt.** MATE T23, T24, T25, T27 đạt. T22, T26: engine phải tìm được chiếu hết ≤ 9 ply; nếu không, báo **UNVERIFIED** (không tính FAIL, vì chưa có lời giải xác nhận độc lập). S1–S8 đạt 100%.

### 6.1. Test 0 — Parity luật (cổng của Bước 2)
1. **Thế xuất phát:** `legal` cho CAM là **18** nước (thả `L`/`P` xuống hàng `b`, mỗi loại 9 ô), sau 1 nước của CAM, XANH cũng có **18**; `perft 1 = 18`, `perft 2 = 324`. Sau khi cả hai bên đã thả hết `reserve`, `legal` khớp `rules.legal_moves`.
2. **200 thế ngẫu nhiên hợp lệ** (sinh bằng `rules.py` chơi ngẫu nhiên 0–150 nước; phải gồm thế có Đội ≥ 3 quân, thế đang bị chiếu, thế có tù binh mọi loại): với mỗi thế, **tập `legal` của engine == tập của `rules.legal_moves`** (so theo `notation` **và** theo 6 trường cấu trúc), số lượng bằng nhau.
3. **Apply parity:** với mỗi nước hợp lệ của 50 thế đầu, thế sau nước đi từ engine (`lppn` sau `position` + lệnh `apply`, hoặc so `positionok` hash khi phát lại) trùng LPPN do `rules.apply_move` + `to_lppn` tạo ra.
4. **Adjudicate parity:** dựng 3 kịch bản (lặp lần 3, chiếu hết, PAT) và 1 kịch bản "lặp lần 3 **và** đối thủ hết nước": engine phải trả đúng `repetition3` thắng trước, `checkmate`/`pat` thua cho bên tới lượt.
5. **LPPN round-trip:** `to_lppn(from_lppn(x)) == x` cho mọi chuỗi trong Phần 6; chuỗi hỏng (thiếu ô, hàng 10 ô, Đội 1 quân, `reserve` trùng, Vương trong tay) phải bị từ chối với `error ...` và engine không bị crash.
6. **Notation parity:** `notation` của mọi nước hợp lệ ở mục 2 khớp `Move.canonical_notation`, gồm các ca nhập nhằng (`.src`), nước `rotate`, nước `team`, nước `drop`.

### 6.2. Test P — Giao thức (cổng của Bước 3)
1. **Bắt tay:** đủ các dòng ở mục 3.3; `readyok` sau `isready`; GUI giả lập (`engine_client.EngineClient` có thể dùng trực tiếp) báo `Engine ready (capabilities verified)`.
2. **Trình tự `position` → `positionok ply n` → 3×`setoption` → 3×`optionok` → `go`.** Trả lời sai định dạng = FAIL.
3. **`movetime` nhỏ:** `movetime = 1` vẫn trả `bestmove` hợp lệ. `movetime = 1000` kết thúc trong `1000 ± 150 ms`.
4. **Giới hạn:** `Depth = 3` dừng ở độ sâu 3 (info cuối `depth ≤ 3`); `Nodes = 20000` dừng ở ≤ 1.05× số nút.
5. **MultiPV:** `MultiPV = 3` phát `info` với `multipv = 1, 2, 3` khác nhau đôi một về nước đầu; hạng 1 điểm ≥ hạng 2 ≥ hạng 3.
6. **`infinite`:** `searchstarted id N` ngay; độ sâu `info` tăng đều; `stop id N` trả `bestmove id N move64 ...` trong ≤ 200 ms; sau đó `go` mới (id mới) chạy bình thường. Id lạc (`stop id 999`) bị bỏ qua, không crash.
7. **State terminal / không có nước:** `result.status = "terminal"` hoặc thế chiếu hết → `bestmove id N none`.
8. **Lịch sử lặp:** state có `repetition.entries` với `count = 2` cho thế sẽ lặp: engine không chọn nước đi lặp lần 3 khi có nước khác không thua (xem S8).
9. **Độ bền:** dòng 2 MiB không crash; JSON thiếu khóa → `error ...`; `quit` thoát sạch trong 1 s.

### 6.3. Test T/M — Các thế trong PDF kèm đáp án của bạn
**Quy trình cho mỗi thế (áp dụng cho mọi AI/người đọc file này):** chuỗi trong khối `text` là **mã LPPN để nhập vào GUI** (menu `Tệp (File) → LPPN (Thế cờ / SFEN)`, dán vào ô LPPN rồi bấm nạp), sau đó **chạy engine ở thế đó** và so nước engine trả về với đáp án. Trong test tự động, harness nạp **đúng chuỗi này** theo cách GUI nhập LPPN (`importLppnStrict`), gửi `position state64` cho engine, rồi so kết quả.

**Cách đọc "Đáp án gốc của bạn":** một chuỗi nước viết kèm tên bên đi, theo thứ tự lần lượt, ví dụ `Lg3 Cam, Se3 Xanh, Lg3 Cam` nghĩa là Cam đi `Lg3`, rồi Xanh đi `Se3`, rồi Cam đi `Lg3`. Dấu `/` hoặc `hoặc` nghĩa là các lựa chọn thay thế cho cùng một bước. Test chỉ so **nước đầu tiên của bên đang tới lượt** (cùng các lựa chọn thay thế của nó) với `expected`; các nước sau chỉ là lời giải thích (riêng chiếu hết thì kiểm cả chuỗi bằng PV-replay).

Mỗi mục: LPPN (đã kiểm `strict`), đáp án gốc bạn nhập, và danh sách `expected` đã chuẩn hóa. Mọi `expected` đều **hợp lệ theo `rules.py`**. Mọi đáp án chiếu hết đều **đã chạy lại bằng `rules.py`** (xem ghi chú từng thế).

#### T01 — V. Ăn quân › Ăn quân không được bảo vệ (Hình 1)
*Cam vừa đi Se6<. Tới lượt Xanh.* · Lượt: **XANH**
**Nhập vào GUI rồi chạy engine** (`Tệp (File) → LPPN (Thế cờ / SFEN)` → dán chuỗi dưới đây vào ô LPPN, hoặc `Dán mã LPPN từ Clipboard…` / Ctrl+Shift+V; sau đó chạy engine ở thế này):
```text
lppn1 X2K2S2T2V2T21K2X2/9/B2B2B2B2B21B2B2B2/4L21[B2P2]2/3S35/9/b0[b0l0]b0b0b0b0b0b0b0/3s0p0s0t02/x0k01t01v01k0x0 x -/- -/- 15
```
* Đáp án gốc của bạn: `Lc8, [L]c8`
* Kiểu **EXPECT**: `expected = [Lc8, [L]c8]`

#### T02 — V. Ăn quân › Ăn quân không được bảo vệ (Hình 2)
*Cam vừa đi [B]e3. Đến lượt Xanh.* · Lượt: **XANH**
**Nhập vào GUI rồi chạy engine** (`Tệp (File) → LPPN (Thế cờ / SFEN)` → dán chuỗi dưới đây vào ô LPPN, hoặc `Dán mã LPPN từ Clipboard…` / Ctrl+Shift+V; sau đó chạy engine ở thế này):
```text
lppn1 X2K21T2V24/1S22T14/B2B2B2B2B21K1B2[B2X2]/4L24/3S32[B2P2B2]2/9/b0[b0l0]b0[b0p0]b0b0b0b0b0/3s02t02/x0k01t0k01s0v0x0 x -/- -/- 27
```
* Đáp án gốc của bạn: `Pc2, [P]c2`
* Kiểu **EXPECT**: `expected = [Pc2, [P]c2]`

#### T03 — V. Ăn quân › Ăn quân không được bảo vệ (Hình 3, lựa chọn ăn quân nhiều điểm)
*Cam vừa đi Bf3, đến lượt Xanh.* · Lượt: **XANH**
**Nhập vào GUI rồi chạy engine** (`Tệp (File) → LPPN (Thế cờ / SFEN)` → dán chuỗi dưới đây vào ô LPPN, hoặc `Dán mã LPPN từ Clipboard…` / Ctrl+Shift+V; sau đó chạy engine ở thế này):
```text
lppn1 X2K21T2V24/1S22T11p02/B2B2B2B2B21K11[B2X2]/4S22L21/6[B2P2]2/6B22/b0[b0l0]b0b0b0[b0k0]b0b0b0/3s02t02/x0k01t02s0v0x0 x -/- -/B 33
```
* Đáp án gốc của bạn: `Ke3<, [K]e3<, Ke3, [K]e3, Ke3>, [K]e3> (đề xuất tốt nhất: [K]e3<)`
* Kiểu **EXPECT**: `expected = [Ke3<, [K]e3<, Ke3^, Ke3>, [K]e3^, [K]e3>]`

#### T04 — V. Ăn quân › Lấy 1 quân đổi 2 quân (Hình 1)
*Cam đi P*d5, đến lượt Xanh.* · Lượt: **XANH**
**Nhập vào GUI rồi chạy engine** (`Tệp (File) → LPPN (Thế cờ / SFEN)` → dán chuỗi dưới đây vào ô LPPN, hoặc `Dán mã LPPN từ Clipboard…` / Ctrl+Shift+V; sau đó chạy engine ở thế này):
```text
lppn1 X2K21T2V24/1S22T14/B2B2B2B2B24/4[S2P2]2L2p0/6k32/6b02/b0[b0l0]b0b0b0b0t0b0b0/3s05/x0k01t02s0v0x0 x -/- B/KXBBB 45
```
* Đáp án gốc của bạn: `Kd5v`
* Kiểu **EXPECT**: `expected = [Kd5v]`

#### T05 — V. Ăn quân › Lấy 1 quân đổi 2 quân (Hình 2)
*Cam có thế lấy 1 đổi 2, hãy tìm nước đi tốt nhất.* · Lượt: **CAM**
**Nhập vào GUI rồi chạy engine** (`Tệp (File) → LPPN (Thế cờ / SFEN)` → dán chuỗi dưới đây vào ô LPPN, hoặc `Dán mã LPPN từ Clipboard…` / Ctrl+Shift+V; sau đó chạy engine ở thế này):
```text
lppn1 1K2S31B2T2V2K21/4T22S21/B22B2B2B2B2B21/9/2P26/b01k01p01b02/1[b0l0][b0k3]b0b01b0v0b3/3s01L2x32/x04S1T02 c -/- -/TXXBBB 82
```
* Đáp án gốc của bạn: `Sh3^`
* Kiểu **EXPECT**: `expected = [Sh3^]`

#### T06 — V. Ăn quân › Đổi quân ít điểm lấy quân cao điểm (Hình 1)
*Cam có cơ hội đổi quân ít điểm lấy quân cao điểm.* · Lượt: **CAM**
**Nhập vào GUI rồi chạy engine** (`Tệp (File) → LPPN (Thế cờ / SFEN)` → dán chuỗi dưới đây vào ô LPPN, hoặc `Dán mã LPPN từ Clipboard…` / Ctrl+Shift+V; sau đó chạy engine ở thế này):
```text
lppn1 X2V2T2K0[B2K3]4/1S21T23L21/B2B2B2B2B3[B2S2]B32/5B23/5X23/1b01l01k0p02/b0b0b0b0b01b1b0b0/2[s0t0]1t12s01/[x0x0]1v06 c -/- K/P 58
```
* Đáp án gốc của bạn: `Xf4, Xf4<, Xf4>`
* Kiểu **EXPECT**: `expected = [Xf4v, Xf4<, Xf4>]`

#### T07 — V. Ăn quân › Đổi quân ít điểm lấy quân cao điểm (Hình 2)
*Cam có cơ hội đổi 1 quân ít điểm lấy 1 quân cao điểm.* · Lượt: **CAM**
**Nhập vào GUI rồi chạy engine** (`Tệp (File) → LPPN (Thế cờ / SFEN)` → dán chuỗi dưới đây vào ô LPPN, hoặc `Dán mã LPPN từ Clipboard…` / Ctrl+Shift+V; sau đó chạy engine ở thế này):
```text
lppn1 X2V2T21K22K21/1S21T1P22S21/B2B2B2B2B2B2B2B21/7X2B2/3b05/5k23/b0b0b0b0b01b02/1v0p01t02l01/x0s11t0b11s0k01 c -/- LB/X 48
```
* Đáp án gốc của bạn: `Xh2, Xh2<`
* Kiểu **EXPECT**: `expected = [Xh2v, Xh2<, Xh2>]`

#### T08 — V. Ăn quân › Bắt đôi (Hình 1)
*Xanh đang có hướng bắt đôi.* · Lượt: **XANH**
**Nhập vào GUI rồi chạy engine** (`Tệp (File) → LPPN (Thế cờ / SFEN)` → dán chuỗi dưới đây vào ô LPPN, hoặc `Dán mã LPPN từ Clipboard…` / Ctrl+Shift+V; sau đó chạy engine ở thế này):
```text
lppn1 X2V2T2K0B24/1S21T25/B2B2B2B2B3B2[B3L2]2/2P2K31B23/9/b0b02b04/1b0b0b01s0B31b0/2s0t1x12s01/[x0x0]1v02l03 x -/- TKB/KP 87
```
* Đáp án gốc của bạn: `P*e1`
* Kiểu **EXPECT**: `expected = [P*e1]`

#### T09 — V. Ăn quân › Bắt đôi (Hình 2)
*Tìm nước đi bắt đôi cho Cam.* · Lượt: **CAM**
**Nhập vào GUI rồi chạy engine** (`Tệp (File) → LPPN (Thế cờ / SFEN)` → dán chuỗi dưới đây vào ô LPPN, hoặc `Dán mã LPPN từ Clipboard…` / Ctrl+Shift+V; sau đó chạy engine ở thế này):
```text
lppn1 X2V2T21K22K21/1S21T1P22S21/B2B2B2B2B2B2B2B21/8B2/3b05/5k23/b0b0b0b0b01b02/1v0p01t02s01/x0s11t0b12k01 c -/- LLB/XX 50
```
* Đáp án gốc của bạn: `Lf3`
* Kiểu **EXPECT**: `expected = [L*f3]`

#### T10 — V. Ăn quân › Nâng cao › Đổi quân lớn lấy quân nhỏ, xong lấy quân nhỏ thả để bắt lại quân lớn
*Cam đi Ld2, đến lượt Xanh.* · Lượt: **XANH**
**Nhập vào GUI rồi chạy engine** (`Tệp (File) → LPPN (Thế cờ / SFEN)` → dán chuỗi dưới đây vào ô LPPN, hoặc `Dán mã LPPN từ Clipboard…` / Ctrl+Shift+V; sau đó chạy engine ở thế này):
```text
lppn1 X2K21T2V24/1S22T11p02/B2B2B2B2B21K1p01/4S22L2[B2X2]/6k32/6b02/b0[b0l0]b0b0b0b0t0b0b0/3s05/x0k01t02s0v0x0 x -/- B/BB 41
```
* Đáp án gốc của bạn: `Pd1 Xanh; nếu Cam đi Kd1v thì Xanh Pd1; nếu Cam tiếp tục Ld1 thì Xanh K*b2< (chiếu bắt Long)`
* Kiểu **EXPECT**: `expected = [Pd1]`

#### T11 — V. Ăn quân › Nâng cao › Xong mở đường để bắt quân lớn (Hình 1)
*Cam đi Se5. Xanh thí quân để lấy 1 đổi 2.* · Lượt: **XANH**
**Nhập vào GUI rồi chạy engine** (`Tệp (File) → LPPN (Thế cờ / SFEN)` → dán chuỗi dưới đây vào ô LPPN, hoặc `Dán mã LPPN từ Clipboard…` / Ctrl+Shift+V; sau đó chạy engine ở thế này):
```text
lppn1 X2K21T2K2T22X2/2V2S2P22L21/B2B2B2B2B2B2B2B2B2/9/1b02S24/4[b0p0]4/b01[b0k0]1b0b0b0b0b0/3l05/x01s0t0v0t0s0k0x0 x -/- -/- 21
```
* Đáp án gốc của bạn: `Pc8 Xanh (chiếu), Vc8 Cam, Be5 Xanh  (bản gốc ghi `P8c`, `V8c`; đã đổi sang ký hiệu chuẩn)`
* Kiểu **EXPECT**: `expected = [Pc8]`

#### T12 — V. Ăn quân › Nâng cao › Xong mở đường để bắt quân lớn (Hình 2)
*Xanh đe dọa lấy Sĩ ở g3 bắt Kị ở f4. Lấy 1 ăn 2, đưa Long vào thế đẹp.* · Lượt: **CAM**
**Nhập vào GUI rồi chạy engine** (`Tệp (File) → LPPN (Thế cờ / SFEN)` → dán chuỗi dưới đây vào ô LPPN, hoặc `Dán mã LPPN từ Clipboard…` / Ctrl+Shift+V; sau đó chạy engine ở thế này):
```text
lppn1 X2V2T2K0B24/1S21T23L21/B2B2B2B2B3B2B32/3K31B23/6S22/b0b03K03/1b0b0b0b0p0[b1s0]b0b0/2s0t0t14/[x0x0]1v02l03 c -/- K/XP 70
```
* Đáp án gốc của bạn: `Kh6> Cam, Vh6 (hoặc Th6>) Xanh, Lg2 Cam`
* Kiểu **EXPECT**: `expected = [Kh6>]`

#### T13 — V. Ăn quân › Nâng cao › Xong mở đường để bắt quân lớn (Hình 3)
*Tìm nước đi tốt nhất cho Cam.* · Lượt: **CAM**
**Nhập vào GUI rồi chạy engine** (`Tệp (File) → LPPN (Thế cờ / SFEN)` → dán chuỗi dưới đây vào ô LPPN, hoặc `Dán mã LPPN từ Clipboard…` / Ctrl+Shift+V; sau đó chạy engine ở thế này):
```text
lppn1 X2V2T2K0B24/1S21T25/B2B2B2B2B3B2B32/3K31B23/6S22/b0b04s02/1b0b0b0b0p0b1L2b0/2s0t15/[x0x0]1v02l03 c -/- TKB/KXP 74
```
* Đáp án gốc của bạn: `Lg3 Cam, Se3 Xanh, Lg3 Cam`
* Kiểu **EXPECT**: `expected = [Lg3]`

#### T14 — VI. Bảo vệ quân › Quân bị nhắm mà không có quân giữ (Hình 1)
*Cam đi Ke5v, Xanh tìm quân bị đe dọa để bảo vệ.* · Lượt: **XANH**
**Nhập vào GUI rồi chạy engine** (`Tệp (File) → LPPN (Thế cờ / SFEN)` → dán chuỗi dưới đây vào ô LPPN, hoặc `Dán mã LPPN từ Clipboard…` / Ctrl+Shift+V; sau đó chạy engine ở thế này):
```text
lppn1 X21S22T22X2/1L2P21T2V21S21/B21B2B2B2B2[B2K2]B2B2/B28/4K04/9/b0b0b0b0b0b0b0b0b0/1l02p02s01/x0k0s0t0k01t0v0x0 x -/- -/- 19
```
* Đáp án gốc của bạn: `Sh6/Th6/Th5`
* Kiểu **EXPECT**: `expected = [Sh6^, Sh6>, Sh6<, Th6^, Th6>, Th5^, Th5>, Th5<]`

#### T15 — VI. Bảo vệ quân › Quân bị nhắm mà không có quân giữ (Hình 2)
*Cam đi Kd4v nhằm bắt Binh, Xanh bảo vệ Binh.* · Lượt: **XANH**
**Nhập vào GUI rồi chạy engine** (`Tệp (File) → LPPN (Thế cờ / SFEN)` → dán chuỗi dưới đây vào ô LPPN, hoặc `Dán mã LPPN từ Clipboard…` / Ctrl+Shift+V; sau đó chạy engine ở thế này):
```text
lppn1 X21S22T22X2/1L22T2V21S21/2B2B2B2[B2K3]B2B2B2/2P22K03/[B2B2]8/3b0b04/1[b0b0]b0p01b0b0b0b0/1l01s01t01s01/x0k02k0t01v0x0 x -/- -/- 37
```
* Đáp án gốc của bạn: `Sg5/Bf7> hoặc có thể Bf5<`
* Kiểu **EXPECT**: `expected = [Sg5^, Sg5>, Sg5<, Bf7>, Bf5<.f5]`

#### T16 — VI. Bảo vệ quân › Tăng quân bảo vệ, tránh 2 đổi 1 (Hình 1)
*Cam đi Ke4, đe dọa dồn quân. Xanh tránh 2 đổi 1.* · Lượt: **XANH**
**Nhập vào GUI rồi chạy engine** (`Tệp (File) → LPPN (Thế cờ / SFEN)` → dán chuỗi dưới đây vào ô LPPN, hoặc `Dán mã LPPN từ Clipboard…` / Ctrl+Shift+V; sau đó chạy engine ở thế này):
```text
lppn1 X21S22T22X2/1L2P21T2V21S21/B21B2B2B2B2B2B2B2/B28/4K0K23/9/b0b0b0b0b0b0b0b0b0/1l01s0p02s01/x0k01t0k01t0v0x0 x -/- -/- 21
```
* Đáp án gốc của bạn: `Th5/Th6`
* Kiểu **EXPECT**: `expected = [Th5^, Th5>, Th5<, Th6^, Th6>, Th6<]`

#### T17 — VI. Bảo vệ quân › Tăng quân bảo vệ (Hình 2)
*Cam đi Pe6, đe dọa dồn quân. Xanh tìm quân bị đe dọa và bảo vệ nó.* · Lượt: **XANH**
**Nhập vào GUI rồi chạy engine** (`Tệp (File) → LPPN (Thế cờ / SFEN)` → dán chuỗi dưới đây vào ô LPPN, hoặc `Dán mã LPPN từ Clipboard…` / Ctrl+Shift+V; sau đó chạy engine ở thế này):
```text
lppn1 X21S22T22X2/1L22T2V21S21/B21B2B2B2B2B2B2B2/B28/3P2K0K23/9/b0b0b0b0b0b0b0b0b0/1l01s0p0t01s01/x0k02[k0t0]2v0x0 x -/- -/- 27
```
* Đáp án gốc của bạn: `Bg9>/Bg7<`
* Kiểu **EXPECT**: `expected = [Bg9>.g9, Bg7<.g7]`

#### T18 — VI. Bảo vệ quân › Chạy quân (Hình 1)
*Cam đi Kc3, Xanh tìm quân bị đe dọa bắt và chạy.* · Lượt: **XANH**
**Nhập vào GUI rồi chạy engine** (`Tệp (File) → LPPN (Thế cờ / SFEN)` → dán chuỗi dưới đây vào ô LPPN, hoặc `Dán mã LPPN từ Clipboard…` / Ctrl+Shift+V; sau đó chạy engine ở thế này):
```text
lppn1 X2K21T21T2S21X2/2V2S2P22L21/B2B2B2B2B2B2[B2K2]B2B2/9/5[b0p0]3/9/b0b0[b0k0]1b0b0b0b0b0/3l05/x01s0t0v0t0s0k0x0 x -/- -/- 11
```
* Đáp án gốc của bạn: `Pf4/[P]f4/Pf3/[P]f3/Pd3`
* Kiểu **EXPECT**: `expected = [Pf3, [P]f3, Pd3, Pf5, [P]f5]`

#### T19 — VI. Bảo vệ quân › Chạy quân (Hình 2)
*Cam đi Ka5, Xanh tìm quân bị đe dọa bắt và chạy.* · Lượt: **XANH**
**Nhập vào GUI rồi chạy engine** (`Tệp (File) → LPPN (Thế cờ / SFEN)` → dán chuỗi dưới đây vào ô LPPN, hoặc `Dán mã LPPN từ Clipboard…` / Ctrl+Shift+V; sau đó chạy engine ở thế này):
```text
lppn1 X2K21T2K2T2S21X2/2V2S2P22L21/B2B2B2B2B2B2B2B2B2/6[b0p0]2/9/9/b0b0[b0k0]1b0b0b0b0b0/3l05/x01s0t0v0t0s0k0x0 x -/- -/- 13
```
* Đáp án gốc của bạn: `Pf4/[P]f4/Pe4/[P]e4/Pe2/[P]e2/Pf1/[P]f1`
* Kiểu **EXPECT**: `expected = [Pe4, [P]e4, Pe2, [P]e2, Pf1, [P]f1, Pf5, [P]f5]`

#### T20 — VI. Bảo vệ quân › Chạy quân (Hình 3)
*Xanh đi B*d4, dọa bắt quân Cam. Cam tìm quân bị đe dọa và chạy.* · Lượt: **CAM**
**Nhập vào GUI rồi chạy engine** (`Tệp (File) → LPPN (Thế cờ / SFEN)` → dán chuỗi dưới đây vào ô LPPN, hoặc `Dán mã LPPN từ Clipboard…` / Ctrl+Shift+V; sau đó chạy engine ở thế này):
```text
lppn1 X2K21T2K2T22X2/P21V2S25/B21B21B2[B2P2]B21B2/1b13b03/3b0b02[B2L2]1/4b04/b0k23b0b0b0b0/3l02s02/x01s0t0v0t01k0x0 c -/- B/S 42
```
* Đáp án gốc của bạn: `Pd3/[P]d3/Pb5/[P]b5/Pb3/[P]b3/Pa2/[P]a2`
* Kiểu **EXPECT**: `expected = [Pd3, [P]d3, Pb5, [P]b5, Pb3, [P]b3, Pa2, [P]a2]`

#### T21 — VII. Bảo vệ Vua › Tìm cách bảo vệ Vua (Hình 1)
*Xanh đe dọa K*c8 chiếu bắt Long; nếu sai có thể bị chiếu hết bằng K*c8v Vb8 Ka6>.* · Lượt: **CAM**
**Nhập vào GUI rồi chạy engine** (`Tệp (File) → LPPN (Thế cờ / SFEN)` → dán chuỗi dưới đây vào ô LPPN, hoặc `Dán mã LPPN từ Clipboard…` / Ctrl+Shift+V; sau đó chạy engine ở thế này):
```text
lppn1 X2V2T24K2X2/3T21S13/B2s0B2B2B21B21B2/6B32/1P2L26/9/b03b0b0b0b0b0/4t3v0t0s01/x0k03b01k0x0 c -/- SLPBB/KBB 46
```
* Đáp án gốc của bạn: `B*b8`
* Kiểu **EXPECT**: `expected = [B*b8^, B*b8>, B*b8v, B*b8<]`

#### T22 — VIII. Chiếu hết › 3 nước (Hình 1)
*Tìm chiếu hết trong 3 nước.* · Lượt: **XANH**
**Nhập vào GUI rồi chạy engine** (`Tệp (File) → LPPN (Thế cờ / SFEN)` → dán chuỗi dưới đây vào ô LPPN, hoặc `Dán mã LPPN từ Clipboard…` / Ctrl+Shift+V; sau đó chạy engine ở thế này):
```text
lppn1 X2K21T2V24/1S22T11p02/B2B2B2B2B21K11[B2X2]/4S22L21/6k32/9/b0[b0l0]b0b0b0b0B2b0b0/3s02t02/x0k01t02s0v0x0 x -/- B/PBB 35
```
* Đáp án gốc của bạn: `L*b5 Xanh, Va6/Va7 Cam, T*b6/T*b7 Xanh`
* Kiểu **MATE(3 ply)**.
* Chưa có lời giải xác nhận độc lập (đáp án gốc không khớp LPPN, xem mục 6.3b).

#### T23 — VIII. Chiếu hết › 3 nước (Hình 2)
*Xanh thả Xa Xh6< là sai lầm. Tìm chiếu hết trong 3 nước cho Cam.* · Lượt: **CAM**
**Nhập vào GUI rồi chạy engine** (`Tệp (File) → LPPN (Thế cờ / SFEN)` → dán chuỗi dưới đây vào ô LPPN, hoặc `Dán mã LPPN từ Clipboard…` / Ctrl+Shift+V; sau đó chạy engine ở thế này):
```text
lppn1 X2K22T23X2/P21V26/B21B22[b3p0]B21B2/1[B2L2]7/9/9/b11T22b01b0b0/3x32s02/2v01b0t02x0 c -/- TSSKBBBBBBB/SKKL 104
```
* Đáp án gốc của bạn: `S*h7/T*h7 Cam, Xh7/Xh7^ Xanh, S*h7/T*h7 Cam`
* Kiểu **MATE(3 ply)**.
* Gợi ý đã kiểm chứng bằng `rules.py`: nước mở đầu `S*h7v`, `T*h7^`, `T*h7>`, `T*h7v`.

#### T24 — VIII. Chiếu hết › 5 nước (Hình 1)
*Tìm mate in 5 cho Cam.* · Lượt: **CAM**
**Nhập vào GUI rồi chạy engine** (`Tệp (File) → LPPN (Thế cờ / SFEN)` → dán chuỗi dưới đây vào ô LPPN, hoặc `Dán mã LPPN từ Clipboard…` / Ctrl+Shift+V; sau đó chạy engine ở thế này):
```text
lppn1 1K2S31B2T2V2K21/4T22S21/B22B2[B2P2]B2B2B21/9/6b0v01/b03p01b02/1[b0l0][b0k3]b0b01b01b3/4k31S02/x05T02 c -/- SXB/TXXLB 94
```
* Đáp án gốc của bạn: `X*d2 Cam, Ve1/Vf1 Xanh, S*e2> Cam, Vf2 Xanh, B*f1 Cam`
* Kiểu **MATE(5 ply)**.
* Gợi ý đã kiểm chứng bằng `rules.py`: nước mở đầu `X*d2v`.

#### T25 — VIII. Chiếu hết › 5 nước (Hình 2)
*Tìm mate in 5 cho Cam.* · Lượt: **CAM**
**Nhập vào GUI rồi chạy engine** (`Tệp (File) → LPPN (Thế cờ / SFEN)` → dán chuỗi dưới đây vào ô LPPN, hoặc `Dán mã LPPN từ Clipboard…` / Ctrl+Shift+V; sau đó chạy engine ở thế này):
```text
lppn1 X2V2T2K0B21l02/1S21T21B13/B2B21B21B23/2B2b01B23/4b04/b0b04x02/1K0b0b0b03b0/l0v07/1S31p05 c -/- TTSKXBB/SKXP 154
```
* Đáp án gốc của bạn: `T*i8^ Cam, Vg8 Xanh, Th9^ Cam, Vf7 Xanh, T*e7 Cam`
* Kiểu **MATE(5 ply)**.
* Gợi ý đã kiểm chứng bằng `rules.py`: nước mở đầu `T*i8^`.

#### T26 — VIII. Chiếu hết › chuỗi dài (Hình 1)
*Cam đi Vb5, tìm đường chiếu hết (luôn là nước chiếu).* · Lượt: **XANH**
**Nhập vào GUI rồi chạy engine** (`Tệp (File) → LPPN (Thế cờ / SFEN)` → dán chuỗi dưới đây vào ô LPPN, hoặc `Dán mã LPPN từ Clipboard…` / Ctrl+Shift+V; sau đó chạy engine ở thế này):
```text
lppn1 X2K27/1S22V2B12x3/B21B2B2B24/1B27/9/6b02/b0b0b0b0b0b0t0b0b0/3s05/x0k01t02s0v0x0 x -/- SKLP/TTKLPBBB 59
```
* Đáp án gốc của bạn: `(chưa có đáp án)`
* Kiểu **MATE(bất kỳ ≤ 9 ply)**.
* Chưa có lời giải xác nhận độc lập.

#### T27 — VIII. Chiếu hết › chuỗi dài (Hình 2)
*Cam đã có chiếu hết, tìm chuỗi nước đi để chiếu hết.* · Lượt: **CAM**
**Nhập vào GUI rồi chạy engine** (`Tệp (File) → LPPN (Thế cờ / SFEN)` → dán chuỗi dưới đây vào ô LPPN, hoặc `Dán mã LPPN từ Clipboard…` / Ctrl+Shift+V; sau đó chạy engine ở thế này):
```text
lppn1 X21V21S12l0X2/3T21S13/B2[t0k2]B2B2B21B21B2/1b04B32/2L26/9/b03b0b0b0b0b0/4P2v0t0s01/x0k03b01k0x0 c -/- TSKPBBB/- 68
```
* Đáp án gốc của bạn: `P*i5 Cam; nếu Xanh Vi5 thì Cam T*i6> (hoặc T*i6^), Xanh Vh4, Cam S*i5> (hoặc S*i5^, S*i5<, S*i5v), Xanh Vi3, Cam K*h5>; nếu Xanh Vh5 thì Cam T*h6> (hoặc T*h6v); nếu Xanh Vi3 thì Cam K*h5>`
* Kiểu **MATE(7 ply)**.
* Gợi ý đã kiểm chứng bằng `rules.py`: nước mở đầu `P*i5`.

#### 6.3b. Đối chiếu đáp án của bạn với `rules.py`: các chỗ đã sửa hoặc cần bạn xác nhận
Đáp án khớp hoàn toàn (hợp lệ và tốt nhất theo oracle vật chất, hoặc chiếu hết đã kiểm chứng): T01–T08, T12, T13, T14, T15, T17, T18 (phần `Pf3`, `Pd3`), T19 (phần `Pe4`, `Pe2`, `Pf1`), T20, T21, T23 (M2), T24 (M3), T25 (M4), T27 (M6). Các chỗ còn lại:

| Thế | Đáp án | Vấn đề / cách xử lý trong file này |
|---|---|---|
| T09 (D2) | `Lf3` | Không hợp lệ. Cam có 2 Long trong tù binh (`LLB`) nên đây là nước **thả**. Sửa thành `L*f3` (hợp lệ) |
| T11 (E2) | `P8c`, `V8c` | Ô viết ngược. Sửa thành `Pc8` (chiếu), `Vc8`, `Be5` |
| T10, T11 | Nước đổi/thí quân | Oracle vật chất báo âm (`Pd1` -280, `Pc8` -200) vì không thấy chuỗi bắt lại. Giữ theo đáp án của bạn |
| T13 (E4) | `Lg3 Cam, Se3 Xanh, Lg3 Cam` | Là chuỗi nước lần lượt, nước đầu `Lg3`. **Không có vấn đề.** Đã phát lại bằng `rules.py`: cả ba nước đều hợp lệ (Xanh `Se3` có 3 hướng, Cam `Lg3` đi tiếp được) |
| T15 (F2) | `Sg5/Bf7>/Bf5<` | Bản gốc ghi `Bg5<` là gõ nhầm, đã sửa thành `Bf5<` (xoay Binh tại chỗ, ký hiệu chuẩn `Bf5<.f5`). Hợp lệ, ngang điểm với `Sg5` |
| T16 (F3) | `Th5/Th6` | Hợp lệ. Oracle thấy `Th6` kém `Th5` (-200 so với -120). Giữ cả hai theo đáp án |
| T17 (F4) | `Bg9>/Bg7<` | LPPN gốc trong PDF có Binh g9 là `b1` (đã quay phải sẵn) nên `Bg9>` không phải nước đi; **đã sửa LPPN thành `b0`**, lượt `x`, ply 27 (LPPN gốc ghi lượt `c`, ply 26 nhưng chú thích là tới lượt Xanh). Sau khi sửa, `Bg9>.g9` và `Bg7<.g7` đều hợp lệ và ngang điểm. Nếu trên GUI Binh g9 thật sự đang quay phải thì LPPN sai chỗ khác, cần export lại |
| T18, T19 (G1, G2) | `Pf4`, `[P]f4` | Không hợp lệ: Phượng chỉ đi chéo, không tới được `f4`. Nghi gõ nhầm `Pf5` (hợp lệ, ngang điểm) nên **thêm** `Pf5`, `[P]f5`. Hãy xác nhận |
| T22 (M1) | `L*b5 Xanh, Va6/Va7 Cam, T*b6/T*b7 Xanh` | LPPN hiện tại chỉ có tù binh Xanh `P,B,B` (không có Long, cũng không có Tướng), nên `L*b5` và `T*b6` **không thả được**. LPPN cần export lại từ GUI (tù binh Xanh phải gồm `L` và `T`). Trong lúc đó test là MATE bất kỳ |
| T23 (M2) | `S*h7/T*h7 …` | Đã kiểm: chiếu hết trong 3 ply với `S*h7v`, `T*h7^`, `T*h7>`, `T*h7v` (Xanh còn 3–4 đường đỡ, đều bị chiếu hết bằng `T*h6v` hoặc `S*h6v`). `S*h7` hướng khác thì **không** |
| T24 (M3) | `X*d2 …` | Chiếu hết 5 ply **chỉ với `X*d2v`** (hướng khác không thắng ép được) |
| T25 (M4) | `T*i8^ …` | Đã kiểm: `T*i8^` thắng ép (đỡ `Vg9` bị `Th9^`; đỡ `Vg8` vẫn bị chiếu hết) |
| T26 (M5) | (trống) | Chưa có đáp án. MATE bất kỳ ≤ 9 ply, hoặc UNVERIFIED |
| T27 (M6) | cây biến trong ảnh | **Đã xác minh toàn bộ cây bằng `rules.py`:** sau `P*i5`, Xanh chỉ có 3 nước hợp lệ (`Vh5`, `Vi3`, `Vi5`). `Vh5` bị `T*h6>` hoặc `T*h6v` chiếu hết; `Vi3` bị `K*h5>` chiếu hết; `Vi5` bị chiếu hết sau đúng 7 ply (`T*i6>` hoặc `T*i6^`, `Vh4`, `S*i5` bốn hướng đều được, `Vi3`, `K*h5>`). Thế cuối (ply 75, biến `T*i6^`, `S*i5<`) **trùng khớp từng ký tự** với LPPN trong ảnh |

**Quy ước độ dài:** PDF ghi "chiếu hết trong N nước" nghĩa là **N ply** (3 nước = Xanh, Cam, Xanh). Nếu bạn muốn ý khác, sửa N ở các test MATE.

### 6.4. Test bổ sung S1–S8 (tổng hợp, đã kiểm chứng bằng `rules.py`)
Các thế dưới đây được dựng lại từ các test cũ (CLP1) sang LPPN hợp lệ. Toàn bộ tập đáp án do script tính từ `rules.py`, không phải nhớ tay. Cả hai Vương luôn nằm ngoài các đường tấn công không mong muốn.

#### S1 — Chiếu hết 1 nước bằng tách Đội / Binh xoay (bù VIII: mate 1 nước, Hình 1)
```text
lppn1 v08/2B36/[L2T0]8/9/9/9/9/9/4V24 c -/- -/- 10
```
* Kiểu **MATE1**: bestmove ∈ `Bb8^`, `Tb8<`, `Tb8>`, `Tb8^`, `Tb9>`, `Tb9^`, `[T]b8<`, `[T]b8>`, `[T]b8^` (đây là toàn bộ nước chiếu hết trong 1 nước, tính bằng `rules.py`). `value` phải là `mate 1`. Quân Xanh duy nhất là Vương nên Xanh có nước đi (ô `a8`) trước khi bị chiếu hết, tức không phải PAT.

#### S2 — Chiếu hết 1 nước bằng thả tù binh (bù VIII: mate 1 nước, Hình 2)
```text
lppn1 v01B36/9/T08/9/9/9/9/9/4V23b0 c -/- X/- 10
```
* Kiểu **MATE1**: bestmove ∈ `X*a7<`, `X*a8<`, `X*b9^`. Xanh có thêm Binh ở góc để thế này **không** phải PAT sẵn (mọi nước tĩnh đều không thắng ngay).

#### S3 — Bị chiếu bởi Long, phải thoát chiếu (bù VII: bảo vệ Vua)
```text
lppn1 V23L24/9/9/9/9/9/5s03/3b01b03/3t0v0t03 x -/- -/B 10
```
* Kiểu **SET**: bestmove ∈ tập thoát chiếu hợp lệ gồm 40 nước: nước đi quân `Sf5<`, `Sf5>`, `Sf5^`, `Sh5<`, `Sh5>`, `Sh5^`, `Th5<.i4`, `Th5<.i6`, `Th5>.i4`, `Th5>.i6`, `Th5^.i4`, `Th5^.i6` và thả Binh `B*?5` xuống cột 5 (`b5` đến `h5`, mọi hướng). Ngoài ra sau nước đi, Cam **không** có chiếu hết 1 nước (harness kiểm bằng vét cạn `rules.py`). Lưu ý: Tướng ở `i6`/`i4` cũng chặn được ở `h5` (bản test cũ bỏ sót).

#### S4 — Ăn quân hở (Phượng ăn Long)
```text
lppn1 3V25/9/9/9/4l04/9/2P26/9/3v05 c -/- -/- 10
```
* Kiểu **EXPECT**: bestmove == `Pe5` (`gain vật chất = +1200`, nước duy nhất).

#### S5 — Binh bị Long Xanh giữ (không ăn bằng Long)
```text
lppn1 3V25/9/9/9/l03b04/9/4L24/9/3v05 c -/- -/- 10
```
* Kiểu **NOT + EXPECT**: bestmove ≠ `Le5` (Long ăn Binh bị Long Xanh ăn lại, oracle `-1000`). bestmove ∈ `Va7`, `Va5`, `Vb7`, `Vb6`, `Vb5`, `Lf5`, `Lg6`, `Lg7`, `Lg8`, `Lg4`, `Lg3`, `Lg2`, `Lg1`.

#### S6 — Long bị Binh đe dọa, ăn Binh
```text
lppn1 3V25/9/9/9/4L24/4b04/9/9/3v05 c -/- -/- 10
```
* Kiểu **EXPECT**: bestmove == `Lf5` (oracle `+200`, nước duy nhất ăn Binh; các nước còn lại gain ≤ 0).

#### S7 — Cứu cả Đội bị Xa đe dọa (Kị hướng 0)
```text
lppn1 3V25/9/9/9/4[B2T2K0]4/9/9/4x04/3v05 c -/- -/- 10
```
* Kiểu **EXPECT**: bestmove ∈ `[K]c6^`, `[K]c6>`, `[K]c6<`, `[K]c4^`, `[K]c4>`, `[K]c4<`, `[K]g7^`, `[K]g7>`, `[K]g7<`, `[K]g3^`, `[K]g3>`, `[K]g3<` (Đội rời cột 5). Vương Cam đặt ở `a6` để Đội rời cột 5 không tự mở tia Xa vào Vương. Bản test cũ dùng `CK2` làm các nước kỳ vọng sai hướng; thế này dùng `K0` nên `[K]c6`, `[K]c4`, `[K]g7`, `[K]g3` đều là nước nhảy thật.

#### S8 — Không tự thua vì lặp thế lần 3 (Thiên Nhật Thủ)
```text
state gốc : lppn1 4V24/9/8B2/9/9/9/9/t07l0/4v04 c -/- -/- 20
lịch sử   : Va6 Vi6 Va5 Vi5 Va6 Vi6 Va5     (7 nước, notation chuẩn, bắt đầu từ CAM)
hiện tại  : lppn1 4V24/9/8B2/9/9/9/9/t07l0/3v05 x -/- -/- 27   (XANH tới lượt)
```
* Harness dựng state bằng cách **phát lại 7 nước bằng `rules.apply_move`**, cộng bộ đếm lặp thế như `core_bridge._apply_move_to_state` (mọi trạng thái trong chu kỳ đã có `count = 2`), rồi gửi `position state64`.
* Kiểu **NOT**: bestmove ≠ `Vi5` (nước đó đưa thế xuất phát lên lần xuất hiện thứ 3, bên vừa đi là XANH nên XANH thua, đã kiểm `count_if_played = 3` bằng script). Phải có ≥ 30 nước hợp lệ khác để chọn (script đếm `legal = 33`). `info` của hạng 1 không được chứa `Vi5` ở đầu PV.
* Kiểm chứng đảo chiều: nếu bảng `repetition.entries` bị **bỏ** (chỉ còn `count = 1`), engine được phép đi `Vi5`. Test này chứng minh engine thực sự dùng lịch sử từ state.

---

## PHẦN 7. CÁC ĐIỂM CẦN XÁC NHẬN / RỦI RO ĐÃ BIẾT

* **R-1 — Ngữ nghĩa `mate` trong `clp.info/1`:** `engine_client.py` chỉ hiển thị `"Mate " + value`, không file nào đính kèm định nghĩa `value` là số *nước* hay *ply*. Spec chọn số nước của bên chiếu hết (UCI-style); test MATE kiểm theo độ dài `pv` (ply) nên không bị ảnh hưởng.
* **R-2 — Thứ tự `prisoners` trong khóa lặp thế:** `core_bridge._position_identity_py` băm JSON với danh sách `prisoners` **có thứ tự** (`[B,K]` ≠ `[K,B]`), trong khi Zobrist của engine coi là multiset. Engine có thể thấy lặp thế **sớm hơn** GUI trong trường hợp hiếm (an toàn, không muộn hơn). Muốn khớp tuyệt đối thì sắp xếp `prisoners` trước khi băm ở GUI.
* **R-3 — `repetition.complete == false`:** chưa rõ ngữ nghĩa trong GUI. Engine coi là cảnh báo, không từ chối.
* **R-4 — Không có `game.py`, `cpp_engine.py`, `clp_rules_service.dll` trong bộ file đã nhận:** PAT = thua và lặp thế lần 3 được xác nhận từ `core_bridge._adjudicate_state_py` (đường Python thuần). Nếu DLL C++ (`ryuugin.rules/1`) khác Python ở điểm nào, **DLL là nguồn sự thật**, và Test 0 phải đối chiếu thêm với `native.py`.
* **R-5 — Notation:** PDF viết `[B]e3` không có hướng, trong khi `canonical_notation` luôn thêm hướng cho quân xoay được (`[B]e3^`). Engine theo `canonical_notation`.
* **R-6 — Đáp án:** các chỗ đáp án không khớp luật được liệt kê ở 6.3b. Chiếu hết M1, M5, M6 chưa có lời giải xác nhận độc lập.
* **R-7 — Tốc độ:** mục tiêu tham khảo ≥ 1 triệu nút/giây/luồng trên i7-8700K, build Release. Không phải tiêu chí PASS, nhưng nếu thấp hơn 200 knps thì phải profile `is_attacked` và `generate` trước khi tinh chỉnh search.
