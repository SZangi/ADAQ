////////////////////////////////////////////////////////////////////////////////
//
// name: AcquisitionManager.cc
// date: 20 Dec 23 (last updated)
// auth: Zach Hartwig
// mail: hartwig@psfc.mit.edu
//
// desc: The purpose of the AcquisitionManager class is to handle all
//       aspects of interfacing with the CAEN hardware for control and
//       readout. In this template (meant to be customized and
//       expanded by the user), we show how to use the ADAQDigitizer
//       class to program, arm, acquire waveform data, and safely
//       shutdown a CAEN digitizer unit in a standard acquisition
//       thread; control and readout of other hardware (e.g. high
//       voltage, VME controllers, etc.) can be easily added. While
//       basic functionality is provided within the acquisition loop
//       when data is retrieved, the user can implement whatever
//       method(s) they like to operate on the data.
//
//       To enable user-control during acquisition, two threads (using
//       the Boost library implementation) are created: an
//       "acquisition thread" that runs the acquisition loop to
//       readout and unpack digitized data; and a "control thread"
//       that enables the user to manually intervene to, for example,
//       terminate the acquisition process.
//
////////////////////////////////////////////////////////////////////////////////

// C++ 
#include <iostream>
using namespace std;

// Boost
#include <boost/assign/std/vector.hpp>
using namespace boost::assign;

#include "AcquisitionManager.hh"


AcquisitionManager::AcquisitionManager()
  : DGLinkOpen(false), Debug(false)
{
  //////////////////////////////////
  // Instantiate a digitizer manager
  
  DGManager = new ADAQDigitizer(zDT5790M,   // ADAQ-specified CAEN device type
				0,          // User-specified ID
				0x00000000, // Address in VME space
				0,          // USB link number
				0);         // CONET node number

  DGManager->SetVerbose(true);
}


AcquisitionManager::~AcquisitionManager()
{
  delete DGManager;
}


void AcquisitionManager::Arm()
{
  OpenConnection();
  
  if(DGFirmwareType == "STD" or DGFirmwareType == "PSD"){
    InitializeParameters();
    ProgramDigitizer();
  }
  else{
    cout << "AcquisitionManager (main thread) : Error! Only STD and PSD firmware is presently supported! Exiting...\n"
	 << endl;
    Disarm();
    exit(-42);
  }
}


void AcquisitionManager::OpenConnection()
{
  /////////////////////////////////
  // Open VME link to the digitizer
  
  cout << "AcquisitionManager (main thread) : Opening a link to the digitizer ...\n"
       << endl;
  
  int DGStatus = DGManager->OpenLink();
  (DGStatus==0) ? DGLinkOpen = true : DGLinkOpen = false;

  if(DGLinkOpen){
    cout << "AcquisitionManager (main thread) : A link to the digitizer has been established!\n"
	 << endl;
    DGNumChannels = DGManager->GetNumChannels();
    DGFirmwareType = DGManager->GetBoardFirmwareType();
  }
  else{
    cout << "AcquisitionManager (main thread) : Error! A link to the digitizer could not be established! Exiting...\n"
	 << endl;
    exit(-42);
  }
}


void AcquisitionManager::InitializeParameters()
{
  if(!DGLinkOpen)
    return;
  
  //////////////////////////////
  // Firmware-agnostic variables
  
  for(int ch=0; ch<DGNumChannels; ch++){
    ChEnabled.push_back(false);
    ChPulsePolarity.push_back(CAEN_DGTZ_PulsePolarityPositive);
    ChDCOffset.push_back(0x8000);
  }

  ChEnabled[0] = true;
  ChannelEnableMask = DGManager->CalculateChannelEnableMask(ChEnabled);

  EventsBeforeReadout = 1;

  
  //////////////////////////////////
  // STD firmware specific variables
  
  if(DGFirmwareType == "STD"){

    // Control variables

    RecordLength = 512;
    PostTriggerSize = 50;
    for(int ch=0; ch<DGNumChannels; ch++){
      ChTriggerThreshold.push_back(2000);
      ChBaselineCalcMin.push_back(0);
      ChBaselineCalcMax.push_back(50);
    }

    // Readout variables

    Buffer = NULL;
    BufferSize = 0;
    
    EventPointer = NULL;
    EventWaveform = NULL;
   
    Waveforms.resize(DGManager->GetNumChannels());
    for(int ch=0; ch<DGManager->GetNumChannels(); ch++)
      Waveforms[ch].resize(RecordLength);
  }


  /////////////////////////////////
  // PSD firmware specific variable

  else if(DGFirmwareType == "PSD"){

    // Control variables
    
    for(int ch=0; ch<DGNumChannels; ch++){
      PSDParameters.thr[ch] = 100;
      PSDParameters.nsbl[ch] = 2;
      PSDParameters.lgate[ch] = 40;
      PSDParameters.sgate[ch] = 10;
      PSDParameters.pgate[ch] = 10;
      PSDParameters.selft[ch] = 0;
      PSDParameters.csens[ch] = 0;
      PSDParameters.tvaw[ch] = 10;
      PSDParameters.trgc[ch] = CAEN_DGTZ_DPP_TriggerConfig_Threshold;

      PSDChRecordLength.push_back(64);
      PSDChPreTrigger.push_back(40);
    }
    PSDParameters.purh = CAEN_DGTZ_DPP_PSD_PUR_DetectOnly;
    PSDParameters.purgap = 100;
    PSDParameters.blthr = 5;
    PSDParameters.bltmo = 100;
    PSDParameters.trgho = 10;
    
    // Readout variables
    
    Buffer = NULL;
    BufferSize = 0;
    PSDEventSize = 0;
    for(int ch=0; ch<DGNumChannels; ch++)
      NumPSDEvents[ch] = 0;
    PSDWaveforms = NULL;
  }
  
  else{
    cout << "\nAcquisitionManager (main thread) : Error! Firmware type '" << DGFirmwareType << "' is not supported\n!"
	 << endl;
    return;
  }
}


