/////////////////////////////////////////////////////////////////////////////////
//
// name: MPIManager.cc
// date: 28 Nov 21
// auth: Zach Hartwig
// mail: hartwig@psfc.mit.edu
//
// desc: The MPIManager is a meyer's singleton class that provides the
//       ability to rapidly parallelize a user's Geant4 simulation for
//       use on multicore or high-performance computing systems. It is
//       generic and can be integrated into any Geant4 simulation for
//       straightforward paralellization using Open MPI. The
//       MPIMessenger class is a complementary class to enable
//       particle source (e.g. /run/beamOn) in parallel processing.
//
//       Integration of MPI into Geant4 simulation is simple:
//
//       1. In the Geant4 simulation Makefile, include the ASIM header
//          directory and link the binary against the ASIM library in
//          the Geant4 Makefile. Add the MPI_ENABLED C++ compiler
//          macro to enable excluding Open MPI code, which enables the
//          simulation to be optionally built on systems without Open
//          MPI installed
//
//       2. Declare a concrete instance of the MPIManager (it is a
//          singleton) and utilize within the simulation
//
//       This version has MPIManager has been compiled and tested with
//       OpenMPI version 4.1.2. Note that as of OpenMPI 2.X.X, all C++
//       bindings were depracated; MPIManager (as of 28 Nov 21) has
//       been updated to use the standard C bindings for compliance.
//
/////////////////////////////////////////////////////////////////////////////////

// Exclude MPI-relevant code from sequential simulation builds
#ifdef MPI_ENABLED

// Geant4
#include "G4RunManager.hh"

// C/C++
#include "limits.h"

// MPI
#include "mpi.h"

// ASIM
#include "MPIManager.hh"
#include "MPIMessenger.hh"


MPIManager *MPIManager::theMPImanager = 0;


MPIManager *MPIManager::GetInstance()
{ return theMPImanager; }


MPIManager::MPIManager(int argcMPI, char **argvMPI)
{
  // Initialize the MPI environment with thread-level support,
  // ensuring only 1 thread can call the MPI libraries at a time
  threadSupport = 0;
  MPI_Init_thread(&argcMPI, &argvMPI, MPI_THREAD_SERIALIZED, &threadSupport);
    
  // Get the size (number of) and the rank the present process
  MPI_Comm_size(MPI_COMM_WORLD, &size);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  
  // Set G4bools for master/slave identification
  isMaster = (rank == RANK_MASTER);
  isSlave = (rank != RANK_MASTER);

  // If the present process is a slave, redirect its std::output to
  // the slaveForum, a special directory for output. The output file
  // is the base specified by argvMPI[1] combined with the slave rank.
  if(isSlave){
    G4String fBase = argvMPI[1];
    std::ostringstream slaveRank;
    slaveRank << rank;
    G4String fName = fBase + slaveRank.str() + ".out";

    // Redirect G4cout to the slave output file
    slaveOut.open(fName.c_str(), std::ios::out);
    G4cout.rdbuf(slaveOut.rdbuf());
  }

  // Initialize the number of events to be processed on all nodes
  totalEvents = 0;
  masterEvents = 0;
  slaveEvents = 0;

  // Initialize distribute events bool
  distributeEvents = true;

  // Create the seeds to ensure randomness in each node
  CreateSeeds();

  // Distribute the seeds to the nodes
  DistributeSeeds();

  if(theMPImanager)
    G4Exception("MPIManager::MPIManager()",
		"MPIManager-Exception00",
		FatalException,
		"The MPIManager singleton was constructed twice!\n");
  
  theMPImanager = this;

  theMPImessenger = new MPIMessenger(this);
}


MPIManager::~MPIManager()
{
  delete theMPImessenger;

  if(isSlave and slaveOut.is_open())
    slaveOut.close();
  
  MPI_Finalize(); 
}


