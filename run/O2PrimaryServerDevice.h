// Copyright CERN and copyright holders of ALICE O2. This software is
// distributed under the terms of the GNU General Public License v3 (GPL
// Version 3), copied verbatim in the file "COPYING".
//
// See http://alice-o2.web.cern.ch/license for full licensing information.
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

/// @author Sandro Wenzel

#ifndef O2_DEVICES_PRIMSERVDEVICE_H_
#define O2_DEVICES_PRIMSERVDEVICE_H_

#include <FairMQDevice.h>
#include <FairMQTransportFactory.h>
#include <FairPrimaryGenerator.h>
#include <Generators/GeneratorFactory.h>
#include <FairMQMessage.h>
#include <SimulationDataFormat/Stack.h>
#include <SimulationDataFormat/MCEventHeader.h>
#include <TMessage.h>
#include <TClass.h>
#include <SimulationDataFormat/PrimaryChunk.h>
#include <Generators/GeneratorFromFile.h>
#include <Generators/PrimaryGenerator.h>
#include <SimConfig/SimConfig.h>
#include <CommonUtils/ConfigurableParam.h>
#include <CommonUtils/RngHelper.h>
#include "Field/MagneticField.h"
#include <TGeoGlobalMagField.h>
#include <typeinfo>
#include <thread>
#include <TROOT.h>
#include <TStopwatch.h>
#include <fstream>
#include <iostream>
#include "PrimaryServerState.h"
#include "SimPublishChannelHelper.h"

namespace o2
{
namespace devices
{

class O2PrimaryServerDevice final : public FairMQDevice
{
 public:
  /// constructor
  O2PrimaryServerDevice() = default;

  /// Default destructor
  ~O2PrimaryServerDevice() final
  {
    if (mGeneratorThread.joinable()) {
      mGeneratorThread.join();
    }
  }

 protected:
  void initGenerator()
  {
    mState = O2PrimaryServerState::Initializing;
    TStopwatch timer;
    timer.Start();
    const auto& conf = mSimConfig;

    // init magnetic field as it might be needed by the generator
    if (TGeoGlobalMagField::Instance()->GetField() == nullptr) {
      auto field = o2::field::MagneticField::createNominalField(conf.getConfigData().mField, conf.getConfigData().mUniformField);
      TGeoGlobalMagField::Instance()->SetField(field);
      TGeoGlobalMagField::Instance()->Lock();
    }

    // look if we find a cached instances of Pythia8 or external generators in order to avoid
    // (long) initialization times.
    // This is evidently a bit weak, as generators might need reconfiguration (to be treated later).
    // For now, we'd like to allow for fast switches between say a pythia8 instance and reading from kinematics
    // to continue an already started simulation.
    //
    // Not using cached instances for external kinematics since these might change input filenames etc.
    // and are in any case quickly setup.
    mPrimGen = nullptr;
    if (conf.getGenerator().compare("extkin") != 0 || conf.getGenerator().compare("extkinO2") != 0) {
      auto iter = mPrimGeneratorCache.find(conf.getGenerator());
      if (iter != mPrimGeneratorCache.end()) {
        mPrimGen = iter->second;
        LOG(INFO) << "Found cached generator for " << conf.getGenerator();
      }
    }

    if (mPrimGen == nullptr) {
      mPrimGen = new o2::eventgen::PrimaryGenerator;
      o2::eventgen::GeneratorFactory::setPrimaryGenerator(conf, mPrimGen);

      auto embedinto_filename = conf.getEmbedIntoFileName();
      if (!embedinto_filename.empty()) {
        mPrimGen->embedInto(embedinto_filename);
      }

      mPrimGen->Init();

      mPrimGeneratorCache[conf.getGenerator()] = mPrimGen;
    }
    mPrimGen->SetEvent(&mEventHeader);

    LOG(INFO) << "Generator initialization took " << timer.CpuTime() << "s";
    generateEvent(); // generate a first event
  }

  // function generating one event
  void generateEvent()
  {
    LOG(INFO) << "Event generation started ";
    mState = O2PrimaryServerState::WaitingEvent;
    TStopwatch timer;
    timer.Start();
    mStack->Reset();
    mPrimGen->GenerateEvent(mStack);
    timer.Stop();
    LOG(INFO) << "Event generation took " << timer.CpuTime() << "s"
              << " and produced " << mStack->getPrimaries().size() << " primaries ";
    mState = O2PrimaryServerState::ReadyToServe;
  }

