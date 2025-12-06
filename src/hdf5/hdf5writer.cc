#include "hdf5writer.hh"

#include "H5Cpp.h"

#include <any>
#include <map>
#include <sstream>
#include <stdfloat>
#include <string>
#include <vector>

namespace XTCPP {
  namespace Base {
    HDF5Writer::HDF5Writer(size_t batch_size)
      : m_batch_size(batch_size)
    {}

    HDF5Writer::HDF5Writer()
      : HDF5Writer(2)
    {}

    HDF5Writer::~HDF5Writer() {
      // Write any remaining data in a batch
      write_batch();
      for (auto [dset_name, dset] : m_h5_dsets) {
        delete dset;
      }
      for (auto [group_name, group] : m_h5_groups) {
        delete group;
      }
      delete m_h5;
    }

    void HDF5Writer::open_file() {
      m_h5 = new H5::H5File("test.h5", H5F_ACC_TRUNC);
    }

    void HDF5Writer::event(std::map<std::string,std::any> event_data,
        std::map<std::string,std::vector<size_t>> shape) {
      if (event_data.empty()) {
        // Didn't get any data - just return, don't increment anything
        return;
      }
      if (m_data_batch.empty()) {
        // We don't have any data currently stored in the batch, so just set this as
        // the data map
        for (const auto& [dset_name, dset_data] : event_data) {
          update_data_batch(dset_name, dset_data, true);
          m_data_shape[dset_name] = shape[dset_name];
        }
      } else {
        // Need to update the map
        for (const auto& [dset_name, dset_data] : event_data) {
          if (!dset_data.has_value()) {
            // Data was empty for this dset
            continue;
          }
          if (m_data_batch.find(dset_name) == m_data_batch.end()) {
            // We may have data in the batch but not for this dataset
            update_data_batch(dset_name, dset_data, true);
          } else {
            // Need to append the new event data to the other batch data for det_name
            update_data_batch(dset_name, dset_data, false);
          }
          if (m_data_shape.find(dset_name) == m_data_shape.end()) {
            m_data_shape[dset_name] = shape[dset_name];
          }
        }
      }
      m_curr_size++;
      if (m_curr_size == m_batch_size) {
        write_batch();
        m_data_batch.clear();
        m_curr_size = 0;
      }
    }

