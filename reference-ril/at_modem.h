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

#ifndef _AT_MODEM_H
#define _AT_MODEM_H

#include <stdbool.h>
#include <telephony/ril.h>

typedef struct {
    int supportedTechs; // Bitmask of supported Modem Technology bits
    int currentTech; // Technology the modem is currently using (in the format used by modem)
    int isMultimode;

    // Preferred mode bitmask. This is actually 4 byte-sized bitmasks with different priority values,
    // in which the byte number from LSB to MSB give the priority.
    //
    //          |MSB|   |   |LSB
    // value:   |00 |00 |00 |00
    // byte #:  |3  |2  |1  |0
    //
    // Higher byte order give higher priority. Thus, a value of 0x0000000f represents
    // a preferred mode of GSM, WCDMA, CDMA, and EvDo in which all are equally preferrable, whereas
    // 0x00000201 represents a mode with GSM and WCDMA, in which WCDMA is preferred over GSM
    int32_t preferredNetworkMode;
    int subscription_source;
} ModemInfo;

// TECH returns the current technology in the format used by the modem.
// It can be used as an l-value
#define TECH(mdminfo) ((mdminfo)->currentTech)
// TECH_BIT returns the bitmask equivalent of the current tech
#define TECH_BIT(mdminfo) (1 << ((mdminfo)->currentTech))
#define IS_MULTIMODE(mdminfo) ((mdminfo)->isMultimode)
#define TECH_SUPPORTED(mdminfo, tech) ((mdminfo)->supportedTechs & (tech))
#define PREFERRED_NETWORK(mdminfo) ((mdminfo)->preferredNetworkMode)

/* Modem Technology bits */
#define MDM_GSM 0x01
#define MDM_WCDMA 0x02
#define MDM_CDMA 0x04
#define MDM_EVDO 0x08
#define MDM_TDSCDMA 0x10
#define MDM_LTE 0x20
#define MDM_NR 0x40

void initModem(void);
ModemInfo* getModemInfo(void);
int isModemEnable(void);
int isRadioOn(void);
int query_ctec(ModemInfo* mdm, int* current, int32_t* preferred);
void setRadioTechnology(ModemInfo* mdm, int newtech);
int techFromModemType(int mdmtype);
int parse_technology_response(const char* response, int* current, int32_t* preferred);
void on_request_modem(int request, void* data, size_t datalen, RIL_Token t);
bool try_handle_unsol_modem(const char* s);

#endif