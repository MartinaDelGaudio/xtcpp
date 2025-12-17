#ifndef XTCPP_MPI_DETECTOR_HH
#define XTCPP_MPI_DETECTOR_HH

#include "common/detector.hh"
#include "bd_reader.hh"

#include "xtcdata/xtc/Dgram.hh"

#include "mpi.h"

#include <map>
#include <memory>
#include <span>
#include <stdfloat>
#include <string>
#include <vector>


namespace XTCPP {

  namespace MPI {
    class Detector final : public Base::Detector {
    public:
      Detector(MPI_Comm comm,
               std::string detname,
               std::string serial_no,
               std::vector<unsigned> segment_nos,
               std::vector<std::shared_ptr<Base::BDReader>> xtc_readers,
               std::string experiment,
               std::string run,
               bool is_epics,
               bool is_scan);
      ~Detector();

    private:
      MPI_Comm m_comm;
      int m_rank;
      int m_n_ranks;
      MPI_Comm m_shmem_comm;
      int m_shmem_rank;
      int m_n_shmem_ranks;
      MPI_Win m_calibconst_win;

      void init_resources() override;
    };
  } // namespace MPI
}

#endif // XTCPP_MPI_DETECTOR_HH
