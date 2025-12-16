#include "mpi/datasource.hh"

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
  std::cerr << "Usage: " << progname << " -e <experiment> -r <run> [-n <fetch_events>] [-p]" << std::endl
            << std::endl
            << R"a(
Run some test reads of the "scan" detector.

Can also be run with MPI
  - `mpirun -np <NUM PROCS> ...
    (You can use `-mca osc ^ucx` if you see UCX errors - they can be ignored though)

Or can change the number of open OpenMP threads by setting the environment variable `OMP_NUM_THREADS`.
 - Set to 1 to run single threaded (`OMP_NUM_THREADS=1 [mpirun] ...`)

Args:
  -e <experiment>  Experiment to process
  -r    <run>      Run number to process
 [-n <fetch_evts>] Number of offsets to read per fetch of .smd.xtc2 file.
 [-p]              Optionally print out the values of the PV.
 [-h]              Display this help message.)a";
}

int main(int argc, char* argv[]) {
  int c;
  int parse_errors = 0;
  size_t events_per_read{1000};

  bool print{false};
  std::string experiment;
  std::string run;
  while ((c = getopt(argc, argv, "he:n:r:p")) != -1) {
    switch (c) {
    case 'h':
      usage(argv[0]);
      exit(0);
    case 'e':
      experiment = optarg;
      break;
    case 'n':
      events_per_read = static_cast<size_t>(std::atoi(optarg));
      break;
    case 'r':
      run = optarg;
      break;
    case 'p':
      print = true;
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

    XTCPP::MPI::DataSource ds(experiment, run, events_per_read);

    std::string scan_detector_name {"scan"};
    auto scan_det = ds.detector(scan_detector_name);
    std::chrono::time_point<std::chrono::steady_clock> load_end_time =
      std::chrono::steady_clock::now();

    std::cout << "Scan detector has algorithms and fields: ";
    for (auto& [alg_name, fields] : scan_det->alg_fields()) {
      std::cout << std::endl << " - " << alg_name;
      for (auto field : fields) {
        std::cout << std::endl << "   - " << field.name << " (Rank: " << field.rank
                  << ", Type: " << field.data_type << ")";
      }
    }
    std::cout << std::endl;

    std::chrono::time_point<std::chrono::steady_clock> start_time = std::chrono::steady_clock::now();
    int n_events{0};

    for (auto it=ds.begin(); it != ds.end(); it++) {
      auto raw_det  = scan_det->get_scan_data(*it, "raw", "step_value");
      if (print) {
        if (raw_det) {
          std::cout << "Value is: " << *reinterpret_cast<int64_t*>(raw_det) << std::endl;
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
