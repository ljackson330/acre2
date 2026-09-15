#include "SoundEngine.h"
#include "AcreSettings.h"
#include "AmbientCapture.h"
#include "Engine.h"

#include <cstdint>

namespace {
    // Generous headroom over TeamSpeak's usual 480/960 samples per callback.
    constexpr int MAX_AMBIENT_SAMPLES = 4096;
}

typedef std::numeric_limits<short int> LIMITER;

CSoundEngine::CSoundEngine( void ) {
    this->soundMixer = new CSoundMixer();
}

acre::Result CSoundEngine::onEditPlaybackVoiceDataEvent(acre::id_t id, short* samples, int sampleCount, int channels) {
    if (CEngine::getInstance()->getSoundSystemOverride())
        return acre::Result::ok;
    if (!CEngine::getInstance()->getGameServer())
        return acre::Result::error;
    if (!CEngine::getInstance()->getGameServer()->getConnected())
        return acre::Result::error;
    CPlayer *player;

    for (int x = 0; x < sampleCount * channels; x += channels) {
        for (int i = 0; i < channels; i++) {
            float result = static_cast<float>(samples[x + i]) * CAcreSettings::getInstance()->getPremixGlobalVolume();

            if (result > LIMITER::max()) result = LIMITER::max();
            else if (result < LIMITER::min()) result = LIMITER::min();

            samples[x + i] = static_cast<short>(result);
        }
    }

    auto it = CEngine::getInstance()->speakingList.find(id);
    CEngine::getInstance()->getSoundEngine()->getSoundMixer()->lock();
    if (it != CEngine::getInstance()->speakingList.end()) {
        player = (CPlayer *)it->second;
        LOCK(player);
        if (player->getSpeakingType() != acre::Speaking::unknown) {
            for (size_t i = 0; i < player->channels.size(); ++i) {
                if (player->channels[i]) {
                    player->channels[i]->lock();
                    player->channels[i]->In(samples, sampleCount, channels);
                    player->channels[i]->unlock();
                }
            }
        } else {
            memset(samples, 0x00, (sampleCount*channels)*sizeof(short) );
        }


        UNLOCK(player);
    } else {
        memset(samples, 0x00, (sampleCount*channels)*sizeof(short) );
    }
    CEngine::getInstance()->getSoundEngine()->getSoundMixer()->unlock();
    return acre::Result::ok;
}

acre::Result CSoundEngine::onEditPostProcessVoiceDataEvent(acre::id_t id, short* samples, int sampleCount, int channels, const unsigned int* channelSpeakerArray, unsigned int* channelFillMask) {

    if (CEngine::getInstance()->getSoundSystemOverride())
        return acre::Result::ok;
    if (!CEngine::getInstance()->getGameServer())
        return acre::Result::error;
    if (!CEngine::getInstance()->getGameServer()->getConnected())
        return acre::Result::error;
    memset(samples, 0x00, (sampleCount*channels)*sizeof(short) );
    *channelFillMask = (1<<channels)-1;
    return acre::Result::ok;
}

acre::Result CSoundEngine::onEditMixedPlaybackVoiceDataEvent(short* samples, int sampleCount, int channels, const unsigned int speakerMask) {
    if (CEngine::getInstance()->getSoundSystemOverride())
        return acre::Result::ok;
    if (!CEngine::getInstance()->getGameServer())
        return acre::Result::error;
    if (!CEngine::getInstance()->getGameServer()->getConnected())
        return acre::Result::error;
    memset(samples, 0x00, (sampleCount*channels)*sizeof(short) );
    this->getSoundMixer()->mixDown(samples, sampleCount, channels, speakerMask);

    for (int x = 0; x < sampleCount * channels; x += channels) {
        for (int i = 0; i < channels; i++) {
            float result = static_cast<float>(samples[x + i]) * CAcreSettings::getInstance()->getGlobalVolume();

            if (result > LIMITER::max()) result = LIMITER::max();
            else if (result < LIMITER::min()) result = LIMITER::min();

            samples[x + i] = static_cast<short>(result);
        }
    }

    return acre::Result::ok;
}

acre::Result CSoundEngine::onEditCapturedVoiceDataEvent(short* samples, int sampleCount, int channels) {
    if (CEngine::getInstance()->getSoundSystemOverride())
        return acre::Result::ok;
    if (!CEngine::getInstance()->getGameServer())
        return acre::Result::error;
    if (!CEngine::getInstance()->getGameServer()->getConnected())
        return acre::Result::error;

    /*
     * Ambient battle sound, phase 2 step 3: drain the capture ring but do not
     * yet mix. This verifies buffer health -- start/stop alignment, and
     * over/underrun behaviour across repeated transmissions -- before any
     * change is made to what the listener actually hears.
     *
     * Mixing comes next, and must also set *edited bit 1 in the TeamSpeak
     * callback or the modified samples are discarded.
     */
    CAmbientCapture *ambient = CAmbientCapture::getInstance();
    if (ambient->isRunning() && sampleCount > 0) {
        ambient->logFormatOnce(sampleCount, channels);

        // Fixed storage: this is the audio path, so no allocation here.
        // TeamSpeak delivers 480 or 960 samples per call at 48 kHz.
        int16_t ambientBuffer[MAX_AMBIENT_SAMPLES];
        const int wanted = (sampleCount < MAX_AMBIENT_SAMPLES) ? sampleCount
                                                               : MAX_AMBIENT_SAMPLES;
        ambient->drain(ambientBuffer, wanted);
    }
    /*
    if (CEngine::getInstance()->getSelf()) {
        if (CEngine::getInstance()->getSelf()->getSpeaking()) {
            CEngine::getInstance()->getSoundEngine()->getSoundMixer()->lock();
            CSelf *self = CEngine::getInstance()->getSelf();
            self->lock();
            for (int i = 0; i < self->channels.size(); ++i) {
                if (self->channels[i]) {
                    self->channels[i]->lock();
                    self->channels[i]->In(samples, sampleCount);
                    self->channels[i]->unlock();
                }
            }
            self->unlock();
            CEngine::getInstance()->getSoundEngine()->getSoundMixer()->unlock();
        }
    }
    */
    return acre::Result::ok;
}
