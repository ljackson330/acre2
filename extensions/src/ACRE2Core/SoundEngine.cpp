#include "SoundEngine.h"
#include "AcreSettings.h"
#include "AmbientCapture.h"
#include "Engine.h"

#include <cmath>
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

acre::Result CSoundEngine::onEditCapturedVoiceDataEvent(short* samples, int sampleCount, int channels, int* edited) {
    if (CEngine::getInstance()->getSoundSystemOverride())
        return acre::Result::ok;
    if (!CEngine::getInstance()->getGameServer())
        return acre::Result::error;
    if (!CEngine::getInstance()->getGameServer()->getConnected())
        return acre::Result::error;

    /*
     * Ambient battle sound: mix the local game's audio into the outgoing
     * transmission, so listeners hear the transmitter's combat environment.
     *
     * This is the injection point the whole design targets -- pre-encode, so
     * the receive-side radio DSP (bandpass, noise, distortion) applies to the
     * ambience automatically, exactly as it does to voice.
     */
    CAmbientCapture *ambient = CAmbientCapture::getInstance();
    if (CAcreSettings::getInstance()->getAmbientEnabled()
        && ambient->isRunning() && sampleCount > 0 && channels > 0) {

        ambient->logFormatOnce(sampleCount, channels);

        // Fixed storage: this is the audio path, so no allocation here.
        int16_t ambientBuffer[MAX_AMBIENT_SAMPLES];
        const int frames = (sampleCount < MAX_AMBIENT_SAMPLES) ? sampleCount
                                                               : MAX_AMBIENT_SAMPLES;
        const size_t produced = ambient->drain(ambientBuffer, frames);

        if (produced > 0) {
            const float volume = CAcreSettings::getInstance()->getAmbientVolume();
            double sumSquares = 0.0;
            for (int frame = 0; frame < frames; ++frame) {
                const double v = ambientBuffer[frame];
                sumSquares += v * v;
                const float contribution = ambientBuffer[frame] * volume;
                for (int channel = 0; channel < channels; ++channel) {
                    const int index = (frame * channels) + channel;
                    float mixed = static_cast<float>(samples[index]) + contribution;

                    if (mixed > LIMITER::max()) mixed = LIMITER::max();
                    else if (mixed < LIMITER::min()) mixed = LIMITER::min();

                    samples[index] = static_cast<short>(mixed);
                }
            }

            // Tells us whether ambience actually reached the stream, and at
            // what level, without needing to monitor TeamSpeak externally.
            ambient->noteMixed(sqrt(sumSquares / frames));

            // Bit 1 tells TeamSpeak the samples changed. Without it every
            // edit above is silently discarded.
            if (edited != nullptr) {
                *edited |= 1;
            }
        }

        // Records the outgoing buffer after mixing -- what is actually sent.
        ambient->dumpOutgoing(samples, sampleCount, channels);
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
