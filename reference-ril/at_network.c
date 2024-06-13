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

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <sys/cdefs.h>
#include <telephony/librilutils.h>
#include <telephony/ril_log.h>
#include "atchannel.h"
#include "at_tok.h"
#include "misc.h"
#include "at_modem.h"
#include "at_network.h"
#include "at_ril.h"
#include "at_sim.h"

#define MAX_OPER_NAME_LENGTH (30)

static int net2modem[] = {
    MDM_GSM | MDM_WCDMA,                                 // 0  - GSM / WCDMA Pref
    MDM_GSM,                                             // 1  - GSM only
    MDM_WCDMA,                                           // 2  - WCDMA only
    MDM_GSM | MDM_WCDMA,                                 // 3  - GSM / WCDMA Auto
    MDM_CDMA | MDM_EVDO,                                 // 4  - CDMA / EvDo Auto
    MDM_CDMA,                                            // 5  - CDMA only
    MDM_EVDO,                                            // 6  - EvDo only
    MDM_GSM | MDM_WCDMA | MDM_CDMA | MDM_EVDO,           // 7  - GSM/WCDMA, CDMA, EvDo
    MDM_LTE | MDM_CDMA | MDM_EVDO,                       // 8  - LTE, CDMA and EvDo
    MDM_LTE | MDM_GSM | MDM_WCDMA,                       // 9  - LTE, GSM/WCDMA
    MDM_LTE | MDM_CDMA | MDM_EVDO | MDM_GSM | MDM_WCDMA, // 10 - LTE, CDMA, EvDo, GSM/WCDMA
    MDM_LTE,                                             // 11 - LTE only
    MDM_LTE | MDM_WCDMA,                                 // 12 - LTE and WCDMA
    MDM_TDSCDMA,                                         // 13 - TD-SCDMA only
    MDM_WCDMA | MDM_TDSCDMA,                             // 14 - TD-SCDMA and WCDMA
    MDM_LTE | MDM_TDSCDMA,                               // 15 - LTE and TD-SCDMA
    MDM_TDSCDMA | MDM_GSM,                               // 16 - TD-SCDMA and GSM
    MDM_LTE | MDM_TDSCDMA | MDM_GSM,                     // 17 - TD-SCDMA, GSM and LTE
    MDM_WCDMA | MDM_TDSCDMA | MDM_GSM,                   // 18 - TD-SCDMA, GSM and WCDMA
    MDM_LTE | MDM_WCDMA | MDM_TDSCDMA,                   // 19 - LTE, TD-SCDMA and WCDMA
    MDM_LTE | MDM_WCDMA | MDM_TDSCDMA | MDM_GSM,         // 20 - LTE, TD-SCDMA, GSM, and WCDMA
    MDM_EVDO | MDM_CDMA | MDM_WCDMA | MDM_TDSCDMA | MDM_GSM,            // 21 - TD-SCDMA, CDMA, EVDO, GSM and WCDMA
    MDM_LTE | MDM_TDSCDMA | MDM_CDMA | MDM_EVDO | MDM_WCDMA | MDM_GSM,  // 22 - LTE, TDCSDMA, CDMA, EVDO, GSM and WCDMA
    MDM_NR,                                                             // 23 - NR 5G only mode
    MDM_NR | MDM_LTE,                                                   // 24 - NR 5G, LTE
    MDM_NR | MDM_LTE | MDM_CDMA | MDM_EVDO,                             // 25 - NR 5G, LTE, CDMA and EvDo
    MDM_NR | MDM_LTE | MDM_WCDMA | MDM_GSM,                             // 26 - NR 5G, LTE, GSM and WCDMA
    MDM_NR | MDM_LTE | MDM_CDMA | MDM_EVDO | MDM_WCDMA | MDM_GSM,       // 27 - NR 5G, LTE, CDMA, EvDo, GSM and WCDMA
    MDM_NR | MDM_LTE | MDM_WCDMA,                                       // 28 - NR 5G, LTE and WCDMA
    MDM_NR | MDM_LTE | MDM_TDSCDMA,                                     // 29 - NR 5G, LTE and TDSCDMA
    MDM_NR | MDM_LTE | MDM_TDSCDMA | MDM_GSM,                           // 30 - NR 5G, LTE, TD-SCDMA and GSM
    MDM_NR | MDM_LTE | MDM_TDSCDMA | MDM_WCDMA,                         // 31 - NR 5G, LTE, TD-SCDMA, WCDMA
    MDM_NR | MDM_LTE | MDM_TDSCDMA | MDM_WCDMA | MDM_GSM,               // 32 - NR 5G, LTE, TD-SCDMA, GSM and WCDMA
    MDM_NR | MDM_LTE | MDM_TDSCDMA | MDM_CDMA | MDM_EVDO | MDM_WCDMA | MDM_GSM,  // 33 - NR 5G, LTE, TD-SCDMA, CDMA, EVDO, GSM and WCDMA
};

