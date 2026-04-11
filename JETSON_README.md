# Sunshine Jetson Orin Hardware Encoder Support

Hardware-accelerated game streaming from NVIDIA Jetson Orin (NX / AGX) to Moonlight clients using the Jetson Multimedia API.

## What's New

This fork adds native Jetson nvv4l2 hardware encoder support to Sunshine, enabling low-latency H.264/HEVC streaming via Moonlight. Key features:

- **NvVideoEncoder (V4L2 M2M)** — Direct hardware encoding, bypassing FFmpeg's avcodec layer for lower latency
- **VIC hardware color conversion** — BGRA to NV12 conversion offloaded to the Video Image Compositor, freeing the CPU
- **DMA-BUF zero-copy** — NV12 frames passed directly to the encoder via DMA-BUF file descriptors, eliminating CPU memcpy
- **Pipelined encode** — CPU+VIC prepares frame N+1 while NVENC encodes frame N, overlapping the work
- **Web UI configuration** — Encoder settings accessible from the Sunshine web interface (Encoders > Jetson nvv4l2)
- **Full config wiring** — All encoder settings (preset, rate control, two-pass CBR, IDR interval, SPS/PPS, max perf) are configurable from the web UI and applied to the hardware encoder at runtime

### Performance (Jetson Orin NX)

| Optimization Stage | 1080p FPS | Bottleneck Removed |
|---|---|---|
| Baseline (swscale + MMAP + all-IDR) | ~40 | — |
| VIC hardware conversion | ~47 | CPU sws_scale (~7ms) |
| Persistent DMA-BUF mapping | ~50 | Per-frame Map/Unmap overhead |
| Single-buffer (removed unnecessary double-buffer) | ~52 | Reduced memory, same perf |
| Pipelined encode | **~60** | CPU+VIC overlaps with NVENC |
| Higher FPS target (140fps) | **~137** | Lower bits/frame = faster NVENC |

### How the Pipeline Works

**Before (serial) — 52 FPS:**
```
Frame 0: [memcpy+VIC 4ms][========= NVENC wait 15ms =========] → packet
Frame 1:                                                         [memcpy+VIC 4ms][========= NVENC wait 15ms =========]
         |______________________ 19ms ________________________|
```

**After (pipelined) — 60+ FPS:**
```
Frame 0: [memcpy+VIC 4ms][qBuffer]
Frame 1:                  [memcpy+VIC 4ms][qBuffer, dqBuffer frame 0] → packet 0
Frame 2:                                  [memcpy+VIC 4ms][qBuffer, dqBuffer frame 1] → packet 1
NVENC:                    [======= encode 0 =======][======= encode 1 =======]
                          |________ 15ms ___________|
```

Frame N is queued to NVENC, then the previous frame's encoded output is dequeued (already finished while CPU+VIC prepared frame N). Effective frame interval: max(CPU+VIC, NVENC).

At 60fps target: max(4ms, 15ms) = **15ms = 66 FPS** (capped to 60 by client).
At 140fps target: lower bits/frame makes NVENC faster (~7ms), achieving **~137 FPS**.

Latency per frame remains the same (19ms capture-to-packet at 60fps). Only throughput improves.

## Requirements

- **Hardware**: NVIDIA Jetson Orin NX or Jetson AGX Orin
- **JetPack**: 5.1.x (L4T 35.x) with Multimedia API
- **Compiler**: Clang 18+ with libc++ (for C++23 `<format>` support)
- **CMake**: 3.29+

### JetPack Packages

The following must be installed (included with JetPack):

```
/usr/src/jetson_multimedia_api/          # Headers + sample classes
/usr/lib/aarch64-linux-gnu/tegra/
  libnvmedia.so
  libnvv4l2.so
  libnvos.so
  libnvbuf_utils.so
  libnvbufsurface.so        # VIC buffer management
  libnvbufsurftransform.so  # VIC hardware transforms
```

## Build Instructions

### 1. Set Up Clang 18 Toolchain

