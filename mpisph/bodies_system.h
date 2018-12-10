/*~--------------------------------------------------------------------------~*
 * Copyright (c) 2017 Los Alamos National Security, LLC
 * All rights reserved.
 *~--------------------------------------------------------------------------~*/

/**
 * @file bodies_system.h
 * @author Julien Loiseau
 * @brief Class and function to handle the system of bodies/particles.
 * Contain the function for user, hidding the IO/distribution and tree search.
 */

#ifndef _mpisph_body_system_h_
#define _mpisph_body_system_h_

#include "tree_colorer.h"
#include "tree_fmm.h"
#include "io.h"
#include "utils.h"
#include "params.h"

#include <omp.h>
#include <iostream>
#include <fstream>
#include <typeinfo>

using namespace mpi_utils;

/**
 * @brief      The bodies/particles system.
 * This is a wrapper for a simpler use from users.
 *
 * @tparam     T     The type of data, usualy double
 * @tparam     D     The dimension of the current simulation
 */
template<
  typename T,
  size_t D
  >
class body_system{

using point_t = flecsi::point__<T,D>;

public:

  /**
   * @brief      Constructs the object.
   */
  body_system():totalnbodies_(0L),localnbodies_(0L),macangle_(0.0),
  maxmasscell_(1.0e-40),tree_(nullptr)
  {
    int rank = 0;
    MPI_Comm_rank(MPI_COMM_WORLD,&rank);
    // Display the number of threads in DEBUG mode

    #pragma omp parallel
    #pragma omp master
    rank || clog(warn)<<"USING OMP THREADS: "<<
      omp_get_num_threads()<<std::endl;

    if(param::sph_variable_h){
      rank || clog(warn) <<"Variable smoothing length ENABLE"<<std::endl;
    }
  };

  /**
   * @brief      Destroys the object.
   */
  ~body_system(){
    if(tree_ != nullptr){
      delete tree_;
    }
  };

  //

  /**
   * @brief      Max mass to stop the tree search during
   * the gravitation computation with FMM
   *
   * @param[in]  maxmasscell  The maximum mass for the cells
   */
  void setMaxmasscell(
    double maxmasscell)
  {
    maxmasscell_ = maxmasscell;
  };

  /**
   * @brief      Sets the Multipole Acceptance Criterion for FMM
   *
   * @param[in]  macangle  Multipole Acceptance Criterion
   */
  void
  setMacangle(
    double macangle)
  {
    macangle_ = macangle;
  };

  /**
   * @brief      Read the bodies from H5part file Compute also the total to
   *             check for mass lost
   *
   * @param[in]  filename        The filename
   * @param[in]  startiteration  The iteration from which load the data
   */
  void
  read_bodies(
      const char * filename,
      const char * output_filename,
      const int startiteration)
  {

    io::inputDataHDF5(localbodies_,filename,output_filename,
        totalnbodies_,localnbodies_,startiteration);
  }

  /**
   * @brief      Write bodies to file in parallel Caution provide the
   * file name
   *             prefix, h5part will be added This is useful in case
   *             of multiple
   *             files output
   *
   * @param[in]  filename       The outut file prefix
   * @param[in]  iter           The iteration of output
   * @param[in]  do_diff_files  Generate a file for each steps
   */
  void
  write_bodies(
      const char * filename,
      int iter,
      double totaltime)
  {
    io::outputDataHDF5(localbodies_,filename,iter,totaltime);
  }


  /**
   * @brief      Compute the largest smoothing length in the system This is
   *             really useful for particles with differents smoothing length
   *
   * @return     The largest smoothinglength of the system.
   */
  double
  getSmoothinglength()
  {
    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD,&rank);
    MPI_Comm_size(MPI_COMM_WORLD,&size);

    // Choose the smoothing length to be the biggest from everyone
    smoothinglength_ = 0;
#pragma omp parallel for reduction(max:smoothinglength_)
    for(size_t i = 0 ; i < localbodies_.size(); ++i){
      if(smoothinglength_ < localbodies_[i].radius()){
        smoothinglength_ = localbodies_[i].radius();
      }
    }

