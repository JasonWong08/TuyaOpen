# 问题：MP3 解码在 `mp3dec_decode_frame` 中 `malloc` 失败（约 16 KB）

## 现象

绑定/提示音播放时日志类似：

```text
malloc failed:0x3f6c free:0x2f1c
--- mp3dec_decode_frame at minimp3.h:...
decoder_mp3_process: invalid frame_bytes=...
```

`0x3f6c`（16236）与 `sizeof(mp3dec_scratch_t)` 一致：minimp3 在**每一帧**解码时从堆上申请一块大型 scratch，堆紧张或碎片大时易失败。

## 根因

`src/audio_player/src/decoder/minimp3/minimp3.h` 中 `mp3dec_decode_frame()` 对 Layer 3 使用：

```c
scratch = (mp3dec_scratch_t *)MP3_MALLOC(sizeof(mp3dec_scratch_t));
```

在无 PSRAM、已拉起 WiFi/BLE/配网的任务场景下，**难以保证每次都有足够连续堆块**。

## 修复

1. 在 `minimp3.h` 的 `MINIMP3_IMPLEMENTATION` 段增加可选宏 **`MINIMP3_USE_STATIC_SCRATCH`**：
   - 使用文件内静态 `g_mp3dec_frame_scratch` 替代 `MP3_MALLOC` / `MP3_FREE`。
2. 在 `decoder_mp3.c` 中于 `#define MINIMP3_IMPLEMENTATION` **之前**定义：

   `#define MINIMP3_USE_STATIC_SCRATCH 1`

3. **`DECODER_MP3_CTX_T`（约 **6676 B**，内含 `mp3dec_t`）**  
   - **有 PSRAM**（`ENABLE_EXT_RAM==1`）：仍用 `tal_psram_malloc` / `tal_psram_free`。  
   - **无 PSRAM**：使用 **BSS 静态单例** `s_decoder_mp3_ctx`，避免绑定时在已有 **AI_PLAYER_RINGBUF**（数 KB）之后再 `malloc 0x1a14`（日志常见 `free≈0x175c` 仍失败）。  
   - **注意**：静态 `ctx` 会缩小堆“竞技场”，若 **LVGL 全屏缓冲过大**（如 PETOI 曾用 10 行 partial buffer），Phase-4 会掉到 **~1 KB**，导致 LFS **0x1000**、`datasink_mem` 连锁失败。**对策**：PETOI 将 `DISPLAY_BUFFER_SIZE` 减到 **6 行**（或更小），使 Phase-4 仍保留 **约 4 KB+** 给 LFS 等。

4. **PETOI ESP32-C3**：`DISPLAY_BUFFER_SIZE` **10→6 行**；可选将 **`CONFIG_AI_PLAYER_RINGBUF_SIZE`** 从 **4096→3072**，进一步降低绑定时峰值堆占用。

## 代价与约束

- **BSS**：约 **16 KB**（`mp3dec_scratch_t`）+ 无 PSRAM 时再 **~6.6 KB**（`DECODER_MP3_CTX_T` 静态单例）。
- 无 PSRAM 时若 UI 缓冲仍过大，Phase-4 可能 **< 4 KB**，LFS / ringbuf 仍失败——需继续减 LVGL 或 ringbuf。
- **帧 scratch 非可重入**（单路 MP3 解码）。

## 验证

- 不再出现 `malloc failed:0x3f6c`（`mp3dec_decode_frame` 内）。
- Phase-4 应 **明显高于 ~1 KB**（避免 `lfs open rst_cnt` 与 `datasink_mem` 连锁失败）。
- 若仍见 `0x1a14` @ `decoder_mp3_start`，继续减小 UI/播放器缓冲或换 PCM 提示音。
