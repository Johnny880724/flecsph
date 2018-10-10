/*~--------------------------------------------------------------------------~*
 *  @@@@@@@@  @@           @@@@@@   @@@@@@@@ @@
 * /@@/////  /@@          @@////@@ @@////// /@@
 * /@@       /@@  @@@@@  @@    // /@@       /@@
 * /@@@@@@@  /@@ @@///@@/@@       /@@@@@@@@@/@@
 * /@@////   /@@/@@@@@@@/@@       ////////@@/@@
 * /@@       /@@/@@//// //@@    @@       /@@/@@
 * /@@       @@@//@@@@@@ //@@@@@@  @@@@@@@@ /@@
 * //       ///  //////   //////  ////////  //
 *
 * Copyright (c) 2016 Los Alamos National Laboratory, LLC
 * All rights reserved
 *~--------------------------------------------------------------------------~*/

#ifndef flecsi_topology_tree_topology_h
#define flecsi_topology_tree_topology_h

/*!
  \file tree_topology.h
  \authors nickm@lanl.gov
  \date Initial file creation: Apr 5, 2016
 */

/*
  Tree topology is a statically configured N-dimensional hashed tree for
  representing localized entities, e.g. particles. It stores entities in a
  configurable branch type. Inserting entities into a branch can cause that
  branch to be refined or coarsened correspondingly. A client of tree topology
  defines a policy which defines its branch and entity types and other
  compile-time parameters. Specializations can define a policy and default
  branch types which can then be specialized in a simpler fashion
  (see the basic_tree specialization).
*/

#include <map>
#include <unordered_map>
#include <vector>
#include <array>
#include <map>
#include <cmath>
#include <bitset>
#include <algorithm>
#include <cassert>
#include <iostream>
#include <set>
#include <functional>
#include <mutex>
#include <stack>
#include <math.h>
#include <float.h>

#include "flecsi/geometry/point.h"
#include "flecsi/concurrency/thread_pool.h"
#include "flecsi/data/storage.h"
#include "flecsi/data/data_client.h"
#include "flecsi/topology/index_space.h"

#include "morton_branch_id.h"
#include "tree_branch.h"
#include "tree_geometry.h"

/*
#define np(X)                                                            \
 std::cout << __FILE__ << ":" << __LINE__ << ": " << __PRETTY_FUNCTION__ \
           << ": " << #X << " = " << (X) << std::endl

#define hp(X)                                                            \
 std::cout << __FILE__ << ":" << __LINE__ << ": " << __PRETTY_FUNCTION__ \
           << ": " << #X << " = " << std::hex << (X) << std::endl
*/

namespace flecsi {
namespace topology {

  /*!
  All tree entities have an associated entity id of this type which is needed
  to interface with the index space.
 */
class entity_id_t{
public:
  entity_id_t()
  {}

  entity_id_t(
    const entity_id_t& id
  )
  : id_(id.id_)
  {}

  entity_id_t(
    size_t id
  )
  : id_(id)
  {}

  operator size_t() const
  {
    return id_;
  }

  entity_id_t&
  operator=(
    const entity_id_t& id
  )
  {
    id_ = id.id_;
    return *this;
  }

  size_t 
  value_()
  {
    return id_;
  }

  size_t
  index_space_index() const
  {
    return id_;
  }

private:
  size_t id_;
};

template<
  typename T,
  size_t D
>
std::ostream&
operator<<(
  std::ostream& ostr,
  const branch_id<T,D>& id
)
{
  id.output_(ostr);
  return ostr;
}

template<
  typename T,
  size_t D
>
struct branch_id_hasher__{
  size_t
  operator()(
    const branch_id<T, D>& k
  ) const
  {
    return std::hash<T>()(k.value_());
  }
};

/*!
  The tree topology is parameterized on a policy P which defines its branch and
  entity types.
 */
template<
  class P
>
class tree_topology : public P, public data::data_client_t
{
public:
  using Policy = P;

  static const size_t dimension = Policy::dimension;

  using element_t = typename Policy::element_t;

  using point_t = point__<element_t, dimension>;

  using range_t = std::pair<element_t, element_t>;

  using branch_int_t = typename Policy::branch_int_t;

  using branch_id_t = branch_id<branch_int_t, dimension>;

  using branch_id_vector_t = std::vector<branch_id_t>;


  using branch_t = typename Policy::branch_t;

  using branch_vector_t = std::vector<branch_t*>;


  using entity_t = typename Policy::entity_t;

  using entity_vector_t = std::vector<entity_t*>;

  using apply_function = std::function<void(branch_t&)>;

  using entity_id_vector_t = std::vector<entity_id_t>;

  using geometry_t = tree_geometry<element_t, dimension>;

  using entity_space_t = index_space__<entity_t*, true, true, false>;

  using branch_space_t = index_space__<branch_t*, true, true, false>;

  using subentity_space_t = index_space__<entity_t*, false, true, false>;

  struct filter_valid{
    bool operator()(entity_t* ent) const{
      return ent->is_valid();
    }
  };

  /*!
    Constuct a tree topology with unit coordinates, i.e. each coordinate
    dimension is in range [0, 1].
   */
  tree_topology()
  {
    branch_map_.emplace(branch_id_t::root(), branch_id_t::root());
    root_ = branch_map_.find(branch_id_t::root()); 
    assert(root_ != branch_map_.end());

    max_depth_ = 0;
    max_scale_ = element_t(1);

    for(size_t d = 0; d < dimension; ++d)
    {
      range_[0][d] = element_t(0);
      range_[1][d] = element_t(1);
      scale_[d] = element_t(1);
    }
  }

  /*!
    Construct a tree topology with specified ranges [end, start] for each
    dimension.
   */
  tree_topology(
    const point__<element_t, dimension>& start,
    const point__<element_t, dimension>& end
  )
  {
    branch_map_.emplace(branch_id_t::root(),branch_id_t::root());
    root_ = branch_map_.find(branch_id_t::root()); 
    assert(root_ != branch_map_.end());
   
    max_depth_ = 0;

    for(size_t d = 0; d < dimension; ++d)
    {
      scale_[d] = end[d] - start[d];
      max_scale_ = std::max(max_scale_, scale_[d]);
      range_[0][d] = start[d];
      range_[1][d] = end[d];
    }
  }

  /** 
   * @brief Destroy the tree: empty the hash-table and destroy the entities 
   * lists 
   */
  ~tree_topology()
  {
    branch_map_.clear();
    entities_.clear(); 
  } 

