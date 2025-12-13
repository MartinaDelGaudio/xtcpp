#include "common/detector.hh"

#include "common/bd_reader.hh"

#include "xtcdata/xtc/Dgram.hh"

#include "httplib.h"
#include "rapidjson/document.h"
#include "spdlog/spdlog.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
//#include <mdspan>
#include <memory>
#include <numeric>
#include <optional>
#include <ostream>
#include <stdfloat>
#include <string>
#include <utility>
#include <vector>

namespace XTCPP {
  void calibrate(std::vector<void*>& data_ptrs,
		 std::span<CalibStruct>& calibconst,
		 std::vector<std::float32_t>& calib_data) {
    auto data_mask = 0x3FFF;
    constexpr size_t NSEGS{32};
    constexpr size_t NROWS{512};
    constexpr size_t NCOLS{1024};
    constexpr size_t NPIX {NSEGS * NROWS * NCOLS};
    constexpr size_t PIX_PER_SEG {NROWS * NCOLS};

    #pragma omp parallel for schedule(static)
    for (size_t seg=0; seg < NSEGS; ++seg) {
      auto* raw_data = reinterpret_cast<std::uint16_t*>(data_ptrs[seg]);

      for (size_t panel_idx=0; panel_idx < PIX_PER_SEG; ++panel_idx) {
        size_t idx = seg*PIX_PER_SEG + panel_idx;

        uint16_t raw_pixel = raw_data[panel_idx];
        uint16_t data = raw_pixel & data_mask;
        size_t gain_idx = raw_pixel >> 14; // Top two bits
        if (gain_idx > 1) {
          if (gain_idx == 2) [[unlikely]] {
            /* This is 0b10 - a bad pixel, hopefully unlikely - what should happen? */
            continue;
          } else [[likely]] {
            gain_idx--; // Map gain_idx 3 (0b11 - low gain) to index 2
          }
        }
        size_t calib_idx = gain_idx * NPIX + idx;

        if (calib_idx >= calibconst.size()) {
          std::cerr << "Invalid calib_idx: " << calib_idx << ", size is: " << calibconst.size()
                    << ", gain idx is: " << gain_idx << std::endl;
          continue;
        }
        calib_data[idx] =
          (data - calibconst[calib_idx].ped) * calibconst[calib_idx].gain;
      }
    }
  }

  std::vector<std::float32_t> calibrate(std::vector<void*>& data_ptrs,
                                        std::span<CalibStruct>& calibconst) {
    auto data_mask = 0x3FFF;
    constexpr size_t NSEGS{32};
    constexpr size_t NROWS{512};
    constexpr size_t NCOLS{1024};
    constexpr size_t NPIX {NSEGS * NROWS * NCOLS};
    constexpr size_t PIX_PER_SEG {NROWS * NCOLS};

    std::vector<std::float32_t> data_out(32*512*1024);

#pragma omp parallel for schedule(static)
    for (size_t idx=0; idx < NPIX; ++idx) {
      size_t seg = idx / PIX_PER_SEG;
      size_t panel_idx = idx % PIX_PER_SEG;

      auto* raw_data = reinterpret_cast<std::uint16_t*>(data_ptrs[seg]);
      uint16_t raw_pixel = raw_data[panel_idx];
      uint16_t data = raw_pixel & data_mask;
      size_t gain_idx = raw_pixel >> 14; // Top two bits
      if (gain_idx > 1) {
        if (gain_idx == 2) [[unlikely]] {
          /* This is 0b10 - a bad pixel, hopefully unlikely - what should happen? */
          continue;
        } else [[likely]] {
          gain_idx--; // Map gain_idx 3 (0b11 - low gain) to index 2
        }
      }
      size_t calib_idx = gain_idx * NPIX + idx;

      if (calib_idx >= calibconst.size()) {
        std::cerr << "Invalid calib_idx: " << calib_idx << ", size is: " << calibconst.size()
                  << ", gain idx is: " << gain_idx << std::endl;
        continue;
      }
      data_out[idx] =
        (data - calibconst[calib_idx].ped) * calibconst[calib_idx].gain;
    }

    return data_out;
  }