JetPack 5.x ships Ubuntu 20.04 with GCC 9, which lacks C++23 support. A local Clang 18 installation is required:

```bash
# Download and extract Clang 18 (example path)
export CLANG_PATH=$HOME/sunshine/clang-install

export LD_LIBRARY_PATH=$CLANG_PATH/usr/lib/aarch64-linux-gnu:$CLANG_PATH/usr/lib/llvm-18/lib:$LD_LIBRARY_PATH
export CPLUS_INCLUDE_PATH=$CLANG_PATH/usr/lib/llvm-18/include/c++/v1:$CLANG_PATH/usr/include:$CLANG_PATH/usr/include/aarch64-linux-gnu
export C_INCLUDE_PATH=$CLANG_PATH/usr/include:$CLANG_PATH/usr/include/aarch64-linux-gnu
```

### 2. Configure with CMake

```bash
cd Sunshine
mkdir -p build && cd build

cmake .. \
  -DCMAKE_CXX_COMPILER=$CLANG_PATH/usr/lib/llvm-18/bin/clang++ \
  -DCMAKE_C_COMPILER=$CLANG_PATH/usr/lib/llvm-18/bin/clang \
  -DCMAKE_CXX_FLAGS="-stdlib=libc++" \
  -DCMAKE_EXE_LINKER_FLAGS="-stdlib=libc++ -L$CLANG_PATH/usr/lib/llvm-18/lib -L$CLANG_PATH/usr/lib/aarch64-linux-gnu -lc++ -lc++abi -lunwind -Wl,-rpath,$CLANG_PATH/usr/lib/llvm-18/lib -Wl,-rpath,$CLANG_PATH/usr/lib/aarch64-linux-gnu" \
  -DCMAKE_SHARED_LINKER_FLAGS="-stdlib=libc++ -L$CLANG_PATH/usr/lib/llvm-18/lib -L$CLANG_PATH/usr/lib/aarch64-linux-gnu -lc++ -lc++abi -lunwind -Wl,-rpath,$CLANG_PATH/usr/lib/llvm-18/lib -Wl,-rpath,$CLANG_PATH/usr/lib/aarch64-linux-gnu" \
  -DCMAKE_BUILD_TYPE=Release \
  -DSUNSHINE_BUILD_JETSON=ON \
  -DSUNSHINE_ENABLE_CUDA=OFF \
  -DSUNSHINE_ENABLE_VULKAN=OFF \
  -DSUNSHINE_ENABLE_WAYLAND=OFF \
  -DSUNSHINE_ENABLE_PORTAL=OFF \
  -DSUNSHINE_ENABLE_TRAY=OFF \
  -DBUILD_DOCS=OFF \
  -DBUILD_TESTS=OFF
```

### 3. Build

```bash
cmake --build . -j$(nproc)
```

Or use the convenience script:

```bash
cd ~/sunshine && ./build.sh
```

### 4. Install Web Assets

The web UI is built automatically by cmake (via npm/vite). The assets need to be copied to the path the binary expects:

```bash
sudo mkdir -p /usr/local/assets/web
sudo cp -r build/assets/web/* /usr/local/assets/web/
sudo cp -r build/assets/* /usr/local/assets/
```

### 5. Run

```bash
./build/sunshine
```

Access the web UI at `https://<jetson-ip>:47990`.

If you get a CSRF error, add your access origin to `~/.config/sunshine/sunshine.conf`:

```ini
csrf_allowed_origins = https://<jetson-ip>:47990
```

## Configuration

### Web UI

Settings are accessible in the web UI under the **Jetson nvv4l2 Encoder** tab (visible only on Jetson builds where `SUNSHINE_PLATFORM="jetson"`).

The platform detection works via cmake: when `SUNSHINE_BUILD_JETSON=ON`, the platform is reported as `"jetson"` to the web UI, which shows only relevant encoder tabs (nvv4l2 and software) and hides desktop-only encoders (NVENC, QuickSync, AMD AMF, etc.).

