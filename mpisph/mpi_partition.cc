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
 * @file mpi_partition.cc
 * @author Julien Loiseau
 * @date April 2017
 * @brief Function needed for MPI distribution of the bodies 
 */

#include "mpi_partition.h"

#include <fstream>
#include <iostream>
  
std::ostream&
operator<<(
  std::ostream& ostr,
  const entity_key_t& id
)
{
  id.output_(ostr);
  return ostr;
}

inline 
bool
operator==(
    const point_t& p1, 
    const point_t& p2)
{
  for(size_t i=0;i<gdimension;++i)
    if(p1[i]!=p2[i])
      return false;
  return true;
}

inline 
bool
operator!=(
    const point_t& p1, 
    const point_t& p2)
{
  for(size_t i=0;i<gdimension;++i)
    if(p1[i]!=p2[i])
      return true;
  return false;
}

inline 
point_t
operator+(
    const point_t& p, 
    const double& val)
{
  point_t pr = p;
  for(size_t i=0;i<gdimension;++i)
    pr[i]+=val;
  return pr;
}

inline 
point_t
operator-(
    const point_t& p, 
    const double& val)
{
  point_t pr = p;
  for(size_t i=0;i<gdimension;++i)
    pr[i]-=val;
  return pr;
}

inline 
bool
operator<(
    const point_t& p, 
    const point_t& q)
{ 
  for(size_t i=0;i<gdimension;++i)
    if(p[i]>q[i])
      return false;
  return true;
}

inline 
bool
operator>(
    const point_t& p, 
    const point_t& q)
{  
  for(size_t i=0;i<gdimension;++i)
    if(p[i]<q[i])
      return false;
  return true;
}

inline 
point_t 
operator*(
    const point_t& p,
    const point_t& q)
{
  point_t r = p;
  for(size_t i=0;i<gdimension;++i)
    r[i] *= q[i];
  return r;
}


// Seek for the cells that are in the mass limit
// Send them to all the other processes 
void
mpi_exchange_cells(
    tree_topology_t& tree,
    std::vector<mpi_cell>& recvcells,
    std::vector<int>& nrecvcells,
    double maxMass)
{
  int rank,size; 
  MPI_Comm_size(MPI_COMM_WORLD,&size);
  MPI_Comm_rank(MPI_COMM_WORLD,&rank);
  // Find in the radius in the tree each of the COM that will be concerned by
  // the FMM method 
  std::vector<mpi_cell> vcells;

  // Perform a tree traversal to gather the cells  
  std::function<void(branch_t*)>traverse;
  traverse = [&tree,&traverse,&vcells,&maxMass](branch_t * b){
    if(b->getMass() == 0.0){
      return;
    }
    if(b->is_leaf() || b->getMass() < maxMass){
      vcells.push_back(mpi_cell(b->getPosition(),
            b->getBMin(),b->getBMax(),b->id()));
    }else{
      for(int i=0;i<(1<<gdimension);++i){
        traverse(tree.child(b,i));
      }
    }
  };
  traverse(tree.root());

  // Gather the number of cells from everyone 
  nrecvcells.resize(size);
  std::vector<int> noffsets(size);
  nrecvcells[rank] = vcells.size();
  MPI_Allgather(&nrecvcells[rank],1,MPI_INT,&nrecvcells[0],1,
      MPI_INT,MPI_COMM_WORLD);

  int totalrecv = 0;
  noffsets[0] = 0;
  for(int i=0;i<size;++i){
    totalrecv += nrecvcells[i];
    nrecvcells[i]*=sizeof(mpi_cell);
    if(i<size-1)
      noffsets[i+1] += nrecvcells[i]+noffsets[i];
  }

  recvcells.resize(totalrecv);
  MPI_Allgatherv(&vcells[0],nrecvcells[rank],MPI_BYTE,
      &recvcells[0],&nrecvcells[0],&noffsets[0],MPI_BYTE,MPI_COMM_WORLD);

  for(size_t i=0;i<vcells.size();++i){
    assert(vcells[i].position == 
        recvcells[i+noffsets[rank]/sizeof(mpi_cell)].position);
  }
}

// Compute the contribution of this process on the cells sended by the 
// other processes
void
mpi_compute_fmm(
    tree_topology_t& tree,
    std::vector<mpi_cell>& vcells,
    double macangle)
{
    for(auto &cell: vcells){
      branch_t sink;
      sink.setPosition(cell.position);
      sink.setBMax(cell.bmax);
      sink.setBMin(cell.bmin);
      memset(&cell.fc,0,sizeof(point_t));
      memset(cell.dfcdr,0,sizeof(double)*9);
      memset(cell.dfcdrdr,0,sizeof(double)*27);
      // Do the tree traversal, compute the cells data
      tree_traversal_c2c(tree,&sink,tree.root(),
          cell.fc,cell.dfcdr,cell.dfcdrdr,
          macangle);
    }
}


// Compute the ghosts particles needed for the gravity computation 
// Then send them to the process that reqiure it
void 
mpi_gather_ghosts_com(
    tree_topology_t& tree,
    std::vector<mpi_cell>& vcells,
    std::vector<int>& nsend,
    std::array<point_t,2>& range)
{
  int rank, size; 
  MPI_Comm_size(MPI_COMM_WORLD,&size);
  MPI_Comm_rank(MPI_COMM_WORLD,&rank);

  std::vector<body_holder> sendbh; 
  std::vector<int> nsendbh(size);
  std::vector<int> soffsets(size);
  std::vector<int> nrecvbh(size);
  std::vector<int> roffsets(size);

  nsendbh[rank] =0;
  int position = 0;

  // Go through all the particles but ignore the particles on this process
  for(int i=0;i<size;++i){
    int ncells = nsend[i]/sizeof(mpi_cell);
    if(i!=rank){
      for(int j=0;j<ncells;++j){
        //std::cout<<vcells[position].bmin<<vcells[position].bmax<<std::endl;
        auto ents = tree.find_in_box(vcells[position].bmin,
            vcells[position].bmax);
        auto vecents = ents.to_vec();
        // Just add the ones that are local 
        for(auto bi: vecents){
          if(bi->is_local()){
            nsendbh[i]++;
            sendbh.push_back(body_holder(bi->getPosition(),
                  nullptr,rank,bi->getMass()));
          }
        }
        position++;
      }
    }else{
      position+=ncells;
    }
  }

  std::vector<body_holder>::iterator iter = sendbh.begin();
  // Do a sort and unique per interval 
  for(int i=0;i<size;++i){
    if(i==rank){
      assert(nsendbh[i]==0);
      continue;
    }
    // First sort 
    std::sort(iter,iter+nsendbh[i],
        [&range](auto& left, auto &right)
      { 
        return entity_key_t(range,left.getPosition()) <
          entity_key_t(range,right.getPosition());
      });
    auto last = std::unique(iter,iter+nsendbh[i],
      [&range](auto& left, auto& right){
        return entity_key_t(range,left.getPosition())
          == entity_key_t(range,right.getPosition());
      });
    int eraselements = std::distance(last,iter+nsendbh[i]);
    sendbh.erase(last,iter+nsendbh[i]);
    // Change the number of elements 
    nsendbh[i] -= eraselements;
    iter = last;
  }

  // Gather the send in recv 
  MPI_Alltoall(&nsendbh[0],1,MPI_INT,&nrecvbh[0],1,MPI_INT,MPI_COMM_WORLD);

  int totalrecv = 0;
  // Generate the offsets 
  for(int i=0;i<size;++i){
    totalrecv+= nrecvbh[i];

    nsendbh[i]*=sizeof(body_holder);
    nrecvbh[i]*=sizeof(body_holder);
    if(i>1){ 
      soffsets[i] = soffsets[i-1]+nsendbh[i-1];
      roffsets[i] = roffsets[i-1]+nrecvbh[i-1];
    }
  }

  std::vector<body_holder> recvbh(totalrecv);
  MPI_Alltoallv(&sendbh[0],&nsendbh[0],&soffsets[0],MPI_BYTE,
      &recvbh[0],&nrecvbh[0],&roffsets[0],MPI_BYTE,MPI_COMM_WORLD);

  std::cout<<rank<<": gathered="<<recvbh.size()<<std::endl;

  // Sort base on keys 
  std::sort(recvbh.begin(),recvbh.end(),
      [&range](auto& left, auto &right)
      { 
        return entity_key_t(range,left.getPosition()) <
          entity_key_t(range,right.getPosition());
      });

  // Unique the vector based on position and then add this comtribution
  recvbh.erase(std::unique(recvbh.begin(),recvbh.end(),
      [&range](auto& left, auto& right){
        return entity_key_t(range,left.getPosition())
          == entity_key_t(range,right.getPosition());
      }),recvbh.end());

  std::cout<<rank<<": gathered unique ="<<recvbh.size()<<std::endl;

  // Create a local tree 
  tree_topology_t localtree(range[0],range[1]);
  // Add in the tree 
  for(auto bi: recvbh){
    auto nbi = localtree.make_entity(bi.getPosition(),nullptr,bi.getOwner(),
        bi.getMass());
    localtree.insert(nbi);
  }

  int ncells = nsend[rank]/sizeof(mpi_cell);
  for(int i=0;i<ncells;++i){
    auto subents = localtree.find_in_box(vcells[i].bmin,vcells[i].bmax);
    auto subentsapply = tree.find_in_box(vcells[i].bmin,vcells[i].bmax);
    auto subvec = subents.to_vec();
    auto subvecapply = subentsapply.to_vec();
    for(auto bi: subvecapply)
    {  
      if(bi->is_local( )){
        point_t grav = bi->getBody()->getGravForce();
        for(auto nb: subvec)
        {  
          double dist = flecsi::distance(bi->getPosition(),nb->getPosition());
          if(dist>0.0)
          {  
            grav += - nb->getMass()/(dist*dist*dist)*
              (bi->getPosition()-nb->getPosition());
          }
        }
        bi->getBody()->setGravForce(grav);
      }
    }
  }
}

