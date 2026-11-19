#include "core.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define CLOSE_SOCKET closesocket
typedef SOCKET TestSocket;
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#define CLOSE_SOCKET close
typedef int TestSocket;
#endif

static unsigned checks;

#define CHECK(expression)                                                                                    \
    do {                                                                                                     \
        ++checks;                                                                                            \
        if (!(expression)) {                                                                                 \
            fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #expression);                          \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)

static void test_encoding(void) {
    char result[128];

    CHECK(base64url((const unsigned char *)"", 0, result, sizeof(result)));
    CHECK(!strcmp(result, ""));
    CHECK(base64url((const unsigned char *)"f", 1, result, sizeof(result)));
    CHECK(!strcmp(result, "Zg"));
    CHECK(base64url((const unsigned char *)"fo", 2, result, sizeof(result)));
    CHECK(!strcmp(result, "Zm8"));
    CHECK(base64url((const unsigned char *)"foo", 3, result, sizeof(result)));
    CHECK(!strcmp(result, "Zm9v"));
    CHECK(!base64url((const unsigned char *)"foo", 3, result, 4));
    CHECK(!base64url((const unsigned char *)"", SIZE_MAX, result, sizeof(result)));

    CHECK(url_encode("hello /+&=é", result, sizeof(result)));
    CHECK(!strcmp(result, "hello%20%2F%2B%26%3D%C3%A9"));
    CHECK(!url_encode("/", result, 3));
    CHECK(!url_encode("", result, 0));
    CHECK(url_encode("-._~AZaz09", result, sizeof(result)));
    CHECK(!strcmp(result, "-._~AZaz09"));
}

static void test_pkce(void) {
    unsigned char digest[32];
    char challenge[44];

    CHECK(platform_sha256("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk", digest));
    CHECK(base64url(digest, sizeof(digest), challenge, sizeof(challenge)));
    CHECK(!strcmp(challenge, "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"));

    Pkce first;
    Pkce second;

    CHECK(pkce_create(&first));
    CHECK(pkce_create(&second));
    CHECK(strlen(first.verifier) == 43);
    CHECK(strlen(first.state) == 43);
    CHECK(strcmp(first.verifier, second.verifier));
    CHECK(strcmp(first.state, second.state));
    CHECK(platform_sha256(first.verifier, digest));
    CHECK(base64url(digest, sizeof(digest), challenge, sizeof(challenge)));
    CHECK(!strcmp(challenge, first.challenge));

    char url[2048];

    CHECK(authorization_url("0123456789abcdef0123456789abcdef", &first, url, sizeof(url)));
    CHECK(strstr(url, "code_challenge_method=S256") != NULL);
    CHECK(strstr(url, "redirect_uri=http%3A%2F%2F127.0.0.1%3A43821%2Fcallback") != NULL);
    CHECK(strstr(url, "client_secret") == NULL);
    CHECK(strstr(url, "user-library-read") != NULL);
    CHECK(strstr(url, "playlist-read-private") != NULL);
    CHECK(strstr(url, "user-read-currently-playing") != NULL);
    CHECK(!authorization_url("bad", &first, url, sizeof(url)));
    CHECK(!authorization_url("0123456789abcdef0123456789abcdef", &first, url, 16));
    CHECK(!client_id_valid(NULL));
    CHECK(!client_id_valid("../../../../../../../../../../../"));
}

static void test_callbacks(void) {
    char code[128];

    CHECK(callback_parse("GET /callback?state=expected&code=a%2Bb%2Fc HTTP/1.1\r\n\r\n", "expected", code,
                         sizeof(code)));
    CHECK(!strcmp(code, "a+b/c"));
    CHECK(callback_parse("GET /callback?code=ok&state=expected HTTP/1.1\r\n\r\n", "expected", code,
                         sizeof(code)));

    const char *invalid[] = {"GET /callback?state=wrong&code=secret HTTP/1.1\r\n\r\n",
                             "GET /callback?code=secret HTTP/1.1\r\n\r\n",
                             "GET /callback?state=expected HTTP/1.1\r\n\r\n",
                             "GET /callback?state=expected&code= HTTP/1.1\r\n\r\n",
                             "GET /callback?state=expected&state=wrong&code=secret HTTP/1.1\r\n\r\n",
                             "GET /callback?state=expected&code=secret&code=other HTTP/1.1\r\n\r\n",
                             "GET /callback?state=expected&code=secret&error=access_denied HTTP/1.1\r\n\r\n",
                             "GET /callback?state=expected&code=%00 HTTP/1.1\r\n\r\n",
                             "GET /callback?state=expected&code=%0d%0a HTTP/1.1\r\n\r\n",
                             "GET /callback?state=expected&code=%zz HTTP/1.1\r\n\r\n",
                             "GET /callback?state=expected&code=% HTTP/1.1\r\n\r\n",
                             "GET /other?state=expected&code=secret HTTP/1.1\r\n\r\n",
                             "POST /callback?state=expected&code=secret HTTP/1.1\r\n\r\n",
                             "GET /callback?state=expected&code=secret"};

    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        CHECK(!callback_parse(invalid[i], "expected", code, sizeof(code)));
    }

    CHECK(!callback_parse("GET /callback?state=expected&code=long HTTP/1.1\r\n\r\n", "expected", code, 2));
    CHECK(callback_result("GET /callback?state=expected&error=access_denied HTTP/1.1\r\n\r\n", "expected",
                          code, sizeof(code)) == CALLBACK_DENIED);
    CHECK(callback_result("GET /callback?state=wrong&error=access_denied HTTP/1.1\r\n\r\n", "expected", code,
                          sizeof(code)) == CALLBACK_INVALID);
    CHECK(callback_result("GET /callback?state=expected&error=access_denied&code=secret HTTP/1.1\r\n\r\n",
                          "expected", code, sizeof(code)) == CALLBACK_INVALID);
    CHECK(callback_result("GET /callback?state=expected&error= HTTP/1.1\r\n\r\n", "expected", code,
                          sizeof(code)) == CALLBACK_INVALID);
}

static void test_tokens(void) {
    Tokens tokens = {0};

    CHECK(tokens_parse("{\"access_token\":\"access\",\"refresh_token\":\"refresh\",\"token_type\":\"Bearer\","
                       "\"expires_in\":3600}",
                       100, &tokens));
    CHECK(!strcmp(tokens.access, "access"));
    CHECK(!strcmp(tokens.refresh, "refresh"));
    CHECK(tokens.expires_at == 3700);

    CHECK(tokens_parse("{\"access_token\":\"new\",\"token_type\":\"Bearer\",\"expires_in\":3600}", 200,
                       &tokens));
    CHECK(!strcmp(tokens.refresh, "refresh"));
    CHECK(tokens.expires_at == 3800);

    CHECK(tokens_parse("{\"access_token\":\"new\",\"refresh_token\":\"rotated\",\"token_type\":\"Bearer\","
                       "\"expires_in\":3600}",
                       300, &tokens));
    CHECK(!strcmp(tokens.refresh, "rotated"));

    Tokens before = tokens;
    const char *invalid[] = {
        "not json",
        "{}",
        "{\"access_token\":\"bad\",\"token_type\":\"Other\",\"expires_in\":3600}",
        "{\"access_token\":\"bad\",\"token_type\":\"Bearer\",\"expires_in\":-1}",
        "{\"access_token\":\"bad\",\"token_type\":\"Bearer\",\"expires_in\":0}",
        "{\"access_token\":\"bad\",\"token_type\":\"Bearer\",\"expires_in\":99999999}",
        "{\"access_token\":\"bad\\r\\nInjected: header\",\"token_type\":\"Bearer\",\"expires_in\":3600}",
        "{\"access_token\":\"bad\\u0000extra\",\"token_type\":\"Bearer\",\"expires_in\":3600}",
        "{\"access_token\":\"bad\",\"refresh_token\":null,\"token_type\":\"Bearer\",\"expires_in\":3600}"};

    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        CHECK(!tokens_parse(invalid[i], 100, &tokens));
        CHECK(!memcmp(&tokens, &before, sizeof(tokens)));
    }

    CHECK(!tokens_parse("{\"access_token\":\"bad\",\"token_type\":\"Bearer\",\"expires_in\":3600}",
                        UINT64_MAX, &tokens));

    memset(&tokens, 0, sizeof(tokens));
    CHECK(!tokens_parse("{\"access_token\":\"bad\",\"token_type\":\"Bearer\",\"expires_in\":3600}", 0,
                        &tokens));
}