  // launches a thread that listens for status requests from outside asynchronously
  void launchStatusThread()
  {
    static std::vector<std::thread> threads;
    LOG(INFO) << "LAUNCHING STATUS THREAD";
    auto lambda = [this]() {
      auto& channel = fChannels.at("primary-status").at(0);
      if (channel.IsValid()) {
        LOG(INFO) << "CHANNEL IS VALID";
      } else {
        LOG(INFO) << "CHANNEL IS NOT VALID";
      }
      while (mState != O2PrimaryServerState::Stopped) {
        std::unique_ptr<FairMQMessage> request(channel.NewMessage());
        if (channel.Receive(request, 500) > 0) {
          LOG(INFO) << "Received status request";
          std::unique_ptr<FairMQMessage> reply(channel.NewSimpleMessage((int)mState));
          if (channel.Send(reply) > 0) {
            LOG(INFO) << "Send successful";
          }
        }
      }
    };
    threads.push_back(std::thread(lambda));
    threads.back().detach();
  }

  void InitTask() final
  {
    o2::simpubsub::publishMessage(fChannels["primary-notifications"].at(0), "SERVER : INITIALIZING");

    mState = O2PrimaryServerState::Initializing;
    LOG(INFO) << "Init Server device ";

    launchStatusThread();

    // init sim config
    auto& conf = o2::conf::SimConfig::Instance();
    auto& vm = GetConfig()->GetVarMap();
    conf.resetFromParsedMap(vm);
    // output varmap
    for (auto& keyvalue : vm) {
      LOG(INFO) << "///// " << keyvalue.first << " " << keyvalue.second.value().type().name();
    }
    // update the parameters from an INI/JSON file, if given (overrides code-based version)
    o2::conf::ConfigurableParam::updateFromFile(conf.getConfigFile());
    // update the parameters from stuff given at command line (overrides file-based version)
    o2::conf::ConfigurableParam::updateFromString(conf.getKeyValueString());

    // from now on mSimConfig should be used within this process
    mSimConfig = conf;

    mStack = new o2::data::Stack();
    mStack->setExternalMode(true);

    // MC ENGINE
    LOG(INFO) << "ENGINE SET TO " << vm["mcEngine"].as<std::string>();
    // CHUNK SIZE
    mChunkGranularity = vm["chunkSize"].as<unsigned int>();
    LOG(INFO) << "CHUNK SIZE SET TO " << mChunkGranularity;

    // initial initial seed --> we should store this somewhere
    mInitialSeed = vm["seed"].as<int>();
    mInitialSeed = o2::utils::RngHelper::setGRandomSeed(mInitialSeed);
    LOG(INFO) << "RNG INITIAL SEED " << mInitialSeed;

    mMaxEvents = conf.getNEvents();

    // need to make ROOT thread-safe since we use ROOT services in all places
    ROOT::EnableThreadSafety();

    // lunch initialization of particle generator asynchronously
    // so that we reach the RUNNING state of the server quickly
    // and do not block here
    mGeneratorThread = std::thread(&O2PrimaryServerDevice::initGenerator, this);

    // init pipe
    auto pipeenv = getenv("ALICE_O2SIMSERVERTODRIVER_PIPE");
    if (pipeenv) {
      mPipeToDriver = atoi(pipeenv);
      LOG(INFO) << "ASSIGNED PIPE HANDLE " << mPipeToDriver;
    } else {
      LOG(INFO) << "DID NOT FIND ENVIRONMENT VARIABLE TO INIT PIPE";
    }

    mAsService = vm["asservice"].as<bool>();
  }

  // function for intermediate/on-the-fly reinitializations
  bool ReInit(o2::conf::SimReconfigData const& reconfig)
  {
    LOG(INFO) << "ReInit Server device ";

    if (reconfig.stop) {
      return false;
    }

    // mSimConfig.getConfigData().mKeyValueTokens=reconfig.keyValueTokens;
    // Think about this:
    // update the parameters from an INI/JSON file, if given (overrides code-based version)
    o2::conf::ConfigurableParam::updateFromFile(reconfig.configFile);
    // update the parameters from stuff given at command line (overrides file-based version)
    o2::conf::ConfigurableParam::updateFromString(reconfig.keyValueTokens);

    // initial initial seed --> we should store this somewhere
    mInitialSeed = reconfig.startSeed;
    mInitialSeed = o2::utils::RngHelper::setGRandomSeed(mInitialSeed);
    LOG(INFO) << "RNG INITIAL SEED " << mInitialSeed;

    mMaxEvents = reconfig.nEvents;

    // updating the simconfig member with new information especially concerning the generators
    // TODO: put this into utility function?
    mSimConfig.getConfigData().mGenerator = reconfig.generator;
    mSimConfig.getConfigData().mTrigger = reconfig.trigger;
    mSimConfig.getConfigData().mExtKinFileName = reconfig.extKinfileName;

    mEventCounter = 0;
    mPartCounter = 0;
    mNeedNewEvent = true;
    // reinit generator and start generation of a new event
    mGeneratorThread = std::thread(&O2PrimaryServerDevice::initGenerator, this);

    return true;
  }