  /**
   * @brief Get the ci-th child of the given branch.
   */
  branch_t*
  child(
    branch_t* b,
    size_t ci
  )
  {
    // Use the hash table 
    branch_id_t bid = b->id(); // Branch id 
    bid.push(ci); // Add child number 
    auto child = branch_map_.find(bid); // Search for the child
    // If it does not exists, return nullptr 
    if(child == branch_map_.end())
    {
      return nullptr; 
    } 
    return &child->second;
  }

  /*!
    Return an index space containing all entities (including those removed).
   */
  auto
  all_entities() const
  {
    return entities_.template slice<>();
  }

  /*!
    Return an index space containing all non-removed entities.
   */
  auto
  entities()
  {
    return entities_.template cast<
      entity_t*, false, false, false, filter_valid>();
  }

  /*!
    Insert an entity into the lowest possible branch division.
   */
  void
  insert(
    entity_t* ent
  )
  {
    insert(ent, max_depth_);
  }
#if 0 
  /*!
    Update is called when an entity's coordinates have changed and may trigger
    a reinsertion.
   */
  void
  update(entity_t* ent)
  {
    branch_id_t bid = ent->get_branch_id();
    branch_id_t nid = to_branch_id(ent->coordinates(), bid.depth());

    if(bid == nid)
    {
      return;
    }

    remove(ent);
    insert(ent, max_depth_);
  }

  /*!
    Effectively re-insert all entities into the tree. Called when all entity
    coordinates are assumed to have changed.
   */
  void
  update_all()
  {
    root_->template dealloc_<branch_t>();
    max_depth_ = 0;
    branch_map_.clear();
    branch_map_.emplace(root_->id(), root_);

    for(auto ent : entities_)
    {
      ent->set_branch_id_(branch_id_t::null());
      insert(ent);
    }
  }

  /*!
    Effectively re-insert all entities into the tree. Called when all entity
    coordinates are assumed to have changed. Additionally expands or contracts
    the coordinate ranges of each dimension to [start, end].
   */
  void
  update_all(
    const point__<element_t, dimension>& start,
    const point__<element_t, dimension>& end
  )
  {

    for(size_t d = 0; d < dimension; ++d)
    {
      scale_[d] = end[d] - start[d];
      max_scale_ = std::max(max_scale_, scale_[d]);
      range_[0][d] = start[d];
      range_[1][d] = end[d];
    }

    root_->template dealloc_<branch_t>();
    max_depth_ = 0;
    branch_map_.clear();
    branch_map_.emplace(root_->id(), root_);

    for(auto ent : entities_)
    {
      ent->set_branch_id_(branch_id_t::null());
      insert(ent);
    }
  }

  /*!
    Remove an entity from the tree. Note this method does not actually
    delete it. This can trigger coarsening and refinements as determined
    by the tree topology policy.
   */
  void
  remove(
    entity_t* ent
  )
  {
    assert(!ent->get_branch_id().is_null());

    auto itr = branch_map_.find(ent->get_branch_id());
    assert(itr != branch_map_.end());
    branch_t* b = itr->second;

    b->remove(ent);
    ent->set_branch_id_(branch_id_t::null());

    switch(b->requested_action_())
    {
      case action::none:
        break;
      case action::coarsen:
      {
        // Find the parent directly in the hashtable 
        // auto p = find_parent(b);
        auto p = static_cast<branch_t*>(b->parent());
        if(p && Policy::should_coarsen(p))
        {
          coarsen_(p);
        }
        break;
      }
      case action::refine:
        b->reset();
        break;
      default:
        assert(false && "invalid action");
    }
  }


  /*!
    Convert a point to unit coordinates.
   */
  point_t
  unit_coordinates(
    const point_t& p
  )
  {
    point_t pn;

    for(size_t d = 0; d < dimension; ++d)
    {
      pn[d] = (p[d] - range_[0][d]) / scale_[d];
    }

    return pn;
  }
#endif 


  /*!
   * Update the branch boundaries
   * Go through all the branches in a DFS order
   */
  void
  update_branches(
      const element_t epsilon = element_t(0))
  {
    // Recursive version 
    std::function<void(branch_t*)> traverse;
    traverse = [&epsilon,&traverse,this](branch_t* b){
      element_t mass = element_t(0); 
      point_t bmax{}; 
      point_t bmin{};
      for(size_t d = 0; d < dimension; ++d)
      {
        bmax[d] = -DBL_MAX;
        bmin[d] = DBL_MAX; 
      }
      point_t coordinates = point_t{};
      uint64_t nchildren = 0; 
      if(b->is_leaf())
      {
        for(auto child: *b)
        {
          nchildren++;
          element_t childmass = child->getMass(); 
          
          for(size_t d = 0; d < dimension; ++d)
          {
            bmax[d] = std::max(bmax[d],child->coordinates()[d]+epsilon);
            bmin[d] = std::min(bmin[d],child->coordinates()[d]-epsilon);
            coordinates[d] += child->coordinates()[d]*childmass; 
          }
          mass += childmass;
        }
        if(mass > element_t(0))
        {
          for(size_t d = 0; d < dimension; ++d)
          {
            coordinates[d] /= mass; 
          }
        }
      }else{
        for(int i=0 ; i<(1<<dimension);++i)
        {
          auto branch = child(b,i); 
          traverse(branch);
          nchildren += branch->sub_entities();
          mass += branch->mass();
          if(branch->mass() > 0){
            for(size_t d = 0; d < dimension; ++d){
              bmax[d] = std::max(bmax[d],branch->bmax()[d]);
              bmin[d] = std::min(bmin[d],branch->bmin()[d]);
            }
          }
          for(size_t d = 0; d < dimension; ++d)
          {
            coordinates[d] += branch->get_coordinates()[d]*branch->mass(); 
          }
        }

        if(mass > element_t(0))
        {
          for(size_t d = 0; d < dimension; ++d)
          {
            coordinates[d] /= mass; 
          }
        }
      }
      // Save in both cases
      b->set_sub_entities(nchildren);
      b->set_coordinates(coordinates);
      b->set_mass(mass);
      b->set_bmin(bmin);
      b->set_bmax(bmax);  
    };
    traverse(root());
  }


