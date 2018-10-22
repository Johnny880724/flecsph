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
 * @file physics.h
 * @author Julien Loiseau
 * @date April 2017
 * @brief Basic physics implementation
 */

#ifndef _default_physics_h_
#define _default_physics_h_

#include <vector>

#include "params.h"
#include "eos.h"
#include "utils.h"
#include "kernels.h"
#include "tree.h"
#include "eforce.h"

namespace physics{
  using namespace param;

  /**
   * Default values
   * They are usually modified in the main_driver 
   * to be application-specific. 
   * \TODO add a parameter file to be read in the main_driver
   */
  point_t max_boundary = {};
  point_t min_boundary = {};
  double dt = 0.0;
  double damp = 1;
  double totaltime = 0.0;
  double A = 1.0;
  double MAC = 1.;
  int64_t iteration = 0;

  /**
   * @brief      Compute the density 
   * Based on Fryer/05 eq(10)
   * @param      srch  The source's body holder
   * @param      nbsh  The neighbors' body holders
   */
  void 
  compute_density(
      body_holder* srch, 
      std::vector<body_holder*>& nbsh)
  {
    body* source = srch->getBody();
    double density = 0;
    mpi_assert(nbsh.size()>0);
    for(auto nbh : nbsh){
      body* nb = nbh->getBody();
      mpi_assert(nb != nullptr);
      double dist = flecsi::distance(source->getPosition(),nb->getPosition());
      mpi_assert(dist>=0.0);
      double kernelresult = kernels::kernel(dist,
            .5*(source->getSmoothinglength()+nb->getSmoothinglength()));
      density += kernelresult*nb->getMass();
    } // for
    mpi_assert(density>0);
    source->setDensity(density);
  } // compute_density


  /**
   * @brief      Calculates total energy for every particle
   * @param      srch  The source's body holder
   */
  void set_total_energy (body_holder* srch) { 
    body* source = srch->getBody();
    const point_t pos = source->getPosition(),
                  vel = source->getVelocity();
    const double eint = source->getInternalenergy(),
                 epot = external_force::potential(srch);
    double ekin = vel[0]*vel[0];
    for (unsigned short i=1; i<gdimension; ++i)
      ekin += vel[i]*vel[i];
    ekin *= .5;
    source->setTotalenergy(eint + epot + ekin);
  } // set_total_energy


  /**
   * @brief      Subtracts mechanical energy from total energy 
   *             to recover internal energy
   * @param      srch  The source's body holder
   */
  void recover_internal_energy (body_holder* srch) { 
    body* source = srch->getBody();
    const point_t pos = source->getPosition(),
                  vel = source->getVelocity();
    const double etot = source->getTotalenergy(),
                 epot = external_force::potential(srch);
    double ekin = vel[0]*vel[0];
    for (unsigned short i=1; i<gdimension; ++i)
      ekin += vel[i]*vel[i];
    ekin *= .5;
    const double eint = etot - ekin - epot;
    if (eint < 0.0) {
      std::cerr << "ERROR: internal energy is negative!" << std::endl
                << "particle id: " << source->getId()    << std::endl
                << "total energy: " << etot              << std::endl
                << "kinetic energy: " << ekin            << std::endl
                << "particle position: " << pos          << std::endl;
      mpi_assert(false);
    }
    source->setInternalenergy(eint);
  } // recover_internal_energy


  /**
   * @brief      Compute the density, EOS and spundspeed in the same function 
   * reduce time to gather the neighbors
   *
   * @param      srch  The source's body holder
   * @param      nbsh  The neighbors' body holders
   */
  void 
  compute_density_pressure_soundspeed(
    body_holder* srch, 
    std::vector<body_holder*>& nbsh)
  {
    compute_density(srch,nbsh);
    if (thermokinetic_formulation)
      recover_internal_energy(srch);
    eos::compute_pressure(srch);
    eos::compute_soundspeed(srch); 
  }


  /**
   * @brief      mu_ij for the artificial viscosity 
   * From Rosswog'09 (arXiv:0903.5075) - 
   * Astrophysical Smoothed Particle Hydrodynamics, eq.(60) 
   *
   * @param      srch     The source particle
   * @param      nbsh     The neighbor particle
   *
   * @return     Contribution for mu_ij of this neighbor
   *
   * @uses       epsilon  global parameter
   */
  double 
  mu(
      body* source, 
      body* nb)
  {  
    using namespace param;
    double result = 0.0;
    double h_ij = .5*(source->getSmoothinglength()+nb->getSmoothinglength()); 
    space_vector_t vecVelocity = flecsi::point_to_vector(
        source->getVelocityhalf() - nb->getVelocityhalf());
    space_vector_t vecPosition = flecsi::point_to_vector(
        source->getPosition() - nb->getPosition());
    double dotproduct = flecsi::dot(vecVelocity,vecPosition);

    if(dotproduct >= 0.0)
      return result;
    double dist = flecsi::distance(source->getPosition(),nb->getPosition());
    result = h_ij*dotproduct / (dist*dist + sph_viscosity_epsilon*h_ij*h_ij);
    
    mpi_assert(result < 0.0);
    return result; 
  } // mu


