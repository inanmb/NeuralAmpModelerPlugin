#pragma once

#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <avrt.h>
  #pragma comment(lib, "Avrt.lib")
#endif

#include <atomic>
#include <condition_variable>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "../AudioDSPTools/dsp/ImpulseResponse.h"
#include "../AudioDSPTools/dsp/NoiseGate.h"
#include "../AudioDSPTools/dsp/dsp.h"
#include "../AudioDSPTools/dsp/wav.h"
#include "../AudioDSPTools/dsp/ResamplingContainer/ResamplingContainer.h"
#include "../NeuralAmpModelerCore/NAM/dsp.h"
#include "../NeuralAmpModelerCore/NAM/get_dsp.h"
#include "../NeuralAmpModelerCore/NAM/slimmable.h"

#include "Colors.h"
#include "ToneStack.h"

#include "IPlug_include_in_plug_hdr.h"
#include "ISender.h"

#if defined(__APPLE__)
#include <pthread.h>
#if __has_include(<pthread/qos.h>)
#include <pthread/qos.h>
#define NAM_HAS_PTHREAD_QOS 1
#else
#define NAM_HAS_PTHREAD_QOS 0
#endif
#endif


const int kNumPresets = 1;
// The plugin is mono inside
constexpr size_t kNumChannelsInternal = 1;

class NAMSender : public iplug::IPeakAvgSender<>
{
public:
  NAMSender()
  : iplug::IPeakAvgSender<>(-90.0, true, 5.0f, 1.0f, 300.0f, 500.0f)
  {
  }
};

enum EParams
{
  // These need to be the first ones because I use their indices to place
  // their rects in the GUI.
  kInputLevel = 0,
  kNoiseGateThreshold,
  kToneBass,
  kToneMid,
  kToneTreble,
  kOutputLevel,
  // The rest is fine though.
  kNoiseGateActive,
  kEQActive,
  kIRToggle,
  // Input calibration
  kCalibrateInput,
  kInputCalibrationLevel,
  kOutputMode,
  kSlim,
  // Model slots
  // Call: press button N to load slot N (radio behaviour)
  kCallSlot1,
  kCallSlot2,
  kCallSlot3,
  kCallSlot4,
  kCallSlot5,
  kCallSlot6,
  kCallSlot7,
  kCallSlot8,
  kCallSlot9,
  kCallSlot10,
  // Assign: press button N to save current full state into slot N (auto-resets to 0)
  kAssignSlot1,
  kAssignSlot2,
  kAssignSlot3,
  kAssignSlot4,
  kAssignSlot5,
  kAssignSlot6,
  kAssignSlot7,
  kAssignSlot8,
  kAssignSlot9,
  kAssignSlot10,
  // Oversampling / multicore
  kOversamplingFactor, // 0=Off(1x), 1=2x, 2=3x, 3=4x, 4=8x, 5=16x, 6=32x
  kMulticoreEnabled,   // bool
  kNumParams
};

const int kNumModelSlots = 10;

static const int kOversamplingFactorValues[] = {1, 2, 3, 4, 8, 16, 32};
static const int kNumOversamplingFactors = 7;

const int numKnobs = 6;

enum ECtrlTags
{
  kCtrlTagModelFileBrowser = 0,
  kCtrlTagIRFileBrowser,
  kCtrlTagInputMeter,
  kCtrlTagOutputMeter,
  kCtrlTagSettingsBox,
  kCtrlTagOutputMode,
  kCtrlTagCalibrateInput,
  kCtrlTagInputCalibrationLevel,
  kCtrlTagSlimmableIcon,
  kCtrlTagSlimOverlayBackdrop,
  kCtrlTagSlimKnob,
  kCtrlTagOversamplingControl,
  kCtrlTagMulticoreControl,
  kNumCtrlTags
};

