#include "mpi/bd_reader.hh"

#include "common/bd_reader.hh"
#include "common/smd_reader.hh"
#include "mpi/smd_reader.hh"

#include "xtcdata/xtc/DescData.hh"
#include "xtcdata/xtc/Dgram.hh"
#include "xtcdata/xtc/NamesLookup.hh"
#include "xtcdata/xtc/ShapesData.hh"

#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"

#include <expected>
#include <iostream>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

namespace XTCPP {
  namespace MPI {
    BDReader::BDReader(MPI_Comm comm,
                       std::string& smd_path,
                       std::string& xtc_path,
                       size_t events_per_read)
      : Base::BDReader(smd_path, xtc_path, events_per_read)
      , m_comm(comm)
      , m_dgram_buf(new char[0x4000000])
      , m_dgram_buf0(new char[0x4000000])
      , m_dgram_buf1(new char[0x4000000])
    {
      if (auto tmp = spdlog::get("MPI::BDReader")) {
        m_logger = tmp;
      } else {
        m_logger = spdlog::stdout_color_mt("MPI::BDReader");
      }
      init_reader();
    }

    BDReader::BDReader(std::string& smd_path,
                       std::string& xtc_path,
                       size_t events_per_read)
      : BDReader(MPI_COMM_WORLD, smd_path, xtc_path, events_per_read)
    {}

    void BDReader::init_reader() {
      m_smd_reader = std::make_unique<SMDReader>(m_smd_path, 0x4000000, m_events_per_read);
      MPI_File_open(m_comm, m_xtc_path.c_str(), MPI_MODE_RDONLY,
                    MPI_INFO_NULL, &m_fh);
      MPI_File_get_size(m_fh, &m_file_size);
      MPI_Comm_rank(m_comm, &m_rank);
      MPI_Comm_size(m_comm, &m_n_ranks);



      MPI_Comm_split_type(m_comm, MPI_COMM_TYPE_SHARED, m_rank, MPI_INFO_NULL, &m_shmem_comm);
      MPI_Comm_rank(m_shmem_comm, &m_shmem_rank);
      MPI_Comm_size(m_shmem_comm, &m_n_shmem_ranks);

      size_t offset_window_size{0};
      size_t slow_update_idx_window_size{0};
      size_t slow_update_dgram_window_size{0};
      if (m_shmem_rank == 0) {
        offset_window_size = sizeof(BDXtcOffset)*m_events_per_read;
        // Wasteful to allocate so much, but we don't know how many...
        slow_update_idx_window_size = sizeof(SlowUpdateXtcOffset)*m_events_per_read;
        slow_update_dgram_window_size = 0x4000000;
      }
      MPI_Win_allocate_shared(offset_window_size,
                              sizeof(BDXtcOffset),
                              MPI_INFO_NULL,
                              m_shmem_comm,
                              &m_offsets,
                              &m_offset_win);

      MPI_Win_allocate_shared(slow_update_idx_window_size,
                              sizeof(SlowUpdateXtcOffset),
                              MPI_INFO_NULL,
                              m_shmem_comm,
                              &m_slow_updates,
                              &m_slow_update_idx_win);

      MPI_Win_allocate_shared(slow_update_dgram_window_size,
                              sizeof(char),
                              MPI_INFO_NULL,
                              m_shmem_comm,
                              &m_slow_update_dgram_buf,
                              &m_slow_update_dgram_win);

      if (m_shmem_rank != 0) {
        MPI_Aint query_size;
        int disp_unit;
        MPI_Win_shared_query(m_offset_win,
                             0,
                             &query_size,
                             &disp_unit,
                             &m_offsets);

        MPI_Win_shared_query(m_slow_update_idx_win,
                             0,
                             &query_size,
                             &disp_unit,
                             &m_slow_updates);

        MPI_Win_shared_query(m_slow_update_dgram_win,
                             0,
                             &query_size,
                             &disp_unit,
                             &m_slow_update_dgram_win);
      }

      // We don't care about the dgram (configure) but the read populates the attributes
      auto ret = m_smd_reader->read();
      if (ret.has_value()) {
        m_detnames = m_smd_reader->detnames();
        m_segment_nos = m_smd_reader->segment_numbers();
        m_serial_nos = m_smd_reader->serial_numbers();
        m_det_types = m_smd_reader->det_types();
        m_epics_detnames = m_smd_reader->epics_detnames();

        m_read_ptr = m_dgram_buf0;
        m_access_ptr = m_dgram_buf0;

        if (m_rank == 0) {
          auto iret = m_smd_reader->iread();
          if (!iret.has_value()) {
            /// Handle errors...
          }
        }
      } else {
        // Handle errors with ret.error() checks....
      }
    }
    void BDReader::close() { MPI_File_close(&m_fh); }