  // method reacting to requests to get the simulation configuration
  bool HandleConfigRequest(FairMQMessagePtr& request)
  {
    LOG(INFO) << "received config request";
    // just sending the simulation configuration to anyone that wants it
    const auto& confdata = mSimConfig.getConfigData();

    TMessage* tmsg = new TMessage(kMESS_OBJECT);
    tmsg->WriteObjectAny((void*)&confdata, TClass::GetClass(typeid(confdata)));

    auto free_tmessage = [](void* data, void* hint) { delete static_cast<TMessage*>(hint); };

    std::unique_ptr<FairMQMessage> message(
      fTransportFactory->CreateMessage(tmsg->Buffer(), tmsg->BufferSize(), free_tmessage, tmsg));

    // send answer
    if (Send(message, "primary-get", 0) > 0) {
      LOG(INFO) << "config reply send ";
      return true;
    }
    return true;
  }

  bool ConditionalRun() override
  {
    auto& channel = fChannels.at("primary-get").at(0);
    std::unique_ptr<FairMQMessage> request(channel.NewMessage());
    auto bytes = channel.Receive(request);
    if (bytes < 0) {
      LOG(ERROR) << "Some error occurred on socket during receive";
      return true; // keep going
    }
    auto more = HandleRequest(request, 0);
    if (!more) {
      LOG(INFO) << "GOING IDLE";
      mState = O2PrimaryServerState::Idle;
      if (mAsService) {
        LOG(INFO) << "WAITING FOR CONTROL INPUT";
        more = waitForControlInput();
      }
    }
    if (!more) {
      mState = O2PrimaryServerState::Stopped;
    } else {
      mState = O2PrimaryServerState::ReadyToServe;
    }
    return more; // will be taken down by external driver
  }

  bool HandleRequest(FairMQMessagePtr& request, int /*index*/)
  {
    LOG(DEBUG) << "GOT A REQUEST WITH SIZE " << request->GetSize();
    std::string requeststring(static_cast<char*>(request->GetData()), request->GetSize());

    if (requeststring.compare("configrequest") == 0) {
      return HandleConfigRequest(request);
    }

    else if (requeststring.compare("primrequest") != 0) {
      LOG(INFO) << "unknown request\n";
      // TODO: we need to fullfill contract and send a reply with an error code
      return true;
    }

    bool workavailable = true;
    if (mEventCounter >= mMaxEvents && mNeedNewEvent) {
      workavailable = false;
    }

    LOG(INFO) << "Received request for work " << mEventCounter << " " << mMaxEvents << " " << mNeedNewEvent << " available " << workavailable;
    if (mNeedNewEvent) {
      // we need a newly generated event now
      if (mGeneratorThread.joinable()) {
        mGeneratorThread.join();
      }
      mNeedNewEvent = false;
      mPartCounter = 0;
      mEventCounter++;
    }

    auto& prims = mStack->getPrimaries();
    auto numberofparts = (int)std::ceil(prims.size() / (1. * mChunkGranularity));
    // number of parts should be at least 1 (even if empty)
    numberofparts = std::max(1, numberofparts);

    LOG(INFO) << "Have " << prims.size() << " " << numberofparts;

    o2::data::PrimaryChunk m;
    o2::data::SubEventInfo i;
    i.eventID = workavailable ? mEventCounter : -1;
    i.maxEvents = mMaxEvents;
    i.part = mPartCounter + 1;
    i.nparts = numberofparts;
    i.seed = mEventCounter + mInitialSeed;
    i.index = m.mParticles.size();
    i.mMCEventHeader = mEventHeader;
    m.mSubEventInfo = i;

    if (workavailable) {
      int endindex = prims.size() - mPartCounter * mChunkGranularity;
      int startindex = prims.size() - (mPartCounter + 1) * mChunkGranularity;
      LOG(INFO) << "indices " << startindex << " " << endindex;

      if (startindex < 0) {
        startindex = 0;
      }
      if (endindex < 0) {
        endindex = 0;
      }

      for (int index = startindex; index < endindex; ++index) {
        m.mParticles.emplace_back(prims[index]);
      }

      LOG(INFO) << "Sending " << m.mParticles.size() << " particles";
      LOG(INFO) << "treating ev " << mEventCounter << " part " << i.part << " out of " << i.nparts;

      // feedback to driver if new event started
      if (mPipeToDriver != -1 && i.part == 1 && workavailable) {
        if (write(mPipeToDriver, &mEventCounter, sizeof(mEventCounter))) {
        }
      }

      mPartCounter++;
      if (mPartCounter == numberofparts) {
        mNeedNewEvent = true;
        // start generation of a new event
        mGeneratorThread = std::thread(&O2PrimaryServerDevice::generateEvent, this);
      }
    }

    TMessage* tmsg = new TMessage(kMESS_OBJECT);
    tmsg->WriteObjectAny((void*)&m, TClass::GetClass("o2::data::PrimaryChunk"));

    auto free_tmessage = [](void* data, void* hint) { delete static_cast<TMessage*>(hint); };

    std::unique_ptr<FairMQMessage> message(
      fTransportFactory->CreateMessage(tmsg->Buffer(), tmsg->BufferSize(), free_tmessage, tmsg));

    // send answer
    TStopwatch timer;
    timer.Start();
    auto code = Send(message, "primary-get", 0, 5000); // we introduce timeout in order not to block other requests
    timer.Stop();
    auto time = timer.CpuTime();
    if (code > 0) {
      LOG(INFO) << "Reply send in " << time << "s";
      return workavailable;
    } else {
      LOG(WARN) << "Sending process had problems. Return code : " << code << " time " << time << "s";
    }
    return false; // -> error should not get here
  }