// Gather the result from the other processes and add the forces 
// Then apply to the particles below it 
void 
mpi_gather_cells(
    tree_topology_t& tree,
    std::vector<mpi_cell>& vcells,
    std::vector<int>& nsend)
{ 
  int rank,size; 
  MPI_Comm_size(MPI_COMM_WORLD,&size);
  MPI_Comm_rank(MPI_COMM_WORLD,&rank);
  
  int totalrecv = nsend[rank]*size;
  int ncells = nsend[rank]/sizeof(mpi_cell);

  std::vector<int> nrecv(size);
  std::vector<int> noffsets(size);
  std::vector<int> soffsets(size);
  noffsets[0] = 0;
  soffsets[0] = 0;
  std::fill(nrecv.begin(),nrecv.end(),nsend[rank]);
  for(int i=1;i<size;++i){ 
    soffsets[i] = soffsets[i-1]+nsend[i-1]; 
    noffsets[i] = noffsets[i-1]+nsend[rank];
  }

  std::vector<mpi_cell> recvcells(ncells*size);
  MPI_Alltoallv(&vcells[0],&nsend[0],&soffsets[0],MPI_BYTE,
      &recvcells[0],&nrecv[0],&noffsets[0],MPI_BYTE,MPI_COMM_WORLD);

  assert((int)recvcells.size()==ncells*size);
  std::cout<<rank<<": receiv total="<<recvcells.size()<<std::endl;
  //MPI_Barrier(MPI_COMM_WORLD);

  // Reduce the sum on the COM 
  // They are in the same order from all the others 
  for(int i=1;i<size;++i){ 
    for(int j=0;j<ncells;++j) {
      assert(recvcells[j].position ==  recvcells[i*ncells+j].position );
      assert(recvcells[j].id == recvcells[i*ncells+j].id);
      recvcells[j].fc += recvcells[i*ncells+j].fc;
      for(int k=0;k<27;++k){ 
        if(k<9){ 
          recvcells[j].dfcdr[k] += recvcells[i*ncells+j].dfcdr[k];
        }
        recvcells[j].dfcdrdr[k] += recvcells[i*ncells+j].dfcdrdr[k];
      } 
    }
  }

  //std::cout<<rank<<": updated"<<std::endl;
  //MPI_Barrier(MPI_COMM_WORLD);
  
  int nbody = 0;
  // Propagate in the particles from sink 
  for(int i=0;i<ncells;++i){ 
    std::vector<body*> subparts;
    // Find the branch in the local tree with the id
    branch_t * sink =  tree.get(recvcells[i].id);
    assert(sink!=nullptr);
    point_t pos = sink->getPosition();
    sink_traversal_c2p(tree,sink,pos,
        recvcells[i].fc,recvcells[i].dfcdr,recvcells[i].dfcdrdr,
        subparts,nbody);
    assert(subparts.size()!=0);
    // Also apply the subcells
    for(auto bi: subparts)
    {  
      point_t grav = bi->getGravForce();
      for(auto nb: subparts)
      {  
        double dist = flecsi::distance(bi->getPosition(),nb->getPosition());
        if(dist>0.0)
        {  
          grav += - nb->getMass()/(dist*dist*dist)*
            (bi->getPosition()-nb->getPosition());
        }
      }
      bi->setGravForce(grav);
    }
  }
  //std::cout<<rank<<": computed"<<std::endl;
  //MPI_Barrier(MPI_COMM_WORLD);
}

// Compute the acceleration due to a source branch to the sink branch 
void 
computeAcceleration(
    point_t sinkPosition,
    point_t sourcePosition,
    double sourceMass,
    point_t& fc, 
    //point_t& dfc,
    double* jacobi,
    double* hessian
    )
{
  double dist = flecsi::distance(sinkPosition,sourcePosition);
  assert(dist > 0.0);
  point_t diffPos =  sinkPosition - sourcePosition;
  fc +=  - sourceMass/(dist*dist*dist)*(diffPos);
  double jacobicoeff = -sourceMass/(dist*dist*dist);
  for(int i=0;i<3;++i){ 
    for(int j=0;j<3;++j){
      if(i==j){ 
        jacobi[i*3+j] += jacobicoeff* 
          (1 - 3*(diffPos[i])*(diffPos[j])/(dist*dist)); 
      }else{ 
        jacobi[i*3+j] += jacobicoeff*
         (- 3*(diffPos[i])*(diffPos[j])/(dist*dist));
      }
      assert(!std::isnan(jacobi[i*3+j]));
    }
  }
  // Compute the Hessian matrix 
  double hessiancoeff = -3*sourceMass/(dist*dist*dist*dist*dist);
  for(int i=0;i<3;++i){
    int matrixPos = i*9;
    for(int j=0;j<3;++j){
      for(int k=0;k<3;++k){
        int position = matrixPos+j*3+k;
        hessian[position] = 0.0;
        double firstterm = 0.0;
        if(i==j){
          firstterm += hessiancoeff * diffPos[k];
        }
        if(j==k){
          firstterm += hessiancoeff * diffPos[i];
        }
        if(k==i){
          firstterm += hessiancoeff * diffPos[j];
        }
        if(i==j==k){
          firstterm*=1.0;
        }else{
          firstterm*=3.0;
        }
        hessian[position] += firstterm;
        hessian[position] += hessiancoeff * -5.0/(dist*dist)*
          (diffPos[i])*(diffPos[j])*(diffPos[k]);
      }
    }
  }
}

bool 
box_intersection(
    point_t& sinkBMin,
    point_t& sinkBMax,
    point_t& sourceBMin, 
    point_t& sourceBMax)
{
  return (sinkBMin[0]<=sourceBMax[0]&&sinkBMax[0]>=sourceBMin[0])&&
         (sinkBMin[1]<=sourceBMax[1]&&sinkBMax[1]>=sourceBMin[1])&&
         (sinkBMin[2]<=sourceBMax[2]&&sinkBMax[2]>=sourceBMin[2]);
}

bool
MAC(
    branch_t * sink,
    branch_t * source,
    double macangle)
{
  double dmax = flecsi::distance(source->getBMin(),source->getBMax());
  double disttoc = flecsi::distance(sink->getPosition(),source->getPosition());
  return dmax/disttoc < macangle;
}

static int count = 0;

void 
tree_traversal_c2c(
    tree_topology_t& tree, 
    branch_t * sink, 
    branch_t * source, 
    point_t& fc, 
    //point_t& dfc,
    double* jacobi,
    double* hessian,
    double& macangle)
{
  if(source->getMass() == 0.0){
    return;
  }
  // Do not consider the cell itself, or just inside its radius
  //if(source->getPosition()==sink->getPosition()){
  //  return; 
  //}
  
  // If the same box, stop
  if(sink->getBMin()==source->getBMin()&&sink->getBMax()==source->getBMax()){
    return;
  }

  // If inside the sink, stop 
  if(sink->getBMin()<source->getBMin()&&sink->getBMax()>source->getBMax()){
    return;
  }
  
  if(MAC(sink,source,macangle)){
      //std::cout<<"MAC OK"<<std::endl;
      computeAcceleration(sink->getPosition(),source->getPosition(),
        source->getMass(),fc,jacobi,hessian);
  }else{
    if(source->is_leaf()){
      for(auto bi: * source){
        if(bi->is_local()){
          if((bi->getPosition() < sink->getBMax() &&
              bi->getPosition() > sink->getBMin()))
            continue;
          //assert(!(bi->getPosition() < sink->getBMax() &&
          //    bi->getPosition() > sink->getBMin()));
          computeAcceleration(sink->getPosition(),bi->getPosition(),
            bi->getMass(),fc,jacobi,hessian);
          count++;
        }
      }
    }else{
      for(int i=0;i<(1<<gdimension);++i){
        tree_traversal_c2c(tree,sink,tree.child(source,i),
          fc,jacobi,hessian,macangle);
      }
    }
  }
}

void 
sink_traversal_c2p(
    tree_topology_t& tree,
    branch_t *b,
    point_t& sinkPosition,
    point_t& fc, 
    //point_t& dfc, 
    double* jacobi,
    double* hessian,
    std::vector<body*>& neighbors,
    int& nbody
    )
{

  if(b->getMass() <= 0.0){
    return;
  }
  if(b->is_leaf()){
    // Apply the expansion on the bodies
    for(auto bi: *b){
      if(!bi->is_local()){
        continue;
      }
      //point_t diffPos = sinkPosition-bi->getPosition();
      point_t diffPos = bi->getPosition() - sinkPosition;
      point_t grav = fc;
      // The Jacobi 
      for(int i=0;i<3;++i){
        for(int  j=0;j<3;++j){
          grav[i] += (jacobi[i*3+j]*(diffPos[j]));
        }
      }
      // The hessian 
      double tmpMatrix[9];
      for(int i=0;i<3;++i){
        for(int j=0;j<3;++j){
          for(int k=0;k<3;++k){
            tmpMatrix[i*3+j] = 0.0;
            tmpMatrix[i*3+j] += diffPos[k]*hessian[i*9+j+k*3];
          }
        } 
      }
      double tmpVector[3];
      for(int i=0;i<3;++i){
        for(int j=0;j<3;++j){
          tmpVector[i] = 0.0;
          tmpVector[i] += tmpMatrix[i*3+j]*diffPos[j];
        }
      }
      for(int i=0;i<3;++i){
        grav[i] += 1./2.*tmpVector[i];
      }
      neighbors.push_back(bi->getBody());
      bi->getBody()->setGravForce(grav); 
      nbody++;
      count++;
    }
  }else{
    for(int i=0;i<(1<<gdimension);++i){
      sink_traversal_c2p(tree,tree.child(b,i),sinkPosition,fc,jacobi,hessian,
          neighbors,nbody);
    }
  } 
}