  /**
   * @brief      Artificial viscosity term, Pi_ab
   * From Rosswog'09 (arXiv:0903.5075) - 
   * Astrophysical Smoothed Particle Hydrodynamics, eq.(59) 
   *
   * @param      srch  The source particle
   * @param      nbsh  The neighbor particle
   *
   * @return     The artificial viscosity contribution 
   */
  double 
  viscosity(
    body* source, 
    body* nb)
  {
    using namespace param;
    double rho_ij = (1./2.)*(source->getDensity()+nb->getDensity());
    double c_ij = (1./2.)*
        (source->getSoundspeed()+nb->getSoundspeed());
    double mu_ij = mu(source,nb);
    if (adaptive_timestep) { // cache max_b mu_ab
      const double mumax = source->getMumax();
      if (mu_ij > mumax) 
        source->setMumax(mu_ij);
    }
    double res = ( -sph_viscosity_alpha*c_ij*mu_ij
                  + sph_viscosity_beta*mu_ij*mu_ij)/rho_ij;
    mpi_assert(res>=0.0);
    return res;
  }


  /**
   * @brief      Calculates the hydro acceleration
   * From CES-Seminar 13/14 - Smoothed Particle Hydrodynamics 
   *
   * @param      srch  The source's body holder
   * @param      nbsh  The neighbors' body holders
   */
  void 
  compute_hydro_acceleration(
    body_holder* srch, 
    std::vector<body_holder*>& ngbsh)
  { 
    using namespace param;
    body* source = srch->getBody();

    // Reset the accelerastion 
    // \TODO add a function to reset in main_driver
    point_t acceleration = {};
    point_t hydro = {};
    source->setMumax(0.0);

    for(auto nbh : ngbsh){ 
      body* nb = nbh->getBody();

      if(nb->getPosition() == source->getPosition())
        continue;

      // Compute viscosity
      double visc = viscosity(source,nb);
      
      // Hydro force
      point_t vecPosition = source->getPosition() - nb->getPosition();
      double rho_a = source->getDensity();
      double rho_b = nb->getDensity();
      double pressureDensity 
          = source->getPressure()/(rho_a*rho_a) 
          + nb->getPressure()/(rho_b*rho_b);

      // Kernel computation
      point_t sourcekernelgradient = kernels::gradKernel(
          vecPosition,
          .5*(source->getSmoothinglength()+nb->getSmoothinglength()));
      point_t resultkernelgradient = sourcekernelgradient;

      hydro += nb->getMass()*(pressureDensity + visc)
        *resultkernelgradient;

    }
    hydro = -1.0*hydro;
    acceleration += hydro;
    acceleration += external_force::acceleration(srch);
    source->setAcceleration(acceleration);
  } // compute_hydro_acceleration


  /**
   * @brief      Calculates the dudt, time derivative of internal energy.
   * From CES-Seminar 13/14 - Smoothed Particle Hydrodynamics 
   *
   * @param      srch  The source's body holder
   * @param      nbsh  The neighbors' body holders
   */
  void compute_dudt(body_holder* srch, std::vector<body_holder*>& ngbsh) {
    body* source = srch->getBody();

    double dudt = 0;
    double dudt_pressure = 0.;
    double dudt_visc = 0.;

    for(auto nbh: ngbsh){
      body* nb = nbh->getBody();

      if(nb->getPosition() == source->getPosition()){
        continue;
      }

      // Artificial viscosity
      double visc = viscosity(source,nb);
    
      // Compute the gradKernel ij      
      point_t vecPosition = source->getPosition()-nb->getPosition();
      point_t sourcekernelgradient = kernels::gradKernel(
          vecPosition,
          .5*(source->getSmoothinglength()+nb->getSmoothinglength()));
      space_vector_t resultkernelgradient = 
          flecsi::point_to_vector(sourcekernelgradient);

      // Velocity vector 
      space_vector_t vecVelocity = flecsi::point_to_vector(
          source->getVelocity() - nb->getVelocity());

      dudt_pressure += nb->getMass()*
        flecsi::dot(vecVelocity,resultkernelgradient);
      dudt_visc += visc*nb->getMass()*
        flecsi::dot(vecVelocity,resultkernelgradient);
    }
    
    double P_a = source->getPressure();
    double rho_a = source->getDensity();
    dudt = P_a/(rho_a*rho_a)*dudt_pressure + .5*dudt_visc;

    //Do not change internal energy during relaxation
    if(do_drag && iteration <= relax_steps){
       dudt = 0.0;
    }

    source->setDudt(dudt);
  } // compute_dudt