static void test_playback(void) {
    Playback playback;

    CHECK(playback_parse("{\"is_playing\":true,\"item\":{\"name\":\"Björk – "
                         "Jóga\",\"artists\":[{\"name\":\"Björk\"}]},\"device\":{\"name\":\"Desktop\"}}",
                         &playback));
    CHECK(playback.playing);
    CHECK(!strcmp(playback.title, "Björk – Jóga"));
    CHECK(!strcmp(playback.artist, "Björk"));
    CHECK(!strcmp(playback.device, "Desktop"));

    CHECK(playback_parse("{\"is_playing\":false,\"item\":null}", &playback));
    CHECK(!playback.playing);
    CHECK(!strcmp(playback.title, "Nothing playing"));
    CHECK(!playback_parse("{}", &playback));
    CHECK(!playback_parse("null", &playback));

    char long_title[800];

    for (size_t i = 0; i < 600; i += 3) {
        memcpy(long_title + i, "♫", 3);
    }

    long_title[600] = 0;

    char json[1024];
    snprintf(json, sizeof(json), "{\"is_playing\":true,\"item\":{\"name\":\"%s\"}}", long_title);

    CHECK(playback_parse(json, &playback));
    CHECK(strlen(playback.title) == 510);
    CHECK((unsigned char)playback.title[509] == 0xab);
}