  namespace Base {
    Detector::Detector(std::string detname,
                       std::string serial_no,
                       std::vector<unsigned> segment_nos,
                       std::vector<std::shared_ptr<BDReader>> xtc_readers,
                       std::string experiment,
                       std::string run,
                       bool is_epics)
      : m_detname(detname)
      , m_serial_no(serial_no)
      , m_det_type(xtc_readers[0]->det_types()[m_detname])
      , m_experiment(experiment)
      , m_run(run)
      , m_segment_nos(segment_nos)
      , m_xtc_readers(xtc_readers)
      , m_data_ptrs(m_segment_nos.size())
      , m_data_sizes(m_segment_nos.size())
      , m_calib_data(32*512*1024)
      , m_is_epics(is_epics)
    {
      if (auto tmp = spdlog::get("Base::Detector")) {
        m_logger = tmp;
      } else {
        m_logger = spdlog::stdout_color_mt("Base::Detector");
      }
      get_detector_short_name();

      const char* det_read_mode = std::getenv("XTCPP_DET_GETDATA");
      if (det_read_mode && std::string(det_read_mode) == "THREADED") {
        get_l1_data_impl = &Detector::get_l1_data_threaded;
        // ThreadPool is not copyable/moveable - construct in place
        m_thread_pool.emplace(m_xtc_readers.size());
      } else {
        get_l1_data_impl = &Detector::get_l1_data_sequential;
        m_thread_pool = std::nullopt;
      }
    }

    void Detector::load_all_calib_constants() {
      // Database all refers to shortname, without cannot get constants
      m_logger->info("*** Starting to load calibration constants ***");
      auto [peds, shape_peds] = load_calib_constants_type("pedestals");
      auto [offs, shape_offs] = load_calib_constants_type("pixel_offset");
      auto [gain, shape_gain] = load_calib_constants_type("pixel_gain");

      // Assume for now they are all the same shape
      size_t total_entries = std::reduce(shape_peds.begin(),
                                         shape_peds.end(),
                                         1,
                                         std::multiplies{});
      m_logger->trace("Constructing m_calibconst vector with {} entries",
                      total_entries);
      m_calibconst.resize(total_entries);
      for (size_t idx=0; idx < total_entries; ++idx) {
        auto ped_off = peds[idx];
        if (!offs.empty()) {
          ped_off += offs[idx];
        }
        std::float32_t gain_val;
        if (!gain.empty()) {
          gain_val = gain[idx];
        } else {
          gain_val = 1.0f;
        }
        m_calibconst[idx] = CalibStruct(ped_off, gain_val);
      }
      m_logger->info("*** Done loading calibration constants ***");
    }

    void Detector::get_detector_short_name() {
      httplib::Client cli("https://pswww.slac.stanford.edu");
      std::string detnames_endpoint = "/calib_ws/cdb_detnames/" + m_det_type;

      m_logger->trace("Searching for `shortname` for " + m_detname);
      if (auto res = cli.Get(detnames_endpoint)) {
        rapidjson::Document docs;
        docs.Parse(res->body.c_str());
        if (!docs.IsArray()) {
          std::cerr << " Not a list of docs! " << std::endl;
          return;
        }

        for (const auto& doc : docs.GetArray()) {

          if (!doc.IsObject()) continue;

          std::string ser_no = doc["long"].GetString();
          if (ser_no == m_serial_no) {
            m_short_name = doc["short"].GetString();
            m_logger->debug("Found short name `" + m_short_name + "` for serial number: "
                            + m_serial_no);
            return;
          }
        }
      } else {
        m_logger->error("Could not find shortname for " + m_detname);
        return;
      }
    }

    std::vector<std::string> Detector::split_string(const std::string& s,
                                                    const std::string& delim) {
      std::vector<std::string> parts;
      size_t nextPos{0};
      size_t lastPos{0};

      std::string part;
      while ((nextPos = s.find(delim, lastPos)) != std::string::npos) {
        part = s.substr(lastPos,nextPos - lastPos);
        if (!part.empty()) {
          parts.push_back(part);
        }
        lastPos = nextPos + 1;
      }
      part = s.substr(lastPos);
      parts.push_back(part);
      return parts;
    }

