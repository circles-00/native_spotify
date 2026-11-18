#ifndef NATIVE_PLAYER_FIXTURE_H
#define NATIVE_PLAYER_FIXTURE_H

#define PLAYER_FIXTURE                                                                                       \
    "{\"is_playing\":true,\"progress_ms\":60000,"                                                            \
    "\"item\":{\"name\":\"Jóga\",\"duration_ms\":240000},"                                                   \
    "\"device\":{\"id\":\"speaker\\\"\\\\&id\",\"name\":\"Desktop\","                                        \
    "\"is_active\":true,\"is_restricted\":false,\"supports_volume\":true,\"volume_percent\":65}}"

#define DEVICES_FIXTURE                                                                                      \
    "{\"devices\":["                                                                                         \
    "{\"id\":\"other\",\"name\":\"Phone\",\"is_active\":false,\"is_restricted\":false},"                     \
    "{\"id\":null,\"name\":\"Unavailable\",\"is_restricted\":true},"                                         \
    "{\"id\":\"speaker\\\"\\\\&id\",\"name\":\"Desktop\",\"is_active\":true,"                                \
    "\"is_restricted\":false,\"supports_volume\":true,\"volume_percent\":65}]}"

#define QUEUE_TRACK "{\"type\":\"track\",\"id\":\"0123456789abcdefghijkl\",\"name\":\"Track\"}"

#define QUEUE_EPISODE                                                                                        \
    "{\"type\":\"episode\",\"id\":\"abcdefghijkl0123456789\",\"name\":\"Episode\","                          \
    "\"show\":{\"name\":\"Show\"},\"is_playable\":false}"

#endif