### Config File

Settings can also be changed directly in `~/.config/sunshine/sunshine.conf`:

| Option | Default | Range | Description |
|---|---|---|---|
| `nvv4l2_preset` | `2` | 1-4 | HW encoder preset. 1 = ultra fast (lowest quality), 2 = fast, 3 = medium, 4 = slow (highest quality). Maps to `V4L2_ENC_HW_PRESET_*` |
| `nvv4l2_rc_mode` | `1` | 0-1 | Rate control. 0 = VBR, 1 = CBR (recommended for streaming) |
| `nvv4l2_max_perf` | `enabled` | on/off | NVENC maximum performance mode. Recommended for streaming |
| `nvv4l2_idr_interval` | `0` | 0-60 | IDR frame interval in frames. 0 = all-IDR (every frame independently decodable). P-frames (>0) are currently incompatible with Moonlight's decoder |
| `nvv4l2_insert_sps_pps` | `enabled` | on/off | Insert SPS/PPS NAL units before IDR frames. Required by Moonlight |
| `nvv4l2_two_pass_cbr` | `enabled` | on/off | Two-pass CBR encoding via `V4L2_CID_MPEG_VIDEOENC_TWO_PASS_CBR`. Better quality but adds latency. Disable for maximum FPS |

All settings are wired from config.h through the encoder init and applied via the NvVideoEncoder API (`setHWPresetType()`, `setRateControlMode()`, `setControl()`, `setIDRInterval()`, `setInsertSpsPpsAtIdrEnabled()`, `setMaxPerfMode()`).

### Recommended Settings

For maximum FPS (lower quality):
```ini
nvv4l2_preset = 1
nvv4l2_rc_mode = 1
nvv4l2_max_perf = enabled
nvv4l2_idr_interval = 0
nvv4l2_insert_sps_pps = enabled
nvv4l2_two_pass_cbr = disabled
```

For balanced quality/performance:
```ini
nvv4l2_preset = 2
nvv4l2_rc_mode = 1
nvv4l2_max_perf = enabled
nvv4l2_idr_interval = 0
nvv4l2_insert_sps_pps = enabled
nvv4l2_two_pass_cbr = enabled
```

## Architecture

```
Screen Capture (KMS / X11)
    |
    v
BGRA frame (system memory)
    |  memcpy into persistently-mapped DMA-BUF
    v
NvBufSurface (BGRA, DMA-BUF)
    |  VIC hardware (NvBufSurfTransform)
    v
NvBufSurface (NV12, DMA-BUF)
    |  V4L2_MEMORY_DMABUF (zero-copy)
    v
NvVideoEncoder (V4L2 M2M hardware) [pipelined: encode N while preparing N+1]
    |
    v
H.264/HEVC bitstream (all-IDR)
    |
    v
RTP + FEC + encryption -> UDP to Moonlight
```

### Hardware Blocks Used

| Block | Purpose | Visible in top/tegrastats? |
|---|---|---|
| **CPU** (~28%) | BGRA memcpy into DMA-BUF | Yes |
| **VIC** | BGRA to NV12 color conversion | No (dedicated HW) |
| **NVENC** | H.264/HEVC encoding | No (dedicated HW) |
| **GPU** (~11%) | Minimal (display compositing) | Yes |

### Pipeline Timing

**At 60fps target (1080p all-IDR):**

| Step | Hardware | Time |
|---|---|---|
| memcpy BGRA into DMA-BUF | CPU | ~3ms |
| VIC BGRA to NV12 | VIC | ~1ms |
| H.264 encode (all-IDR) | NVENC | ~15ms |
| **Effective frame interval** (pipelined) | | **~15ms (66 FPS capable)** |

**At 140fps target (1080p all-IDR):**

| Step | Hardware | Time |
|---|---|---|
| memcpy BGRA into DMA-BUF | CPU | ~3ms |
| VIC BGRA to NV12 | VIC | ~1ms |
| H.264 encode (all-IDR, lower bits/frame) | NVENC | ~7.3ms |
| **Effective frame interval** (pipelined) | | **~7.3ms (137 FPS)** |