    std::pair<std::vector<std::float32_t>,std::vector<size_t>>
    Detector::load_calib_constants_type(std::string constants_type) {

      std::string data_type{""}; // Whether an array etc
      std::string data_dtype{""}; // If an array whats the dtype (e.g. float32)
      size_t data_ndim{0}; // Number of dimensions
      size_t data_size{0}; // Total number of pixels
      std::vector<size_t> data_shape(0); // Shape
      std::vector<std::float32_t> constants;
      if (m_short_name.empty()) {
        m_logger->error("Cannot load constants without short name!");
        return std::make_pair(constants,data_shape);
      }
      httplib::Client cli("https://pswww.slac.stanford.edu");

      std::string db_in_use = "cdb_" + m_experiment;

      std::string data_doc_id{""};
      std::string exp_endpoint = "/calib_ws/" + db_in_use + "/" + m_short_name;
      m_logger->debug("Getting " + constants_type + " for " + m_short_name);
      m_logger->debug("Trying experiment endpoint: " + exp_endpoint);
      if (auto res = cli.Get(exp_endpoint)) {
        rapidjson::Document docs;
        docs.Parse(res->body.c_str());
        if (!docs.IsArray()) {
          m_logger->error("Not a list of docs from " + exp_endpoint);
        } else {
          for (const auto& doc : docs.GetArray()) {
            if (!doc.IsObject()) {
              continue;
            }
            std::string doc_constants_type = doc["ctype"].GetString();
            if (constants_type != doc_constants_type) {
              continue;
            }

            // Really --- Here would need to do the run validity checks!!!
            data_doc_id = doc["id_data"].GetString();
            m_logger->debug("Selected data ID: " + data_doc_id);
            data_type = doc["data_type"].GetString();
            data_dtype = doc["data_dtype"].GetString();
            data_ndim = static_cast<size_t>(std::atoi(doc["data_ndim"].GetString()));
            data_size = static_cast<size_t>(std::atoi(doc["data_size"].GetString()));

            std::string data_shape_str = doc["data_shape"].GetString();
            data_shape_str = data_shape_str.substr(1, data_shape_str.size()-2);
            std::vector<std::string> shape_parts = split_string(data_shape_str, ",");
            data_shape.resize(data_ndim);
            for (size_t i=0; i < data_ndim; i++) {
              data_shape[i] = static_cast<size_t>(std::stoi(shape_parts[i]));
            }
            break;
          }
        }
      } else {
        db_in_use = "cdb_" + m_short_name;
        std::string det_endpoint = "/calib_ws/" + db_in_use + "/" + m_short_name;
        m_logger->debug("Trying det endpoint (exp doesn't exist): " + det_endpoint);
        if (auto res = cli.Get(det_endpoint)) {
          rapidjson::Document docs;
          docs.Parse(res->body.c_str());
          if (!docs.IsArray()) {
            m_logger->error("Not a list of docs from " + exp_endpoint);
          } else {
            for (const auto& doc : docs.GetArray()) {
              if (!doc.IsObject()) {
                continue;
              }
              std::string doc_constants_type = doc["ctype"].GetString();
              if (constants_type != doc_constants_type) {
                continue;
              }

              // Really --- Here would need to do the run validity checks!!!
              data_doc_id = doc["id_data"].GetString();
              m_logger->debug("Selected data ID: " + data_doc_id);
              data_type = doc["data_type"].GetString();
              data_dtype = doc["data_dtype"].GetString();
              data_ndim = static_cast<size_t>(std::atoi(doc["data_ndim"].GetString()));
              data_size = static_cast<size_t>(std::atoi(doc["data_size"].GetString()));

              std::string data_shape_str = doc["data_shape"].GetString();
              data_shape_str = data_shape_str.substr(1, data_shape_str.size()-2);
              std::vector<std::string> shape_parts = split_string(data_shape_str, ",");
              data_shape.resize(data_ndim);
              for (size_t i=0; i < data_ndim; i++) {
                data_shape[i] = static_cast<size_t>(std::stoi(shape_parts[i]));
              }
              break;
            }
          }
        }
      }
      if (data_doc_id.empty()) {
        db_in_use = "cdb_" + m_short_name;
        std::string det_endpoint = "/calib_ws/" + db_in_use + "/" + m_short_name;
        m_logger->debug("Trying det endpoint (data id not found): " + det_endpoint);
        if (auto res = cli.Get(det_endpoint)) {
          rapidjson::Document docs;
          docs.Parse(res->body.c_str());
          if (!docs.IsArray()) {
            m_logger->error("Not a list of docs from " + exp_endpoint);
          } else {
            for (const auto& doc : docs.GetArray()) {
              if (!doc.IsObject()) {
                continue;
              }
              std::string doc_constants_type = doc["ctype"].GetString();
              if (constants_type != doc_constants_type) {
                continue;
              }

              // Really --- Here would need to do the run validity checks!!!
              data_doc_id = doc["id_data"].GetString();
              m_logger->debug("Selected data ID: " + data_doc_id);
              data_type = doc["data_type"].GetString();
              data_dtype = doc["data_dtype"].GetString();
              data_ndim = static_cast<size_t>(std::atoi(doc["data_ndim"].GetString()));
              data_size = static_cast<size_t>(std::atoi(doc["data_size"].GetString()));

              std::string data_shape_str = doc["data_shape"].GetString();
              data_shape_str = data_shape_str.substr(1, data_shape_str.size()-2);
              std::vector<std::string> shape_parts = split_string(data_shape_str, ",");
              data_shape.resize(data_ndim);
              for (size_t i=0; i < data_ndim; i++) {
                data_shape[i] = static_cast<size_t>(std::stoi(shape_parts[i]));
              }
              break;
            }
          }
        }
      }
      m_logger->trace("Selected data has {} dims and total size = {}",
                      data_ndim,
                      data_size);
      std::string shape_msg{"("};
      for (size_t i=0; i<data_shape.size(); ++i) {
        shape_msg += std::to_string(data_shape[i]);
        if (i < data_shape.size() - 1) {
          shape_msg += ", ";
        }
      }
      shape_msg += ")";
      m_logger->trace("Data shape is: {}", shape_msg);
      // Now get the data using data_doc_id
      std::string data_endpoint = "/calib_ws/" + db_in_use + "/gridfs/" + data_doc_id;
      if (auto res = cli.Get(data_endpoint)) {
        if (data_type == "ndarray") {
          auto* raw_data = reinterpret_cast<const unsigned char*>(res->body.data());
          constants.resize(data_size);
          if (data_dtype == "float32") {
            m_logger->debug("Getting float32 ndarray");
            std::memcpy(constants.data(), raw_data, data_size*sizeof(std::float32_t));
          } else if (data_dtype == "float64") {
            m_logger->debug("Getting float64 ndarray");
            std::vector<std::float64_t> temp(data_size);
            std::memcpy(temp.data(), raw_data, data_size*sizeof(std::float64_t));
            std::transform(temp.begin(),temp.end(), constants.begin(),
                           [](std::float64_t val) {
                             return static_cast<std::float32_t>(val);
                           });
          }
        }
      }
      return std::make_pair(constants, data_shape);
    }