enum EMsgTags
{
  // These tags are used from UI -> DSP
  kMsgTagClearModel = 0,
  kMsgTagClearIR,
  kMsgTagHighlightColor,
  // The following tags are from DSP -> UI
  kMsgTagLoadFailed,
  kMsgTagLoadedModel,
  kMsgTagLoadedIR,
  kNumMsgTags
};

// Get the sample rate of a NAM model.
// Sometimes, the model doesn't know its own sample rate; this wrapper guesses 48k based on the way that most
// people have used NAM in the past.
double GetNAMSampleRate(const std::unique_ptr<nam::DSP>& model)
{
  const double assumedSampleRate = 48000.0;
  const double reported = model->GetExpectedSampleRate();
  return reported <= 0.0 ? assumedSampleRate : reported;
};

static inline void NAMConfigurePhaseWorkerThread(int /*workerJobIndex*/)
{
#if defined(_WIN32)
  DWORD taskIndex = 0;
  HANDLE mmcss = AvSetMmThreadCharacteristicsA("Pro Audio", &taskIndex);
  if (mmcss == nullptr)
    mmcss = AvSetMmThreadCharacteristicsA("Audio", &taskIndex);
  if (mmcss != nullptr)
    AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#elif defined(__APPLE__)
#if NAM_HAS_PTHREAD_QOS
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
#endif
}

class NAMPhaseMulticorePool
{
public:
  explicit NAMPhaseMulticorePool(int totalThreads)
  {
    const int workerCount = std::max(0, totalThreads - 1);
    mWorkers.reserve(static_cast<size_t>(workerCount));
    for (int i = 0; i < workerCount; i++)
    {
      const int workerJobIndex = i + 1;
      mWorkers.emplace_back([this, workerJobIndex] { WorkerLoop(workerJobIndex); });
    }
  }

  ~NAMPhaseMulticorePool()
  {
    {
      std::lock_guard<std::mutex> lock(mMutex);
      mStop = true;
      ++mGeneration;
    }
    mCV.notify_all();
    for (auto& t : mWorkers)
      if (t.joinable()) t.join();
  }

  int ThreadCount() const { return static_cast<int>(mWorkers.size()) + 1; }

  template <typename Fn>
  void ParallelFor(int jobCount, Fn&& fn)
  {
    if (jobCount <= 1 || mWorkers.empty())
    {
      for (int j = 0; j < jobCount; j++) fn(j);
      return;
    }
    const int clamped = std::max(1, std::min(jobCount, ThreadCount()));
    const int workerJobs = std::max(0, clamped - 1);
    {
      std::lock_guard<std::mutex> lock(mMutex);
      mJob = std::forward<Fn>(fn);
      mJobCount = clamped;
      mRemainingWorkers = workerJobs;
      mDone = (workerJobs == 0);
      ++mGeneration;
    }
    mCV.notify_all();
    mJob(0);
    if (workerJobs > 0)
    {
      std::unique_lock<std::mutex> lock(mMutex);
      mDoneCV.wait(lock, [this] { return mDone; });
    }
    { std::lock_guard<std::mutex> lock(mMutex); mJob = nullptr; }
  }

private:
  void WorkerLoop(int workerJobIndex)
  {
    NAMConfigurePhaseWorkerThread(workerJobIndex);
    int seenGeneration = 0;
    for (;;)
    {
      std::function<void(int)> job;
      bool shouldRun = false;
      {
        std::unique_lock<std::mutex> lock(mMutex);
        mCV.wait(lock, [this, &seenGeneration] { return mStop || mGeneration != seenGeneration; });
        if (mStop) return;
        seenGeneration = mGeneration;
        shouldRun = workerJobIndex < mJobCount && static_cast<bool>(mJob);
        if (shouldRun) job = mJob;
      }
      if (!shouldRun) continue;
      job(workerJobIndex);
      {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mRemainingWorkers > 0) --mRemainingWorkers;
        if (mRemainingWorkers == 0 && !mDone) { mDone = true; mDoneCV.notify_one(); }
      }
    }
  }

  std::vector<std::thread> mWorkers;
  std::mutex mMutex;
  std::condition_variable mCV;
  std::condition_variable mDoneCV;
  std::function<void(int)> mJob;
  int mJobCount = 0;
  int mRemainingWorkers = 0;
  int mGeneration = 0;
  bool mDone = true;
  bool mStop = false;
};