    BDReader::~BDReader() {
      close();
      delete[] m_dgram_buf;
      delete[] m_dgram_buf0;
      delete[] m_dgram_buf1;
    }

    std::expected<size_t, BDReadError> BDReader::get_next_offsets() {
      MPI_Win_lock_all(0, m_offset_win);
      if (m_shmem_rank == 0) {
        size_t n_events {0};
        size_t n_slow_updates {0};
        auto ret = m_smd_reader->wait();
        if (ret.has_value()) {
          while (n_events < m_events_per_read) {
            XtcData::Dgram* dg = m_smd_reader->get_offset_into(m_offsets,
                                                               m_slow_updates);
            if (!dg) {
              break;
            }
            if (dg->service() == XtcData::TransitionId::L1Accept) {
              n_events++;
            } else if (dg->service() == XtcData::TransitionId::SlowUpdate) {
              n_slow_updates++;
            }
          }
          m_smd_reader->iread();
          m_num_events = n_events;
          m_num_slow_updates = n_slow_updates;
          m_logger->info("Read " + std::to_string(m_num_events) + " offsets (W/ " +
                         std::to_string(m_num_slow_updates) + " slow updates)");
        } else if (ret.error() == SMDReadError::ZeroBytesRead) {
          m_num_events = 0;
        } else {
          /// Handle errors...
        }
      }
      MPI_Win_sync(m_offset_win);
      MPI_Bcast(&m_num_events, 1, MPI_UNSIGNED_LONG_LONG, 0, m_shmem_comm);
      MPI_Bcast(&m_num_slow_updates, 1, MPI_LONG_LONG, 0, m_shmem_comm);
      MPI_Win_unlock_all(m_offset_win);
      return m_num_events;
    }

    std::expected<void, BDReadError>
    BDReader::read_slowupdate_at(size_t unwrapped_offset_idx) {
      SlowUpdateXtcOffset su_offset = m_slow_updates[m_curr_slow_update_index];
      ssize_t prev_l1_idx = su_offset.previous_l1_index;
      if (prev_l1_idx <= static_cast<ssize_t>(unwrapped_offset_idx) &&
          m_curr_slow_update_index < m_num_slow_updates) {
        m_curr_slow_update_index++;
        size_t dgram_size = su_offset.size;
        MPI_Offset file_offset;
        if (prev_l1_idx == -1) {
          /* Sigh... For some reason transition sizes in .xtc2 and .smd.xtc2
             files are different. See more comments in common/smd_reader.cc...
             So if prev_l1_idx is -1, figure out the offset from the L1Accept
             offset that follows...
          */
          BDXtcOffset& l1_offset = m_offsets[0];
          file_offset = l1_offset.offset - dgram_size;
        } else {
          file_offset = su_offset.offset;
        }

        XtcData::Dgram* dg = reinterpret_cast<XtcData::Dgram*>(m_slow_update_dgram_buf);
        MPI_Win_lock_all(0, m_slow_update_dgram_win);
        if (m_shmem_rank == 0) {
          MPI_Status status;
          std::memset(&status, 0, sizeof(MPI_Status));
          int rc = MPI_File_read_at(m_fh,
                                    file_offset,
                                    dg,
                                    dgram_size,
                                    MPI_BYTE,
                                    &status);
          /* An error mechanism is needed to distribute the info.. */
          if (rc != MPI_SUCCESS) {
            char error_buf[256];
            int error_buf_len;
            MPI_Error_string(rc, error_buf, &error_buf_len);
            m_logger->error("*** read was unsuccessful: " +
                            std::string(error_buf));
            return std::unexpected(BDReadError::GeneralIOError);
          }
          int count;
          MPI_Get_count(&status, MPI_BYTE, &count);
          if (count == 0) {
            return std::unexpected(BDReadError::ZeroBytesRead);
          } else if (count == MPI_UNDEFINED) {
            // This happens if count is not a multiple of the element type
            // The element type is the one used for the read (MPI_BYTE)
            m_logger->error("*** On read, read was not a multiple of MPI_BYTE");
            return std::unexpected(BDReadError::GeneralIOError);
          }
        }
        MPI_Win_sync(m_slow_update_dgram_win);
        MPI_Win_unlock_all(m_slow_update_dgram_win);
        m_payload_ptr = reinterpret_cast<XtcData::Xtc*>(dg->xtc.payload());
        m_remaining_payload = dg->xtc.sizeofPayload();
      }
      return {};
    }

