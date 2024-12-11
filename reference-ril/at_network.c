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

#define LOG_TAG "AT_NETWORK"
#define NDEBUG 1

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/cdefs.h>

#include <log/log_radio.h>
#include <telephony/librilutils.h>

#include "at_modem.h"
#include "at_network.h"
#include "at_ril.h"
#include "at_sim.h"
#include "at_tok.h"
#include "atchannel.h"
#include "misc.h"

#define MAX_OPER_NAME_LENGTH (30)

static int net2modem[] = {
    MDM_GSM | MDM_WCDMA, // 0  - GSM / WCDMA Pref
    MDM_GSM, // 1  - GSM only
    MDM_WCDMA, // 2  - WCDMA only
    MDM_GSM | MDM_WCDMA, // 3  - GSM / WCDMA Auto
    MDM_CDMA | MDM_EVDO, // 4  - CDMA / EvDo Auto
    MDM_CDMA, // 5  - CDMA only
    MDM_EVDO, // 6  - EvDo only
    MDM_GSM | MDM_WCDMA | MDM_CDMA | MDM_EVDO, // 7  - GSM/WCDMA, CDMA, EvDo
    MDM_LTE | MDM_CDMA | MDM_EVDO, // 8  - LTE, CDMA and EvDo
    MDM_LTE | MDM_GSM | MDM_WCDMA, // 9  - LTE, GSM/WCDMA
    MDM_LTE | MDM_CDMA | MDM_EVDO | MDM_GSM | MDM_WCDMA, // 10 - LTE, CDMA, EvDo, GSM/WCDMA
    MDM_LTE, // 11 - LTE only
    MDM_LTE | MDM_WCDMA, // 12 - LTE and WCDMA
    MDM_TDSCDMA, // 13 - TD-SCDMA only
    MDM_WCDMA | MDM_TDSCDMA, // 14 - TD-SCDMA and WCDMA
    MDM_LTE | MDM_TDSCDMA, // 15 - LTE and TD-SCDMA
    MDM_TDSCDMA | MDM_GSM, // 16 - TD-SCDMA and GSM
    MDM_LTE | MDM_TDSCDMA | MDM_GSM, // 17 - TD-SCDMA, GSM and LTE
    MDM_WCDMA | MDM_TDSCDMA | MDM_GSM, // 18 - TD-SCDMA, GSM and WCDMA
    MDM_LTE | MDM_WCDMA | MDM_TDSCDMA, // 19 - LTE, TD-SCDMA and WCDMA
    MDM_LTE | MDM_WCDMA | MDM_TDSCDMA | MDM_GSM, // 20 - LTE, TD-SCDMA, GSM, and WCDMA
    MDM_EVDO | MDM_CDMA | MDM_WCDMA | MDM_TDSCDMA | MDM_GSM, // 21 - TD-SCDMA, CDMA, EVDO, GSM and WCDMA
    MDM_LTE | MDM_TDSCDMA | MDM_CDMA | MDM_EVDO | MDM_WCDMA | MDM_GSM, // 22 - LTE, TDCSDMA, CDMA, EVDO, GSM and WCDMA
    MDM_NR, // 23 - NR 5G only mode
    MDM_NR | MDM_LTE, // 24 - NR 5G, LTE
    MDM_NR | MDM_LTE | MDM_CDMA | MDM_EVDO, // 25 - NR 5G, LTE, CDMA and EvDo
    MDM_NR | MDM_LTE | MDM_WCDMA | MDM_GSM, // 26 - NR 5G, LTE, GSM and WCDMA
    MDM_NR | MDM_LTE | MDM_CDMA | MDM_EVDO | MDM_WCDMA | MDM_GSM, // 27 - NR 5G, LTE, CDMA, EvDo, GSM and WCDMA
    MDM_NR | MDM_LTE | MDM_WCDMA, // 28 - NR 5G, LTE and WCDMA
    MDM_NR | MDM_LTE | MDM_TDSCDMA, // 29 - NR 5G, LTE and TDSCDMA
    MDM_NR | MDM_LTE | MDM_TDSCDMA | MDM_GSM, // 30 - NR 5G, LTE, TD-SCDMA and GSM
    MDM_NR | MDM_LTE | MDM_TDSCDMA | MDM_WCDMA, // 31 - NR 5G, LTE, TD-SCDMA, WCDMA
    MDM_NR | MDM_LTE | MDM_TDSCDMA | MDM_WCDMA | MDM_GSM, // 32 - NR 5G, LTE, TD-SCDMA, GSM and WCDMA
    MDM_NR | MDM_LTE | MDM_TDSCDMA | MDM_CDMA | MDM_EVDO | MDM_WCDMA | MDM_GSM, // 33 - NR 5G, LTE, TD-SCDMA, CDMA, EVDO, GSM and WCDMA
};

static int32_t net2pmask[] = {
    MDM_GSM | (MDM_WCDMA << 8), // 0  - GSM / WCDMA Pref
    MDM_GSM, // 1  - GSM only
    MDM_WCDMA, // 2  - WCDMA only
    MDM_GSM | MDM_WCDMA, // 3  - GSM / WCDMA Auto
    MDM_CDMA | MDM_EVDO, // 4  - CDMA / EvDo Auto
    MDM_CDMA, // 5  - CDMA only
    MDM_EVDO, // 6  - EvDo only
    MDM_GSM | MDM_WCDMA | MDM_CDMA | MDM_EVDO, // 7  - GSM/WCDMA, CDMA, EvDo
    MDM_LTE | MDM_CDMA | MDM_EVDO, // 8  - LTE, CDMA and EvDo
    MDM_LTE | MDM_GSM | MDM_WCDMA, // 9  - LTE, GSM/WCDMA
    MDM_LTE | MDM_CDMA | MDM_EVDO | MDM_GSM | MDM_WCDMA, // 10 - LTE, CDMA, EvDo, GSM/WCDMA
    MDM_LTE, // 11 - LTE only
    MDM_LTE | MDM_WCDMA, // 12 - LTE and WCDMA
    MDM_TDSCDMA, // 13 - TD-SCDMA only
    MDM_WCDMA | MDM_TDSCDMA, // 14 - TD-SCDMA and WCDMA
    MDM_LTE | MDM_TDSCDMA, // 15 - LTE and TD-SCDMA
    MDM_TDSCDMA | MDM_GSM, // 16 - TD-SCDMA and GSM
    MDM_LTE | MDM_TDSCDMA | MDM_GSM, // 17 - TD-SCDMA, GSM and LTE
    MDM_WCDMA | MDM_TDSCDMA | MDM_GSM, // 18 - TD-SCDMA, GSM and WCDMA
    MDM_LTE | MDM_WCDMA | MDM_TDSCDMA, // 19 - LTE, TD-SCDMA and WCDMA
    MDM_LTE | MDM_WCDMA | MDM_TDSCDMA | MDM_GSM, // 20 - LTE, TD-SCDMA, GSM, and WCDMA
    MDM_EVDO | MDM_CDMA | MDM_WCDMA | MDM_TDSCDMA | MDM_GSM, // 21 - TD-SCDMA, CDMA, EVDO, GSM and WCDMA
    MDM_LTE | MDM_TDSCDMA | MDM_CDMA | MDM_EVDO | MDM_WCDMA | MDM_GSM, // 22 - LTE, TDCSDMA, CDMA, EVDO, GSM and WCDMA
    MDM_NR, // 23 - NR 5G only mode
    MDM_NR | MDM_LTE, // 24 - NR 5G, LTE
    MDM_NR | MDM_LTE | MDM_CDMA | MDM_EVDO, // 25 - NR 5G, LTE, CDMA and EvDo
    MDM_NR | MDM_LTE | MDM_WCDMA | MDM_GSM, // 26 - NR 5G, LTE, GSM and WCDMA
    MDM_NR | MDM_LTE | MDM_CDMA | MDM_EVDO | MDM_WCDMA | MDM_GSM, // 27 - NR 5G, LTE, CDMA, EvDo, GSM and WCDMA
    MDM_NR | MDM_LTE | MDM_WCDMA, // 28 - NR 5G, LTE and WCDMA
    MDM_NR | MDM_LTE | MDM_TDSCDMA, // 29 - NR 5G, LTE and TDSCDMA
    MDM_NR | MDM_LTE | MDM_TDSCDMA | MDM_GSM, // 30 - NR 5G, LTE, TD-SCDMA and GSM
    MDM_NR | MDM_LTE | MDM_TDSCDMA | MDM_WCDMA, // 31 - NR 5G, LTE, TD-SCDMA, WCDMA
    MDM_NR | MDM_LTE | MDM_TDSCDMA | MDM_WCDMA | MDM_GSM, // 32 - NR 5G, LTE, TD-SCDMA, GSM and WCDMA
    MDM_NR | MDM_LTE | MDM_TDSCDMA | MDM_CDMA | MDM_EVDO | MDM_WCDMA | MDM_GSM, // 33 - NR 5G, LTE, TD-SCDMA, CDMA, EVDO, GSM and WCDMA
};