static inline int NAMPhaseMulticoreHardwareThreads()
{
  const unsigned hw = std::thread::hardware_concurrency();
  return hw > 0 ? static_cast<int>(hw) : 8;
}

static inline std::shared_ptr<NAMPhaseMulticorePool> NAMGetPhasePool(int totalThreads)
{
  const int maxT = std::max(1, NAMPhaseMulticoreHardwareThreads());
  const int n = std::max(1, std::min(totalThreads, maxT));
  static std::mutex poolsMutex;
  static std::vector<std::shared_ptr<NAMPhaseMulticorePool>> pools;
  std::lock_guard<std::mutex> lock(poolsMutex);
  if (static_cast<int>(pools.size()) <= n) pools.resize(static_cast<size_t>(n + 1));
  if (!pools[static_cast<size_t>(n)]) pools[static_cast<size_t>(n)] = std::make_shared<NAMPhaseMulticorePool>(n);
  return pools[static_cast<size_t>(n)];
}

class ResamplingNAM : public nam::DSP
{
public:
  ResamplingNAM(std::unique_ptr<nam::DSP> encapsulated, const double expected_sample_rate,
                const std::filesystem::path& modelPath = std::filesystem::path())
  : nam::DSP(encapsulated->NumInputChannels(), encapsulated->NumOutputChannels(), expected_sample_rate)
  , mEncapsulated(std::move(encapsulated))
  , mModelPath(modelPath)
  {
    if (mEncapsulated->HasLoudness()) SetLoudness(mEncapsulated->GetLoudness());
    if (mEncapsulated->HasInputLevel()) SetInputLevel(mEncapsulated->GetInputLevel());
    if (mEncapsulated->HasOutputLevel()) SetOutputLevel(mEncapsulated->GetOutputLevel());
    Reset(expected_sample_rate, 2048);
  };

  ~ResamplingNAM() = default;

  void prewarm() override
  {
    std::lock_guard<std::mutex> lock(mStateMutex);
    mEncapsulated->prewarm();
  };

  void process(NAM_SAMPLE** input, NAM_SAMPLE** output, const int num_frames) override
  {
    std::lock_guard<std::mutex> lock(mStateMutex);
    if (num_frames > mMaxExternalBlockSize)
      ResetUnlocked(mExternalSampleRate, num_frames);
    if (!IsResamplingActive())
    {
      mEncapsulated->process(input, output, num_frames);
      return;
    }
    mResamplingContainer->ProcessBlock(
      input, output, num_frames,
      [this](NAM_SAMPLE** resampledIn, NAM_SAMPLE** resampledOut, int resampledFrames) {
        if (mPhaseMulticoreActive)
          ProcessPhaseMulticoreUnlocked(resampledIn, resampledOut, resampledFrames);
        else
          mEncapsulated->process(resampledIn, resampledOut, resampledFrames);
      });
  };

  int GetLatency() const
  {
    std::lock_guard<std::mutex> lock(mStateMutex);
    return IsResamplingActive() ? mResamplingContainer->GetLatency() : 0;
  };

  void SetOversamplingFactor(int factor)
  {
    std::lock_guard<std::mutex> lock(mStateMutex);
    mRequestedOversamplingFactor = factor < 1 ? 1 : factor;
    if (mEncapsulated) ResetUnlocked(mExternalSampleRate, mMaxExternalBlockSize);
  };

