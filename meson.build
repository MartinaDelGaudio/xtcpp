project(
    'xtcpp',
    'cpp',
    version: '0.1.0',
    default_options: [
        'cpp_std=c++23',
        'optimization=3',
        # Statically link subproject libraries by default as there are
        # difficulties getting the wheels to work with shared libraries
        'default_library=static',
    ],
)
add_project_arguments('-frtti', language: 'cpp')
add_project_arguments('-fopenmp', language: 'cpp')
add_project_arguments('-march=native', language: 'cpp')

pymod = import('python')

py3 = pymod.find_installation(pure: false)
py3_dep = py3.dependency()

min_python_ver = '3.8'
py_ver = py3.language_version()
if py_ver.version_compare(f'<@min_python_ver@')
    error(f'Minimum supported Python version is @min_python_ver@, found @py_ver@')
endif

pybind11_dep = dependency('pybind11', required: true)

#mpi_dep = dependency('mpi', required: true)

include_directories = include_directories(
    '/sdf/group/lcls/ds/ana/sw/conda2/rel/lcls2_100925/install/include',
    '/sdf/group/lcls/ds/ana/sw/conda2/inst/envs/ps_20241122/include',
)

# Specify library directories
library_directories = [
    '/sdf/group/lcls/ds/ana/sw/conda2/rel/lcls2_100925/install/lib',
    '/sdf/group/lcls/ds/ana/sw/conda2/inst/envs/ps_20241122/lib',
    'src',
]

openmp_dep = dependency('openmp', required: true)

#### Kludge HDF5 ########

hdf5_dep = declare_dependency(
    include_directories: include_directories,
    link_args: [
        '-L/sdf/group/lcls/ds/ana/sw/conda2/inst/envs/ps_20241122/lib',
        '-lhdf5_cpp',
        '-lhdf5',
    ],
)

#### xtcdata subproject dependency ####
#######################################

xtcdata_proj = subproject('xtcdata')
staticXtc_dep = xtcdata_proj.get_variable('staticXtc_dep')

#### cpp-httplib subproject dependency ####
###########################################

cpp_httplib_proj = subproject('cpp-httplib')
httplib_dep = cpp_httplib_proj.get_variable('cpp_httplib_dep')

#### rapidjson subproject dependency ####
#########################################

rapidjson_proj = subproject('rapidjson')
rapidjson_dep = rapidjson_proj.get_variable('rapidjson_dep')

#### spdlog subproject dependency ####
######################################

spdlog_proj = subproject('spdlog')
spdlog_dep = spdlog_proj.get_variable('spdlog_dep')

#### XTCPP ####
###############
## Common/base library

libxtcpp_base = shared_library(
    'xtcpp_base',
    [
        'src/common/datasource.cc',
        'src/common/detector.cc',
        'src/common/bd_reader.cc',
        'src/common/smd_reader.cc',
        'src/util/threadpool.cc',
    ],
    dependencies: [
        spdlog_dep,
        httplib_dep,
        openmp_dep,
        rapidjson_dep,
        staticXtc_dep,
    ],
    include_directories: include_directories,
    install: true,
    install_dir: py3.get_install_dir(),
    version: '1',
    soversion: '1',
)

xtcpp_base_dep = declare_dependency(
    link_with: [
        libxtcpp_base
    ],
    dependencies: [
        httplib_dep,
        spdlog_dep,
        openmp_dep,
        rapidjson_dep,
        staticXtc_dep,
    ],
    include_directories: include_directories(
        'src',
    ),
)

## MPI Implementation

libxtcpp_mpi = shared_library(
    'xtcpp_mpi',
    [
        'src/mpi/datasource.cc',
        'src/mpi/detector.cc',
        'src/mpi/bd_reader.cc',
        'src/mpi/smd_reader.cc',
        'src/util/threadpool.cc',
    ],
    dependencies: [
        spdlog_dep,
        httplib_dep,
        xtcpp_base_dep,
        staticXtc_dep,
    ],
    install: true,
    install_dir: py3.get_install_dir(),
    version: '1',
    soversion: '1',
)

xtcpp_mpi_dep = declare_dependency(
    link_with: [
        libxtcpp_mpi
    ],
    dependencies: [
        httplib_dep,
        xtcpp_base_dep,
        rapidjson_dep,
        staticXtc_dep,
    ],
    include_directories: include_directories(
        'src/mpi',
    ),
)

## HDF5 writing

libxtcpp_hdf5 = shared_library(
    'xtcpp_hdf5',
    [
        'src/hdf5/hdf5writer.cc',
        'src/hdf5/mpiwriter.cc',
    ],
    dependencies: [
        spdlog_dep,
        hdf5_dep,
    ],
    install: true,
    install_dir: py3.get_install_dir(),
    version: '1',
    soversion: '1',
)

xtcpp_hdf5_dep = declare_dependency(
    link_with: [
        libxtcpp_hdf5
    ],
    dependencies: [
        spdlog_dep,
        hdf5_dep,
        xtcpp_mpi_dep,
    ],
    include_directories: include_directories(
        'src/hdf5',
    ),
)

#### Test executables ####
##########################

jungfrau_test_app = executable(
    'jungfrau_reader',
    [
        'src/app/jungfrau_reader.cc',
    ],
    dependencies: [
        xtcpp_mpi_dep,
        xtcpp_hdf5_dep,
    ],
    include_directories: include_directories(
        'src',
    ),
    install: true,
    install_dir: get_option('bindir'),
    # *** this is horrible but I don't know how to fix it *** #
    build_rpath: '$ORIGIN/../lib/python' + py_ver + '/site-packages',
    install_rpath: '$ORIGIN/../lib/python' + py_ver + '/site-packages',
)

#### Python module ####
#######################

py_mod = py3.extension_module(
    '_xtcpp',
    [
        'src/python/pyxtcpp.cc'
    ],
    install: true,
    dependencies : [
        hdf5_dep,
        pybind11_dep,
        xtcpp_base_dep,
        xtcpp_mpi_dep,
        xtcpp_hdf5_dep,
        spdlog_dep,
    ],
    # subdir: '_xtcpp',
    install_rpath: '$ORIGIN',
)