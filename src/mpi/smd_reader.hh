#ifndef XTCPP_MPI_SMDREADER_HH
#define XTCPP_MPI_SMDREADER_HH

#include "common/smd_reader.hh" // XTCPP::Base::SMDReader and XTCPP::XtcOffset

#include "xtcdata/xtc/Dgram.hh"
#include "xtcdata/xtc/NamesLookup.hh"
#include "xtcdata/xtc/NameIndex.hh"

#include "mpi.h"

#include <stdio.h>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace XTCPP
{

  namespace MPI {
    class SMDReader final : public Base::SMDReader {
    public:
      SMDReader(std::string& smd_path, size_t max_dgram_size, size_t events_per_read);

      ~SMDReader() override;

      XtcData::Dgram* next();
      void read() override;  ///< Triggers reading of the .smd.xtc2
      char* wait() override; ///< Waits on a read and returns the pointer to block of offsets read
    private:
      MPI_File m_fh;

      char* m_file_buf0;
      char* m_file_buf1;

      char* m_read_ptr;
      size_t m_read_count{0};

      MPIO_Request m_read_req{MPI_REQUEST_NULL};
      MPIO_Request* m_req_ptr;

      char* m_buf;
      virtual void init_file() override;
    };
  } // namespace MPI
} // namespace XTCPP

#endif
