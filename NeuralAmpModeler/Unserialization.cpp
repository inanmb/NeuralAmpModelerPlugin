// Unserialization
//
// This plugin is used in important places, so we need to be considerate when
// attempting to unserialize. If the project was last saved with a legacy
// version, then we need it to "update" to the current version is as
// reasonable a way as possible.
//
// In order to handle older versions, the pattern is:
// 1. Implement unserialization for every version into a version-specific
//    struct (Let's use our friend nlohmann::json. Why not?)
// 2. Implement an "update" from each struct to the next one.
// 3. Implement assigning the data contained in the current struct to the
//    current plugin configuration.
//
// This way, a constant amount of effort is required every time the
// serialization changes instead of having to implement a current
// unserialization for each past version.

// Add new unserialization versions to the top, then add logic to the class method at the bottom.

// Forward declarations for helpers defined later in this file.
static int _TryReadSlots(const iplug::IByteChunk& chunk, int pos, nlohmann::json& config);
static void _AddMissingSlotParams(nlohmann::json& config);

// Boilerplate

void NeuralAmpModeler::_UnserializeApplyConfig(nlohmann::json& config)
{
  // Suppress OnParamChange slot triggers during param restore.
  mSlotParamGuard.store(true);

  auto getParamByName = [&](std::string& name) {
    // Could use a map but eh
    for (int i = 0; i < kNumParams; i++)
    {
      iplug::IParam* param = GetParam(i);
      if (strcmp(param->GetName(), name.c_str()) == 0)
      {
        return param;
      }
    }
    // else
    return (iplug::IParam*)nullptr;
  };
  TRACE
  ENTER_PARAMS_MUTEX
  for (auto it = config.begin(); it != config.end(); ++it)
  {
    std::string name = it.key();
    iplug::IParam* pParam = getParamByName(name);
    if (pParam != nullptr)
    {
      pParam->Set(*it);
      iplug::Trace(TRACELOC, "%s %f", pParam->GetName(), pParam->Value());
    }
    else
    {
      iplug::Trace(TRACELOC, "%s NOT-FOUND", name.c_str());
    }
  }
  OnParamReset(iplug::EParamSource::kPresetRecall);
  LEAVE_PARAMS_MUTEX

  mNAMPath.Set(static_cast<std::string>(config["NAMPath"]).c_str());
  mIRPath.Set(static_cast<std::string>(config["IRPath"]).c_str());

  // Model slots
  for (int i = 0; i < kNumModelSlots; i++)
  {
    SlotState& s = mSlots[i];
    const std::string namKey = "SlotNAMPath" + std::to_string(i);
    const std::string irKey  = "SlotIRPath"  + std::to_string(i);
    const std::string jsonKey = "SlotJSON"   + std::to_string(i);
    if (config.contains(namKey))
    {
      s.namPath.Set(static_cast<std::string>(config[namKey]).c_str());
      s.irPath.Set(config.contains(irKey) ? static_cast<std::string>(config[irKey]).c_str() : "");
      s.irToggle = true;
      s.params.clear();
      s.assigned = false;
      if (config.contains(jsonKey))
      {
        try
        {
          nlohmann::json j = nlohmann::json::parse(static_cast<std::string>(config[jsonKey]));
          s.assigned = j.value("assigned", false);
          s.irToggle = j.value("irToggle", true);
          if (j.contains("params"))
            s.params = j["params"].get<std::unordered_map<std::string, double>>();
        }
        catch (...) {}
      }
    }
    else
    {
      s.namPath.Set("");
      s.irPath.Set("");
      s.irToggle = true;
      s.params.clear();
      s.assigned = false;
    }
  }
  mActiveSlot.store(0);
  mSlotLoadRequest.store(0);
  mSlotAssignRequest.store(0);
  mSlotParamGuard.store(false);

  if (mNAMPath.GetLength())
  {
    _StageModel(mNAMPath);
  }
  if (mIRPath.GetLength())
  {
    _StageIR(mIRPath);
  }
}

// Unserialize NAM Path, IR path, then named keys
int _UnserializePathsAndExpectedKeys(const iplug::IByteChunk& chunk, int startPos, nlohmann::json& config,
                                     std::vector<std::string>& paramNames)
{
  int pos = startPos;
  WDL_String path;
  pos = chunk.GetStr(path, pos);
  config["NAMPath"] = std::string(path.Get());
  pos = chunk.GetStr(path, pos);
  config["IRPath"] = std::string(path.Get());

  for (auto it = paramNames.begin(); it != paramNames.end(); ++it)
  {
    double v = 0.0;
    const int nextPos = chunk.Get(&v, pos);
    if (nextPos < 0)
      break;
    pos = nextPos;
    config[*it] = v;
  }
  return pos;
}