void 
tree_traversal_grav(
    tree_topology_t& tree,
    branch_t * sink,
    double& mcell,
    double& macangle,
    int& nbody)
{
  // Do not consider empty branches, mass = 0  
  if(sink->getMass() == 0.0)
    return;
  if(sink->is_leaf() || sink->getMass() < mcell){
    point_t fc = {0.0,0.0,0.0};
    count = 0;
    double jacobi[9] = {};
    double hessian[27];
    memset(hessian,0,sizeof(double)*27);
    std::vector<body*> neighbors;
    tree_traversal_c2c(tree,sink,tree.root(),fc,jacobi,hessian,macangle);
    point_t sinkPosition = sink->getPosition();
    sink_traversal_c2p(tree,sink,sinkPosition,
        fc,jacobi,hessian,neighbors,nbody);
    //if(macangle == 0)
    //  assert(count == 4169); 
    // apply local gravitation from particles in this branch 
    for(auto bi: neighbors){ 
      point_t grav = bi->getGravForce();
      for(auto nb: neighbors){ 
        double dist = flecsi::distance(bi->getPosition(),nb->getPosition());
        if(dist>0.0){ 
          grav += - nb->getMass()/(dist*dist*dist)*
            (bi->getPosition()-nb->getPosition());
        }
      }
      bi->setGravForce(grav);
    } 
  }else{
    for(int i=0;i<(1<<gdimension);++i){
      tree_traversal_grav(tree,tree.child(sink,i),mcell,macangle,nbody);
    }
  }
}

// Compute the center of mass values from the particles 
void tree_traversal_com(tree_topology_t& tree)
{
  std::function<void(branch_t*)>traverse;
  traverse = [&tree,&traverse](branch_t * b)
  {
    //std::cout<< b->id() <<std::endl<<std::flush;
    double mass = 0.0;
    point_t com = {0,0,0};
    point_t bmax = {-99999,-99999,-99999};
    point_t bmin = {99999,99999,99999};
    //double radius = 0.0;
    if(b->is_leaf())
    {
      for(auto child: *b)
      {
        // Only for local particles 
        if(child->is_local()){
          double h = child->getBody()->getSmoothinglength();
          assert(child->getMass()>0.0);
          com += child->getMass()*child->getPosition();
          mass += child->getMass();
          for(size_t i=0;i<gdimension;++i){
            if(bmax[i] < child->getPosition()[i]){
              bmax[i] = child->getPosition()[i];
            }
            if(bmin[i] > child->getPosition()[i]){
              bmin[i] = child->getPosition()[i];
            }
          }
        }
      }
      if(mass > 0.0){
        com = com / mass;
      }
    }else{
      for(int i=0;i<(1<<gdimension);++i)
      {
        auto branch = tree.child(b,i);
        traverse(branch);
        // Add this children position and coordinates
        com += branch->getMass()*branch->getPosition();
        mass += branch->getMass();
        for(size_t i=0;i<gdimension;++i){
          if(bmax[i] < branch->getBMax()[i]){
            bmax[i] = branch->getBMax()[i];
          }
          if(bmin[i] > branch->getBMin()[i]){
            bmin[i] = branch->getBMin()[i];
          }
        }
      }
      if(mass > 0.0){
        com = com / mass;
      }
    }
    b->setMass(mass);
    assert(!std::isnan(mass));
    assert(mass>=0.0);
    b->setPosition(com);
    for(size_t i=0;i<gdimension;++i)
      assert(!std::isnan(com[i]));
    b->setBMax(bmax);
    b->setBMin(bmin);
    //b->setRadius(radius);
    //assert(!std::isnan(radius));
    //if(b->id().depth() == 1)
    //  std::cout<<bmin<<bmax<<std::endl;
  };

  traverse(tree.root());
  //std::cout<<"Root mass: "<<tree.root()->getMass()<<std::endl;
  //std::cout<<"Root boundaries: "<<
  //  tree.root()->getBMin()<<tree.root()->getBMax()<<std::endl;
}

void mpi_sort_unbalanced(
    std::vector<std::pair<entity_key_t,body>>& rbodies,
    int totalnbodies
    )
{
  int size, rank;
  MPI_Comm_size(MPI_COMM_WORLD,&size);
  MPI_Comm_rank(MPI_COMM_WORLD,&rank);

  // Sort the keys 
  std::sort(rbodies.begin(),rbodies.end(),
      [](auto& left, auto& right)
      {
        return left.first < right.first;
      });

  // If on process, done 
  if(size==1)
    return;

  // Create a vector for the samplers 
  std::vector<entity_key_t> keys_sample;
  // Number of elements for sampling 
  // In this implementation we share up to 256KB to 
  // the master. 
  size_t noct = 256*1024;
  size_t nsample = noct / sizeof(entity_key_t);
  if(rbodies.size()<nsample){nsample = rbodies.size();}
  int chuncksize = rbodies.size()/nsample;
  //std::cout<<rank<<": nsample: "<<nsample<<" chuncksize: "<<chuncksize<<std::endl;

  for(size_t i=0;i<nsample;++i)
  {
    keys_sample.push_back(rbodies[chuncksize*i].first);
  }
  assert(keys_sample.size()==nsample);

  std::vector<entity_key_t> master_keys;
  std::vector<int> master_recvcounts;
  std::vector<int> master_offsets;
  int master_nkeys = 0; 

  if(rank==0)
    master_recvcounts.resize(size);
  
  // Echange the number of samples
  MPI_Gather(&nsample,1,MPI_INT,
      &master_recvcounts[0],1,MPI_INT,0,MPI_COMM_WORLD);

  // Master 
  if(rank == 0)
  {
    master_offsets.resize(size); 
    master_nkeys = noct/sizeof(entity_key_t)*size;
    if(totalnbodies<master_nkeys){master_nkeys=totalnbodies;}
    // Number to receiv from each process
    for(int i=0;i<size;++i)
      master_recvcounts[i]*=sizeof(entity_key_t);
    std::partial_sum(master_recvcounts.begin(),master_recvcounts.end(),
        &master_offsets[0]); 
    master_offsets.insert(master_offsets.begin(),0);
    master_keys.resize(master_nkeys);
  }

  MPI_Gatherv(&keys_sample[0],nsample*sizeof(entity_key_t),MPI_BYTE,
      &master_keys[0],&master_recvcounts[0],&master_offsets[0],MPI_BYTE,
      0,MPI_COMM_WORLD);

  // Generate the splitters
  std::vector<entity_key_t> splitters; 
  splitters.resize(size-1);
  if(rank==0)
  {

    std::sort(master_keys.begin(),master_keys.end());
    std::cout<<entity_key_t::first_key()<<std::endl;
    chuncksize = master_nkeys/size;
    for(int i=0;i<size-1;++i){
      splitters[i] = master_keys[(i+1)*chuncksize];
      std::cout<<splitters[i]<<std::endl;
    }
    std::cout<<entity_key_t::last_key()<<std::endl;
  }

  // Bradcast the splitters 
  MPI_Bcast(&splitters[0],(size-1)*sizeof(entity_key_t),MPI_BYTE,
      0,MPI_COMM_WORLD);

  // The rbodies array is already sorted. We just need to determine the 
  // limits for each process
  std::vector<int> sendcount(size);
  std::fill(sendcount.begin(),sendcount.end(),0);
  for(auto bi: rbodies)
  {
    for(int i = 0; i< size;++i)
    {
      if(i == 0 && bi.first < splitters[i]){
        sendcount[0]++;
      }else if(i == size-1 && bi.first >= splitters[size-2]){
        sendcount[i]++;
      }else if(bi.first < splitters[i] && bi.first >= splitters[i-1]){
        sendcount[i]++;
      }
    }
  }
 
  // Share the bucket 
  // First send the sizes of each bucket 
  std::vector<int> recvcount(size);
  MPI_Alltoall(&sendcount[0],1,MPI_INT,&recvcount[0],1,MPI_INT,
      MPI_COMM_WORLD);

  // Create the offset for alltoallv
  //  First receiv offset
  std::vector<int> recvoffsets(size);
  std::partial_sum(recvcount.begin(),recvcount.end(),&recvoffsets[0]); 
  // As we need an exscan, add a zero
  recvoffsets.insert(recvoffsets.begin(),0);
  
  // Then send offsets
  std::vector<int> sendoffsets(size);
  std::partial_sum(sendcount.begin(),sendcount.end(),&sendoffsets[0]);
  // As we need an exscan, add a zero
  sendoffsets.insert(sendoffsets.begin(),0);
 
  // The receiv buffer, work in th rbodies
  std::vector<std::pair<entity_key_t,body>> recvbuffer; 
  recvbuffer.resize(recvoffsets.back(),
      std::make_pair(entity_key_t::null(),body()));

  // We need to keep the last value to generate rbodies before erasing
  //recvoffsets.pop_back(); 
  //sendoffsets.pop_back();

  // Trnaform the offsets for bytes 
  for(auto& bs: sendcount)
    bs*=sizeof(std::pair<entity_key_t,body>);
  for(auto& bs: sendoffsets)
    bs*=sizeof(std::pair<entity_key_t,body>);
  for(auto& bs: recvcount)
    bs*=sizeof(std::pair<entity_key_t,body>);
  for(auto& bs: recvoffsets)
    bs*=sizeof(std::pair<entity_key_t,body>);

  // Use this array for the global buckets communication
  MPI_Alltoallv(&rbodies[0],&sendcount[0],&sendoffsets[0],MPI_BYTE,
      &recvbuffer[0],&recvcount[0],&recvoffsets[0],MPI_BYTE,
      MPI_COMM_WORLD);

  //rbodies.clear();
  rbodies = recvbuffer;

  // Sort the incoming buffer 
  sort(rbodies.begin(),rbodies.end(),[](auto& left, auto &right)
  {
    return left.first<right.first;
  }); 

  // Check for duplicates
  //assert(rbodies.end() == std::unique(rbodies.begin(),rbodies.end(),
  //    [](const auto& left, const auto& right ){ 
  //      return left.first == right.first;
  //    })
  //);

  std::vector<int> totalprocbodies;
  totalprocbodies.resize(size);
  int mybodies = rbodies.size();
  // Share the final array size of everybody 
  MPI_Allgather(&mybodies,1,MPI_INT,&totalprocbodies[0],1,MPI_INT,
      MPI_COMM_WORLD);

  if(rank == 0){
    std::cout<<"Repartition: ";
    for(auto num: totalprocbodies)
      std::cout<<num<<";";
    std::cout<<std::endl;
  }
}