    std::vector<std::string> HDF5Writer::split_string(const std::string& s,
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

    void HDF5Writer::save_summary(std::map<std::string, std::any> summary_data,
               std::map<std::string, std::vector<size_t>> shape) {
      for (const auto& [dset_name, dset_data] : summary_data) {
        m_data_shape[dset_name] = shape[dset_name];
        write_data_to_file(dset_name, dset_data);
      }
    }

    void HDF5Writer::write_batch() {
      for (const auto& [dset_name, dset_data] : m_data_batch) {
        write_data_to_file(dset_name, dset_data);
      }
    }

    void HDF5Writer::write_data_to_file(const std::string& dset_name,
               const std::any& dset_data) {
      auto group_parts = split_string(dset_name, "/");
      std::string group_name{""};
      // Last part is the dataset itself
      for (size_t idx=0; idx<group_parts.size()-1; ++idx) {
        group_name += "/" + group_parts[idx];
        if (m_h5_groups.find(group_name) == m_h5_groups.end()) {
          m_h5_groups[group_name] = new H5::Group(m_h5->createGroup(group_name));
        }
      }

      auto& stored_shape = m_data_shape[dset_name];
      size_t stored_rank = stored_shape.size();
      const hsize_t RANK{stored_rank+1}; // First index will be event access
      if (m_h5_dsets.find(dset_name) == m_h5_dsets.end()) {
	// Use vector for everything since ISO C++ forbids VLA
        // Dimensions at creation time
	std::vector<hsize_t> dims(RANK);
        // Maximum dimensions, first axis is extendable
	std::vector<hsize_t> max_dims(RANK);
        // Setup chunking
	std::vector<hsize_t> chunk_dims(RANK);
        for (size_t idx = 0; idx < RANK; ++idx) {
          if (idx == 0) {
            dims[idx] = 0;
            max_dims[idx] = H5S_UNLIMITED;
            chunk_dims[idx] = m_batch_size;
          } else {
            dims[idx] = stored_shape[idx - 1];
            max_dims[idx] = dims[idx];
            chunk_dims[idx] = dims[idx];
          }
        }
        H5::DataSpace dspace(RANK, dims.data(), max_dims.data());
        H5::DSetCreatPropList p_list;
        p_list.setChunk(RANK, chunk_dims.data());
        float fill_val{0.0};
        p_list.setFillValue(H5::PredType::NATIVE_FLOAT, &fill_val);
        m_h5_dsets[dset_name] = new H5::DataSet(m_h5->createDataSet(dset_name,
                                                                    H5::PredType::NATIVE_FLOAT,
                                                                    dspace,
                                                                    p_list));
      }
      auto dset = m_h5_dsets[dset_name];
      auto dspace = dset->getSpace();
      std::vector<hsize_t> old_dims(RANK);
      dspace.getSimpleExtentDims(old_dims.data());

      // Use m_curr_size not m_batch_size since at the end of a run
      // you may flush less than a full batch to disk
      std::vector<hsize_t> new_dims(RANK);
      std::vector<hsize_t> offset(RANK);
      std::vector<hsize_t> count(RANK);
      std::vector<hsize_t> stride(RANK); // We assume all contiguous for now
      std::vector<hsize_t> block(RANK);  // Nothing fancy...
      for (size_t idx = 0; idx < RANK; ++idx) {
        if (idx == 0) {
          new_dims[idx] = old_dims[idx] + m_curr_size;
          offset[idx] = old_dims[idx];
          count[idx] = m_curr_size;
        } else {
          new_dims[idx] = old_dims[idx];
          offset[idx] = 0;
          count[idx] = old_dims[idx];
        }
        stride[idx] = 1;
        block[idx] = 1;
      }
      dset->extend(new_dims.data());

      H5::DataSpace hslab_space = dset->getSpace();
      hslab_space.selectHyperslab(H5S_SELECT_SET,
				  count.data(),
				  offset.data(),
				  stride.data(),
				  block.data());

      H5::DataSpace memspace(RANK, count.data());
      if ([[maybe_unused]] auto vec_ptr = std::any_cast<std::vector<std::float32_t>>(&dset_data)) {
        dset->write(vec_ptr->data(),
		    H5::PredType::NATIVE_FLOAT,
		    memspace,
		    hslab_space);
      }
    }

    void HDF5Writer::update_data_batch(const std::string& dset_name,
                                      const std::any dset_data,
                                      const bool begin_batch) {
      // Will use pointers to avoid exception handling to determine types in
      // the std::any data
      if ([[maybe_unused]] auto vec_ptr = std::any_cast<std::vector<std::float32_t>>(&dset_data)) {
        // Have a vector of float
        // This means the batch should be a vector of vectors of float
        // Last argument is whether we are beginning a new batch or not
        handle_vector_batch<std::float32_t>(dset_name, dset_data, begin_batch);
      } else if ([[maybe_unused]] auto vec_ptr = std::any_cast<std::vector<std::uint16_t>>(&dset_data)) {
        // Have a vector of uint16_t
        // This means the batch should be a vector of vectors of uint16_t
        // Last argument is whether we are beginning a new batch or not
        handle_vector_batch<std::uint16_t>(dset_name, dset_data, begin_batch);
      } else if ([[maybe_unused]] auto map_ptr = std::any_cast<std::map<std::string, std::any>>(&dset_data)) {
        for (const auto& [sub_dset_name, sub_dset_data] : *map_ptr) {
          std::string qualified_dset_name = dset_name + "/" + sub_dset_name;
          update_data_batch(qualified_dset_name, sub_dset_data, begin_batch);
        }
      }
    }
  } // namespace Base
} // namespace XTCPP