static int32_t net2pmask[] = {
    MDM_GSM | (MDM_WCDMA << 8),                          // 0  - GSM / WCDMA Pref
    MDM_GSM,                                             // 1  - GSM only
    MDM_WCDMA,                                           // 2  - WCDMA only
    MDM_GSM | MDM_WCDMA,                                 // 3  - GSM / WCDMA Auto
    MDM_CDMA | MDM_EVDO,                                 // 4  - CDMA / EvDo Auto
    MDM_CDMA,                                            // 5  - CDMA only
    MDM_EVDO,                                            // 6  - EvDo only
    MDM_GSM | MDM_WCDMA | MDM_CDMA | MDM_EVDO,           // 7  - GSM/WCDMA, CDMA, EvDo
    MDM_LTE | MDM_CDMA | MDM_EVDO,                       // 8  - LTE, CDMA and EvDo
    MDM_LTE | MDM_GSM | MDM_WCDMA,                       // 9  - LTE, GSM/WCDMA
    MDM_LTE | MDM_CDMA | MDM_EVDO | MDM_GSM | MDM_WCDMA, // 10 - LTE, CDMA, EvDo, GSM/WCDMA
    MDM_LTE,                                             // 11 - LTE only
    MDM_LTE | MDM_WCDMA,                                 // 12 - LTE and WCDMA
    MDM_TDSCDMA,                                         // 13 - TD-SCDMA only
    MDM_WCDMA | MDM_TDSCDMA,                             // 14 - TD-SCDMA and WCDMA
    MDM_LTE | MDM_TDSCDMA,                               // 15 - LTE and TD-SCDMA
    MDM_TDSCDMA | MDM_GSM,                               // 16 - TD-SCDMA and GSM
    MDM_LTE | MDM_TDSCDMA | MDM_GSM,                     // 17 - TD-SCDMA, GSM and LTE
    MDM_WCDMA | MDM_TDSCDMA | MDM_GSM,                   // 18 - TD-SCDMA, GSM and WCDMA
    MDM_LTE | MDM_WCDMA | MDM_TDSCDMA,                   // 19 - LTE, TD-SCDMA and WCDMA
    MDM_LTE | MDM_WCDMA | MDM_TDSCDMA | MDM_GSM,         // 20 - LTE, TD-SCDMA, GSM, and WCDMA
    MDM_EVDO | MDM_CDMA | MDM_WCDMA | MDM_TDSCDMA | MDM_GSM,            // 21 - TD-SCDMA, CDMA, EVDO, GSM and WCDMA
    MDM_LTE | MDM_TDSCDMA | MDM_CDMA | MDM_EVDO | MDM_WCDMA | MDM_GSM,  // 22 - LTE, TDCSDMA, CDMA, EVDO, GSM and WCDMA
    MDM_NR,                                                             // 23 - NR 5G only mode
    MDM_NR | MDM_LTE,                                                   // 24 - NR 5G, LTE
    MDM_NR | MDM_LTE | MDM_CDMA | MDM_EVDO,                             // 25 - NR 5G, LTE, CDMA and EvDo
    MDM_NR | MDM_LTE | MDM_WCDMA | MDM_GSM,                             // 26 - NR 5G, LTE, GSM and WCDMA
    MDM_NR | MDM_LTE | MDM_CDMA | MDM_EVDO | MDM_WCDMA | MDM_GSM,       // 27 - NR 5G, LTE, CDMA, EvDo, GSM and WCDMA
    MDM_NR | MDM_LTE | MDM_WCDMA,                                       // 28 - NR 5G, LTE and WCDMA
    MDM_NR | MDM_LTE | MDM_TDSCDMA,                                     // 29 - NR 5G, LTE and TDSCDMA
    MDM_NR | MDM_LTE | MDM_TDSCDMA | MDM_GSM,                           // 30 - NR 5G, LTE, TD-SCDMA and GSM
    MDM_NR | MDM_LTE | MDM_TDSCDMA | MDM_WCDMA,                         // 31 - NR 5G, LTE, TD-SCDMA, WCDMA
    MDM_NR | MDM_LTE | MDM_TDSCDMA | MDM_WCDMA | MDM_GSM,               // 32 - NR 5G, LTE, TD-SCDMA, GSM and WCDMA
    MDM_NR | MDM_LTE | MDM_TDSCDMA | MDM_CDMA | MDM_EVDO | MDM_WCDMA | MDM_GSM,  // 33 - NR 5G, LTE, TD-SCDMA, CDMA, EVDO, GSM and WCDMA
};