void mpi_tree_traversal_graphviz(tree_topology_t & tree,
   std::array<point_t,2>& range)
{
  int rank = 0;
  MPI_Comm_rank(MPI_COMM_WORLD,&rank);

  char fname[64];
  sprintf(fname,"output_graphviz_%d.gv",rank);
  std::ofstream output;
  output.open(fname);
  output<<"digraph G {"<<std::endl;

  std::stack<branch_t*> stk;
  // Get root
  auto rt = tree.root();
  stk.push(rt);

  while(!stk.empty()){
    branch_t* cur = stk.top();
    stk.pop();
    if(!cur->is_leaf()){
      // Add the child to the stack and add for display 
      for(size_t i=0;i<(1<<gdimension);++i){
        auto br = tree.child(cur,i);
        //if(br){
          stk.push(br);
          if(gdimension == 3){
            output<<std::oct<<cur->id().value_()
              <<"->"<<br->id().value_()<<std::dec<<std::endl;
          }else if(gdimension == 1){
            output<<std::bitset<64>(cur->id().value_())<<"->"<<
              std::bitset<64>(br->id().value_())<<std::endl;
          }
        //}
      }
    }else{
      for(auto ent: *cur){
        entity_key_t key(range,ent->coordinates());
        output<<std::bitset<64>(cur->id().value_())<<
          "->"<<key<<std::endl;
        //fprintf(output,"\"%lo\" -> \"%lo\"\n",cur->id().value_(),
        //    key.truncate_value(17));
        switch (ent->getLocality()){
          case SHARED:
            output<<key<<"[shape=box,color=blue]"<<std::endl;
            break;
          case EXCL:
            output<<key<<" [shape=box,color=red]"<<std::endl;
            //fprintf(output,"\"%lo\" [shape=box,color=red]\n",
            //  key.truncate_value(17));
            break;
          case GHOST:
            output<<key<<" [shape=box,color=green]"<<std::endl;
            //fprintf(output,"\"%lo\" [shape=box,color=green]\n",
            //  key.truncate_value(17));
            break;
          default:
            output<<key<<" [shape=circle,color=black]"<<std::endl;
            //fprintf(output,"\"%lo\" [shape=circle,color=black]\n",
            //  key.truncate_value(17));
            break;
        }
      }
    } 
  }
  output<<"}"<<std::endl;
  output.close();
}


void mpi_output_txt(
    std::vector<std::pair<entity_key_t,body>>&rbodies,
    int iter)
{

  int rank,size;
  MPI_Comm_rank(MPI_COMM_WORLD,&rank);
  MPI_Comm_size(MPI_COMM_WORLD,&size);

  std::vector<int> processnbodies(size);
  processnbodies[rank] = rbodies.size();
  // Gather the number of bodies per process on 0
  MPI_Gather(&processnbodies[rank],1,MPI_INT,
      &processnbodies[0],1,MPI_INT,0,MPI_COMM_WORLD);

  int totalnbodies = std::accumulate(processnbodies.begin(),
      processnbodies.end(),0);
  //std::cout<<totalnbodies<<std::endl;
  // Display initial data 
  // Ouput the data
  // Gather on process 0 then output everythings
  std::vector<std::pair<entity_key_t,body>> gatheroutput;
  std::vector<int> recvcount;
  std::vector<int> recvoffsets;
  if(rank==0){
    recvcount.resize(size);
    recvoffsets.resize(size);
    std::fill(recvoffsets.begin(),recvoffsets.end(),0);
    for(int i=0;i<size;++i)
    {
      recvcount[i] = processnbodies[i]*sizeof(std::pair<entity_key_t,body>);
      if(i < size-1){
        recvoffsets[i+1] = recvoffsets[i] + processnbodies[i];
        //recvoffsets[i+1] *=  sizeof(std::pair<entity_key_t,body>);
      }
    }
    gatheroutput.resize(totalnbodies);
    for(auto& val: recvoffsets)
    {
      val *= sizeof(std::pair<entity_key_t,body>);
    } 
  }
  
   MPI_Gatherv(&rbodies[0],
      processnbodies[rank]*sizeof(std::pair<entity_key_t,body>),
      MPI_BYTE,
      &gatheroutput[0],&recvcount[0],&recvoffsets[0],MPI_BYTE,
      0,MPI_COMM_WORLD);
  
  if(rank == 0)
  {
    char name[64];
    sprintf(name,"output_sod_%05d.txt",iter);
    std::cout<<"Output in file "<<name<<std::endl;
    //std::cout<<"Received: "<<gatheroutput.size()<<std::endl;
    FILE * file;
    file = fopen(name,"w");
    fprintf(file,"# pX pY pZ d p u vX vY vZ\n");
    // Write in an output file 
    for(auto bi: gatheroutput)
    {
      if(gdimension==1)
        fprintf(file,"%.10f %.10f %.10f %.10f %.10f\n",
          bi.second.getPosition()[0],bi.second.getDensity(),
          bi.second.getPressure(),bi.second.getInternalenergy(),
          bi.second.getVelocity()[0]);
      if(gdimension==3)
        fprintf(file,"%.10f %.10f %.10f %.10f %.10f %.10f %.10f %.10f %.10f\n",
          bi.second.getPosition()[0],bi.second.getPosition()[1],
          bi.second.getPosition()[2],bi.second.getDensity(),
          bi.second.getPressure(),bi.second.getInternalenergy(),
          bi.second.getVelocity()[0],bi.second.getVelocity()[1],
          bi.second.getVelocity()[2]);

    }
    fclose(file);
  }
}

// Exchange the usefull body_holder based on the bounding box of this process 
// This will be used to find the ghosts 
void
mpi_branches_exchange_useful_positions(
    tree_topology_t& tree,
    std::vector<std::pair<entity_key_t,body>>& rbodies,
    std::vector<std::pair<point_t,point_t>>& ranges,
    double smoothinglength)
{
  int rank,size;
  MPI_Comm_rank(MPI_COMM_WORLD,&rank);
  MPI_Comm_size(MPI_COMM_WORLD,&size);
  MPI_Barrier(MPI_COMM_WORLD);
  if(rank==0)
    std::cout<<"Branches repartition" << std::flush;
  MPI_Barrier(MPI_COMM_WORLD);

  // Search for my min and max posititon needed
  std::pair<point_t,point_t> range;
  range.first = rbodies.front().second.getPosition();
  range.second = rbodies.front().second.getPosition();

  // Lowest position:
  for(auto bi: rbodies)
  {
    for(size_t dim=0;dim<gdimension;++dim)
    {
      if(range.first[dim]>bi.second.getPosition()[dim])
        range.first[dim] = bi.second.getPosition()[dim];
      if(range.second[dim]<bi.second.getPosition()[dim])
        range.second[dim] = bi.second.getPosition()[dim];
    } 
  }

  range.first = range.first-2*smoothinglength;
  range.second = range.second+2*smoothinglength;

  std::cout<<rank<<": "<<range.first<<";"<<range.second<<std::endl;
  
  // Gather the keys of everyone 
  if(ranges.size() == 0)
    ranges.resize(size);
  //std::vector<std::pair<entity_key_t,entity_key_t>> ranges(size);
  MPI_Allgather(&range,sizeof(std::pair<point_t,point_t>),
      MPI_BYTE,&ranges[0],sizeof(std::pair<point_t,point_t>),
      MPI_BYTE,MPI_COMM_WORLD);

  // Now generate the sendbuffer, ordered by processes
  // for the holders 
  std::vector<body_holder_mpi_t> sendbuffer;
  std::vector<int> sendcount(size);
  sendcount[rank]=0;

  // Search in the tree for each processes 
  for(int i=0;i<size;++i)
  {
    if(i==rank)
      continue;
    auto ents = tree.find_in_box(ranges[i].first,ranges[i].second);
    sendcount[i] = ents.size();
    for(auto ent: ents)
    {
      sendbuffer.push_back(body_holder_mpi_t{
            ent->getPosition(),
            rank,
            ent->getMass()});
    }
  }

  //Count the elements to send 
  std::vector<int> sendoffsets(size);
  std::vector<int> recvcount(size);
  std::vector<int> recvoffsets(size);

  MPI_Alltoall(&sendcount[0],1,MPI_INT,
      &recvcount[0],1,MPI_INT,MPI_COMM_WORLD);

  int totalrecv = 0;
  for(int i=0;i<size;++i)
  {
    totalrecv += recvcount[i];
    sendcount[i]*=sizeof(body_holder_mpi_t);
    recvcount[i]*=sizeof(body_holder_mpi_t);
    if(i<size-1)
    {
      sendoffsets[i+1]=sendoffsets[i]+sendcount[i];
      recvoffsets[i+1]=recvoffsets[i]+recvcount[i];
    }
  }

  std::vector<body_holder_mpi_t> recvbuffer(totalrecv);
  MPI_Alltoallv(&sendbuffer[0],&sendcount[0],&sendoffsets[0],MPI_BYTE,
      &recvbuffer[0],&recvcount[0],&recvoffsets[0],MPI_BYTE,
      MPI_COMM_WORLD);

  // Add them in the tree 
  for(auto bi: recvbuffer)
  {
    assert(bi.owner!=rank);
    auto nbi = tree.make_entity(bi.position,nullptr,bi.owner,bi.mass);
    tree.insert(nbi);
  }

  if(rank==0)
    std::cout<<".done"<<std::endl;

}