    void Detector::load_dummy_calib() {
      if (m_detname == "jungfrau") {
        std::ifstream peds_in("peds.npy", std::ios::binary);
        std::ifstream gain_in("gain.npy", std::ios::binary);
        std::ifstream offs_in("offset.npy", std::ios::binary);

        std::vector<std::float32_t> peds(3*32*512*1024);
        std::vector<std::float32_t> gain(3*32*512*1024);
        std::vector<std::float32_t> offs(3*32*512*1024);

        peds_in.read(reinterpret_cast<char*>(peds.data()),peds.size()*sizeof(std::float32_t));
        gain_in.read(reinterpret_cast<char*>(gain.data()),gain.size()*sizeof(std::float32_t));
        offs_in.read(reinterpret_cast<char*>(offs.data()),offs.size()*sizeof(std::float32_t));

        m_calibconst.resize(3*32*512*1024);
        for (size_t gain_idx=0; gain_idx < 3; ++gain_idx) {
          for (size_t seg_idx=0; seg_idx < 32; ++seg_idx) {
            for (size_t row_idx=0; row_idx < 512; ++row_idx) {
              for (size_t col_idx=0; col_idx < 1024; ++col_idx) {
                size_t idx =
                  gain_idx*32*512*1024 +
                  seg_idx*512*1024 +
                  row_idx*1024 +
                  col_idx;
                auto ped_off = peds[idx] + offs[idx];
                m_calibconst[idx] = CalibStruct(ped_off, gain[idx]);
              }
            }
          }
        }
      }
    }

    XtcData::Dgram* Detector::operator()(size_t offset_idx) {
      auto& reader = m_xtc_readers[0];
      auto ret = reader->read_l1_at(offset_idx);
      if (ret.has_value()) {
        return reader->get_current_dgram();
      } else {
        /// Handle errors?
        return nullptr;
      }
    }