  /**
   * @brief      Calculates the dedt, time derivative of either 
   *             thermokinetic (internal + kinetic) or total 
   *             (internal + kinetic + potential) energy.
   * See e.g. Rosswog (2009) "Astrophysical SPH" eq. (34) 
   *
   * @param      srch  The source's body holder
   * @param      nbsh  The neighbors' body holders
   */
  void compute_dedt(body_holder* srch, std::vector<body_holder*>& ngbsh) {
    body* source = srch->getBody();

    double dedt = 0;

    const point_t pos_a = source->getPosition(),
                  vel_a = source->getVelocity();
    const double h_a = source->getSmoothinglength(),
                 P_a = source->getPressure(),
                 rho_a = source->getDensity();
    
    const double Prho2_a = P_a/(rho_a*rho_a);

    for(auto nbh: ngbsh){
      body* nb = nbh->getBody();
      const double h_b = nb->getSmoothinglength();
      const point_t pos_b = nb->getPosition();
      if(pos_a == pos_b)
        continue;

      // Compute the \nabla_a W_ab      
      const point_t Da_Wab = kernels::gradKernel(pos_a - pos_b,.5*(h_a+h_b)),
                    vel_b = nb->getVelocity();
    
      // va*DaWab and vb*DaWab
      double va_dot_DaWab = vel_a[0]*Da_Wab[0];
      double vb_dot_DaWab = vel_b[0]*Da_Wab[0];
      for (unsigned short i=1; i<gdimension; ++i) {
        va_dot_DaWab += vel_a[i]*Da_Wab[i],
        vb_dot_DaWab += vel_b[i]*Da_Wab[i];
      }

      const double m_b = nb->getMass(),
                   P_b = nb->getPressure(),
                   rho_b = nb->getDensity();
      const double Prho2_b = P_b/(rho_b*rho_b),
                   Pi_ab = viscosity(source,nb);

      // add this neighbour's contribution
      dedt -= m_b*( Prho2_a*vb_dot_DaWab + Prho2_b*va_dot_DaWab 
                + .5*Pi_ab*(va_dot_DaWab + vb_dot_DaWab));
    }
    
    source->setDudt(dedt);
  } // compute_dedt


  /**
   * @brief      Compute the adiabatic index for the particles 
   *
   * @param      srch   The source's body holder
   * @param      ngbsh  The neighbors' body holders
   */
  void 
  compute_dadt(
    body_holder* srch, 
    std::vector<body_holder*>& ngbsh)
  { 
    using namespace param;
    body* source = srch->getBody();
    
    // Compute the adiabatic factor here 
    double dadt = 0;
    
    for(auto nbh : ngbsh){ 
      body* nb = nbh->getBody();

      if(nb->getPosition() == source->getPosition()){
        continue;
      }

      // Artificial viscosity
      double density_ij = (1./2.)*(source->getDensity()+nb->getDensity());
      double soundspeed_ij = (1./2.)*
        (source->getSoundspeed()+nb->getSoundspeed());
      double mu_ij = mu(source,nb);
      double viscosity = ( -sph_viscosity_alpha*mu_ij*soundspeed_ij
                          + sph_viscosity_beta*mu_ij*mu_ij)/density_ij;
      mpi_assert(viscosity>=0.0);

      point_t vecPosition = source->getPosition()-nb->getPosition();
      point_t sourcekernelgradient = kernels::gradKernel(
          vecPosition,
          .5*(source->getSmoothinglength()+nb->getSmoothinglength()));
      point_t resultkernelgradient = sourcekernelgradient;

      
      // Compute the adiabatic factor evolution 
      dadt += nb->getMass() * viscosity * 
        flecsi::dot(
          flecsi::point_to_vector(source->getVelocity()-nb->getVelocity()),
          flecsi::point_to_vector(resultkernelgradient)
        );
    }
    
    dadt *= (poly_gamma - 1) / 
            (2*pow(source->getDensity(),poly_gamma-1));
    source->setDadt(dadt);
 

  } // compute_hydro_acceleration

