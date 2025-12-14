#include "common/bd_reader.hh"

#include "common/smd_reader.hh"

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
      m_logger->debug("Looking up data for " + detname + " segment " +
                      std::to_string(seg_no) + ": " + alg + "." + data_name +
                      ". The offset will be cached for faster lookup next time.");
      SegAlgData seg_alg_data = std::make_tuple(seg_no, alg, data_name);

      BDXtcOffset offset_placeholder(0,0);

      std::map<SegAlgData, BDXtcOffset> placeholder;
      m_offsets_in_dg.try_emplace(detname, placeholder);
      auto& det_offset_map = m_offsets_in_dg[detname];
      det_offset_map.try_emplace(seg_alg_data, offset_placeholder);
      auto& seg_alg_data_offset = det_offset_map[seg_alg_data];

      auto& alg_map = const_cast<AlgDataNameIndex&>(m_smd_reader->alg_map());
      // Starting_ptr will be used for calculating an address offset
      char* starting_ptr = reinterpret_cast<char*>(m_payload_ptr);
      // Payload_ptr will be updated to iterate through the data w/o updating
      // the stored m_payload_ptr which will always point to the start of the
      // entire datagram payload.
      XtcData::Xtc* payload_ptr = m_payload_ptr;
      size_t remaining_payload = m_remaining_payload;
      while (remaining_payload > 0) {
        if (payload_ptr->contains.id() != XtcData::TypeId::ShapesData) {
          std::string type_name{""};
          switch (payload_ptr->contains.id()) {
          case (XtcData::TypeId::Parent): {
            type_name = "Parent";
            // If Parent must go into the XTC
            payload_ptr = reinterpret_cast<XtcData::Xtc*>(payload_ptr->payload());
            remaining_payload = payload_ptr->sizeofPayload();
            break;
          }
          case (XtcData::TypeId::Names): {
            type_name = "Names";
            // If Names must continue onwards
            remaining_payload -= payload_ptr->sizeofPayload() + sizeof(XtcData::Xtc);
            payload_ptr = payload_ptr->next();
            break;
          }
          default: {
            type_name = "Uncased type: " + std::to_string(payload_ptr->contains.id());
            // Don't know what to do here...
            remaining_payload = 0;
            break;
          }
          }
          m_logger->trace("Skipping type in payload: " + type_name);
          continue;
        }
        XtcData::ShapesData& shapesdata = *reinterpret_cast<XtcData::ShapesData*>(payload_ptr);
        size_t single_shapes_offset{0};
        size_t shape_index{0};
        try {
          XtcData::DescData descdata(shapesdata, alg_map[detname][alg][seg_no]);
          XtcData::Names& names = descdata.nameindex().names();
          if (names.segment() != seg_no) {
            size_t shapes_data_size {payload_ptr->sizeofPayload() + sizeof(XtcData::Xtc)};
            remaining_payload -= shapes_data_size;
            payload_ptr = payload_ptr->next();
            continue;
          }
          for (size_t i = 0; i < names.num(); i++) {
            XtcData::Name& name = names.get(i);
            if (name.name() != data_name) {
              m_logger->trace("Skipping " + std::string(name.name()) +
                              " which has rank " + std::to_string(name.rank()) +
                              " and type " + std::to_string(name.type()));
              if (name.rank() == 0) {
                single_shapes_offset += XtcData::Name::get_element_size(name.type());
              } else {
                single_shapes_offset += shapesdata.shapes().get(shape_index).size(name);
                shape_index++;
              }
              continue;
            }

            XtcData::Data& selected_data = shapesdata.data();
            char* data_addr = selected_data.payload() + single_shapes_offset;
            void* data_ptr = reinterpret_cast<void*>(data_addr);

            size_t offset = data_addr - starting_ptr;
            size_t data_size{0};
            if (name.rank() == 0) {
              data_size = XtcData::Name::get_element_size(name.type());
            } else {
              data_size = shapesdata.shapes().get(shape_index).size(name);
            }

            // Unfortunately, for EPICS this lookup table is somewhat unintuitive
            // but it still works. The PV name is in seg_alg_data, with detname
            // being "epics"
            seg_alg_data_offset.offset = offset;
            seg_alg_data_offset.size = data_size;
            return std::make_pair(data_ptr, data_size);
          }
          remaining_payload -= payload_ptr->sizeofPayload() + sizeof(XtcData::Xtc);
          payload_ptr = payload_ptr->next();
        } catch (...) {
          //
          remaining_payload -= payload_ptr->sizeofPayload() + sizeof(XtcData::Xtc);
          payload_ptr = payload_ptr->next();
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
