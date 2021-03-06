
#include "gtest/gtest.h"

#include <cmath>
#include <iostream>
#include <log.h>

#include <mpi.h>

namespace analysis {
enum e_conservation : size_t {
  MASS = 0,
  ENERGY = 1,
  MOMENTUM = 2,
  ANG_MOMENTUM = 3
};
}
using namespace analysis;

namespace flecsi {
namespace execution {
void mpi_init_task(const char * parameter_file);
bool check_conservation(const std::vector<e_conservation> &);
} // namespace execution
} // namespace flecsi

using namespace flecsi;
using namespace execution;

TEST(RT, working) {
  MPI_Init(nullptr, nullptr);
  mpi_init_task("RT_2d.par");
  ASSERT_TRUE(check_conservation({MASS, ENERGY, MOMENTUM}));
  MPI_Finalize();
}