  void SetAntiAliasFilterPhase(dsp::EAntiAliasFilterPhase filterPhase)
  {
    std::lock_guard<std::mutex> lock(mStateMutex);
    mAntiAliasFilterPhase = filterPhase;
    if (mEncapsulated) ResetUnlocked(mExternalSampleRate, mMaxExternalBlockSize);
  };

  void SetPhaseMulticoreThreadCount(int totalThreads)
  {
    std::lock_guard<std::mutex> lock(mStateMutex);
    mPhaseMulticoreThreadCount = std::max(1, totalThreads);
    if (mEncapsulated) ResetUnlocked(mExternalSampleRate, mMaxExternalBlockSize);
  }

  void Reset(const double sampleRate, const int maxBlockSize) override
  {
    std::lock_guard<std::mutex> lock(mStateMutex);
    ResetUnlocked(sampleRate, maxBlockSize);
  };

  double GetEncapsulatedSampleRate() const { return GetNAMSampleRate(mEncapsulated); };

  nam::SlimmableModel* GetSlimmableModel() { return dynamic_cast<nam::SlimmableModel*>(mEncapsulated.get()); }
  const nam::SlimmableModel* GetSlimmableModel() const
  {
    return dynamic_cast<const nam::SlimmableModel*>(mEncapsulated.get());
  }

  void SetSlimmableSize(double value)
  {
    std::lock_guard<std::mutex> lock(mStateMutex);
    mSlimmableSize = value;
    ApplySlimmableSizeUnlocked();
  }

private:
  void ResetUnlocked(const double sampleRate, const int maxBlockSize)
  {
    mExpectedSampleRate = sampleRate;
    mExternalSampleRate = sampleRate;
    mMaxExternalBlockSize = maxBlockSize;

    const double encapsulatedSampleRate = GetEncapsulatedSampleRate();
    const double renderingSampleRate = GetRenderingSampleRate(sampleRate);
    const bool resamplingActive = std::abs(renderingSampleRate - sampleRate) > 1.0e-6;
    const auto maxEncapsulatedBlockSize =
      static_cast<int>(std::ceil(maxBlockSize * renderingSampleRate / sampleRate)) + 1;
    const int timeScale =
      static_cast<int>(std::max(1.0, std::round(renderingSampleRate / encapsulatedSampleRate)));

    mPhaseMulticoreActive = resamplingActive && mRequestedOversamplingFactor > 1
                            && timeScale > 1 && !mModelPath.empty()
                            && mPhaseMulticoreThreadCount > 1;
    mPhaseCount = mPhaseMulticoreActive ? timeScale : 1;

    if (resamplingActive)
    {
      if (mResamplingContainer == nullptr || std::abs(mRenderingSampleRate - renderingSampleRate) > 1.0e-6
          || std::abs(mResamplingBandwidthSampleRate - encapsulatedSampleRate) > 1.0e-6)
      {
        mResamplingContainer = std::make_unique<dsp::ResamplingContainer<NAM_SAMPLE, 1, 32>>(
          renderingSampleRate, mAntiAliasFilterPhase, encapsulatedSampleRate);
        mRenderingSampleRate = renderingSampleRate;
        mResamplingBandwidthSampleRate = encapsulatedSampleRate;
      }
      mResamplingContainer->SetAntiAliasFilterPhase(mAntiAliasFilterPhase);
      mResamplingContainer->Reset(sampleRate, maxBlockSize);

      if (mPhaseMulticoreActive)
      {
        const int maxPhaseBlockSize = (maxEncapsulatedBlockSize + mPhaseCount - 1) / mPhaseCount + 1;
        mEncapsulated->SetTimeScale(1);
        ApplySlimmableSizeUnlocked();
        mEncapsulated->ResetAndPrewarm(encapsulatedSampleRate, maxPhaseBlockSize);
        RebuildPhaseModelsUnlocked(encapsulatedSampleRate, maxPhaseBlockSize);
        ResizePhaseBuffersUnlocked(maxPhaseBlockSize);
      }
      else
      {
        ClearPhaseModelsUnlocked();
        mEncapsulated->SetTimeScale(timeScale);
        ApplySlimmableSizeUnlocked();
        mEncapsulated->ResetAndPrewarm(renderingSampleRate, maxEncapsulatedBlockSize);
      }
    }
    else
    {
      ClearPhaseModelsUnlocked();
      mResamplingContainer = nullptr;
      mRenderingSampleRate = sampleRate;
      mEncapsulated->SetTimeScale(1);
      ApplySlimmableSizeUnlocked();
      mEncapsulated->ResetAndPrewarm(sampleRate, maxBlockSize);
    }
  }

