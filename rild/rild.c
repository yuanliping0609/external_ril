/* //device/system/rild/rild.c
**
** Copyright 2006 The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#define LOG_TAG "RILD"
#define NDEBUG 1

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <local_socket.h>
#include <telephony/ril.h>
#define LIB_PATH_PROPERTY "rild.libpath"
#define LIB_ARGS_PROPERTY "rild.libargs"
#define MAX_LIB_ARGS 16

extern void RIL_register(const RIL_RadioFunctions* callbacks);

extern void RIL_startEventLoop(void);
extern void RIL_onRequestComplete(RIL_Token t, RIL_Errno e,
    void* response, size_t responselen);

extern void RIL_onUnsolicitedResponse(int unsolResponse, const void* data,
    size_t datalen);

extern void RIL_requestTimedCallback(RIL_TimedCallback callback,
    void* param, const struct timeval* relativeTime);

static struct RIL_Env s_rilEnv = {
    RIL_onRequestComplete,
    RIL_onUnsolicitedResponse,
    RIL_requestTimedCallback
};

int main(int argc, char** argv)
{
    const RIL_RadioFunctions* funcs;
    int ret;

    ret = ril_socket_init();

    if (ret < 0) {
        RLOGE("start rile_socket_init failed");
        return 0;
    }

    funcs = RIL_Init(&s_rilEnv, argc, NULL);
    RLOGD("start RIL_register");
    RIL_register(funcs);

    return 0;
}