// compute the range, minposition and maxposition from a group of bodies
void 
mpi_compute_range(
    std::vector<std::pair<entity_key_t,body>>& bodies,
    std::array<point_t,2>& range,
    double smoothinglength
    )
{
  int rank, size; 
  MPI_Comm_size(MPI_COMM_WORLD,&size);
  MPI_Comm_rank(MPI_COMM_WORLD,&rank);

  // Compute the range to compute the keys 
  double max[gdimension];
  for(size_t i=0;i<gdimension;++i)
    // TODO replace by the max value 
    max[i] = -9999;
  double min[gdimension];
  for(size_t i=0;i<gdimension;++i)
    // TODO replace by the max value 
    min[i] = 9999;
  for(auto bi: bodies){
    for(size_t i=0;i<gdimension;++i){
        if(bi.second.coordinates()[i]>max[i])
          max[i] = bi.second.coordinates()[i];
        if(bi.second.coordinates()[i]<min[i])
          min[i] = bi.second.coordinates()[i];
      }
  }

  // Do the MPI Reduction 
  MPI_Allreduce(MPI_IN_PLACE,max,gdimension,MPI_DOUBLE,MPI_MAX,MPI_COMM_WORLD); 
  MPI_Allreduce(MPI_IN_PLACE,min,gdimension,MPI_DOUBLE,MPI_MIN,MPI_COMM_WORLD); 
 
  point_t minposition; 
  point_t maxposition; 

  for(size_t i=0;i<gdimension;++i){
    minposition[i] = min[i]-2*smoothinglength;
    maxposition[i] = max[i]+2*smoothinglength;
  }

  if(rank==0)
    std::cout <<"boundaries: "<< minposition << maxposition << std::endl;
  
  range[0] = minposition;
  range[1] = maxposition;
}

void mpi_refresh_ghosts(
    tree_topology_t& tree, 
    mpi_ghosts_t& refresh,
    std::array<point_t,2>& range    
    )
{
  int rank,size;
  MPI_Comm_rank(MPI_COMM_WORLD,&rank);
  MPI_Comm_size(MPI_COMM_WORLD,&size);
  
  MPI_Barrier(MPI_COMM_WORLD);
  if(rank==0)
    std::cout<<"Refresh Ghosts" << std::flush;

  // Refresh the sendbodies with new data
  auto itsb = refresh.sendbodies.begin();
  for(auto proc: refresh.sendholders)
  {
    for(auto bi: proc)
    {
      assert(bi->getBody()!=nullptr);
      *itsb = *(bi->getBody());
      itsb++;
    }
  } 

  MPI_Alltoallv(&refresh.sendbodies[0],&refresh.nsendholders[0],
      &refresh.nsendoffsets[0],MPI_BYTE,
      &refresh.recvbodies[0],&refresh.nrecvholders[0],
      &refresh.nrecvoffsets[0],MPI_BYTE,
      MPI_COMM_WORLD);

  // Sort the bodies based on key
  std::sort(refresh.recvbodies.begin(),refresh.recvbodies.end(),
      [range](auto& left, auto& right){
        return entity_key_t(range,left.coordinates())<
        entity_key_t(range,right.coordinates());
      });
 
  // Then link the holders with these bodies
  auto it = refresh.recvbodies.begin();
  if(size==1)
   assert(refresh.totalrecvholders.size() == 0); 
  for(auto& bi: refresh.totalrecvholders)
  {
    auto bh = tree.get(bi->id());
    assert(bh->getLocality()==NONLOCAL||bh->getLocality()==GHOST);
    bh->setBody(&(*it));
    assert(bh->coordinates()==bh->getBody()->coordinates());
    //std::cout<<*(bh->getBody())<<std::endl;
    //bh->setLocality(NONLOCAL);
    ++it;
  }  
  
  MPI_Barrier(MPI_COMM_WORLD);
  if(rank==0)
    std::cout<<".done" << std::endl << std::flush;
}

void 
mpi_compute_ghosts(
    tree_topology_t& tree,
    double smoothinglength,
    mpi_ghosts_t& ghosts_data,
    std::array<point_t,2>& range)
{
  int rank,size;
  MPI_Comm_rank(MPI_COMM_WORLD,&rank);
  MPI_Comm_size(MPI_COMM_WORLD,&size);
  
  MPI_Barrier(MPI_COMM_WORLD);
  if(rank==0)
    std::cout<<"Compute Ghosts" << std::flush;

  // Clean the structure 
  ghosts_data.sendbodies.clear();
  ghosts_data.recvbodies.clear();
  ghosts_data.totalrecvholders.clear();
  ghosts_data.sendholders.clear();
  // For the first iteration init the count and offset size in the structure
  if(ghosts_data.nsendholders.size()==0)
  {
    ghosts_data.nsendholders.resize(size);
    ghosts_data.nsendoffsets.resize(size);
    ghosts_data.nrecvholders.resize(size);
    ghosts_data.nrecvoffsets.resize(size);
  }
  assert(ghosts_data.nsendholders.size()==(size_t)size);

  // 1. for each processes, bucket of send, bucket of receiv 
  ghosts_data.sendholders.resize(size);
  std::vector<std::set<body_holder*>> recvholders(size);

  // Considering the biggest h
  // TODO add a reduction over h 
  int totalrecvbodies = 0;
  int totalsendbodies = 0;
  auto treeents = tree.entities();
  for(auto bi: treeents)
  { 
    if(bi->is_local())
    {
      assert(bi->getOwner() == rank);
      auto bodiesneighbs = tree.find_in_radius(bi->coordinates(),
           2*smoothinglength);
      for(auto nb: bodiesneighbs)
      {
        if(!nb->is_local())
        {
          // THIS IS TRUE BECAUSE WE CONSIDER THE BIGGEST SMOOTHING LENGTH
          // The distant particle will need mine
          ghosts_data.sendholders[nb->getOwner()].insert(bi);
          // I will also need the distant particle 
          recvholders[nb->getOwner()].insert(nb);
        }
      }
    }
  }

  for(int i=0;i<size;++i){
    ghosts_data.nsendholders[i] = ghosts_data.sendholders[i].size();
    assert(ghosts_data.nsendholders[i]>=0);
    totalsendbodies += ghosts_data.nsendholders[i];
  }
  
  for(int i=0;i<size;++i){
    ghosts_data.nrecvholders[i] = recvholders[i].size();
    assert(ghosts_data.nrecvholders[i]>=0);
    totalrecvbodies += ghosts_data.nrecvholders[i];
  }
  
  // Make a vector with the recvholsters to be able to connect the pointer
  // at the end of the communication
  for(auto proc: recvholders){
    ghosts_data.totalrecvholders.insert(
        ghosts_data.totalrecvholders.end(),proc.begin(),proc.end());
  }

  // Now gather the bodies data to send in a vector 
  // Take the holders in the order 0 to n processes
  //for(auto proc: ghosts_data.sendholders)
  //{
  //  for(auto bi: proc)
  //  {
  //    auto bodyholder = tree.get(bi->id());
  //    assert(bodyholder->getBody()!=nullptr);
  //    ghosts_data.sendbodies.push_back(*(bodyholder->getBody()));
  //  }
  //}
  ghosts_data.sendbodies.resize(totalsendbodies);

  // Prepare offsets for alltoallv
  ghosts_data.nrecvoffsets[0]=0;
  ghosts_data.nsendoffsets[0]=0;

  for(int i=1;i<size;++i)
  {
    ghosts_data.nrecvoffsets[i] = ghosts_data.nrecvholders[i-1]+
      ghosts_data.nrecvoffsets[i-1];
    ghosts_data.nsendoffsets[i] = ghosts_data.nsendholders[i-1]+
      ghosts_data.nsendoffsets[i-1]; 
  }

  ghosts_data.recvbodies.resize(totalrecvbodies);

  // Convert the offsets to byte
  for(int i=0;i<size;++i)
  {
    ghosts_data.nsendholders[i]*=sizeof(body);
    assert(ghosts_data.nsendholders[i]>=0);
    ghosts_data.nrecvholders[i]*=sizeof(body);
    assert(ghosts_data.nrecvholders[i]>=0);
    ghosts_data.nsendoffsets[i]*=sizeof(body);
    assert(ghosts_data.nsendoffsets[i]>=0);
    ghosts_data.nrecvoffsets[i]*=sizeof(body);
    assert(ghosts_data.nrecvoffsets[i]>=0);
  }
  
  //std::cout<<rank<<": "<<  <<std::endl;

  // Sort the aolders once
  std::sort(ghosts_data.totalrecvholders.begin(),
            ghosts_data.totalrecvholders.end(),
      [range](auto& left, auto& right){
        return entity_key_t(range,left->coordinates())<
        entity_key_t(range,right->coordinates());
      });

  MPI_Barrier(MPI_COMM_WORLD);
  if(rank==0)
    std::cout<<".done"<<std::endl;
}

/*~--------------------------------------------------------------------------~*/
/*                            OLD FUNCTION VERSIONS                           */ 

#if 0
bool 
intersect_sphere_box(
    point_t& scenter, 
    double sradius, 
    point_t& bmax,
    point_t& bmin
    )
{
  double x[gdimension];
  for(size_t dim=0;dim<gdimension;++dim)
    x[dim]=std::max(bmin[dim],std::min(scenter[dim],bmax[dim]));
  double dist = 0.0;
  for(size_t dim=0;dim<gdimension;++dim)
    dist += (x[dim]-scenter[dim])*(x[dim]-scenter[dim]);
  dist = sqrt(dist);
  return dist<sradius;

}