static void callback_client(void *argument) {
    const char *requests[] = {
        "GET /callback?state=wrong&code=bad HTTP/1.1\r\nHost: 127.0.0.1:43821\r\n\r\n",
        argument
            ? (const char *)argument
            : "GET /callback?state=integration&code=actual-code HTTP/1.1\r\nHost: 127.0.0.1:43821\r\n\r\n"};

    for (size_t i = 0; i < 2; ++i) {
        TestSocket client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        struct sockaddr_in address = {0};
        address.sin_family = AF_INET;
        address.sin_port = htons(43821);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (connect(client, (struct sockaddr *)&address, sizeof(address))) {
            fprintf(stderr, "Loopback test connection failed\n");
            exit(1);
        }

        send(client, requests[i], (int)strlen(requests[i]), 0);

        char response[512];
        recv(client, response, sizeof(response), 0);
        CLOSE_SOCKET(client);
    }
}

static void test_loopback(void) {
    Listener listener = loopback_open();
    CHECK(listener != INVALID_LISTENER);
    CHECK(loopback_open() == INVALID_LISTENER);
    CHECK(platform_start_thread(callback_client, NULL));

    char code[128];
    CHECK(loopback_authorize(listener, "integration", code, sizeof(code)));
    CHECK(!strcmp(code, "actual-code"));

    CHECK(platform_start_thread(callback_client,
                                (void *)"GET /callback?state=integration&error=access_denied "
                                        "HTTP/1.1\r\nHost: 127.0.0.1:43821\r\n\r\n"));
    uint64_t started = platform_milliseconds();
    CHECK(!loopback_authorize(listener, "integration", code, sizeof(code)));
    CHECK(!code[0] && platform_milliseconds() - started < 5000);

    loopback_close(listener);
}

#include "track_order.h"

static void test_track_order(void) {
    CHECK(track_compare_text("beta", "Alpha") > 0);
    CHECK(track_compare_text("alpha", "ALPHA") == 0);
    CHECK(track_filter_matches("Alpha", "Example Artist", "Second Album", "EXAMPLE album"));
    CHECK(!track_filter_matches("Alpha", "Example Artist", "Second Album", "example missing"));
    CHECK(track_filter_matches("A", "B", "C", " \t"));
}

int main(void) {
    test_track_order();
    CHECK(platform_init());

    test_encoding();
    test_pkce();
    test_callbacks();
    test_tokens();
    test_playback();
    test_loopback();

    printf("PASS: %u checks (encoding, RFC 7636, crypto, callbacks, tokens, playback, real loopback)\n",
           checks);

    return 0;
}
