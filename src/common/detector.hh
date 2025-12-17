#ifndef XTCPP_BASE_DETECTOR_HH
#define XTCPP_BASE_DETECTOR_HH

#include "common/bd_reader.hh"
#include "common/detector_utils.hh"

#include "../util/threadpool.hh"

#include "smd_reader.hh"
#include "xtcdata/xtc/Dgram.hh"
#include "xtcdata/xtc/ShapesData.hh" // XtcData::Name::DataType

#include "spdlog/sinks/stdout_color_sinks.h"

#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdfloat>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace XTCPP {
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
               bool is_epics,
               bool is_scan);

      virtual ~Detector(){}

      /**
       * Retrieve the datagram at an offset index.
       * @param[in] offset_idx The index of the offset to use. I.e. offset index 400
       *            corresponds to the 401st event. The concrete implementations must
       *            provide the mechanism to turn these indices into the actual offset
       *            read by the SMDReader class.
       * @return dgram The pointer to the datagram. May be a nullptr if no more data,
       *               not found, etc.
       */
      const XtcData::Dgram* const operator()(size_t offset_idx);

      /**
       * Return the data associated with a specific "algorithm" and field name
       * for the specified offset index.
       * This function searches for L1Accept data which is applicable to all
       * detectors *except* EPICS (epicsArch) and the scan detector.
       *
       * NOTE: The algorithm and field names to use can be retrieved using the
       *      `algs` and `alg_fields` functions.
       *
       * @param[in] offset_idx The index (i.e. event) to retrieve data for.
       * @param[in] alg The algorithm, e.g. `raw`.
       * @param[in] data_name The field/data name within the algorithm. E.g. `raw`.
       * @return data The pointer to the requested data inside the datagram. May be
       *              a nullptr if not found (e.g. doesn't exist). In general, you should
       *              use the `data_ptrs` function for easier handling as this returns
       *              an unstructured pointer to the underlying set of pointers for
       *              potentially many segments.
       */
      virtual std::tuple<void**,uint32_t,uint32_t*>
      get_l1_data(size_t offset_idx, const std::string& alg, const std::string& data_name);

      /**
       * Return the closest SlowUpdate data to the offset index.
       * epicsArch data does not have an algorithm or field/data name. The data
       * are stored under the `epics` detector using the PV name. That PV name
       * is used as the Detector name here, so no further information is
       * required to look up the data.
       *
       * @param[in] offset_idx The index (i.e. event) to retrieve data for.
       * @return data The pointer to the requested SlowUpdate data.
       */
      virtual std::tuple<void**, uint32_t, uint32_t*>
      get_slow_update_data(size_t offset_idx);

      /**
       * Return the closest scan step data to the offset index.
       * This data is encoded in BeginStep transitions which is why the
       * interface is independent of the `get_l1_data` function.
       *
       * In general, scan data will always have a `step_value` field to access
       * and will usually also have a `step_docstring` field (although this is
       * not strictly required.) The detector uses the `raw` algorithm currently.
       * The rest of the fields are the scanned variables and will depend on the
       * scan performed. The functions `algs` and `alg_fields` can be used to
       * determine the names of these fields.
       *
       * @param[in] offset_idx The index (i.e. event) to retrieve data for.
       * @param[in] alg The algorithm, usually (if not always) `raw`.
       * @param[in] data_name The field/data name within the algorithm.
       * @return data The pointer to the requested scan data.
       */
      virtual std::tuple<void**, uint32_t, uint32_t*>
      get_scan_data(size_t offset_idx, const std::string& alg, const std::string& data_name);

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

      /**
       * Whether this is a scan detector or not.
       */
      bool is_scan() const { return m_is_scan; }

      /**
       * The algorithms implemented by the detector.
       */
      std::vector<std::string> algs() const { return m_det_algs; }

      /**
       * A mapping of field names to the various algorithms.
       * Algorithm keys are stored as (alg.name(),alg.version()) pairs.
       */
      std::map<std::pair<std::string,unsigned>, std::vector<DataField>>
      alg_fields() const { return m_det_alg_fields; }

      std::string det_type() const { return m_det_type; }

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

      /**
       * Whether this is a scan detector or not. Like the EPICS detector
       * the "scan" detector must read transition data instead of L1Accepts
       * so the access mechanism is different.
       */
      bool m_is_scan{false};

      /**
       * The algorithms implemented by the detector.
       */
      std::vector<std::string> m_det_algs;

      /**
       * A mapping of field names to the various algorithms.
       */
      std::map<std::pair<std::string, unsigned>, std::vector<DataField>>
      m_det_alg_fields;

      /**
       * Keeps track of the last index data was read for. Since a `get_data_..`
       * function may be called multiple times for different alg/fields we only
       * want to read the actual data from disk once.
       */
      ssize_t m_last_index_read {-1};

      std::shared_ptr<spdlog::logger> m_logger; ///< Logger

    private:
      std::optional<ThreadPool> m_thread_pool;
      using GetDataFn =
        std::tuple<void**, uint32_t, uint32_t*> (Detector::*)(size_t,
                                                             const std::string&,
                                                             const std::string&);

      GetDataFn get_l1_data_impl;
      std::tuple<void**, uint32_t, uint32_t*>
      get_l1_data_threaded(size_t, const std::string&, const std::string&);

      std::tuple<void**, uint32_t, uint32_t*>
      get_l1_data_sequential(size_t, const std::string&, const std::string&);
    };
  } // namespace Base
}

#endif // XTCPP_BASE_DETECTOR_HH