  /**
   * @brief Update the COM data regarding the local bodies. 
   * Do not consider GHOSTS 
   * This function is useful to prepare the tree for local search 
   * It is currently used in the FMM method. 
   * 
   * @param epsilon The radius to add in the boundaries of COM
   */
  void
  update_branches_local(
      const element_t epsilon = element_t(0))
  {
    // Recursive version 
    std::function<void(branch_t*)> traverse;
    traverse = [&epsilon,&traverse,this](branch_t* b){
      element_t mass = element_t(0); 
      point_t bmax{}; 
      point_t bmin{};
      for(size_t d = 0; d < dimension; ++d)
      {
        bmax[d] = -DBL_MAX;
        bmin[d] = DBL_MAX; 
      }
      point_t coordinates{};
      uint64_t nchildren = 0; 
      if(b->is_leaf())
      {
        for(auto child: *b)
        {
          if(child->is_local()){
            nchildren++;
            element_t childmass = child->getMass(); 
          
            for(size_t d = 0; d < dimension; ++d)
            {
              bmax[d] = std::max(bmax[d],child->coordinates()[d]+epsilon);
              bmin[d] = std::min(bmin[d],child->coordinates()[d]-epsilon);
              coordinates[d] += child->coordinates()[d]*childmass; 
            }
            mass += childmass;
          }
        }
        if(mass > element_t(0))
        {
          for(size_t d = 0; d < dimension; ++d)
          {
            coordinates[d] /= mass; 
          }
        }
      }else{
        for(int i=0 ; i<(1<<dimension);++i)
        {
          auto branch = child(b,i); 
          traverse(branch);
          nchildren += branch->sub_entities();
          mass += branch->mass();

          if(branch->sub_entities() > 0){
            for(size_t d = 0; d < dimension; ++d){
              bmax[d] = std::max(bmax[d],branch->bmax()[d]);
              bmin[d] = std::min(bmin[d],branch->bmin()[d]);
            }
          }
          for(size_t d = 0; d < dimension; ++d)
          {
            coordinates[d] += branch->get_coordinates()[d]*branch->mass(); 
          }
        }

        if(mass > element_t(0))
        {
          for(size_t d = 0; d < dimension; ++d)
          {
            coordinates[d] /= mass; 
          }
        }
      }
      // Save in both cases leaf or not
      b->set_sub_entities(nchildren);
      b->set_coordinates(coordinates);
      b->set_mass(mass);
      b->set_bmin(bmin);
      b->set_bmax(bmax);  
    };
    traverse(root());
  }

  void
  get_all_branches(
    branch_t * start,
    std::vector<branch_t*>& search_list)
  {
    std::stack<branch_t*> stk;
    stk.push(start);
    search_list.push_back(start);
    
    while(!stk.empty()){
      branch_t* c = stk.top();
      stk.pop();
      if(c->is_leaf()){
        //search_list.push_back(c);
      }else{
        for(int i=0; i<(1<<dimension);++i){
          branch_t * next = child(c,i);
          if(next->sub_entities() > 0){
            search_list.push_back(next);
            stk.push(next);
          }
        }
      } 
    }
  }

  /**
   * @brief Return a vector with all the local sub entities
   * 
   * @param start The branch in which the search occur 
   * @param search_list The found entities vector
   */
  void
  get_sub_entities_local(
    branch_t * start,
    std::vector<entity_t*>& search_list)
  {
    std::stack<branch_t*> stk;
    stk.push(start);
    
    while(!stk.empty()){
      branch_t* c = stk.top();
      stk.pop();
      if(c->is_leaf()){
        for(auto bh: *c){
          if(bh->is_local()){
            search_list.push_back(bh);
          }
        }
      }else{
        for(int i=0; i<(1<<dimension);++i){
          branch_t * next = child(c,i);
          if(next->sub_entities() > 0){
            stk.push(next);
          }
        }
      } 
    }
  }

  void 
  find_sub_cells(
      branch_t * b,
      uint64_t criterion,
      std::vector<branch_t*>& search_list)
  {
    std::stack<branch_t*> stk;
    stk.push(b);
    
    while(!stk.empty()){
      branch_t* c = stk.top();
      stk.pop();
      if(c->is_leaf() && c->sub_entities() > 0){
        search_list.push_back(c);
      }else{
        if(c->sub_entities() <= criterion && c->sub_entities() > 0){
          search_list.push_back(c); 
        }else{
          for(int i=0; i<(1<<dimension);++i){
            branch_t * next = child(c,i);
            if(next->sub_entities() > 0){
              stk.push(next);
            }
          }
        }
      } 
    }
  }

  /**
   * @brief Find all the center of mass of the tree up to the 
   * maximum mass criterion.  
   * 
   * @param b The starting branch for the search, usually root
   * @param mass_criterion The maximum mass for the COMs
   * @param search_list The extracted COMs
   */
  void 
  find_sub_cells_mass(
      branch_t * b,
      double mass_criterion,
      std::vector<branch_t*>& search_list)
  {
    std::stack<branch_t*> stk;
    stk.push(b);
    
    while(!stk.empty()){
      branch_t* c = stk.top();
      stk.pop();
      if(c->is_leaf() && c->sub_entities() > 0){
        search_list.push_back(c);
      }else{
        if(c->mass() <= mass_criterion && c->sub_entities() > 0){
          search_list.push_back(c); 
        }else{
          for(int i=0; i<(1<<dimension);++i){
            branch_t * next = child(c,i);
            if(next->sub_entities() > 0){
              stk.push(next);
            }
          }
        }
      } 
    }
  }
  
  template<
    typename EF,
    typename... ARGS
  >
  void 
  apply_sub_cells(
      branch_t * b,
      element_t radius,
      element_t MAC,
      int64_t ncritical,
      EF&& ef,
      ARGS&&... args)
  {
    std::stack<branch_t*> stk;
    stk.push(b);
    
    #pragma omp parallel
    #pragma omp single
    while(!stk.empty()){
      branch_t* c = stk.top();
      stk.pop();
      if(c->is_leaf() && c->sub_entities() > 0){
        #pragma omp task firstprivate(c)
        {
          std::vector<branch_t*> inter_list; 
          sub_cells_inter(c,MAC,inter_list);
          force_calc(c,inter_list,radius,ef,std::forward<ARGS>(args)...);
        }
      }else{
        if((int64_t)c->sub_entities() < ncritical && c->sub_entities() > 0){
          #pragma omp task firstprivate(c)
          {
            std::vector<branch_t*> inter_list; 
            sub_cells_inter(c,MAC,inter_list);
            force_calc(c,inter_list,radius,ef,std::forward<ARGS>(args)...);
          } 
        }else{
          for(int i=0; i<(1<<dimension);++i){
            branch_t * next = child(c,i);
            if(next->sub_entities() > 0){
              stk.push(next);
            }
          }
        }
      } 
    }
    #pragma omp taskwait
  }

