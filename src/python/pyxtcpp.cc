#include "common/smd_reader.hh"
#include "common/bd_reader.hh"
#include "mpi/datasource.hh"
#include "mpi/detector.hh"
#include "mpi/bd_reader.hh"
#include "hdf5/mpiwriter.hh"
#include "mpi/smd_reader.hh"

#include "mpi.h"

//#include <pybind11/chrono.h>
//#include <pybind11/functional.h>
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <any>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdfloat>
#include <string>
#include <variant>
#include <vector>

namespace py = pybind11;

PYBIND11_MAKE_OPAQUE(XTCPP::MPI::DataSource)
PYBIND11_MAKE_OPAQUE(XTCPP::MPI::Detector)
PYBIND11_MAKE_OPAQUE(XTCPP::MPI::BDReader)

PYBIND11_MAKE_OPAQUE(XTCPP::Base::DataSource)
PYBIND11_MAKE_OPAQUE(XTCPP::Base::Detector)
PYBIND11_MAKE_OPAQUE(XTCPP::Base::BDReader)

/**
 * Note: Must be careful in the class definitions for selecting the "holder"
 * type. This is by default a unique_ptr (for pybind11 backward compatibility).
 * This will cause issues for the implementation of the C++ code in terms of
 * ownership and lifetime. Instead use py::smart_holder or shared_ptr as used
 * for the relevant classes below.
 * ( py::class_<CPP::Class, std::shared_ptr<CPP::Class>>(m, "PyClass")... )
 */

