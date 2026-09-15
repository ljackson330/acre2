#include "AmbientCapture.h"

#include "Log.h"

#include <chrono>

CAmbientCapture *CAmbientCapture::getInstance() {
    static CAmbientCapture instance;
    return &instance;
}

CAmbientCapture::~CAmbientCapture() {
    this->stop();
}

/*
 * Connect to the helper without ever blocking the caller for long.
 *
 * start() runs on the RPC thread that handles startRadioSpeaking, which is in
 * the player's push-to-talk path. A blocking connect to a helper that is not
 * running would stall their PTT, so the socket is put in non-blocking mode and
 * given a short bounded window via select().
 */
bool CAmbientCapture::connectToHelper() {
    // Winsock is refcounted per process. The TeamSpeak client has certainly
    // initialised it already, but relying on a host's internals is fragile and
    // a second startup is cheap.
    if (!this->m_wsaReady) {
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            LOG("AMBIENT: WSAStartup failed -- ambient sound unavailable");
            return false;
        }
        this->m_wsaReady = true;
    }

    this->m_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (this->m_socket == INVALID_SOCKET) {
        LOG("AMBIENT: socket() failed: %d", WSAGetLastError());
        return false;
    }

    u_long nonBlocking = 1;
    ioctlsocket(this->m_socket, FIONBIO, &nonBlocking);

    struct sockaddr_in addr;
    memset(&addr, 0x00, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(HELPER_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    connect(this->m_socket, (struct sockaddr *)&addr, sizeof(addr));

    fd_set writeSet;
    FD_ZERO(&writeSet);
    FD_SET(this->m_socket, &writeSet);

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = CONNECT_TIMEOUT_MS * 1000;

    if (select(0, NULL, &writeSet, NULL, &timeout) <= 0) {
        LOG("AMBIENT: helper not reachable on port %d -- ambient sound disabled "
            "for this transmission", HELPER_PORT);
        closesocket(this->m_socket);
        this->m_socket = INVALID_SOCKET;
        return false;
    }

    // select() reporting writable does not by itself mean the connect
    // succeeded; a refused connection also becomes writable.
    int soError = 0;
    int soErrorLen = sizeof(soError);
    if (getsockopt(this->m_socket, SOL_SOCKET, SO_ERROR, (char *)&soError, &soErrorLen) != 0
        || soError != 0) {
        LOG("AMBIENT: connect refused (%d) -- is ambient-helper.py running?", soError);
        closesocket(this->m_socket);
        this->m_socket = INVALID_SOCKET;
        return false;
    }

    u_long blocking = 0;
    ioctlsocket(this->m_socket, FIONBIO, &blocking);

    // Bound recv() so the reader thread cannot wedge if the helper stops
    // sending without closing the socket.
    DWORD recvTimeout = 500;
    setsockopt(this->m_socket, SOL_SOCKET, SO_RCVTIMEO, (char *)&recvTimeout, sizeof(recvTimeout));

    return true;
}

void CAmbientCapture::start() {
    if (this->m_running.load(std::memory_order_acquire)) {
        return;
    }

    // Join a previous thread that has exited on its own (helper went away).
    if (this->m_thread.joinable()) {
        this->m_thread.join();
    }

    if (!this->connectToHelper()) {
        return;  // fail soft: no ambient audio, voice path untouched
    }

    this->m_bytesThisSession.store(0, std::memory_order_release);
    this->m_running.store(true, std::memory_order_release);
    this->m_thread = std::thread(&CAmbientCapture::readLoop, this);

    LOG("AMBIENT: capture started");
}

void CAmbientCapture::stop() {
    // Do not gate on m_running: readLoop clears it when the helper goes away,
    // and in that case there is still a thread to join and a socket to close.
    const bool hadSession = this->m_thread.joinable() || this->m_socket != INVALID_SOCKET;
    if (!hadSession) {
        return;
    }

    this->m_running.store(false, std::memory_order_release);

    // Shut the socket down first so a blocked recv() returns promptly rather
    // than waiting out its timeout.
    if (this->m_socket != INVALID_SOCKET) {
        shutdown(this->m_socket, SD_BOTH);
    }

    if (this->m_thread.joinable()) {
        this->m_thread.join();
    }

    if (this->m_socket != INVALID_SOCKET) {
        closesocket(this->m_socket);
        this->m_socket = INVALID_SOCKET;
    }

    const uint64_t bytes = this->m_bytesThisSession.load(std::memory_order_acquire);
    LOG("AMBIENT: capture stopped -- %llu bytes (%.2f s of audio)",
        (unsigned long long)bytes, bytes / 2.0 / 48000.0);
}

void CAmbientCapture::readLoop() {
    // Wire format is signed 16-bit mono at 48 kHz: already exactly what
    // onEditCapturedVoiceDataEvent expects, so there is nothing to convert.
    char buffer[4096];

    while (this->m_running.load(std::memory_order_acquire)) {
        const int received = recv(this->m_socket, buffer, sizeof(buffer), 0);

        if (received > 0) {
            this->m_bytesThisSession.fetch_add(received, std::memory_order_relaxed);
            // Step 3 will drain this into a ring buffer. For now the read
            // itself is the thing being verified.
            continue;
        }

        if (received == 0) {
            LOG("AMBIENT: helper closed the connection");
            break;
        }

        const int err = WSAGetLastError();
        if (err == WSAETIMEDOUT) {
            continue;  // no audio yet; keep waiting
        }
        if (this->m_running.load(std::memory_order_acquire)) {
            LOG("AMBIENT: recv failed: %d", err);
        }
        break;
    }

    this->m_running.store(false, std::memory_order_release);
}
