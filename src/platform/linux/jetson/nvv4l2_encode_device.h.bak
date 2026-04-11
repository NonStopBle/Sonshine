/**
 * @file src/platform/linux/jetson/nvv4l2_encode_device.h
 * @brief Jetson Orin NX / AGX hardware encoder device utilities (V4L2 M2M).
 *
 * Path A (FFmpeg V4L2 M2M — h264_v4l2m2m / hevc_v4l2m2m):
 *   The encoder uses AV_HWDEVICE_TYPE_NONE, so Sunshine's existing
 *   avcodec_software_encode_device_t handles frame conversion (BGRA → NV12).
 *   No custom encode device is required — only the encoder_t registration in
 *   video.cpp is needed.
 *
 *   The nvv4l2_encode_device_t class in this file is used to:
 *     - Probe for available V4L2 M2M encoder devices (probe_device())
 *     - Hold a reference to /dev/videoN for Path B IDR injection
 *
 * Path B (NvVideoEncoder — requires HAVE_NVVIDEOENCO / libnvmedia):
 *   The NvV4L2HWEncoder class in nvv4l2_encoder.cpp provides lower IDR
 *   latency via NvVideoEncoder::setIDRFrame() at the V4L2 kernel driver level.
 */
#pragma once
#ifdef SUNSHINE_BUILD_JETSON

#include "src/platform/common.h"
#include "nvv4l2_encoder.h"

#include <linux/videodev2.h>
#include <memory>

namespace platf {

  /**
   * @brief Probe /dev/video* to find the first V4L2 M2M H.264 encoder.
   * @return File descriptor (caller must close), or -1 if not found.
   */
  int find_v4l2_m2m_h264_encoder_fd();

  /**
   * @brief Jetson V4L2 M2M encode device.
   *
   * Extends avcodec_encode_device_t without overriding convert() — when
   * data == nullptr (the default for avcodec_encode_device_t), Sunshine's
   * video.cpp automatically uses avcodec_software_encode_device_t for the
   * BGRA → NV12 conversion required by the V4L2 M2M encoder.
   *
   * This class exists to:
   *   1. Probe V4L2 device availability (probe_device())
   *   2. Keep the V4L2 fd open for potential Path B IDR injection
   *   3. Hold the native NvV4L2HWEncoder for Path B.
   */
  struct nvv4l2_encode_device_t: avcodec_encode_device_t {
    /**
     * @brief Check that a V4L2 M2M H.264 encoder node exists.
     * @return true if a compatible /dev/videoN device was found.
     */
    static bool probe_device();

    // V4L2 device fd — held open during the session for Path B IDR injection.
    // Kept at -1 when Path B (libnvmedia) is not available.
    int v4l2_device_fd = -1;

    // true if VIC hardware is available for BGRA→NV12 conversion
    bool vic_available = false;

#if defined(HAVE_NVVIDEOENCO)
    std::unique_ptr<NvV4L2HWEncoder> jetson_encoder;
#endif

    ~nvv4l2_encode_device_t() override;
  };

  std::unique_ptr<nvv4l2_encode_device_t> make_jetson_encode_device(int w, int h, const ::video::config_t &config, const ::video::sunshine_colorspace_t &colorspace);

}  // namespace platf

#endif  // SUNSHINE_BUILD_JETSON