  bool waitForControlInput()
  {
    o2::simpubsub::publishMessage(fChannels["primary-notifications"].at(0), o2::simpubsub::simStatusString("PRIMSERVER", "STATUS", "AWAITING INPUT"));

    auto factory = FairMQTransportFactory::CreateTransportFactory("zeromq");
    auto channel = FairMQChannel{"o2sim-control", "sub", factory};
    auto controlsocketname = getenv("ALICE_O2SIMCONTROL");
    channel.Connect(std::string(controlsocketname));
    channel.Validate();
    std::unique_ptr<FairMQMessage> reply(channel.NewMessage());

    LOG(INFO) << "WAITING FOR CONTROL INPUT";
    if (channel.Receive(reply) > 0) {
      auto data = reply->GetData();
      auto size = reply->GetSize();

      std::string command(reinterpret_cast<char const*>(data), size);
      LOG(INFO) << "message: " << command;

      o2::conf::SimReconfigData reconfig;
      o2::conf::parseSimReconfigFromString(command, reconfig);
      LOG(INFO) << "Processing " << reconfig.nEvents << " new events";
      return ReInit(reconfig);
    } else {
      LOG(INFO) << "NOTHING RECEIVED";
    }
    return false;
  }

 private:
  o2::conf::SimConfig mSimConfig = o2::conf::SimConfig::Instance(); // local sim config object
  o2::eventgen::PrimaryGenerator* mPrimGen = nullptr;               // the current primary generator
  o2::dataformats::MCEventHeader mEventHeader;
  o2::data::Stack* mStack = nullptr; // the stack which is filled (pointer since constructor to be called only init method)
  int mChunkGranularity = 500;       // how many primaries to send to a worker
  int mPartCounter = 0;
  bool mNeedNewEvent = true;
  int mMaxEvents = 2;
  int mInitialSeed = -1;
  int mPipeToDriver = -1; // handle for direct piper to driver (to communicate meta info)
  int mEventCounter = 0;

  std::thread mGeneratorThread; //! a thread used to concurrently init the particle generator
                                //  or to generate events

  // Keeps various generators instantiated in memory
  // useful when running simulation as a service (when generators
  // change between batches)
  // TODO: some care needs to be taken (or the user warned) that the caching is based on generator name
  //       and that parameter-based reconfiguration is not yet implemented (for which we would need to hash all
  //       configuration parameters as well)
  std::map<std::string, o2::eventgen::PrimaryGenerator*> mPrimGeneratorCache;

  O2PrimaryServerState mState = O2PrimaryServerState::Initializing;
  bool mAsService = false;
};

} // namespace devices
} // namespace o2

#endif
