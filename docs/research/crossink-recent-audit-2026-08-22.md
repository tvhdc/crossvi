# Audit commit CrossPoint và CrossInk gần đây cho CrossVi (2026-08-22)

## Phạm vi và cách kiểm tra

- Nguồn CrossPoint: nhánh `develop` tại SHA `69dd947f5f4ce48c3e8eafe32e3469de93ad2317`;
  đã đọc các commit mới từ 2026-08-12 đến 2026-08-21 và diff chính xác của các ứng viên liên quan CrossVi.

- Nguồn chính: default branch `main` của `uxjulia/crossink` tại
  `cab4f24922f05811e7f44be1057f62ea2d978c52`; từ 2026-07-01 đến nay nhánh này chủ yếu có release manifest và
  bug-report housekeeping. Các thay đổi runtime mới nhất nằm trên `development`, nên đã đọc thêm 50 commit
  non-merge mới nhất của nhánh đó, tới `6fb35deb795b99521e7a643848acbcec09b72614` (2026-08-21), cùng diff chính
  xác của các thay đổi có ảnh hưởng runtime.
- Đối chiếu với worktree CrossVi hiện tại, gồm cả thay đổi chưa commit. Không dùng release cũ để suy ra
  CrossVi đang thiếu gì.
- Đây là audit source, không phải bằng chứng timing/RAM/panel trên X3/X4. Không sửa mã nguồn.
- Theo policy của repository, mọi hunk sleep/wake bị loại khỏi đề xuất lần này vì yêu cầu hiện tại không trực tiếp
  cho phép sửa sleep/wake.

## Kết luận ngắn

Nhóm đáng adapt sớm nhất gồm:

1. ngăn saved-position cũ kéo reader ngược lại sau khi người dùng đã lật trang;
2. bỏ cấp phát heap lặp lại trong `ButtonNavigator` bằng container cố định một phần tử;
3. chấp nhận EPUB có dữ liệu rác sau thẻ đóng `</html>`;
4. thêm entity XML `&apos;` vào parser EPUB;
5. validate kích thước JPEG trước khi ép về `int16_t`;
6. lưu các thanh ghi exception RISC-V vào crash report để chẩn đoán đúng địa chỉ gây panic.

Hai thay đổi web EPUB optimizer đáng cân nhắc tiếp theo: bỏ comment XHTML và, dưới dạng tùy chọn rõ ràng,
bỏ font nhúng. Hai nhóm tối ưu font/Wi-Fi có ý tưởng tốt nhưng cần đo heap/timing trên X3 và X4 trước khi port.
Các commit còn lại hoặc CrossVi đã có logic tương đương, hoặc gắn với touch/X4 Pro/FreeInkUI, hoặc dựa trên
kiến trúc dữ liệu khác nên không nên cherry-pick.

## CrossPoint: các thay đổi đáng adapt

### P1 — ngăn resume position cũ kéo trang đọc ngược lại

