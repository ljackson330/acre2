#pragma once

#include "Macros.h"
#include "Singleton.h"
#include "Lockable.h"

#include "SoundEngine.h"
#include "ini_reader.hpp"

class CAcreSettings :
    public TSingleton<CAcreSettings>, public CLockable {
public:
    CAcreSettings();
    ~CAcreSettings();

    acre::Result save();
    acre::Result load();
    acre::Result save(std::string filename);
    acre::Result load(std::string filename);

    DECLARE_MEMBER(std::string, LastVersion);

    DECLARE_MEMBER(float, GlobalVolume);
    DECLARE_MEMBER(float, PremixGlobalVolume);

    DECLARE_MEMBER(bool, DisablePosition);
    DECLARE_MEMBER(bool, DisableMuting);
    DECLARE_MEMBER(bool, DisableRadioNoise);
    DECLARE_MEMBER(bool, DisableUnmuteClients);
    DECLARE_MEMBER(bool, DisableTS3ChannelSwitch);

    DECLARE_MEMBER(bool, EnableAudioTest);

    // Ambient battle sound: mixes the local game's audio into outgoing radio
    // transmissions. AmbientVolume scales it relative to the player's voice;
    // AmbientGateThreshold is in dBFS, below which quiet ambience is
    // suppressed rather than transmitted.
    DECLARE_MEMBER(bool, AmbientEnabled);
    DECLARE_MEMBER(float, AmbientVolume);
    DECLARE_MEMBER(float, AmbientGateThreshold);
    // Absolute path; empty disables. Writes the outgoing voice stream after
    // mixing -- what TeamSpeak actually encodes and sends.
    DECLARE_MEMBER(std::string, AmbientDumpFile);
    // Diagnostic: run a short capture at plugin start and report the result to
    // the log, so a tester can confirm the capture backend works without
    // getting in-game and keying a radio.
    DECLARE_MEMBER(bool, AmbientSelfTest);

    DECLARE_MEMBER(std::string, Path);
};
