/*
** Copyright 2006, The Android Open Source Project
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

#ifndef _AT_SIM_H
#define _AT_SIM_H

#include <stdbool.h>
#include <telephony/ril.h>

typedef enum {
    SIM_ABSENT = 0,
    SIM_NOT_READY = 1,
    SIM_READY = 2,
    SIM_PIN = 3,
    SIM_PUK = 4,
    SIM_NETWORK_PERSONALIZATION = 5,
    RUIM_ABSENT = 6,
    RUIM_NOT_READY = 7,
    RUIM_READY = 8,
    RUIM_PIN = 9,
    RUIM_PUK = 10,
    RUIM_NETWORK_PERSONALIZATION = 11,
    ISIM_ABSENT = 12,
    ISIM_NOT_READY = 13,
    ISIM_READY = 14,
    ISIM_PIN = 15,
    ISIM_PUK = 16,
    ISIM_NETWORK_PERSONALIZATION = 17,
} SIM_Status;

int getMcc(void);
int getMnc(void);
int getMncLength(void);
void pollSIMState(void* param);
SIM_Status getSIMStatus(void);
void on_request_sim(int request, void* data, size_t datalen, RIL_Token t);
bool try_handle_unsol_sim(const char* s);

#endif