void AcquisitionManager::ProgramDigitizer()
{
  if(!DGLinkOpen)
    return;

  // Reset the digitizer
  //  DGManager->Initialize();
  DGManager->Reset();
  //  DGManager->ClearData();

  /////////////////////////////////////////
  // Program the STD firmware digitizers //
  /////////////////////////////////////////

  if(DGFirmwareType == "STD"){

    /*
    for(int ch=0; ch<DGNumChannels; ch++){
      DGManager->SetChannelTriggerThreshold(ch, ChTriggerThreshold[ch]);
      DGManager->SetChannelDCOffset(ch, ChDCOffset[ch]);

      if(ChPosPolarity[ch])
	DGManager->SetChannelPulsePolarity(ch, CAEN_DGTZ_PulsePolarityPositive);
      else
	DGManager->SetChannelPulsePolarity(ch, CAEN_DGTZ_PulsePolarityNegative);

      if(TriggerEdgeRising)
	DGManager->SetTriggerEdge(ch, "Rising");
      else
	DGManager->SetTriggerEdge(ch, "Falling");
    }

    DGManager->SetChannelEnableMask(ChannelEnableMask);
    DGManager->SetRecordLength(RecordLength);
    DGManager->SetPostTriggerSize(PostTriggerSize);
    DGManager->SetAcquisitionControl("Software");
    DGManager->SetZSMode("None");
    DGManager->SetMaxNumEventsBLT(EventsBeforeReadout);

    if(TriggerTypeAutomatic)
      DGManager->EnableAutoTrigger(ChannelEnableMask);
    else
      DGManager->DisableAutoTrigger(ChannelEnableMask);

    if(TriggerTypeSoftware)
      DGManager->EnableSWTrigger();
    else
      DGManager->DisableSWTrigger();

    DGManager->DisableExternalTrigger();
    */
  }
  
  /////////////////////////////////////////
  // Program digitizer with PSD firmware //
  /////////////////////////////////////////

  else if(DGFirmwareType == "PSD"){

    DGManager->SetChannelEnableMask(ChannelEnableMask);
    DGManager->SetAcquisitionControl("Software");
    DGManager->SetIOLevel(CAEN_DGTZ_IOLevel_TTL);
    DGManager->SetRunSynchronizationMode(CAEN_DGTZ_RUN_SYNC_Disabled);

    DGManager->DisableExternalTrigger();
    DGManager->DisableAutoTrigger(ChannelEnableMask);
    DGManager->EnableSWTrigger();

    DGManager->SetDPPAcquisitionMode(CAEN_DGTZ_DPP_ACQ_MODE_Mixed, CAEN_DGTZ_DPP_SAVE_PARAM_EnergyAndTime);
    DGManager->SetDPPEventAggregation(EventsBeforeReadout,0);
    DGManager->SetDPPTriggerMode(CAEN_DGTZ_DPP_TriggerMode_Normal);
    DGManager->SetDPPParameters(ChannelEnableMask, &PSDParameters);
    
    for(int ch=0; ch<DGNumChannels; ch++){
      DGManager->SetRecordLength(PSDChRecordLength[ch], ch);
      DGManager->SetChannelDCOffset(ch, ChDCOffset[ch]);
      DGManager->SetChannelPulsePolarity(ch, ChPulsePolarity[ch]);
      DGManager->SetDPPPreTriggerSize(ch, PSDChPreTrigger[ch]);
    }

    DGManager->MallocReadoutBuffer(&Buffer, &BufferSize);
    DGManager->MallocDPPEvents(PSDEvents, &PSDEventSize);
    DGManager->MallocDPPWaveforms(&PSDWaveforms, &PSDWaveformSize); 
  }

  if(!DGManager->CheckForEnabledChannels()){
    cout << "\nAcquisitionManager (main thread) : Warning! No digitizer channels were enabled, ie, ChannelEnableMask==0!\n"
	 << endl;
  }
}