    MPI_Allreduce(MPI_IN_PLACE,&smoothinglength_,1,MPI_DOUBLE,MPI_MAX,
        MPI_COMM_WORLD);

    return smoothinglength_;

  }

  /**
   * @brief      Compute the range of thw whole particle system
   *
   * @return     The range.
   */
  std::array<point_t,2>&
  getRange()
  {
    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD,&rank);
    MPI_Comm_size(MPI_COMM_WORLD,&size);

    tcolorer_.mpi_compute_range(localbodies_,range_);
    return range_;
  }

  /**
   * @brief      Generate and share the particle for this iteration
   * @details    This part if decomposed with:
   *    - Compute and prepare the tree for this iteration
   *    - Compute the Max smoothing length
   *    - Compute the range of the system using the smoothinglength
   *    - Cmopute the keys
   *    - Distributed qsort and sharing
   *    - Generate and feed the tree
   *    - Exchange branches for smoothing length
   *    - Compute and exchange ghosts in real smoothing length
   */
  void
  update_iteration()
  {
    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD,&rank);
    MPI_Comm_size(MPI_COMM_WORLD,&size);
    std::ostringstream oss;

    // Destroy the previous tree
    if(tree_ !=  nullptr){
      delete tree_;
    }

    if(param::periodic_boundary_x || param::periodic_boundary_y ||
      param::periodic_boundary_z)
      boundary::pboundary_clean(localbodies_);

    // Choose the smoothing length to be the biggest from everyone
    smoothinglength_ = getSmoothinglength();

    if(param::periodic_boundary_x || param::periodic_boundary_y ||
      param::periodic_boundary_z){
      boundary::pboundary_generate(localbodies_,2.5*smoothinglength_);
      localnbodies_ = localbodies_.size();
      MPI_Allreduce(&localnbodies_,&totalnbodies_,1,MPI_INT64_T,MPI_SUM,
        MPI_COMM_WORLD);
    }


   // Then compute the range of the system
    tcolorer_.mpi_compute_range(localbodies_,range_);
    assert(range_[0] != range_[1]);
    rank || clog(trace) << "Range="<<range_[0]<<";"<<range_[1]<<std::endl;

    // Generate the tree based on the range
    tree_ = new tree_topology_t(range_[0],range_[1]);

    // Compute the keys
//#pragma omp parallel for
    for(size_t i = 0; i < localbodies_.size(); ++i){
      localbodies_[i].set_key(
          entity_key_t(tree_->range(),localbodies_[i].coordinates()));
    }

    tcolorer_.mpi_qsort(localbodies_,totalnbodies_);

#ifdef OUTPUT_TREE_INFO
    rank || clog(trace) << "Construction of the tree";
#endif

