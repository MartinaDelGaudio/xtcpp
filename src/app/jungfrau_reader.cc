#include "mpi/datasource.hh"
#include "hdf5/mpiwriter.hh"
#include "hdf5/hdf5writer.hh"

#include "mpi.h"

#include <stdlib.h>
#include <unistd.h>

#include <any>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <stdfloat>
#include <string>
#include <vector>

namespace fs = std::filesystem;

void usage(char* progname)
{
  std::cerr << "Usage: " << progname << " -e <experiment> -r <run> [-n <fetch_events>] [-c] [-s] [-t]" << std::endl
            << std::endl
            << R"a(
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
 [-h]             Display this help message.)a";
}

int main(int argc, char* argv[]) {
  int c;
  int parse_errors = 0;
  size_t events_per_read{1000};

  size_t total_events{0};
  bool run_calib{false};
  bool test_smd{false};
  std::string experiment;
  std::string run;
  while ((c = getopt(argc, argv, "hce:n:r:st")) != -1) {
    switch (c) {
    case 'h':
      usage(argv[0]);
      exit(0);
    case 'c':
      run_calib = true;
      break;
    case 'e':
      experiment = optarg;
      break;
    case 'n':
      events_per_read = static_cast<size_t>(std::atoi(optarg));
      break;
    case 'r':
      run = optarg;
      break;
    case 's':
      test_smd = true;
      break;
    case 't':
      total_events = static_cast<size_t>(std::atoi(optarg));
      break;
    default:
      parse_errors++;
    }
  }

  if (experiment.empty() || run.empty()) {
    usage(argv[0]);
    std::exit(0);
  }

  std::cout << "Loading " << events_per_read << " offsets at a time." << std::endl;
  MPI_Init(&argc, &argv);

  {
    using namespace std::literals;
    std::chrono::time_point<std::chrono::steady_clock> load_start_time =
      std::chrono::steady_clock::now();

    using SmallData = XTCPP::MPI::HDF5Writer;
    XTCPP::MPI::DataSource ds(experiment, run, events_per_read);
    SmallData small_data(MPI_COMM_WORLD);
    small_data.open_file();

    auto epix100 = ds.detector("epix100_0");
    auto jungfrau = ds.detector("jungfrau");
    std::chrono::time_point<std::chrono::steady_clock> load_end_time =
      std::chrono::steady_clock::now();

    std::chrono::time_point<std::chrono::steady_clock> start_time = std::chrono::steady_clock::now();
    int n_events{0};

    for (auto it=ds.begin(); it != ds.end(); it++) {
      //std::cout << "Event offset index: " << *it << " [Rank: " << ds.rank() << "]" << std::endl;
      //auto dg_epix100 = epix100(*it);
      //auto dg_jungfrau = jungfrau(*it);
      [[maybe_unused]] auto raw_epix100  = epix100->get_data(*it,"raw","raw");
      auto raw_jungfrau = jungfrau->get_data(*it,"raw","raw");

      if (run_calib && raw_jungfrau) {
	//auto calib_jungfrau = XTCPP::calibrate(jungfrau->data_ptrs(), jungfrau->calibconst_span());
	XTCPP::calibrate(jungfrau->data_ptrs(),
			 jungfrau->calibconst_span(),
			 jungfrau->calib_data_buf());

	if (test_smd && n_events % 1 == 0) {
	  //std::vector<std::float32_t> dat_to_write(calib_jungfrau.begin(),
	  //					   calib_jungfrau.begin() + 512*1024);
	  std::vector<std::float32_t> dat_to_write(jungfrau->calib_data_buf().begin(),
	  					   jungfrau->calib_data_buf().begin() + 512*1024);
	  std::map<std::string,std::vector<size_t>> shape;
	  shape["/jungfrau/test"] = {1,512,1024};
	  std::map<std::string,std::any> evt_data;
	  evt_data["/jungfrau/test"]=dat_to_write;
	  small_data.event(evt_data, shape);
	}
        if (total_events && static_cast<size_t>(ds.size()*n_events) >= total_events) {
          break;
        }
      }
      n_events++;
    }
    std::chrono::time_point<std::chrono::steady_clock> end_time = std::chrono::steady_clock::now();
    const auto run_time = end_time - start_time;
    auto rate_ms = static_cast<double>(n_events)/static_cast<double>(run_time/1ms);

    const auto load_time = load_end_time - load_start_time;
    std::cout << "[Rank " << ds.rank() << "] " << run_time/1ms << " ms to iterate "
              << n_events << " events. (" << rate_ms << " events/ms) - Loading took: "
              << load_time/1ms << " ms" << std::endl;
  }
  MPI_Finalize();
}