static int s_cell_info_rate_ms = INT_MAX;
static int s_lac = 0;
static int s_cid = 0;

static void requestQueryNetworkSelectionMode(void* data, size_t datalen, RIL_Token t)
{
    (void)data;
    (void)datalen;

    int err = -1;
    ATResponse* p_response = NULL;
    RIL_Errno ril_err = RIL_E_SUCCESS;
    int response = 0;
    char* line = NULL;

    err = at_send_command_singleline("AT+COPS?", "+COPS:", &p_response);
    if (err != AT_ERROR_OK || !p_response || p_response->success != AT_OK) {
        RLOGE("Fail to send AT+COPS? due to: %s", at_io_err_str(err));
        ril_err = RIL_E_GENERIC_FAILURE;
        goto on_exit;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) {
        RLOGE("Fail to parse line in %s", __func__);
        ril_err = RIL_E_GENERIC_FAILURE;
        goto on_exit;
    }

    err = at_tok_nextint(&line, &response);
    if (err < 0) {
        RLOGE("Fail to parse mode in %s", __func__);
        ril_err = RIL_E_GENERIC_FAILURE;
        goto on_exit;
    }

on_exit:
    if (ril_err != RIL_E_SUCCESS) {
        RLOGE("requestQueryNetworkSelectionMode must never return error when radio is on");
    }

    RIL_onRequestComplete(t, ril_err, ril_err == RIL_E_SUCCESS ? &response : NULL,
        ril_err == RIL_E_SUCCESS ? sizeof(int) : 0);
    at_response_free(p_response);
}

static void requestSignalStrength(void* data, size_t datalen, RIL_Token t)
{
    (void)data;
    (void)datalen;

    ATResponse* p_response = NULL;
    RIL_Errno ril_err = RIL_E_SUCCESS;
    int err = -1;
    char* line = NULL;
    int count;
    // Accept a response that is at least v6, and up to v12
    int minNumOfElements = sizeof(RIL_SignalStrength_v6) / sizeof(int);
    int maxNumOfElements = sizeof(RIL_SignalStrength_v12) / sizeof(int);
    int response[maxNumOfElements];

    memset(response, 0, sizeof(response));
    err = at_send_command_singleline("AT+CSQ", "+CSQ:", &p_response);
    if (err != AT_ERROR_OK || !p_response || p_response->success != AT_OK) {
        RLOGE("Fail to send AT+CSQ due to: %s", at_io_err_str(err));
        ril_err = RIL_E_GENERIC_FAILURE;
        goto on_exit;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) {
        RLOGE("Fail to parse line in %s", __func__);
        ril_err = RIL_E_GENERIC_FAILURE;
        goto on_exit;
    }

    for (count = 0; count < maxNumOfElements; count++) {
        err = at_tok_nextint(&line, &(response[count]));
        if (err < 0 && count < minNumOfElements) {
            RLOGE("Fail to parse signal strength in %s", __func__);
            ril_err = RIL_E_GENERIC_FAILURE;
            goto on_exit;
        }
    }

on_exit:
    if (ril_err != RIL_E_SUCCESS) {
        RLOGE("requestSignalStrength must never return an error when radio is on");
    }

    RIL_onRequestComplete(t, ril_err, ril_err == RIL_E_SUCCESS ? response : NULL,
        ril_err == RIL_E_SUCCESS ? sizeof(response) : 0);
    at_response_free(p_response);
}

/**
 * networkModePossible. Decides whether the network mode is appropriate for the
 * specified modem
 */
static int networkModePossible(ModemInfo* mdm, int nm)
{
    const int asize = sizeof(net2modem) / sizeof(net2modem[0]);

    if (nm >= asize || nm < 0) {
        RLOGW("%s %d: invalid net2modem index: %d", __func__, __LINE__, nm);
        return 0;
    }

    if ((net2modem[nm] & mdm->supportedTechs) == net2modem[nm]) {
        return 1;
    }

    return 0;
}