    std::expected<void, BDReadError>
    BDReader::read_l1_at(size_t unwrapped_offset_idx) {
      size_t offset_idx = unwrapped_offset_idx % m_events_per_read;
      if (offset_idx >= m_num_events) {
        return std::unexpected(BDReadError::AllDgramOffsetsRead);
      }
      BDXtcOffset& offset = m_offsets[offset_idx];

      XtcData::Dgram* dg = reinterpret_cast<XtcData::Dgram*>(m_dgram_buf);
      MPI_Status status;

      MPI_Offset file_offset = offset.offset;
      size_t dgram_size = offset.size;
      int rc = MPI_File_read_at(m_fh,
                                file_offset,
                                dg,
                                dgram_size,
                                MPI_BYTE,
                                &status);

      if (rc != MPI_SUCCESS) {
        return std::unexpected(BDReadError::GeneralIOError);
      }

      int count;
      MPI_Get_count(&status, MPI_BYTE, &count);
      if (count == 0) {
        return std::unexpected(BDReadError::ZeroBytesRead);
      } else if (count == MPI_UNDEFINED) {
        // This happens if count is not a multiple of the element type
        // The element type is the one used for the read (MPI_BYTE)
        m_logger->error("*** On read, read was not a multiple of MPI_BYTE");
        return std::unexpected(BDReadError::GeneralIOError);
      }

      m_payload_ptr = reinterpret_cast<XtcData::Xtc*>(dg->xtc.payload());
      m_remaining_payload = dg->xtc.sizeofPayload();
      return {};
    }


    std::expected<void, BDReadError>
    BDReader::iread_l1_at(size_t unwrapped_offset_idx) {
      size_t offset_idx = unwrapped_offset_idx % m_events_per_read;
      if (offset_idx >= m_num_events) {
        return std::unexpected(BDReadError::AllDgramOffsetsRead);
      }
      BDXtcOffset& offset = m_offsets[offset_idx];

      XtcData::Dgram* dg = reinterpret_cast<XtcData::Dgram*>(m_read_ptr);

      MPI_Offset file_offset = offset.offset;
      size_t dgram_size = offset.size;

      //std::memset(&m_dgram_req, 0, sizeof(MPIO_Request));
      int rc = MPI_File_iread_at(m_fh,
                                 file_offset,
                                 dg,
                                 dgram_size,
                                 MPI_BYTE,
                                 &m_dgram_req);
      if (rc != MPI_SUCCESS) {
        char error_buf[256];
        int error_buf_len;
        MPI_Error_string(rc, error_buf, &error_buf_len);
        m_logger->error("*** iread was unsuccessful: " + std::string(error_buf));
        return std::unexpected(BDReadError::GeneralIOError);
      }

      return {};
    }

    std::expected<void, BDReadError> BDReader::wait() {
      MPI_Status status;
      std::memset(&status, 0, sizeof(MPI_Status));
      int rc = MPI_Wait(&m_dgram_req, &status);

      if (rc != MPI_SUCCESS) {
        char error_buf[256];
        int error_buf_len;
        MPI_Error_string(rc, error_buf, &error_buf_len);
        m_logger->error("*** Wait was unsuccessful: " + std::string(error_buf));
        return std::unexpected(BDReadError::GeneralIOError);
      }

      if (status.MPI_ERROR != MPI_SUCCESS) {
        char error_buf[256];
        int error_buf_len;
        MPI_Error_string(status.MPI_ERROR, error_buf, &error_buf_len);
        m_logger->error("*** Wait was unsuccessful: " + std::string(error_buf));
        return std::unexpected(BDReadError::GeneralIOError);
      }

      int count;
      MPI_Get_count(&status, MPI_BYTE, &count);
      if (count == 0) {
        return std::unexpected(BDReadError::ZeroBytesRead);
      } else if (count == MPI_UNDEFINED) {
        // This happens if count is not a multiple of the element type
        // The element type is the one used for the read (MPI_BYTE)
        m_logger->error("*** On wait for iread, read was not a multiple of MPI_BYTE");
        return std::unexpected(BDReadError::GeneralIOError);
      }

      XtcData::Dgram* dg = reinterpret_cast<XtcData::Dgram*>(m_read_ptr);
      m_payload_ptr = reinterpret_cast<XtcData::Xtc*>(dg->xtc.payload());
      m_remaining_payload = dg->xtc.sizeofPayload();
      return {};
    }
  } // namespace MPI
} // namespace XTCPP
