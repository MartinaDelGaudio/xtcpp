#include "bd_reader.hh"
#include "smd_reader.hh"

#include "xtcdata/xtc/DescData.hh"
#include "xtcdata/xtc/Dgram.hh"
#include "xtcdata/xtc/NamesLookup.hh"
#include "xtcdata/xtc/ShapesData.hh"

#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"

#include <any>
#include <iostream>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

namespace XTCPP {
  namespace Base {
    BDReader::BDReader(std::string& smd_path,
                       std::string& xtc_path,
                       size_t events_per_read)
      : m_events_per_read(events_per_read)
      , m_smd_path(smd_path)
      , m_xtc_path(xtc_path)
    {
      constexpr size_t max_dgram_size = 0x4000000;
      m_smd_reader = std::make_unique<SMDReader>(smd_path, max_dgram_size, events_per_read);
      if (auto tmp = spdlog::get("Base::BDReader")) {
        m_logger = tmp;
      } else {
        m_logger = spdlog::stdout_color_mt("Base::BDReader");
      }
    }

    BDReader::~BDReader() {}

    std::pair<void*, size_t> BDReader::get_data(const std::string& detname,
                                                 const unsigned& seg_no,
                                                 const std::string& alg,
                                                 const std::string& data_name) {
      if (m_offsets_in_dg.find(detname) != m_offsets_in_dg.end()) {
        SegAlgData seg_alg_data = std::make_tuple(seg_no, alg, data_name);
        if (m_offsets_in_dg[detname].find(seg_alg_data) != m_offsets_in_dg[detname].end()) {
          auto& offset = m_offsets_in_dg[detname][seg_alg_data];
          char* char_ptr = reinterpret_cast<char*>(m_payload_ptr);
          void* data_ptr = reinterpret_cast<void*>(char_ptr + offset.offset);
          size_t data_size = offset.size;

          return std::make_pair(data_ptr, data_size);
        }
      }
      return get_data_internal(detname, seg_no, alg, data_name);
    }

    void BDReader::close() {}

    std::pair<void*,size_t> BDReader::get_data_internal(const std::string& detname,
                                                        const unsigned& seg_no,
                                                        const std::string& alg,
                                                        const std::string& data_name) {
      SegAlgData seg_alg_data = std::make_tuple(seg_no, alg, data_name);

      BDXtcOffset offset_placeholder(0,0);

      std::map<SegAlgData, BDXtcOffset> placeholder;
      m_offsets_in_dg.try_emplace(detname, placeholder);
      auto& det_offset_map = m_offsets_in_dg[detname];
      det_offset_map.try_emplace(seg_alg_data, offset_placeholder);
      auto& seg_alg_data_offset = det_offset_map[seg_alg_data];

      auto& alg_map = const_cast<AlgDataNameIndex&>(m_smd_reader->alg_map());
      char* starting_ptr = reinterpret_cast<char*>(m_payload_ptr);
      while (m_remaining_payload > 0) {
        XtcData::ShapesData& shapesdata = *reinterpret_cast<XtcData::ShapesData*>(m_payload_ptr);
        try {
          XtcData::DescData descdata(shapesdata, alg_map[detname][alg][seg_no]);
          XtcData::Names& names = descdata.nameindex().names();
          if (names.segment() != seg_no) {
            m_remaining_payload -= m_payload_ptr->sizeofPayload() + sizeof(XtcData::Xtc);
            m_payload_ptr = m_payload_ptr->next();
            continue;
          }
          for (size_t i = 0; i < names.num(); i++) {
            XtcData::Name& name = names.get(i);
            if (name.name() != data_name) {
              continue;
            }
            size_t data_size = static_cast<size_t>(m_payload_ptr->sizeofPayload());
            //m_remaining_payload -= m_payload_ptr->sizeofPayload() + sizeof(XtcData::Xtc);
            //m_payload_ptr = m_payload_ptr->next();

            std::any val = get_value(i, name, descdata);
            if (auto data = std::any_cast<void*>(val)) {
              //void* data = get_value(i, name, descdata);

              size_t diff = reinterpret_cast<char*>(data) - starting_ptr;
              seg_alg_data_offset.offset = diff;
              seg_alg_data_offset.size = data_size;
              return std::make_pair(data, data_size);
            } else {
              void* data_ptr = reinterpret_cast<void*>(reinterpret_cast<char*>(starting_ptr) + 56);
              size_t diff = 56;
              seg_alg_data_offset.offset = diff;
              seg_alg_data_offset.size = data_size;
              return std::make_pair(data_ptr, data_size);
            }
          }
          m_remaining_payload -= m_payload_ptr->sizeofPayload() + sizeof(XtcData::Xtc);
          m_payload_ptr = m_payload_ptr->next();
        } catch (...) {
          //
          m_remaining_payload -= m_payload_ptr->sizeofPayload() + sizeof(XtcData::Xtc);
          m_payload_ptr = m_payload_ptr->next();
          continue;
        }
      }
      throw std::runtime_error("Unable to find requested data: " + alg + "." + data_name);
    }