At higher FPS targets, the per-frame bitrate allocation is lower (same total bitrate / more frames), resulting in smaller IDR frames that NVENC encodes faster.

The 4ms CPU+VIC work overlaps with the NVENC encode of the previous frame via pipelining. Without pipelining, these are serial (19ms = 52 FPS at 60fps target).

### Key Source Files

| File | Purpose |
|---|---|
| `src/platform/linux/jetson/nvv4l2_encoder.h/.cpp` | NvVideoEncoder wrapper — init with full config wiring, pipelined encode, DMABUF/MMAP paths |
| `src/platform/linux/jetson/nvv4l2_encode_device.h/.cpp` | Device probing, VIC availability detection, encoder factory |
| `src/video.cpp` | `jetson_encode_session_t` — VIC conversion with persistent DMA-BUF mapping, fallback swscale, FPS logging |
| `src/config.h` | `config::video.nvv4l2` settings struct (preset, rc_mode, max_perf, idr_interval, two_pass_cbr, insert_sps_pps) |
| `src/config.cpp` | Config parsing: `int_between_f`, `bool_f` calls for all nvv4l2 settings |
| `src/platform/common.h` | `mem_type_e::dma` enum value, `img_t::buf_fd` field for DMA-BUF passing |
| `cmake/compile_definitions/linux.cmake` | Sets `SUNSHINE_PLATFORM="jetson"` when `SUNSHINE_BUILD_JETSON` is on |
| `cmake/dependencies/jetson.cmake` | Auto-detection of Jetson libraries and headers |
| `src_assets/.../configs/tabs/encoders/JetsonNvv4l2Encoder.vue` | Web UI encoder settings tab |
| `src_assets/.../configs/tabs/ContainerEncoders.vue` | Tab registration (nvv4l2 tab conditional render) |
| `src_assets/.../web/config.html` | Platform filtering — shows nvv4l2 tab only on `"jetson"` platform |
| `src_assets/.../locale/en.json` | English locale strings for nvv4l2 settings labels and descriptions |

### Config Wiring Flow

```
Web UI (JetsonNvv4l2Encoder.vue)
    |  v-model binds to config object
    v
POST /api/config  →  sunshine.conf file
    |
    v
config.cpp  →  int_between_f() / bool_f()  →  config::video.nvv4l2 struct
    |
    v
nvv4l2_encoder.cpp init()
    |  setHWPresetType()        ← nvv4l2_preset (1-4)
    |  setRateControlMode()     ← nvv4l2_rc_mode (0=VBR, 1=CBR)
    |  setMaxPerfMode()         ← nvv4l2_max_perf
    |  setControl(TWO_PASS_CBR) ← nvv4l2_two_pass_cbr
    |  setIDRInterval()         ← nvv4l2_idr_interval
    |  setInsertSpsPpsAtIdr()   ← nvv4l2_insert_sps_pps
    v
NvVideoEncoder (V4L2 M2M hardware)
```

## Known Limitations

### P-frames incompatible with Moonlight

P-frames from NvVideoEncoder cause "Missing reference picture" decode errors in Moonlight's FFmpeg H.264 decoder. Root cause is suspected to be in the CABAC-encoded macroblock data or constraint_set flags. All-IDR mode is used as a workaround — every frame is independently decodable.

### BGRA memcpy overhead

The screen capture subsystem (KMS/X11) delivers frames in system memory. A 3ms CPU memcpy copies the 8.3 MB BGRA frame into a DMA-BUF before VIC can process it. At current FPS targets this is hidden by pipeline overlap with NVENC, but it adds 3ms latency per frame. Eliminating this would require modifying the capture pipeline to write directly to DMA-BUF via `drmPrimeHandleToFD()`.

### NVENC is the throughput bottleneck

