#ifndef XTCPP_BASE_DETECTOR_HH
#define XTCPP_BASE_DETECTOR_HH

#include "bd_reader.hh"

#include "../util/threadpool.hh"

#include "xtcdata/xtc/Dgram.hh"

#include "spdlog/sinks/stdout_color_sinks.h"

#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdfloat>
#include <string>
#include <utility>
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

  /**
   * Calibrate a detector image
   * @param[in] data_ptrs A vector of per segment/module raw data.
   * @param[in] calibconst The calibration constants. Should be a 1 dimensional
   *            span with a length equal to the total number of pixels. I.e. it
   *            is not per segment like the data_ptrs vector.
   * @return calib_data The calibrated data.
   */
  std::vector<std::float32_t> calibrate(std::vector<void*>& data_ptrs,
                                        std::span<CalibStruct>& calibconst);

  /**
   * Calibrate a detector image
   * @param[in] data_ptrs A vector of per segment/module raw data.
   * @param[in] calibconst The calibration constants. Should be a 1 dimensional
   *            span with a length equal to the total number of pixels. I.e. it
   *            is not per segment like the data_ptrs vector.
   * @param[out] calib_data An output buffer to hold the calibrated data.
   */
  void calibrate(std::vector<void*>& data_ptrs,
		 std::span<CalibStruct>& calibconst,
		 std::vector<std::float32_t>& calib_data);

  namespace Base {
    /**
     * The detector class provides a convenience wrapper around a set of BDReader
     * instances. It is intended to be constructed from the DataSource::detector
     * function so that it receives the correct BDReaders. It can then be used
     * to retrieve all data for the detector even if it is spread across multiple
     * files. Care must be taken if constructing these objects independently of the
     * DataSource.
     */
    class Detector {
    public:
      Detector(std::string detname,
               std::string serial_no,
               std::vector<unsigned> segment_nos,
               std::vector<std::shared_ptr<BDReader>> xtc_readers,
               std::string experiment,
               std::string run,
               bool is_epics);

      /**
       * Retrieve the datagram at an offset index.
       * @param[in] offset_idx The index of the offset to use. I.e. offset index 400
       *            corresponds to the 401st event. The concrete implementations must
       *            provide the mechanism to turn these indices into the actual offset
       *            read by the SMDReader class.
       * @return dgram The pointer to the datagram. May be a nullptr if no more data,
       *               not found, etc.
       */
      XtcData::Dgram* operator()(size_t offset_idx);

      /**
       * Return the data associated with a specific "algorithm" and field name
       * for the specified offset index.
       * @param[in] offset_idx The index (i.e. event) to retrieve data for.
       * @param[in] alg The algorithm, e.g. `raw`.
       * @param[in] data_name The field/data name within the algorithm. E.g. `raw`.
       * @return data The poitner to the requested data inside the datagram. May be
       *              a nullptr if not found (e.g. doesn't exist). In general, you should
       *              use the `data_ptrs` function for easier handling as this returns
       *              an unstructured pointer to the underlying set of pointers for
       *              potentially many segments.
       */
      virtual void* get_data(size_t offset_idx, const std::string& alg, const std::string& data_name);

      /**
       * Access the calibration constants.
       * NOTE: This function should likely not be used - implementations may clear
       * this vector if it makes sense to. This may allow them to share constants
       * between multiple processes etc. On the otherhand the corresponding `_span`
       * function will always point to the data since it constructs a view on top
       * of the memory.
       */
      std::vector<CalibStruct>& calibconst() { return m_calibconst; }

      /**
       * Access a span constructed over the calibration constants in memory.
       * This function should always return a valid view of the constants (once
       * they have been fetched, and assuming they exist for the detector.)
       */
      std::span<CalibStruct>& calibconst_span() { return m_calibconst_span; }

      /**
       * Access the current set of data pointers. This is a vector that holds
       * pointers to the data for each segment/module of the detector. E.g. a
       * detector that has 4 modules will return a size 4 vector here.
       * This function should be used after each call to `get_data` above.
       */
      std::vector<void*>& data_ptrs() { return m_data_ptrs; }

      /**
       * Access to a pre-allocated buffer which can be used for holding output
       * calib data. This should be passed in as an input to the XTCPP::calibrate
       * function and reused event by event.
       */
      std::vector<std::float32_t>& calib_data_buf() { return m_calib_data; }

      /**
       * The detector name.
       */
      std::string detname() const { return m_detname; }

      /**
       * Whether this is an "EPICS" detector or not.
       */
      bool is_epics() const { return m_is_epics; }

    protected:
      /**
       * Very dumb function to load a set of calibration constants.
       * Must have a `gain.npy`, `ped.npy`, `offset.npy` in the current working
       * directory.
       */
      void load_dummy_calib();

      /**
       * Make a database call to determine the short name from the serial number.
       * Will populate `m_short_name`.
       */
      void get_detector_short_name();

      /**
       * Utility function to split a string on a delimiter.
       */
      std::vector<std::string> split_string(const std::string& s, const std::string& delim);

      /**
       * Load calibration constants for a specified type.
       * NOTE: This function is currently only implemented to handle float32 data!
       * TODO: Handle other constants types.
       *
       * @param[in] constants_type The type of constants to load from the database.
       *            E.g. `pedestals`, `pixel_gain`.
       * @return constants_shape A pair of the constants and a vector containing
       *         the shape of those constants.
       */
      std::pair<std::vector<std::float32_t>,std::vector<size_t>>
      load_calib_constants_type(std::string constants_type);

      /**
       * Load pixel_gain, pixel_offset and pedestals and then construct the CalibStruct
       * array.
       */
      void load_all_calib_constants();

      /**
       * An initialization function for sub-classes to specify how to setup resources
       * for their implementations.
       */
      virtual void init_resources() {}

    protected:
      CalibStruct* m_const_ptr{nullptr}; ///< Underlying memory for holding calib constants

      std::string m_detname; ///< Detector name
      std::string m_serial_no; ///< The detector serial number (includes det type at beginning)
      std::string m_det_type; ///< The detector type (epix100, jungfrau, etc.)
      /**
       * The "short name" associated to the serial number.
       * The short name is used for interactions with the calibration database.
       */
      std::string m_short_name{""};
      std::string m_experiment; ///< The experiment for which data is being read
      std::string m_run; ///< The experimental run for which data is being read
      /**
       * The set of segment numbers being read for the detector.
       * If created by the DataSource::detector function this should contain all
       * the segment numbers across all xtc2 files.
       */
      std::vector<unsigned> m_segment_nos;
      /**
       * The set of BDReaders managing the XTC2 files with this detector's data.
       */
      std::vector<std::shared_ptr<BDReader>> m_xtc_readers;

      /**
       * The set of pointers to data requested by `get_data` calls.
       * Has a size equal to the size of m_segment_nos.
       */
      std::vector<void*> m_data_ptrs;
      /**
       * The corresponding vector of sizes for each entry in `m_data_ptrs`
       */
      std::vector<size_t> m_data_sizes;

      /**
       * A buffer that can be pre-allocated to hold calib data.
       */
      std::vector<std::float32_t> m_calib_data;

      // Should hold ped/gain for 1 gain mode then the next etc..
      std::vector<CalibStruct> m_calibconst; ///< Vector of calib constants
      std::span<CalibStruct> m_calibconst_span; ///< Span over m_const_ptr

      /**
       * Whether this is an EPICS detector or not. The access mechanisms
       * are different for EPICS/non-EPICS detectors. Passing the flag
       * up front makes it simpler long term.
       */
      bool m_is_epics{false};

      std::shared_ptr<spdlog::logger> m_logger; ///< Logger

    private:
      std::optional<ThreadPool> m_thread_pool;
      using GetDataFn = void* (Detector::*)(size_t,
                                            const std::string&,
                                            const std::string&);
      GetDataFn get_data_impl;
      void* get_data_threaded(size_t, const std::string&, const std::string&);
      void* get_data_sequential(size_t, const std::string&, const std::string&);
    };
  } // namespace Base
}

#endif // XTCPP_BASE_DETECTOR_HH