  void
  sub_cells_inter(
    branch_t* b,
    element_t MAC,
    std::vector<branch_t*>& inter_list)
  {
    std::stack<branch_t*> stk; 
    stk.push(root());
    //std::cout<<"sub_cells_inter: coord="<<b->get_coordinates()<<std::endl<<std::flush;
    while(!stk.empty()){
      branch_t* c = stk.top();
      stk.pop();
      if(c->is_leaf()){
        inter_list.push_back(c);
      }else{
        for(int i=0 ; i<(1<<dimension);++i){
          auto branch = child(c,i);
          if(branch->sub_entities() > 0 && geometry_t::intersects_box_box(
            b->bmin(),
            b->bmax(),
            branch->bmin(),
            branch->bmax()))
          {
            stk.push(branch);
          }
        }
      }
    }
  }

  template<
    typename EF,
    typename... ARGS
  >
  void
  force_calc(
      branch_t* b, 
      std::vector<branch_t*>& inter_list,
      element_t radius,
      EF&& ef,
      ARGS&&... args)
  {
    std::stack<branch_t*> stk;
    stk.push(b);
    while(!stk.empty())
    {
      branch_t* c = stk.top();
      stk.pop();
      if(c->is_leaf()){
        for(auto child: *c){
          if(child->is_local()){
            apply_sub_entity(child,inter_list,radius,ef,std::forward<ARGS>(args)...);
          }
        }
      }else{
        for(int i=0; i<(1<<dimension); ++i){
          branch_t * next = child(c,i);
          if(next->getMass() > 0.){
            stk.push(next);
          }
        }
      }
    }
  }

  template<
    typename EF,
    typename... ARGS
  >
  void 
  apply_sub_entity(
      entity_t* ent, 
      std::vector<branch_t*>& inter_list,
      element_t radius,
      EF&& ef,
      ARGS&&... args)
  {
    std::vector<entity_t*> neighbors;
    for(auto b: inter_list){
      for(auto nb: *b){
        if(geometry_t::within(
              ent->coordinates(),
              nb->coordinates(),
              radius))
        {
          neighbors.push_back(nb);
        }
      }
    }
    ef(ent,neighbors,std::forward<ARGS>(args)...);
  }

  /*!
    Return an index space containing all entities within the specified
    spheroid.
   */
  subentity_space_t
  find_in_radius_b(
    const point_t& center,
    element_t radius
  )
  {
    subentity_space_t ents;
    ents.set_master(entities_);

    // ITERATIVE VERSION
    std::stack<branch_t*> stk;
    stk.push(root());

    while(!stk.empty()){
      branch_t* b = stk.top();
      stk.pop();
      if(b->is_leaf()){
        for(auto child: *b){
            // Check if in radius 
            if(geometry_t::within(center,child->coordinates(),radius)){
              ents.push_back(child);
            }
        }
      }else{
        for(int i=0 ; i<(1<<dimension);++i){
          auto branch = child(b,i);
          if(geometry_t::intersects_sphere_box(
                branch->bmin(),
                branch->bmax(),
                center,
                radius
                ))
          {
            stk.push(branch);
          }
        }
      }
    }
    return ents;
  }


  /*!
    Return an index space containing all entities within the specified
    spheroid.
   */
  subentity_space_t
  find_in_radius(
    const point_t& center,
    element_t radius
  )
  {
    subentity_space_t ents;
    ents.set_master(entities_);

    auto ef =
    [&](entity_t* ent, const point_t& center, element_t radius) -> bool{
      return geometry_t::within(ent->coordinates(), center, radius);
    };

    size_t depth;
    element_t size;
    branch_t* b = find_start_(center, radius, depth, size);

    find_(b, size, ents, ef, geometry_t::intersects, center, radius);

    return ents;
  }

  /*!
    Return an index space containing all entities within the specified
    spheroid. (Concurrent version.)
   */
  subentity_space_t
  find_in_radius(
    thread_pool& pool,
    const point_t& center,
    element_t radius
  )
  {

    size_t queue_depth = get_queue_depth(pool);
    size_t m = branch_int_t(1) << queue_depth * P::dimension;

    auto ef =
    [&](entity_t* ent, const point_t& center, element_t radius) -> bool{
      return geometry_t::within(ent->coordinates(), center, radius);
    };

    virtual_semaphore sem(1 - int(m));
    std::mutex mtx;

    subentity_space_t ents;
    ents.set_master(entities_);

    size_t depth;
    element_t size;
    branch_t* b = find_start_(center, radius, depth, size);
    queue_depth += depth;

    find_(pool, sem, mtx, queue_depth, depth, b, size, ents, ef,
          geometry_t::intersects, center, radius);

    sem.acquire();

    return ents;
  }

/*!
    Return an index space containing all entities within the specified
    spheroid.
   */
  subentity_space_t
  find_in_box_b(
    const point_t& min,
    const point_t& max
  )
  {
    subentity_space_t ents;
    ents.set_master(entities_);

    // ITERATIVE VERSION
    std::stack<branch_t*> stk;
    stk.push(root());

    while(!stk.empty()){
      branch_t* b = stk.top();
        stk.pop();
        if(b->is_leaf()){
          for(auto child: *b){
              // Check if in box 
              if(geometry_t::within_box(child->coordinates(),min,max)){
                ents.push_back(child);
              }
          }
        }else{
          for(int i=0 ; i<(1<<dimension);++i){
            auto branch = child(b,i);
            if(geometry_t::intersects_box_box(
                  min,
                  max,
                  branch->bmin(),
                  branch->bmax()
                  ))
            {
              stk.push(branch);
            }
          }
        }
      }
      return ents;
    }



    /*!
      Return an index space containing all entities within the specified
      box.
     */
    subentity_space_t
    find_in_box(
      const point_t& min,
      const point_t& max
    )
    {
      subentity_space_t ents;
      ents.set_master(entities_);

      auto ef =
      [&](entity_t* ent, const point_t& min, const point_t& max) -> bool{
        return geometry_t::within_box(ent->coordinates(), min, max);
      };

      element_t radius = 0;
      for(size_t d = 0; d < dimension; ++d)
      {
        radius = std::max(radius, max[d] - min[d]);
      }

      element_t const c = std::sqrt(element_t(2))/element_t(2);
      radius *= c;

      point_t center = min;
      center += radius;

      size_t depth;
      element_t size;
      branch_t* b = find_start_(center, radius, depth, size);

      find_(b, size, ents, ef, geometry_t::intersects_box, min, max);

      return ents;
    }

