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

#ifndef _AT_NETWORK_H
#define _AT_NETWORK_H

#include <stdbool.h>
#include <telephony/ril.h>

void on_request_network(int request, void* data, size_t datalen, RIL_Token t);
int parseRegistrationState(char* str, int* type, int* items, int** response);
int is3gpp2(int radioTech);
bool try_handle_unsol_net(const char* s);
int mapNetworkRegistrationResponse(int in_response);

#endif