    std::any BDReader::get_value(size_t idx, XtcData::Name& name, XtcData::DescData& descdata) {
      int data_rank = name.rank();

      switch (name.type()) {
      case (XtcData::Name::UINT8): {
        if (data_rank > 0) {
          return reinterpret_cast<void*>(descdata.get_array<uint8_t>(idx).data());
        } else {
          return descdata.get_value<uint8_t>(idx);
        }
      }
      case (XtcData::Name::UINT16): {
        if (data_rank > 0) {
          return reinterpret_cast<void*>(descdata.get_array<uint16_t>(idx).data());
        } else {
          return descdata.get_value<uint16_t>(idx);
        }
      }
      case (XtcData::Name::UINT32): {
        if (data_rank > 0) {
          return reinterpret_cast<void*>(descdata.get_array<uint32_t>(idx).data());
        } else {
          return descdata.get_value<uint32_t>(idx);
        }
      }
      case (XtcData::Name::UINT64): {
        if (data_rank > 0) {
          return reinterpret_cast<void*>(descdata.get_array<uint64_t>(idx).data());
        } else {
          return descdata.get_value<uint64_t>(idx);
        }
      }
      case (XtcData::Name::INT8): {
        if (data_rank > 0) {
          return reinterpret_cast<void*>(descdata.get_array<int8_t>(idx).data());
        } else {
          return descdata.get_value<int8_t>(idx);
        }
      }
      case (XtcData::Name::INT16): {
        if (data_rank > 0) {
          return reinterpret_cast<void*>(descdata.get_array<int16_t>(idx).data());
        } else {
          return descdata.get_value<int16_t>(idx);
        }
      }
      case (XtcData::Name::INT32): {
        if (data_rank > 0) {
          return reinterpret_cast<void*>(descdata.get_array<int32_t>(idx).data());
        } else {
          return descdata.get_value<int32_t>(idx);
        }
      }
      case (XtcData::Name::INT64): {
        if (data_rank > 0) {
          return reinterpret_cast<void*>(descdata.get_array<int64_t>(idx).data());
        } else {
          return descdata.get_value<int64_t>(idx);
        }
      }
      case (XtcData::Name::FLOAT): {
        if(data_rank > 0) {
          return reinterpret_cast<void*>(descdata.get_array<float>(idx).data());
      } else {
          return descdata.get_value<float>(idx);
        }
      }
      case (XtcData::Name::DOUBLE): {
        if (data_rank > 0) {
          return reinterpret_cast<void*>(descdata.get_array<double>(idx).data());
        } else {
          return descdata.get_value<double>(idx);
        }
      }
      case(XtcData::Name::CHARSTR):
        return reinterpret_cast<void*>(descdata.get_array<char>(idx).data());
      case(XtcData::Name::ENUMVAL): {
        if (data_rank > 0) {
          return reinterpret_cast<void*>(descdata.get_array<int32_t>(idx).data());
        } else {
          return descdata.get_value<int32_t>(idx);
        }
      }
      case (XtcData::Name::ENUMDICT):
        return descdata.get_value<int32_t>(idx);
      default:
        throw std::runtime_error("Could not figure out parsing for: " +
                                 std::string(name.name()) +
                                 " (type: " + std::to_string(name.type()) + ")");
      }
    }
  } // namespace Base
} // namespace XTCPP