void traversal_COM(
    int rank,
    tree_topology_t &tree,
    std::array<point_t,2> &range,
    branch_t * b,
    std::vector<body_holder>& vbh,
    std::pair<point_t,point_t>& rangeproc,
    int& nelements)
{
  auto pos = b->getPosition();
  // If in the range, go further 
  if(intersect_sphere_box(pos,b->getRadius(),
        rangeproc.first,rangeproc.second))
  {
    if(!b->is_leaf())
    {
      for(int i=0;i<(1<<gdimension);++i)
        traversal_COM(rank,tree,range,tree.child(b,i),vbh,rangeproc,nelements);
    }else{
      // If I am a leaf check if one of my children is out of the range
      for(auto child: *b)
      {
        // Only for local particles 
        if(child->is_local()){
           auto childpos = child->getPosition();
           if(intersect_sphere_box(childpos,
                 child->getBody()->getSmoothinglength()*2,
                 rangeproc.first,rangeproc.second))
          {
            vbh.push_back(body_holder(child->getPosition(),nullptr,rank,
                child->getMass()));
            nelements++;
          }
        }   
      }
    }
  }else
  {
    // Mass = 0 just for elements I did not owe
    if(b->getMass()!=0)
    {
      // Not in range, stop going down and add in vector 
      vbh.push_back(body_holder(b->getPosition(),nullptr,rank,
            b->getMass()));
      nelements++;
    }
  } 
};
#endif 

#if 0
void 
mpi_gather_com_positions(
    tree_topology_t& tree,
    std::array<point_t,2>& range,
    std::vector<std::pair<point_t,point_t>>& rangeproc,
    std::vector<body_holder>& recv_COM
    )
{
  int rank,size;
  MPI_Comm_size(MPI_COMM_WORLD,&size);
  MPI_Comm_rank(MPI_COMM_WORLD,&rank);

  if(rank==0)
    std::cout<<"Gather COM"<<std::flush;
  MPI_Barrier(MPI_COMM_WORLD);

  // Reset the recv vector 
  recv_COM.clear();
  // Compute COM data for the current tree
  tree_traversal_com(tree);

  // Display my root data
  double mymass = tree.root()->getMass();
  //std::cout<<rank<<":"<<tree.root()->getMass()<<" "<<tree.root()->getRadius()<<
  //  tree.root()->getPosition()<<std::endl;
  double total = 0.0;
  MPI_Reduce(&(mymass),&total,1,MPI_DOUBLE,
      MPI_SUM,0,MPI_COMM_WORLD); 

#ifdef DEBUG
  static double checkmass = 0.0;
  if(checkmass == 0.0)
    checkmass = total;
  else
    printf("Diff = %g\n",checkmass-total);
    assert(fabs(total-checkmass)<1.0e-10 && 
        "Error in total mass from COM");
#endif
  // Get the interesting branches for everyones. 
  // Do a traversal and create a vector for every other processes

  // Set a unique vector for everyone, to be contiguous 
  std::vector<body_holder> shared_COM;
  std::vector<int> shared_COM_size(size);
  std::vector<int> shared_COM_offsets(size);
  std::fill(shared_COM_size.begin(),shared_COM_size.end(),0);
  std::fill(shared_COM_offsets.begin(),shared_COM_offsets.end(),0);

  for(int i=0;i<size;++i)
  {
    int nelements = 0;
    // Do a tree traversal knowing the limits of every processes limits
    // Only take the keys that are outside
    if(i!=rank)
      traversal_COM(rank,tree,range,tree.root(),shared_COM,rangeproc[i],nelements);
    shared_COM_size[i]=nelements*sizeof(body_holder);
    if(i<size-2)
      shared_COM_offsets[i+1] = shared_COM_offsets[i]+shared_COM_size[i];
    //std::cout<<rank<<" for "<<i<<": "<<nelements<<std::endl;
  }

  std::vector<int> recvcount(size);
  std::vector<int> recvoffsets(size);
  // Share data size
  MPI_Alltoall(&shared_COM_size[0],1,MPI_INT,
      &recvcount[0],1,MPI_INT,MPI_COMM_WORLD);
  std::partial_sum(recvcount.begin(),recvcount.end(),&recvoffsets[0]); 
  // As we need an exscan, add a zero and delete the last element 
  recvoffsets.insert(recvoffsets.begin(),0);
 
  recv_COM.resize(recvoffsets.back()/sizeof(body_holder)); 
  // Share the data
  MPI_Alltoallv(&shared_COM[0],&shared_COM_size[0],&shared_COM_offsets[0],
      MPI_BYTE,&recv_COM[0],&recvcount[0],&recvoffsets[0],
      MPI_BYTE,MPI_COMM_WORLD); 

  if(rank==0)
    std::cout<<".done"<<std::endl<<std::flush;
  if(rank==0)
    std::cout<<"Total mass = "<<total<<std::endl;
  

  MPI_Barrier(MPI_COMM_WORLD);
}
#endif 

#if 0
void 
mpi_gather_com(
    tree_topology_t& tree,
    std::array<point_t,2>& range,
    std::vector<std::pair<entity_key_t,entity_key_t>>& rangeproc,
    std::vector<body_holder>& recv_COM
    )
{
  int rank,size;
  MPI_Comm_size(MPI_COMM_WORLD,&size);
  MPI_Comm_rank(MPI_COMM_WORLD,&rank);


  if(rank==0)
    std::cout<<"Gather COM"<<std::flush;
  MPI_Barrier(MPI_COMM_WORLD);

  // Reset the recv vector 
  recv_COM.clear();
  // Compute COM data for the current tree
  mpi_tree_traversal_com(tree);

  // Display my root data
  double mymass = tree.root()->getMass();
  //std::cout<<rank<<":"<<tree.root()->getPosition()<<";"
  //  <<tree.root()->getMass()<<std::endl;
  double total = 0.0;
  MPI_Reduce(&(mymass),&total,1,MPI_DOUBLE,
      MPI_SUM,0,MPI_COMM_WORLD); 
  //if(rank==0)
  //  std::cout<<"Total mass = "<<total<<std::endl;
  
  // Get the interesting branches for everyones. 
  // Do a traversal and create a vector for every other processes

  // Set a unique vector for everyone, to be contiguous 
  std::vector<body_holder> shared_COM;
  std::vector<int> shared_COM_size(size);
  std::vector<int> shared_COM_offsets(size);
  std::fill(shared_COM_size.begin(),shared_COM_size.end(),0);
  std::fill(shared_COM_offsets.begin(),shared_COM_offsets.end(),0);

  std::cout<<rank<<": keyrank="<<rangeproc[rank].first<<" : "<<rangeproc[rank].second<<std::endl;


  std::function<void(
    branch_t *,
    std::vector<body_holder>&,
    std::pair<entity_key_t,entity_key_t>&,
    int&)>traverse;

  traverse = [rank,&tree,&traverse,&range]
    (branch_t * b,
     std::vector<body_holder>& vbh,
     std::pair<entity_key_t,entity_key_t>& rangekeys,
     int& nelements)
  {
    // If in the range, go further 
    if(rangekeys.first<entity_key_t(range,b->getPosition())
        &&entity_key_t(range,b->getPosition())<rangekeys.second)
    {
      if(!b->is_leaf())
      {
        for(int i=0;i<(1<<gdimension);++i)
          traverse(tree.child(b,i),vbh,rangekeys,nelements);
      }else{
        // If I am a leaf check if one of my children is out of the range
        for(auto child: *b)
        {
          // Only for local particles 
          if(child->is_local()){
            if(rangekeys.first<entity_key_t(range,child->getPosition())
        &&entity_key_t(range,child->getPosition())<rangekeys.second)
            {
              vbh.push_back(body_holder(child->getPosition(),nullptr,rank,
                  child->getMass()));
              nelements++;
            }
          }   
        }
      }
    }else
    {
      // Mass = 0 just for elements I did not owe
      if(b->getMass()!=0)
      {
        // Not in range, stop going down and add in vector 
        vbh.push_back(body_holder(b->getPosition(),nullptr,rank,
              b->getMass()));
        nelements++;
      }
    } 
  };

  for(int i=0;i<size;++i)
  {
    int nelements = 0;
    // Do a tree traversal knowing the limits of every processes limits
    // Only take the keys that are outside
    //std::cout<<rank<<" search for "<<i<<": "<<rangeproc[i].first
    //  <<":"<<rangeproc[i].second<<std::endl;
    if(i!=rank)
      traverse(tree.root(),shared_COM,rangeproc[i],nelements);
    shared_COM_size[i]=nelements*sizeof(body_holder);
    if(i<size-2)
      shared_COM_offsets[i+1] = shared_COM_offsets[i]+shared_COM_size[i];
    //std::cout<<rank<<" for "<<i<<": "<<nelements<<std::endl;
  }

  std::vector<int> recvcount(size);
  std::vector<int> recvoffsets(size);
  // Share data size
  MPI_Alltoall(&shared_COM_size[0],1,MPI_INT,
      &recvcount[0],1,MPI_INT,MPI_COMM_WORLD);
  std::partial_sum(recvcount.begin(),recvcount.end(),&recvoffsets[0]); 
  // As we need an exscan, add a zero and delete the last element 
  recvoffsets.insert(recvoffsets.begin(),0);
 
  recv_COM.resize(recvoffsets.back()/sizeof(body_holder)); 
  // Share the data
  MPI_Alltoallv(&shared_COM[0],&shared_COM_size[0],&shared_COM_offsets[0],
      MPI_BYTE,&recv_COM[0],&recvcount[0],&recvoffsets[0],
      MPI_BYTE,MPI_COMM_WORLD); 

  if(rank==0)
    std::cout<<".done"<<std::endl<<std::flush;
  if(rank==0)
    std::cout<<"Total mass = "<<total<<std::endl;
  

  MPI_Barrier(MPI_COMM_WORLD);
}

#endif 

#if 0
bool 
MAC(
    body_holder* bi,
    branch_t* b,
    double gravradius,
    double macangle)
{
  double dmax = 2*b->getRadius();
  double distance_to_c = flecsi::distance(b->getPosition(),bi->getPosition());
  return dmax/distance_to_c < macangle;
}

