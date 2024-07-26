/*
 * Copyright (C) 2023 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "LOCAL_SOCKET"
#define NDEBUG 1

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <local_socket.h>
#include <telephony/ril.h>

#define SOCKET_NAME_RIL "rild"

static const char* ENV[32];

int add_environment(const char* key, const char* val)
{
    int n;

    for (n = 0; n < 31; n++) {
        if (!ENV[n]) {
            size_t len = strlen(key) + strlen(val) + 2;
            char* entry = malloc(len);
            snprintf(entry, len, "%s=%s", key, val);
            ENV[n] = entry;
            RLOGD("add_environment success entry is %s\n", ENV[n]);
            RLOGD("add_environment success val is %s\n", val);
            setenv(ENV[n], val, 1);
            return 0;
        }
    }

    return 1;
}

static void publish_socket(const char* name, int fd)
{
    char key[64] = LOCAL_SOCKET_ENV_PREFIX;
    char val[64];

    strlcpy(key + sizeof(LOCAL_SOCKET_ENV_PREFIX) - 1,
        name,
        sizeof(key) - sizeof(LOCAL_SOCKET_ENV_PREFIX));
    snprintf(val, sizeof(val), "%d", fd);
    add_environment(key, val);

    /* make sure we don't close-on-exec */
    fcntl(fd, F_SETFD, 0);
}

int ril_socket_init(void)
{
    char* name = SOCKET_NAME_RIL;
    int serverScoket;
    int socket_type = SOCK_STREAM;

    serverScoket = ril_socket_create(name, socket_type);
    RLOGD("start ril_socket_create success %d\n", serverScoket);

    if (serverScoket >= 0) {
        // put it into envirment
        publish_socket(name, serverScoket);
        return 0;
    } else {
        return -1;
    }
}