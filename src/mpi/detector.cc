#include "detector.hh"

#include "common/detector.hh"

#include "xtcdata/xtc/Dgram.hh"

//#include "omp.h"
#include "mpi.h"

#include <algorithm>
#include <fstream>
//#include <mdspan>
#include <memory>
#include <ostream>
#include <stdfloat>
#include <string>
#include <vector>

namespace XTCPP {
  namespace MPI {
    Detector::Detector(MPI_Comm comm,
                       std::string detname,
                       std::string serial_no,
                       std::vector<unsigned> segment_nos,
                       std::vector<std::shared_ptr<Base::BDReader>> xtc_readers,
                       std::string experiment,
                       std::string run,
                       bool is_epics,
                       bool is_scan)
      : Base::Detector(detname,
                       serial_no,
                       segment_nos,
                       xtc_readers,
                       experiment,
                       run,
                       is_epics,
                       is_scan)
      , m_comm(comm)
    {
      init_resources();
    }

    Detector::~Detector() {
      if (!m_is_epics && !m_is_scan) {
        MPI_Win_free(&m_calibconst_win);
      }
    }

    void Detector::init_resources() {
      MPI_Comm_rank(m_comm, &m_rank);
      MPI_Comm_size(m_comm, &m_n_ranks);
      //MPI_Info info;
      //MPI_Info_set(info, "shmem_alloc_hint", "explicit");
      //MPI_Comm_split_type(m_comm, MPI_COMM_TYPE_SHARED, m_rank, info, &m_shmem_comm);
      MPI_Comm_split_type(m_comm, MPI_COMM_TYPE_SHARED, m_rank, MPI_INFO_NULL, &m_shmem_comm);

      MPI_Comm_rank(m_shmem_comm, &m_shmem_rank);
      MPI_Comm_size(m_shmem_comm, &m_n_shmem_ranks);

      if (!m_short_name.empty()) {
        size_t window_size{0};
        if (m_shmem_rank == 0) {
          load_all_calib_constants();
          window_size = sizeof(std::float32_t)*2*m_calibconst.size();
        }
        MPI_Win_allocate_shared(window_size,
                                sizeof(std::float32_t),
                                MPI_INFO_NULL,
                                m_shmem_comm,
                                &m_const_ptr,
                                &m_calibconst_win);

        size_t n_constants = window_size;
        if (m_shmem_rank != 0) {
          MPI_Aint query_size;
          int disp_unit;
          MPI_Win_shared_query(m_calibconst_win,
                               0,
                               &query_size,
                               &disp_unit,
                               &m_const_ptr);
          n_constants = query_size;
        }
        // Has the window size currently, which is in bytes
        n_constants = n_constants / (sizeof(std::float32_t)*2);
        MPI_Win_lock_all(0, m_calibconst_win);
        if (m_shmem_rank == 0) {
          std::copy(m_calibconst.begin(), m_calibconst.end(), m_const_ptr);
          m_calibconst.clear();
        }
        MPI_Win_sync(m_calibconst_win);
        MPI_Barrier(m_shmem_comm);
        MPI_Win_unlock_all(m_calibconst_win);
        m_calibconst_span = std::span<CalibStruct>(m_const_ptr,n_constants);
      }
    }
  } // namespace MPI
} // namespace XTCPP