void
traversal_COM_MAC_seq(
    tree_topology_t& tree, 
    body_holder* bi,
    branch_t *b,
    std::vector<body_holder>& bholders,
    double& gravradius,
    double& macangle)
{
  if(MAC(bi,b,gravradius,macangle))
  {
    // Add them to the vector 
    bholders.push_back(body_holder(b->getPosition(),nullptr,0,b->getMass()));
  }else{
    if(b->is_leaf())
    {
      // Directly add the children 
      for(auto bil: *b)
      {
        if(flecsi::distance(bil->getPosition(),bi->getPosition())>gravradius)
          bholders.push_back(body_holder(
                bil->getPosition(),nullptr,0,bil->getMass()));
      } 
    }else{
    // Go deeper 
    for(int i=0;i<(1<<gdimension);++i)
      traversal_COM_MAC_seq(tree,bi,tree.child(b,i),bholders,
          gravradius,macangle);
    }
  }
}
#endif
 
#if 0
void
mpi_branches_exchange_useful(tree_topology_t& tree,
    std::vector<std::pair<entity_key_t,body>>& rbodies,
    std::array<point_t,2>& rangekeys,
    std::vector<std::pair<entity_key_t,entity_key_t>>& ranges)
{
  int rank,size;
  MPI_Comm_rank(MPI_COMM_WORLD,&rank);
  MPI_Comm_size(MPI_COMM_WORLD,&size);
  MPI_Barrier(MPI_COMM_WORLD);
  if(rank==0)
    std::cout<<"Branches repartition" << std::flush;
  
  // Search for my min and max key needed
  std::pair<entity_key_t,entity_key_t> range;
  range.first = entity_key_t::last_key();
  range.second = entity_key_t::first_key();

  // Lowest key:
  auto minkey = std::min_element(rbodies.begin(),rbodies.end(),
      [](auto& left, auto& right)
      {
        return left.first < right.first; 
      });
  auto maxkey = std::max_element(rbodies.begin(),rbodies.end(),
      [](auto& left, auto& right)
      {
        return left.first < right.first; 
      });
  
  range.first = (*minkey).first;
  range.second = (*maxkey).first;
  //std::cout<<rank<<": "<<(*minkey).first<<"-"<<(*maxkey).first<<std::endl;

  // Point out of bounds
  auto outofbounds = [](point_t& p, point_t& limit) ->bool
  {
    for(size_t i=0;i<gdimension;++i)
      if(p[i]<limit[i])
        return true;
    return false;
  };

  // Get all my entities 
  for(auto ent: rbodies)
  {
    // TODO check validity for 3 dimensions
    point_t pos = ent.second.getPosition(); 
    double h = ent.second.getSmoothinglength()*2;

    // not considering less than the total range ! 
    point_t p = pos-h;
    if(!outofbounds(p,rangekeys[0]))
      if(entity_key_t(rangekeys,p) < range.first)
        range.first = entity_key_t(rangekeys,p);
    p = pos+h;
    if(!outofbounds(rangekeys[1],p))
      if(entity_key_t(rangekeys,p) > range.second)
        range.second = entity_key_t(rangekeys,p);
  }
  // Handle on the extremities for 0 and size-1
  if(range.first < entity_key_t::first_key())
    range.first = entity_key_t::first_key();

  if(range.second > entity_key_t::last_key())
    range.second = entity_key_t::last_key();

  //std::cout<<rank<<": "<<range.first<<" "<<range.second<<std::endl<<std::flush;
  //MPI_Barrier(MPI_COMM_WORLD);
  //exit(-1);

  // Gather the keys of everyone 
  if(ranges.size() == 0)
    ranges.resize(size);
  //std::vector<std::pair<entity_key_t,entity_key_t>> ranges(size);
  MPI_Allgather(&range,sizeof(std::pair<entity_key_t,entity_key_t>),
      MPI_BYTE,&ranges[0],sizeof(std::pair<entity_key_t,entity_key_t>),
      MPI_BYTE,MPI_COMM_WORLD);

  // Now generate the snedbufer, ordered by processes
  //  for the holders 
  std::vector<body_holder_mpi_t> sendbuffer;
  std::vector<int> sendcount(size);
  // To be able to search in the rbodies array 
  std::vector<std::pair<entity_key_t,body>> s1(size);
  std::vector<std::pair<entity_key_t,body>> s2(size);

  for(int i=0;i<size;++i)
  {
    s1[i].first=ranges[i].first;
    s2[i].first=ranges[i].second;
  }

  for(int i=0;i<size;++i)
  {
    if(i==rank)
    {
      sendcount[i]=0;
      continue;
    }
    auto lb = std::lower_bound(rbodies.begin(),rbodies.end(),s1[i],
        [](auto& left, auto& right)
        {
          return left.first < right.first; 
        });
    auto hb = std::upper_bound(rbodies.begin(),rbodies.end(),s2[i],
        [](auto& left, auto& right)
        {
          return left.first < right.first; 
        });
    // TODO check here, if last element is valid 
    if(lb != rbodies.end())
    {
      sendcount[i]=std::distance(lb,hb); 
      for(;lb!=hb;++lb)
        sendbuffer.push_back(
            body_holder_mpi_t{lb->second.coordinates(),rank,
            lb->second.getMass()}); 
    }else{
      sendcount[i]=0;
    } 
  }

  //int tot = 0;
  //for(auto val: sendcount)
  //{
  //  std::cout<<rank<<": sendto "<<tot<<"="<<val<<" from: "<<
  //    s1[tot].first<<";"<<s2[tot].first<<std::endl;
  //  tot++;
  //}

  //Count the elements to send 
  std::vector<int> sendoffsets(size);
  std::vector<int> recvcount(size);
  std::vector<int> recvoffsets(size);

  MPI_Alltoall(&sendcount[0],1,MPI_INT,
      &recvcount[0],1,MPI_INT,MPI_COMM_WORLD);

  int totalrecv = 0;
  for(int i=0;i<size;++i)
  {
    totalrecv += recvcount[i];
    sendcount[i]*=sizeof(body_holder_mpi_t);
    recvcount[i]*=sizeof(body_holder_mpi_t);
    if(i<size-1)
    {
      sendoffsets[i+1]=sendoffsets[i]+sendcount[i];
      recvoffsets[i+1]=recvoffsets[i]+recvcount[i];
    }
  }

  std::vector<body_holder_mpi_t> recvbuffer(totalrecv);
  MPI_Alltoallv(&sendbuffer[0],&sendcount[0],&sendoffsets[0],MPI_BYTE,
      &recvbuffer[0],&recvcount[0],&recvoffsets[0],MPI_BYTE,
      MPI_COMM_WORLD);

  // Add in the tree 
  for(auto bi: recvbuffer)
  {
    assert(bi.owner!=rank);
    auto nbi = tree.make_entity(bi.position,nullptr,bi.owner,bi.mass);
    tree.insert(nbi);
  }

  if(rank==0)
    std::cout<<".done"<<std::endl;
}
#endif 

#if 0
// The aim of this method is to shared the global informations on the tree 
// and the local informations with the process neighbors 
// Send all the body holders to everyone 
// Exchage all the tree informations
// Here we need to gather all the nodes and bodies info (key)
// The communication will propagate info in all the processes 
// For 4 processes :
// 0 <-> 1 2 <-> 3
// 0 <-> 2 1 <-> 3
void mpi_branches_exchange(tree_topology_t& tree)
{
  int rank,size;
  MPI_Comm_rank(MPI_COMM_WORLD,&rank);
  MPI_Comm_size(MPI_COMM_WORLD,&size);
  MPI_Barrier(MPI_COMM_WORLD);
  if(rank==0)
    std::cout<<"Branches repartition";

  int comms = log2(size);
  // TODO handle for non power of 2
  for(int comm=0;comm<comms;++comm)
  {
    int neighb = rank ^ (1 << comm);
    auto allents_ncontiguous = tree.entities();
    
    // Create a new vector to have contiguous memory TODO better way 
    std::vector<body_holder_mpi_t> allents;
    for(auto ent: allents_ncontiguous)
      allents.push_back(body_holder_mpi_t{ent->coordinates(),ent->getOwner()});
    
    // Send this to the neighb
    // 1. exchange to sizes
    int sendsize = allents.size()*sizeof(body_holder_mpi_t);
    int recvsize = 0;
    MPI_Sendrecv(&sendsize,1,MPI_INT,neighb,0,
      &recvsize,1,MPI_INT,neighb,MPI_ANY_TAG,
      MPI_COMM_WORLD,MPI_STATUS_IGNORE);

    std::vector<body_holder_mpi_t> recvents;
    recvents.resize(recvsize/sizeof(body_holder_mpi_t));
 
    MPI_Sendrecv(&allents[0],sendsize,MPI_BYTE,neighb,0,
      &recvents[0],recvsize,MPI_BYTE,neighb,MPI_ANY_TAG,
      MPI_COMM_WORLD,MPI_STATUS_IGNORE);

    // Add the entities in the tree, changing the ptr to nullptr
    for(auto bi: recvents)
    {
      assert(bi.owner!=rank);
      auto nbi = tree.make_entity(bi.position,nullptr,bi.owner,
          bi.mass);
      tree.insert(nbi);
    }
  }

  MPI_Barrier(MPI_COMM_WORLD); 
  if(rank == 0)
    std::cout<<".done"<<std::endl;
}
#endif 

