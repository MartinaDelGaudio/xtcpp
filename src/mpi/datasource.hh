#ifndef XTCPP_MPI_DATASOURCE_HH
#define XTCPP_MPI_DATASOURCE_HH

#include "common/datasource.hh"
#include "common/detector.hh"
#include "detector.hh"
#include "bd_reader.hh"

#include "xtcdata/xtc/Dgram.hh"

#include <cstddef>
#include <iterator>
#include <map>
#include <memory>
#include <numeric>
#include <string>
#include <variant>
#include <vector>

namespace XTCPP {
  namespace MPI {
    class DataSource final : public Base::DataSource {
    public:
      DataSource(std::string exp, std::variant<std::string, int> run, size_t events_per_read);

      std::shared_ptr<Base::Detector> detector(std::string detname) override;

      int rank() const { return m_rank; }

      int size() const { return m_n_ranks; }

    protected:
      MPI_Comm m_comm;
      int m_rank;
      int m_n_ranks;

      MPI_Win m_idx_window;
      MPI_Aint* m_curr_idx{nullptr};

      size_t fetch_next_idx() override;
      void init_detectors() override;
    };
  } // namespace MPI
} // namespace XTCPP

#endif // XTCPP_MPI_DATASOURCE_HH