At all FPS targets, NVENC encode time is the limiting factor. The pipelined design means CPU+VIC work (~4ms) runs in parallel with NVENC, so reducing CPU/VIC time further would only improve latency, not throughput. To increase throughput beyond current limits, NVENC must encode faster — achievable by lowering preset, disabling two-pass CBR, or reducing per-frame bitrate.

## Reverting Changes

Backup files (`.bak`) are stored alongside modified files:

```bash
# Restore to pre-web-UI-wiring state
cp src/platform/linux/jetson/nvv4l2_encoder.cpp.pre_webui.bak src/platform/linux/jetson/nvv4l2_encoder.cpp
cp cmake/compile_definitions/linux.cmake.bak cmake/compile_definitions/linux.cmake

# Restore to pre-pipeline state (~52 FPS)
cp src/video.cpp.pre_pipeline.bak src/video.cpp
cp src/platform/linux/jetson/nvv4l2_encoder.cpp.pre_pipeline.bak src/platform/linux/jetson/nvv4l2_encoder.cpp
cp src/platform/linux/jetson/nvv4l2_encoder.h.pre_pipeline.bak src/platform/linux/jetson/nvv4l2_encoder.h

# Restore to original baseline (~40 FPS, swscale + MMAP)
cp src/video.cpp.bak src/video.cpp
cp src/platform/linux/jetson/nvv4l2_encoder.cpp.bak src/platform/linux/jetson/nvv4l2_encoder.cpp
cp src/platform/linux/jetson/nvv4l2_encoder.h.bak src/platform/linux/jetson/nvv4l2_encoder.h
cp src/platform/linux/jetson/nvv4l2_encode_device.cpp.bak src/platform/linux/jetson/nvv4l2_encode_device.cpp
cp src/platform/linux/jetson/nvv4l2_encode_device.h.bak src/platform/linux/jetson/nvv4l2_encode_device.h
cp cmake/dependencies/jetson.cmake.bak cmake/dependencies/jetson.cmake
```

Then rebuild.

## Troubleshooting

### White screen on web UI

The web assets need to be installed to the path the binary expects (`/usr/local/assets/web/`). See step 4 in Build Instructions.

### CSRF Protection Error

Add your access URL to `~/.config/sunshine/sunshine.conf`:

```ini
csrf_allowed_origins = https://<jetson-ip>:47990
```

### nvv4l2 encoder tab not visible in web UI

The tab only appears when the platform is reported as `"jetson"`. This requires building with `-DSUNSHINE_BUILD_JETSON=ON`, which sets `SUNSHINE_PLATFORM="jetson"` in `cmake/compile_definitions/linux.cmake`. If you see other encoder tabs (NVENC, QuickSync, etc.) instead, the build was not configured with the Jetson flag.

### Low FPS at 1080p (~40 FPS)

1. Check logs for `[DMABUF zero-copy]` — if you see `[MMAP]`, VIC acceleration is not active
2. Set `nvv4l2_preset = 1` for fastest encoding
3. Set `nvv4l2_max_perf = enabled`
4. Set `nvv4l2_two_pass_cbr = disabled` to remove two-pass overhead

### Encoder not detected

Check that JetPack Multimedia API is installed:

```bash
ls /usr/src/jetson_multimedia_api/include/NvVideoEncoder.h
ls /usr/lib/aarch64-linux-gnu/tegra/libnvv4l2.so
```

CMake should report:

```
[Jetson] L4T Multimedia API found at: /usr/src/jetson_multimedia_api/include
[Jetson] VIC hardware available — using DMABUF zero-copy encoder path
```

### Settings not taking effect

After changing settings in the web UI or config file, restart Sunshine. The encoder reads config values during `init()` at stream start. Check the log for the settings confirmation line:

```
Jetson NvVideoEncoder ready: 1920x1080 @ 20000 kbps 60fps H.264 preset=2 rc=CBR idr_interval=0 two_pass=1 sps_pps=1 output_bufs=10 [DMABUF zero-copy]
```
