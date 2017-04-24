#ifndef main_driver_h
#define main_driver_h

#include <iostream>
#include <numeric> // For accumulate
#include <iostream>

#include <mpi.h>
#include <legion.h>
#include <omp.h>

#include "flecsi/execution/execution.h"
#include "flecsi/data/data_client.h"
#include "flecsi/data/data.h"

#include "mpi_partition.h"
#include "physics.h"
#include "io.h"

bool
operator==(
  const point_t& p1,
  const point_t& p2
)
{
  return p1[0]==p2[0]&&p1[1]==p2[1]&&p1[2]==p2[2];
}

namespace flecsi{
namespace execution{

void
mpi_task(/*const char * filename*/){
  const char * filename = "../data/data_test_40.txt";
  //std::vector<body*> rbodies; // Body read by the process

  int rank; 
  int size; 
  int nbodies = 0;
  int totalnbodies = 0;
  std::vector<std::pair<entity_key_t,body>> rbodies;
  tree_topology_t tree;

  MPI_Comm_size(MPI_COMM_WORLD,&size);
  MPI_Comm_rank(MPI_COMM_WORLD,&rank); 
  printf("%d/%d, file %s\n",rank,size,filename);   
  // Read data from file, each process read a part of it 
  // For HDF5, no problems because we know the number of particles 
  // For txt format, work on the number of lines yet ... 
  io::inputDataTxtRange(rbodies,nbodies,totalnbodies,rank,size,filename,tree); 
  //std::cout << "Read done" << std::endl;

  // Compute the range to compute the keys 
  double max[3] = {-9999,-9999,-9999};
  double min[3] = {9999,9999,9999};
  for(auto bi: rbodies){
    for(int i=0;i<3;++i){
        if(bi.second.coordinates()[i]>max[i])
          max[i] = bi.second.coordinates()[i];
        if(bi.second.coordinates()[i]<min[i])
          min[i] = bi.second.coordinates()[i];
      }
  }

  // Do the MPI Reduction 
  MPI_Allreduce(MPI_IN_PLACE,max,3,MPI_DOUBLE,MPI_MAX,MPI_COMM_WORLD); 
  MPI_Allreduce(MPI_IN_PLACE,min,3,MPI_DOUBLE,MPI_MIN,MPI_COMM_WORLD); 
 
  point_t minposition = {min[0]-0.1,min[1]-0.1,min[2]-0.1};
  point_t maxposition = {max[0]+0.1,max[1]+0.1,max[2]+0.1};

  if(rank==0)
    std::cout <<"boundaries: "<< minposition << maxposition << std::endl;

  std::array<point_t,2> range = {minposition,maxposition};

  // The bodies are loaded
  // Compute the key and sort them 
  for(auto& bi: rbodies){
    bi.first = entity_key_t(range,bi.second.coordinates());
    //std::cout << bi.first << std::endl;
  }

  // Check for duplicates keys, particles
  assert(rbodies.end() == 
      std::unique(rbodies.begin(),rbodies.end(),
        [](const auto& left, const auto& right)
        {
          return left.first == right.first;
        }
        )
  );
 
  // Target number of bodies for every process
  // The last one will takes more 
  std::vector<int> targetnbodies;
  for(int i=0;i<size;++i){
    if(i!=size-1){
      targetnbodies.push_back(totalnbodies/size);
    }else{ 
      targetnbodies.push_back(totalnbodies-((size-1)*(totalnbodies/size)));
    }
  }

  // Apply a distributed sort algorithm 
  mpi_sort(rbodies,targetnbodies);
  assert(rbodies.size() == targetnbodies[rank]); 
  assert(rbodies.end() == 
      std::unique(rbodies.begin(),rbodies.end(),
        [](const auto& left, const auto& right)
        {
          return left.second.coordinates()==right.second.coordinates() &&
            left.first == right.first;
        }
        )
  );

  // Use a new tree strcture or clear the old one 
  tree_topology_t ntree;
  // Recreate the bodies 
  std::vector<body*> bodies;
  for(auto bi: rbodies){
    auto nbi = ntree.make_entity(bi.second.getPosition(),
        bi.second.getVelocity(),
        bi.second.getVelocityhalf(),
        bi.second.getAcceleration(),
        bi.second.getDensity(),
        bi.second.getPressure(),
        bi.second.getEntropy(),
        bi.second.getMass(),
        bi.second.getSmoothinglength());
    bodies.push_back(nbi); 
  }

  // Generate the local tree 
  ntree.update_all(minposition,maxposition);
  for(auto bi: bodies){
    ntree.insert(bi); 
  }

  MPI_Barrier(MPI_COMM_WORLD);
  mpi_tree_traversal_graphviz(ntree,range);


  // Search and share the branches 
  //mpi_branches_exchange(tree);

  // Do the research of ghost and shared 
  
  // Index everything

  // Register data and create the final tree ?
  MPI_Barrier(MPI_COMM_WORLD);

}

flecsi_register_task(mpi_task,mpi,index);

void 
specialization_driver(int argc, char * argv[]){
  if (argc!=2) {
    std::cerr << "Error not enough arguments\n"
        "Usage: tree <datafile>\n";
    exit(-1); 
  }

  std::cout << "In user specialization_driver" << std::endl;
  /*const char * filename = argv[1];*/

  flecsi_execute_task(mpi_task,mpi,index/*,filename*/); 
} // specialization driver

void 
driver(int argc, char * argv[]){
  std::cout << "In user driver" << std::endl;
} // driver


} // namespace
} // namespace


#endif // tree_driver_h