  double GetRenderingSampleRate(double externalSampleRate) const
  {
    const double encapsulatedSampleRate = GetEncapsulatedSampleRate();
    if (mRequestedOversamplingFactor <= 1) return encapsulatedSampleRate;
    const double requestedRendering = externalSampleRate * static_cast<double>(mRequestedOversamplingFactor);
    const double scale = std::max(1.0, std::round(requestedRendering / encapsulatedSampleRate));
    return encapsulatedSampleRate * scale;
  }

  bool IsResamplingActive() const { return mResamplingContainer != nullptr; }

  void RebuildPhaseModelsUnlocked(double encapsulatedSampleRate, int maxPhaseBlockSize)
  {
    const int needed = std::max(0, mPhaseCount - 1);
    while (static_cast<int>(mPhaseModels.size()) < needed)
    {
      auto clone = mEncapsulated->CloneForPhase();
      if (!clone) clone = nam::get_dsp(mModelPath);
      clone->SetTimeScale(1);
      if (auto* s = dynamic_cast<nam::SlimmableModel*>(clone.get())) s->SetSlimmableSize(mSlimmableSize);
      clone->ResetAndPrewarm(encapsulatedSampleRate, maxPhaseBlockSize);
      mPhaseModels.push_back(std::move(clone));
    }
    while (static_cast<int>(mPhaseModels.size()) > needed)
      mPhaseModels.pop_back();

    for (auto& m : mPhaseModels)
    {
      m->SetTimeScale(1);
      m->ResetAndPrewarm(encapsulatedSampleRate, maxPhaseBlockSize);
    }
  }

  void ResizePhaseBuffersUnlocked(int maxPhaseBlockSize)
  {
    mPhaseInputBuffers.resize(static_cast<size_t>(mPhaseCount));
    mPhaseOutputBuffers.resize(static_cast<size_t>(mPhaseCount));
    for (int p = 0; p < mPhaseCount; p++)
    {
      mPhaseInputBuffers[static_cast<size_t>(p)].assign(static_cast<size_t>(maxPhaseBlockSize), NAM_SAMPLE(0));
      mPhaseOutputBuffers[static_cast<size_t>(p)].assign(static_cast<size_t>(maxPhaseBlockSize), NAM_SAMPLE(0));
    }
  }

  void ClearPhaseModelsUnlocked()
  {
    mPhaseMulticoreActive = false;
    mPhaseCount = 1;
    mPhaseModels.clear();
    mPhaseInputBuffers.clear();
    mPhaseOutputBuffers.clear();
  }

  void ApplySlimmableSizeUnlocked()
  {
    if (auto* s = dynamic_cast<nam::SlimmableModel*>(mEncapsulated.get())) s->SetSlimmableSize(mSlimmableSize);
    for (auto& m : mPhaseModels)
      if (m)
        if (auto* s = dynamic_cast<nam::SlimmableModel*>(m.get())) s->SetSlimmableSize(mSlimmableSize);
  }

  nam::DSP* GetPhaseModelUnlocked(int phase)
  {
    return phase == 0 ? mEncapsulated.get() : mPhaseModels[static_cast<size_t>(phase - 1)].get();
  }

