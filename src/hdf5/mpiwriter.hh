#ifndef XTCPP_MPI_HDF5WRITER_HH
#define XTCPP_MPI_HDF5WRITER_HH

#include "hdf5writer.hh"

#include "mpi.h"

namespace XTCPP {
  namespace MPI {
    class HDF5Writer : public Base::HDF5Writer {
    public:
      HDF5Writer(MPI_Comm comm, size_t batch_size);
      HDF5Writer(MPI_Comm comm);
      HDF5Writer(size_t batch_size);
      HDF5Writer();

      int rank() const { return m_rank; }
      int size() const { return m_n_ranks; }

      virtual void open_file() override;

    private:
      MPI_Comm m_comm;
      int m_rank;
      int m_n_ranks;
    };
  } // namespace MPI
} // namespace XTCPP

#endif // XTCPP_MPI_HDF5WRITER_HH
