#include "detector_utils.hh"

#include <cstdint>
#include <span>
#include <stdfloat>
#include <string>
#include <vector>

namespace XTCPP {
  void calibrate(std::string dettype,
                 std::vector<void*>& data_ptrs,
                 std::span<CalibStruct>& calibconst,
                 std::vector<std::float32_t>& calib_data) {
    if (dettype == "jungfrau") {
      calibrate_jungfrau(data_ptrs, calibconst, calib_data);
    } else if (dettype == "epix100") {
      calibrate_epix100(data_ptrs, calibconst, calib_data);
    }
  }

  void calibrate_segment(std::string dettype,
                         size_t segment,
                         std::vector<void*>& data_ptrs,
                         std::span<CalibStruct>& calibconst,
                         std::vector<std::float32_t>& calib_data) {
    if (dettype == "jungfrau") {
      calibrate_jungfrau(segment, data_ptrs, calibconst, calib_data);
    } else if (dettype == "epix100") {
      calibrate_epix100(segment, data_ptrs, calibconst, calib_data);
    }
  }

  void calibrate_jungfrau(size_t segment,
                          std::vector<void*>& data_ptrs,
                          std::span<CalibStruct>& calibconst,
                          std::vector<std::float32_t>& calib_data) {
    auto data_mask = 0x3FFF;
    constexpr size_t NSEGS{32};
    constexpr size_t NROWS{512};
    constexpr size_t NCOLS{1024};
    constexpr size_t NPIX {NSEGS * NROWS * NCOLS};
    constexpr size_t PIX_PER_SEG {NROWS * NCOLS};

    //#pragma omp parallel for schedule(static)
    auto* raw_data = reinterpret_cast<std::uint16_t*>(data_ptrs[segment]);

    for (size_t panel_idx=0; panel_idx < PIX_PER_SEG; ++panel_idx) {
      size_t idx = segment*PIX_PER_SEG + panel_idx;

      uint16_t raw_pixel = raw_data[panel_idx];
      uint16_t data = raw_pixel & data_mask;
      size_t gain_idx = raw_pixel >> 14; // Top two bits
      if (gain_idx > 1) {
        if (gain_idx == 2) [[unlikely]] {
          /* This is 0b10 - a bad pixel, hopefully unlikely - what should happen? */
          continue;
        } else [[likely]] {
          gain_idx--; // Map gain_idx 3 (0b11 - low gain) to index 2
        }
      }
      size_t calib_idx = gain_idx * NPIX + idx;

      if (calib_idx >= calibconst.size()) {
        continue;
      }
      calib_data[idx] =
        (data - calibconst[calib_idx].ped) * calibconst[calib_idx].gain;
    }
  }


  void calibrate_jungfrau(std::vector<void*>& data_ptrs,
                          std::span<CalibStruct>& calibconst,
                          std::vector<std::float32_t>& calib_data) {
    auto data_mask = 0x3FFF;
    constexpr size_t NSEGS{32};
    constexpr size_t NROWS{512};
    constexpr size_t NCOLS{1024};
    constexpr size_t NPIX {NSEGS * NROWS * NCOLS};
    constexpr size_t PIX_PER_SEG {NROWS * NCOLS};

    #pragma omp parallel for schedule(static)
    for (size_t seg=0; seg < NSEGS; ++seg) {
      auto* raw_data = reinterpret_cast<std::uint16_t*>(data_ptrs[seg]);

      for (size_t panel_idx=0; panel_idx < PIX_PER_SEG; ++panel_idx) {
        size_t idx = seg*PIX_PER_SEG + panel_idx;

        uint16_t raw_pixel = raw_data[panel_idx];
        uint16_t data = raw_pixel & data_mask;
        size_t gain_idx = raw_pixel >> 14; // Top two bits
        if (gain_idx > 1) {
          if (gain_idx == 2) [[unlikely]] {
            /* This is 0b10 - a bad pixel, hopefully unlikely - what should happen? */
            continue;
          } else [[likely]] {
            gain_idx--; // Map gain_idx 3 (0b11 - low gain) to index 2
          }
        }
        size_t calib_idx = gain_idx * NPIX + idx;

        if (calib_idx >= calibconst.size()) {
          continue;
        }
        calib_data[idx] =
          (data - calibconst[calib_idx].ped) * calibconst[calib_idx].gain;
      }
    }
  }

  void calibrate_epix100(std::vector<void*>& data_ptrs,
                         std::span<CalibStruct>& calibconst,
                         std::vector<std::float32_t>& calib_data) {

    constexpr size_t NROWS{704};
    constexpr size_t NCOLS{768};
    constexpr size_t NPIX {NROWS * NCOLS};
    auto* raw_data = reinterpret_cast<std::uint16_t*>(data_ptrs[0]);

    for (size_t idx=0; idx < NPIX; ++idx) {
      uint16_t data = raw_data[idx];
      calib_data[idx] = (data - calibconst[idx].ped) * calibconst[idx].gain;
    }
  }

  void calibrate_epix100(size_t segment, /* ignored for epix100 */
                         std::vector<void*>& data_ptrs,
                         std::span<CalibStruct>& calibconst,
                         std::vector<std::float32_t>& calib_data) {

    constexpr size_t NROWS{704};
    constexpr size_t NCOLS{768};
    constexpr size_t NPIX{NROWS * NCOLS};
    auto* raw_data = reinterpret_cast<std::uint16_t*>(data_ptrs[0]);

    for (size_t idx = 0; idx < NPIX; ++idx) {
      uint16_t data = raw_data[idx];
      calib_data[idx] = (data - calibconst[idx].ped) * calibconst[idx].gain;
    }
  }

  std::vector<std::float32_t> calibrate_jungfrau(std::vector<void*>& data_ptrs,
                                                 std::span<CalibStruct>& calibconst) {
    auto data_mask = 0x3FFF;
    constexpr size_t NSEGS{32};
    constexpr size_t NROWS{512};
    constexpr size_t NCOLS{1024};
    constexpr size_t NPIX {NSEGS * NROWS * NCOLS};
    constexpr size_t PIX_PER_SEG {NROWS * NCOLS};

    std::vector<std::float32_t> data_out(32*512*1024);

    #pragma omp parallel for schedule(static)
    for (size_t seg = 0; seg < NSEGS; ++seg) {
      auto* raw_data = reinterpret_cast<std::uint16_t*>(data_ptrs[seg]);

      for (size_t panel_idx = 0; panel_idx < PIX_PER_SEG; ++panel_idx) {
        size_t idx = seg * PIX_PER_SEG + panel_idx;

        uint16_t raw_pixel = raw_data[panel_idx];
        uint16_t data = raw_pixel & data_mask;
        size_t gain_idx = raw_pixel >> 14; // Top two bits
        if (gain_idx > 1) {
          if (gain_idx == 2) [[unlikely]] {
            /* This is 0b10 - a bad pixel, hopefully unlikely - what should
             * happen? */
            continue;
          } else [[likely]] {
            gain_idx--; // Map gain_idx 3 (0b11 - low gain) to index 2
          }
        }
        size_t calib_idx = gain_idx * NPIX + idx;

        if (calib_idx >= calibconst.size()) {
          continue;
        }
        data_out[idx] =
          (data - calibconst[calib_idx].ped) * calibconst[calib_idx].gain;
      }
    }

    return data_out;
  }
} // namespace XTCPP
