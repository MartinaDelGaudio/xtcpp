#include "bd_reader.hh"
#include "smd_reader.hh"

#include "xtcdata/xtc/DescData.hh"
#include "xtcdata/xtc/Dgram.hh"
#include "xtcdata/xtc/NamesLookup.hh"
#include "xtcdata/xtc/ShapesData.hh"

#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"

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

      size_t window_size{0};
      if (m_shmem_rank == 0) {
        window_size = sizeof(XtcOffset)*m_events_per_read;
      }
      MPI_Win_allocate_shared(window_size,
                              sizeof(XtcOffset),
                              MPI_INFO_NULL,
                              m_shmem_comm,
                              &m_offsets,
                              &m_offset_win);
      if (m_shmem_rank != 0) {
        MPI_Aint query_size;
        int disp_unit;
        MPI_Win_shared_query(m_offset_win,
                             0,
                             &query_size,
                             &disp_unit,
                             &m_offsets);
      }

      // We don't care about the dgram (configure) but the read populates the attributes
      [[maybe_unused]] XtcData::Dgram* dg = m_smd_reader->next();
      m_detnames = m_smd_reader->detnames();
      m_segment_nos = m_smd_reader->segment_numbers();
      m_serial_nos = m_smd_reader->serial_numbers();
      m_det_types = m_smd_reader->det_types();

      //commit_offset_type();
      m_read_ptr = m_dgram_buf0;
      m_access_ptr = m_dgram_buf1;
      m_req_ptr = &m_dgram_req0;

      if (m_rank == 0) {
        m_smd_reader->read();
      }
    }
    void BDReader::close() { MPI_File_close(&m_fh); }

    BDReader::~BDReader() {
      close();
      delete[] m_dgram_buf;
      delete[] m_dgram_buf0;
      delete[] m_dgram_buf1;
    }

    size_t BDReader::get_next_offsets() {
      MPI_Win_lock_all(0, m_offset_win);
      if (m_shmem_rank == 0) {
        size_t n_events = 0;
        m_smd_reader->wait();
        while (n_events < m_events_per_read) {
          XtcData::Dgram* dg = m_smd_reader->next(m_offsets);
          if (!dg) {
            break;
          }
          if (dg->service() == XtcData::TransitionId::L1Accept) {
            n_events++;
          }
        }
        m_smd_reader->read();
        m_num_events = n_events;
	m_logger->info("Read " + std::to_string(m_num_events) + " offsets");
      }
      MPI_Win_sync(m_offset_win);
      MPI_Bcast(&m_num_events, 1, MPI_UNSIGNED_LONG_LONG, 0, m_shmem_comm);
      MPI_Win_unlock_all(m_offset_win);
      return m_num_events;
    }

    XtcData::Dgram* BDReader::get_dgram(size_t unwrapped_offset_idx) {
      size_t offset_idx = unwrapped_offset_idx % m_events_per_read;
      if (offset_idx >= m_num_events) {
        return nullptr; // All data read.
      }
      XtcOffset& offset = m_offsets[offset_idx];

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
      int count;
      MPI_Get_count(&status, MPI_INT, &count);
      if (count == 0) {
        return nullptr;
      }
      if (rc != MPI_SUCCESS) {
        return nullptr;
      }
      m_payload_ptr = reinterpret_cast<XtcData::Xtc*>(dg->xtc.payload());
      m_remaining_payload = dg->xtc.sizeofPayload();
      return dg;
    }
  } // namespace MPI
} // namespace XTCPP
