/**
 * @file src/platform/linux/jetson/nvv4l2_encoder.cpp
 * @brief Jetson NvVideoEncoder Path B — direct Jetson Multimedia API.
 *
 * Uses NvVideoEncoder (V4L2 M2M) with MMAP buffers.
 * The session provides NV12 data (converted from BGRA via swscale).
 * We copy NV12 directly into V4L2 output plane buffers.
 */
#if defined(SUNSHINE_BUILD_JETSON) && defined(HAVE_NVVIDEOENCO)

#include "nvv4l2_encoder.h"
#include "nvv4l2_encode_device.h"
#include "src/logging.h"
#include "src/config.h"

#include <v4l2_nv_extensions.h>

using namespace std::literals;

namespace platf {

    bool NvV4L2HWEncoder::init(int w, int h, int fps, int bitrate_bps, bool is_hevc, bool use_dmabuf) {
      width  = w;
      height = h;
      framerate = fps;
      dmabuf_mode = use_dmabuf;

      enc = NvVideoEncoder::createVideoEncoder("nvv4l2-enc");
      if (!enc) {
        BOOST_LOG(error) << "Jetson: NvVideoEncoder::createVideoEncoder failed"sv;
        return false;
      }

      uint32_t codec = is_hevc ? V4L2_PIX_FMT_H265 : V4L2_PIX_FMT_H264;

      // Encoded bitstream output format
      if (enc->setCapturePlaneFormat(codec, w, h, 2 * 1024 * 1024) < 0) {
        BOOST_LOG(error) << "Jetson: NvVideoEncoder setCapturePlaneFormat failed"sv;
        return false;
      }

      // NV12 pixel input format (multi-plane: Y + UV)
      if (enc->setOutputPlaneFormat(V4L2_PIX_FMT_NV12M, w, h) < 0) {
        BOOST_LOG(error) << "Jetson: NvVideoEncoder setOutputPlaneFormat failed"sv;
        return false;
      }

      // Use full client-requested bitrate — no artificial cap.
      // The old 5Mbps cap was needed when IDR frames were being concatenated
      // into mega-packets; with the single-frame dequeue fix, each IDR is
      // sent as its own packet and decodes quickly.
      enc->setBitrate(bitrate_bps);

      // Rate control from config: 0 = VBR, 1 = CBR (default)
      if (config::video.nvv4l2.rc_mode == 0) {
        enc->setRateControlMode(V4L2_MPEG_VIDEO_BITRATE_MODE_VBR);
      } else {
        enc->setRateControlMode(V4L2_MPEG_VIDEO_BITRATE_MODE_CBR);
      }

      enc->setFrameRate(fps, 1);
      enc->setNumBFrames(0);

      // Encoder preset from config (1-4, default 2)
      // Maps to V4L2_MPEG_VIDEO_H264_HW_LEVEL_ENCODER preset
      if (config::video.nvv4l2.preset >= 1 && config::video.nvv4l2.preset <= 4) {
        enc->setHWPresetType(static_cast<v4l2_enc_hw_preset_type>(config::video.nvv4l2.preset));
      }

      // Baseline profile — CAVLC is faster to software-decode than CABAC (High)
      if (is_hevc) {
        enc->setProfile(V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN);
        enc->setLevel(V4L2_MPEG_VIDEO_HEVC_LEVEL_5);
      } else {
        enc->setProfile(V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE);
        enc->setLevel(V4L2_MPEG_VIDEO_H264_LEVEL_5_1);
      }

      if (config::video.nvv4l2.max_perf) {
        enc->setMaxPerfMode(1);
      }

      // Two-pass CBR from config (no wrapper method, use V4L2 control directly)
      if (config::video.nvv4l2.two_pass_cbr && config::video.nvv4l2.rc_mode == 1) {
        enc->setControl(V4L2_CID_MPEG_VIDEOENC_TWO_PASS_CBR, 1);
      }

      // IDR interval from config (0 = all-IDR, >0 = P-frames between IDRs)
      // WARNING: P-frames are currently incompatible with Moonlight's decoder.
      // Use idr_interval=0 (all-IDR) unless testing P-frame compatibility.
      if (config::video.nvv4l2.idr_interval == 0) {
        enc->setIDRInterval(1);
        enc->setIFrameInterval(0);
      } else {
        enc->setIDRInterval(config::video.nvv4l2.idr_interval);
        enc->setIFrameInterval(config::video.nvv4l2.idr_interval);
      }

      enc->setInsertSpsPpsAtIdrEnabled(config::video.nvv4l2.insert_sps_pps);

      // Setup planes: output (raw frames) and capture (encoded bitstream)
      // DMABUF mode: output plane accepts external DMA-BUF fds, no internal mapping needed
      // MMAP mode: output plane uses kernel-allocated mapped buffers
      auto out_mem = dmabuf_mode ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;
      bool map_out = !dmabuf_mode;  // only map for MMAP mode
      if (enc->output_plane.setupPlane(out_mem, 10, map_out, false) < 0 ||
          enc->capture_plane.setupPlane(V4L2_MEMORY_MMAP, 6, true, false) < 0) {
        BOOST_LOG(error) << "Jetson: NvVideoEncoder plane setup failed"sv;
        return false;
      }

      enc->output_plane.setStreamStatus(true);
      enc->capture_plane.setStreamStatus(true);

      // Pre-queue capture plane buffers only
      for (uint32_t i = 0; i < enc->capture_plane.getNumBuffers(); i++) {
        struct v4l2_buffer v4l2_buf;
        struct v4l2_plane planes[MAX_PLANES];
        std::memset(&v4l2_buf, 0, sizeof(v4l2_buf));
        std::memset(planes, 0, sizeof(planes));

        v4l2_buf.index = i;
        v4l2_buf.m.planes = planes;
        v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        v4l2_buf.memory = V4L2_MEMORY_MMAP;

        if (enc->capture_plane.qBuffer(v4l2_buf, nullptr) < 0) {
          BOOST_LOG(error) << "Jetson: capture qBuffer failed during init"sv;
          return false;
        }
      }

      output_buf_count = enc->output_plane.getNumBuffers();
      queued_count = 0;

      started = true;
      BOOST_LOG(info) << "Jetson NvVideoEncoder ready: "sv
                      << w << "x"sv << h
                      << " @ "sv << bitrate_bps / 1000 << " kbps "sv << fps << "fps"sv
                      << (is_hevc ? " HEVC"sv : " H.264"sv)
                      << " preset="sv << config::video.nvv4l2.preset
                      << " rc="sv << (config::video.nvv4l2.rc_mode == 0 ? "VBR"sv : "CBR"sv)
                      << " idr_interval="sv << config::video.nvv4l2.idr_interval
                      << " two_pass="sv << config::video.nvv4l2.two_pass_cbr
                      << " sps_pps="sv << config::video.nvv4l2.insert_sps_pps
                      << " output_bufs="sv << output_buf_count
                      << (dmabuf_mode ? " [DMABUF zero-copy]"sv : " [MMAP]"sv);
      return true;
    }