void AcquisitionManager::RunAcquisitionLoop()
{
  if(!DGLinkOpen)
    return;

  if(Debug)
    RunDebugLoop();
    
  cout << "AcquisitionManager (acquisition thread) : Beginning waveform acquisition...\n"
       << endl;
  
  DGManager->SWStartAcquisition();

  if(DGFirmwareType == "STD"){
    /*
    
    // Determine the number of events in the buffer
    DGManager->GetNumEvents(Buffer, BufferSize, &PCEvents);

    // If there are no events in the current buffer then continue in
    // the 'while' loop without executing the CPU intensive 'for'
    // loops on the next lines. This maximizes code efficiency and
    // only scans/processes events when there is need to do so.
    if(PCEvents==0)
      continue;
    
    // For each event in the PC memory buffer...
    for(uint32_t evt=0; evt<PCEvents; evt++){
      
      // Get the event information
      DGManager->GetEventInfo(Buffer, BufferSize, evt, &EventInfo, &EventPointer);
      
      // Decode the event and obtain the waveform (voltage as a function of time)
      DGManager->DecodeEvent(EventPointer, &EventWaveform);
      
      // If there is no waveform in the PC buffer, continue in the
      // while loop to avoid segfaulting
      if(EventWaveform==NULL)
	continue;
      
      // For each channel...
      for(int ch=0; ch<DGManager->GetNumChannels(); ch++){
	
	// Only proceed to waveform analysis if the channel is enabled
	if(!ChEnabled[ch])
	  continue;

	for(uint32_t sample=0; sample<RecordLength; sample++){
	  // Get the digitized voltage in units of analog-to-digital conversion bits
	  Voltage[sample] = EventWaveform->DataChannel[ch][sample]; // [ADC]
	}
      }

      TotalEvents++;
      
      if(TotalEvents % 1 == 0){
	cout << "\r" << flush;
	cout << "Accumulated events = " << TotalEvents << flush;
      }
    }
    */
  }

  else if(DGFirmwareType == "PSD"){
    
    while(true){

      boost::this_thread::interruption_point();
      
      DGManager->ReadData(Buffer, &BufferSize);
      
      if(BufferSize==0)
      	continue;
      
      DGManager->GetDPPEvents(Buffer, BufferSize, PSDEvents, NumPSDEvents);

      for(int ch=0; ch<DGNumChannels; ch++){
	
	if(!(ChannelEnableMask & (1<<ch)))
	  continue;
	
	for(int evt=0; evt<NumPSDEvents[ch]; evt++){
	  
	  DGManager->DecodeDPPWaveforms(&PSDEvents[ch][evt], PSDWaveforms);
	  
	  int Size = (int)PSDWaveforms->Ns;
	  uint16_t *Voltage = PSDWaveforms->Trace1;

	  for(int i=0; i<Size; i++)
	    cout << i << " " << Voltage[i] << endl;
	}
      }
    }
  }
}


void AcquisitionManager::RunDebugLoop()
{
  while(true){
    boost::posix_time::milliseconds Time(500);
    cout << "AcquisitionManager (acquisition thread) : Acquisition loop is running in debug mode with 500 ms cycles" << endl;
    boost::this_thread::sleep(Time);
    boost::this_thread::interruption_point();
  }
}


void AcquisitionManager::RunControlLoop(boost::thread *Acquisition_thread)
{
  cout << "AcquisitionManager (control thread) : Enter a '0' to terminate acquisition!\n" 
       << "                                      Enter 1 '1' to trigger the digitizer!\n" 
       << endl;

  int Choice = -1;
  bool RunLoop = true;

  while(RunLoop){
    cin >> Choice;
    
    if(Choice == 0){
      cout << "AcquisitionManager (control thread) : Acquisition loop terminated!\n"
	   << endl;
      Acquisition_thread->interrupt();
      DGManager->SWStopAcquisition();
      RunLoop = false;
    }
    else if(Choice == 1){
      cout << "AcquisitionManager (control thread) : Manual trigger!\n"
	   << endl;
      DGManager->SendSWTrigger();
    }
  }
}      


void AcquisitionManager::Disarm()
{
  DGManager->FreeReadoutBuffer(&Buffer);

  if(DGFirmwareType == "STD"){
    DGManager->FreeEvent(&EventWaveform);
  }
  else if(DGFirmwareType == "PSD"){
    DGManager->FreeDPPEvents((void**)PSDEvents);
    DGManager->FreeDPPWaveforms(PSDWaveforms);
  }

  DGManager->CloseLink();
}