    /*!
      Return an index space containing all entities within the specified
      box. (Concurrent version.)
     */
    subentity_space_t
    find_in_box(
      thread_pool& pool,
      const point_t& min,
      const point_t& max
    )
    {
      size_t queue_depth = get_queue_depth(pool);
      size_t m = branch_int_t(1) << queue_depth * P::dimension;

      auto ef =
      [&](entity_t* ent, const point_t& min, const point_t& max) -> bool{
        return geometry_t::within_box(ent->coordinates(), min, max);
      };

      element_t radius = 0;
      for(size_t d = 0; d < dimension; ++d)
      {
        radius = std::max(radius, max[d] - min[d]);
      }

      constexpr element_t c = std::sqrt(element_t(2))/element_t(2);
      radius *= c;

      point_t center = min;
      center += radius;

      size_t depth;
      element_t size;
      branch_t* b = find_start_(center, radius, depth, size);

      subentity_space_t ents;
      ents.set_master(entities_);

      queue_depth += depth;

      virtual_semaphore sem(1 - int(m));
      std::mutex mtx;

      find_(pool, sem, mtx, queue_depth, depth, b, size, ents, ef,
            geometry_t::intersects_box, min, max);

      sem.acquire();

      return ents;
    }

    /*!
      For all entities within the specified spheroid, apply the given callable
      object ef with args.
     */
    template<
      typename EF,
      typename... ARGS
    >
    void
    apply_in_radius(
      const point_t& center,
      element_t radius,
      EF&& ef,
      ARGS&&... args)
    {

      auto f = [&](entity_t* ent, const point_t& center, element_t radius)
      {
        if(geometry_t::within(ent->coordinates(), center, radius))
        {
          ef(ent, std::forward<ARGS>(args)...);
        }
      };

      size_t depth;
      element_t size;
      branch_t* b = find_start_(center, radius, depth, size);

      apply_(b, size, f, geometry_t::intersects, center, radius);
    }

    /*!
      For all entities within the specified spheroid, apply the given callable
      object ef with args. (Concurrent version.)
     */
    template<
      typename EF,
      typename... ARGS
    >
    void
    apply_in_radius(
      thread_pool& pool,
      const point_t& center,
      element_t radius,
      EF&& ef,
      ARGS&&... args)
    {

      size_t queue_depth = get_queue_depth(pool);
      size_t m = branch_int_t(1) << queue_depth * P::dimension;

      auto f = [&](entity_t* ent, const point_t& center, element_t radius)
      {
        if(geometry_t::within(ent->coordinates(), center, radius))
        {
          ef(ent, std::forward<ARGS>(args)...);
        }
      };

      size_t depth;
      element_t size;
      branch_t* b = find_start_(center, radius, depth, size);
      queue_depth += depth;

      virtual_semaphore sem(1 - int(m));

      apply_(pool, sem, queue_depth, depth, b, size,
             f, geometry_t::intersects, center, radius);

      sem.acquire();
    }

    /*!
      For all entities within the specified box, apply the given callable
      object ef with args.
     */
    template<
      typename EF,
      typename... ARGS
    >
    void
    apply_in_box(
      thread_pool& pool,
      const point_t& min,
      const point_t& max,
      EF&& ef,
      ARGS&&... args
    )
    {

      size_t queue_depth = get_queue_depth(pool);
      size_t m = branch_int_t(1) << queue_depth * P::dimension;

      auto f = [&](entity_t* ent, const point_t& min, const point_t& max)
      {
        if(geometry_t::within_box(ent->coordinates(), min, max))
        {
          ef(ent, std::forward<ARGS>(args)...);
        }
      };

      element_t radius = 0;
      for(size_t d = 0; d < dimension; ++d)
      {
        radius = std::max(radius, max[d] - min[d]);
      }

      constexpr element_t c = std::sqrt(element_t(2))/element_t(2);
      radius *= c;

      point_t center = min;
      center += radius;

      size_t depth;
      element_t size;
      branch_t* b = find_start_(center, radius, depth, size);
      queue_depth += depth;

      virtual_semaphore sem(1 - int(m));

      apply_(pool, sem, queue_depth, depth, b, size,
             f, geometry_t::intersects_box, min, max);

      sem.acquire();
    }

    /*!
      For all entities within the specified spheroid, apply the given callable
      object ef with args. (Concurrent version.)
     */
    template<
      typename EF,
      typename... ARGS
    >
    void
    apply_in_box(
      const point_t& min,
      const point_t& max,
      EF&& ef,
      ARGS&&... args
    )
    {
      auto f = [&](entity_t* ent, const point_t& min, const point_t& max)
      {
        if(geometry_t::within_box(ent->coordinates(), min, max))
        {
          ef(ent, std::forward<ARGS>(args)...);
        }
      };

      element_t radius = 0;
      for(size_t d = 0; d < dimension; ++d)
      {
        radius = std::max(radius, max[d] - min[d]);
      }

      constexpr element_t c = std::sqrt(element_t(2))/element_t(2);
      radius *= c;

      point_t center = min;
      center += radius;

      size_t depth;
      element_t size;
      branch_t* b = find_start_(center, radius, depth, size);

      apply_(b, size, f, geometry_t::intersects_box, min, max);
    }

    /*!
      Construct a new entity. The entity's constructor should not be called
      directly.
     */
    template<
      class... Args
    >
    entity_t*
    make_entity(
      Args&&... args
    )
    {
      auto ent = new entity_t(std::forward<Args>(args)...);
      entity_id_t id = entities_.size();
      ent->set_id_(id);
      entities_.push_back(ent);
      return ent;
    }

    /*!
      Return the tree's current max depth.
     */
    size_t
    max_depth() const
    {
      return max_depth_;
    }

    /*!
      Get an entity by entity id.
     */
    entity_t*
    get(
      entity_id_t id
    )
    {
      assert(id < entities_.size());
      return entities_[id];
    }

    branch_t*
    get(
      branch_id_t id
    )
    { 
      auto itr = branch_map_.find(id);
      assert(itr != branch_map_.end());
      return &itr->second;
    }

    /*!
      Get the root branch (depth 0).
     */
    branch_t*
    root()
    {
      return &root_->second;
    }

    /*!
      Visit and apply callable object f and args on all sub-branches of branch b.
     */
    template<
      typename F,
      typename... ARGS
    >
    void
    visit(
      branch_t* b,
      F&& f,
      ARGS&&... args
    )
    {
      visit_(b, 0, std::forward<F>(f), std::forward<ARGS>(args)...);
    }

