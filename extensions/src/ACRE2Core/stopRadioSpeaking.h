#pragma once

#include "compat.h"
#include "Types.h"
#include "Macros.h"
#include "Log.h"
#include "IRpcFunction.h"

#include "IServer.h"
#include "Engine.h"

#include "TextMessage.h"
#include "AmbientCapture.h"

RPC_FUNCTION(stopRadioSpeaking) {

    CEngine::getInstance()->getClient()->localStopSpeaking(acre::Speaking::radio);

    CAmbientCapture::getInstance()->stop();

    return acre::Result::ok;
}
public:
    inline void setName(const char *const value) final { m_Name = value; }
    inline const char* getName() const final { return m_Name; }

protected:
    const char* m_Name;
};
