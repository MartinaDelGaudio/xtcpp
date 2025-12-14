#include "common/smd_reader.hh"

#include "xtcdata/xtc/DescData.hh"
#include "xtcdata/xtc/NamesLookup.hh"
#include "xtcdata/xtc/ShapesData.hh"

#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"

#include <algorithm>
#include <exception>
#include <expected>
#include <iostream>
#include <map>
#include <memory>
#include <string>

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

    void SMDReader::inspect_xtc(XtcData::Xtc* xtc,
                                XtcData::TransitionId::Value transition_id) {
      switch (xtc->contains.id()) {
      case (XtcData::TypeId::Parent): {
        recurse_dgram_xtcs(xtc, transition_id);
        break;
      }
      case (XtcData::TypeId::Names): {
        XtcData::Names& names = *reinterpret_cast<XtcData::Names*>(xtc);
        std::string detname = names.detName();
        std::string dettype = names.detType();
        if (transition_id == XtcData::TransitionId::Configure && // Shouldn't be needed
            detname != "runinfo" &&
            detname != "chunkinfo" &&
            detname != "epicsinfo" && // TODO: Revisit to support EPICS
            detname != "triginfo") {
          unsigned seg_no = names.segment();
          std::string ser_no = names.detId();
          if (detname == "epics") {
            for (size_t i=0; i<names.num(); ++i) {
              m_epics_detnames.emplace_back(names.get(i).name());
            }
          }
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
          det_seg_map.try_emplace(seg_no, XtcData::NameIndex(names));

          if (m_det_algs.find(detname) != m_det_algs.end()) {
            m_det_algs[detname].push_back(alg.name());
          } else {
            m_det_algs[detname] = {alg.name()};
          }

          std::vector<std::string> fields;
          for (size_t i = 0; i < names.num(); ++i) {
            fields.push_back(names.get(i).name());
          }
          if (m_det_alg_fields.find(detname) != m_det_alg_fields.end()) {
            auto& alg_fields = m_det_alg_fields[detname];
            alg_fields[alg.name()] = fields;
          } else {
            std::map<std::string,std::vector<std::string>> alg_fields;
            alg_fields[alg.name()] = fields;
            m_det_alg_fields[detname] = alg_fields;
          }
        }
        XtcData::NamesId& names_id = names.namesId();
        m_names_lookup[names_id] = XtcData::NameIndex(names);
        break;
      }
      case (XtcData::TypeId::ShapesData): {
        if (transition_id == XtcData::TransitionId::L1Accept) {
          XtcData::ShapesData& shapesdata = *reinterpret_cast<XtcData::ShapesData*>(xtc);
          XtcData::NamesId namesid = shapesdata.namesId();
          XtcData::DescData descdata(shapesdata, m_names_lookup[namesid]);

          m_curr_offset_idx = m_curr_offset_idx % m_events_per_read;
          m_curr_offset_idx++;
        }
        break;
      }
      default: {
        break;
      }
      }
    }
    void SMDReader::extract_offset_from_dgram_into(XtcData::Xtc* xtc,
                                                   std::shared_ptr<BDXtcOffset[]> external_buf) {


      auto char_ptr = reinterpret_cast<char*>(xtc);
      auto offset_ptr = reinterpret_cast<uint64_t*>(char_ptr + m_offset_in_l1accept_payload);
      auto offset = *offset_ptr;
      auto size = *(offset_ptr + 1);

      // We only care about the side effect of constructing in memory here
      m_curr_offset_idx = m_curr_offset_idx % m_events_per_read;
      new (external_buf.get() + m_curr_offset_idx) BDXtcOffset(offset, size);
      m_curr_offset_idx++;
    }

    void SMDReader::recurse_dgram_xtcs(XtcData::Xtc* xtc,
                                       XtcData::TransitionId::Value transition_id) {
      int remaining = xtc->sizeofPayload();
      XtcData::Xtc* subxtc = reinterpret_cast<XtcData::Xtc*>(xtc->payload());
      while (remaining > 0) {
        inspect_xtc(subxtc, transition_id);
        remaining -= subxtc->sizeofPayload() + sizeof(XtcData::Xtc);
        subxtc = subxtc->next();
      }
    }

    XtcData::Dgram* SMDReader::get_offset_into(std::shared_ptr<BDXtcOffset[]> external_buf,
                                               std::shared_ptr<TransitionXtcOffset[]> transition_buf) {
      XtcData::Dgram& dg = *reinterpret_cast<XtcData::Dgram*>(m_access_ptr + m_access_offset);
      size_t payload_size = dg.xtc.sizeofPayload();
      if (payload_size > static_cast<size_t>(m_file_size)) {
        return nullptr;
      }

      if (dg.service() == XtcData::TransitionId::L1Accept) {
        extract_offset_from_dgram_into(&dg.xtc, external_buf);
        m_last_l1_idx_seen++;
      //} else if (dg.service() == XtcData::TransitionId::SlowUpdate) {
      } else {
        m_curr_transition_index = m_curr_transition_index % m_events_per_read;
        uint64_t size = sizeof(dg) + payload_size;
        // Get the current offset. We've read m_read_count and updated m_file_offset
        // So to find the offset of the datagram revert m_file_offset by bytes read
        // and then add in the offset we are currently at.

        uint64_t offset;
        if (m_last_l1_idx_seen < 0) {
          // Have seen nothing but transitions... Then the smd file_offset can be used
          // This is because entire transitions are also stored in .smd.xtc2 files
          // If we have yet to see an L1Accept, then the offset in .smd.xtc2 is equal
          // to the offset in .xtc2
          /// TODO: The above actually doesn't seem to be true!!! Investigate why!
          /// For now, the BDReader must do some hackery if prev_l1 is -1. It will then
          /// Calculate based on the size (which IS accurate at least) and the first
          /// L1Accept offset what the correct SlowUpdate offset should be...
          offset = (m_file_offset - m_read_count) + m_access_offset;
        } else if (m_curr_offset_idx != 0) {
          // Have seen L1 (and not wrapped)... Can use previous L1 offset+size
          BDXtcOffset prev_l1 = external_buf[m_curr_offset_idx-1];
          offset = prev_l1.offset + prev_l1.size;
        } else {
          // We wrapped and the first item is a SlowUpdate...
          // TODO: Find a better approach...
          // Living dangerously for now... Assume they didn't clear the memory
          // and we will use the offset at the end of the buffer...
          BDXtcOffset prev_l1 = external_buf[m_events_per_read-1];
          offset = prev_l1.offset + prev_l1.size;
        }
        new (transition_buf.get() + m_curr_transition_index)
          TransitionXtcOffset(m_last_l1_idx_seen, offset, size, dg.service());
        m_curr_transition_index++;

        if (dg.service() == XtcData::TransitionId::EndRun) {
          m_seen_end_run = true;
        }
      }
      m_access_offset += sizeof(dg) + payload_size;
      return &dg;
    }

    std::expected<BDXtcOffset, SMDReadError> SMDReader::get_offset() {
      XtcData::Dgram& dg = *reinterpret_cast<XtcData::Dgram*>(m_access_ptr + m_access_offset);
      size_t payload_size = dg.xtc.sizeofPayload();
      if (payload_size > static_cast<size_t>(m_file_size)) {
        return std::unexpected(SMDReadError::PayloadTruncatedError);
      }

      m_access_offset += sizeof(dg) + payload_size;
      if (dg.service() == XtcData::TransitionId::L1Accept) {
        //process_data(&dg.xtc, external_buf);
        auto char_ptr = reinterpret_cast<char*>(&dg.xtc);
        auto offset_ptr =
          reinterpret_cast<uint64_t*>(char_ptr + m_offset_in_l1accept_payload);
        auto offset = *offset_ptr;
        auto size = *(offset_ptr + 1);
        // NOTE: We always keep track of the number of offsets read.
        // It is up to the caller to keep track which offsets are read into an external
        // buffer (using get_offset_into) and which are not (i.e. read using
        // this function). If use is mixed, the external buffer could have gaps.
        m_curr_offset_idx = m_curr_offset_idx % m_events_per_read;
        m_curr_offset_idx++;
        return BDXtcOffset(offset, size);
        //return std::make_optional(BDXtcOffset(offset,size));
      }
      return std::unexpected(SMDReadError::NoOffsetInData);
    }
  } // namespace Base
} // namespace XTCPP
