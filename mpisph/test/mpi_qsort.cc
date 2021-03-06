#include "gtest/gtest.h"

#include <cmath>
#include <iostream>
#include <log.h>
#include <mpi.h>

#include "bodies_system.h"

using namespace ::testing;

// Rule for body equals == same position
inline bool
operator==(const body & b1, const body & b2) {
  return b1.coordinates() == b2.coordinates();
};

namespace flecsi {
namespace execution {
void
driver(int argc, char * argv[]) {}
} // namespace execution
} // namespace flecsi

TEST(tree_colorer, mpi_qsort) {
  MPI_Init(nullptr, nullptr);
  int rank;
  int size;
  MPI_Comm_size(MPI_COMM_WORLD, &size);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  srand(time(NULL) * rank);
  log_set_output_rank(0);

  // Generating the particles randomly on each process
  int64_t nparticles = 10000;
  int64_t nparticlesperproc = nparticles / size;
  double maxbound = 1.0; // Particles positions between [0,1]
  // Adjust for last one
  if(rank == size - 1) {
    nparticlesperproc = (nparticles - nparticlesperproc * (size - 1));
  }
  log_one(info) << "Generating " << nparticles << std::endl;

  std::cout << "Rank " << rank << ": " << nparticlesperproc << " particles"
            << std::endl;

  // Range to compute the keys
  std::array<point_t, 2> range;
  range[0] = point_t{};
  range[1] = point_t{maxbound, maxbound, maxbound};
  std::vector<body> bodies(nparticlesperproc);
  // Create the bodies and keys
  for(size_t i = 0; i < nparticlesperproc; ++i) {
    // Random x, y and z
    bodies[i].set_coordinates(
      point_t{(double)rand() / (double)RAND_MAX * (maxbound),
        (double)rand() / (double)RAND_MAX * (maxbound),
        (double)rand() / (double)RAND_MAX * (maxbound)});

    // Compute the key
    bodies[i].set_key(key_type(range, bodies[i].coordinates()));
  }

  // Gather all the particles everywhere and sort locally
  std::vector<body> checking(nparticles);
  MPI_Allgather(&bodies[0], nparticlesperproc * sizeof(body), MPI_BYTE,
    &checking[0], nparticlesperproc * sizeof(body), MPI_BYTE, MPI_COMM_WORLD);

  // Sort it locally base on the keys
  std::sort(checking.begin(), checking.end(),
    [](auto & left, auto & right) { return left.key() < right.key(); });

  // Extract the subset of this process
  std::vector<body> my_checking(checking.begin() + rank * (nparticles / size),
    checking.begin() + rank * nparticlesperproc + nparticlesperproc);

  int * dist = new int[size];
  dist[rank] = bodies.size();
  MPI_Allgather(MPI_IN_PLACE, 1, MPI_INT, dist, 1, MPI_INT, MPI_COMM_WORLD);

  psort::psort(
    bodies,
    [](auto & left, auto & right) {
      if(left.key() < right.key()) {
        return true;
      }
      if(left.key() == right.key()) {
        return left.id() < right.id();
      }
      return false;
    },
    dist);

  // Compare the results with all processes particles subset
  ASSERT_TRUE(my_checking == bodies);
  MPI_Finalize();
}
