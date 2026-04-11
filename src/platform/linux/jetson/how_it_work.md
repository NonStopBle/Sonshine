# Jetson NvVideoEncoder — How It Works

## Overview

Sunshine streams the Jetson desktop to Moonlight clients using the Jetson Multimedia API (`NvVideoEncoder`) for hardware H.264 encoding. This is "Path B" — a direct encoder path that bypasses FFmpeg's avcodec layer for lower latency.

### Architecture

```
Screen Capture (KMS/X11)
    |
    v
BGRA frame (img_t)
    |
    v  [jetson_encode_session_t::convert()]
swscale: BGRA -> NV12
    |
    v  [NvV4L2HWEncoder::encode_single()]
NvVideoEncoder (V4L2 M2M hardware encoder)
    |
    v
H.264 Annex B bitstream (IDR frames with SPS/PPS)
    |
    v  [encode_jetson() in video.cpp]
packet_raw_generic (NAL parsing, frame numbering)
    |
    v  [videoBroadcastThread() in stream.cpp]
RTP packetization + FEC + encryption -> UDP to Moonlight
```

## Key Files

| File | Purpose |
|------|---------|
| `nvv4l2_encoder.h` | `NvV4L2HWEncoder` class definition |
| `nvv4l2_encoder.cpp` | Encoder init, encode_single(), destructor |
| `nvv4l2_encode_device.h` | `nvv4l2_encode_device_t` — device probing, holds encoder instance |
| `nvv4l2_encode_device.cpp` | V4L2 M2M device enumeration, encoder factory |
| `src/video.cpp` | `jetson_encode_session_t` (BGRA->NV12 conversion), `encode_jetson()` (frame numbering + packet emission) |
| `src/video.h` | `packet_raw_generic` struct for non-FFmpeg encoded data |
| `src/stream.cpp` | `videoBroadcastThread()` — frame headers, RTP, FEC |
| `src/config.h` | `config::video.nvv4l2` settings (max_perf, idr_interval, etc.) |

## Encoder Configuration (Current — All-IDR Mode)

```cpp
// Bitrate capped to 5Mbps for fast client decode (~83KB/frame at 60fps)
int max_idr_bitrate = 5 * 1000 * 1000;
enc->setBitrate(std::min(bitrate_bps, max_idr_bitrate));
enc->setRateControlMode(V4L2_MPEG_VIDEO_BITRATE_MODE_CBR);
enc->setNumBFrames(0);

// Baseline profile — CAVLC is faster to decode than CABAC (High profile)
enc->setProfile(V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE);
enc->setLevel(V4L2_MPEG_VIDEO_H264_LEVEL_5_1);

// IDR every frame — Moonlight can decode each frame independently
enc->setIDRInterval(1);
enc->setIFrameInterval(0);
enc->setInsertSpsPpsAtIdrEnabled(true);  // SPS+PPS before every IDR
```

### Why All-IDR?

P-frames from NvVideoEncoder fail to decode on Moonlight's FFmpeg H.264 decoder with "Missing reference picture" errors. The bitstream was analyzed and verified correct (SPS max_num_ref_frames=1, PPS num_ref_idx_l0=0, frame_num incrementing properly), but P-frames still fail. The root cause is likely in the CABAC-encoded macroblock data or a subtle incompatibility with FFmpeg's reference picture management.

All-IDR mode means every frame is independently decodable — no reference frame dependencies — so Moonlight always succeeds.

### Why Bitrate Cap at 5Mbps?

At full bitrate (e.g. 20Mbps), each IDR frame is ~200KB and takes ~40ms to decode on the client. At 60fps, each frame budget is 16.6ms — so 40ms decode = ~25fps max. Capping to 5Mbps produces ~83KB frames that decode in ~10-15ms, enabling 60fps.

**Tradeoff**: Lower bitrate = lower image quality. Adjust `max_idr_bitrate` in `nvv4l2_encoder.cpp` to balance FPS vs quality.

### Why Baseline Profile?

Baseline uses CAVLC entropy coding (simpler than CABAC in High profile). CAVLC is faster for software decoders (FFmpeg) to decode, reducing per-frame decode time.

## V4L2 Buffer Pipeline

```
Output Plane (raw NV12 input):
  - 10 MMAP buffers
  - First 10 frames: direct getNthBuffer() + qBuffer()
  - Frame 11+: dqBuffer() to reclaim, then copy + qBuffer()

Capture Plane (encoded H.264 output):
  - 6 MMAP buffers
  - Pre-queued at init
  - After each input frame: dqBuffer(16ms timeout) to get encoded data
  - Re-queue after reading
```

### Capture Dequeue Strategy

```cpp
// First attempt: 16ms blocking wait (one frame period at 60fps)
// Subsequent attempts: non-blocking to drain any extra buffered output
int timeout_ms = first_attempt ? 16 : 0;
enc->capture_plane.dqBuffer(cap_buf, &cap_buffer, nullptr, timeout_ms);
```