  /**
   * @brief      Integrate the internal energy variation, update internal energy
   *
   * @param      srch  The source's body holder
   */
  void dadt_integration(
      body_holder* srch)
  {
    body* source = srch->getBody(); 
    source->setAdiabatic(
      source->getAdiabatic()+dt*source->getDadt());
  }


  /**
   * @brief      Apply boundaries if they are set
   *
   * @param      srch  The source's body holder
   *
   * @return     True if the particle have been considered outside the 
   * boundaries
   */
  bool
  compute_boundaries(
      body_holder* srch)
  {
    body* source = srch->getBody();
    point_t velocity = source->getVelocity();
    point_t position = source->getPosition();
    point_t velocityHalf = source->getVelocityhalf();

    bool considered = false;

    if(stop_boundaries){
      bool stop = false; 
      for(size_t i = 0; i < gdimension; ++i){
        if(position[i] < min_boundary[i] ||
          position[i] > max_boundary[i]){
          stop = true; 
        }
      }
      if(stop){
        velocity = point_t{};
        velocityHalf = point_t{};
        considered = true;
      
      }
    }else if(reflect_boundaries){
      for(size_t dim=0;dim < gdimension ; ++dim){
        if(position[dim] < min_boundary[dim] || 
            position[dim] > max_boundary[dim]){
          double barrier = max_boundary[dim];
          if(position[dim] < min_boundary[dim]){
            barrier = min_boundary[dim];
          }

          // Here just invert the velocity vector and velocityHalf 
          double tbounce = (position[dim]-barrier)/velocity[dim];
          position -= velocity*(1-damp)*tbounce;

          position[dim] = 2*barrier-position[dim];
          velocity[dim] = -velocity[dim];
          velocityHalf[dim] = -velocityHalf[dim];

          velocity *= damp;
          velocityHalf *= damp;
          considered = true;
        }
      }
    }
    source->setPosition(position);
    source->setVelocity(velocity);
    source->setVelocityhalf(velocityHalf);
    return considered;
  }

  /**
   * @brief Leapfrog integration, first step 
   *        TODO: deprecate; new Leapfrog should be implemented with
   *              the kick-drift-kick formulae
   *
   * @param srch  The source's body holder
   */
  void 
  leapfrog_integration_first_step(
      body_holder* srch)
  {
    body* source = srch->getBody();

    // If wall, reset velocity and dont move 
    if(source->is_wall()){
      source->setVelocity(point_t{});
      source->setVelocityhalf(point_t{}); 
      return; 
    }

    point_t velocityHalf = source->getVelocity() + 
        dt/2.*source->getAcceleration();
    point_t position = source->getPosition()+velocityHalf*dt;
    point_t velocity = 1./2.*(source->getVelocityhalf()+velocityHalf);

    if(do_boundaries){
      if(physics::compute_boundaries(srch)){
        return;
      }
    }

    source->setVelocityhalf(velocityHalf);
    source->setVelocity(velocity);
    source->setPosition(position);

    mpi_assert(!std::isnan(position[0])); 
  }

  /**
   * @brief Leapfrog integration
   *        TODO: deprecate; new Leapfrog should be implemented with
   *              the kick-drift-kick formulae
   *
   * @param srch  The source's body holder
   */
  void 
  leapfrog_integration(
      body_holder* srch)
  {
    body* source = srch->getBody();
    
    // If wall, reset velocity and dont move 
    if(source->is_wall()){
      source->setVelocity(point_t{});
      source->setVelocityhalf(point_t{}); 
      return; 
    }
    
    point_t velocityHalf = source->getVelocityhalf() + 
        dt*source->getAcceleration();
    point_t position = source->getPosition()+velocityHalf*dt;
    point_t velocity = 1./2.*(source->getVelocityhalf()+velocityHalf);

    if(do_boundaries){
      if(physics::compute_boundaries(srch)){
        return;
      }
    }

    source->setVelocityhalf(velocityHalf);
    source->setVelocity(velocity);
    source->setPosition(position);
    
    mpi_assert(!std::isnan(position[0])); 
  }


  /*******************************************************/
  /**
   * @brief      v -> v12
   *
   * @param      srch  The source's body holder
   */
  void 
  save_velocityhalf (body_holder* srch) {
    body* source = srch->getBody();
    source->setVelocityhalf(source->getVelocity());
  }

  /**
   * @brief      Leapfrog: kick velocity
   *             v^{n+1/2} = v^{n} + (dv/dt)^n * dt/2
   *             or
   *             v^{n+1} = v^{n+1/2} + (dv/dt)^n * dt/2
   *
   * @param      srch  The source's body holder
   */
  void 
  leapfrog_kick_v (body_holder* srch) {
    body* source = srch->getBody();
    source->setVelocity(source->getVelocity()
               + 0.5*dt*source->getAcceleration());
  }


