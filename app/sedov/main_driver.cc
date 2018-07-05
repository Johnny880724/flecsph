/*~--------------------------------------------------------------------------~*
 * Copyright (c) 2017 Los Alamos National Security, LLC
 * All rights reserved.
 *~--------------------------------------------------------------------------~*/

 /*~--------------------------------------------------------------------------~*
 *
 * /@@@@@@@@  @@           @@@@@@   @@@@@@@@ @@@@@@@  @@      @@
 * /@@/////  /@@          @@////@@ @@////// /@@////@@/@@     /@@
 * /@@       /@@  @@@@@  @@    // /@@       /@@   /@@/@@     /@@
 * /@@@@@@@  /@@ @@///@@/@@       /@@@@@@@@@/@@@@@@@ /@@@@@@@@@@
 * /@@////   /@@/@@@@@@@/@@       ////////@@/@@////  /@@//////@@
 * /@@       /@@/@@//// //@@    @@       /@@/@@      /@@     /@@
 * /@@       @@@//@@@@@@ //@@@@@@  @@@@@@@@ /@@      /@@     /@@
 * //       ///  //////   //////  ////////  //       //      //
 *
 *~--------------------------------------------------------------------------~*/

/**
 * @file main_driver.cc
 * @author Julien Loiseau
 * @date April 2017
 * @brief Specialization and Main driver used in FleCSI.
 * The Specialization Driver is normally used to register data and the main
 * code is in the Driver.
 */

#include <iostream>
#include <numeric> // For accumulate
#include <iostream>

#include <mpi.h>
#include <legion.h>
#include <omp.h>

#include "flecsi/execution/execution.h"
#include "flecsi/data/data_client.h"
#include "flecsi/data/data.h"

#include "params.h"
#include <bodies_system.h>

#include "default_physics.h"
#include "analysis.h"

#define OUTPUT_ANALYSIS

static std::string initial_data_file;  // = initial_data_prefix  + ".h5part"
static std::string output_h5data_file; // = output_h5data_prefix + ".h5part"

void set_derived_params() {
  using namespace param;

  // filenames (this will change for multiple files output)
  std::ostringstream oss;
  oss << initial_data_prefix << ".h5part";
  initial_data_file = oss.str();
  oss << output_h5data_prefix << ".h5part";
  output_h5data_file = oss.str();

  // iteration and time
  physics::iteration = initial_iteration;
  physics::totaltime = initial_time;
  physics::dt = initial_dt; // TODO: use particle separation and Courant factor
}

namespace flecsi{
namespace execution{

void
mpi_init_task(const char * parameter_file){
  using namespace param;

  int rank;
  int size;
  MPI_Comm_size(MPI_COMM_WORLD,&size);
  MPI_Comm_rank(MPI_COMM_WORLD,&rank);
  clog_set_output_rank(0);

  // set simulation parameters
  param::mpi_read_params(parameter_file);
  set_derived_params();

  // remove output file
  remove(output_h5data_file.c_str());

  // read input file
  body_system<double,gdimension> bs;
  bs.read_bodies(initial_data_file.c_str(),initial_iteration);

  // boundaries
  auto range_boundaries = bs.getRange();
  point_t distance = range_boundaries[1]-range_boundaries[0];
  for(int i = 0; i < gdimension; ++i){
    distance[i] = fabs(distance[i]);
  }
  double h = bs.getSmoothinglength();
  physics::min_boundary = {(0.1+2*h)*distance+range_boundaries[0]};
  physics::max_boundary = {-(0.1-2*h)*distance+range_boundaries[1]};
  clog_one(info) << "Limits: " << physics::min_boundary << " ; "
         << physics::max_boundary << std::endl;

#ifdef OUTPUT
  bs.write_bodies(output_h5data_prefix,physics::iteration);
#endif

  double stopt, startt;

  ++physics::iteration;
  do
  {
    analysis::screen_output();
    MPI_Barrier(MPI_COMM_WORLD);

    // Compute and prepare the tree for this iteration
    // - Compute the Max smoothing length
    // - Compute the range of the system using the smoothinglength
    // - Cmopute the keys
    // - Distributed qsort and sharing
    // - Generate and feed the tree
    // - Exchange branches for smoothing length
    // - Compute and exchange ghosts in real smoothing length
    bs.update_iteration();

    clog_one(trace) << "compute_density_pressure_soundspeed" << std::flush;
    bs.apply_in_smoothinglength(physics::compute_density_pressure_soundspeed);
    clog_one(trace) << ".done" << std::endl;

    // Refresh the neighbors within the smoothing length
    bs.update_neighbors();

    clog_one(trace) << "Hydro acceleration" << std::flush;
    bs.apply_in_smoothinglength(physics::compute_hydro_acceleration);
    clog_one(trace) << ".done" << std::endl;

    clog_one(trace) << "Internalenergy"<<std::flush;
    bs.apply_in_smoothinglength(physics::compute_dudt);
    clog_one(trace) << ".done" << std::endl;

    if(physics::iteration == initial_iteration + 1){
      clog_one(trace) << "leapfrog" << std::flush;
      bs.apply_all(physics::leapfrog_integration_first_step);
      clog_one(trace) << ".done" << std::endl;
    }else{
      if(rank==0)
      clog_one(trace) << "leapfrog" << std::flush;
      bs.apply_all(physics::leapfrog_integration);
      clog_one(trace) << ".done" << std::endl;
    }

    clog_one(trace) << "dudt integration" << std::flush;
    bs.apply_all(physics::dudt_integration);
    clog_one(trace) << ".done" << std::endl;

#ifdef OUTPUT_ANALYSIS
    // Compute the analysis values based on physics
    bs.get_all(analysis::compute_lin_momentum);
    bs.get_all(analysis::compute_total_mass);
    // Only add the header in the first iteration
    analysis::scalar_output("scalar_reductions.dat");
#endif

#ifdef OUTPUT
    if(out_h5data_every > 0 && physics::iteration % out_h5data_every == 0){
      startt = omp_get_wtime();
      bs.write_bodies(output_h5data_prefix,physics::iteration/out_h5data_every);
      stopt = omp_get_wtime();
      clog_one(trace) << "Output time: " << omp_get_wtime()-startt << "s"
                      << std::endl;
    }
    MPI_Barrier(MPI_COMM_WORLD);
#endif
    ++physics::iteration;
    physics::totaltime += physics::dt;

  } while(physics::iteration <= final_iteration);
}

flecsi_register_mpi_task(mpi_init_task);

void 
usage() {
  clog_one(warn) << "Usage: ./sedov <parameter-file.par>"
                 << std::endl << std::flush;
}

void
specialization_tlt_init(int argc, char * argv[]){
  clog_one(trace) << "In user specialization_driver" << std::endl;

  // check options list: exactly one option is allowed
  if (argc != 2) {
    clog_one(error) << "ERROR: parameter file not specified!" << std::endl;
    usage();
    return;
  }

  flecsi_execute_mpi_task(mpi_init_task, argv[1]);

} // specialization driver

void
driver(int argc,  char * argv[]){
  clog_one(trace) << "In user driver" << std::endl;
} // driver


} // namespace execution
} // namespace flesci