void _RenameKeys(nlohmann::json& j, std::unordered_map<std::string, std::string> newNames)
{
  // Assumes no aliasing!
  for (auto it = newNames.begin(); it != newNames.end(); ++it)
  {
    j[it->second] = j[it->first];
    j.erase(it->first);
  }
}

// v1.2.1

void _UpdateConfigFrom_1_2_1(nlohmann::json& config)
{
  // Current format.
  if (!config.contains("Offline Filter Phase"))
  {
    if (config.contains("Filter Phase"))
      config["Offline Filter Phase"] = config["Filter Phase"];
    else
      config["Offline Filter Phase"] = 0.0;
  }
}

int _GetConfigFrom_1_2_1(const iplug::IByteChunk& chunk, int startPos, nlohmann::json& config)
{
  std::vector<std::string> paramNames{"Input",
                                      "Threshold",
                                      "Bass",
                                      "Middle",
                                      "Treble",
                                      "Output",
                                      "NoiseGateActive",
                                      "ToneStack",
                                      "IRToggle",
                                      "CalibrateInput",
                                      "InputCalibrationLevel",
                                      "OutputMode",
                                      "Slim",
                                      "Oversampling",
                                      "Filter Phase",
                                      "Offline Oversampling",
                                      "EQ Post",
                                      "Channel Mode",
                                      "Offline Filter Phase"};

  int pos = _UnserializePathsAndExpectedKeys(chunk, startPos, config, paramNames);
  _UpdateConfigFrom_1_2_1(config);
  pos = _TryReadSlots(chunk, pos, config);
  _AddMissingSlotParams(config);
  return pos;
}

// v1.2.0

void _UpdateConfigFrom_1_2_0(nlohmann::json& config)
{
  if (!config.contains("Channel Mode"))
    config["Channel Mode"] = 0.0;
  if (config.contains("Filter Phase") && static_cast<double>(config["Filter Phase"]) >= 1.0)
    config["Filter Phase"] = 3.0;
  config["Offline Filter Phase"] = config["Filter Phase"];
  _UpdateConfigFrom_1_2_1(config);
}

int _GetConfigFrom_1_2_0(const iplug::IByteChunk& chunk, int startPos, nlohmann::json& config)
{
  std::vector<std::string> paramNames{"Input",
                                      "Threshold",
                                      "Bass",
                                      "Middle",
                                      "Treble",
                                      "Output",
                                      "NoiseGateActive",
                                      "ToneStack",
                                      "IRToggle",
                                      "CalibrateInput",
                                      "InputCalibrationLevel",
                                      "OutputMode",
                                      "Slim",
                                      "Oversampling",
                                      "Filter Phase",
                                      "Offline Oversampling",
                                      "EQ Post",
                                      "Channel Mode"};

  int pos = _UnserializePathsAndExpectedKeys(chunk, startPos, config, paramNames);
  _UpdateConfigFrom_1_2_0(config);
  pos = _TryReadSlots(chunk, pos, config);
  _AddMissingSlotParams(config);
  return pos;
}

// v1.1.1

void _UpdateConfigFrom_1_1_1(nlohmann::json& config)
{
  config["EQ Post"] = 1.0;
  _UpdateConfigFrom_1_2_0(config);
}

int _GetConfigFrom_1_1_1(const iplug::IByteChunk& chunk, int startPos, nlohmann::json& config)
{
  std::vector<std::string> paramNames{"Input",
                                      "Threshold",
                                      "Bass",
                                      "Middle",
                                      "Treble",
                                      "Output",
                                      "NoiseGateActive",
                                      "ToneStack",
                                      "IRToggle",
                                      "CalibrateInput",
                                      "InputCalibrationLevel",
                                      "OutputMode",
                                      "Slim",
                                      "Oversampling",
                                      "Filter Phase",
                                      "Offline Oversampling"};

  int pos = _UnserializePathsAndExpectedKeys(chunk, startPos, config, paramNames);
  _UpdateConfigFrom_1_1_1(config);
  pos = _TryReadSlots(chunk, pos, config);
  _AddMissingSlotParams(config);
  return pos;
}

// v1.1.0

void _UpdateConfigFrom_1_1_0(nlohmann::json& config)
{
  config["EQ Post"] = 1.0;
  _UpdateConfigFrom_1_2_0(config);
}