    void NvV4L2HWEncoder::force_idr() {
      if (enc) {
        enc->forceIDR();
      }
    }

    std::vector<uint8_t> NvV4L2HWEncoder::encode_single(const platf::img_t &img, uint64_t frame_index, bool force_idr_flag) {
      if (!enc) return {};

      if (force_idr_flag) {
        enc->forceIDR();
      }

      // DMA-BUF zero-copy path: pass NV12 DMA-BUF fd directly to V4L2 encoder
      if (img.buf_fd >= 0 && img.mem_type == platf::mem_type_e::dma) {
        return encode_dmabuf(img, frame_index);
      }

      // Fallback: MMAP path with CPU memcpy
      return encode_mmap(img, frame_index);
    }

    std::vector<uint8_t> NvV4L2HWEncoder::encode_dmabuf(const platf::img_t &img, uint64_t frame_index) {
      struct v4l2_buffer v4l2_buf;
      struct v4l2_plane planes[MAX_PLANES];

      std::memset(&v4l2_buf, 0, sizeof(v4l2_buf));
      std::memset(planes, 0, sizeof(planes));
      v4l2_buf.m.planes = planes;

      // Recycle a previously queued buffer if we've used all initial slots
      if (queued_count >= output_buf_count) {
        NvBuffer *dummy = nullptr;
        int ret = enc->output_plane.dqBuffer(v4l2_buf, &dummy, nullptr, 1000);
        if (ret < 0) {
          if (frame_index <= 5 || frame_index % 60 == 0) {
            BOOST_LOG(warning) << "Jetson: DMABUF output dqBuffer failed for frame " << frame_index;
          }
          return {};
        }
      } else {
        v4l2_buf.index = queued_count;
      }

      v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
      v4l2_buf.memory = V4L2_MEMORY_DMABUF;

      // Get plane info from NvBufSurface to set bytesused correctly
      NvBufSurface *surf = nullptr;
      if (NvBufSurfaceFromFd(img.buf_fd, (void **)&surf) < 0 || !surf) {
        BOOST_LOG(error) << "Jetson: NvBufSurfaceFromFd failed for encoder input";
        return {};
      }

      // NV12M has 2 planes: Y and UV. Set the DMA-BUF fd for each plane.
      for (uint32_t i = 0; i < 2; i++) {
        planes[i].m.fd = img.buf_fd;
        planes[i].bytesused = surf->surfaceList[0].planeParams.psize[i];
        planes[i].data_offset = surf->surfaceList[0].planeParams.offset[i];
      }

      uint64_t ts_us = frame_index * (1000000ULL / framerate);
      v4l2_buf.timestamp.tv_sec  = ts_us / 1000000;
      v4l2_buf.timestamp.tv_usec = ts_us % 1000000;
      v4l2_buf.length = 2;

      if (enc->output_plane.qBuffer(v4l2_buf, nullptr) < 0) {
        BOOST_LOG(error) << "Jetson: DMABUF output qBuffer failed for frame " << frame_index;
        return {};
      }
      queued_count++;
      pipeline_depth++;

      // Pipelined: first frame has nothing to dequeue yet — NVENC is working on it.
      // For subsequent frames, dequeue the PREVIOUS frame's encoded output
      // (which NVENC finished while we were doing memcpy+VIC for this frame).
      if (pipeline_depth <= 1) {
        return {};
      }

      return dequeue_encoded_frame(frame_index);
    }