  /**
   * @brief      Leapfrog: kick internal energy
   *             u^{n+1/2} = u^{n} + (du/dt)^n * dt/2
   *             or
   *             u^{n+1} = u^{n+1/2} + (du/dt)^n * dt/2
   *
   * @param      srch  The source's body holder
   */
  void 
  leapfrog_kick_u (body_holder* srch) {
    body* source = srch->getBody();
    source->setInternalenergy(source->getInternalenergy()
                     + 0.5*dt*source->getDudt());
  }


  /**
   * @brief      Leapfrog: kick thermokinetic or total energy
   *             e^{n+1/2} = e^{n} + (de/dt)^n * dt/2
   *             or
   *             e^{n+1} = e^{n+1/2} + (de/dt)^n * dt/2
   *
   * @param      srch  The source's body holder
   */
  void 
  leapfrog_kick_e (body_holder* srch) {
    body* source = srch->getBody();
    source->setTotalenergy(source->getTotalenergy()
                     + 0.5*dt*source->getDedt());
  }


  /**
   * @brief      Leapfrog: drift
   *             r^{n+1} = r^{n} + v^{n+1/2} * dt
   *
   * @param      srch  The source's body holder
   */
  void 
  leapfrog_drift (body_holder* srch) {
    body* source = srch->getBody();
    source->setPosition(source->getPosition()
                   + dt*source->getVelocity());
  }


  /**
   * @brief      Compute the timestep from acceleration and mu 
   * From CES-Seminar 13/14 - Smoothed Particle Hydrodynamics 
   *
   * @param      srch   The source's body holder
   */
  void compute_dt(body_holder* srch) {
    body* source = srch->getBody();
    const double tiny = 1e-24;
    const double mc   = 0.6; // constant in denominator for viscosity
    
    // particles separation around this particle
    const double dx = source->getSmoothinglength() 
                    / (sph_eta*kernels::kernel_width);
    
    // timestep based on particle velocity
    const double vel = norm_point(source->getVelocity());
    const double dt_v = dx/(vel + tiny);

    // timestep based on acceleration
    const double acc = norm_point(source->getAcceleration());
    const double dt_a = sqrt(dx/(acc + tiny));
  
    // timestep based on sound speed and viscosity 
    const double max_mu_ab = source->getMumax();
    const double cs_a = source->getSoundspeed();
    const double dt_c = dx/ (tiny + cs_a*(1 + mc*sph_viscosity_alpha) 
                                  + mc*sph_viscosity_beta*max_mu_ab);

    // critical OMP to avoid outside synchronizations
    double dtmin = timestep_cfl_factor * std::min(std::min(dt_v,dt_a), dt_c);
    source->setDt(dtmin);
  }

  /**
   * @brief      Reduce adaptive timestep and set its value
   *
   * @param      bodies   Set of bodies
   */
  void set_adaptive_timestep(std::vector<body_holder*>& bodies) {
    double dtmin = 1e24; // some ludicrous number
    for(auto nbh: bodies) {
      dtmin = std::min(dtmin, nbh->getBody()->getDt());
    }
    mpi_utils::reduce_min(dtmin);
  
  #pragma omp critical
    if (dtmin < physics::dt)
      physics::dt = std::min(dtmin, physics::dt/2.0);

    if (dtmin > 2.0*physics::dt)
      physics::dt = physics::dt*2.0;
  }

  void compute_smoothinglength( std::vector<body_holder*>& bodies)
  {
    for(auto b: bodies) {
      b->getBody()->setSmoothinglength(
          sph_eta*kernels::kernel_width*
          pow(b->getBody()->getMass()/b->getBody()->getDensity(),
          1./(double)gdimension));
    }
  }

  /**
   * @brief update smoothing length for particles (Rosswog'09, eq.51)
   * 
   * ha = eta/N \sum_b pow(m_b / rho_b,1/dimension)
   */
  void compute_average_smoothinglength( std::vector<body_holder*>& bodies,
      int64_t nparticles) {
    compute_smoothinglength(bodies);
    // Compute the total 
    double total = 0.;
    for(auto b: bodies)
    {
      total += b->getBody()->getSmoothinglength();
    }
    // Add up with all the processes 
    MPI_Allreduce(MPI_IN_PLACE,&total,1,MPI_DOUBLE,MPI_SUM,MPI_COMM_WORLD);
    // Compute the new smoothing length 
    double new_h = 1./(double)nparticles * total;
    for(auto b: bodies) { 
      b->getBody()->setSmoothinglength(new_h);
    }
  }
  



}; // physics

#endif // _default_physics_h_