int _GetConfigFrom_1_1_0(const iplug::IByteChunk& chunk, int startPos, nlohmann::json& config)
{
  std::vector<std::string> paramNames{"Input",
                                      "Threshold",
                                      "Bass",
                                      "Middle",
                                      "Treble",
                                      "Output",
                                      "NoiseGateActive",
                                      "ToneStack",
                                      "IRToggle",
                                      "CalibrateInput",
                                      "InputCalibrationLevel",
                                      "OutputMode",
                                      "Slim",
                                      "Oversampling",
                                      "Filter Phase",
                                      "Offline Oversampling"};

  int pos = _UnserializePathsAndExpectedKeys(chunk, startPos, config, paramNames);
  _UpdateConfigFrom_1_1_0(config);
  pos = _TryReadSlots(chunk, pos, config);
  _AddMissingSlotParams(config);
  return pos;
}

// Reads slot data written by SerializeState if the ###Slots### tag is present.
// Returns updated pos. If the tag is absent, slots are left empty (legacy save).
static int _TryReadSlots(const iplug::IByteChunk& chunk, int pos, nlohmann::json& config)
{
  WDL_String tag;
  const int posAfterTag = chunk.GetStr(tag, pos);
  if (strcmp(tag.Get(), "###Slots###") != 0)
    return pos; // no slot data — leave pos unchanged

  pos = posAfterTag;
  for (int i = 0; i < kNumModelSlots; i++)
  {
    WDL_String namPath, irPath, jsonStr;
    pos = chunk.GetStr(namPath, pos);
    pos = chunk.GetStr(irPath, pos);
    pos = chunk.GetStr(jsonStr, pos);
    config["SlotNAMPath" + std::to_string(i)] = std::string(namPath.Get());
    config["SlotIRPath"  + std::to_string(i)] = std::string(irPath.Get());
    config["SlotJSON"    + std::to_string(i)] = std::string(jsonStr.Get());
  }
  return pos;
}

// v0.7.14

static void _AddMissingSlotParams(nlohmann::json& config)
{
  // Add oversampling/stereo defaults for states saved before these features existed.
  if (!config.contains("Oversampling"))
    config["Oversampling"] = 0.0;
  if (!config.contains("Filter Phase"))
    config["Filter Phase"] = 0.0;
  if (!config.contains("Offline Oversampling"))
    config["Offline Oversampling"] = 0.0;
  _UpdateConfigFrom_1_1_0(config);
  // Add slot params for states saved before slots existed.
  for (int i = 1; i <= kNumModelSlots; i++)
  {
    config["CallSlot"   + std::to_string(i)] = 0.0;
    config["AssignSlot" + std::to_string(i)] = 0.0;
  }
}

int _GetConfigFrom_0_7_14(const iplug::IByteChunk& chunk, int startPos, nlohmann::json& config)
{
  std::vector<std::string> paramNames{"Input",
                                      "Threshold",
                                      "Bass",
                                      "Middle",
                                      "Treble",
                                      "Output",
                                      "NoiseGateActive",
                                      "ToneStack",
                                      "IRToggle",
                                      "CalibrateInput",
                                      "InputCalibrationLevel",
                                      "OutputMode",
                                      "Slim"};

  int pos = _UnserializePathsAndExpectedKeys(chunk, startPos, config, paramNames);
  pos = _TryReadSlots(chunk, pos, config);
  _AddMissingSlotParams(config);
  return pos;
}

// v0.7.12

int _GetConfigFrom_0_7_12(const iplug::IByteChunk& chunk, int startPos, nlohmann::json& config)
{
  std::vector<std::string> paramNames{"Input",
                                      "Threshold",
                                      "Bass",
                                      "Middle",
                                      "Treble",
                                      "Output",
                                      "NoiseGateActive",
                                      "ToneStack",
                                      "IRToggle",
                                      "CalibrateInput",
                                      "InputCalibrationLevel",
                                      "OutputMode"};

  int pos = _UnserializePathsAndExpectedKeys(chunk, startPos, config, paramNames);
  config["Slim"] = 1.0;
  pos = _TryReadSlots(chunk, pos, config);
  _AddMissingSlotParams(config);
  return pos;
}

// 0.7.10

int _GetConfigFrom_0_7_10(const iplug::IByteChunk& chunk, int startPos, nlohmann::json& config)
{
  std::vector<std::string> paramNames{
    "Input", "Threshold", "Bass", "Middle", "Treble", "Output", "NoiseGateActive", "ToneStack", "OutNorm", "IRToggle"};
  int pos = _UnserializePathsAndExpectedKeys(chunk, startPos, config, paramNames);
  std::unordered_map<std::string, std::string> newNames{{"OutNorm", "OutputMode"}};
  _RenameKeys(config, newNames);
  config[kCalibrateInputParamName] = (double)kDefaultCalibrateInput;
  config[kInputCalibrationLevelParamName] = kDefaultInputCalibrationLevel;
  config["Slim"] = 1.0;
  pos = _TryReadSlots(chunk, pos, config);
  _AddMissingSlotParams(config);
  return pos;
}

