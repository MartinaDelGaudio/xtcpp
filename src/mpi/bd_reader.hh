#ifndef XTCPP_MPI_BDREADER_HH
#define XTCPP_MPI_BDREADER_HH

#include "common/bd_reader.hh"

#include "xtcdata/xtc/DescData.hh"
#include "xtcdata/xtc/Dgram.hh"
#include "xtcdata/xtc/NamesLookup.hh"

#include "mpi.h"
#include "spdlog/sinks/stdout_color_sinks.h"

#include <expected>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace XTCPP {
  namespace MPI {
    class BDReader final : public Base::BDReader {
    public:
      BDReader(MPI_Comm comm,
               std::string& smd_path,
               std::string& xtc_path,
               size_t events_per_read);
      BDReader(std::string& smd_path,
               std::string& xtc_path,
               size_t events_per_read);
      ~BDReader();

      int rank() const { return m_rank; }

      virtual std::expected<size_t, BDReadError>
      get_next_offsets() override;

      virtual std::expected<XtcData::Dgram*, BDReadError>
      get_dgram_at(size_t unwrapped_offset_idx) override;

      void close();
    private:
      MPI_File m_fh;
      MPI_Offset m_file_size;

      MPI_Comm m_comm;
      int m_rank;
      int m_n_ranks;

      MPI_Comm m_shmem_comm;
      int m_shmem_rank;
      int m_n_shmem_ranks;

      MPI_Datatype m_smd_offset_type;

      MPI_Win m_offset_win;

      char* m_dgram_buf;
      char* m_dgram_buf0;
      char* m_dgram_buf1;

      char* m_read_ptr;
      char* m_access_ptr;

      MPIO_Request m_dgram_req0;
      MPIO_Request* m_req_ptr;

      void* get_value(size_t idx, XtcData::Name& name, XtcData::DescData& descdata);

      void init_reader() override;

      std::shared_ptr<spdlog::logger> m_logger;
    };
  } // namespace MPI
} // namespace XTCPP

#endif // XTCPP_MPI_BDREADER_HH
