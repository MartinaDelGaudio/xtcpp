#ifndef XTCPP_BASE_HDF5WRITER_HH
#define XTCPP_BASE_HDF5WRITER_HH

#include "H5Cpp.h"
//#include "spdlog/sinks/stdout_color_sinks.h"

#include <any>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace XTCPP {
  namespace Base {
    class HDF5Writer {
    public:
      HDF5Writer(size_t batch_size);
      HDF5Writer();
      virtual ~HDF5Writer();

      virtual void open_file();
      void event(std::map<std::string,std::any> event_data,
                 std::map<std::string,std::vector<size_t>> shape);
      void save_summary(std::map<std::string,std::any> summary_data,
                        std::map<std::string,std::vector<size_t>> shape);

      size_t current_batch_size() const { return m_curr_size; }

    protected:
      H5::H5File* m_h5;
      std::map<std::string, H5::Group*> m_h5_groups;
      std::map<std::string, H5::DataSet*> m_h5_dsets;

      //size_t m_batch_size{1000}; ///< Number of events to store before flushing data
      size_t m_batch_size; ///< Number of events to store before flushing data
      size_t m_curr_size{0}; ///< Counter for number of events currently stored
      std::map<std::string,std::any> m_data_batch; ///< Data stored until batch size reached
      /// Holds rank and shape information for a dataset
      std::map<std::string,std::vector<size_t>> m_data_shape;

      std::vector<std::string> split_string(const std::string& s, const std::string& delim);

      void update_data_batch(const std::string& dset_name,
                             const std::any data,
                             const bool begin_batch);

      template <class T>
      void handle_vector_batch(const std::string& dset_name,
                               const std::any dset_data,
                               const bool begin_batch=false) {
        if (begin_batch) {
          m_data_batch[dset_name] = std::vector<T>();
        }
        auto& dset_name_batch =
          std::any_cast<std::vector<T>&>(m_data_batch[dset_name]);
        try {
          auto dset = std::any_cast<std::vector<T>>(dset_data);
          dset_name_batch.insert(dset_name_batch.end(), dset.begin(), dset.end());
        } catch (const std::bad_any_cast& e) {
          // The function should've been called after the type check happened so
          // would hope this case isn't executed
          std::cerr << "Got an unexpected data type when appending to SMD batch: "
                    << e.what() << std::endl
                    << "Mangled type name is: " << dset_data.type().name() << std::endl;
        }
      }

      void write_batch();
      void write_data_to_file(const std::string& dset_name, const std::any& dset_data);

      //std::shared_ptr<spdlog::logger> m_logger;
    };
  } // namespace Base
} // namespace XTCPP

#endif // XTCPP_BASE_HDF5WRITER_HH