// An MPI specific method to run the beam (primary particles),
// allowing the user to specify the number of events to run, as well
// as whether or not to distribute those events evenly across the
// available nodes or to run the same number of events on all
// nodes. Note that "events" is of type G4double in order to handle
// a total number of events greater then 2.1e9 (range of G4int data type)
void MPIManager::BeamOn(G4double events, G4bool distribute)
{
  // Set distribute events bool
  distributeEvents = distribute;

  // Obtain the run mananager
  G4RunManager *runManager = G4RunManager::GetRunManager();

  // If set, distribute the total events to be processed evenly across
  // all available nodes, assigning possible "remainders" to master
  if(distributeEvents){ 
    
    slaveEvents = G4int(events/size);
    masterEvents = G4int(events-slaveEvents*(size-1));
    
    if(isMaster) {
      G4cout << "\nMPIManager : # events in master = " << masterEvents 
	     << " / # events in slave = "  << slaveEvents << "\n" << G4endl;
    }
    
    totalEvents = events;

    // Error check to ensure events < range_G4int
    if(masterEvents > 2e9 or slaveEvents > 2e9)
      ThrowEventError();
    
    // "Prepare to run the beam!"  "You're always preparing!  Just run
    // it!"  "Sir, you already used that joke in SWS.cc..."
    if(isMaster) 
      G4RunManager::GetRunManager()->BeamOn(masterEvents);
    else
      G4RunManager::GetRunManager()->BeamOn(slaveEvents);
  }
  
  // Otherwise, each node will run totalEvents number of events
  else {

    slaveEvents = G4int(events);
    masterEvents = G4int(events);

    if(isMaster)
      G4cout << "\nMPIManager : # events in master = " << masterEvents 
	     << " / # events in slave = "  << slaveEvents << G4endl;
    
    // Error check to ensure events < range_G4int
    if(events>2e9)
      ThrowEventError();
      
    // Store the total number of events on all nodes
    totalEvents = events*size;  
    
     // Ruuuuuuuuuuuuuuuuun that baby!
    runManager->BeamOn(G4int(events));
  }
}


void MPIManager::ThrowEventError()
{
  // Because events variable must be passed to the G4RunManager,
  // which is expecting a G4int, unspecified behavior could result
  // if the events variable (which is a G4double and can exceed the
  // 2.1e9 data range of G4int) is actually passed. Therefore,
  // ensure that events is < 2.1e9 if the user has chosen NOT to
  // distribute events across the slaves
  G4cout << "\nMPIManager : You have chosen to run (n_events > n_G4int_range) on each of the \n"
	 <<   "             slaves! Since MPIManager passes n_events to the runManager\n"
	 <<   "             this could result in unspecified behavior. Please choose another\n"
	 <<   "             number of primary events or distribute them across nodes such that\n"
	 <<   "             (n_events < n_G4int_range = 2,147,483,647). SWS will now abort...\n"
	 << G4endl;
  
  G4Exception("MPIManager::ThrowEventError()",
	      "MPIManagerException002",
	      FatalException,
	      "MPIManager : Crashing this ship like the Hindenburg!\n");
}


// Method to create randomized seeds for each node.  I currently store
// the seeds in vector in case I want them later on in the executable.
// I may also want to replicate runs, in which case a master seed that
// generates subsequent seeds should be used.

void MPIManager::CreateSeeds()
{
  // Create a list of seeds that will be distributed to all the nodes
  for(G4int i=0; i<size; i++){
    G4double x = G4UniformRand();
    G4long seed = G4long(x*LONG_MAX);
    seedPacket.push_back(seed);
  }
  
  // Check to ensure that no two seeds are alike
  G4bool doubleCount = false;
  while(doubleCount){
    for(G4int i=0; i<size; i++)
      for(G4int j=0; j<size; j++)
	
	// If two seeds are the same, create a new seed
	if( (i!=j) and (seedPacket[i] == seedPacket[j]) ){
	  G4double x = G4UniformRand();
	  seedPacket[j] = G4long(x*LONG_MAX);
	  doubleCount=true;
	}
    doubleCount = false;
  }
}

// Distribute the seeds to the random number generator for each node
void MPIManager::DistributeSeeds()
{
  /*
  if(rank==0){
    for(size_t i=0; i<seedPacket.size(); i++)
      G4cout << "Rank[" << i << "] seed = " << seedPacket[i] << G4endl;
  }
  */
  CLHEP::HepRandom::setTheSeed(seedPacket[rank]);
}


// MPI_Barrier forces all nodes to reach a common point before
// proceeding. Useful to ensure that nodes are synchronized before
// beginning key operations
void MPIManager::ForceBarrier(G4String location)
{ 
  MPI_Barrier(MPI_COMM_WORLD);
  
  G4cout << "\n\nMPIManager : All nodes have reached the MPI barrier at " << location << "!\n"
         <<     "             Nodes now proceeding onward ..."
	 << G4endl;
}


// MPI_Reduce operates on all nodes to produce a single value on the
// specified node by using an predefined MPI operation.

// Sum all doubles into a single value on the master
G4double MPIManager::SumDoublesToMaster(G4double slaveValue)
{
  G4double masterSum = 0.;
  MPI_Reduce(&slaveValue, &masterSum, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
  return masterSum;
}

// Sum all integers into a single value on the master
G4int MPIManager::SumIntsToMaster(G4int slaveValue)
{
  G4int masterSum = 0;
  MPI_Reduce(&slaveValue, &masterSum, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);
  return masterSum;
}

#endif