    /*!
      Visit and apply callable object f and args on all sub-branches of branch b.
      (Concurrent version.)
     */
    template<
      typename F,
      typename... ARGS
    >
    void
    visit(
      thread_pool& pool,
      branch_t* b,
      F&& f,
      ARGS&&... args
    )
    {
      size_t queue_depth = get_queue_depth(pool);
      size_t m = branch_int_t(1) << queue_depth * P::dimension;

      virtual_semaphore sem(1 - int(m));

      visit_(pool, sem, b, 0, queue_depth,
             std::forward<F>(f), std::forward<ARGS>(args)...);

      sem.acquire();
    }

    /*!
      Visit and apply callable object f and args on all sub-entities of branch b.
     */
    template<
    typename F,
    typename... ARGS
    >
    void
    visit_children(
      branch_t* b,
      F&& f,
      ARGS&&... args
    )
    {
      if(b->is_leaf())
      {
        for(auto ent : *b)
        {
          f(ent, std::forward<ARGS>(args)...);
        }
        return;
      }

      for(size_t i = 0; i < branch_t::num_children; ++i)
      {
        branch_t* bi = b->template child_<branch_t>(i);
        visit_children(bi, std::forward<F>(f), std::forward<ARGS>(args)...);
      }
    }

    /*!
      Visit and apply callable object f and args on all sub-entities of branch b.
      (Concurrent version.)
     */
    template<
      typename F,
      typename... ARGS
    >
    void
    visit_children(
      thread_pool& pool,
      branch_t* b,
      F&& f,
      ARGS&&... args
    )
    {
      size_t queue_depth = get_queue_depth(pool);
      size_t m = branch_int_t(1) << queue_depth * P::dimension;

      virtual_semaphore sem(1 - int(m));

      visit_children_(pool, sem, 0, queue_depth, b,
                      std::forward<F>(f), std::forward<ARGS>(args)...);

      sem.acquire();
    }
#if 0 
    /*!
      Save (serialize) the tree to an archive.
     */
    template<
      typename A
    >
    void
    save(
      A & archive
    ) const
    {
      size_t size;
      char* data = serialize_(size);
      archive.saveBinary(&size, sizeof(size));

      archive.saveBinary(data, size);
         
      delete [] data;
    } // save

    /*!
      Load (de-serialize) the tree from an archive.
     */
    template<
      typename A
    >
    void
    load(
      A & archive
    )
    {
      size_t size;
      archive.loadBinary(&size, sizeof(size));

      char* data = new char [size];
      archive.loadBinary(data, size);
      unserialize_(data);
      delete [] data;
    } // load

    char*
    serialize_(
      uint64_t& size
    )
    {
      uint64_t num_entities = entities_.size();

      const size_t alloc_size =
        sizeof(num_entities) + num_entities * sizeof(branch_id_t);

      size = alloc_size;

      char* buf = new char [alloc_size];
      uint64_t pos = 0;

      memcpy(buf + pos, &num_entities, sizeof(num_entities));
      pos += sizeof(num_entities);

      for(size_t entity_id = 0; entity_id < num_entities; ++entity_id)
      {
        entity_t* ent = entities_[entity_id];
        branch_int_t bid = ent->get_branch_id().value_();
        memcpy(buf + pos, &bid, sizeof(bid));
        pos += sizeof(bid);
      }

      return buf;
    }

    void
    unserialize_(
      char* buf
    )
    {
      uint64_t pos = 0;

      uint64_t num_entities;
      memcpy(&num_entities, buf + pos, sizeof(num_entities));
      pos += sizeof(num_entities);

      for(size_t entity_id = 0; entity_id < num_entities; ++entity_id)
      {
        entity_t* ent = new entity_t;
        ent->set_id_(entity_id);

        entities_.push_back(ent);

        branch_int_t bi;
        memcpy(&bi, buf + pos, sizeof(bi));
        pos += sizeof(bi);

        branch_id_t bid;
        bid.set_value_(bi);

        insert(ent, bid);
      }
    }
 #endif

   /**
    * @brief Generic information for the tree topology 
    */
   friend std::ostream& operator<<(std::ostream& os,tree_topology& t )
   {
     os<<"Tree topology: "<<"#branches: "<<t.branch_map_.size()<<
       " #entities: "<<t.entities_.size();
     os <<" #root_subentities: "<<t.root()->sub_entities();
     return os;
   } 

  private:
    using branch_map_t = std::unordered_map<branch_id_t, branch_t,
      branch_id_hasher__<branch_int_t, dimension>>;

      branch_id_t
      to_branch_id(
        const point_t& p,
        size_t max_depth
      )
      {
        return branch_id_t(range_, p, max_depth);
      }

      branch_id_t
      to_branch_id(
        const point_t& p
      )
      {
        return branch_id_t(range_, p);
      }

      void
      insert(
        entity_t* ent,
        size_t max_depth
      )
      {
        branch_id_t bid = to_branch_id(ent->coordinates(), max_depth);
        branch_t& b = find_parent(bid, max_depth);
        ent->set_branch_id_(b.id());

        b.insert(ent);

        switch(b.requested_action_())
        {
          case action::none:
            break;
          case action::refine:
            refine_(b);
            break;
          default:
            assert(false && "invalid action");
        }
      }

#if 0 
      void
      insert(
        entity_t* ent,
        branch_id_t bid
      )
      {
        branch_t& b = find_parent(bid, max_depth_);
        ent->set_branch_id_(b.id());

        b.insert(ent);

        switch(b.requested_action())
        {
          case action::none:
            break;
          case action::refine:
            refine_(b);
            break;
          default:
            assert(false && "invalid action");
        }
      }
#endif 
      branch_t&
      find_parent_(
      branch_id_t bid
    )
    {
      for(;;)
      {
        auto itr = branch_map_.find(bid);
        if(itr != branch_map_.end())
        {
          return itr->second;
        }
        bid.pop();
      }
    }

    branch_t*
    find_parent(
      branch_t* b
    )
    {
      return find_parent(b.id());
    }

    branch_t*
    find_parent(
      branch_id_t bid
    )
    {
      return find_parent(bid, max_depth_);
    }

    branch_t&
    find_parent(
      branch_id_t bid,
      size_t max_depth
    )
    {
      branch_id_t pid = bid;
      pid.truncate(max_depth);

      return find_parent_(pid);
    }