  void ProcessPhaseMulticoreUnlocked(NAM_SAMPLE** resampledInput, NAM_SAMPLE** resampledOutput, int resampledFrames)
  {
    if (!mPhaseMulticoreActive || mPhaseCount <= 1)
    {
      mEncapsulated->process(resampledInput, resampledOutput, resampledFrames);
      return;
    }
    const int phaseCount = mPhaseCount;
    const int jobCount = std::max(1, std::min(mPhaseMulticoreThreadCount, phaseCount));
    const int phasesPerJob = (phaseCount + jobCount - 1) / jobCount;
    auto pool = NAMGetPhasePool(jobCount);
    pool->ParallelFor(jobCount, [this, resampledInput, resampledOutput, resampledFrames,
                                  phaseCount, phasesPerJob](int jobIndex) {
      const int phaseBegin = jobIndex * phasesPerJob;
      const int phaseEnd = std::min(phaseCount, phaseBegin + phasesPerJob);
      for (int phase = phaseBegin; phase < phaseEnd; phase++)
      {
        const int phaseFrames =
          phase < resampledFrames ? ((resampledFrames - phase + phaseCount - 1) / phaseCount) : 0;
        if (phaseFrames <= 0) continue;
        nam::DSP* phaseModel = GetPhaseModelUnlocked(phase);
        if (phaseModel && phaseModel->SupportsStridedProcess())
        {
          phaseModel->process_strided(
            resampledInput[0] + phase, phaseCount, resampledOutput[0] + phase, phaseCount, phaseFrames);
          continue;
        }
        auto& phaseIn  = mPhaseInputBuffers[static_cast<size_t>(phase)];
        auto& phaseOut = mPhaseOutputBuffers[static_cast<size_t>(phase)];
        for (int i = 0; i < phaseFrames; i++)
          phaseIn[static_cast<size_t>(i)] = resampledInput[0][phase + i * phaseCount];
        NAM_SAMPLE* inPtrs[1]  = {phaseIn.data()};
        NAM_SAMPLE* outPtrs[1] = {phaseOut.data()};
        phaseModel->process(inPtrs, outPtrs, phaseFrames);
        for (int i = 0; i < phaseFrames; i++)
          resampledOutput[0][phase + i * phaseCount] = phaseOut[static_cast<size_t>(i)];
      }
    });
  }

  std::unique_ptr<nam::DSP> mEncapsulated;
  std::filesystem::path mModelPath;
  bool mPhaseMulticoreActive = false;
  int mPhaseCount = 1;
  int mPhaseMulticoreThreadCount = 1;
  double mSlimmableSize = 1.0;
  std::vector<std::unique_ptr<nam::DSP>> mPhaseModels;
  std::vector<std::vector<NAM_SAMPLE>> mPhaseInputBuffers;
  std::vector<std::vector<NAM_SAMPLE>> mPhaseOutputBuffers;
  mutable std::mutex mStateMutex;

  std::unique_ptr<dsp::ResamplingContainer<NAM_SAMPLE, 1, 32>> mResamplingContainer;
  double mRenderingSampleRate = 0.0;
  double mResamplingBandwidthSampleRate = 0.0;

  int mMaxExternalBlockSize = 0;
  int mRequestedOversamplingFactor = 1;
  dsp::EAntiAliasFilterPhase mAntiAliasFilterPhase = dsp::EAntiAliasFilterPhase::MinimumPhaseCascadedFIR;
  double mExternalSampleRate = 48000.0;
};

class NeuralAmpModeler final : public iplug::Plugin
{
public:
  NeuralAmpModeler(const iplug::InstanceInfo& info);
  ~NeuralAmpModeler();

  void ProcessBlock(iplug::sample** inputs, iplug::sample** outputs, int nFrames) override;
  void OnReset() override;
  void OnIdle() override;

