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
    // 0 disables. Above zero, the dump is run through the same receive-side
    // radio DSP a listener's client applies, at this signal quality (0..1), so
    // a demo sounds like what is heard rather than what is sent.
    DECLARE_MEMBER(float, AmbientDumpSignalQuality);
    // Absolute path; empty disables. Writes a stereo file with the ambience on
    // the left and the microphone on the right, both taken before they are
    // summed, so DSP can be tried offline against real material. Diagnostic
    // only -- it is not part of what gets transmitted.
    DECLARE_MEMBER(std::string, AmbientDumpSplitFile);
    // Diagnostic: run a short capture at plugin start and report the result to
    // the log, so a tester can confirm the capture backend works without
    // getting in-game and keying a radio.
    DECLARE_MEMBER(bool, AmbientSelfTest);

    DECLARE_MEMBER(std::string, Path);
};