     void
    refine_(
      branch_t& b
    )
    {
      // Not leaf anymore 
      branch_id_t pid = b.id();
      size_t depth = pid.depth() + 1;

      // If there are no children
      for(size_t i = 0; i < branch_t::num_children; ++i)
      {
        branch_id_t cid = pid;
        cid.push(i);
        branch_map_.emplace(cid,cid);
      }

      max_depth_ = std::max(max_depth_, depth);

      for(auto ent : b)
      {
        insert(ent, depth);
      }

      b.set_leaf(false); 
      b.clear();
      b.reset();
    }

    // helper method in coarsening
    // insert into p, coarsen all recursive children of b

    void
    coarsen_(
      branch_t* p,
      branch_t* b
    )
    {
      if(b->is_leaf())
      {
        return;
      }

      for(size_t i = 0; i < branch_t::num_children; ++i)
      {
        branch_t* ci = b->template child_<branch_t>(i);

        for(auto ent : *ci)
        {
          p->insert(ent);
          ent->set_branch_id_(p->id());
        }

        coarsen_(p, ci);
        branch_map_.erase(ci->id());
      }
    }

    void
    coarsen_(
      branch_t* p
    )
    {
      coarsen_(p, p);
      p->template into_leaf_<branch_t>();
      p->reset();
    }

    size_t
    get_queue_depth(
      thread_pool& pool
    )
    {
      size_t n = pool.num_threads();
      constexpr size_t rb = branch_int_t(1) << P::dimension;
      double bn = std::log2(double(rb));
      return std::log2(double(n))/bn + 1;
    }

    branch_t*
    find_start_(
      const point_t& center,
      element_t radius,
      size_t& depth,
      element_t& size
    )
    {

      //element_t norm_radius = radius / max_scale_;

      branch_id_t bid = to_branch_id(center, max_depth_);
            
      int d = bid.depth();

      while(d > 0)
      {
        branch_t* b = find_parent(bid, d);

        point_t p2;
        b->id().coordinates(range_, p2);

        size = std::pow(element_t(2), -d);

        //bool found = true;
        if(!(distance(center,p2) <= radius)){
          depth = d;
          return b;
        }
        
        //for(size_t dim = 0; dim < dimension; ++dim)
        //{
        //  if(!(center[dim] - radius >= p2[dim] &&
        //       center[dim] + radius <= p2[dim] + size))
        //  {
        //    found = false;
        //    break;
        //  }
        // }

        //if(found)
        //{
        //  depth = d;
        //  return b;
        //}

        --d;
      }

      depth = 0;
      size = element_t(1);
      return root_;
    }

    template<
      typename EF,
      typename BF,
      typename... ARGS
    >
    void
    apply_(
      branch_t* b,
      element_t size,
      EF&& ef,
      BF&& bf,
      ARGS&&... args
    )
    {

      if(b->is_leaf())
      {
        for(auto ent : *b)
        {
          ef(ent, std::forward<ARGS>(args)...);
        }
        return;
      }

      size /= 2;

      for(size_t i = 0; i < branch_t::num_children; ++i)
      {
        branch_t* ci = b->template child_<branch_t>(i);

        if(bf(ci->coordinates(range_),
              size, scale_, std::forward<ARGS>(args)...))
        {
          apply_(ci, size,
                 std::forward<EF>(ef), std::forward<BF>(bf),
                 std::forward<ARGS>(args)...);
        }
      }
    }


    template<
      typename EF,
      typename BF,
      typename... ARGS
    >
    void
    apply_(
      thread_pool& pool,
      virtual_semaphore& sem,
      size_t queue_depth,
      size_t depth,
      branch_t* b,
      element_t size,
      EF&& ef,
      BF&& bf,
      ARGS&&... args
    )
    {

      if(b->is_leaf())
      {
        for(auto ent : *b)
        {
          ef(ent, std::forward<ARGS>(args)...);
        }

        size_t m = branch_int_t(1) << (queue_depth - depth) * P::dimension;

        for(size_t i = 0; i < m; ++i)
        {
          sem.release();
        }

        return;
      }

      size /= 2;
      ++depth;

      for(size_t i = 0; i < branch_t::num_children; ++i)
      {
        branch_t* ci = b->template child_<branch_t>(i);

        if(bf(ci->coordinates(range_),
              size, scale_, std::forward<ARGS>(args)...))
        {
          if(depth == queue_depth)
          {

            auto f = [&, size, ci]()
            {
              apply_(ci, size,
                std::forward<EF>(ef), std::forward<BF>(bf),
                std::forward<ARGS>(args)...);

              sem.release();
            };

            pool.queue(f);
          }
          else{
            apply_(pool, sem, queue_depth, depth, ci, size,
                   std::forward<EF>(ef), std::forward<BF>(bf),
                   std::forward<ARGS>(args)...);
          }
        }
        else{
          if(depth > queue_depth)
          {
            continue;
          }

          size_t m =
            branch_int_t(1) << (queue_depth - depth) * P::dimension;

          for(size_t i = 0; i < m; ++i)
          {
            sem.release();
          }
        }
      }
    }

    template<
      typename EF,
      typename BF,
      typename... ARGS
    >
    void
    find_(
      branch_t* b,
      element_t size,
      subentity_space_t& ents,
      EF&& ef,
      BF&& bf,
      ARGS&&... args
    )
    {

      if(b->is_leaf())
      {
        for(auto ent : *b)
        {
          if(ef(ent, std::forward<ARGS>(args)...))
          {
            ents.push_back(ent);
          }
        }
        return;
      }

      size /= 2;

      for(size_t i = 0; i < branch_t::num_children; ++i)
      {
        branch_t* ci = b->template child_<branch_t>(i);

        if(bf(ci->coordinates(range_),
              size, scale_, std::forward<ARGS>(args)...))
        {
          find_(ci, size, ents,
                std::forward<EF>(ef), std::forward<BF>(bf),
                std::forward<ARGS>(args)...);
        }
      }
    }