- CrossPoint: [`48e39eb0eee6da72dfe45d1343c9e9014e84e36a`](https://github.com/crosspoint-reader/crosspoint-reader/commit/48e39eb0eee6da72dfe45d1343c9e9014e84e36a).
- CrossVi vẫn giữ `cachedContentSourceOffset`, `cachedVisibleTextOffset`, `cachedChapterTotalPageCount` và source-offset
  target trong lúc section đang repaginate. Sau một lượt lật thật, deferred reposition cũ có thể được áp lại khi build
  hoàn tất và đưa reader về vị trí trước đó.
- Phân loại: **worth adapting, correctness impact cao, regression risk trung bình**. Không cherry-pick vì CrossVi có
  queue/retarget riêng; thêm một helper xóa deferred resume target đúng lúc lượt lật hoặc absolute navigation thực sự
  được áp dụng, rồi test incomplete section, đổi hướng và burst queue.

### P1 — bỏ qua dữ liệu không phải XML sau `</html>`

- CrossPoint: [`7e42d070774e75f250c8ffefe5e9357dacd39eb2`](https://github.com/crosspoint-reader/crosspoint-reader/commit/7e42d070774e75f250c8ffefe5e9357dacd39eb2).
- `ChapterHtmlSlimParser` của CrossVi chưa có trạng thái kết thúc document sớm; EPUB có trailer/rác sau thẻ đóng có thể
  bị coi là lỗi dù nội dung XHTML hợp lệ đã kết thúc.
- Phân loại: **worth adapting, regression risk thấp**. Bản vá nhỏ, cần fixture có trailer và fixture malformed trước
  `</html>` để không che lỗi thật.

### P1 — validate kích thước JPEG trước khi thu hẹp kiểu

- CrossPoint: [`d3b3b5669595c8fd1417d84ee0558e5aa5df2186`](https://github.com/crosspoint-reader/crosspoint-reader/commit/d3b3b5669595c8fd1417d84ee0558e5aa5df2186).
- CrossVi đã dùng tích `uint64_t` trong validator và probe chung đã chặn kích thước quá `INT16_MAX`, nhưng
  `JpegToFramebufferConverter::getDimensionsStatic()` vẫn gán trực tiếp kích thước decoder vào `int16_t`.
- Phân loại: **worth adapting một hunk nhỏ, regression risk thấp**. Dùng validator hiện có trước cast; không port toàn
  bộ thay đổi decode/yield vì scheduler và `RenderLock` của CrossVi khác.

### P2 — giải phóng toàn bộ cache font tái tạo được trước Wi-Fi/web

- CrossPoint: [`080b1d719a0f58285a36ea5268aed0992b275aa9`](https://github.com/crosspoint-reader/crosspoint-reader/commit/080b1d719a0f58285a36ea5268aed0992b275aa9).
- CrossVi đã `clearCache()` trước khi tạo server, nhưng SD font vẫn giữ advance/kern/ligature resident data.
- Phân loại: **chỉ adapt sau khi đo**. Đo `free heap` và `largest allocatable block` trước/sau Wi-Fi trên X3/X4 với SD
  font lớn; chỉ lấy phần cache tái tạo được nếu cải thiện allocation headroom và không làm lần quay lại reader chậm rõ.

## CrossPoint: đã có, nên hoãn hoặc bỏ qua

- [`99724aa79a9543124d7511bac32ad95a237cfcdf`](https://github.com/crosspoint-reader/crosspoint-reader/commit/99724aa79a9543124d7511bac32ad95a237cfcdf)
  (OPF namespace-local metadata) và [`33f07db7101ec8d817305ade3c7a9ec3afa66fd9`](https://github.com/crosspoint-reader/crosspoint-reader/commit/33f07db7101ec8d817305ade3c7a9ec3afa66fd9)
  (FileBrowser render race): CrossVi đã có logic tương đương.
- [`da3d50c245334155daccb85058f3645637fd6db4`](https://github.com/crosspoint-reader/crosspoint-reader/commit/da3d50c245334155daccb85058f3645637fd6db4)
  (table columns) là feature khoảng 500 dòng và thay cache semantics; chỉ nên làm như dự án EPUB riêng có fixture.
- Ruby grouping, CJK font/UI overhaul, Ubuntu Medium, wrapped-list layout và Extra Wide spacing không phải low-risk
  performance patch cho CrossVi hiện tại.
- X4 Pro/frontlight/touch không phù hợp X3/X4 CrossVi. Các hunk sleep/wake bị loại hoàn toàn khỏi vòng này theo policy
  repository và vì worktree đang có thay đổi sleep riêng.

## CrossInk: nên adapt

### P1 — thêm `&apos;` vào bảng entity EPUB

- CrossInk: [`a7a776a992ad5fa102eb262b28ae0b8ee34d8d8f`](https://github.com/uxjulia/crossink/commit/a7a776a992ad5fa102eb262b28ae0b8ee34d8d8f).
- CrossVi: `lib/Epub/Epub/htmlEntities.cpp:14-30` dùng binary search trên bảng đã sắp xếp nhưng hiện đi thẳng từ
  `&ang;` đến `&aring;`, thiếu `&apos;`.
- Phân loại: **worth adapting, regression risk thấp**. Đây là entity XML chuẩn; lỗi hiện tại có thể để nguyên
  chuỗi `&apos;` trong nội dung/metadata. Bản vá chỉ cần chèn đúng vị trí sort và thêm host test lookup.

### P1 — bỏ allocation trong `ButtonNavigator`

- CrossInk: [`a1deb05f522ebd05a3b247bbd21ac25c88c17c8a`](https://github.com/uxjulia/crossink/commit/a1deb05f522ebd05a3b247bbd21ac25c88c17c8a)
  đổi `Buttons` từ `std::vector` sang `std::array`, sau crash khi giữ nút lúc heap thấp.
- CrossVi: `src/util/ButtonNavigator.h:11-14,34-59` vẫn tạo `std::vector` tạm; các đường giữ nút gọi lại qua
  `src/util/ButtonNavigator.cpp:19-75`. Search toàn bộ caller hiện tại cho thấy mọi danh sách truyền vào chỉ có
  **một** nút; `getNextButtons()`/`getPreviousButtons()` cũng chỉ trả một logical `NavNext`/`NavPrevious`.
- Phân loại: **worth adapting, regression risk thấp**. Adapt phù hợp CrossVi là
  `std::array<MappedInputManager::Button, 1>`, không copy kích thước 2 của CrossInk vì mapping vật lý đã được gom
  thành logical button. Chạy focused input/navigation tests để bảo toàn latch, release và hold-repeat.

### P2 — thêm thanh ghi fault RISC-V vào crash report

- CrossInk: [`f0ae8f4d9af0ae9ad2497fba924004770a45b692`](https://github.com/uxjulia/crossink/commit/f0ae8f4d9af0ae9ad2497fba924004770a45b692)
  lưu `MEPC`, `RA`, `SP`, `S0/FP`, `MCAUSE`, `MTVAL`, `MSTATUS` trong RTC no-init.
- CrossVi: `lib/hal/HalSystem.cpp:15-74,124-170` hiện chỉ lưu message và 32 dòng raw stack; report không có
  instruction gây fault, cause hoặc bad address.
- Phân loại: **worth adapting, runtime regression risk thấp; compile/API risk trung bình**. Chỉ lấy nhánh RISC-V
  cho ESP32-C3 của X3/X4, không lấy Xtensa/X4 Pro. Phải build bằng toolchain hiện tại để xác nhận layout
  `esp_cpu_frame_t`; test phần format trên host và xác minh một panic có kiểm soát trên thiết bị trước khi gọi là đã
  hoạt động.

### P2 — bỏ comment XHTML trong EPUB optimizer trên web

- CrossInk: [`ecfee9cb4f01efedad6965d220c183e36ffc49bb`](https://github.com/uxjulia/crossink/commit/ecfee9cb4f01efedad6965d220c183e36ffc49bb)
  bỏ comment trước `DOMParser`, nhưng giữ CDATA, processing instruction và DOCTYPE có internal subset.
- CrossVi: optimizer trong `src/network/html/FilesPage.html:4554-4590,5643-5807` hiện parse/serialize XHTML nhưng
  không có bước này.
- Phân loại: **worth adapting, regression risk thấp đến trung bình**. Lợi ích rõ nhất là các EPUB có comment sinh
  tự động/rất lớn: file tối ưu nhỏ hơn và giảm text parser phải đi qua. Đây là code browser, không thêm chi phí
  firmware reader. Cần JS fixtures cho comment bình thường, comment chưa đóng, CDATA, PI và DOCTYPE trước khi giữ.

### P3 — bỏ font nhúng trong bản EPUB đã tối ưu (nên là tùy chọn)

- CrossInk: [`d28b9390a2da1db88d916a330bb9a84ddfec0818`](https://github.com/uxjulia/crossink/commit/d28b9390a2da1db88d916a330bb9a84ddfec0818)
  bỏ font file, `@font-face`, OPF manifest và font-obfuscation entry.
- CrossVi: cùng optimizer ở `src/network/html/FilesPage.html:4653-4739,5643-5807` chưa có bước tương đương;
  firmware CrossVi không dùng font nhúng của EPUB.
- Phân loại: **worth adapting có điều kiện, regression risk trung bình**. Có thể giảm hàng trăm KiB đến vài MiB
  mỗi bản tối ưu, nhưng kết quả sẽ mất typography khi mở bằng app khác. Không nên âm thầm bật mặc định; cần lựa
  chọn/mô tả rõ và fixture OPF path-encoding, CSS nested syntax, `data:` font và mixed `encryption.xml`.

## CrossInk: cần đo trên phần cứng trước

### Giải phóng font trước Wi-Fi

- CrossInk: [`34cd593accf32cb2e72d3958a3c136b227df2ce9`](https://github.com/uxjulia/crossink/commit/34cd593accf32cb2e72d3958a3c136b227df2ce9)
  gọi `releaseForNetwork()`, giải phóng cả active SD font và registry để tăng contiguous heap trước khi radio/TLS cấp
  phát.
- CrossVi: `src/activities/network/WifiSelectionActivity.cpp:19-75` chưa giải phóng font khi vào Wi-Fi;
  `src/SdCardFontSystem.cpp:50-59` chỉ có `releaseLoadedFont()`, còn registry được discover từ boot tại
  `src/SdCardFontSystem.cpp:27-47` và giữ resident.
- Phân loại: **needs hardware evidence**. Ý tưởng hợp lý, nhưng CrossVi reader đã release active font khi pause/exit
  (`src/activities/reader/EpubReaderActivity.cpp:450-509`), nên phần lợi ích còn lại chủ yếu là registry. Đo
  `free heap` và `largest allocatable block` ngay trước/sau Wi-Fi setup với catalog font lớn trên cả X3/X4; chỉ adapt
  nếu tăng headroom có ý nghĩa và việc rediscover sau Wi-Fi không làm lag/mất lựa chọn font.

### Prewarm mixed/CJK fonts

- CrossInk: [`8c920e41192d59ca92b21e67d6ee50f6a62ec2d8`](https://github.com/uxjulia/crossink/commit/8c920e41192d59ca92b21e67d6ee50f6a62ec2d8)
  và [`a0daab99416726804d14b8be77c5284c33d3348f`](https://github.com/uxjulia/crossink/commit/a0daab99416726804d14b8be77c5284c33d3348f).
- CrossVi đã có prewarm theo bốn style, gộp physical fallback style và giới hạn text
  (`lib/GfxRenderer/FontCacheManager.h:25-64`, `lib/GfxRenderer/FontCacheManager.cpp:145-175`), rồi collect trực tiếp
  page text trước render (`src/activities/reader/EpubReaderActivity.cpp:3799-3814`). Vì vậy phần lớn ý tưởng đã có.
  Phần CrossInk còn khác là giữ union codepoint giữa nhiều font/fallback và prewarm TOC/list/status bar.
- Phân loại: **needs hardware evidence, không port nguyên patch**. Đo cold/warm CJK TOC, file list và page có
  Latin+CJK với SD font; theo dõi timing, free heap và largest block. Chỉ bổ sung đúng text/font bị miss nếu log cho
  thấy repeated glyph I/O; patch CrossInk lớn và có rủi ro RAM/fragmentation.

### RTL low-memory fail-safe

- CrossInk: [`b1f3e91dba699580370a121bf068319f3937cf97`](https://github.com/uxjulia/crossink/commit/b1f3e91dba699580370a121bf068319f3937cf97)
  chuyển riêng scratch width sang `ArenaVector` có lỗi trả về thay vì `std::vector::reserve()` abort.
- CrossVi: `lib/Epub/Epub/ParsedText.cpp:1065-1208` có cùng loại `reorderedWidthsScratch` bằng `std::vector`, nhưng
  không có arena/safe-container contract tương đương và còn nhiều scratch vector khác trong cùng flow.
- Phân loại: **needs a reproducible OOM case/design work**. Không thay một vector rồi tuyên bố hết abort. Dựng RTL
  fixture + heap-pressure harness, sau đó xử lý cả allocation boundary của `extractLine()` theo kiến trúc CrossVi.

## CrossInk: đã có hoặc đã có logic tương đương

| CrossInk commit | Đối chiếu CrossVi | Kết luận |
|---|---|---|
| [`9a0d1de2`](https://github.com/uxjulia/crossink/commit/9a0d1de2c74fddbcfd13d4dc543f4c464904744b) Wi-Fi prompt | `src/activities/network/WifiSelectionActivity.cpp:795-892` đã có lựa chọn Save/Do not save và Cancel/Forget cho button UI. | Already present. |
| [`280dd038`](https://github.com/uxjulia/crossink/commit/280dd0387b23ef865332dfd4cda8c4e7f28c065b) null KOReader progress | `lib/KOReaderSync/KOReaderSyncClient.cpp:274-317` đã xử lý cả hai field null là `NOT_FOUND`, validate type/range và chỉ assign output sau parse thành công. | Already present/superior. |
| [`4436a4d4`](https://github.com/uxjulia/crossink/commit/4436a4d4805c331d4ab2884ac1d3ce48547e4625), [`27f964b3`](https://github.com/uxjulia/crossink/commit/27f964b3e7ea73d65933946a13ea8ceb127ad3a7) negative font ID | `lib/GfxRenderer/FontCacheManager.h:60-63` đã dùng signed ID và chỉ coi `0` là sentinel. | Already present. |
| [`f0788a4b`](https://github.com/uxjulia/crossink/commit/f0788a4bc7ae1246995a16aed519b120da6c013f) long-hold chapter skip | `src/activities/reader/EpubReaderActivity.cpp:1476-1558` nhận `longPress` và đổi chapter ngay trên event hold, không chờ release. | Equivalent. |
| [`bb99c0b6`](https://github.com/uxjulia/crossink/commit/bb99c0b6eca6638e665bbeb23d6114207e2a9ac4) previous chapter first page | Cùng flow tại `EpubReaderActivity.cpp:1536-1554` đặt previous spine với `nextPageNumber=0`; CrossInk patch gốc chỉ cho touch gesture. | Equivalent. |
| [`671c17dd`](https://github.com/uxjulia/crossink/commit/671c17ddf5b37271a6d3bcf702e86544b8a9f53d) XTC covers | CrossVi đã tạo/đọc XTC/XTCH thumbnails cho Home/Library tại `src/activities/home/RecentBooksActivity.cpp:1176-1263`; theme ưu tiên đúng thumbnail tại `src/components/themes/crossvi/CrossViTheme.cpp:299-331`. | Already present, implementation khác và cooperative hơn. |
| [`fbfe7206`](https://github.com/uxjulia/crossink/commit/fbfe7206ed496136a21bf3801836c9a9ff3d85dd) PNG Set Cover | Worktree CrossVi hiện đã cho image picker/preview nhận BMP/PNG và có flow set sleep image riêng. | Already present trong dirty worktree; không port lại. |

## CrossInk: không phù hợp hoặc không port trực tiếp

| Nhóm commit CrossInk | Lý do |
|---|---|
| [`9d36a067`](https://github.com/uxjulia/crossink/commit/9d36a06729ee0111bfaa0549d5a606e59bb4e8e9), [`94d13eb2`](https://github.com/uxjulia/crossink/commit/94d13eb2ea2c1a314450c93f097cea1dd8bf9e6b) table/caption/PXC | CrossInk dùng `CompactTableLayout`/retained PXC khác hẳn. CrossVi flatten mỗi cell thành paragraph (`ChapterHtmlSlimParser.cpp:601-665`) và cache image dùng candidate buffer rồi mới publish (`ImageBlock.cpp:188-203`), nên exact crash/overflow không tồn tại theo cùng flow. Caption có thể cần fixture riêng, nhưng không có bằng chứng để port patch lớn. |
| [`9eeaeb3e`](https://github.com/uxjulia/crossink/commit/9eeaeb3e389ff1dba20c2f0ab835bd9891bdff2c) XTC/token/pinch | XTC CrossVi có chapter count/range contract nghiêm ngặt (`lib/Xtc/Xtc/XtcParser.cpp:293-298,426-460`); bỏ qua row invalid sẽ làm yếu validation. Pinch không tồn tại trên X3/X4; token split khác implementation. Chỉ xem lại nếu có file XTC thật tái hiện incompatibility. |
| [`88b81c60`](https://github.com/uxjulia/crossink/commit/88b81c6098e44b1d9c65d9fc1589b977a0154f24), [`c6fe67d4`](https://github.com/uxjulia/crossink/commit/c6fe67d48d8b65f5dfdefae31f06d0bc9826f267) ruby | CrossVi chưa có ruby grouping model tương đương; đây là feature/parser project, không phải low-risk port. |
| [`34487e1b`](https://github.com/uxjulia/crossink/commit/34487e1bd291b21fa613dd3ed484d2f9f3eb56b5), [`2025f695`](https://github.com/uxjulia/crossink/commit/2025f69589d07218b4fa17187f0deb9bf1bd4fe8), [`47f6d15e`](https://github.com/uxjulia/crossink/commit/47f6d15e7cbaa16c02ecb00aa1072d880cdc8dd5) file/virtual list | Fix cho FreeInkUI virtual window/touch action tables. CrossVi dùng `GUI.drawList` và cooperative file browser riêng, không có class/window contract bị sửa. |
| [`47b83e9e`](https://github.com/uxjulia/crossink/commit/47b83e9e356b51fdefad300acab85b3dbb72a4f6) hidden image hints | CrossVi truyền chuỗi rỗng tại `BmpViewerActivity.cpp:120-130`, nhưng theme chủ ý vẫn vẽ pill ngắn cho cả null và empty (`CrossViTheme.cpp:672-695`). Đổi sang `nullptr` như CrossInk không thay đổi output. |
| [`16ed227a`](https://github.com/uxjulia/crossink/commit/16ed227a07cae943f7980d8fb6318810bfdd27ed), [`031d6ce2`](https://github.com/uxjulia/crossink/commit/031d6ce2613696f74e6f79af5d43e39e0ddc1c54), [`7a485029`](https://github.com/uxjulia/crossink/commit/7a485029bd7bc7dc18d9c46c9040c98fcb38a67e), [`fb373b72`](https://github.com/uxjulia/crossink/commit/fb373b729d626e498da8d5b20f00ad1315876670), [`7780a91a`](https://github.com/uxjulia/crossink/commit/7780a91a1239daca3a976b17a5c62fd51e387b82), phần pinch của `9eeaeb3e` | Touch/home-button/X4-Pro specific; X3/X4 CrossVi không có input capability tương ứng. |
| [`52cdf58d`](https://github.com/uxjulia/crossink/commit/52cdf58d411ee61b7c5e3ffee36b6a405c24060a), [`31927100`](https://github.com/uxjulia/crossink/commit/31927100e21325dc3474ea650e96850fb9face49), [`3b8caae4`](https://github.com/uxjulia/crossink/commit/3b8caae41f48a5a0287eb941957e59d57032075d) | Touch/Quick Actions/combo feature và input architecture khác. CrossVi đã có reader gesture/latch riêng; không có repro tương ứng và không nên mở rộng shortcut scope. |
| [`8a144a4a`](https://github.com/uxjulia/crossink/commit/8a144a4a16586cdc7cb8e149a93f123169810c42), [`d6f787ac`](https://github.com/uxjulia/crossink/commit/d6f787acbada53be634546b2aed9c801f5305f3c), [`6fb35deb`](https://github.com/uxjulia/crossink/commit/6fb35deb795b99521e7a643848acbcec09b72614) | FreeInkUI migration, i18n refactor và test-link housekeeping; không có lợi ích runtime rõ cho CrossVi và tăng churn. |
| [`aabcb670`](https://github.com/uxjulia/crossink/commit/aabcb670d46303fe3f1b7ccf091ea06033df2a44) clipping NBSP | CrossVi dùng clipping/reanchor/fingerprint pipeline khác và không có `ClippingTextMatcher` tương ứng. Cần EPUB+clipping fixture tái hiện trước, không copy matcher. |
| [`b1ed6a98`](https://github.com/uxjulia/crossink/commit/b1ed6a988d9213130929a456f2907db8e97bf93d) case-insensitive folders | Trộn sleep, dictionaries và fonts. Lợi ích nhỏ, path/persistence surface rộng; không đủ ưu tiên so với regression risk. Nếu có yêu cầu thực tế, tách riêng font/dictionary và test duplicate case-collisions. |

## Bị loại khỏi scope vì liên quan sleep/wake

Không đề xuất hoặc đánh giá để áp dụng trong vòng này: `35b8610c`, `a4a03c6b`, `f9a8b2da`, `88afeaba`,
`eef20504`, phần sleep của `b1ed6a98`, và phần `SleepImageIndex` của `9eeaeb3e`. CrossVi đang có nhiều dirty
change ở sleep image, nhưng yêu cầu hiện tại không cho phép tiếp tục thay đổi sleep/wake; audit các hunk này riêng sẽ
cần full entry-path X3/X4 và kiểm thử phần cứng.

## Thứ tự triển khai đề xuất nếu được yêu cầu sửa

1. `48e39eb0` (xóa deferred resume target sau navigation thật) + regression test pagination/queued turn.
2. `a1deb05f` (fixed one-button array) + focused button hold/release/navigation tests.
3. `7e42d070` (trailing data after `</html>`) + parser fixture.
4. `a7a776a9` (`&apos;`, xuất hiện ở cả hai fork) + lookup test.
5. Hunk validate-before-cast từ `d3b3b566` + malformed JPEG dimension test.
6. `f0ae8f4d` (RISC-V crash registers only) + `gh_release`; phần cứng panic test sau đó.
7. `ecfee9cb` (comment stripper) + browser fixtures + regenerate HTML header.
8. Chỉ sau số đo: giải phóng font trước Wi-Fi và phần thiếu thật sự của mixed/CJK prewarm.
9. `d28b9390` chỉ nếu chấp nhận một tùy chọn optimizer làm EPUB output phụ thuộc CrossVi hơn.

Không nên cherry-pick nguyên commit nào: ngay cả hai patch rất nhỏ đầu tiên cũng cần adapt tên logical button, test và
format theo CrossVi hiện tại.

## Nguồn chính

- [CrossPoint `develop`](https://github.com/crosspoint-reader/crosspoint-reader/commits/develop/)
- [CrossInk `development`](https://github.com/uxjulia/crossink/commits/development/)