    std::vector<uint8_t> NvV4L2HWEncoder::encode_mmap(const platf::img_t &img, uint64_t frame_index) {
      struct v4l2_buffer v4l2_buf;
      struct v4l2_plane planes[MAX_PLANES];
      NvBuffer *buffer = nullptr;

      std::memset(&v4l2_buf, 0, sizeof(v4l2_buf));
      std::memset(planes, 0, sizeof(planes));
      v4l2_buf.m.planes = planes;

      // 1. Get a buffer for the raw frame
      if (queued_count < output_buf_count) {
        buffer = enc->output_plane.getNthBuffer(queued_count);
        v4l2_buf.index = queued_count;
        v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        v4l2_buf.memory = V4L2_MEMORY_MMAP;
      } else {
        int ret = enc->output_plane.dqBuffer(v4l2_buf, &buffer, nullptr, 1000);
        if (ret < 0) {
          if (frame_index <= 5 || frame_index % 60 == 0) {
            BOOST_LOG(warning) << "Jetson: output dqBuffer failed for frame " << frame_index;
          }
          return {};
        }
      }

      // 2. Copy NV12 data into V4L2 buffer planes (row-by-row for stride)
      if (img.data && buffer) {
        uint32_t uv_height = height / 2;

        NvBuffer::NvBufferPlane &y_plane = buffer->planes[0];
        uint8_t *src_y = img.data;
        uint8_t *dst_y = (uint8_t *)y_plane.data;
        uint32_t y_stride = y_plane.fmt.stride;

        for (int row = 0; row < height; row++) {
          std::memcpy(dst_y + row * y_stride, src_y + row * width, width);
        }
        y_plane.bytesused = y_stride * height;

        NvBuffer::NvBufferPlane &uv_plane = buffer->planes[1];
        uint8_t *src_uv = img.data + (width * height);
        uint8_t *dst_uv = (uint8_t *)uv_plane.data;
        uint32_t uv_stride = uv_plane.fmt.stride;

        for (uint32_t row = 0; row < uv_height; row++) {
          std::memcpy(dst_uv + row * uv_stride, src_uv + row * width, width);
        }
        uv_plane.bytesused = uv_stride * uv_height;
      }

      // 3. Set bytesused, timestamp, and queue.
      for (uint32_t i = 0; i < buffer->n_planes; i++) {
        v4l2_buf.m.planes[i].bytesused = buffer->planes[i].bytesused;
      }
      uint64_t ts_us = frame_index * (1000000ULL / framerate);
      v4l2_buf.timestamp.tv_sec  = ts_us / 1000000;
      v4l2_buf.timestamp.tv_usec = ts_us % 1000000;

      if (enc->output_plane.qBuffer(v4l2_buf, nullptr) < 0) {
        BOOST_LOG(error) << "Jetson: output qBuffer failed for frame " << frame_index;
        return {};
      }
      queued_count++;
      pipeline_depth++;

      if (pipeline_depth <= 1) {
        return {};
      }

      return dequeue_encoded_frame(frame_index);
    }

    std::vector<uint8_t> NvV4L2HWEncoder::dequeue_encoded_frame(uint64_t frame_index) {
      std::vector<uint8_t> frame_data;

      struct v4l2_buffer cap_buf;
      struct v4l2_plane cap_planes[MAX_PLANES];
      NvBuffer *cap_buffer = nullptr;

      std::memset(&cap_buf, 0, sizeof(cap_buf));
      std::memset(cap_planes, 0, sizeof(cap_planes));
      cap_buf.m.planes = cap_planes;

      // Wait up to 2 frame periods for the encoder to produce output
      int timeout_ms = std::max(2, 2000 / framerate);
      if (enc->capture_plane.dqBuffer(cap_buf, &cap_buffer, nullptr, timeout_ms) >= 0) {
        uint32_t bytes = cap_buf.m.planes[0].bytesused;
        if (bytes > 0) {
          uint8_t *data = cap_buffer->planes[0].data;
          frame_data.assign(data, data + bytes);
        }

        // Re-queue capture buffer
        if (enc->capture_plane.qBuffer(cap_buf, nullptr) < 0) {
          BOOST_LOG(error) << "Jetson: capture re-qBuffer failed"sv;
        }
      }

      return frame_data;
    }

    NvV4L2HWEncoder::~NvV4L2HWEncoder() {
      if (enc) {
        if (started) {
          enc->output_plane.setStreamStatus(false);
          enc->capture_plane.setStreamStatus(false);
        }
        delete enc;
        enc = nullptr;
      }
    }

}  // namespace platf

#endif  // SUNSHINE_BUILD_JETSON && HAVE_NVVIDEOENCO
