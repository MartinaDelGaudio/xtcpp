#ifndef XTCPP_BASE_SMDREADER_HH
#define XTCPP_BASE_SMDREADER_HH

#include "xtcdata/xtc/Dgram.hh"
#include "xtcdata/xtc/NamesLookup.hh"
#include "xtcdata/xtc/NameIndex.hh"

#include "spdlog/sinks/stdout_color_sinks.h"

#include <stdio.h>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace XTCPP
{
  /**
   * Holds information about the offset and size of a single datagram in an XTC2
   * file. A vector/array of these should be used to represent the offsets of an
   * entire XTC2 or a portion of it.
   */
#pragma pack(push, 1)
  struct XtcOffset {
    XtcOffset()
      : offset(0)
      , size(0)
    {}
    XtcOffset(uint64_t offset_, uint64_t size_)
      : offset(offset_)
      , size(size_)
    {}
    uint64_t offset; ///< Offset of the datagram in bytes
    uint64_t size; ///< Size of the datagram
  };
#pragma pack(pop)

  using AlgDataNameIndex = std::map<std::string, std::map<std::string, std::map<unsigned, XtcData::NameIndex>>>;
  namespace Base {
    /**
     * The SMDReader class manages reading 1 single .smd.xtc2 file.
     * It should likely only be used via a managing BDReader instance.
     */
    class SMDReader {
    public:
      SMDReader(std::string& smd_path,
                size_t max_dgram_size,
                size_t events_per_read);

      virtual ~SMDReader() {}

      /**
       * Return the pointer to the next Dgram
       * @return dgram The pointer to the next datagram.
       */
      virtual XtcData::Dgram* next() { return nullptr; } ///< Return the pointer to the next Dgram

      /**
       * Return the pointer to the next Dgram AND construct offsets into memory.
       * @param[in] external_buf An external buffer that the XtcOffset objects will
       *            be constructed into while the SMDReader is iterating through the
       *            .smd.xtc2 file.
       * @return dgram The pointer to the next datagram.
       */
      XtcData::Dgram* next(std::shared_ptr<XtcOffset[]> external_buf);

      /**
       * Trigger a read of the .smd.xtc2 file.
       */
      virtual void read() {};

      /**
       * Wait on a read of the .smd.xtc2 file. Depending on the implementation
       * the read function may be implemented as non-blocking, so these two can
       * be called one after another, or separated as needed.
       */
      virtual char* wait() { return nullptr; };

      /**
       * Max size of a datagram (used for building buffers for reads.)
       */
      size_t max_dgram_size() const { return m_max_dgram_size; }

      /**
       * The current number of events that have been read.
       */
      size_t n_events() const { return m_curr_offset_idx; }

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

      const AlgDataNameIndex&
      alg_map() const { return m_alg_map; }

    protected:
      std::string m_smd_path;
      size_t m_events_per_read;
      size_t m_max_dgram_size;
      size_t m_curr_offset_idx{0};
      size_t m_file_offset{0};

      char* m_access_ptr;
      size_t m_access_offset{0};
      size_t m_file_size;

      std::vector<std::string> m_detnames;
      std::map<std::string, std::vector<unsigned>> m_segment_nos;
      std::map<std::string, std::vector<std::string>> m_serial_nos;
      std::map<std::string, std::string> m_det_types;

      /**
       * Describes how many bytes into the dgram.payload the offset information is.
       * Since both the offset and size of the L1Accept dgrams in the smd files are
       * uint64_t, the size information, which comes after the offset information,
       * is m_offset_in_payload + 8.
       * This only works for the L1Accept datagrams.
       */
      size_t m_offset_in_l1accept_payload{48};

      char* dgrams_buf; ///< Data read into this buffer

      void process_data(XtcData::Xtc *xtc,
                        std::shared_ptr<XtcOffset[]> external_buf);

      void process_data(XtcData::Xtc *xtc,
                        XtcData::TransitionId::Value transition_id);
      void process_data_internal(XtcData::Xtc *xtc,
                                 XtcData::TransitionId::Value transition_id);

      char* m_buf;
      XtcData::NamesLookup m_names_lookup;

      std::vector<XtcOffset> m_offsets;

      std::map<std::string,                   // detname
               std::map<std::string,          // alg_name
                        std::map<unsigned,    // segment #
                                 XtcData::NameIndex>>> m_alg_map;

      std::vector<unsigned> m_offset_in_xtc;

      virtual void init_file(){}

      std::shared_ptr<spdlog::logger> m_logger;
    };
  } // namespace Base
} // namespace XTCPP

#endif
