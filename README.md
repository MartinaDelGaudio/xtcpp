# XTCPP - XTC Parallel Processing

This repo is intended to provide a lightweight library for performing parallel IO (and more specifically, reads) of XTC2 files.

This is just a prototype and very early in development. The goal, however, is to provide a basic interface and then specific implementations for various parallelization schemes. MPI is currently targeted as the first scheme.

## Installation

On S3DF run:

```bash
git clone git@github.com:slac-lcls/xtcpp
cd xtcpp
./build.sh
```

This will create a sub-directory `install` with the shared libraries, example binary, and the Python bindings.

If you are currently in the `xtcpp` folder after cloning and having built the code, the following commands will make the Python bindings available and add the test binary to your PATH.

```bash
export PYTHONPATH="$(pwd)/install/lib/python3.9/site-packages:${PYTHONPATH}"
export PATH="$(pwd)/install/bin:${PATH}"
```

### Build System Information and Dependencies

- Build system: meson
- Standard: C++23
- Python bindings: pybind11
- Dependencies:
  - Build only:
      - pybind11
      - meson-python
      - meson
  - Core:
      - XtcData
      - spdlog
      - mpi (for MPI implementation)
  - Calibration:
      - Rapidjson
      - cpp-httplib
  - HDF5
      - HDF5

**Note:** Dependencies are statically linked currently. They are `rapidjson`, `cpp-httplib`, and `spdlog` (plus its `fmt` dependency) are included as meson subprojects in this repository.

## Logging

`spdlog` is used for logging. You can set the log level with an environment variable:

```bash
export XTCPP_LOG_LEVEL=debug
```

This can also be done per logger. E.g.:

```bash
export XTCPP_LOG_LEVEL=info,Base::Detector=debug
```

## Example uses

After installation the following programs can be used as a starting point.

Note: The test C++ example looks at both a jungfrau detector and an epix100 detector. The test Python script looks only at the jungfrau detector.

All programs can (should) be submitted with `mpirun`. **See the notes following this for important information about MPI. Do NOT skip that overview.**

### C++ Interface

```bash
> jungfrau_reader -h
Usage: jungfrau_reader -e <experiment> -r <run> [-n <fetch_events>] [-c] [-s] [-t]


Run some test processing on `jungfrau` and `epix100_0` for an MFX experiment.
NOTE: Both detectors must be present in the experiment/run chosen.

Can also be run with MPI
  - `mpirun -np <NUM PROCS> ...
    (You can use `-mca osc ^ucx` if you see UCX errors - they can be ignored though)

Or can change the number of open OpenMP threads by setting the environment variable `OMP_NUM_THREADS`.
 - Set to 1 to run single threaded (`OMP_NUM_THREADS=1 [mpirun] ...`)

Args:
  -e <experiment> Experiment to process
  -r    <run>     Run number to process
  -n <fetch_evts> Number of offsets to read per fetch of .smd.xtc2 file.
 [-c]             Optionally run the `calib` method instead of `raw`.
 [-s]             Optionally write an HDF5 file with a ROI for the jungfrau.
 [-t]             Set a total number of events to iterate.
 [-h]             Display this help message.
```

(The `-t` flag currently does not work)

### Python bindings

A test Python script is provided in `examples/test_pytiming.py`:

```bash
> python -B examples/test_pytiming.py -h
usage: test_pytiming.py [-h] [-b BATCH_SIZE] [-c] [--events_per_read EVENTS_PER_READ] [-e EXPERIMENT] [-r RUN] [-s]

optional arguments:
  -h, --help            show this help message and exit
  -b BATCH_SIZE, --batch_size BATCH_SIZE
                        Batch size for SmallData writes.
  -c, --calib           If passed, use det.raw.calib. Otherwise det.raw.raw
  --events_per_read EVENTS_PER_READ
                        Number of smd offsets (event offsets) to fetch per read.
  -e EXPERIMENT, --experiment EXPERIMENT
                        Experiment to load
  -r RUN, --run RUN     Run to load
  -s, --smalldata       If passed, write a test HDF5 with SmallData.
```

For comparison, a psana-based script is also provided:

```bash
> python -B examples/iterate_psana.py -h
usage: iterate_psana.py [-h] [-c] [--debug] [-e EXPERIMENT] [-r RUN]

optional arguments:
  -h, --help            show this help message and exit
  -c, --calib           If passed, use det.raw.calib. Otherwise det.raw.raw
  --debug               If passed perform debug prints every 100 events (slows performance).
  -e EXPERIMENT, --experiment EXPERIMENT
                        Experiment to load
  -r RUN, --run RUN     Run to load
```

## OpenMP

The code is compiled with support for OpenMP - currently this is only actually used in the calibrate function; however, it is not yet optimized. You can disable the threading by doing:

```bash
> export OMP_NUM_THREADS=1
```

Alternatively, you could increase this number. If using threads, it would then make sense to play with the mapping and binding of MPI ranks to the available cores.

## Using MPI

### `OSC` issues

The MPI implementation makes use of "windows". This is provided by the `one-sided communication` (`OSC`) component of MPI. By default `UCX` (The unified communication X framework library) is used, but unfortunately this has some issues on the batch nodes (but not the interactive ones). Therefore, it must be excluded for batch jobs.

Use:
```bash
> mpirun --mca osc ^ucx <rest of arguments>
```

- Alternatively, you could specify what to use explicitly; however, excluding ucx is probably fine. For reference though:

  - `sm` is probably the fastest choice, but it only will work on a single node.

### Optimizing rank afinity and placement

Due to the use of shared memory the placement of ranks can have an a fairly large impact on the performance.

An example of a set of directives that will likely perform better than a vanilla call to `mpirun` is:

```bash
> mpirun --mca mpi_paffinity_alone 1 --bind-to core --map-by numa --mca osc ^ucx
```

## Repo organization

- `src/common` contains a base interface and some common implementation.
- `src/mpi` contains the MPI specific code. Concrete classes from this source code should be instantiated for actual use of the library.
- `src/hdf5` contains HDF5 writing facilities (similar to the "smalldata" mechanism in psana)
- `src/python` contains Python bindings with pybind11

## Classes/Code Overview

There are four main objects:

1. `SMDReader`: Reads `.smd.xtc2` files for determining offsets in the "big data" files.
2. `BDReader`: Reads the "big data" files for getting actual image data. Each `BDReader` reads a single `xtc2` file. It manages a corresponding `SMDReader`.
3. `Detector`: Manages a collection of `BDReader`s which collectively read all the data corresponding to a single detector.
4. `DataSource`: A small wrapper class which provides easy access to creating `Detector` objects through its `detector` function.

These classes are defined in each implementation in their respective namespaces. E.g. you have `XTCPP::Base::DataSource` held in the `src/common` folder. The concrete implementation is in `XTCPP::MPI::DataSource` for an MPI-aware version.

## TODO List

In priority order the current features and improvements to work on are:

1. Finish calibration constants selection and make `calibrate` function general (only works on jungfrau).
2. "Live mode" support to allow reading XTC2 files as they are written.
3. Scans - support the DAQ scans.
4. epics - `PvaDetector` should be semi-supported; however, `epicsArch` is not.
5. Finish the HDF5 writing implementation.

