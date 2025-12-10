#include "smd_reader.hh"

#include "common/smd_reader.hh"

#include "xtcdata/xtc/DescData.hh"
#include "xtcdata/xtc/NamesLookup.hh"
#include "xtcdata/xtc/ShapesData.hh"

#include "mpi.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/spdlog.h"

#include <algorithm>
#include <expected>
#include <iostream>
#include <map>
#include <memory>
#include <string>


namespace XTCPP {

  namespace MPI {
    SMDReader::SMDReader(std::string& smd_path,
                         size_t max_dgram_size,
                         size_t events_per_read)
      : Base::SMDReader(smd_path, max_dgram_size, events_per_read)
      , m_buf(new char[max_dgram_size])
      , m_file_buf0(new char[(sizeof(XtcData::Dgram) + 80) * events_per_read])
      , m_file_buf1(new char[(sizeof(XtcData::Dgram) + 80) * events_per_read])
    {
      init_file();
      if (auto tmp = spdlog::get("MPI::SMDReader")) {
        m_logger = tmp;
      } else {
        m_logger = spdlog::stdout_color_mt("MPI::SMDReader");
      }
    }

    void SMDReader::init_file() {
      MPI_File_open(MPI_COMM_SELF, m_smd_path.c_str(), MPI_MODE_RDONLY,
                    MPI_INFO_NULL, &m_fh);
      MPI_Offset file_size;
      MPI_File_get_size(m_fh, &file_size);
      m_file_size = static_cast<size_t>(file_size);
      m_read_ptr = m_file_buf0;
      m_access_ptr = m_file_buf1;
    }

    SMDReader::~SMDReader() {
      delete[] m_buf;
      delete[] m_file_buf0;
      delete[] m_file_buf1;
      MPI_File_close(&m_fh);
    }

    std::expected<void, SMDReadError> SMDReader::read() {
      XtcData::Dgram& dg = *reinterpret_cast<XtcData::Dgram*>(m_buf);
      MPI_Status status;
      MPI_Offset dgram_offset = m_file_offset;
      int rc = MPI_File_read_at(m_fh,
                                dgram_offset,
                                &dg,
                                sizeof(XtcData::Dgram),
                                MPI_BYTE,
                                &status);
      if (rc != MPI_SUCCESS) {
        char error_buf[256];
        int error_buf_len;
        MPI_Error_string(status.MPI_ERROR, error_buf, &error_buf_len);
        m_logger->error("*** [read] Unable to read dgram header: " +
                        std::string(error_buf));
        return std::unexpected(SMDReadError::DgramHeaderError);
      }
      int count;
      MPI_Get_count(&status, MPI_INT, &count);
      if (count == 0) {
        return std::unexpected(SMDReadError::ZeroBytesRead);
      }
      size_t payload_size = dg.xtc.sizeofPayload();
      rc = MPI_File_read_at(m_fh,
                            dgram_offset + sizeof(dg),
                            dg.xtc.payload(),
                            payload_size,
                            MPI_BYTE,
                            &status);
      if (rc != MPI_SUCCESS) {
        char error_buf[256];
        int error_buf_len;
        MPI_Error_string(status.MPI_ERROR, error_buf, &error_buf_len);
        m_logger->error("*** [read] Unable to read payload: " + std::string(error_buf));
        return std::unexpected(SMDReadError::GeneralIOError);
      }

      recurse_dgram_xtcs(&dg.xtc, dg.service());
      m_file_offset += sizeof(dg) + payload_size;
      return {};
    }

    std::expected<void, SMDReadError> SMDReader::iread() {
      size_t read_size = (sizeof(XtcData::Dgram) + 80) * m_events_per_read;

      if (m_access_offset) {
        size_t missing_chunk = m_read_count - m_access_offset;
        m_file_offset -= missing_chunk;
      }
      size_t diff = m_file_size - m_file_offset;
      read_size = read_size > diff ? diff : read_size;

      std::memset(&m_read_req, 0, sizeof(MPIO_Request));
      MPI_File_iread_at(m_fh,
                        m_file_offset,
                        m_read_ptr,
                        read_size,
                        MPI_BYTE,
                        &m_read_req);
      return {};
    }

    std::expected<void, SMDReadError> SMDReader::wait() {
      MPI_Status status;
      std::memset(&status, 0, sizeof(status));
      int rc = MPI_Wait(&m_read_req, &status);

      if (rc != MPI_SUCCESS) {
        char error_buf[256];
        int error_buf_len;
        MPI_Error_string(rc, error_buf, &error_buf_len);
        m_logger->error("*** Wait was unsuccessful: " + std::string(error_buf));
      }

      if (status.MPI_ERROR != MPI_SUCCESS) {
        char error_buf[256];
        int error_buf_len;
        MPI_Error_string(status.MPI_ERROR, error_buf, &error_buf_len);
        m_logger->error("*** Wait was unsuccessful: " + std::string(error_buf));
      }

      int count;
      MPI_Get_count(&status, MPI_INT, &count);
      if (count == 0) {
        return std::unexpected(SMDReadError::ZeroBytesRead);
      }

      if (count < 0) {
        m_logger->error("*** On waiting for iread, bytes read returned negative?");
      }

      m_file_offset += count;
      m_read_count = count;

      auto* tmp = m_read_ptr;
      m_read_ptr = m_access_ptr;
      m_access_ptr = tmp;

      m_access_offset = 0;
      size_t rs = (sizeof(XtcData::Dgram)+80)*m_events_per_read;
      std::fill(m_read_ptr,m_read_ptr+rs,0);
      return {};
    }
  } // namespace MPI
} // namespace XTCPP
