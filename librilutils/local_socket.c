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

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

#include <local_socket.h>
#include <telephony/ril.h>
#define UTF16_REPLACEMENT_CHAR 0xfffd

#define UTF8_SEQ_LENGTH(ch) (((0xe5000000 >> ((ch >> 3) & 0x1e)) & 3) + 1)

#define UTF8_SHIFT_AND_MASK(unicode, byte) \
    (unicode) <<= 6;                       \
    (unicode) |= (0x3f & (byte));

#define UNICODE_UPPER_LIMIT 0x10fffd

int local_get_control_socket(const char* name)
{
    char key[64] = LOCAL_SOCKET_ENV_PREFIX;
    const char* val = NULL;
    int fd = 0;

    strlcpy(key + sizeof(LOCAL_SOCKET_ENV_PREFIX) - 1, name,
        sizeof(key) - sizeof(LOCAL_SOCKET_ENV_PREFIX));

    RLOGD("get env info");
    val = getenv(key);
    RLOGD("get env info val is %s", val);
    if (!val) {
        return -1;
    }

    errno = 0;
    fd = strtol(val, NULL, 10);
    if (errno) {
        return -1;
    }

    return fd;
}

int ril_socket_create(const char* name, int type)
{
    struct sockaddr_un addr;
    int fd, ret;

    fd = socket(PF_UNIX, type, 0);
    if (fd < 0) {
        RLOGE("Failed to open socket '%s': %s\n", name, strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), LOCAL_SOCKET_DIR "/%s", name);
    ret = unlink(addr.sun_path);
    if (ret != 0 && errno != ENOENT) {
        RLOGE("Failed to unlink old socket '%s': %s\n", name, strerror(errno));
        goto out_close;
    }

    ret = bind(fd, (struct sockaddr*)&addr, sizeof(addr));
    if (ret) {
        RLOGE("Failed to bind socket '%s': %s\n", name, strerror(errno));
        goto out_unlink;
    }

    return fd;

out_unlink:
    unlink(addr.sun_path);
out_close:
    close(fd);
    return -1;
}