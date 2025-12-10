#ifndef XTCPP_MPI_SMDREADER_HH
#define XTCPP_MPI_SMDREADER_HH

#include "common/smd_reader.hh" // XTCPP::Base::SMDReader and XTCPP::BDXtcOffset

#include "xtcdata/xtc/Dgram.hh"
#include "xtcdata/xtc/NamesLookup.hh"
#include "xtcdata/xtc/NameIndex.hh"

#include "mpi.h"
#include "spdlog/sinks/stdout_color_sinks.h"

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
      /* Synchronous API  */
      /**
       * Perform a synchronous (blocking) read. This API is implemented to read ONE
       * datagram only. This is useful, for instance, for reading configure transitions.
       */
      virtual std::expected<void, SMDReadError> read() override;

      /* Asynchronous API */
      /**
       * Trigger an asynchronous (non-blocking) read. This API will read up to
       * `events_per_read` L1Accepts into a buffer.
       */
      virtual std::expected<void, SMDReadError> iread() override;

      /**
       * Wait on a previously triggered asynchronous read.
       */
      virtual std::expected<void, SMDReadError> wait() override;

    private:
      virtual void init_file() override;

    private:
      MPI_File m_fh;

      char* m_buf;
      char* m_file_buf0;
      char* m_file_buf1;

      char* m_read_ptr;
      size_t m_read_count{0};

      MPIO_Request m_read_req{MPI_REQUEST_NULL};

      std::shared_ptr<spdlog::logger> m_logger;
    };
  } // namespace MPI
} // namespace XTCPP

#endif
