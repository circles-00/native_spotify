#include "platform.h"
#include "core.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>

typedef SOCKET NativeSocket;
#define SOCKET_CLOSE closesocket
#else
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

typedef int NativeSocket;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define SOCKET_CLOSE close
#define MSG_NOSIGNAL_FLAG MSG_NOSIGNAL
#endif
#ifndef MSG_NOSIGNAL_FLAG
#define MSG_NOSIGNAL_FLAG 0
#endif

Listener loopback_open(void) {
    NativeSocket listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (listener == INVALID_SOCKET) {
        return INVALID_LISTENER;
    }

#ifdef _WIN32
    int exclusive = 1;

    if (setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&exclusive, sizeof(exclusive))) {
        SOCKET_CLOSE(listener);

        return INVALID_LISTENER;
    }
#else
    int reuse = 1;

    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse))) {
        SOCKET_CLOSE(listener);

        return INVALID_LISTENER;
    }
#endif

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(43821);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) == SOCKET_ERROR ||
        listen(listener, 4) == SOCKET_ERROR) {
        SOCKET_CLOSE(listener);

        return INVALID_LISTENER;
    }

    return (Listener)listener;
}

static bool readable(NativeSocket socket, uint64_t deadline) {
    uint64_t now = platform_seconds();

    if (now >= deadline) {
        return false;
    }

    fd_set set;
    FD_ZERO(&set);
    FD_SET(socket, &set);

    struct timeval timeout = {(long)(deadline - now), 0};

    return select((int)socket + 1, &set, NULL, NULL, &timeout) > 0;
}

bool loopback_authorize(Listener listener, const char *state, char *code, size_t capacity) {
    uint64_t deadline = platform_seconds() + 120;

    while (readable((NativeSocket)listener, deadline)) {
        NativeSocket client = accept((NativeSocket)listener, NULL, NULL);

        if (client == INVALID_SOCKET) {
            return false;
        }

        char request[8192] = {0};
        size_t length = 0;
        uint64_t client_deadline = platform_seconds() + 3;

        if (client_deadline > deadline) {
            client_deadline = deadline;
        }

        while (length < sizeof(request) - 1 && !strstr(request, "\r\n\r\n") &&
               readable(client, client_deadline)) {
            int count = (int)recv(client, request + length, (int)(sizeof(request) - length - 1), 0);

            if (count <= 0) {
                break;
            }

            length += (size_t)count;
            request[length] = 0;
        }

        CallbackResult result =
            strstr(request, "\r\n\r\n") ? callback_result(request, state, code, capacity) : CALLBACK_INVALID;
        bool ok = result == CALLBACK_CODE;

        const char *response =
            ok ? "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nCache-Control: no-store\r\nConnection: "
                 "close\r\n\r\nAuthorization received. Return to Native Controller.\n"
               : "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\nCache-Control: "
                 "no-store\r\nConnection: close\r\n\r\nInvalid callback or authorization denied.\n";

        send(client, response, (int)strlen(response), MSG_NOSIGNAL_FLAG);
        SOCKET_CLOSE(client);
        platform_wipe(request, sizeof(request));

        if (result == CALLBACK_DENIED) {
            platform_wipe(code, capacity);
            return false;
        }

        if (ok) {
            return true;
        }
    }

    return false;
}

void loopback_close(Listener listener) {
    if (listener != INVALID_LISTENER) {
        SOCKET_CLOSE((NativeSocket)listener);
    }
}
