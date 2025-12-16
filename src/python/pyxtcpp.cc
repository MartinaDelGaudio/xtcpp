#include "common/smd_reader.hh"
#include "common/bd_reader.hh"
#include "mpi/datasource.hh"
#include "mpi/detector.hh"
#include "mpi/bd_reader.hh"
#include "hdf5/mpiwriter.hh"
#include "mpi/smd_reader.hh"

#include "xtcdata/xtc/ShapesData.hh"

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

namespace {
  /**
   * The XtcData::Name::DataType enum has the following enumerators:
   * { UINT8, UINT16, UINT32, UINT64, INT8, INT16, INT32, INT64, FLOAT, DOUBLE,
   *   CHARSTR, ENUMVAL, ENUMDICT}
   *
   * This function takes void* pointer which is returned from the get_*_data
   * C++ APIs, and a datatype enumerator to return a Python object of appropriate
   * type.
   *
   * @param[in] val Raw data pointer.
   * @param[in] dtype The enumerator describing the data type pointed to by val.
   * @return object The cast Python object.
   */
  py::object cast_to_pyobject(void* val, XtcData::Name::DataType dtype) {
    switch (dtype) {
    case XtcData::Name::UINT8:
      return py::cast(*reinterpret_cast<uint8_t*>(val));
    case XtcData::Name::UINT16:
      return py::cast(*reinterpret_cast<uint16_t*>(val));
    case XtcData::Name::UINT32:
      return py::cast(*reinterpret_cast<uint32_t*>(val));
    case XtcData::Name::UINT64:
      return py::cast(*reinterpret_cast<uint64_t*>(val));
    case XtcData::Name::INT8:
      return py::cast(*reinterpret_cast<int8_t*>(val));
    case XtcData::Name::INT16:
      return py::cast(*reinterpret_cast<int16_t*>(val));
    case XtcData::Name::INT32:
      return py::cast(*reinterpret_cast<int32_t*>(val));
    case XtcData::Name::INT64:
      return py::cast(*reinterpret_cast<int64_t*>(val));
    case XtcData::Name::FLOAT:
      return py::cast(*reinterpret_cast<float*>(val));
    case XtcData::Name::DOUBLE:
      return py::cast(*reinterpret_cast<double*>(val));
    case XtcData::Name::CHARSTR:
      return py::cast(*reinterpret_cast<char*>(val));
    default:
      return py::object(py::cast(nullptr));
    }
  }

  class AlgWrapper {
  public:
    AlgWrapper(std::shared_ptr<XTCPP::Base::Detector> det_,
               std::string name_,
               unsigned version_)
      : det(det_)
      , name(name_)
      , version(version_)
    {}
  public:
    std::shared_ptr<XTCPP::Base::Detector> det;
    std::string name;
    unsigned version;
  };

  class DetectorWrapper {
  public:
    DetectorWrapper(std::shared_ptr<XTCPP::Base::Detector> det)
      : _det(det)
    {}
  public:
    std::shared_ptr<XTCPP::Base::Detector> _det;
  };
}

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

  py::class_<DetectorWrapper, std::shared_ptr<DetectorWrapper>>(m, "DetectorWrapper", py::dynamic_attr());
  py::class_<AlgWrapper, std::shared_ptr<AlgWrapper>>(m, "AlgWrapper", py::dynamic_attr());

  py::class_<XTCPP::MPI::DataSource>(m, "MPIDataSource")
    .def(py::init<std::string, std::variant<std::string, int>, size_t>())
    .def("rank", &XTCPP::MPI::DataSource::rank)
    .def("__iter__",
         [](XTCPP::MPI::DataSource& self) {
           return py::make_iterator(self.begin(), self.end());
         },
         py::keep_alive<0, 1>())
    .def("detector",
         [](XTCPP::MPI::DataSource& self, std::string detname) {
           auto det = self.detector(detname);
           std::shared_ptr<DetectorWrapper> det_wrapper = std::make_shared<DetectorWrapper>(det);
           py::object py_det = py::cast(det_wrapper);
           for (auto [alg_name, fields] : det->alg_fields()) {
             std::shared_ptr<AlgWrapper> alg_wrapper = std::make_shared<AlgWrapper>(det,
                                                                                    alg_name,
                                                                                    0);
             py::object py_alg = py::cast(alg_wrapper);
             for (auto field : fields) {
               py_alg.attr(field.name.c_str()) =
                 py::cpp_function([field](AlgWrapper& self, size_t evt) -> py::object {
                   auto alg_det = self.det;
                   void* val;
                   if (alg_det->is_epics()) {
                     val = alg_det->get_slow_update_data(evt);
                   } else if (alg_det->is_scan()) {
                     val = alg_det->get_scan_data(evt, self.name, field.name);
                   } else {
                     val = alg_det->get_l1_data(evt, self.name, field.name);
                   }
                   return cast_to_pyobject(val, field.data_type);
                 },
                   py::is_method(py_alg));
             }
             py_det.attr(alg_name.c_str()) = py_alg;
           }
           return py_det;
         },
         py::keep_alive<0,1>(),
         py::return_value_policy::reference);

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