static void requestSetPreferredNetworkType(void* data, size_t datalen, RIL_Token t)
{
    (void)datalen;

    ATResponse* p_response = NULL;
    RIL_Errno ril_err = RIL_E_SUCCESS;
    char* cmd = NULL;
    int value;
    int current, old;
    int err = -1;
    int32_t preferred;

    if (data == NULL) {
        RLOGE("requestSetPreferredNetworkType data is NULL");
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    value = *(int*)data;
    if (value < 0 || value >= (sizeof(net2pmask) / sizeof(net2pmask[0]))) {
        RLOGE("data is invalid");
        RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
        return;
    }

    preferred = net2pmask[value];

    RLOGD("requestSetPreferredNetworkType: current: %lx. New: %lx",
        PREFERRED_NETWORK(getModemInfo()), preferred);
    if (!networkModePossible(getModemInfo(), value)) {
        RIL_onRequestComplete(t, RIL_E_MODE_NOT_SUPPORTED, NULL, 0);
        return;
    }

    if (query_ctec(getModemInfo(), &current, NULL) < 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    old = PREFERRED_NETWORK(getModemInfo());
    RLOGD("old != preferred: %d", old != preferred);

    if (old != preferred) {
        if (asprintf(&cmd, "AT+CTEC=%d,\"%lx\"", current, preferred) < 0) {
            RLOGE("Failed to allocate memory");
            ril_err = RIL_E_NO_MEMORY;
            goto on_exit;
        }

        RLOGD("Sending command: <%s>", cmd);
        err = at_send_command_singleline(cmd, "+CTEC:", &p_response);

        if (err != AT_ERROR_OK || !p_response || p_response->success != AT_OK) {
            RLOGE("Failure occurred in sending %s due to: %s", cmd, at_io_err_str(err));
            ril_err = RIL_E_GENERIC_FAILURE;
            goto on_exit;
        }

        PREFERRED_NETWORK(getModemInfo()) = value;
        if (!strstr(p_response->p_intermediates->line, "DONE")) {
            int curr;
            int res = parse_technology_response(p_response->p_intermediates->line, &curr, NULL);
            switch (res) {
            case -1: // Error or unable to parse
                break;
            case 1: // Only able to parse current
            case 0: // Both current and preferred were parsed
                setRadioTechnology(getModemInfo(), curr);
                break;
            }
        }
    }

on_exit:
    RIL_onRequestComplete(t, ril_err, NULL, 0);
    at_response_free(p_response);
    free(cmd);
}

static void requestGetPreferredNetworkType(void* data, size_t datalen, RIL_Token t)
{
    (void)data;
    (void)datalen;

    int32_t preferred;
    unsigned i;

    switch (query_ctec(getModemInfo(), NULL, &preferred)) {
    case -1: // Error or unable to parse
    case 1: // Only able to parse current
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        break;
    case 0: // Both current and preferred were parsed
        for (i = 0; i < sizeof(net2pmask) / sizeof(int32_t); i++) {
            if (preferred == net2pmask[i]) {
                goto done;
            }
        }

        RLOGE("Unknown preferred mode received from modem: %ld", preferred);
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

done:
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &i, sizeof(i));
}

/*
 * RIL_REQUEST_IMS_REG_STATE_CHANGE
 * open/close ims resgister
 *
 * "data" is int *
 * ((int *)data)[0] == 1 open ims reg
 * ((int *)data)[0] == 0 colse ims reg
 *
 * "response" is NULL
 */
static void requestImsRegStateChange(void* data, size_t datalen, RIL_Token t)
{
    (void)datalen;

    int is_on = 0;
    char* cmd = NULL;
    ATResponse* p_response = NULL;
    RIL_Errno ril_err = RIL_E_SUCCESS;
    int err = -1;

    if (data == NULL) {
        RLOGD("requestImsRegStateChange data is NULL");
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    is_on = ((int*)data)[0];
    RLOGD("set volte: is_on = %d\n", is_on);

    if (is_on != 0 && is_on != 1) {
        RLOGE("Invalid arguments in RIL");
        ril_err = RIL_E_GENERIC_FAILURE;
        goto on_exit;
    }

    if (asprintf(&cmd, "AT+CAVIMS=%d", is_on) < 0) {
        RLOGE("Failed to allocate memory");
        ril_err = RIL_E_NO_MEMORY;
        goto on_exit;
    }

    err = at_send_command(cmd, &p_response);
    if (err != AT_ERROR_OK || !p_response || p_response->success != AT_OK) {
        RLOGE("Failure occurred in sending %s due to: %s", cmd, at_io_err_str(err));
        ril_err = RIL_E_GENERIC_FAILURE;
        goto on_exit;
    }

on_exit:
    RIL_onRequestComplete(t, ril_err, NULL, 0);
    at_response_free(p_response);
    free(cmd);
}

/*
 * RIL_REQUEST_IMS_SET_SERVICE_STATUS
 *
 * Request set current IMS service state
 *
 * "data" is int *
 * ((int *)data)[0]
 * 0x01: voice
 * 0x04: sms
 * 0x05: voice & sms
 *
 * "response" is NULL
 */
static void requestImsSetServiceStatus(void* data, size_t datalen, RIL_Token t)
{
    (void)datalen;

    int ims_service = 0;
    char* cmd = NULL;
    ATResponse* p_response = NULL;
    RIL_Errno ril_err = RIL_E_SUCCESS;
    int err = -1;

    if (data == NULL) {
        RLOGD("requestImsSetServiceStatus data is NULL");
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    ims_service = (*(int*)data);
    RLOGD("set ims_service : ims_service = %d\n", ims_service);
    if (ims_service != 1 && ims_service != 4 && ims_service != 5) {
        RLOGE("Invalid arguments in RIL");
        ril_err = RIL_E_INVALID_ARGUMENTS;
        goto on_exit;
    }

    if (asprintf(&cmd, "AT+CASIMS=%d", ims_service) < 0) {
        RLOGE("Failed to allocate memory");
        ril_err = RIL_E_NO_MEMORY;
        goto on_exit;
    }

    err = at_send_command(cmd, &p_response);
    if (err != AT_ERROR_OK || !p_response || p_response->success != AT_OK) {
        RLOGE("Failure occurred in sending %s due to: %s", cmd, at_io_err_str(err));
        ril_err = RIL_E_GENERIC_FAILURE;
        goto on_exit;
    }

on_exit:
    RIL_onRequestComplete(t, ril_err, NULL, 0);
    at_response_free(p_response);
    free(cmd);
}

static void requestSetNetworlSelectionManual(void* data, size_t datalen, RIL_Token t)
{
    (void)datalen;

    int err = -1;
    int ret = -1;
    char* cmd = NULL;
    ATResponse* p_response = NULL;
    RIL_Errno ril_err = RIL_E_SUCCESS;
    RIL_NetworkOperator* operator= NULL;

    if (data == NULL) {
        RLOGE("requestSetNetworlSelectionManual data is NULL");
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    operator=(RIL_NetworkOperator*) data;
    ret = asprintf(&cmd, "AT+COPS=1,2,\"%s\",%d", operator->operatorNumeric, (int)operator->act);
    if (ret < 0) {
        RLOGE("Failed to allocate memory");
        ril_err = RIL_E_NO_MEMORY;
        goto on_exit;
    }

    err = at_send_command(cmd, &p_response);
    if (err != AT_ERROR_OK || !p_response || p_response->success != AT_OK) {
        RLOGE("Failure occurred in sending %s due to: %s", cmd, at_io_err_str(err));
        ril_err = RIL_E_GENERIC_FAILURE;
        goto on_exit;
    }

on_exit:
    if (ril_err != RIL_E_SUCCESS) {
        if (p_response != NULL && !strcmp(p_response->finalResponse, "+CME ERROR: 30")) {
            ril_err = RIL_E_RADIO_NOT_AVAILABLE;
        } else {
            ril_err = RIL_E_GENERIC_FAILURE;
        }
    }

    RIL_onRequestComplete(t, ril_err, NULL, 0);
    at_response_free(p_response);
    free(cmd);
}

void requestQueryAvailableNetworks(void* data, size_t datalen, RIL_Token t)
{
    (void)data;
    (void)datalen;

    ATResponse* p_response = NULL;
    RIL_Errno ril_err = RIL_E_SUCCESS;
    int err = -1;
    char* line;
    int len;
    int i, j, k;
    int nplmns;
    int nplmns_valid;
    char** response = NULL;
    char** response_valid = NULL;
    char* str = NULL;
    char* item = NULL;
    char* result = NULL;

    err = at_send_command_singleline("AT+COPS=?", "+COPS:", &p_response);
    if (err != AT_ERROR_OK || !p_response || p_response->success != AT_OK) {
        RLOGE("Failure occurred in sending %s due to: %s", "AT+COPS=?", at_io_err_str(err));
        ril_err = RIL_E_GENERIC_FAILURE;
        goto on_exit;
    }

    /*
     * response is +COPS: (3,"CHINA MOBILE","CMCC","46000"),(3,"CHINA-UNICOM","UNICOM","46001"),
     */
    line = p_response->p_intermediates->line;
    len = strlen(line);

    for (i = 0, nplmns = 0, nplmns_valid = 0; i < len; i++) {
        if (line[i] == ')') {
            nplmns++;
            nplmns_valid++;
        }
    }

    response = (char**)calloc(1, sizeof(char*) * nplmns * 4);
    if (!response) {
        RLOGE("Memory allocation failed in requestQueryAvailableNetworks");
        ril_err = RIL_E_NO_MEMORY;
        goto on_exit;
    }

    item = (char*)calloc(1, nplmns * sizeof(char) * 4 * MAX_OPER_NAME_LENGTH);
    if (!item) {
        RLOGE("Memory allocation failed in requestQueryAvailableNetworks");
        ril_err = RIL_E_NO_MEMORY;
        goto on_exit;
    }

    result = line;
    for (i = 0, j = 0; i < nplmns; i++, j += 4) {
        char* next_item = strchr(result, '(');
        if (!next_item)
            break;

        result = next_item + 1;
        str = strchr(result, ',');
        if (!str)
            break;

        *str++ = '\0';
        response[j + 3] = &item[(j + 3) * MAX_OPER_NAME_LENGTH];

        switch (atoi(result)) {
        case 0:
            strcpy(response[j + 3], "unknown");
            break;
        case 1:
            strcpy(response[j + 3], "available");
            break;
        case 2:
            strcpy(response[j + 3], "current");
            break;
        case 3:
            strcpy(response[j + 3], "forbidden");
            break;
        default:
            RLOGE("<stat> %d is an invalid value", i);
            break;
        }

        result = strchr(str, ',');
        if (!result)
            break;

        *result++ = '\0';
        response[j + 0] = &item[(j + 0) * MAX_OPER_NAME_LENGTH];
        strcpy(response[j + 0], str);

        str = strchr(result, ',');
        if (!str)
            break;

        *str++ = '\0';
        response[j + 1] = &item[(j + 1) * MAX_OPER_NAME_LENGTH];
        strcpy(response[j + 1], result);

        result = strchr(str, ')');
        if (!result)
            break;

        *result++ = '\0';
        response[j + 2] = &item[(j + 2) * MAX_OPER_NAME_LENGTH];
        strcpy(response[j + 2], str);

        len = strlen(response[j + 2]);
        if (len != 5 && len != 6) {
            RLOGE("The length of the numeric code is incorrect");
            ril_err = RIL_E_GENERIC_FAILURE;
            goto on_exit;
        }

        for (k = 0; k < j; k += 4) {
            if (0 == strncmp(response[j + 2], response[k + 2], MAX_OPER_NAME_LENGTH)) {
                response[j + 2] = "";
                nplmns_valid--;
                break;
            }
        }
    }

    response_valid = (char**)calloc(1, sizeof(char*) * nplmns_valid * 4);
    if (!response_valid) {
        RLOGE("Memory allocation failed in requestQueryAvailableNetworks");
        ril_err = RIL_E_NO_MEMORY;
        goto on_exit;
    }

    for (i = 0, k = 0; i < nplmns; i++) {
        if (response[i * 4 + 2] && strlen(response[i * 4 + 2]) > 0) {
            response_valid[k + 0] = response[i * 4 + 0];
            response_valid[k + 1] = response[i * 4 + 1];
            response_valid[k + 2] = response[i * 4 + 2];
            response_valid[k + 3] = response[i * 4 + 3];
            k += 4;
        }
    }

on_exit:
    RIL_onRequestComplete(t, ril_err, ril_err == RIL_E_SUCCESS ? response_valid : NULL,
        ril_err == RIL_E_SUCCESS ? sizeof(char*) * nplmns_valid * 4 : 0);
    at_response_free(p_response);
    free(response_valid);
    free(response);
    free(item);
}

static void requestSetCellInfoListRate(void* data, size_t datalen, RIL_Token t)
{
    (void)datalen;

    // For now we'll save the rate but no RIL_UNSOL_CELL_INFO_LIST messages
    // will be sent.
    assert(datalen == sizeof(int));
    s_cell_info_rate_ms = ((int*)data)[0];

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static int get_lte_cell_info_from_response(char* line, RIL_CellInfo_v12* info)
{
    int err = -1;
    const int invalid = 0x7FFFFFFF;

    err = at_tok_nextint(&line, &info->CellInfo.lte.cellIdentityLte.mcc);
    if (err < 0) {
        RLOGE("Fail to parse mcc in %s", __func__);
        goto on_exit;
    }

    err = at_tok_nextint(&line, &info->CellInfo.lte.cellIdentityLte.mnc);
    if (err < 0) {
        RLOGE("Fail to parse mnc in %s", __func__);
        goto on_exit;
    }

    err = at_tok_nextint(&line, &info->CellInfo.lte.cellIdentityLte.ci);
    if (err < 0) {
        RLOGE("Fail to parse ci in %s", __func__);
        goto on_exit;
    }

    err = at_tok_nextint(&line, &info->CellInfo.lte.cellIdentityLte.pci);
    if (err < 0) {
        RLOGE("Fail to parse pci in %s", __func__);
        goto on_exit;
    }

    err = at_tok_nextint(&line, &info->CellInfo.lte.cellIdentityLte.tac);
    if (err < 0) {
        RLOGE("Fail to parse tac in %s", __func__);
        goto on_exit;
    }

    err = at_tok_nextint(&line, &info->CellInfo.lte.cellIdentityLte.earfcn);
    if (err < 0) {
        RLOGE("Fail to parse earfcn in %s", __func__);
        goto on_exit;
    }

    err = at_tok_nextint(&line, &info->CellInfo.lte.signalStrengthLte.signalStrength);
    if (err < 0) {
        RLOGE("Fail to parse signalStrength in %s", __func__);
        goto on_exit;
    }

    err = at_tok_nextint(&line, &info->CellInfo.lte.signalStrengthLte.rsrp);
    if (err < 0) {
        RLOGE("Fail to parse rsrp in %s", __func__);
        goto on_exit;
    }

    err = at_tok_nextint(&line, &info->CellInfo.lte.signalStrengthLte.rsrq);
    if (err < 0) {
        RLOGE("Fail to parse rsrq in %s", __func__);
        goto on_exit;
    }

    info->CellInfo.lte.signalStrengthLte.rssnr = invalid;
    info->CellInfo.lte.signalStrengthLte.cqi = invalid;
    info->CellInfo.lte.signalStrengthLte.timingAdvance = invalid;

on_exit:
    return err;
}

static int get_cell_info_from_response(char* line, RIL_CellInfo_v12* info)
{
    int ret = 0;

    assert(line);
    assert(info);

    switch (info->cellInfoType) {
    case RIL_CELL_INFO_TYPE_LTE:
        ret = get_lte_cell_info_from_response(line, info);
        break;
    default:
        RLOGE("Unsupported cell info type %d", info->cellInfoType);
        ret = -1;
        break;
    }

    return ret;
}

static int get_neighboring_cell_info_from_response(char* line, RIL_CellInfo_v12* info)
{
    int err = -1;
    int ret = 0;
    char* type = NULL;

    err = at_tok_start(&line);
    if (err < 0) {
        RLOGE("Fail to parse line in %s", __func__);
        goto on_exit;
    }

    err = at_tok_nextstr(&line, &type);
    if (err < 0) {
        RLOGE("Fail to parse neighboring cellInfoType in %s", __func__);
        goto on_exit;
    }

    if (!strcmp(type, "LTE") || !strcmp(type, "1")) {
        RLOGI("The neighboring cell info type is LTE!");

        uint64_t curtime;

        info->cellInfoType = RIL_CELL_INFO_TYPE_LTE;
        info->registered = 0;
        info->timeStampType = RIL_TIMESTAMP_TYPE_OEM_RIL;
        curtime = ril_nano_time();
        info->timeStamp = curtime - 1000;
    } else if (!strcmp(type, "NONE")) {
        RLOGW("No available neighboring cells found");
        ret = 0;
        goto on_exit;
    } else {
        RLOGE("Unsupported neighboring cell info type %s", type);
        ret = -1;
        goto on_exit;
    }

    ret = get_cell_info_from_response(line, info);
    if (ret == 0)
        ret = 1;

on_exit:
    return ret;
}

static void get_neighboring_cell_info_list(RIL_CellInfo_v12* ci, RIL_Token t)
{
    ATResponse* p_response = NULL;
    RIL_CellInfo_v12* cell_info_lists = NULL;
    RIL_Errno ril_err = RIL_E_SUCCESS;
    int cell_num = 0;
    int cell_num_lte = 0;
    ATLine* cur = NULL;
    int err = -1;

    err = at_send_command_multiline("AT^MONNC", "^MONNC:", &p_response);
    if (err != AT_ERROR_OK || !p_response || p_response->success != AT_OK) {
        RLOGE("Failure occurred in sending %s due to: %s", "AT^MONNC", at_io_err_str(err));
        ril_err = RIL_E_GENERIC_FAILURE;
        goto on_exit;
    }

    for (cur = p_response->p_intermediates; cur; cur = cur->p_next) {
        cell_num++;
    }

    cell_info_lists = calloc(cell_num + 1, sizeof(RIL_CellInfo_v12));
    if (cell_info_lists == NULL) {
        RLOGE("Fail to allocate memory in %s", __func__);
        ril_err = RIL_E_NO_MEMORY;
        goto on_exit;
    }

    for (cur = p_response->p_intermediates; cur != NULL && cell_num_lte < cell_num; cur = cur->p_next) {
        err = get_neighboring_cell_info_from_response(cur->line, cell_info_lists + cell_num_lte + 1);
        if (err < 0) {
            RLOGE("Fail to parse neighboring cell info");
            ril_err = RIL_E_GENERIC_FAILURE;
            goto on_exit;
        } else if (err == 0) {
            RLOGW("No available neighboring cell info");
            ril_err = RIL_E_SUCCESS;
            break;
        }

        cell_num_lte++;
    }

    memcpy(&cell_info_lists[0], ci, sizeof(RIL_CellInfo_v12));

on_exit:
    RIL_onRequestComplete(t, ril_err, ril_err == RIL_E_SUCCESS ? cell_info_lists : ci,
        ril_err == RIL_E_SUCCESS ? (cell_num_lte + 1) * sizeof(RIL_CellInfo_v12) : sizeof(RIL_CellInfo_v12));
    at_response_free(p_response);
    free(cell_info_lists);
}

static void requestGetCellInfoList(void* data, size_t datalen, RIL_Token t)
{
    (void)data;
    (void)datalen;

    ATResponse* p_response = NULL;
    RIL_Errno ril_err = RIL_E_SUCCESS;
    RIL_CellInfo_v12* ci = NULL;
    char* line = NULL;
    char* type = NULL;
    uint64_t curtime;
    int err = -1;

    err = at_send_command_singleline("AT^MONSC", "^MONSC:", &p_response);
    if (err != AT_ERROR_OK || !p_response || p_response->success != AT_OK) {
        RLOGE("Failure occurred in sending %s due to: %s", "AT^MONSC", at_io_err_str(err));
        ril_err = RIL_E_GENERIC_FAILURE;
        goto on_exit;
    }

    line = p_response->p_intermediates->line;
    ci = (RIL_CellInfo_v12*)calloc(1, sizeof(RIL_CellInfo_v12));
    if (ci == NULL) {
        RLOGE("Fail to allocate memory in %s", __func__);
        ril_err = RIL_E_NO_MEMORY;
        goto on_exit;
    }

    err = at_tok_start(&line);
    if (err < 0) {
        RLOGE("Fail to parse line in %s", __func__);
        ril_err = RIL_E_GENERIC_FAILURE;
        goto on_exit;
    }

    err = at_tok_nextstr(&line, &type);
    if (err < 0) {
        RLOGE("Fail to parse cellInfoType in %s", __func__);
        ril_err = RIL_E_GENERIC_FAILURE;
        goto on_exit;
    }

    if (!strcmp(type, "LTE") || !strcmp(type, "1")) {
        RLOGI("The cell info type is LTE!");

        ci->cellInfoType = RIL_CELL_INFO_TYPE_LTE;
        ci->registered = 1;
        ci->timeStampType = RIL_TIMESTAMP_TYPE_OEM_RIL;
        curtime = ril_nano_time();
        ci->timeStamp = curtime - 1000;
    } else {
        RLOGE("The cell info type is not invalid!");
        ril_err = RIL_E_GENERIC_FAILURE;
        goto on_exit;
    }

    if (get_cell_info_from_response(line, ci) < 0) {
        RLOGE("Fail to parse cell info in %s", __func__);
        ril_err = RIL_E_GENERIC_FAILURE;
        goto on_exit;
    }

on_exit:
    if (ril_err == RIL_E_SUCCESS) {
        get_neighboring_cell_info_list(ci, t);
    } else {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    }

    at_response_free(p_response);
    free(ci);
}

static void requestImsRegState(void* data, size_t datalen, RIL_Token t)
{
    (void)data;
    (void)datalen;

    ATResponse* p_response = NULL;
    RIL_Errno ril_err = RIL_E_SUCCESS;
    int err = -1;
    char* line = NULL;
    RIL_IMS_REGISTRATION_STATE_RESPONSE reply;

    err = at_send_command_singleline("AT+CIREG?", "+CIREG:", &p_response);
    if (err != AT_ERROR_OK || !p_response || p_response->success != AT_OK) {
        RLOGE("Failure occurred in sending %s due to: %s", "AT+CIREG?", at_io_err_str(err));
        ril_err = RIL_E_GENERIC_FAILURE;
        goto on_exit;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) {
        RLOGE("Fail to parse line in %s", __func__);
        ril_err = RIL_E_GENERIC_FAILURE;
        goto on_exit;
    }

    err = at_tok_nextint(&line, &reply.reg_state);
    if (err < 0) {
        RLOGE("Fail to parse reg_state in %s", __func__);
        ril_err = RIL_E_GENERIC_FAILURE;
        goto on_exit;
    }

    err = at_tok_nextint(&line, &reply.service_type);
    if (err < 0) {
        RLOGE("Fail to parse service_type in %s", __func__);
        ril_err = RIL_E_GENERIC_FAILURE;
        goto on_exit;
    }

    err = at_send_command_singleline("AT+CNUM", "+CNUM:", &p_response);
    if (err != AT_ERROR_OK || !p_response || p_response->success != AT_OK) {
        RLOGE("Failure occurred in sending %s due to: %s", "AT+CNUM", at_io_err_str(err));
        ril_err = RIL_E_GENERIC_FAILURE;
        goto on_exit;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) {
        RLOGE("Fail to parse line in %s", __func__);
        ril_err = RIL_E_GENERIC_FAILURE;
        goto on_exit;
    }

    err = at_tok_nextstr(&line, &reply.uri_response);
    if (err < 0) {
        RLOGE("Fail to parse uri in %s", __func__);
        ril_err = RIL_E_GENERIC_FAILURE;
        goto on_exit;
    }

on_exit:
    RIL_onRequestComplete(t, ril_err, ril_err == RIL_E_SUCCESS ? &reply : NULL,
        ril_err == RIL_E_SUCCESS ? sizeof(reply) : 0);
    at_response_free(p_response);
}

#define REG_STATE_LEN 18
static void requestVoiceRegistrationState(void* data, size_t datalen, RIL_Token t)
{
    int err;
    int* registration = NULL;
    char** responseStr = NULL;
    ATResponse* p_response = NULL;
    RIL_Errno ril_err = RIL_E_SUCCESS;
    const char* cmd;
    const char* prefix;
    char* line;
    int i = 0, j, numElements = 0;
    int count = 3;
    int type, startfrom;

    RLOGD("requestRegistrationState");
    cmd = "AT+CREG?";
    prefix = "+CREG:";
    numElements = REG_STATE_LEN;

    err = at_send_command_singleline(cmd, prefix, &p_response);
    if (err != AT_ERROR_OK || !p_response || p_response->success != AT_OK) {
        RLOGE("Failure occurred in sending %s due to: %s", cmd, at_io_err_str(err));
        ril_err = RIL_E_SUCCESS;
        goto on_exit;
    }

    line = p_response->p_intermediates->line;

    if (parseRegistrationState(line, &type, &count, &registration)) {
        RLOGE("Fail to parse registration state in %s", __func__);
        ril_err = RIL_E_GENERIC_FAILURE;
        goto on_exit;
    }

    responseStr = malloc(numElements * sizeof(char*));
    if (!responseStr) {
        RLOGE("Failed to allocate memory");
        ril_err = RIL_E_NO_MEMORY;
        goto on_exit;
    }

    memset(responseStr, 0, numElements * sizeof(char*));

    /**
     * The first '4' bytes for both registration states remain the same.
     * But if the request is 'DATA_REGISTRATION_STATE',
     * the 5th and 6th byte(s) are optional.
     */
    if (is3gpp2(type) == 1) {
        RLOGD("registration state type: 3GPP2");
        // TODO: Query modem
        startfrom = 3;

        // EvDo revA
        if (asprintf(&responseStr[3], "8") < 0) {
            RLOGE("Failed to allocate memory");
            ril_err = RIL_E_NO_MEMORY;
            goto on_exit;
        }

        // BSID
        if (asprintf(&responseStr[4], "1") < 0) {
            RLOGE("Failed to allocate memory");
            ril_err = RIL_E_NO_MEMORY;
            goto on_exit;
        }

        // Latitude
        if (asprintf(&responseStr[5], "123") < 0) {
            RLOGE("Failed to allocate memory");
            ril_err = RIL_E_NO_MEMORY;
            goto on_exit;
        }

        // Longitude
        if (asprintf(&responseStr[6], "222") < 0) {
            RLOGE("Failed to allocate memory");
            ril_err = RIL_E_NO_MEMORY;
            goto on_exit;
        }

        // CSS Indicator
        if (asprintf(&responseStr[7], "0") < 0) {
            RLOGE("Failed to allocate memory");
            ril_err = RIL_E_NO_MEMORY;
            goto on_exit;
        }

        // SID
        if (asprintf(&responseStr[8], "4") < 0) {
            RLOGE("Failed to allocate memory");
            ril_err = RIL_E_NO_MEMORY;
            goto on_exit;
        }

        // NID
        if (asprintf(&responseStr[9], "65535") < 0) {
            RLOGE("Failed to allocate memory");
            ril_err = RIL_E_NO_MEMORY;
            goto on_exit;
        }

        // Roaming indicator
        if (asprintf(&responseStr[10], "0") < 0) {
            RLOGE("Failed to allocate memory");
            ril_err = RIL_E_NO_MEMORY;
            goto on_exit;
        }

        // System is in PRL
        if (asprintf(&responseStr[11], "1") < 0) {
            RLOGE("Failed to allocate memory");
            ril_err = RIL_E_NO_MEMORY;
            goto on_exit;
        }

        // Default Roaming indicator
        if (asprintf(&responseStr[12], "0") < 0) {
            RLOGE("Failed to allocate memory");
            ril_err = RIL_E_NO_MEMORY;
            goto on_exit;
        }

        // Reason for denial
        if (asprintf(&responseStr[13], "0") < 0) {
            RLOGE("Failed to allocate memory");
            ril_err = RIL_E_NO_MEMORY;
            goto on_exit;
        }

        // Primary Scrambling Code of Current cell
        if (asprintf(&responseStr[14], "0") < 0) {
            RLOGE("Failed to allocate memory");
            ril_err = RIL_E_NO_MEMORY;
            goto on_exit;
        }
    } else { // type == RADIO_TECH_3GPP
        RLOGD("registration state type: 3GPP");
        startfrom = 0;

        if (count > 1) {
            if (asprintf(&responseStr[1], "%x", registration[1]) < 0) {
                RLOGE("Failed to allocate memory");
                ril_err = RIL_E_NO_MEMORY;
                goto on_exit;
            }
        }

        if (count > 2) {
            if (asprintf(&responseStr[2], "%x", registration[2]) < 0) {
                RLOGE("Failed to allocate memory");
                ril_err = RIL_E_NO_MEMORY;
                goto on_exit;
            }
        }

        if (count > 3) {
            if (asprintf(&responseStr[3], "%d",
                    mapNetworkRegistrationResponse(registration[3]))
                < 0) {
                RLOGE("Failed to allocate memory");
                ril_err = RIL_E_NO_MEMORY;
                goto on_exit;
            }
        }
    }

    if (asprintf(&responseStr[0], "%d", registration[0]) < 0) {
        RLOGE("Failed to allocate memory");
        ril_err = RIL_E_NO_MEMORY;
        goto on_exit;
    }

    if (asprintf(&responseStr[15], "%d", getMcc()) < 0) {
        RLOGE("Failed to allocate memory");
        ril_err = RIL_E_NO_MEMORY;
        goto on_exit;
    }

    if (asprintf(&responseStr[16], "%d", getMnc()) < 0) {
        RLOGE("Failed to allocate memory");
        ril_err = RIL_E_NO_MEMORY;
        goto on_exit;
    }

    if (getMncLength() == 2) {
        if (asprintf(&responseStr[17], "%03d%02d", getMcc(), getMnc()) < 0) {
            RLOGE("Failed to allocate memory");
            ril_err = RIL_E_NO_MEMORY;
            goto on_exit;
        }
    } else {
        if (asprintf(&responseStr[17], "%03d%03d", getMcc(), getMnc()) < 0) {
            RLOGE("Failed to allocate memory");
            ril_err = RIL_E_NO_MEMORY;
            goto on_exit;
        }
    }

    for (j = startfrom; j < numElements; j++) {
        if (!responseStr[i])
            goto on_exit;
    }

on_exit:
    free(registration);
    registration = NULL;
    RIL_onRequestComplete(t, ril_err, ril_err == RIL_E_SUCCESS ? responseStr : NULL,
        ril_err == RIL_E_SUCCESS ? numElements * sizeof(responseStr) : 0);
    at_response_free(p_response);
    if (responseStr) {
        for (j = 0; j < numElements; j++) {
            free(responseStr[j]);
            responseStr[j] = NULL;
        }

        free(responseStr);
        responseStr = NULL;
    }

    if (ril_err != RIL_E_SUCCESS)
        RLOGE("requestRegistrationState must never return an error when radio is on");
}

static void requestGetNeighboringCellIds(void* data, size_t datalen, RIL_Token t)
{
    (void)data;
    (void)datalen;

    RIL_NeighboringCell info[] = {
        { "2024", 90 },
        { "2025", 91 },
    };

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &info,
        sizeof(info) / sizeof(info[0]) * sizeof(RIL_NeighboringCell*));
}

static void requestSetNetowkAutoMode(void* data, size_t datalen, RIL_Token t)
{
    (void)data;
    (void)datalen;

    if (getSIMStatus() == SIM_ABSENT) {
        RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
        return;
    }

    ATResponse* p_response = NULL;
    RIL_Errno ril_err = RIL_E_SUCCESS;
    int err = -1;

    err = at_send_command("AT+COPS=0", &p_response);
    if (err != AT_ERROR_OK || !p_response || p_response->success != AT_OK) {
        RLOGE("Failure occurred in sending %s due to: %s", "AT+COPS=0", at_io_err_str(err));
        ril_err = RIL_E_GENERIC_FAILURE;
        goto on_exit;
    }

on_exit:
    RIL_onRequestComplete(t, ril_err, NULL, 0);
    at_response_free(p_response);
}

static void requestQueryBandMode(void* data, size_t datalen, RIL_Token t)
{
    int size = 5;
    int response[20] = { 0 };
    for (int i = 1; i <= size; i++) {
        response[i] = i - 1;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, (size + 1) * sizeof(int));
}

static void on_nitz_unsol_resp(const char* s)
{
    char *line = NULL, *p;
    int err;
    /* TI specific -- NITZ time */
    char* response;

    line = p = strdup(s);
    at_tok_start(&p);

    err = at_tok_nextstr(&p, &response);

    if (err != 0) {
        RLOGE("invalid NITZ line %s\n", s);
    } else {
        RIL_onUnsolicitedResponse(
            RIL_UNSOL_NITZ_TIME_RECEIVED,
            response, strlen(response) + 1);
    }

    free(line);
}

static void on_signal_strength_unsol_resp(const char* s)
{
    char *line = NULL, *p;
    int err;

    // Accept a response that is at least v6, and up to v12
    int minNumOfElements = sizeof(RIL_SignalStrength_v6) / sizeof(int);
    int maxNumOfElements = sizeof(RIL_SignalStrength_v12) / sizeof(int);
    int response[maxNumOfElements];
    memset(response, 0, sizeof(response));

    line = p = strdup(s);
    at_tok_start(&p);

    for (int count = 0; count < maxNumOfElements; count++) {
        err = at_tok_nextint(&p, &(response[count]));
        if (err < 0 && count < minNumOfElements) {
            RLOGE("Fail to parse response in %s", __func__);
            free(line);
            return;
        }
    }

    RIL_onUnsolicitedResponse(RIL_UNSOL_SIGNAL_STRENGTH,
        response, sizeof(response));
    free(line);
}

int mapNetworkRegistrationResponse(int in_response)
{
    int out_response = 0;

    switch (in_response) {
    case 0:
        out_response = RADIO_TECH_GPRS; /* GPRS */
        break;
    case 3:
        out_response = RADIO_TECH_EDGE; /* EDGE */
        break;
    case 2:
        out_response = RADIO_TECH_UMTS; /* TD */
        break;
    case 4:
        out_response = RADIO_TECH_HSDPA; /* HSDPA */
        break;
    case 5:
        out_response = RADIO_TECH_HSUPA; /* HSUPA */
        break;
    case 6:
        out_response = RADIO_TECH_HSPA; /* HSPA */
        break;
    case 15:
        out_response = RADIO_TECH_HSPAP; /* HSPA+ */
        break;
    case 7:
        out_response = RADIO_TECH_LTE; /* LTE */
        break;
    case 16:
        out_response = RADIO_TECH_LTE_CA; /* LTE_CA */
        break;
    case 11: // NR connected to a 5GCN
    case 12: // NG-RAN
    case 13: // E-UTRA-NR dual connectivity
        out_response = RADIO_TECH_NR; /* NR */
        break;
    default:
        out_response = RADIO_TECH_UNKNOWN; /* UNKNOWN */
        break;
    }

    return out_response;
}

int parseRegistrationState(char* str, int* type, int* items, int** response)
{
    int err;
    char *line = str, *p;
    int* resp = NULL;
    int skip;
    int commas;

    RLOGD("parseRegistrationState. Parsing: %s", str);

    err = at_tok_start(&line);
    if (err < 0) {
        RLOGE("Fail to parse line in %s", __func__);
        goto error;
    }

    /* Ok you have to be careful here
     * The solicited version of the CREG response is
     * +CREG: n, stat, [lac, cid]
     * and the unsolicited version is
     * +CREG: stat, [lac, cid]
     * The <n> parameter is basically "is unsolicited creg on?"
     * which it should always be
     *
     * Now we should normally get the solicited version here,
     * but the unsolicited version could have snuck in
     * so we have to handle both
     *
     * Also since the LAC and CID are only reported when registered,
     * we can have 1, 2, 3, or 4 arguments here
     *
     * finally, a +CGREG: answer may have a fifth value that corresponds
     * to the network type, as in;
     *
     *   +CGREG: n, stat [,lac, cid [,networkType]]
     */

    /* count number of commas */
    commas = 0;
    for (p = line; *p != '\0'; p++) {
        if (*p == ',')
            commas++;
    }

    resp = (int*)calloc(commas + 1, sizeof(int));
    if (!resp) {
        RLOGE("resp is null");
        goto error;
    }

    switch (commas) {
    case 0: /* +CREG: <stat> */
        err = at_tok_nextint(&line, &resp[0]);
        if (err < 0) {
            RLOGE("Fail to parse stat in %s", __func__);
            goto error;
        }
        break;

    case 1: /* +CREG: <n>, <stat> */
        err = at_tok_nextint(&line, &skip);
        if (err < 0) {
            RLOGE("Fail to parse integer in %s", __func__);
            goto error;
        }

        err = at_tok_nextint(&line, &resp[0]);
        if (err < 0) {
            RLOGE("Fail to parse stat in %s", __func__);
            goto error;
        }
        break;

    case 2: /* +CREG: <stat>, <lac>, <cid> */
        err = at_tok_nextint(&line, &resp[0]);
        if (err < 0) {
            RLOGE("Fail to parse stat in %s", __func__);
            goto error;
        }

        err = at_tok_nexthexint(&line, &resp[1]);
        if (err < 0) {
            RLOGE("Fail to parse lac in %s", __func__);
            goto error;
        }

        err = at_tok_nexthexint(&line, &resp[2]);
        if (err < 0) {
            RLOGE("Fail to parse cid in %s", __func__);
            goto error;
        }
        break;

    case 3: /* +CREG: <n>, <stat>, <lac>, <cid> */
        err = at_tok_nextint(&line, &skip);
        if (err < 0) {
            RLOGE("Fail to parse integer in %s", __func__);
            goto error;
        }

        err = at_tok_nextint(&line, &resp[0]);
        if (err < 0) {
            RLOGE("Fail to parse stat in %s", __func__);
            goto error;
        }

        err = at_tok_nexthexint(&line, &resp[1]);
        if (err < 0) {
            RLOGE("Fail to parse lac in %s", __func__);
            goto error;
        }

        err = at_tok_nexthexint(&line, &resp[2]);
        if (err < 0) {
            RLOGE("Fail to parse cid in %s", __func__);
            goto error;
        }
        break;

    /* special case for CGREG, there is a fourth parameter
     * that is the network type (unknown/gprs/edge/umts)
     */
    case 4: /* +CGREG: <n>, <stat>, <lac>, <cid>, <networkType> */
        err = at_tok_nextint(&line, &skip);
        if (err < 0) {
            RLOGE("Fail to parse integer in %s", __func__);
            goto error;
        }

        err = at_tok_nextint(&line, &resp[0]);
        if (err < 0) {
            RLOGE("Fail to parse stat in %s", __func__);
            goto error;
        }

        err = at_tok_nexthexint(&line, &resp[1]);
        if (err < 0) {
            RLOGE("Fail to parse lac in %s", __func__);
            goto error;
        }

        err = at_tok_nexthexint(&line, &resp[2]);
        if (err < 0) {
            RLOGE("Fail to parse cid in %s", __func__);
            goto error;
        }

        err = at_tok_nextint(&line, &resp[3]);
        if (err < 0) {
            RLOGE("Fail to parse networkType in %s", __func__);
            goto error;
        }
        break;

    default:
        goto error;
    }

    if (commas >= 2) {
        s_lac = resp[1];
        s_cid = resp[2];
    }

    if (response) {
        *response = resp;
    } else {
        free(resp);
        resp = NULL;
    }

    if (items)
        *items = commas + 1;

    if (type)
        *type = techFromModemType(TECH(getModemInfo()));

    return 0;

error:
    free(resp);
    return -1;
}

int is3gpp2(int radioTech)
{
    switch (radioTech) {
    case RADIO_TECH_IS95A:
    case RADIO_TECH_IS95B:
    case RADIO_TECH_1xRTT:
    case RADIO_TECH_EVDO_0:
    case RADIO_TECH_EVDO_A:
    case RADIO_TECH_EVDO_B:
    case RADIO_TECH_EHRPD:
        return 1;
    default:
        return 0;
    }
}

void on_request_network(int request, void* data, size_t datalen, RIL_Token t)
{
    switch (request) {
    case RIL_REQUEST_ENTER_NETWORK_DEPERSONALIZATION:
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
        break;
    case RIL_REQUEST_SIGNAL_STRENGTH:
        requestSignalStrength(data, datalen, t);
        break;
    case RIL_REQUEST_VOICE_REGISTRATION_STATE:
        requestVoiceRegistrationState(data, datalen, t);
        break;
    case RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE:
        requestQueryNetworkSelectionMode(data, datalen, t);
        break;
    case RIL_REQUEST_SET_NETWORK_SELECTION_AUTOMATIC:
        requestSetNetowkAutoMode(data, datalen, t);
        break;
    case RIL_REQUEST_SET_NETWORK_SELECTION_MANUAL:
        requestSetNetworlSelectionManual(data, datalen, t);
        break;
    case RIL_REQUEST_QUERY_AVAILABLE_NETWORKS:
        requestQueryAvailableNetworks(data, datalen, t);
        break;
    case RIL_REQUEST_SET_BAND_MODE:
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
        break;
    case RIL_REQUEST_QUERY_AVAILABLE_BAND_MODE:
        requestQueryBandMode(data, datalen, t);
        break;
    case RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE:
        requestSetPreferredNetworkType(data, datalen, t);
        break;
    case RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE:
        requestGetPreferredNetworkType(data, datalen, t);
        break;
    case RIL_REQUEST_GET_NEIGHBORING_CELL_IDS:
        requestGetNeighboringCellIds(data, datalen, t);
        break;
    case RIL_REQUEST_SET_LOCATION_UPDATES:
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
        break;
    case RIL_REQUEST_GET_CELL_INFO_LIST:
        requestGetCellInfoList(data, datalen, t);
        break;
    case RIL_REQUEST_SET_UNSOL_CELL_INFO_LIST_RATE:
        requestSetCellInfoListRate(data, datalen, t);
        break;
    case RIL_REQUEST_IMS_REG_STATE_CHANGE:
        requestImsRegStateChange(data, datalen, t);
        break;
    case RIL_REQUEST_IMS_REGISTRATION_STATE:
        requestImsRegState(data, datalen, t);
        break;
    case RIL_REQUEST_IMS_SET_SERVICE_STATUS:
        requestImsSetServiceStatus(data, datalen, t);
        break;
    default:
        RLOGE("Request not supported");
        RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
        break;
    }

    RLOGD("On request network end");
}

bool try_handle_unsol_net(const char* s)
{
    bool ret = false;
    char *line = NULL, *p;
    int err;

    RLOGD("unsol network string: %s", s);

    if (strStartsWith(s, "%CTZV:")) {
        RLOGI("Receive NITZ URC");
        on_nitz_unsol_resp(s);
        ret = true;
    } else if (strStartsWith(s, "+CREG:") || strStartsWith(s, "+CGREG:")) {
        RLOGI("Receive EPS network state change URC");
        RIL_onUnsolicitedResponse(
            RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED, NULL, 0);
        ret = true;
    }
#define CGFPCCFG "%CGFPCCFG:"
    else if (strStartsWith(s, CGFPCCFG)) {
        RLOGI("Receive physical channel configs URC");
        /* cuttlefish/goldfish specific */
        line = p = strdup(s);
        RLOGD("got CGFPCCFG line %s and %s\n", s, p);
        err = at_tok_start(&line);
        if (err) {
            RLOGE("invalid CGFPCCFG line %s and %s\n", s, p);
        }
#define kSize 5
        int configs[kSize];
        for (int i = 0; i < kSize && !err; ++i) {
            err = at_tok_nextint(&line, &(configs[i]));
            RLOGD("got i %d, val = %d", i, configs[i]);
        }

        if (err) {
            RLOGE("invalid CGFPCCFG line %s and %s\n", s, p);
        } else {
            int modem_tech = configs[2];
            configs[2] = techFromModemType(modem_tech);
            RIL_onUnsolicitedResponse(
                RIL_UNSOL_PHYSICAL_CHANNEL_CONFIGS,
                configs, kSize);
        }

        free(p);
        ret = true;
    } else if (strStartsWith(s, "+CSQ: ")) {
        RLOGI("Receive signal strength URC");
        on_signal_strength_unsol_resp(s);
        ret = true;
    } else if (strStartsWith(s, "+CIREGU")) {
        RLOGI("Receive ims_reg change URC");
        RIL_onUnsolicitedResponse(
            RIL_UNSOL_RESPONSE_IMS_NETWORK_STATE_CHANGED,
            NULL, 0);
        ret = true;
    } else {
        RLOGD("Can't match any unsol network handlers");
    }

    return ret;
}