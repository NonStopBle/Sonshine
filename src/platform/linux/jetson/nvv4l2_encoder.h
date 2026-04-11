/**
 * @file src/platform/linux/jetson/nvv4l2_encoder.h
 * @brief Jetson NvVideoEncoder Path B — direct Jetson Multimedia API.
 */
#pragma once
#if defined(SUNSHINE_BUILD_JETSON) && defined(HAVE_NVVIDEOENCO)

#include <NvVideoEncoder.h>
#include <NvBufSurface.h>
#include <nvbufsurface.h>
#include <vector>
#include <cstdint>
#include "src/platform/common.h"

namespace platf {

  /**
   * @brief Thin C++ wrapper around NvVideoEncoder for streaming use.
   *
   * Expects NV12 input (Y plane + UV plane contiguous in img.data).
   * Returns a single encoded frame buffer per call.
   */
  class NvV4L2HWEncoder {
  public:
    NvVideoEncoder *enc     = nullptr;
    int             width   = 0;
    int             height  = 0;
    int             framerate = 60;
    bool            started = false;

    uint32_t output_buf_count = 0;
    uint32_t queued_count = 0;
    bool dmabuf_mode = false;  // true = V4L2_MEMORY_DMABUF, false = V4L2_MEMORY_MMAP
    uint64_t pipeline_depth = 0;  // frames queued but not yet dequeued from capture plane

    bool init(int w, int h, int fps, int bitrate_bps, bool is_hevc = false, bool use_dmabuf = false);
    void force_idr();

    /**
     * @brief Encode a frame.
     * @param img NV12 image data.
     * @param frame_index Frame number.
     * @param force_idr Whether to force an IDR frame.
     * @return Single encoded frame buffer, or empty if no output ready.
     */
    std::vector<uint8_t> encode_single(const platf::img_t &img, uint64_t frame_index, bool force_idr);

    // Internal encode paths
    std::vector<uint8_t> encode_dmabuf(const platf::img_t &img, uint64_t frame_index);
    std::vector<uint8_t> encode_mmap(const platf::img_t &img, uint64_t frame_index);
    std::vector<uint8_t> dequeue_encoded_frame(uint64_t frame_index);

    ~NvV4L2HWEncoder();
  };

}  // namespace platf

#endif  // SUNSHINE_BUILD_JETSON && HAVE_NVVIDEOENCO
