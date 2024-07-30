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

#ifndef _AT_RIL_H
#define _AT_RIL_H

#include <stdbool.h>
#include <telephony/ril.h>

int isConnectionClosed(void);
const struct RIL_Env* getRilEnv(void);
const char* requestToString(int request);

#define RIL_onRequestComplete(t, e, response, responselen) getRilEnv()->OnRequestComplete(t, e, response, responselen)
#define RIL_onUnsolicitedResponse(a, b, c) getRilEnv()->OnUnsolicitedResponse(a, b, c)
#define RIL_requestTimedCallback(a, b, c) getRilEnv()->RequestTimedCallback(a, b, c)

void setRadioState(RIL_RadioState newState);
RIL_RadioState getRadioState(void);

#endif