// Sort the bodies
#ifdef BOOST_PARALLEL
    boost::sort::block_indirect_sort(
#else
    std::sort(
#endif
      localbodies_.begin(),localbodies_.end(),
        [](auto& left, auto &right){
          if(left.key()<right.key()){
            return true;
          }
          if(left.key() == right.key()){
            return left.id()<right.id();
          }
          return false;
    }); // sort

    // Add my local bodies in my tree
    // Clear the bodies_ vector
    //bodies_.clear();
    for(auto& bi:  localbodies_){
      auto id = tree_->make_entity(bi.key(),bi.coordinates(),
        &(bi),rank,bi.mass(),bi.id(),bi.radius());
      tree_->insert(id);
      auto nbi = tree_->get(id);
      //bodies_.push_back(nbi);
      assert(nbi->global_id() == bi.id());
      assert(nbi->getBody() != nullptr);
      assert(nbi->is_local());
    }
    localnbodies_ = localbodies_.size();

    #ifdef OUTPUT_TREE_INFO
        rank || clog(trace) << ".done"<<std::endl;
    #endif

if(!(param::periodic_boundary_x || param::periodic_boundary_y ||
  param::periodic_boundary_z))
{
#ifdef DEBUG
    // Check the total number of bodies
    int64_t checknparticles = tree_->entities().size();
    MPI_Allreduce(MPI_IN_PLACE,&checknparticles,1,MPI_INT64_T,
    MPI_SUM,MPI_COMM_WORLD);
    assert(checknparticles==totalnbodies_);
#endif
}

#ifdef OUTPUT_TREE_INFO
    rank || clog(trace) << "Computing branches"<<std::endl;
#endif

    tree_->post_order_traversal(tree_->root(),
        traversal_t::update_COM,epsilon_,false);
    assert(tree_->root()->sub_entities() == localnbodies_);

#ifdef OUTPUT_TREE_INFO
    std::vector<int> nentities(size);
    int lentities = tree_->root()->sub_entities();
    // Get on 0
    MPI_Gather(
      &lentities,
      1,
      MPI_INT,
      &nentities[0],
      1,
      MPI_INT,
      0,
      MPI_COMM_WORLD
      );

    oss << rank << " sub_entities before=";
    for(auto v: nentities){
      oss << v << ";";
    }
    oss << std::endl;
    rank|| clog(trace) << oss.str() << std::flush;

    oss.str("");
    oss.clear();
#endif

    // Exchnage usefull body_holder from my tree to other processes
    tcolorer_.mpi_branches_exchange(*tree_,localbodies_,rangeposproc_,
        range_);

    // update the tree
    tree_->post_order_traversal(tree_->root(),
        traversal_t::update_COM,epsilon_,false);

#ifdef OUTPUT_TREE_INFO
    lentities = tree_->root()->sub_entities();
    // Get on 0
    MPI_Gather(
      &lentities,
      1,
      MPI_INT,
      &nentities[0],
      1,
      MPI_INT,
      0,
      MPI_COMM_WORLD
      );

    oss << rank << " sub_entities after=";
    for(auto v: nentities){
      oss << v << ";";
    }
    oss << std::endl;
    rank|| clog(trace) << oss.str() << std::flush;
#endif

    tcolorer_.mpi_compute_ghosts(*tree_);
    tcolorer_.mpi_refresh_ghosts(*tree_);

#ifdef OUTPUT_TREE_INFO
    // Tree informations
    rank || clog(trace) << *tree_ << " root range = "<< tree_->root()->bmin()
     <<";"<<tree_->root()->bmax()<< std::endl;
#endif

  }

  /**
   * @brief      Update the neighbors that have beem compute in update_iteration
   * This function use buffer pre-computed to update the data faster.
   */
  void update_neighbors()
  {
    tcolorer_.mpi_refresh_ghosts(*tree_);
  }

  /**
   * @brief      Compute the gravition interction between all the particles
   * @details    The function is based on Fast Multipole Method. The functions
   *             are defined in the file tree_fmm.h
   */
  void
  gravitation_fmm()
  {
    int rank, size;
    MPI_Comm_size(MPI_COMM_WORLD,&size);
    MPI_Comm_rank(MPI_COMM_WORLD,&rank);

    rank|| clog(trace)<<"FMM: mmass="<<maxmasscell_<<" angle="<<macangle_<<std::endl;

    // Just consider the local particles in the tree for FMM
    tree_->post_order_traversal(tree_->root(),
        traversal_t::update_COM,epsilon_,true);


    //tree_->update_branches(smoothinglength_,true);
    assert((int64_t)tree_->root()->sub_entities() == localnbodies_);

    tfmm_.mpi_exchange_cells(*tree_,maxmasscell_);
    tfmm_.mpi_compute_fmm(*tree_,macangle_,0);
    tfmm_.mpi_gather_cells(*tree_,macangle_,totalnbodies_);


    tree_->post_order_traversal(tree_->root(),
        traversal_t::update_COM,epsilon_,false);
  }

  /**
   * @brief      Apply the function EF with ARGS in the smoothing length of all
   *             the lcoal particles. This function need a previous call to
   *             update_iteration and update_neighbors for the remote particles'
   *             data.
   *
   * @param[in]  ef    The function to apply in the smoothing length
   * @param[in]  args  Arguments of the physics function applied in the
   *                   smoothing length
   *
   * @tparam     EF         The function to apply in the smoothing length
   * @tparam     ARGS       Arguments of the physics function applied in the
   *                        smoothing length
   */
  template<
    typename EF,
    typename... ARGS
  >
  void apply_in_smoothinglength(
      EF&& ef,
      ARGS&&... args)
  {
    int64_t ncritical = 32;
    tree_->apply_sub_cells(
        tree_->root(),
        0.,
        ncritical,
        param::sph_variable_h,
        ef,
        std::forward<ARGS>(args)...);
  }

  /**
   * @brief      Apply a function to all the particles.
   *
   * @param[in]  <unnamed>  { parameter_description }
   * @param[in]  <unnamed>  { parameter_description }
   *
   * @tparam     EF         The function to apply to all particles
   * @tparam     ARGS       Arguments of the function for all particles
   */
  template<
    typename EF,
    typename... ARGS
  >
  void apply_all(
      EF&& ef,
      ARGS&&... args)
  {
    int64_t nelem = tree_->entities().size();
    #pragma omp parallel for
    for(int64_t i=0; i<nelem; ++i){
        auto ent = tree_->get(i);
        if(ent->is_local())
          ef(ent,std::forward<ARGS>(args)...);
    }
  }

  /**
   * @brief      Apply a function on the vector of local bodies
   *
   * @param[in]  <unnamed>  { parameter_description }
   * @param[in]  <unnamed>  { parameter_description }
   *
   * @tparam     EF         The function to apply to the vector
   * @tparam     ARGS       Arguments of the function to apply to the vector
   */
  template<
    typename EF,
    typename... ARGS
  >
  void get_all(
    EF&& ef,
    ARGS&&... args)
  {
    ef(tree_->entities(),std::forward<ARGS>(args)...);
  }


  /**
   * @brief      Test function using the n^2 algorithm testing
   *
   * @param[in]  <unnamed>  The function to apply
   * @param[in]  <unnamed>  The arguments of the function
   *
   * @tparam     EF         The function to apply
   * @tparam     ARGS       The arguments of the function
   */
  template<
    typename EF,
    typename... ARGS
  >
  void apply_square(
    EF&& ef,
    ARGS&&... args)
  {
    int64_t nelem = tree_->entities().size();
    #pragma omp parallel for
    for(int64_t i = 0 ; i < nelem; ++i){
      ef(tree_->get(i),tree_->entities(),std::forward<ARGS>(args)...);
    }
  }

  /**
   * @brief      Gets a vector of the local bodies of this process.
   *
   * @return     The localbodies.
   */
  std::vector<body>&
    getLocalbodies()
  {
    return localbodies_;
  };

  /**
   * @ brief return the number of local bodies
   */
  int64_t getNLocalBodies()
  {
    return localnbodies_;
  }

  int64_t getNBodies()
  {
    return totalnbodies_;
  }

  tree_topology_t* tree() const
  {
    return tree_;
  }

private:
  int64_t totalnbodies_;        // Total number of local particles
  int64_t localnbodies_;        // Local number of particles
  double macangle_;             // Macangle for FMM
  double maxmasscell_;          // Mass criterion for FMM
  std::vector<body> localbodies_;
  range_t range_;
  std::vector<range_t> rangeposproc_;
  tree_colorer<T,D> tcolorer_;
  tree_fmm<T,D> tfmm_;        // tree_fmm.h function for FMM
  tree_topology_t* tree_;     // The particle tree data structure
  double smoothinglength_;    // Keep track of the biggest smoothing length
  double epsilon_ = 0.;
};

#endif