PYBIND11_MODULE(_xtcpp, m, py::mod_gil_not_used()) {
  m.doc() = "XTCPP Python bindings.";

  py::class_<XTCPP::BDXtcOffset>(m, "BDXtcOffset")
    .def_readwrite("offset", &XTCPP::BDXtcOffset::offset)
    .def_readwrite("size", &XTCPP::BDXtcOffset::size);

  py::class_<XTCPP::MPI::DataSource>(m, "DataSource")
    .def(py::init<std::string,
                  std::variant<std::string, int>,
	                size_t>())
    .def("rank", &XTCPP::MPI::DataSource::rank)
    .def("detector",
         &XTCPP::MPI::DataSource::detector,
         py::keep_alive<0,1>(),
         py::return_value_policy::reference)
    .def("__iter__",
         [](XTCPP::MPI::DataSource& ds) {
           return py::make_iterator(ds.begin(), ds.end());
         },
         py::keep_alive<0, 1>());

  py::class_<XTCPP::Base::Detector,
             std::shared_ptr<XTCPP::Base::Detector>>(m, "Detector")
    .def(py::init([](std::string detname,
                     std::string serial_no,
                     std::vector<unsigned> segment_nos,
                     std::vector<std::shared_ptr<XTCPP::Base::BDReader>> xtc_readers,
                     std::string experiment,
                     std::string run,
                     bool is_epics) {
      return new XTCPP::Base::Detector(detname,
                                       serial_no,
                                       segment_nos,
                                       xtc_readers,
                                       experiment,
                                       run,
                                       is_epics);
    }))
    .def("raw", [](XTCPP::Base::Detector& self, size_t evt) {
      return self.get_data(evt, "raw", "raw");
		})
    .def("calib", [](XTCPP::Base::Detector& self, size_t evt) {
      [[maybe_unused]]auto raw_data = self.get_data(evt, "raw", "raw");
		    std::vector<std::float32_t> calib_data =
		      XTCPP::calibrate(self.data_ptrs(), self.calibconst_span());
		    float* float_data = reinterpret_cast<float*>(calib_data.data());
		    size_t nsegs {32};
		    size_t nrows {512};
		    size_t ncols {1024};
		    std::vector<size_t> shape {nsegs, nrows, ncols};
		    return py::array_t<float>(shape, float_data);
		  });

  py::class_<XTCPP::MPI::Detector,
             std::shared_ptr<XTCPP::MPI::Detector>,
             XTCPP::Base::Detector>(m, "MPIDetector")
    .def(py::init([](int comm_f,
                     std::string detname,
                     std::string serial_no,
                     std::vector<unsigned> segment_nos,
                     std::vector<std::shared_ptr<XTCPP::Base::BDReader>> xtc_readers,
                     std::string experiment,
                     std::string run,
                     bool is_epics) {
      MPI_Comm comm = MPI_Comm_f2c(comm_f);
      ///*
      return new XTCPP::MPI::Detector(comm,
                                      detname,
                                      serial_no,
                                      segment_nos,
                                      xtc_readers,
                                      experiment,
                                      run,
                                      is_epics);
    }))
    .def("raw", [](XTCPP::MPI::Detector& self, size_t evt) {
      if (self.is_epics()) {
		    return self.get_data(evt, "raw", self.detname());
      } else {
        return self.get_data(evt, "raw", "raw");
      }
    })
    .def("calib", [](XTCPP::MPI::Detector& self, size_t evt) {
      [[maybe_unused]]auto raw_data = self.get_data(evt, "raw", "raw");
        //std::vector<std::float32_t> calib_data =
        //  XTCPP::calibrate(self.data_ptrs(), self.calibconst_span());
        //float* float_data = reinterpret_cast<float*>(calib_data.data());
		    // This version will fill in a passed buffer. We fill in the buffer
		    // which is pre-allocated. Then once it is filled with new data
		    // we will return it.
		    XTCPP::calibrate(self.data_ptrs(),
                         self.calibconst_span(),
                         self.calib_data_buf());
		    float* float_data = reinterpret_cast<float*>(self.calib_data_buf().data());
		    size_t nsegs {32};
		    size_t nrows {512};
		    size_t ncols {1024};
		    std::vector<size_t> shape {nsegs, nrows, ncols};
		    return py::array_t<float>(shape, float_data);
    });

  py::class_<XTCPP::MPI::HDF5Writer>(m, "SmallData")
    .def(py::init([](size_t batch_size) {
      return new XTCPP::MPI::HDF5Writer(MPI_COMM_WORLD, batch_size);
    }))
    .def("event", [](XTCPP::MPI::HDF5Writer& self,
                     py::dict event_data,
                     py::dict event_shape) {
      std::map<std::string, std::any> evt_data;
      std::map<std::string, std::vector<size_t>> evt_shape;
      for (auto& item : evt_data) {
        std::string dset_name = py::str(item.first);
        py::object val = std::any_cast<py::object>(item.second);
        if (py::isinstance<py::array>(val)) {
          py::array arr = val.cast<py::array>();
          py::buffer_info buf_info = arr.request();
          std::vector<std::float32_t> vec(buf_info.size);
          std::memcpy(vec.data(), buf_info.ptr, buf_info.size*sizeof(std::float32_t));
          evt_data[dset_name] = std::move(vec);
        }
      }
      for (auto& item : evt_shape) {
        std::string dset_name = py::str(item.first);
        evt_shape[dset_name] = std::move(item.second);
      }
      self.event(evt_data, evt_shape);
    })
    .def("save_summary", &XTCPP::MPI::HDF5Writer::save_summary)
    .def("rank", &XTCPP::MPI::HDF5Writer::rank)
    .def("mpi_size", &XTCPP::MPI::HDF5Writer::size)
    .def("current_batch_size", &XTCPP::MPI::HDF5Writer::current_batch_size);

  py::class_<XTCPP::MPI::BDReader>(m, "MPIBDReader")
    //std::shared_ptr<XTCPP::Base::BDReader>>(m, "MPIBDReader")
    .def(py::init([](int comm_f,
		     std::string& smd_path,
		     std::string& xtc_path,
		     size_t events_per_read) {
      MPI_Comm comm = MPI_Comm_f2c(comm_f);
      return new XTCPP::MPI::BDReader(comm,
                                      smd_path,
                                      xtc_path,
                                      events_per_read);
    }))
    .def("get_next_offsets", &XTCPP::MPI::BDReader::get_next_offsets)
    .def("read_l1_at", &XTCPP::MPI::BDReader::read_l1_at)
    .def("get_data", &XTCPP::MPI::BDReader::get_data)
    .def("detnames", &XTCPP::MPI::BDReader::detnames)
    .def("segments", &XTCPP::MPI::BDReader::segment_numbers)
    .def("serial_nos", &XTCPP::MPI::BDReader::serial_numbers)
    //.def("offsets", &XTCPP::Base::BDReader::offsets)
    .def("det_types", &XTCPP::MPI::BDReader::det_types);

  //m.def("calibrate", &XTCPP::calibrate, "Calibrate raw data.");
}