#if 0
// This method sort the particles over all the processes
// Then proceed to balancing the particles over the processes
void mpi_sort(std::vector<std::pair<entity_key_t,body>>& rbodies,
    std::vector<int> targetnbodies)
{
  int rank,size;
  MPI_Comm_rank(MPI_COMM_WORLD,&rank);
  MPI_Comm_size(MPI_COMM_WORLD,&size);

  std::sort(rbodies.begin(),rbodies.end(),[](auto& left, auto& right){
    return left.first == right.first;    
  });

  // If there is just one process, skip the distribution
  if(size==1)
    return;
  // Normally the pivot position should be selected randomly 
  // Here we hae the assumption that at the begining the particles 
  // are approximetly well distributed then we consider:
  // p0 = 10000-11750
  // p1 = 11750-13500
  // p2 = 13500-15250
  // p3 = 15250-17777
  // In this case we does not need to share the pivots
  // In the random case we need to add a communication step
  std::vector<entity_key_t> pivots;
  entity_key_t rangeperproc = (entity_key_t::first_key()
      -entity_key_t::last_key())
    /(size); 
  //std::cout<<entity_key_t::first_key()<<std::endl; 
  //std::cout<<entity_key_t::last_key()<<std::endl;
  entity_key_t currentkey = entity_key_t::first_key();

  //if(rank==0)
  //  std::cout << "start: "<<currentkey<<std::endl;
  
  for(int i=0;i<size-1;++i)
  {
    currentkey = currentkey + rangeperproc; 
    pivots.push_back(currentkey); 
    //if(rank==0)
    //  std::cout << i << ": "<<currentkey<<std::endl;
  }

  // Then we create buckets based on this pivot 
  std::vector<std::vector<std::pair<entity_key_t,body>>> buckets;
  std::vector<int> bucketssize;
  bucketssize.resize(size);
  std::fill(bucketssize.begin(),bucketssize.end(),0);
  buckets.resize(size);
  
  for(std::vector<std::pair<entity_key_t,body>>::iterator 
      it =  rbodies.begin();
      it != rbodies.end();/*++it*/)
  {
    for(int i=0;i<size;++i){
      if(i == 0)
      {
        if(it->first < pivots[0]){
          buckets[0].push_back(std::move(*it)); 
          bucketssize[0]++;
          rbodies.erase(it);
        }
      }else if(i == size-1){
        // All the others elements should fit here ... test anyway
        if(it->first >= pivots[size-2]){
          buckets[i].push_back(std::move(*it));
          bucketssize[i]++;
          rbodies.erase(it);
        }
      }else{
        if(it->first <= pivots[i] && it->first > pivots[i-1]){
          buckets[i].push_back(std::move(*it));
          bucketssize[i]++;
          rbodies.erase(it);
        }
      }
    }
  }
  assert(rbodies.empty());

  //std::cout<<rank<<":"<<bucketssize[0]<<";"<<bucketssize[1]<<std::endl;

  // The vectors of vector are not contigous in memory
  // Let's append the vectors 
  std::vector<std::pair<entity_key_t,body>> sendbuffer;
  for(auto bucket: buckets)
  {
    std::move(bucket.begin(),bucket.end(),std::back_inserter(sendbuffer));
    bucket.erase(bucket.begin(),bucket.end());
  }
  
  // Share the bucket 
  // First send the sizes of each bucket 
  std::vector<int> receivbucket;
  receivbucket.resize(size);
  MPI_Alltoall(&bucketssize[0],1,MPI_INT,&receivbucket[0],1,MPI_INT,
      MPI_COMM_WORLD);

  // Create the offset for alltoallv
  //  First receiv offset
  std::vector<int> receivoffsets;
  receivoffsets.resize(size);
  std::partial_sum(receivbucket.begin(),receivbucket.end(),&receivoffsets[0]); 
  // As we need an exscan, add a zero and delete the last element 
  receivoffsets.insert(receivoffsets.begin(),0);
  //receivoffsets.pop_back(); 
  
  // Then send offsets
  std::vector<int> sendoffsets;
  sendoffsets.resize(size);
  std::partial_sum(bucketssize.begin(),bucketssize.end(),&sendoffsets[0]);
  // As we need an exscan, add a zero and delete the last element 
  sendoffsets.insert(sendoffsets.begin(),0);
  //sendoffsets.pop_back();
 
  // The receiv buffer, work in th rbodies
  //std::vector<std::pair<entity_key_t,body>> receivbuffer; 
  rbodies.resize(receivoffsets.back(),
      std::make_pair(entity_key_t::null(),body()));

  // We need to keep the last value to generate rbodies before erasing
  receivoffsets.pop_back(); 
  sendoffsets.pop_back();

  // Trnaform the offsets for bytes 
  for(auto& bs: bucketssize)
    bs*=sizeof(std::pair<entity_key_t,body>);
  for(auto& bs: sendoffsets)
    bs*=sizeof(std::pair<entity_key_t,body>);
  for(auto& bs: receivbucket)
    bs*=sizeof(std::pair<entity_key_t,body>);
  for(auto& bs: receivoffsets)
    bs*=sizeof(std::pair<entity_key_t,body>);

  // Use this array for the global buckets communication
  MPI_Alltoallv(&sendbuffer[0],&bucketssize[0],&sendoffsets[0],MPI_BYTE,
      &rbodies[0],&receivbucket[0],&receivoffsets[0],MPI_BYTE,
      MPI_COMM_WORLD);

  // Sort the incoming buffer 
  sort(rbodies.begin(),rbodies.end(),[](auto& left, auto &right)
  {
    return left.first<right.first;
  }); 

  // Check for duplicates
  assert(rbodies.end() == std::unique(rbodies.begin(),rbodies.end(),
      [](const auto& left, const auto& right ){ 
        return left.first == right.first;
      })
  );

  std::vector<int> totalnbodies;
  totalnbodies.resize(size);
  int mybodies = rbodies.size();
  // Share the final array size of everybody 
  MPI_Allgather(&mybodies,1,MPI_INT,&totalnbodies[0],1,MPI_INT,MPI_COMM_WORLD);

  if(rank == 0){
    std::cout<<"Repartition: ";
    for(auto num: totalnbodies)
      std::cout<<num<<";";
    std::cout<<std::endl;
  }

  //if(rank == 0){
  //  std::cout<<"Target: ";
  //  for(auto num: targetnbodies)
  //    std::cout<<num<<";";
  //  std::cout<<std::endl;
  //}

  // Distribution using full right and then full left
  // First full right, normally the worst if size iteration
  for(int i=0;i<size;++i)
  {  
    std::vector<int> needs(size);
    // Compute the current needs  
    for(int i=0;i<size;++i){
      needs[i] = targetnbodies[i]-totalnbodies[i];
    }  
    // Look at the right if someone have the bodies I need 
    if(rank!=0 && needs[rank]>0 && totalnbodies[rank-1]>0){
      int nrecv = needs[rank];
      if(totalnbodies[rank-1]<nrecv)
        nrecv = totalnbodies[rank-1];
      std::vector<std::pair<entity_key_t,body>> recvbuffer(nrecv);
      MPI_Recv(&recvbuffer[0],nrecv*sizeof(std::pair<entity_key_t,body>),
          MPI_BYTE,rank-1,0,MPI_COMM_WORLD,
          MPI_STATUS_IGNORE);
      // Add and keep sorted
      rbodies.insert(rbodies.end(),recvbuffer.begin(),
          recvbuffer.end());
      std::sort(rbodies.begin(),rbodies.end(),[]
          (auto& left,auto& right)
          {
            return left.first<right.first;
          });
    }
    if(rank!=size-1 && needs[rank+1]>0 && totalnbodies[rank]>0){
      int nsend = needs[rank+1];
      if(nsend>totalnbodies[rank])
        nsend = totalnbodies[rank];
      int position = rbodies.size() - nsend;
      MPI_Send(&(rbodies[position]),nsend*sizeof(std::pair<entity_key_t,body>),
          MPI_BYTE,rank+1,0,
          MPI_COMM_WORLD);
      // Suppress the bodies 
      rbodies.erase(rbodies.end()-nsend,rbodies.end()); 
    }
    // Gather new array size
    mybodies = rbodies.size();
    // Share the final array size of everybody 
    MPI_Allgather(&mybodies,1,MPI_INT,&totalnbodies[0],1,
        MPI_INT,MPI_COMM_WORLD);
  }

  // Now go to left  
  while(!std::equal(totalnbodies.begin(),totalnbodies.end(),
        targetnbodies.begin()))
  {
    
    std::vector<int> needs(size);
    // Compute the current needs  
    for(int i=0;i<size;++i){
      needs[i] = targetnbodies[i]-totalnbodies[i];
    }  
    // Look at the right if someone have the bodies I need 
    if(rank!=size-1 && needs[rank]>0 && totalnbodies[rank+1]>=needs[rank]){
      int nrecv = needs[rank];
      if(totalnbodies[rank+1]<nrecv)
        nrecv = totalnbodies[rank+1];
      std::vector<std::pair<entity_key_t,body>> recvbuffer(nrecv);
      MPI_Recv(&recvbuffer[0],nrecv*sizeof(std::pair<entity_key_t,body>),
          MPI_BYTE,rank+1,0,MPI_COMM_WORLD,
          MPI_STATUS_IGNORE);
      // Add and keep sorted
      rbodies.insert(rbodies.end(),recvbuffer.begin(),
          recvbuffer.end());
      std::sort(rbodies.begin(),rbodies.end(),[]
          (auto& left,auto& right)
          {
            return left.first<right.first;
          });
    }
    if(rank!=0 && needs[rank-1]>0 && totalnbodies[rank]>=needs[rank-1]){
      int nsend = needs[rank-1];
      if(nsend>totalnbodies[rank])
        nsend = totalnbodies[rank];
      int position = 0;
      MPI_Send(&rbodies[position],nsend*sizeof(std::pair<entity_key_t,body>),
          MPI_BYTE,rank-1,0,
          MPI_COMM_WORLD);
      // Suppress the bodies 
      rbodies.erase(rbodies.begin(),rbodies.begin()+nsend); 
    }
    // Gather new array size
    mybodies = rbodies.size();
    // Share the final array size of everybody 
    MPI_Allgather(&mybodies,1,MPI_INT,&totalnbodies[0],1,
        MPI_INT,MPI_COMM_WORLD);
  }
  if(rank == 0){
    std::cout<<"Repartition: ";
    for(auto num: totalnbodies)
      std::cout<<num<<";";
    std::cout<<std::endl;
  }

  // Display final particles 
  //for(auto bi: rbodies)
  //  std::cout<<rank<<": "<< bi.first <<std::endl;
}
#endif

