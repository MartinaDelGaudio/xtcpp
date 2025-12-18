#ifndef XTCPP_BASE_DETECTOR_UTIL_HH
#define XTCPP_BASE_DETECTOR_UTIL_HH

#include <cstdint>
#include <functional>
#include <span>
#include <stdfloat>
#include <string>
#include <vector>

namespace XTCPP {
  /**
   * Holds a single set of constants required to calibrate a pixel.
   * A vector/array etc of these structs would be required to calibrate a full
   * detector image.
   */
  #pragma pack(push,1)
  class CalibStruct {
  public:
    std::float32_t ped;  ///< Holds pedestal + offset should an offset exist
    std::float32_t gain; ///< Holds the gain value in keV/ADU
  };
  #pragma pack(pop)

  using OpFn =
      std::function<void(std::string,                    /* Detector Type */
                         size_t,                         /* Segment number */
                         std::vector<void*>&,            /* Data pointers */
                         std::span<CalibStruct>&,        /* Calib constants */
                         std::vector<std::float32_t>&)>; /* Output buffer */

  /**
   * Calibrate a detector image
   * @param[in] dettype The detector type in order to call the correct calibration
   *            function.
   * @param[in] data_ptrs A vector of per segment/module raw data.
   * @param[in] calibconst The calibration constants. Should be a 1 dimensional
   *            span with a length equal to the total number of pixels. I.e. it
   *            is not per segment like the data_ptrs vector.
   * @param[out] calib_data An output buffer to hold the calibrated data.
   */
  void calibrate(std::string dettype,
                 std::vector<void*>& data_ptrs,
                 std::span<CalibStruct>& calibconst,
                 std::vector<std::float32_t>& calib_data);

  /**
   * Calibrate a Jungfrau detector image
   * @param[in] data_ptrs A vector of per segment/module raw data.
   * @param[in] calibconst The calibration constants. Should be a 1 dimensional
   *            span with a length equal to the total number of pixels. I.e. it
   *            is not per segment like the data_ptrs vector.
   * @return calib_data The calibrated data.
   */
  void calibrate_jungfrau(std::vector<void*>& data_ptrs,
                          std::span<CalibStruct>& calibconst,
                          std::vector<std::float32_t>& calib_data);

  /**
   * Calibrate an ePix100 detector image.
   * @param[in] data_ptrs A vector of per segment/module raw data.
   * @param[in] calibconst The calibration constants. Should be a 1 dimensional
   *            span with a length equal to the total number of pixels. I.e. it
   *            is not per segment like the data_ptrs vector.
   * @param[out] calib_data An output buffer to hold the calibrated data.
   */
  void calibrate_epix100(std::vector<void*>& data_ptrs,
                         std::span<CalibStruct>& calibconst,
                         std::vector<std::float32_t>& calib_data);

  /*** Per segment overloads ***/

  /**
   * Calibrate a detector image segment
   * @param[in] dettype The detector type in order to call the correct calibration
   *            function.
   * @param[in] segment The segment number to do per segment processing.
   * @param[in] data_ptrs A vector of per segment/module raw data.
   * @param[in] calibconst The calibration constants. Should be a 1 dimensional
   *            span with a length equal to the total number of pixels. I.e. it
   *            is not per segment like the data_ptrs vector.
   * @param[out] calib_data An output buffer to hold the calibrated data.
   */
  void calibrate_segment(std::string dettype,
                         size_t segment,
                         std::vector<void*>& data_ptrs,
                         std::span<CalibStruct>& calibconst,
                         std::vector<std::float32_t>& calib_data);
  /**
   * Calibrate a Jungfrau detector image segment
   * @param[in] segment The segment number to do per segment processing.
   * @param[in] data_ptrs A vector of per segment/module raw data.
   * @param[in] calibconst The calibration constants. Should be a 1 dimensional
   *            span with a length equal to the total number of pixels. I.e. it
   *            is not per segment like the data_ptrs vector.
   * @return calib_data The calibrated data.
   */
  void calibrate_jungfrau(size_t segment,
                          std::vector<void*>& data_ptrs,
                          std::span<CalibStruct>& calibconst,
                          std::vector<std::float32_t>& calib_data);

  /**
   * Calibrate an ePix100 detector image. Segment-based version provided just
   * for simplifying API compatibility.
   *
   * @param[in] segment The segment number. Ignored for ePix100.
   * @param[in] data_ptrs A vector of per segment/module raw data.
   * @param[in] calibconst The calibration constants. Should be a 1 dimensional
   *            span with a length equal to the total number of pixels. I.e. it
   *            is not per segment like the data_ptrs vector.
   * @param[out] calib_data An output buffer to hold the calibrated data.
   */
  void calibrate_epix100(size_t segment,
                         std::vector<void*>& data_ptrs,
                         std::span<CalibStruct>& calibconst,
                         std::vector<std::float32_t>& calib_data);

  /**
   * Calibrate a Jungfrau detector image
   * @param[in] data_ptrs A vector of per segment/module raw data.
   * @param[in] calibconst The calibration constants. Should be a 1 dimensional
   *            span with a length equal to the total number of pixels. I.e. it
   *            is not per segment like the data_ptrs vector.
   * @return calib_data The calibrated data.
   */
  std::vector<std::float32_t> calibrate_jungfrau(std::vector<void*>& data_ptrs,
                                                 std::span<CalibStruct>& calibconst);

} // namespace XTCPP

#endif // XTCPP_BASE_DETECTOR_UTIL_HH
