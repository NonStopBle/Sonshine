/**
 * @file src/platform/linux/jetson/nvv4l2_encode_device.cpp
 * @brief Jetson V4L2 M2M encode device implementation.
 *
 * Implements the device-probing utilities and lifecycle management for the
 * Jetson nvv4l2 encoder path.
 *
 * For Path A (FFmpeg h264_v4l2m2m / hevc_v4l2m2m), the actual frame
 * conversion from BGRA to NV12 is handled by Sunshine's existing
 * avcodec_software_encode_device_t (triggered when encode_device->data == nullptr).
 * This file only adds device probing and the V4L2 fd management.
 */
#ifdef SUNSHINE_BUILD_JETSON

#include "nvv4l2_encode_device.h"
#include "src/logging.h"
#include "src/video.h"

#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <cstring>
#include <string>
#include <sys/ioctl.h>
#include <NvBufSurface.h>
#include <nvbufsurface.h>

using namespace std::literals;

namespace platf {

  // -------------------------------------------------------------------------
  // find_v4l2_m2m_h264_encoder_fd
  //
  // Enumerate /dev/video* and return the first device that reports:
  //   V4L2_CAP_VIDEO_M2M  (single-plane) or V4L2_CAP_VIDEO_M2M_MPLANE
  //   + V4L2_CAP_STREAMING
  // and lists V4L2_PIX_FMT_H264 as a capturable output format.
  //
  // On Jetson Orin, the relevant device is typically /dev/video0 and is
  // provided by the nvhost-msenc driver (JetPack 5.x / 6.x).
  // -------------------------------------------------------------------------
  int find_v4l2_m2m_h264_encoder_fd() {
    DIR *dir = opendir("/dev");
    if (!dir) {
      BOOST_LOG(error) << "Jetson: cannot open /dev to enumerate V4L2 devices"sv;
      return -1;
    }

    struct dirent *entry;
    std::vector<std::string> devices;

    while ((entry = readdir(dir)) != nullptr) {
      if (strncmp(entry->d_name, "video", 5) == 0) {
        devices.push_back("/dev/"s + entry->d_name);
      }
    }
    closedir(dir);

    // Also check the specific nvhost device just in case
    devices.push_back("/dev/nvhost-msenc"s);

    for (const auto &path : devices) {
      int fd = open(path.c_str(), O_RDWR | O_NONBLOCK);
      if (fd < 0) continue;

      struct v4l2_capability cap {};
      if (ioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) {
        close(fd);
        continue;
      }

      const bool is_m2m =
        ((cap.capabilities & V4L2_CAP_VIDEO_M2M) ||
         (cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE)) &&
        (cap.capabilities & V4L2_CAP_STREAMING);

      if (!is_m2m) {
        // If it's the nvhost device, we might not get V4L2 caps but Path B can still use it.
        // However, this function is specifically for Path A (V4L2 M2M).
        close(fd);
        continue;
      }

      // Check multi-planar capture type first (Jetson nvhost-msenc uses MPLANE)
      bool h264_found = false;
      for (int type : { V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_BUF_TYPE_VIDEO_CAPTURE }) {
        struct v4l2_fmtdesc fmt {};
        fmt.index = 0;
        fmt.type  = type;
        while (ioctl(fd, VIDIOC_ENUM_FMT, &fmt) == 0) {
          if (fmt.pixelformat == V4L2_PIX_FMT_H264) {
            h264_found = true;
            break;
          }
          fmt.index++;
        }
        if (h264_found) break;
      }

      if (h264_found) {
        BOOST_LOG(info) << "Jetson: V4L2 M2M H.264 encoder found at "sv << path;
        return fd;
      }
      close(fd);
    }

    BOOST_LOG(warning) << "Jetson: no V4L2 M2M H.264 encoder found in /dev"sv;
    return -1;
  }

  // -------------------------------------------------------------------------
  // nvv4l2_encode_device_t::probe_device
  // -------------------------------------------------------------------------
  bool nvv4l2_encode_device_t::probe_device() {
    int fd = find_v4l2_m2m_h264_encoder_fd();
    if (fd >= 0) {
      close(fd);
      return true;
    }
    return false;
  }

  // -------------------------------------------------------------------------
  // nvv4l2_encode_device_t destructor
  // -------------------------------------------------------------------------
  nvv4l2_encode_device_t::~nvv4l2_encode_device_t() {
    if (v4l2_device_fd >= 0) {
      close(v4l2_device_fd);
      v4l2_device_fd = -1;
    }
  }

  static bool probe_vic_available() {
    // Try allocating a small NvBufSurface to check if VIC/NvBufSurfTransform is available
    int test_fd = -1;
    NvBufSurf::NvCommonAllocateParams params = {};
    params.width = 64;
    params.height = 64;
    params.colorFormat = NVBUF_COLOR_FORMAT_NV12;
    params.layout = NVBUF_LAYOUT_PITCH;
    params.memType = NVBUF_MEM_SURFACE_ARRAY;
    params.memtag = NvBufSurfaceTag_VIDEO_CONVERT;

    if (NvBufSurf::NvAllocate(&params, 1, &test_fd) < 0 || test_fd < 0) {
      return false;
    }
    NvBufSurf::NvDestroy(test_fd);
    return true;
  }

  std::unique_ptr<nvv4l2_encode_device_t> make_jetson_encode_device(int w, int h, const ::video::config_t &config, const ::video::sunshine_colorspace_t &colorspace) {
    auto device = std::make_unique<nvv4l2_encode_device_t>();
    device->v4l2_device_fd = find_v4l2_m2m_h264_encoder_fd();

#if defined(HAVE_NVVIDEOENCO)
    device->jetson_encoder = std::make_unique<NvV4L2HWEncoder>();

    // Check if HEVC is requested: videoFormat 1 = HEVC, 0 = H.264
    bool is_hevc = (config.videoFormat == 1);

    // Probe VIC hardware for zero-copy DMABUF path
    bool use_dmabuf = probe_vic_available();
    if (use_dmabuf) {
      BOOST_LOG(info) << "Jetson: VIC hardware available — using DMABUF zero-copy encoder path"sv;
    } else {
      BOOST_LOG(info) << "Jetson: VIC not available — using MMAP encoder path with swscale"sv;
    }

    if (!device->jetson_encoder->init(w, h, config.framerate, config.bitrate * 1000, is_hevc, use_dmabuf)) {
      BOOST_LOG(warning) << "Jetson: Path B NvVideoEncoder failed to initialize, falling back to Path A"sv;
      device->jetson_encoder.reset();
    } else {
      BOOST_LOG(info) << "Jetson: Path B NvVideoEncoder initialized successfully"sv;
      device->vic_available = use_dmabuf;
    }
#endif

    return device;
  }

}  // namespace platf

#endif  // SUNSHINE_BUILD_JETSON
