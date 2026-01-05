#include "common/bd_reader.hh"
#include "common/detector.hh"
#include "common/detector_utils.hh"
#include "common/smd_reader.hh"

#include "hdf5/mpiwriter.hh"
#include "mpi/bd_reader.hh"
#include "mpi/datasource.hh"
#include "mpi/detector.hh"
#include "mpi/smd_reader.hh"

#include "xtcdata/xtc/ShapesData.hh"

#include "mpi.h"

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <any>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdfloat>
#include <string>
#include <sys/types.h>
#include <unordered_set>
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
   * A non-PEP3118 conforming array view that supports double pointers.
   *
   * Since we know how the data will be arranged, this is not attempting to be
   * a conforming implementation of PEP3118 suboffsets. Instead, the first axis
   * is known to be a double pointer, and the offset calculations are adjusted
   * to make use of this known (and non-changing) fact.
   *
   * This class implements a limited subset of the NumPy functionality in order
   * to allow inspection of raw data that gets returned in a fashion that is
   * incompatible with NumPy's implementation (i.e. it would require suboffsets).
   * Slicing the array in a fashion that would result in a selection of data that
   * is compatible with NumPy will opt to return the NumPy array instead.
   * Additionally, any operations that would require a copy will also lead to the
   * return of a NumPy array.
   *
   * NOTE: The returned view is valid only while the underlying C++ buffer is.
   *       Python does NOT take any ownership via the wrapped version of this
   *       class.
   *
   * A future implementation could make use of PEP3118 suboffsets for greater
   * compatability. A description of that method is described here.
   *
   * The Python buffer protocol supports views of data where the various portions
   * of the buffer are located anywhere in memory. To do so requires a concept
   * called `suboffsets`. Refer to the Python API documentation for more information
   * but the basic usage of suboffsets is demonstrated by the following code snippet
   * used to access data from a buffer employing them.
   * Taken from the PEP3118 docs: https://peps.python.org/pep-3118/
   *
   * @code
   * void *get_item_pointer(int ndim, void *buf, Py_ssize_t *strides,
   *                        Py_ssize_t *suboffsets, Py_ssize_t *indices) {
   *    char *pointer = (char*)buf;
   *    int i;
   *    for (i = 0; i < ndim; i++) {
   *        pointer += strides[i] * indices[i];
   *        if (suboffsets[i] >=0 ) {
   *            pointer = *((char**)pointer) + suboffsets[i];
   *        }
   *     }
   *     return (void*)pointer;
   * }
   * @endcode
   */
  template <class T>
  class ArrayView {
  public:
    ArrayView(void** data_,
              std::vector<size_t>& shape_,
              std::vector<size_t>& strides_,
              py::dtype dtype_)
      : data(data_)
      , shape(shape_)
      , strides(strides_)
      , dtype(dtype_)
    {}

    /**
     * Retrieve the data at the multi-dimensional index.
     * Negative indices are supported in the normal Python fashion.
     */
    T at(const std::vector<ssize_t>& indices) {
      if (indices.size() > shape.size()) {
        py::index_error();
      }
      void** working_dbl_ptr = data;
      uint8_t* working_ptr {nullptr};
      for (size_t i=0; i < indices.size(); ++i) {
        ssize_t idx = indices[i];
        if (idx < 0) {
          // Support negative indices - wrap around
          idx += shape[i];
        }
        if (idx < 0 || static_cast<size_t>(idx) >= shape[i]) {
          throw py::index_error();
        }
        if (i == 0) {
          working_ptr =
            reinterpret_cast<uint8_t*>(reinterpret_cast<T**>(working_dbl_ptr)[idx]);
        } else {
          working_ptr += idx * strides[i];
        }
      }
      return *reinterpret_cast<T*>(working_ptr);
    }

    std::variant<ArrayView<T>, py::array_t<T>>
    operator[](py::object slices_or_indices) const {
      void** new_data = data;
      std::vector<size_t> new_shape = shape;
      std::vector<size_t> new_strides = strides;

      // Check to see if we still need the double pointers for the first axis
      // if not, we'll just return a NumPy array
      bool new_first_axis_ptrs {true};
      size_t axis{0};
      if (py::isinstance<py::int_>(slices_or_indices)) {
        // Just an integer passed as index
        ssize_t idx = slices_or_indices.cast<ssize_t>();
        if (idx < 0) {
          idx += new_shape[0];
        }
        if (idx < 0 || static_cast<size_t>(idx) >= new_shape[0]) {
          throw py::index_error();
        }

        // Convert to NumPy array now
        void* arr_data = new_data[idx];
        new_shape.erase(new_shape.begin());
        new_strides.erase(new_strides.begin());
        return py::array_t<T>(py::buffer_info(arr_data,
                                              sizeof(T),
                                              py::format_descriptor<T>::format(),
                                              new_shape.size(),
                                              new_shape,
                                              new_strides));
      }
      // If not integer must be a tuple otherwise throw error
      if (!py::isinstance<py::tuple>(slices_or_indices)) {
        throw py::index_error();
      }

      // Handle each dimension specified by the tuple
      for (auto arg : slices_or_indices) {
        if (py::isinstance<py::slice>(arg)) {
          // Dealing with slices
          py::slice slice = arg.cast<py::slice>();

          ssize_t start, stop, step, length;
          if (!slice.compute(shape[axis], &start, &stop, &step, &length)) {
            throw py::error_already_set();
          }
          if (axis == 0) {
            // Pointer axis (segments)
            new_data += start;
            new_shape[axis] = length;
          } else {
            uint8_t* tmp = reinterpret_cast<uint8_t*>(reinterpret_cast<T**>(new_data));
            tmp += start * strides[axis];
            new_data = reinterpret_cast<void**>(tmp);
            new_shape[axis] = length;
            new_strides[axis] *= step;
          }
          axis++;
        } else if (py::isinstance<py::int_>(arg)) {
          // Dealing with single integer indices for the axis
          ssize_t idx = arg.cast<ssize_t>();
          if (idx < 0) {
            // Support negative indices - wrap around
            idx += new_shape[axis];
          }
          if (idx < 0 || static_cast<size_t>(idx) >= new_shape[axis]) {
            throw py::index_error();
          }
          uint8_t* tmp =
              reinterpret_cast<uint8_t*>(reinterpret_cast<T**>(new_data));
          if (axis == 0) {
            new_data = &new_data[idx];
            new_first_axis_ptrs = false;
          } else {
            tmp += idx * new_strides[axis];
            new_data = reinterpret_cast<void**>(tmp);
          }
          new_shape.erase(new_shape.begin() + axis);
          new_strides.erase(new_strides.begin() + axis);
        } else if (py::isinstance<py::ellipsis>(arg)) {
          axis = new_shape.size();
        } else {
          throw py::index_error("Unrecognized indexing type.");
        }
      }

      if (!new_first_axis_ptrs) {
        return py::array_t<T>(py::buffer_info(new_data[0],
                                              sizeof(T),
                                              py::format_descriptor<T>::format(),
                                              new_shape.size(),
                                              new_shape,
                                              new_strides));
      } else {
        return ArrayView<T>(new_data,
                            new_shape,
                            new_strides,
                            dtype);
      }
    }

    /**
     * Return a NumPy style string for a __repr__ binding.
     *
     * @return repr_str A string representaiton of the ArrayView data. It will
     *         truncate axes which get too long and replace them with "...", as
     *         NumPy's formatting does.
     */
    std::string repr() {
      std::string class_name {"ArrayView("};
      std::string opening {class_name + "("};
      std::ostringstream oss;

      oss << opening;

      size_t base_indent {opening.size() + shape.size() - 1};
      if (shape.empty()) {
        oss << "[], dtype=" << py::str(dtype).cast<std::string>() << ")";
        return oss.str();
      }

      oss << "[";
      // Define number of items along axis before replacing with output with `...`
      constexpr size_t max_items {3};
      size_t nsegs = shape[0];

      for (size_t seg=0; seg < nsegs; ++seg) {
        uint8_t* base = reinterpret_cast<uint8_t*>(reinterpret_cast<T**>(data)[seg]);

        repr_internal(oss, base, 1, base_indent, max_items);
        if (seg < nsegs - 1) {
          if (nsegs > 2 * max_items && seg == 2) {
            seg = nsegs - (max_items + 1); // Will be incremented on next iteration
          }
          oss << ",\n\n" << std::string(base_indent-1, ' ');
        }
      }

      oss << "], dtype=" << py::str(dtype).cast<std::string>() << ")";

      return oss.str();
    }

    /**
     * A function to support an __add__ implementation in the Python bindings.
     *
     * TODO: Support the use of a NumPy array as `other`.
     */
    py::array_t<T> add(const ArrayView<T>& other) const {
      using SumFn = std::function<void(uint8_t*, uint8_t*, T*)>;

      SumFn add = [&](uint8_t* lhs, uint8_t* rhs, T* out) {
        *out = *reinterpret_cast<T*>(lhs) + *reinterpret_cast<T*>(rhs);
      };

      return op_impl(other, add);
    }

    /**
     * A function to support an __mul__ implementation in the Python bindings.
     *
     * TODO: Support the use of a NumPy array as `other`.
     */
    py::array_t<T> mul(const ArrayView<T>& other) const {
      using MulFn = std::function<void(uint8_t*, uint8_t*, T*)>;

      MulFn mul = [&](uint8_t* lhs, uint8_t* rhs, T* out) {
        *out = *reinterpret_cast<T*>(lhs) * *reinterpret_cast<T*>(rhs);
      };

      return op_impl(other, mul);
    }

  private:
    void along_each_inner_axis_do(uint8_t* lhs_base,
                                  uint8_t* rhs_base,
                                  T* out_base,
                                  size_t axis,
                                  std::function<void(uint8_t*,uint8_t*,T*)> operation) const {
      if (axis == shape.size()) {
        operation(lhs_base, rhs_base, out_base);
        return;
      }

      size_t dim = shape[axis];
      size_t stride = strides[axis];

      for (size_t i=0; i < dim; ++i) {
        along_each_inner_axis_do(lhs_base + i * stride,
                                 rhs_base + i * stride,
                                 out_base + i,
                                 axis + 1,
                                 operation);
      }
    }

    py::array_t<T> op_impl(const ArrayView<T>& other,
                           std::function<void(uint8_t*,uint8_t*,T*)> operation) const {
      if (shape != other.shape) {
        throw py::value_error("ArrayView shapes must match!");
      }

      // Will create new contiguous output array - jumps to NumPy so we're done
      // with ArrayViews after this function executes
      py::array_t<T> result(shape);

      // Get the buffer info (can grab pointer from this)
      py::buffer_info result_buffer = result.request();

      const size_t nsegs = shape[0];
      const size_t pix_per_seg = std::accumulate(shape.begin() + 1,
                                                 shape.end(),
                                                 1,
                                                 std::multiplies{});

      // Segment data is contiguous - loop over segments, and pass in a contiguous
      // block to operate on. Then fill the contiguous array allocated above
      for (size_t seg=0; seg < nsegs; ++seg) {
        uint8_t* lhs_base =
          reinterpret_cast<uint8_t*>(reinterpret_cast<T**>(data)[seg]);
        uint8_t* rhs_base =
          reinterpret_cast<uint8_t*>(reinterpret_cast<T**>(other.data)[seg]);

        // Get raw pointer and calculate the offset
        // Block is all contiguous now, so should be simple math
        T* out_base = reinterpret_cast<T*>(result_buffer.ptr);
        out_base += seg * pix_per_seg;

        along_each_inner_axis_do(lhs_base,
                                 rhs_base,
                                 out_base,
                                 1,
                                 operation);
      }

      return result;
    }

    void repr_internal(std::ostringstream& oss,
                       uint8_t* base,
                       size_t axis,
                       size_t indent,
                       size_t max_items) const {
      if (axis == shape.size()) {
        oss << *reinterpret_cast<T*>(base);
        return;
      }

      oss << "[";

      size_t dim = shape[axis];
      size_t stride = strides[axis];

      auto format_item = [&](size_t i) -> void {
        if (i > 0 && axis == shape.size() - 1) {
          oss << ", ";
        }

        repr_internal(oss,
                      base + i * stride,
                      axis + 1,
                      indent,
                      max_items);

        if (axis + 1 < shape.size()) {
          oss << "\n" << std::string(indent, ' ');
        }

      };

      if (dim <= 2 * max_items) {
        for (size_t i=0; i < dim; ++i) {
          format_item(i);
        }
      } else {
        for (size_t i=0; i < max_items; ++i) {
          format_item(i);
        }

        if (axis + 1 < shape.size()) {
          oss << "...\n" << std::string(indent, ' ');
        } else {
          oss << ", ...";
        }

        for (size_t i = dim - max_items; i < dim; ++i) {
          if (axis == shape.size() - 1) {
            oss << ", ";
          }
          repr_internal(oss,
                        base + i * stride,
                        axis + 1,
                        indent + 1,
                        max_items);
          if (axis + 1 < shape.size() && i < dim - 1) {
            oss << "\n" << std::string(indent, ' ');
          }
        }
        oss << "]";
      }
    }


  public:
    void** data;
    std::vector<size_t> shape;
    std::vector<size_t> strides;
    py::dtype dtype;
  };

  /**
   * Properly convert the scalar or array to a Python object.
   * Scalar and arrays constructed from a single segment are hopefully
   * self-explanatory.
   *
   * NOTE: The intended approach for multi-segment arrays is described here,
   *       however, it has been abandoned for now. NumPy does not support
   *       the use of suboffsets - so for now we use an ArrayView
   *       The description for the suboffsets method is retained.
   *       is retained, and some of the required code is left commented in case
   *       it can be used in the future.
   *
   * @param[in] val Raw data pointer. A double pointer since multiple segments
   *            may be located at different places in memory.
   * @param[in] rank The rank/number of dimensions. 0 indicates scalar
   * @param[in] shape The shape pointer. Ignored for scalars (rank == 0)
   * @return object The cast Python object.
   */
  template <class T>
  py::object handle_scalar_or_array(void** val, uint32_t rank, std::vector<uint32_t> shape) {
    if (rank == 0) {
      /* Scalar */
      return py::cast(**reinterpret_cast<T**>(val));
    } else {
      size_t n_elements = 1;
      for (size_t i = 0; i < rank; ++i) {
        n_elements *= shape[i];
      }
      if (rank == 1) {
        /* Simpler case - can just de-reference the first pointer */
        std::vector<size_t> strides = {sizeof(T)};
        std::vector<size_t> arrshape = {n_elements};
        if constexpr (std::is_same_v<char, T>) {
          return py::memoryview::from_buffer(reinterpret_cast<T**>(val)[0],
                                             arrshape,
                                             strides);
        } else {
          return py::array_t<T>(py::buffer_info(reinterpret_cast<T**>(val)[0],
                                                sizeof(T),
                                                py::format_descriptor<T>::format(),
                                                1,
                                                arrshape,
                                                strides));
        }
      } else {
        size_t nsegs = static_cast<size_t>(shape[0]);
        std::vector<size_t> strides(rank - 1, sizeof(T));
        // NOTE: Do NOT use shape.end() -- the size of the shape vector will be
        // something like 10. This is to match the "max dimensions" mechanism
        // used in XtcData. Always use the rank value passed independently to
        // determine dimensionality.
        std::vector<size_t> arrshape(shape.begin() + 1, shape.begin() + rank);
        for (ssize_t j = rank - 3; j >= 0; --j) {
          size_t i = static_cast<size_t>(j);
          strides[i] = strides[i + 1] * arrshape[i + 1];
        }
        if (nsegs <= 1) {
          return py::array_t<T>(py::buffer_info(reinterpret_cast<T**>(val)[0],
                                                sizeof(T),
                                                py::format_descriptor<T>::format(),
                                                rank - 1,
                                                arrshape,
                                                strides));
        } else {
          strides.insert(strides.begin(), 1);
          arrshape.insert(arrshape.begin(),shape[0]);
          return py::cast(ArrayView<T>(val, arrshape, strides, py::dtype::of<T>()));
        }
      }
    }
  }

  /**
   * The XtcData::Name::DataType enum has the following enumerators (in order):
   * { UINT8, UINT16, UINT32, UINT64, INT8, INT16, INT32, INT64, FLOAT, DOUBLE,
   *   CHARSTR, ENUMVAL, ENUMDICT}
   *
   * This function takes void** pointer which is returned from the get_*_data
   * C++ APIs, and a datatype enumerator to return a Python object of
   * appropriate type. Rank and shape information is also passed to create arrays.
   *
   * This function only parses the XTC type. The scalar/array creation is done
   * by `handle_scalar_or_array`, which is templated, the correct invokation
   * being decided here.
   *
   * @param[in] val Raw data pointer. A double pointer since multiple segments
   *            may be located at different places in memory.
   * @param[in] dtype The enumerator describing the data type pointed to by val.
   * @param[in] rank The rank/number of dimensions. 0 indicates scalar
   * @param[in] shape The shape pointer. Ignored for scalars (rank==0).
   * @return object The cast Python object.
   */
  py::object cast_to_pyobject(void** val,
                              XtcData::Name::DataType dtype,
                              uint32_t rank,
                              std::vector<uint32_t> shape) {
    switch (dtype) {
    case XtcData::Name::UINT8:
      return handle_scalar_or_array<uint8_t>(val, rank, shape);
    case XtcData::Name::UINT16:
      return handle_scalar_or_array<uint16_t>(val, rank, shape);
    case XtcData::Name::UINT32:
      return handle_scalar_or_array<uint32_t>(val, rank, shape);
    case XtcData::Name::UINT64:
      return handle_scalar_or_array<uint64_t>(val, rank, shape);
    case XtcData::Name::INT8:
      return handle_scalar_or_array<int8_t>(val, rank, shape);
    case XtcData::Name::INT16:
      return handle_scalar_or_array<int16_t>(val, rank, shape);
    case XtcData::Name::INT32:
      return handle_scalar_or_array<int32_t>(val, rank, shape);
    case XtcData::Name::INT64:
      return handle_scalar_or_array<int64_t>(val, rank, shape);
    case XtcData::Name::FLOAT:
      return handle_scalar_or_array<float>(val, rank, shape);
    case XtcData::Name::DOUBLE:
      return handle_scalar_or_array<double>(val, rank, shape);
    case XtcData::Name::CHARSTR: {
      // Can do coercion directly to a string for this guy - memoryview otherwise
      return handle_scalar_or_array<char>(val, rank, shape);
    }
    default:
      return py::object(py::cast(nullptr));
    }
  }

  /**
   * A basically empty container for "Algorithm" information.
   * This is used to provide modifiable objects to attach methods to.
   */
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

  /**
   * A basically empty wrapper for the C++ Detector object.
   * Can be modified to make Python bindings more user friendly without making
   * any modifications to the underlying C++ classes.
   */
  class DetectorWrapper {
  public:
    DetectorWrapper(std::shared_ptr<XTCPP::Base::Detector> det)
      : _det(det)
    {}
  public:
    std::shared_ptr<XTCPP::Base::Detector> _det;
  };

  // Potentially use sets like this or something similar for the todo idea below
  //std::unordered_set<std::string> constructed_det_classes;
  //std::unordered_set<std::string> constructed_det_alg_classes;

  /**
   * Dynamically construct Algorithms of the appropriate type with
   * methods to retrieve all raw data in the XTC2 file.
   *
   * NOTE: The DetectorWrapper and AlgWrapper classes MUST have been registered
   *       with pybind and added to the module BEFORE any call to this function.
   *
   * TODO: This should be revisited to avoid the use of dynamic attributes and
   *       __dict__. Instead, the classes should be created dynamically.
   *
   * @param[in] m The module the class will be added to.
   * @param[in] detname The detector name for which the sub-class Algorithm is created
   * @param[in] ds The DataSource through which this detector will be accessed.
   * @return py_det The populated detector object
   */
  py::object dynamically_create_alg(py::module& m, const std::string detname, XTCPP::MPI::DataSource& ds) {
    auto det = ds.detector(detname);
    std::shared_ptr<DetectorWrapper> det_wrapper = std::make_shared<DetectorWrapper>(det);
    py::object py_det = py::cast(det_wrapper);
    auto py_det_setattr = py::getattr(py_det, "__setattr__");
    auto MethodType = py::module_::import("types").attr("MethodType");
    if (det->is_epics()) {
      std::shared_ptr<AlgWrapper> alg_wrapper = std::make_shared<AlgWrapper>(det,
                                                                             "raw",
                                                                             0x020000);
      for (auto [alg_info, fields] : det->alg_fields()) {
        for (auto field: fields) {
          if (field.name != det->detname()) {
            continue;
          }
          auto get_field_method =
            py::cpp_function([field](DetectorWrapper& self, size_t evt) -> py::object {
              auto alg_det = self._det;
              std::tuple<void**, uint32_t, std::vector<uint32_t>> ret =
                alg_det->get_slow_update_data(evt);
              auto [val, rank, shape] = ret;
              return cast_to_pyobject(val, field.data_type, rank, shape);
            },
              py::is_method(py_det));
          py_det_setattr("__call__", MethodType(get_field_method, py_det));
          py_det_setattr("get", MethodType(get_field_method, py_det));
        }
      }
    } else {
      for (auto [alg_info, fields] : det->alg_fields()) {
        auto [alg_name, alg_version] = alg_info;
        std::shared_ptr<AlgWrapper> alg_wrapper = std::make_shared<AlgWrapper>(det,
                                                                               alg_name,
                                                                               alg_version);
        py::object py_alg = py::cast(alg_wrapper);
        auto py_alg_setattr = py::getattr(py_alg, "__setattr__");
        for (auto field : fields) {
          auto get_field_method =
            py::cpp_function([field](AlgWrapper& self, size_t evt) -> py::object {
              auto alg_det = self.det;
              std::tuple<void**, uint32_t, std::vector<uint32_t>> ret;
              if (alg_det->is_scan()) {
                ret = alg_det->get_scan_data(evt, self.name, field.name);
              } else {
                ret = alg_det->get_l1_data(evt, self.name, field.name);
              }
              auto [val, rank, shape] = ret;
              return cast_to_pyobject(val, field.data_type, rank, shape);
            },
              py::is_method(py_alg));
          // MethodType call is hacky mechanism to ensure the method is seen as
          // bound to the py_alg object. This avoids needing to call it by passing
          // the instance as the first argument (i.e. `self` is auto-passed)
          py_alg_setattr(field.name.c_str(), MethodType(get_field_method, py_alg));

          // TODO: Implement a better method for calib method...
          std::string det_type = det->det_type();
          if (alg_name == "raw" && (det_type == "jungfrau" || det_type == "epix100")) {
            auto calib_method =
              py::cpp_function([det_type](AlgWrapper& self, size_t evt) -> py::object {
                auto alg_det = self.det;
                XTCPP::OpFn operation = XTCPP::calibrate_segment;;
                [[maybe_unused]] auto raw_data = alg_det->get_l1_data(evt,
                                                                      "raw",
                                                                      "raw");
                XTCPP::calibrate(det_type,
                                 alg_det->data_ptrs(),
                                 alg_det->calibconst_span(),
                                 alg_det->calib_data_buf());

                size_t ndim;
                std::vector<size_t> shape;
                std::vector<size_t> strides;
                if (det_type == "jungfrau") {
                  size_t nsegs{32};
                  size_t nrows{512};
                  size_t ncols{1024};
                  ndim = 3;
                  shape = std::vector<size_t>{nsegs, nrows, ncols};
                  strides = std::vector<size_t>(3);
                  strides[2] = sizeof(float);
                  strides[1] = ncols * strides[2];
                  strides[0] = nrows * strides[1];
                } else if (det_type == "epix100") {
                  size_t nrows{704};
                  size_t ncols{768};
                  ndim = 2;
                  shape = std::vector<size_t>{nrows, ncols};
                  strides = std::vector<size_t>(2);
                  strides[1] = sizeof(float);
                  strides[0] = nrows * strides[1];
                } else {
                  // This shouldn't happen...
                  return py::object(py::cast(nullptr));
                }
                return py::array_t<float>(py::buffer_info(reinterpret_cast<float*>(alg_det->calib_data_buf().data()),
                                                          sizeof(float),
                                                          py::format_descriptor<float>::format(),
                                                          ndim,
                                                          shape,
                                                          strides));
              });
            py_alg_setattr("calib", MethodType(calib_method, py_alg));
          }
        }
        py_det_setattr(alg_name.c_str(), py_alg);
      }
    }
    return py_det;
  }
} // anonymous namespace