  bool SerializeState(iplug::IByteChunk& chunk) const override;
  int UnserializeState(const iplug::IByteChunk& chunk, int startPos) override;
  void OnUIOpen() override;
  bool OnHostRequestingSupportedViewConfiguration(int width, int height) override { return true; }

  void OnParamChange(int paramIdx) override;
  void OnParamChangeUI(int paramIdx, iplug::EParamSource source) override;
  bool OnMessage(int msgTag, int ctrlTag, int dataSize, const void* pData) override;

private:
  // Allocates mInputPointers and mOutputPointers
  void _AllocateIOPointers(const size_t nChans);
  // Moves DSP modules from staging area to the main area.
  // Also deletes DSP modules that are flagged for removal.
  // Exists so that we don't try to use a DSP module that's only
  // partially-instantiated.
  void _ApplyDSPStaging();
  // Deallocates mInputPointers and mOutputPointers
  void _DeallocateIOPointers();
  // Fallback that just copies inputs to outputs if mDSP doesn't hold a model.
  void _FallbackDSP(iplug::sample** inputs, iplug::sample** outputs, const size_t numChannels, const size_t numFrames);
  // Sizes based on mInputArray
  size_t _GetBufferNumChannels() const;
  size_t _GetBufferNumFrames() const;
  void _InitToneStack();
  // Loads a NAM model and stores it to mStagedNAM
  // Returns an empty string on success, or an error message on failure.
  std::string _StageModel(const WDL_String& dspFile);
  // Loads an IR and stores it to mStagedIR.
  // Return status code so that error messages can be relayed if
  // it wasn't successful.
  dsp::wav::LoadReturnCode _StageIR(const WDL_String& irPath);

  // === Model slots ===
  // Background worker: processes pending slot load/assign requests so that
  // model switching works even when the plugin UI is closed.
  void _SlotWorkerFunc();
  // Process one round of pending slot requests (called from _SlotWorkerFunc).
  void _ProcessSlotRequests();
  // Sync kCallSlot1-10 to reflect activeSlot, and inform the host.
  void _SyncCallSlotBooleans(int activeSlot);
  // Set a slot-related parameter value and inform the host, without
  // re-triggering a slot request (guarded by mSlotParamGuard).
  void _SetSlotParamValue(int paramIdx, int value);

  bool _HaveModel() const { return this->mModel != nullptr; };
  // Prepare the input & output buffers
  void _PrepareBuffers(const size_t numChannels, const size_t numFrames);
  // Manage pointers
  void _PrepareIOPointers(const size_t nChans);
  // Copy the input buffer to the object, applying input level.
  // :param nChansIn: In from external
  // :param nChansOut: Out to the internal of the DSP routine
  void _ProcessInput(iplug::sample** inputs, const size_t nFrames, const size_t nChansIn, const size_t nChansOut);
  // Copy the output to the output buffer, applying output level.
  // :param nChansIn: In from internal
  // :param nChansOut: Out to external
  void _ProcessOutput(iplug::sample** inputs, iplug::sample** outputs, const size_t nFrames, const size_t nChansIn,
                      const size_t nChansOut);
  // Resetting for models and IRs, called by OnReset
  void _ResetModelAndIR(const double sampleRate, const int maxBlockSize);

  void _SetInputGain();
  void _SetOutputGain();
  void _ApplySlimParamToLoadedNAMs();
  void _ApplyTransitionGain(iplug::sample** outputs, size_t nFrames, size_t nChans);

  // See: Unserialization.cpp
  void _UnserializeApplyConfig(nlohmann::json& config);
  // 0.7.9 and later
  int _UnserializeStateWithKnownVersion(const iplug::IByteChunk& chunk, int startPos);
  // Hopefully 0.7.3-0.7.8, but no gurantees
  int _UnserializeStateWithUnknownVersion(const iplug::IByteChunk& chunk, int startPos);

  // Update all controls that depend on a model
  void _UpdateControlsFromModel();

