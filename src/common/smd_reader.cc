#include "smd_reader.hh"

#include "xtcdata/xtc/DescData.hh"
#include "xtcdata/xtc/NamesLookup.hh"
#include "xtcdata/xtc/ShapesData.hh"

#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"

#include <algorithm>
#include <iostream>
#include <map>
#include <memory>
#include <string>

using namespace XtcData;

namespace XTCPP {

  namespace Base {
    SMDReader::SMDReader(std::string& smd_path,
                         size_t max_dgram_size,
                         size_t events_per_read)
      : m_smd_path(smd_path)
      , m_events_per_read(events_per_read)
      , m_max_dgram_size(max_dgram_size)
    {
      if (auto tmp = spdlog::get("Base::SMDReader")) {
	m_logger = tmp;
      } else {
	m_logger = spdlog::stdout_color_mt("Base::SMDReader");
      }
    }

    void SMDReader::process_data(XtcData::Xtc* xtc,
                                 std::shared_ptr<XtcOffset[]> external_buf) {


      auto char_ptr = reinterpret_cast<char*>(xtc);
      auto offset_ptr = reinterpret_cast<uint64_t*>(char_ptr + m_offset_in_l1accept_payload);
      auto offset = *offset_ptr;
      auto size = *(offset_ptr + 1);

      // We only care about the side effect of constructing in memory here
      m_curr_offset_idx = m_curr_offset_idx % m_events_per_read;
      new (external_buf.get() + m_curr_offset_idx) XtcOffset(offset, size);
      m_curr_offset_idx++;
    }

    void SMDReader::process_data(XtcData::Xtc* xtc,
                                 XtcData::TransitionId::Value transition_id) {
      int remaining = xtc->sizeofPayload();
      XtcData::Xtc* subxtc = reinterpret_cast<XtcData::Xtc*>(xtc->payload());
      while (remaining > 0) {
        process_data_internal(subxtc, transition_id);
        remaining -= subxtc->sizeofPayload() + sizeof(XtcData::Xtc);
        subxtc = subxtc->next();
      }
    }

    void SMDReader::process_data_internal(XtcData::Xtc* xtc,
                                          XtcData::TransitionId::Value transition_id) {
      switch (xtc->contains.id()) {
      case (XtcData::TypeId::Parent): {
        process_data(xtc, transition_id);
        break;
      }
      case (XtcData::TypeId::Names): {
        XtcData::Names& names = *reinterpret_cast<Names*>(xtc);
        std::string detname = names.detName();
	std::string dettype = names.detType();
        if (transition_id == XtcData::TransitionId::Configure && // Shouldn't be needed
            detname != "runinfo" &&
            detname != "chunkinfo" &&
            detname != "epicsinfo" &&
            detname != "triginfo") {
          unsigned seg_no = names.segment();
          std::string ser_no = names.detId();
          if (std::find(m_detnames.begin(), m_detnames.end(), detname) == m_detnames.end()) {
            m_detnames.push_back(detname);
	    m_det_types[detname] = dettype;
          }
          if (m_segment_nos.find(detname) != m_segment_nos.end()) {
            auto& det_segs = m_segment_nos[detname];
            if (std::find(det_segs.begin(), det_segs.end(), seg_no) == det_segs.end()) {
              det_segs.push_back(seg_no);
            }
          } else {
            m_segment_nos[detname] = {seg_no};
          }
          if (m_serial_nos.find(detname) != m_serial_nos.end()) {
            auto& det_ser_nos = m_serial_nos[detname];
            if (std::find(det_ser_nos.begin(), det_ser_nos.end(), ser_no) == det_ser_nos.end()) {
              det_ser_nos.push_back(ser_no);
            }
          } else {
            m_serial_nos[detname] = {ser_no};
          }
          XtcData::Alg& alg = names.alg();
          std::map<std::string,std::map<unsigned,XtcData::NameIndex>> det_alg_map_tmp;
          m_alg_map.try_emplace(detname, det_alg_map_tmp);
          auto& det_alg_map = m_alg_map[detname];
          std::map<unsigned,XtcData::NameIndex> det_seg_map_tmp;
          det_alg_map.try_emplace(alg.name(), det_seg_map_tmp);
          auto& det_seg_map = det_alg_map[alg.name()];
          det_seg_map.try_emplace(seg_no,NameIndex(names));
        }
        XtcData::NamesId& names_id = names.namesId();
        m_names_lookup[names_id] = NameIndex(names);
        break;
      }
      case (XtcData::TypeId::ShapesData): {
        if (transition_id == XtcData::TransitionId::L1Accept) {
          XtcData::ShapesData& shapesdata = *reinterpret_cast<XtcData::ShapesData*>(xtc);
          XtcData::NamesId namesid = shapesdata.namesId();
          XtcData::DescData descdata(shapesdata, m_names_lookup[namesid]);

          m_curr_offset_idx = m_curr_offset_idx % m_events_per_read;
          auto pos_it = m_offsets.begin() + m_curr_offset_idx;
          m_offsets.emplace(pos_it,
                            descdata.get_value<uint64_t>(static_cast<int>(0)),
                            descdata.get_value<uint64_t>(static_cast<int>(1)));
          m_curr_offset_idx++;
        }
        break;
      }
      default: {
        break;
      }
      }
    }

    XtcData::Dgram* SMDReader::next(std::shared_ptr<XtcOffset[]> external_buf) {
      Dgram& dg = *reinterpret_cast<XtcData::Dgram*>(m_access_ptr + m_access_offset);
      size_t payload_size = dg.xtc.sizeofPayload();
      if (payload_size > static_cast<size_t>(m_file_size)) {
        return nullptr;
      }

      if (dg.service() == XtcData::TransitionId::L1Accept) {
        process_data(&dg.xtc, external_buf);
      }
      m_access_offset += sizeof(dg) + payload_size;
      return &dg;
    }
  } // namespace Base
} // namespace XTCPP