    template<
      typename EF,
      typename BF,
      typename... ARGS
    >
    void
    find_(
      thread_pool& pool,
      virtual_semaphore& sem,
      std::mutex& mtx,
      size_t queue_depth,
      size_t depth,
      branch_t* b,
      element_t size,
      subentity_space_t& ents,
      EF&& ef,
      BF&& bf,
      ARGS&&... args
    )
    {

      if(b->is_leaf())
      {
        mtx.lock();
        for(auto ent : *b)
        {
          if(ef(ent, std::forward<ARGS>(args)...))
          {
            ents.push_back(ent);
          }
        }
        mtx.unlock();

        size_t m = branch_int_t(1) << (queue_depth - depth) * P::dimension;

        for(size_t i = 0; i < m; ++i)
        {
          sem.release();
        }

        return;
      }

      size /= 2;
      ++depth;

      for(size_t i = 0; i < branch_t::num_children; ++i)
      {
        branch_t* ci = b->template child_<branch_t>(i);

        if(bf(ci->coordinates(range_),
              size, scale_, std::forward<ARGS>(args)...))
        {
          if(depth == queue_depth)
          {

            auto f = [&, size, ci]()
            {
              subentity_space_t branch_ents;

              find_(ci, size, branch_ents,
                std::forward<EF>(ef), std::forward<BF>(bf),
                std::forward<ARGS>(args)...);

              mtx.lock();
              ents.append(branch_ents);
              mtx.unlock();

              sem.release();
            };

            pool.queue(f);
          }
          else{
            find_(pool, sem, mtx, queue_depth, depth, ci, size, ents,
                  std::forward<EF>(ef), std::forward<BF>(bf),
                  std::forward<ARGS>(args)...);
          }
        }
        else{
          if(depth > queue_depth)
          {
            continue;
          }

          size_t m =
            branch_int_t(1) << (queue_depth - depth) * P::dimension;

          for(size_t i = 0; i < m; ++i)
          {
            sem.release();
          }
        }
      }
    }

    template<
      typename F,
      typename... ARGS
    >
    void visit_(
      branch_t* b,
      size_t depth,
      F&& f,
      ARGS&&... args
    )
    {
      if(f(b, depth, std::forward<ARGS>(args)...))
      {
        return;
      }

      if(b->is_leaf())
      {
        return;
      }

      for(size_t i = 0; i < branch_t::num_children; ++i)
      {
        branch_t* bi = b->template child_<branch_t>(i);
        visit_(bi, depth + 1, std::forward<F>(f), std::forward<ARGS>(args)...);
      }
    }

    template<
      typename F,
      typename... ARGS
    >
    void
    visit_(
      thread_pool& pool,
      virtual_semaphore& sem,
      branch_t* b,
      size_t depth,
      size_t queue_depth,
      F&& f,
      ARGS&&... args
    )
    {

      if(depth == queue_depth)
      {
        auto vf = [&, depth, b]()
        {
          visit_(b, depth, std::forward<F>(f), std::forward<ARGS>(args)...);
          sem.release();
        };

        pool.queue(vf);
        return;
      }

      if(f(b, depth, std::forward<ARGS>(args)...))
      {
        size_t m = branch_int_t(1) << (queue_depth - depth) * P::dimension;

        for(size_t i = 0; i < m; ++i)
        {
          sem.release();
        }

        return;
      }

      if(b->is_leaf())
      {
        size_t m = branch_int_t(1) << (queue_depth - depth) * P::dimension;

        for(size_t i = 0; i < m; ++i)
        {
          sem.release();
        }

        return;
      }

      for(size_t i = 0; i < branch_t::num_children; ++i)
      {
        branch_t* bi = b->template child_<branch_t>(i);

        visit_(pool, sem, bi, depth + 1, queue_depth,
               std::forward<F>(f), std::forward<ARGS>(args)...);
      }
    }

    template<
      typename F,
      typename... ARGS
    >
    void
    visit_children_(
      thread_pool& pool,
      virtual_semaphore& sem,
      size_t depth,
      size_t queue_depth,
      branch_t* b,
      F&& f,
      ARGS&&... args
    )
    {

      if(depth == queue_depth)
      {
        auto vf = [&, b]()
        {
          visit_children(b, std::forward<F>(f), std::forward<ARGS>(args)...);
          sem.release();
        };

        pool.queue(vf);
        return;
      }

      if(b->is_leaf())
      {
        for(auto ent : *b)
        {
          f(ent, std::forward<ARGS>(args)...);
        }

        size_t m = branch_int_t(1) << (queue_depth - depth) * P::dimension;

        for(size_t i = 0; i < m; ++i)
        {
          sem.release();
        }

        return;
      }

      for(size_t i = 0; i < branch_t::num_children; ++i)
      {
        branch_t* bi = b->template child_<branch_t>(i);
        visit_children_(pool, sem, depth + 1, queue_depth,
                        bi, std::forward<F>(f), std::forward<ARGS>(args)...);
      }
    }
 
    
  // Declared before, here for readability 
  //using branch_map_t = std::unordered_map<branch_id_t, branch_t,
  //    branch_id_hasher__<branch_int_t, dimension>>;

  branch_map_t branch_map_;
  size_t max_depth_;
  typename std::unordered_map<branch_id_t,branch_t,
    branch_id_hasher__<branch_int_t, dimension>>::iterator root_;
  entity_space_t entities_;
  std::array<point__<element_t, dimension>, 2> range_;
  point__<element_t, dimension> scale_;
  element_t max_scale_;
};
  
/*!
  Tree entity base class.
 */
template<
  typename T,
  size_t D
>
class tree_entity{
public:

  using id_t = entity_id_t;

  using branch_id_t = branch_id<T, D>;
  
protected:
  enum locality {LOCAL=0,NONLOCAL=1,SHARED=2,EXCL=3,GHOST=4}; 
  
public:

  tree_entity()
  : branch_id_(branch_id_t::null()),
  locality_(NONLOCAL)
  {}

  branch_id_t
  get_branch_id() const
  {
    return branch_id_;
  }

  entity_id_t
  id() const
  {
    return id_;
  }

  entity_id_t
  index_space_id() const
  {
    return id_;
  }

  /*!
    Return whether the entity is current inserted in a tree.
   */
  bool
  is_valid() const
  {
    return branch_id_ != branch_id_t::null();
  }

  /*!
   * Return true if the entity is local in this process
   */
  bool
  is_local() const 
  {
    return (locality_ == LOCAL || locality_ == EXCL || locality_ == SHARED); 
  }

  void 
  setLocality(locality loc)
  {
    locality_ = loc;
  }
  
  locality 
  getLocality()
  {
    return locality_;
  };

protected:
  template<class P>
  friend class tree_topology;

  void
  set_id_(
    entity_id_t id
  )
  {
    id_ = id;
  }

  void
  set_branch_id_(
    branch_id_t bid
  )
  {
    branch_id_ = bid;
  }

  branch_id_t branch_id_;
  entity_id_t id_;

  locality locality_;
};
 
} // namespace topology
} // namespace flecsi

#endif // flecsi_topology_tree_topology_h

/*~-------------------------------------------------------------------------~-*
 * Formatting options for vim.
 * vim: set tabstop=2 shiftwidth=2 expandtab :
 *~-------------------------------------------------------------------------~-*/