#include "hdf5writer.hh"
#include "mpiwriter.hh"

#include "mpi.h"

#include <sstream>

namespace XTCPP {
  namespace MPI {
    HDF5Writer::HDF5Writer(MPI_Comm comm, size_t batch_size)
      : Base::HDF5Writer(batch_size)
      , m_comm(comm) {
      MPI_Comm_rank(m_comm, &m_rank);
      MPI_Comm_size(m_comm, &m_n_ranks);
    }
    HDF5Writer::HDF5Writer(MPI_Comm comm) : HDF5Writer(comm, 2) {}

    HDF5Writer::HDF5Writer() : HDF5Writer(MPI_COMM_WORLD, 2) {}

    HDF5Writer::HDF5Writer(size_t batch_size)
      : HDF5Writer(MPI_COMM_WORLD, batch_size) {}

    void HDF5Writer::open_file() {
      std::ostringstream oss;
      oss << "test_" << m_rank << ".h5";
      m_h5 = new H5::H5File(oss.str().c_str(), H5F_ACC_TRUNC);
    }
  } // namespace MPI
} // namespace XTCPP
