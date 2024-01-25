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
#ifndef __LOCAL_SOCKET_H__
#define __LOCAL_SOCKET_H__


#define LOCAL_SOCKET_ENV_PREFIX "LOCAL_SOCKET_"
#define LOCAL_SOCKET_DIR "/dev/socket"

#include <string.h>
#include <sys/types.h>
#include <telephony/ril.h>
#include <errno.h>
#include <telephony/ril_log.h>
#ifdef __cplusplus
extern "C" {
#endif

static inline int local_get_control_socket(const char *name)
{
    char key[64] = LOCAL_SOCKET_ENV_PREFIX;
    const char *val = NULL;
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
    fd = strtol(val,NULL,10);
    if (errno) {
        return -1;
    }

    return fd;
}

int ril_socket_create(const char *name, int type);
int ril_socket_init(void);

#ifdef __cplusplus
}
#endif

#endif //__LOCAL_SOCKET_H__