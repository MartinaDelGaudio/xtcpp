#ifndef XTCPP_BASE_DATASOURCE_HH
#define XTCPP_BASE_DATASOURCE_HH

#include "detector.hh"
#include "bd_reader.hh"

#include "xtcdata/xtc/Dgram.hh"

#include "spdlog/sinks/stdout_color_sinks.h"

#include <cstddef>
#include <iterator>
#include <map>
#include <memory>
#include <numeric>
#include <string>
#include <variant>
#include <vector>

namespace XTCPP {
  namespace Base {
    /**
     * The DataSource provides a convenience wrapper to locate a set of files
     * associated with an experiment and run. It then will construct the BDReader
     * objects which manage these files and allow IO. It provides for construction
     * of Detector objects which wrap subsets of these readers allowing for per-detector
     * data access.
     *
     * The DataSource iterator provides a convenient way to go through event/offset
     * indices and trigger reads of data.
     */
    class DataSource {
    public:
      DataSource(std::string exp, std::variant<std::string, int> run, size_t events_per_read);

      /**
       * Construct a detector.
       * @param[in] detname The name of the Detector in the XTC2 data.
       * @return det A shared_ptr to the Detector object.
       */
      virtual std::shared_ptr<Detector> detector(std::string detname) = 0;

      /**
       * DataSource iterator to get offset indices.
       */
      class Iterator {
      public:
        using iterator_category = std::forward_iterator_tag; // Only 1 direction
        using difference_type   = std::ptrdiff_t;
        using value_type        = size_t;
        using pointer           = value_type*;
        using reference         = value_type&;

        Iterator(pointer ptr);

        Iterator(pointer ptr, DataSource* ds, bool init);

        reference operator*() const { return *m_ptr; }
        pointer operator->() { return m_ptr; }

        Iterator& operator++();

        Iterator operator++(int);

        friend bool operator== (const Iterator& a, const Iterator& b) { return a.m_ptr == b.m_ptr; }
        friend bool operator!= (const Iterator& a, const Iterator& b) { return a.m_ptr != b.m_ptr; }

      private:
        pointer m_ptr;
        DataSource* m_ds;
      };

      virtual Iterator begin();
      virtual Iterator end();

    protected:
      std::string m_experiment; ///< The experiment for which data is being read
      std::string m_hutch; ///< The hutch associated to the experiment
      std::string m_run; ///< The experimental run for which data is being read

      /**
       * The paths of all .smd.xtc2 files for this experiment and run.
       */
      std::vector<std::string> m_smd_files;
      /**
       * The paths of all .xtc2 files for this experiment and run.
       */
      std::vector<std::string> m_xtc_files;

      /**
       * A map of detector name to BDReaders (in turn wrapping the XTC2 files.)
       * These readers are being used for L1Accept reading ("normal" event data.)
       */
      std::map<std::string,std::vector<std::shared_ptr<BDReader>>> m_l1_xtc_readers;

      /**
       * A map of detector name to BDReaders (in turn wrapping the XTC2 files.)
       * These readers are being used for SlowUpdate reading (I.e. EPICS data.)
       */
      std::map<std::string, std::vector<std::shared_ptr<BDReader>>> m_epics_xtc_readers;

      /**
       * The subset of BDReaders from the whole which are actually in use by the
       * application code.
       */
      std::vector<std::shared_ptr<BDReader>> m_xtc_readers_in_use;

      std::vector<size_t> m_offset_indices; ///< Vector of offset indices
      /**
       * Number of events/offsets to fetch on each read of the .smd.xtc2 files.
       */
      size_t m_events_per_read;
      size_t m_last_offset_index{0}; ///< Current last offset index
      size_t load_next_offsets(); ///< Fetch a new set of offsets from the .smd.xtc2 files

      /**
       * Determine the next offset index to use.
       * This should be used after loading a set of offsets (since chunks are stored
       * in memory)
       */
      virtual size_t fetch_next_idx() = 0;
      virtual void init_detectors(){}; ///< Initialize the detectors

      std::shared_ptr<spdlog::logger> m_logger; ///< Logger
    };
  } // namespace Base
} // namespace XTCPP

#endif // XTCPP_BASE_DATASOURCE_HH