The 16ms timeout ensures we don't waste time waiting when the encoder hasn't finished yet, while still giving enough time for normal encode latency.

## Frame Numbering (Critical for Moonlight)

Moonlight's `VideoDepacketizer.c` expects **strictly consecutive frameIndex** values (1, 2, 3, ...). Any gap triggers frame drop mode where ALL subsequent frames are discarded until the next IDR.

Since the V4L2 encoder is asynchronous (input frame N may not produce output immediately), we use a **separate output frame counter** in `video.cpp`:

```cpp
static int64_t jetson_output_frame_nr = 0;  // only increments when we emit a packet

// In encode_jetson():
for (auto &frame : encoded_frames) {
    int64_t output_nr = ++jetson_output_frame_nr;  // consecutive, no gaps
    auto packet = std::make_unique<packet_raw_generic>(std::move(frame), output_nr, has_idr);
    packets->raise(std::move(packet));
}
```

The counter resets when a new session starts (detected by `frame_nr <= jetson_last_input_frame_nr`).

## Moonlight Protocol Integration

### Frame Headers (stream.cpp)

```cpp
video_short_frame_header_t frame_header = {};
frame_header.headerType = 0x01;  // 8-byte short header
frame_header.frameType = packet->is_idr() ? 2 : 1;  // 2=IDR, 1=P-frame
```

### IDR Detection in Moonlight

Moonlight does NOT trust the frame header `frameType` for H.264. Instead it parses the NAL data:
- `isIdrFrameStart()` checks for **SPS NAL (type 7)** — NOT IDR slice (type 5)
- If SPS is found → slow path: splits NALs individually, sets `waitingForIdrFrame = false`
- If no SPS → fast path: queues raw data as single fragment

This is why `setInsertSpsPpsAtIdrEnabled(true)` is essential — without SPS prepended, Moonlight won't recognize our IDR frames.

### Moonlight's Drop Behavior

When `strictIdrFrameWait = true` (always true when reference frame invalidation is unsupported — our case):
- **Any decode error** → `dropFrameState()` → `waitingForIdrFrame = true`
- **All frames dropped** until next IDR with SPS
- This is why P-frame decode failures cause 0 FPS

## BGRA to NV12 Conversion (video.cpp)

`jetson_encode_session_t::convert()` uses FFmpeg's swscale:

```cpp
sws_getContext(src_w, src_h, AV_PIX_FMT_BGRA,
               dst_w, dst_h, AV_PIX_FMT_NV12,
               SWS_FAST_BILINEAR, ...);

// NV12 layout in nv12_buffer:
// [0 .. w*h)       = Y plane
// [w*h .. w*h*3/2) = UV plane (interleaved)
```

The NV12 data is then copied row-by-row into V4L2 MMAP buffer planes (accounting for stride alignment):

```cpp
for (int row = 0; row < height; row++)
    memcpy(dst_y + row * y_stride, src_y + row * width, width);
for (int row = 0; row < uv_height; row++)
    memcpy(dst_uv + row * uv_stride, src_uv + row * width, width);
```

## Known Issues and Future Work

### P-Frame Decode Failure (Unsolved)

**Symptoms**: Moonlight shows "Missing reference picture" / "decode_slice_header error" on every P-frame. Screen freezes (0 FPS).

**Verified correct**:
- SPS: profile=High(100), level=5.1, max_num_ref_frames=1
- PPS: num_ref_idx_l0_default_active_minus1=0, CABAC, deblocking enabled
- Slice headers: frame_num incrementing (0,1,2...), poc_lsb incrementing (0,2,4...)
- No reference picture list modification

**Suspected cause**: Something in the CABAC-encoded macroblock data or the constraint_set4/5 flags (both set to 1 by the Jetson encoder) that FFmpeg's decoder doesn't handle correctly.

**Workaround**: All-IDR mode (current).

**Future fix (Path 2)**: Use FFmpeg's `h264_v4l2m2m` encoder wrapper instead of direct NvVideoEncoder. Same Jetson hardware, but FFmpeg handles H.264 bitstream compliance. P-frames from FFmpeg's wrapper are known to work with Moonlight.

### Bitrate vs Quality Tradeoff

Current 5Mbps cap trades quality for FPS. To adjust:
- Edit `max_idr_bitrate` in `nvv4l2_encoder.cpp`
- Higher = better quality, slower decode, lower FPS
- Lower = worse quality, faster decode, higher FPS
- Sweet spot depends on client hardware (software vs hardware decoder)

## Build

```bash
bash /home/orin/sunshine/build.sh
# or manually:
cd /home/orin/sunshine/Sunshine/build && make sunshine -j$(nproc)
```

Requires:
- `SUNSHINE_BUILD_JETSON` defined
- `HAVE_NVVIDEOENCO` defined (for Path B / NvVideoEncoder)
- Jetson Multimedia API headers (`NvVideoEncoder.h`, `v4l2_nv_extensions.h`)
- JetPack 5.x or 6.x
