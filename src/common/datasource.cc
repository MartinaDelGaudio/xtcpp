#include "datasource.hh"
#include "bd_reader.hh"

#include "spdlog/spdlog.h"
#include "spdlog/cfg/env.h"
#include "spdlog/sinks/stdout_color_sinks.h"

#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <numeric>
#include <ostream>
#include <string>
#include <variant>
#include <vector>

namespace fs = std::filesystem;

namespace XTCPP {
  namespace Base {
    DataSource::DataSource(std::string exp,
                           std::variant<std::string, int> run,
                           size_t events_per_read)
      : m_experiment(exp)
      , m_hutch(m_experiment.substr(0,3))
      , m_events_per_read(events_per_read)
    {
      if (std::holds_alternative<std::string>(run)) {
        m_run = std::get<std::string>(run);
      } else {
        m_run = std::to_string(std::get<int>(run));
      }
      spdlog::cfg::load_env_levels("XTCPP_LOG_LEVEL");
      if (auto tmp = spdlog::get("Base::DataSource")) {
	m_logger = tmp;
      } else {
	m_logger = spdlog::stdout_color_mt("Base::DataSource");
      }
    }

    size_t DataSource::load_next_offsets() {
      size_t n_new_offsets{0};
      for (auto& reader : m_xtc_readers_in_use) {
        auto ret = reader->get_next_offsets();
        if (ret.has_value()) {
          size_t n_det_new_offsets = ret.value();
            n_new_offsets =
            n_det_new_offsets > n_new_offsets ? n_det_new_offsets : n_new_offsets;
        } else {
          // Handle errors?
        }
      }
      return n_new_offsets;
    }

    DataSource::Iterator& DataSource::Iterator::operator++() {
      size_t curr_idx = m_ds->fetch_next_idx();
      if (curr_idx > m_ds->m_last_offset_index) {
        size_t n_new_offsets = m_ds->load_next_offsets();
        if (n_new_offsets) {
          size_t old_last_idx = m_ds->m_last_offset_index;
          m_ds->m_last_offset_index += n_new_offsets - 1;
          std::iota(m_ds->m_offset_indices.begin(),
                    m_ds->m_offset_indices.end(),
                    old_last_idx);
        } else {
          curr_idx = m_ds->m_offset_indices.size();
          m_ptr = m_ds->m_offset_indices.data() + curr_idx;
          return *this;
        }
      }
      curr_idx = curr_idx % m_ds->m_events_per_read;
      m_ptr = m_ds->m_offset_indices.data() + curr_idx;
      return *this;
    }

    DataSource::Iterator DataSource::Iterator::operator++(int) {
      DataSource::Iterator tmp = *this;
      ++(*this);
      return tmp;
    }

    DataSource::Iterator::Iterator(size_t* ptr)
      : m_ptr(ptr)
      , m_ds(nullptr)
    {}

    DataSource::Iterator::Iterator(size_t* ptr, DataSource* ds, bool init)
      : m_ptr(ptr)
      , m_ds(ds)
    {
      if (init) {
        size_t offset = m_ds->fetch_next_idx();
        m_ptr += offset;
      }
    }

    DataSource::Iterator DataSource::begin() {
      return DataSource::Iterator(m_offset_indices.data(), this, true);
    }

    DataSource::Iterator DataSource::end() {
      return DataSource::Iterator(m_offset_indices.data() + m_offset_indices.size(), this, false);
    }
  } // namespace Base
} // namespace XTCPP