template <typename T>
void bind_arrayview(py::module_& m, const std::string& type_name) {
  using ArrayViewType = ArrayView<T>;

  py::class_<ArrayViewType>(m, type_name.c_str())
      .def_property_readonly(
          "shape", [](const ArrayViewType& self) { return self.shape; })
      .def_property_readonly(
          "ndim", [](const ArrayViewType& self) { return self.shape.size(); })
      .def_property_readonly(
          "dtype", [](const ArrayViewType& self) { return self.dtype; })
      .def("__getitem__", &ArrayViewType::operator[])
      .def("__repr__", &ArrayViewType::repr)
      .def("__str__", &ArrayViewType::repr)
      .def("__add__", &ArrayViewType::add, py::is_operator())
      .def("__mul__", &ArrayViewType::mul, py::is_operator());
}

/**
 * Note: Must be careful in the class definitions for selecting the "holder"
 * type. This is by default a unique_ptr (for pybind11 backward compatibility).
 * This will cause issues for the implementation of the C++ code in terms of
 * ownership and lifetime. Instead use py::smart_holder or shared_ptr as used
 * for the relevant classes below.
 * ( py::class_<CPP::Class, std::shared_ptr<CPP::Class>>(pyxtcpp_module, "PyClass")... )
 */

// mod_gil_not_used is generally inteded for the free-threading build
// This is strictly speaking quite dangerous since we're releasing the GIL
// to be used with Python bindings. However, this is quite a thin wrapper
// lets... try it for now
PYBIND11_MODULE(_xtcpp, pyxtcpp_module, py::mod_gil_not_used()) {
  pyxtcpp_module.doc() = "XTCPP Python bindings.";

  //pyxtcpp_dets_module = pyxtcpp_module.def_submodule("detectors", py::mod_gil_not_used())

  bind_arrayview<uint8_t>(pyxtcpp_module, "ArrayView_u8");
  bind_arrayview<uint16_t>(pyxtcpp_module, "ArrayView_u16");
  bind_arrayview<uint32_t>(pyxtcpp_module, "ArrayView_u32");
  bind_arrayview<uint64_t>(pyxtcpp_module, "ArrayView_u64");

  bind_arrayview<int8_t>(pyxtcpp_module, "ArrayView_i8");
  bind_arrayview<int16_t>(pyxtcpp_module, "ArrayView_i16");
  bind_arrayview<int32_t>(pyxtcpp_module, "ArrayView_i32");
  bind_arrayview<int64_t>(pyxtcpp_module, "ArrayView_i64");

  bind_arrayview<float>(pyxtcpp_module, "ArrayView_float");
  bind_arrayview<double>(pyxtcpp_module, "ArrayView_double");

  py::class_<XTCPP::BDXtcOffset>(pyxtcpp_module, "BDXtcOffset")
    .def_readwrite("offset", &XTCPP::BDXtcOffset::offset)
    .def_readwrite("size", &XTCPP::BDXtcOffset::size);

  // TODO: Try to rework these wrappers to remove dynamic_attr
  //       Instead, create the *classes* dynamically and attach the methods
  py::class_<DetectorWrapper, std::shared_ptr<DetectorWrapper>>(pyxtcpp_module,
                                                                "DetectorWrapper",
                                                                py::dynamic_attr());
  py::class_<AlgWrapper, std::shared_ptr<AlgWrapper>>(pyxtcpp_module,
                                                      "AlgWrapper",
                                                      py::dynamic_attr());
  py::class_<XTCPP::MPI::DataSource>(pyxtcpp_module, "MPIDataSource")
    .def(py::init<std::string, std::variant<std::string, int>, size_t>())
    .def("rank", &XTCPP::MPI::DataSource::rank)
    .def("__iter__",
         [](XTCPP::MPI::DataSource& self) {
           return py::make_iterator(self.begin(), self.end());
         },
         py::keep_alive<0, 1>())
    .def("detector",
         [&pyxtcpp_module](XTCPP::MPI::DataSource& self, std::string detname) {
           // This function will dynamically construct instances for the
           // Algorithms and Detectors, as AlgWrapper and DetectorWrapper objects.
           // The methods to retrieve raw data from the XTC2 files are added to
           // the algorithms, which are in turn added to the detector objects
           // In the future this should be done as a creation of sub-classes, which
           // will avoid the use of py::dynamic_attr (see the todo above)
           return dynamically_create_alg(pyxtcpp_module, detname, self);
         },
         py::keep_alive<0,1>(),
         py::return_value_policy::reference)
    . def("get_last_index", &XTCPP::MPI::DataSource::get_last_index);

  // TODO: Actually implement HDF5Writer
  py::class_<XTCPP::MPI::HDF5Writer>(pyxtcpp_module, "SmallData")
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

  py::class_<XTCPP::MPI::BDReader>(pyxtcpp_module, "MPIBDReader")
    //std::shared_ptr<XTCPP::Base::BDReader>>(pyxtcpp_module, "MPIBDReader")
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
} // pyxtcpp_module