static int s_cell_info_rate_ms = INT_MAX;
static int s_lac = 0;
static int s_cid = 0;

static void requestQueryNetworkSelectionMode(void *data, size_t datalen, RIL_Token t)
{
    (void)data;
    (void)datalen;

    int err;
    ATResponse *p_response = NULL;
    int response = 0;
    char *line;

    err = at_send_command_singleline("AT+COPS?", "+COPS:", &p_response);

    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);

    if (err < 0) {
        goto error;
    }

    err = at_tok_nextint(&line, &response);

    if (err < 0) {
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(int));
    at_response_free(p_response);
    return;

error:
    at_response_free(p_response);
    RLOGE("requestQueryNetworkSelectionMode must never return error when radio is on");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestSignalStrength(void *data, size_t datalen, RIL_Token t)
{
    (void)data;
    (void)datalen;

    ATResponse *p_response = NULL;
    int err;
    char *line;
    int count = 0;
    // Accept a response that is at least v6, and up to v12
    int minNumOfElements=sizeof(RIL_SignalStrength_v6)/sizeof(int);
    int maxNumOfElements=sizeof(RIL_SignalStrength_v12)/sizeof(int);
    int response[maxNumOfElements];

    memset(response, 0, sizeof(response));
    err = at_send_command_singleline("AT+CSQ", "+CSQ:", &p_response);

    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        goto error;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;

    for (count = 0; count < maxNumOfElements; count ++) {
        err = at_tok_nextint(&line, &(response[count]));
        if (err < 0 && count < minNumOfElements) goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));
    at_response_free(p_response);
    return;

error:
    RLOGE("requestSignalStrength must never return an error when radio is on");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

/**
 * networkModePossible. Decides whether the network mode is appropriate for the
 * specified modem
 */
static int networkModePossible(ModemInfo *mdm, int nm)
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