  // Make sure that the latency is reported correctly.
  void _UpdateLatency();

  // Update level meters
  // Called within ProcessBlock().
  // Assume _ProcessInput() and _ProcessOutput() were run immediately before.
  void _UpdateMeters(iplug::sample** inputPointer, iplug::sample** outputPointer, const size_t nFrames,
                     const size_t nChansIn, const size_t nChansOut);

  // Member data

  // Input arrays to NAM
  std::vector<std::vector<iplug::sample>> mInputArray;
  // Output from NAM
  std::vector<std::vector<iplug::sample>> mOutputArray;
  // Pointer versions
  iplug::sample** mInputPointers = nullptr;
  iplug::sample** mOutputPointers = nullptr;

  // Input and output gain
  double mInputGain = 1.0;
  double mOutputGain = 1.0;

  // Noise gates
  dsp::noise_gate::Trigger mNoiseGateTrigger;
  dsp::noise_gate::Gain mNoiseGateGain;
  // The model actually being used:
  std::unique_ptr<ResamplingNAM> mModel;
  // And the IR
  std::unique_ptr<dsp::ImpulseResponse> mIR;
  // Manages switching what DSP is being used.
  std::unique_ptr<ResamplingNAM> mStagedModel;
  std::unique_ptr<dsp::ImpulseResponse> mStagedIR;
  // Flags to take away the modules at a safe time.
  std::atomic<bool> mShouldRemoveModel = false;
  std::atomic<bool> mShouldRemoveIR = false;

  std::atomic<bool> mNewModelLoadedInDSP = false;
  std::atomic<bool> mModelCleared = false;

  // Fade-in/fade-out when switching models (audio thread only)
  bool mTransitionFadingOut = false;
  bool mTransitionFadingIn  = false;
  int  mTransitionSamplesRemaining = 0;
  int  mTransitionLength = 0;
  // Pending latency value to apply on the UI thread (VST3 requires SetLatency from UI thread).
  // -1 means no pending update.
  std::atomic<int> mPendingLatency{-1};

  // Tone stack modules
  std::unique_ptr<dsp::tone_stack::AbstractToneStack> mToneStack;

  // Post-IR filters
  recursive_linear_filter::HighPass mHighPass;
  //  recursive_linear_filter::LowPass mLowPass;

  // Path to model's config.json or model.nam
  WDL_String mNAMPath;

  // === Model slots ===
  struct SlotState
  {
    WDL_String namPath;
    WDL_String irPath;
    bool irToggle = true;
    std::unordered_map<std::string, double> params; // param name → value
    bool assigned = false;
  };
  SlotState mSlots[kNumModelSlots];
  // Pending requests written from OnParamChange (any thread).
  // 0 = no request. Consumed by the worker thread.
  std::atomic<int> mSlotLoadRequest{0};
  std::atomic<int> mSlotAssignRequest{0};
  // Slot currently providing the loaded model. 0 = manual (file browser).
  std::atomic<int> mActiveSlot{0};
  // True while the plugin is writing slot parameters internally, to suppress
  // re-entrant requests from OnParamChange.
  std::atomic<bool> mSlotParamGuard{false};
  // Mutex that serialises concurrent calls to _StageModel (file browser
  // lambda vs worker thread).
  std::mutex mStageMutex;
  // Worker thread for slot loading.
  std::thread mSlotWorkerThread;
  std::mutex mSlotWorkerMutex;
  std::condition_variable mSlotWorkerCV;
  bool mSlotWorkerStop = false;
  // Path to IR (.wav file)
  WDL_String mIRPath;

  WDL_String mHighLightColor{PluginColors::NAM_THEMECOLOR.ToColorCode()};

  std::unordered_map<std::string, double> mNAMParams = {{"Input", 0.0}, {"Output", 0.0}};

  NAMSender mInputSender, mOutputSender;

  std::vector<NAM_SAMPLE> mModelInF, mModelOutF;
};