    void* Detector::get_l1_data(size_t offset_idx,
                                const std::string& alg,
                                const std::string& data_name) {
      return (this->*get_l1_data_impl)(offset_idx, alg, data_name);
    }

    void* Detector::get_slow_update_data(size_t offset_idx) {
      if (!m_is_epics) {
        m_logger->warn("This function is for EPICS detectors! Use get_l1_data instead.");
        return nullptr;
      }
      for (auto& reader : m_xtc_readers) {
        std::expected<void, BDReadError> ret;
        ret = reader->read_slowupdate_at(offset_idx);
        if (ret.has_value()) {
          // Data is stored under "epics" detector. The algorithm
          // is always "raw" and the field name is the PV name - our m_detname
          std::vector<unsigned> reader_seg_nos =
            reader->segment_numbers()["epics"];
          unsigned seg_no = reader_seg_nos[0]; // There should only be 1
          std::string epics_detname{"epics"};
          std::string epics_alg{"raw"};
          std::string pv_name{m_detname};
          auto [data_ptr, data_size] =
            reader->get_data(epics_detname, seg_no, epics_alg, pv_name);

          m_data_ptrs[seg_no] = data_ptr;
          m_data_sizes[seg_no] = data_size;
        } else {
          // Handle errors?
          return nullptr;
        }
      }
      return m_data_ptrs[0];
    }

    void* Detector::get_l1_data_threaded(size_t offset_idx,
                                      const std::string& alg,
                                      const std::string& data_name) {
      /*
        m_logger->trace("Getting data for algorithm {} and field {} at offset idx {}",
        alg,
        data_name,
        offset_idx);
      */

      auto read_func = [&](std::shared_ptr<Base::BDReader> reader) -> void {
        auto ret = reader->read_l1_at(offset_idx);
        if (ret.has_value()) {
          std::vector<unsigned> reader_seg_nos =
              reader->segment_numbers()[m_detname];
          auto seg_no_it = reader_seg_nos.begin();
          while (seg_no_it != reader_seg_nos.end()) {
            auto [data_ptr, data_size] = reader->get_data(m_detname,
                                                          *seg_no_it,
                                                          alg,
                                                          data_name);
            m_data_ptrs[*seg_no_it] = data_ptr;
            m_data_sizes[*seg_no_it] = data_size;
            seg_no_it++;
          }
        }
      };

      std::vector<std::shared_future<void>> read_futs;
      for (auto& reader : m_xtc_readers) {
        read_futs.push_back((*m_thread_pool).enqueue(read_func, reader));
      }
      // Just wait on all the futures - we don't really care if we get stuck
      // on an early one while a later finished first. We have to wait for them
      // all
      for (auto it = read_futs.begin(); it != read_futs.end(); it++) {
        it->wait();
      }
      return m_data_ptrs.data();
    }

    void* Detector::get_l1_data_sequential(size_t offset_idx,
                                           const std::string& alg,
                                           const std::string& data_name) {
      /*
        m_logger->trace("Getting data for algorithm {} and field {} at offset idx {}",
                        alg,
                        data_name,
                        offset_idx);
      */
      // Launch all read asynchronously
      for (auto& reader : m_xtc_readers) {
        std::expected<void, BDReadError> ret = ret = reader->iread_l1_at(offset_idx);
        if (ret.has_value()) {
          continue;
        } else {
          // Handle errors?
          return nullptr;
        }
      }

      // Now wait on all of them
      for (auto& reader : m_xtc_readers) {
        auto ret = reader->wait();
        if (ret.has_value()) {
          //m_logger->trace("** Have a non-null dgram return. Now accessing the data field.");
          std::vector<unsigned> reader_seg_nos = reader->segment_numbers()[m_detname];
          auto seg_no_it = reader_seg_nos.begin();
          while (seg_no_it != reader_seg_nos.end()) {
            auto [data_ptr, data_size] = reader->get_data(m_detname,
                                                          *seg_no_it,
                                                          alg,
                                                          data_name);

            m_data_ptrs[*seg_no_it] = data_ptr;
            m_data_sizes[*seg_no_it] = data_size;

            //m_logger->trace("*** Filled in data for segment # {}", *seg_no_it);
            seg_no_it++;
          }
        } else {
          // Handle specific errors?
          return nullptr;
        }
      }
      return m_data_ptrs.data();
    }
  } // namespace Base
} // namespace XTCPP