static void requestSetPreferredNetworkType(void *data, size_t datalen, RIL_Token t)
{
    (void)datalen;

    ATResponse *p_response = NULL;
    char *cmd = NULL;
    int value = *(int *)data;
    int current, old;
    int err;
    int32_t preferred = net2pmask[value];

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
        asprintf(&cmd, "AT+CTEC=%d,\"%lx\"", current, preferred);
        RLOGD("Sending command: <%s>", cmd);
        err = at_send_command_singleline(cmd, "+CTEC:", &p_response);
        free(cmd);

        if (err || !p_response->success) {
            RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            return;
        }

        PREFERRED_NETWORK(getModemInfo()) = value;
        if (!strstr( p_response->p_intermediates->line, "DONE")) {
            int curr;
            int res = parse_technology_response(p_response->p_intermediates->line, &curr, NULL);
            switch (res) {
                case -1:        // Error or unable to parse
                    break;
                case 1:         // Only able to parse current
                case 0:         // Both current and preferred were parsed
                    setRadioTechnology(getModemInfo(), curr);
                    break;
            }
        }
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestGetPreferredNetworkType(void *data, size_t datalen, RIL_Token t)
{
    (void)data;
    (void)datalen;

    int32_t preferred;
    unsigned i;

    switch (query_ctec(getModemInfo(), NULL, &preferred)) {
        case -1:        // Error or unable to parse
        case 1:         // Only able to parse current
            RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            break;
        case 0:         // Both current and preferred were parsed
            for (i = 0 ; i < sizeof(net2pmask) / sizeof(int32_t) ; i ++) {
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
static void requestImsRegStateChange(void *data, size_t datalen, RIL_Token t)
{
    (void)datalen;

    int is_on = 0;
    char *cmd;
    ATResponse *p_response = NULL;
    int err = -1;

    if (data == NULL) {
        RLOGD("data is NULL");
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    is_on = ((int*)data)[0];
    RLOGD("set volte: is_on = %d\n", is_on);

    if (is_on != 0 && is_on != 1) {
        RLOGE("Invalid arguments in RIL");
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    asprintf(&cmd, "AT+CAVIMS=%d", is_on);
    err = at_send_command(cmd, &p_response);
    free(cmd);

    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }

    at_response_free(p_response);
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
static void requestImsSetServiceStatus(void *data, size_t datalen, RIL_Token t)
{
    (void)datalen;

    int ims_service = 0;
    char *cmd;
    ATResponse *p_response = NULL;
    int err = -1;

    if (data == NULL) {
        RLOGD("data is NULL");
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    ims_service = (*(int*)data);
    RLOGD("set ims_service : ims_service = %d\n", ims_service);
    if (ims_service != 1 && ims_service != 4 && ims_service != 5) {
        RLOGE("Invalid arguments in RIL");
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    asprintf(&cmd, "AT+CASIMS=%d", ims_service);
    err = at_send_command(cmd, &p_response);
    free(cmd);

    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }

    at_response_free(p_response);
}

static void requestSetNetworlSelectionManual(void *data, size_t datalen, RIL_Token t)
{
    (void)datalen;

    int err = -1;
    char cmd[64] = {0};
    ATResponse *p_response = NULL;
    RIL_NetworkOperator *operator = (RIL_NetworkOperator *)data;

    if (operator->act != UNKNOWN) {
        snprintf(cmd, sizeof(cmd), "AT+COPS=1,2,\"%s\"", operator->operatorNumeric);
    } else {
        snprintf(cmd, sizeof(cmd), "AT+COPS=1,2,\"%s\",%d",
            operator->operatorNumeric, operator->act);
    }

    err = at_send_command(cmd, &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    at_response_free(p_response);
    return;

error:
    if (p_response != NULL &&
        !strcmp(p_response->finalResponse, "+CME ERROR: 30")) {
            RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    }

    at_response_free(p_response);
}

void requestQueryAvailableNetworks(void *data, size_t datalen, RIL_Token t)
{
    (void)data;
    (void)datalen;

    ATResponse *p_response = NULL;
    int err = -1;
    char *line;
    int len;
    int i, j, k;
    int nplmns;
    int nplmns_valid;
    char **response;
    char **response_valid;
    char *str;
    char *item;
    char *result;

    err = at_send_command_singleline("AT+COPS=?", "+COPS:", &p_response);
    if (err < 0 || !p_response || !p_response->success || !p_response->p_intermediates) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        at_response_free(p_response);
        return;
    }

    /*
     * response is +COPS: (3,"CHINA MOBILE","CMCC","46000"),(3,"CHINA-UNICOM","UNICOM","46001"),
     */
    line = p_response->p_intermediates->line;
    len = strlen(line);

    for (i = 0, nplmns = 0, nplmns_valid = 0; i < len; i ++) {
        if (line[i] == ')') {
            nplmns ++;
            nplmns_valid ++;
        }
    }

    response = (char **)calloc(1, sizeof(char *) * nplmns * 4);
    if (!response) {
        RIL_onRequestComplete(t, RIL_E_NO_MEMORY, NULL, 0);
        at_response_free(p_response);
        return;
    }

    item = (char *)calloc(1, nplmns * sizeof(char) * 4 * MAX_OPER_NAME_LENGTH);
    if (!item) {
        RIL_onRequestComplete(t, RIL_E_NO_MEMORY, NULL, 0);
        free(response);
        at_response_free(p_response);
        return;
    }

    result = line;
    for (i = 0, j = 0; i < nplmns; i ++, j += 4) {
        char *next_item = strchr(result, '(');
        if (!next_item)
            break;

        result = next_item + 1;
        str = strchr(result, ',');
        if (!str)
            break;

        *str ++ = '\0';
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
                RLOGE("<stat> %d is an invalid value: %d", i, value);
                break;
        }

        result = strchr(str, ',');
        if (!result)
            break;

        *result ++ = '\0';
        response[j + 0] = &item[(j + 0) * MAX_OPER_NAME_LENGTH];
        strcpy(response[j + 0], str);

        str = strchr(result, ',');
        if (!str)
            break;

        *str ++ = '\0';
        response[j + 1] = &item[(j + 1) * MAX_OPER_NAME_LENGTH];
        strcpy(response[j + 1], result);

        result = strchr(str, ')');
        if (!result)
            break;

        *result ++ = '\0';
        response[j + 2] = &item[(j + 2) * MAX_OPER_NAME_LENGTH];
        strcpy(response[j + 2], str);

        len = strlen(response[j + 2]);
        if (len != 5 && len != 6) {
            RLOGE("The length of the numeric code is incorrect");
            RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
            free(response);
            free(item);
            at_response_free(p_response);
            return;
        }

        for (k = 0; k < j; k += 4) {
            if (0 == strncmp(response[j + 2], response[k + 2], MAX_OPER_NAME_LENGTH)) {
                response[j + 2] = "";
                nplmns_valid --;
                break;
            }
        }
    }

    response_valid = (char **)calloc(1, sizeof(char *) * nplmns_valid * 4);
    if (!response_valid) {
        RIL_onRequestComplete(t, RIL_E_NO_MEMORY, NULL, 0);
        free(response);
        free(item);
        at_response_free(p_response);
        return;
    }

    for (i = 0, k = 0; i < nplmns; i ++) {
        if (response[i * 4 + 2] && strlen(response[i * 4 + 2]) > 0) {
            response_valid[k + 0] = response[i * 4 + 0];
            response_valid[k + 1] = response[i * 4 + 1];
            response_valid[k + 2] = response[i * 4 + 2];
            response_valid[k + 3] = response[i * 4 + 3];
            k += 4;
        }
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, response_valid, sizeof(char *) * nplmns_valid * 4);

    free(response);
    free(response_valid);
    free(item);
    at_response_free(p_response);
}

static void requestSetCellInfoListRate(void *data, size_t datalen, RIL_Token t)
{
    (void)datalen;

    // For now we'll save the rate but no RIL_UNSOL_CELL_INFO_LIST messages
    // will be sent.
    assert(datalen == sizeof(int));
    s_cell_info_rate_ms = ((int *)data)[0];

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestGetCellInfoList(void *data, size_t datalen, RIL_Token t)
{
    (void)data;
    (void)datalen;

    uint64_t curTime = ril_nano_time();
    RIL_CellInfo_v12 ci[1] =
    {
        {   // ci[0]
            1,      // cellInfoType
            1,      // registered
            RIL_TIMESTAMP_TYPE_MODEM,
            curTime - 1000,         // Fake some time in the past
            {           // union CellInfo
                {       // RIL_CellInfoGsm gsm
                    {   // gsm.cellIdneityGsm
                        getMcc(),      // mcc
                        getMnc(),      // mnc
                        s_lac,      // lac
                        s_cid,      // cid
                        0,          // arfcn unknown
                        0x1,        // Base Station Identity Code set to arbitrarily 1
                    },
                    {   // gsm.signalStrengthGsm
                        10,         // signalStrength
                        0           // bitErrorRate
                        , INT_MAX   // timingAdvance invalid value
                    }
                }
            }
        }
    };

    RIL_onRequestComplete(t, RIL_E_SUCCESS, ci, sizeof(ci));
}

static void requestImsRegState(void *data, size_t datalen, RIL_Token t)
{
    (void)data;
    (void)datalen;

    ATResponse *p_response = NULL;
    int err;
    char *line;
    RIL_IMS_REGISTRATION_STATE_RESPONSE reply;

    err = at_send_command_singleline("AT+CIREG?", "+CIREG:", &p_response);
    if (err < 0 || !p_response->success) {
        goto error;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &reply.reg_state);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &reply.service_type);
    if (err < 0)
        goto error;

    err = at_send_command_singleline("AT+CNUM", "+CNUM:", &p_response);
    if (err < 0 || !p_response->success) {
        goto error;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextstr(&line, &reply.uri_response);

    if (err < 0)
        goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &reply, sizeof(reply));
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

#define REG_STATE_LEN 18
static void requestVoiceRegistrationState(void *data, size_t datalen, RIL_Token t)
{
    int err;
    int *registration;
    char **responseStr = NULL;
    ATResponse *p_response = NULL;
    const char *cmd;
    const char *prefix;
    char *line;
    int i = 0, j, numElements = 0;
    int count = 3;
    int type, startfrom;

    RLOGD("requestRegistrationState");
    cmd = "AT+CREG?";
    prefix = "+CREG:";
    numElements = REG_STATE_LEN;

    err = at_send_command_singleline(cmd, prefix, &p_response);

    if (err < 0 || !p_response->success) goto error;

    line = p_response->p_intermediates->line;

    if (parseRegistrationState(line, &type, &count, &registration)) goto error;

    responseStr = malloc(numElements * sizeof(char *));
    if (!responseStr) goto error;
    memset(responseStr, 0, numElements * sizeof(char *));

    /**
     * The first '4' bytes for both registration states remain the same.
     * But if the request is 'DATA_REGISTRATION_STATE',
     * the 5th and 6th byte(s) are optional.
     */
    if (is3gpp2(type) == 1) {
        RLOGD("registration state type: 3GPP2");
        // TODO: Query modem
        startfrom = 3;
        asprintf(&responseStr[3], "8");             // EvDo revA
        asprintf(&responseStr[4], "1");             // BSID
        asprintf(&responseStr[5], "123");           // Latitude
        asprintf(&responseStr[6], "222");           // Longitude
        asprintf(&responseStr[7], "0");             // CSS Indicator
        asprintf(&responseStr[8], "4");             // SID
        asprintf(&responseStr[9], "65535");         // NID
        asprintf(&responseStr[10], "0");            // Roaming indicator
        asprintf(&responseStr[11], "1");            // System is in PRL
        asprintf(&responseStr[12], "0");            // Default Roaming indicator
        asprintf(&responseStr[13], "0");            // Reason for denial
        asprintf(&responseStr[14], "0");            // Primary Scrambling Code of Current cell
    } else {                                        // type == RADIO_TECH_3GPP
        RLOGD("registration state type: 3GPP");
        startfrom = 0;

        if (count > 1) {
            asprintf(&responseStr[1], "%x", registration[1]);
        }

        if (count > 2) {
            asprintf(&responseStr[2], "%x", registration[2]);
        }

        if (count > 3) {
            asprintf(&responseStr[3], "%d", mapNetworkRegistrationResponse(registration[3]));
        }
    }

    asprintf(&responseStr[0], "%d", registration[0]);
    asprintf(&responseStr[15], "%d", getMcc());
    asprintf(&responseStr[16], "%d", getMnc());
    if (getMncLength() == 2) {
        asprintf(&responseStr[17], "%03d%02d", getMcc(), getMnc());
    } else {
        asprintf(&responseStr[17], "%03d%03d", getMcc(), getMnc());
    }

    for (j = startfrom; j < numElements; j ++) {
        if (!responseStr[i]) goto error;
    }

    free(registration);
    registration = NULL;
    RIL_onRequestComplete(t, RIL_E_SUCCESS, responseStr, numElements*sizeof(responseStr));

    for (j = 0; j < numElements; j ++) {
        free(responseStr[j]);
        responseStr[j] = NULL;
    }

    free(responseStr);
    responseStr = NULL;
    at_response_free(p_response);

    return;
error:
    if (responseStr) {
        for (j = 0; j < numElements; j ++) {
            free(responseStr[j]);
            responseStr[j] = NULL;
        }

        free(responseStr);
        responseStr = NULL;
    }

    RLOGE("requestRegistrationState must never return an error when radio is on");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestGetNeighboringCellIds(void *data, size_t datalen, RIL_Token t)
{
    (void)data;
    (void)datalen;

    RIL_CellInfo info;
    memset(&info, 0, sizeof(info));
    info.cellInfoType = RIL_CELL_INFO_TYPE_LTE;
    info.registered = 1;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &info, sizeof(info));
}

static void requestSetNetowkAutoMode(void *data, size_t datalen, RIL_Token t)
{
    if (getSIMStatus() == SIM_ABSENT) {
        RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
        return;
    }

    ATResponse *p_response = NULL;
    int err = at_send_command("AT+COPS=0", &p_response);
    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }

    at_response_free(p_response);
}

static void requestQueryBandMode(void *data, size_t datalen, RIL_Token t)
{
    int size = 5;
    int response[20] = {0};
    for (int i = 1; i <= size; i ++) {
        response[i] = i - 1;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, (size + 1) * sizeof(int));
}

static void on_nitz_unsol_resp(const char *s)
{
    char *line = NULL, *p;
    int err;
    /* TI specific -- NITZ time */
    char *response;

    line = p = strdup(s);
    at_tok_start(&p);

    err = at_tok_nextstr(&p, &response);

    if (err != 0) {
        RLOGE("invalid NITZ line %s\n", s);
    } else {
        RIL_onUnsolicitedResponse (
            RIL_UNSOL_NITZ_TIME_RECEIVED,
            response, strlen(response) + 1);
    }

    free(line);
}

static void on_signal_strength_unsol_resp(const char *s)
{
    char *line = NULL, *p;
    int err;

    // Accept a response that is at least v6, and up to v12
    int minNumOfElements=sizeof(RIL_SignalStrength_v6)/sizeof(int);
    int maxNumOfElements=sizeof(RIL_SignalStrength_v12)/sizeof(int);
    int response[maxNumOfElements];
    memset(response, 0, sizeof(response));

    line = p = strdup(s);
    at_tok_start(&p);

    for (int count = 0; count < maxNumOfElements; count++) {
        err = at_tok_nextint(&p, &(response[count]));
        if (err < 0 && count < minNumOfElements) {
            free(line);
            return;
        }
    }

    RIL_onUnsolicitedResponse(RIL_UNSOL_SIGNAL_STRENGTH,
        response, sizeof(response));
    free(line);
}

int mapNetworkRegistrationResponse(int in_response) {
    int out_response = 0;

    switch (in_response) {
        case 0:
            out_response = RADIO_TECH_GPRS;    /* GPRS */
            break;
        case 3:
            out_response = RADIO_TECH_EDGE;    /* EDGE */
            break;
        case 2:
            out_response = RADIO_TECH_UMTS;    /* TD */
            break;
        case 4:
            out_response = RADIO_TECH_HSDPA;   /* HSDPA */
            break;
        case 5:
            out_response = RADIO_TECH_HSUPA;   /* HSUPA */
            break;
        case 6:
            out_response = RADIO_TECH_HSPA;    /* HSPA */
            break;
        case 15:
            out_response = RADIO_TECH_HSPAP;   /* HSPA+ */
            break;
        case 7:
            out_response = RADIO_TECH_LTE;     /* LTE */
            break;
        case 16:
            out_response = RADIO_TECH_LTE_CA;  /* LTE_CA */
            break;
        case 11:        // NR connected to a 5GCN
        case 12:        // NG-RAN
        case 13:        // E-UTRA-NR dual connectivity
            out_response = RADIO_TECH_NR;      /* NR */
            break;
        default:
            out_response = RADIO_TECH_UNKNOWN; /* UNKNOWN */
            break;
    }

    return out_response;
}

int parseRegistrationState(char *str, int *type, int *items, int **response)
{
    int err;
    char *line = str, *p;
    int *resp = NULL;
    int skip;
    int commas;

    RLOGD("parseRegistrationState. Parsing: %s", str);

    err = at_tok_start(&line);
    if (err < 0) goto error;

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
    for (p = line; *p != '\0'; p ++) {
        if (*p == ',')
            commas ++;
    }

    resp = (int *)calloc(commas + 1, sizeof(int));
    if (!resp)
        goto error;

    switch (commas) {
        case 0:         /* +CREG: <stat> */
            err = at_tok_nextint(&line, &resp[0]);
            if (err < 0) goto error;
            break;

        case 1:         /* +CREG: <n>, <stat> */
            err = at_tok_nextint(&line, &skip);
            if (err < 0) goto error;
            err = at_tok_nextint(&line, &resp[0]);
            if (err < 0) goto error;
            break;

        case 2:         /* +CREG: <stat>, <lac>, <cid> */
            err = at_tok_nextint(&line, &resp[0]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &resp[1]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &resp[2]);
            if (err < 0) goto error;
            break;

        case 3:         /* +CREG: <n>, <stat>, <lac>, <cid> */
            err = at_tok_nextint(&line, &skip);
            if (err < 0) goto error;
            err = at_tok_nextint(&line, &resp[0]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &resp[1]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &resp[2]);
            if (err < 0) goto error;
            break;

        /* special case for CGREG, there is a fourth parameter
         * that is the network type (unknown/gprs/edge/umts)
         */
        case 4:         /* +CGREG: <n>, <stat>, <lac>, <cid>, <networkType> */
            err = at_tok_nextint(&line, &skip);
            if (err < 0) goto error;
            err = at_tok_nextint(&line, &resp[0]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &resp[1]);
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &resp[2]);
            if (err < 0) goto error;
            err = at_tok_nextint(&line, &resp[3]);
            if (err < 0) goto error;
            break;

        default:
            goto error;
    }

    if (commas >= 2) {
        s_lac = resp[1];
        s_cid = resp[2];
    }

    if (response)
        *response = resp;

    if (items)
        *items = commas + 1;

    if (type)
        *type = techFromModemType(TECH(getModemInfo()));

    return 0;

error:
    free(resp);
    return -1;
}

int is3gpp2(int radioTech) {
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

void on_request_network(int request, void *data, size_t datalen, RIL_Token t)
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

bool try_handle_unsol_net(const char *s)
{
    bool ret = false;
    char *line = NULL, *p;
    int err;

    if (strStartsWith(s, "%CTZV:")) {
        on_nitz_unsol_resp(s);
        ret = true;
    } else if (strStartsWith(s, "+CREG:") || strStartsWith(s, "+CGREG:")) {
        RIL_onUnsolicitedResponse (
            RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED, NULL, 0);
#ifdef WORKAROUND_FAKE_CGEV
        RIL_requestTimedCallback (onDataCallListChanged, NULL, NULL);
#endif /* WORKAROUND_FAKE_CGEV */
        ret = true;
    }
#define  CGFPCCFG "%CGFPCCFG:"
    else if (strStartsWith(s, CGFPCCFG)) {
        /* cuttlefish/goldfish specific */
        line = p = strdup(s);
        RLOGD("got CGFPCCFG line %s and %s\n", s, p);
        err = at_tok_start(&line);
        if (err) {
            RLOGE("invalid CGFPCCFG line %s and %s\n", s, p);
        }
#define kSize 5
        int configs[kSize];
        for (int i = 0; i < kSize && !err; ++ i) {
            err = at_tok_nextint(&line, &(configs[i]));
            RLOGD("got i %d, val = %d", i, configs[i]);
        }

        if (err) {
            RLOGE("invalid CGFPCCFG line %s and %s\n", s, p);
        } else {
            int modem_tech = configs[2];
            configs[2] = techFromModemType(modem_tech);
            RIL_onUnsolicitedResponse (
                RIL_UNSOL_PHYSICAL_CHANNEL_CONFIGS,
                configs, kSize);
        }

        free(p);
        ret = true;
    } else if (strStartsWith(s, "+WPRL: ")) {
        int version = -1;
        line = p = strdup(s);
        if (!line) {
            RLOGE("+WPRL: Unable to allocate memory");
            return true;
        }

        if (at_tok_start(&p) < 0) {
            RLOGE("invalid +WPRL response: %s", s);
            free(line);
            return true;
        }

        if (at_tok_nextint(&p, &version) < 0) {
            RLOGE("invalid +WPRL response: %s", s);
            free(line);
            return true;
        }

        free(line);
        RIL_onUnsolicitedResponse(RIL_UNSOL_CDMA_PRL_CHANGED, &version, sizeof(version));
        ret = true;
    } else if (strStartsWith(s, "+CSQ: ")) {
        on_signal_strength_unsol_resp(s);
        ret = true;
    }

    return ret;
}