// Earlier than 0.7.10 (Assumed to be 0.7.3-0.7.9)

int _GetConfigFrom_Earlier(const iplug::IByteChunk& chunk, int startPos, nlohmann::json& config)
{
  std::vector<std::string> paramNames{
    "Input", "Gate", "Bass", "Middle", "Treble", "Output", "NoiseGateActive", "ToneStack", "OutNorm", "IRToggle"};

  int pos = _UnserializePathsAndExpectedKeys(chunk, startPos, config, paramNames);
  std::unordered_map<std::string, std::string> newNames{{"Gate", "Threshold"}, {"OutNorm", "OutputMode"}};
  _RenameKeys(config, newNames);
  config[kCalibrateInputParamName] = (double)kDefaultCalibrateInput;
  config[kInputCalibrationLevelParamName] = kDefaultInputCalibrationLevel;
  config["Slim"] = 1.0;
  pos = _TryReadSlots(chunk, pos, config);
  _AddMissingSlotParams(config);
  return pos;
}

//==============================================================================

class _Version
{
public:
  _Version(const int major, const int minor, const int patch)
  : mMajor(major)
  , mMinor(minor)
  , mPatch(patch) {};
  _Version(const std::string& versionStr)
  {
    std::istringstream stream(versionStr);
    std::string token;
    std::vector<int> parts;

    // Split the string by "."
    while (std::getline(stream, token, '.'))
    {
      parts.push_back(std::stoi(token)); // Convert to int and store
    }

    // Check if we have exactly 3 parts
    if (parts.size() != 3)
    {
      throw std::invalid_argument("Input string does not contain exactly 3 segments separated by '.'");
    }

    // Assign the parts to the provided int variables
    mMajor = parts[0];
    mMinor = parts[1];
    mPatch = parts[2];
  };

  bool operator>=(const _Version& other) const
  {
    // Compare on major version:
    if (GetMajor() > other.GetMajor())
    {
      return true;
    }
    if (GetMajor() < other.GetMajor())
    {
      return false;
    }
    // Compare on minor
    if (GetMinor() > other.GetMinor())
    {
      return true;
    }
    if (GetMinor() < other.GetMinor())
    {
      return false;
    }
    // Compare on patch
    return GetPatch() >= other.GetPatch();
  };

  int GetMajor() const { return mMajor; };
  int GetMinor() const { return mMinor; };
  int GetPatch() const { return mPatch; };

private:
  int mMajor;
  int mMinor;
  int mPatch;
};

int NeuralAmpModeler::_UnserializeStateWithKnownVersion(const iplug::IByteChunk& chunk, int startPos)
{
  // We already got through the header before calling this.
  int pos = startPos;

  // Get the version
  WDL_String wVersion;
  pos = chunk.GetStr(wVersion, pos);
  std::string versionStr(wVersion.Get());
  _Version version(versionStr);
  // Act accordingly
  nlohmann::json config;
  if (version >= _Version(1, 2, 1))
  {
    pos = _GetConfigFrom_1_2_1(chunk, pos, config);
  }
  else if (version >= _Version(1, 2, 0))
  {
    pos = _GetConfigFrom_1_2_0(chunk, pos, config);
  }
  else if (version >= _Version(1, 1, 1))
  {
    pos = _GetConfigFrom_1_1_1(chunk, pos, config);
  }
  else if (version >= _Version(1, 1, 0))
  {
    pos = _GetConfigFrom_1_1_0(chunk, pos, config);
  }
  else if (version >= _Version(0, 7, 14))
  {
    pos = _GetConfigFrom_0_7_14(chunk, pos, config);
  }
  else if (version >= _Version(0, 7, 12))
  {
    pos = _GetConfigFrom_0_7_12(chunk, pos, config);
  }
  else if (version >= _Version(0, 7, 10))
  {
    pos = _GetConfigFrom_0_7_10(chunk, pos, config);
  }
  else if (version >= _Version(0, 7, 9))
  {
    pos = _GetConfigFrom_Earlier(chunk, pos, config);
  }
  else
  {
    // You shouldn't be here...
    assert(false);
  }
  _UnserializeApplyConfig(config);
  return pos;
}

int NeuralAmpModeler::_UnserializeStateWithUnknownVersion(const iplug::IByteChunk& chunk, int startPos)
{
  nlohmann::json config;
  int pos = _GetConfigFrom_Earlier(chunk, startPos, config);
  _UnserializeApplyConfig(config);
  return pos;
}
