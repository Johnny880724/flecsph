/*~--------------------------------------------------------------------------~*
 * Copyright (c) 2017 Triad National Security, LLC
 * All rights reserved.
 *~--------------------------------------------------------------------------~*/

#include <algorithm>
#include <cassert>
#include <iostream>
#include <math.h>

#include "io.h"
#include "kernels.h"
#include "lattice.h"
#include "params.h"
#include "sodtube.h"
#include "user.h"

using namespace io;
//
// help message
//
void
print_usage() {
  log_one(warn) << "Initial data generator for KH test in" << gdimension << "D"
                << std::endl
                << "Usage: ./fluid_XD_generator <parameter-file.par>"
                << std::endl;
}

//
// derived parameters
//
static double pressure_0; // Initial pressure
static int64_t nparticlesproc; // number of particles per proc
static double rho_1; // densities
static double vx_1; // velocities
static double pressure_1; // pressures
static std::string initial_data_file; // = initial_data_prefix + ".h5part"

// geometric extents of the two regions: top and bottom
static point_t box_min, box_max;

static int64_t np = 0; // number of particles in the top block
static double sph_sep_t = 0; // particle separation in top or bottom blocks
static double mass = 0; // particle mass in the middle block

double
pressure_gravity(const double & y, const double & rho) {
  using namespace param;
  return pressure_0 - rho * gravity_acceleration_constant * y;
}

double
u_from_eos(const double & rho, const double & p) {
  return p / ((param::poly_gamma - 1.0) * rho);
}

void
set_derived_params() {
  using namespace std;
  using namespace param;

  // boundary tolerance factor
  const double b_tol = 1e-8;

  box_max[0] = -box_length / 4.;
  box_min[0] = -box_length / 2.;

  box_min[1] = -box_width / 2.;
  box_max[1] = 0.;

  if(gdimension == 3) {
    box_max[2] = box_height / 2.;
    box_min[2] = -box_height / 2.;
  }

  pressure_0 = 2.5;

  // 1 = bottom 2 = top
  rho_1 = rho_initial; // 2.0 by default

  // file to be generated
  std::ostringstream oss;
  oss << initial_data_prefix << ".h5part";
  initial_data_file = oss.str();

  std::cout << "Box: " << std::endl << box_min << "-" << box_max << std::endl;

  // select particle lattice and kernel function
  particle_lattice::select();
  kernels::select();

  double totalmass = 0.;

  // particle mass and spacing
  SET_PARAM(
    sph_separation, (box_length * (1.0 - b_tol) / (double)(lattice_nx - 1)));
  if(gdimension == 3) {
    totalmass = rho_1 * abs(box_max[0] - box_min[0]) *
                abs(box_min[1] - box_max[1]) * abs(box_min[2] - box_max[2]);
  }
  if(gdimension == 2) {
    totalmass =
      rho_1 * abs(box_max[0] - box_min[0]) * abs(box_min[1] - box_max[1]);
  }

  // count the number of particles
  np = particle_lattice::count(
    lattice_type, domain_type, box_min, box_max, sph_separation, 0);

  mass = totalmass / np;

  SET_PARAM(nparticles, np);
}

//----------------------------------------------------------------------------//
int
main(int argc, char * argv[]) {
  using namespace param;

  // launch MPI
  int rank, size;
  MPI_Init(&argc, &argv);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);
  log_set_output_rank(0);

  // check options list: exactly one option is allowed
  if(argc != 2) {
    print_usage();
    MPI_Finalize();
    exit(0);
  }

  // anything other than 2D is not implemented yet
  // assert (gdimension == 2);
  assert(domain_type == 0);

  // set simulation parameters
  param::mpi_read_params(argv[1]);
  set_derived_params();

  // screen output
  std::cout << "Kelvin-Helmholtz instability setup in " << gdimension
            << "D:" << std::endl
            << " - number of particles: " << nparticles << std::endl
            << " - generated initial data file: " << initial_data_file
            << std::endl;

  // allocate arrays
  // Position
  double * x = new double[nparticles]();
  double * y = new double[nparticles]();
  double * z = new double[nparticles]();
  // Velocity
  double * vx = new double[nparticles]();
  double * vy = new double[nparticles]();
  double * vz = new double[nparticles]();
  // Acceleration
  double * ax = new double[nparticles]();
  double * ay = new double[nparticles]();
  double * az = new double[nparticles]();
  // Smoothing length
  double * h = new double[nparticles]();
  // Density
  double * rho = new double[nparticles]();
  // Internal Energy
  double * u = new double[nparticles]();
  // Pressure
  double * P = new double[nparticles]();
  // Mass
  double * m = new double[nparticles]();
  // Id
  int64_t * id = new int64_t[nparticles]();
  // Timestep
  double * dt = new double[nparticles]();

  // generate the lattice
  auto _np = particle_lattice::generate(
    lattice_type, domain_type, box_min, box_max, sph_separation, 0, x, y, z);
  assert(np == _np);

  // max. value for the speed of sound
  double cs = sqrt(poly_gamma * pressure_1 / rho_1);

  // suggested timestep
  double timestep =
    timestep_cfl_factor * sph_separation / std::max(cs, flow_velocity);

  // particle id number
  int64_t posid = 0;
  for(int64_t part = 0; part < nparticles; ++part) {
    id[part] = posid++;
    rho[part] = rho_1;
    m[part] = mass;

    P[part] = pressure_gravity(y[part], rho[part]);
    u[part] = u_from_eos(rho[part], P[part]);

    vy[part] = 0.;
    // particle masses and smoothing length
    m[part] = mass;
    h[part] = sph_eta * kernels::kernel_width *
              pow(m[part] / rho[part], 1. / gdimension);

  } // for part=0..nparticles

  // delete the output file if exists
  remove(initial_data_file.c_str());

  hid_t dataFile = H5P_openFile(initial_data_file.c_str(), H5F_ACC_RDWR);

  int use_fixed_timestep = 1;
  // add the global attributes
  H5P_writeAttribute(dataFile, "nparticles", &nparticles);
  H5P_writeAttribute(dataFile, "timestep", &timestep);
  int dim = gdimension;
  H5P_writeAttribute(dataFile, "dimension", &dim);
  H5P_writeAttribute(dataFile, "use_fixed_timestep", &use_fixed_timestep);

  H5P_setNumParticles(nparticles);
  H5P_setStep(dataFile, 0);

  // H5PartSetNumParticles(dataFile,nparticles);
  H5P_writeDataset(dataFile, "x", x, nparticles);
  H5P_writeDataset(dataFile, "y", y, nparticles);
  H5P_writeDataset(dataFile, "z", z, nparticles);
  H5P_writeDataset(dataFile, "vx", vx, nparticles);
  H5P_writeDataset(dataFile, "vy", vy, nparticles);
  H5P_writeDataset(dataFile, "h", h, nparticles);
  H5P_writeDataset(dataFile, "rho", rho, nparticles);
  H5P_writeDataset(dataFile, "u", u, nparticles);
  H5P_writeDataset(dataFile, "P", P, nparticles);
  H5P_writeDataset(dataFile, "m", m, nparticles);
  H5P_writeDataset(dataFile, "id", id, nparticles);

  H5P_closeFile(dataFile);

  delete[] x, y, z, vx, vy, vz, ax, ay, az, h, rho, u, P, m, id, dt;

  MPI_Finalize();
  return 0;
}
