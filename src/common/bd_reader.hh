#ifndef XTCPP_BASE_BDREADER_HH
#define XTCPP_BASE_BDREADER_HH

#include "common/smd_reader.hh"

#include "smd_reader.hh"
#include "xtcdata/xtc/DescData.hh"
#include "xtcdata/xtc/Dgram.hh"
#include "xtcdata/xtc/NamesLookup.hh"

#include "mpi.h"
#include "spdlog/sinks/stdout_color_sinks.h"

#include <any>
#include <expected>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace XTCPP {

  enum class BDReadError {
    UnimplementedBaseFunction,
    ZeroBytesRead,
    AllDgramOffsetsRead,
    GeneralIOError
  };

  // A tuple of segment number, algorithm, and data field within the algorithm
  using SegAlgData = std::tuple<unsigned, std::string, std::string>;
  namespace Base {
    /**
     * The BDReader class manages reading 1 single XTC2 file.
     * It in turn manages an associated SMDReader object which will fetch the offsets
     * it can use to more efficently read through its managed XTC2 file.
     */
    class BDReader {
    public:
      BDReader(std::string& smd_path, std::string& xtc_path, size_t events_per_read);

      virtual ~BDReader();

      /* Synchronous API */
      /**
       * Retrieve the datagram at an offset index.
       * @param[in] offset_idx The index of the offset to use. I.e. offset index 400
       *            corresponds to the 401st event. The concrete implementations must
       *            provide the mechanism to turn these indices into the actual offset
       *            read by the SMDReader class. The index is internally wrapped
       *            by the events_per_read that was passed at creation since only
       *            this number of offsets is held in memory at a time.
       * @return dgram The pointer to the datagram. May return a BDReadError with
       *         appropriate enumerator if data is not there etc.
       */
      virtual std::expected<void, BDReadError>
      read_l1_at(size_t unwrapped_offset_idx) {
        return std::unexpected(BDReadError::UnimplementedBaseFunction);
      }

      virtual std::expected<void, BDReadError>
      read_slowupdate_at(size_t unwrapped_offset_idx) {
        return std::unexpected(BDReadError::UnimplementedBaseFunction);
      }

      /* Asynchronous API */
      /**
       * Trigger an asynchronous read of the datagram at the specified offset index.
       * @param[in] offset_idx The index of the offset to use. I.e. offset index 400
       *            corresponds to the 401st event. The concrete implementations must
       *            provide the mechanism to turn these indices into the actual offset
       *            read by the SMDReader class. The index is internally wrapped by
       *            the events_per_read that was passed at creation since only
       *            this number of offsets is held in memory at a time.
       * @return dgram The pointer to the datagram. May return a BDReadError with
       *         appropriate enumerator if data is not there etc.
       */
      virtual std::expected<void, BDReadError>
      iread_l1_at(size_t unwrapped_offset_idx) {
        return std::unexpected(BDReadError::UnimplementedBaseFunction);
      }

      virtual std::expected<void, BDReadError> wait() {
        return std::unexpected(BDReadError::UnimplementedBaseFunction);
      }

      /* Data access */
      virtual XtcData::Dgram* get_current_dgram() { return nullptr; }

      /**
       * Get the next set of offsets via the managed SMDReader.
       * @return num_offsets The number of offsets read. May return a
       * BDReadError if something goes wrong or no more offsets to read.
       */
      virtual std::expected<size_t, BDReadError> get_next_offsets() {
        return std::unexpected(BDReadError::UnimplementedBaseFunction);
      }

      /**
       * Return the data associated with a specific "algorithm" and field name
       * for a detector and segment number.
       * NOTE: The `read_l1_at` function MUST be called before this one. That function
       *       reads the data from the file, this one then selects the relevant portion
       *       from within it.
       *
       * @param[in] detname The detector to get data for.
       * @param[in] seg_no The segment number for the detector.
       * @param[in] alg The algorithm, e.g. `raw`.
       * @param[in] data_name The field/data name within the algorithm. E.g. `raw`.
       * @return data_and_size The pair of a pointer to the requested data and the
       *         size of that data in bytes. The pointer may be nullptr if not found, etc.
       */
      virtual std::pair<void*, size_t> get_data(const std::string& detname,
                                                const unsigned& seg_no,
                                                const std::string& alg,
                                                const std::string& data_name);

      /**
       * Close the XTC2 file (in whatever manner appropriate for the
       * implementation). Also cleanup any additional resources.
       */
      virtual void close();

      /**
       * The set of detector names in the XTC2 file managed by this reader.
       */
      std::vector<std::string> detnames() const { return m_detnames; }

      /**
       * The map of detector names to segment numbers for the data in this XTC2 file.
       */
      std::map<std::string, std::vector<unsigned>> segment_numbers() const { return m_segment_nos; }

      /**
       * The map of detector names to serial numbers for the data in this XTC2 file.
       */
      std::map<std::string, std::vector<std::string>> serial_numbers() const { return m_serial_nos; }

      /**
       * The map of detector names to detector types for the data in this XTC2 file.
       */
      std::map<std::string, std::string> det_types() const { return m_det_types; }

      /**
       * A pointer to the offsets being used to read datagrams.
       */
      std::shared_ptr<BDXtcOffset[]> offsets() const { return m_offsets; }

      /**
       * The set of EPICS detector names (if any) in the file managed by this
       * reader.
       */
      std::vector<std::string> epics_detnames() const { return m_epics_detnames; }

    protected:
      /**
       * An iterative approach to pulling out the requested data if the offset
       * is not stored in `m_offsets_in_dg` or it is not reliable to use it.
       */
      std::pair<void*, size_t> get_data_internal(const std::string& detname,
                                                 const unsigned& seg_no,
                                                 const std::string& alg,
                                                 const std::string& data_name);

      /**
       * Extract a value from an XTC by looking at the type/rank/size
       * information in all the auxiliary XTCs etc. See the xtcdata package for
       * more examples of this.
       */
      std::any get_value(size_t idx, XtcData::Name &name,
                         XtcData::DescData &descdata);

      virtual void init_reader(){}; ///< Initialize the BDReader

    protected:
      size_t m_events_per_read; ///< Number of events/offsets to store in memory
      std::unique_ptr<SMDReader> m_smd_reader; ///< The SMDReader used to read .smd.xtc2
      /**
       * The buffer used to hold `m_events_per_read` offsets in memory.
       */
      std::shared_ptr<BDXtcOffset[]> m_offsets{nullptr};

      /**
       * A shared buffer for holding indices for slow updates.
       * This index always points to the L1Accept index that immediately preceeds
       * a SlowUpdate. E.g. if the index is 42, that means that after the L1Accept
       * at offset index 42, there is a SlowUpdate (before you get to L1Accept at
       * the offset index of 43.). This array is signed, because a value of -1
       * indicates that before the first L1Accept, there is a SlowUpdate.
       */
      std::shared_ptr<SlowUpdateXtcOffset[]> m_slow_updates{nullptr};
      size_t m_curr_slow_update_index{0};
      size_t m_num_slow_updates;

      size_t m_num_events; ///< Current number of events/offsets read
      XtcData::Xtc* m_payload_ptr; ///< Pointer to the data requested by get_data
      int m_remaining_payload; ///< Remaining payload f iterating a datagram

      std::vector<std::string> m_detnames; ///< Set of detector names
      /**
       * Map of detector names to segment numbers.
       */
      std::map<std::string,std::vector<unsigned>> m_segment_nos;

      /**
       * Map of detector names to serial numbers.
       */
      std::map<std::string,std::vector<std::string>> m_serial_nos;

      /**
       * Map of detector names to detector types.
       */
      std::map<std::string,std::string> m_det_types;

      /**
       * A map of per detector offsets for fast lookup of data within a
       * datagram. This assumes consistent size of data. This is valid for
       * some algorithms but not all.
       */
      std::map<std::string, std::map<SegAlgData, BDXtcOffset>> m_offsets_in_dg;

      std::vector<std::string> m_epics_detnames;

      std::string m_smd_path; ///< Path to the .smd.xtc2 file
      std::string m_xtc_path; ///< Path to the .xtc2 file

      std::shared_ptr<spdlog::logger> m_logger;
    };
  } // namespace Base
} // namespace XTCPP

#endif // XTCPP_BASE_BDREADER_HH
