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

#define LOG_TAG "AT_MODEM"
#define NDEBUG 1

#include <assert.h>
#include <stdio.h>
#include <sys/cdefs.h>

#include <telephony/librilutils.h>
#include <telephony/ril_log.h>

#include "at_modem.h"
#include "at_ril.h"
#include "at_sim.h"
#include "at_tok.h"
#include "atchannel.h"
#include "misc.h"

#define GSM (RAF_GSM | RAF_GPRS | RAF_EDGE)
#define CDMA (RAF_IS95A | RAF_IS95B | RAF_1xRTT)
#define EVDO (RAF_EVDO_0 | RAF_EVDO_A | RAF_EVDO_B | RAF_EHRPD)
#define WCDMA (RAF_HSUPA | RAF_HSDPA | RAF_HSPA | RAF_HSPAP | RAF_UMTS)
#define LTE (RAF_LTE | RAF_LTE_CA)
#define NR (RAF_NR)

static ModemInfo* sMdmInfo;
static int s_modem_enabled = 0;

static void requestRadioPower(void* data, size_t datalen, RIL_Token t)
{
    int onOff;
    int err;
    ATResponse* p_response = NULL;

    if (data == NULL) {
        RLOGE("requestRadioPower data is null");
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    assert(datalen >= sizeof(int*));
    onOff = ((int*)data)[0];

    if (onOff == 0 && getRadioState() != RADIO_STATE_OFF) {
        err = at_send_command("AT+CFUN=0", &p_response);
        if (err != AT_ERROR_OK || !p_response || p_response->success != AT_OK) {
            RLOGE("Failure occurred in sending %s due to: %s", "AT+CFUN=0", at_io_err_str(err));
            goto error;
        }
        setRadioState(RADIO_STATE_OFF);
    } else if (onOff > 0 && getRadioState() == RADIO_STATE_OFF) {
        err = at_send_command("AT+CFUN=1", &p_response);
        if (err != AT_ERROR_OK || !p_response || p_response->success != AT_OK) {
            RLOGE("Failure occurred in sending %s due to: %s", "AT+CFUN=1", at_io_err_str(err));
            // Some stacks return an error when there is no SIM,
            // but they really turn the RF portion on
            // So, if we get an error, let's check to see if it
            // turned on anyway

            if (isRadioOn() != 1) {
                goto error;
            }
        }

        setRadioState(RADIO_STATE_ON);
    }

    at_response_free(p_response);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    return;

error:
    at_response_free(p_response);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestBaseBandVersion(void* data, size_t datalen, RIL_Token t)
{
    (void)data;
    (void)datalen;

    ATResponse* p_response = NULL;
    RIL_Errno ril_err = RIL_E_SUCCESS;
    int err = -1;
    char* line = NULL;
    char* responseStr = NULL;

    err = at_send_command_singleline("AT+CGMR", "+CGMR:", &p_response);
    if (err != AT_ERROR_OK || !p_response || p_response->success != AT_OK) {
        RLOGE("Failure occurred in sending %s due to: %s", "AT+CGMR", at_io_err_str(err));
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

    err = at_tok_nextstr(&line, &responseStr);
    if (err < 0) {
        RLOGE("Fail to parse base band version in %s", __func__);
        ril_err = RIL_E_GENERIC_FAILURE;
        goto on_exit;
    }

on_exit:
    RIL_onRequestComplete(t, ril_err, ril_err == RIL_E_SUCCESS ? responseStr : NULL,
        ril_err == RIL_E_SUCCESS ? sizeof(responseStr) : 0);
    at_response_free(p_response);
}

static void requestDeviceIdentity(void* data, size_t datalen, RIL_Token t)
{
    (void)data;
    (void)datalen;

    int err;
    char* responseStr[4];
    ATResponse* p_response = NULL;
    int count = 4;

    // Fixed values. TODO: Query modem
    responseStr[0] = "358240051111110";
    responseStr[1] = "";
    responseStr[2] = "77777777";
    responseStr[3] = ""; // default empty for non-CDMA

    err = at_send_command_numeric("AT+CGSN", &p_response);
    if (err != AT_ERROR_OK || !p_response || p_response->success != AT_OK) {
        RLOGE("Failure occurred in sending %s due to: %s", "AT+CGSN", at_io_err_str(err));
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    } else {
        if (TECH_BIT(sMdmInfo) == MDM_CDMA) {
            responseStr[3] = p_response->p_intermediates->line;
        } else {
            responseStr[0] = p_response->p_intermediates->line;
        }
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, responseStr, count * sizeof(char*));
    at_response_free(p_response);
}

static void unsolicitedRingBackTone(const char* s)
{
    char *line, *p;
    int cid, action, type;

    line = p = strdup(s);
    if (!line) {
        RLOGE("unsolicitedRingBackTone line is null");
        return;
    }

    if (at_tok_start(&p) < 0) {
        RLOGE("Fail to parse line in %s", __func__);
        free(line);
        return;
    }

    if (at_tok_nextint(&p, &cid) < 0) {
        RLOGE("Fail to parse cid in %s", __func__);
        free(line);
        return;
    }

    if (at_tok_nextint(&p, &action) < 0) {
        RLOGE("Fail to parse action in %s", __func__);
        free(line);
        return;
    }

    if (at_tok_nextint(&p, &type) < 0) {
        RLOGE("Fail to parse tyoe in %s", __func__);
        free(line);
        return;
    }

    RLOGD("On Ringback tone URC, cid: %d, action: %s, type: %s", cid,
        action == 1 ? "START" : "STOP", type == 1 ? "RINGBACK" : "CALL HOLDING");
    RIL_onUnsolicitedResponse(RIL_UNSOL_RINGBACK_TONE, &action, sizeof(int));
    free(line);
}

static void requestScreenState(void* data, size_t datalen, RIL_Token t)
{
    (void)datalen;

    ATResponse* p_response = NULL;
    RIL_Errno ril_err = RIL_E_SUCCESS;
    int err = -1;
    int status;

    if (data == NULL) {
        RLOGE("requestScreenState data is null");
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    status = *((int*)data);

    if (!status) {
        /* Suspend */
        err = at_send_command("AT+CEREG=1", &p_response);
        if (err != AT_ERROR_OK || !p_response || p_response->success != AT_OK) {
            RLOGE("Failure occurred in sending %s due to: %s", "AT+CEREG=1", at_io_err_str(err));
            ril_err = RIL_E_GENERIC_FAILURE;
            goto on_exit;
        }

        err = at_send_command("AT+CREG=1", &p_response);
        if (err != AT_ERROR_OK || !p_response || p_response->success != AT_OK) {
            RLOGE("Failure occurred in sending %s due to: %s", "AT+CREG=1", at_io_err_str(err));
            ril_err = RIL_E_GENERIC_FAILURE;
            goto on_exit;
        }

        err = at_send_command("AT+CGREG=1", &p_response);
        if (err != AT_ERROR_OK || !p_response || p_response->success != AT_OK) {
            RLOGE("Failure occurred in sending %s due to: %s", "AT+CGREG=1", at_io_err_str(err));
            ril_err = RIL_E_GENERIC_FAILURE;
            goto on_exit;
        }
    } else {
        /* Resume */
        err = at_send_command("AT+CEREG=2", &p_response);
        if (err != AT_ERROR_OK || !p_response || p_response->success != AT_OK) {
            RLOGE("Failure occurred in sending %s due to: %s", "AT+CEREG=2", at_io_err_str(err));
            ril_err = RIL_E_GENERIC_FAILURE;
            goto on_exit;
        }

        err = at_send_command("AT+CREG=2", &p_response);
        if (err != AT_ERROR_OK || !p_response || p_response->success != AT_OK) {
            RLOGE("Failure occurred in sending %s due to: %s", "AT+CREG=2", at_io_err_str(err));
            ril_err = RIL_E_GENERIC_FAILURE;
            goto on_exit;
        }

        err = at_send_command("AT+CGREG=2", &p_response);
        if (err != AT_ERROR_OK || !p_response || p_response->success != AT_OK) {
            RLOGE("Failure occurred in sending %s due to: %s", "AT+CGREG=2", at_io_err_str(err));
            ril_err = RIL_E_GENERIC_FAILURE;
            goto on_exit;
        }
    }

on_exit:
    RIL_onRequestComplete(t, ril_err, NULL, 0);
    at_response_free(p_response);
}

static void requestGetModemStatus(void* data, size_t datalen, RIL_Token t)
{
    (void)data;
    (void)datalen;

    int modemState = s_modem_enabled;
    RLOGI("response RIL_REQUEST_GET_MODEM_STATUS, status is [%d]", modemState);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &modemState, sizeof(int));
    return;
}

static uint64_t s_last_activity_info_query = 0;

static void requestGetActivityInfo(void* data, size_t datalen, RIL_Token t)
{
    (void)data;
    (void)datalen;

    uint64_t curTime = ril_nano_time();
    RIL_ActivityStatsInfo stats = {
        0, // sleep_mode_time_ms
        ((curTime - s_last_activity_info_query) / 1000000) - 1, // idle_mode_time_ms
        { 0, 0, 0, 0, 0 }, // tx_mode_time_ms
        0 // rx_mode_time_ms
    };
    s_last_activity_info_query = curTime;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &stats, sizeof(stats));
}

static void requestGetIMEI(void* data, size_t datalen, RIL_Token t)
{
    (void)data;
    (void)datalen;

    ATResponse* p_response = NULL;
    RIL_Errno ril_err = RIL_E_SUCCESS;
    int err = -1;

    err = at_send_command_numeric("AT+CGSN", &p_response);

    if (err != AT_ERROR_OK || !p_response || p_response->success != AT_OK) {
        RLOGE("Failure occurred in sending %s due to: %s", "AT+CGSN", at_io_err_str(err));
        ril_err = RIL_E_GENERIC_FAILURE;
        goto on_exit;
    }

on_exit:
    RIL_onRequestComplete(t, ril_err, ril_err == RIL_E_SUCCESS ? p_response->p_intermediates->line : NULL,
        ril_err == RIL_E_SUCCESS ? sizeof(char*) : 0);
    at_response_free(p_response);
}

static void requestGetIMEISV(void* data, size_t datalen, RIL_Token t)
{
    (void)data;
    (void)datalen;

    ATResponse* p_response = NULL;
    int err = at_send_command_numeric("AT+CGSN=2", &p_response);

    if (err < 0 || p_response->success == 0) {
        RLOGE("Failure occurred in sending %s due to: %s", "AT+CGSN=2", at_io_err_str(err));
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS,
            p_response->p_intermediates->line, sizeof(char*));
    }

    at_response_free(p_response);
}

static void requestOemHookStrings(void* data, size_t datalen, RIL_Token t)
{
    int i;
    int num_strings;
    const char** cur;
    ATResponse* p_response;
    char** responseStr = NULL;

    RLOGD("got OEM_HOOK_STRINGS: 0x%8p %lu", data, (long)datalen);

    num_strings = datalen / sizeof(char*);
    responseStr = malloc(num_strings * sizeof(char*));
    if (responseStr == NULL) {
        RLOGE("Failed to allocate memory");
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    memset(responseStr, 0, num_strings * sizeof(char*));
    for (i = 0, cur = (const char**)data;
         i < num_strings; cur++, i++) {
        p_response = NULL;
        at_send_command(*cur, &p_response);

        if (p_response && p_response->finalResponse) {
            if (asprintf(&responseStr[i], "%s", p_response->finalResponse) < 0) {
                RLOGE("Failed to allocate memory");
            }
        } else {
            if (asprintf(&responseStr[i], "%s", "ERROR") < 0) {
                RLOGE("Failed to allocate memory");
            }
        }

        at_response_free(p_response);
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, responseStr, num_strings * sizeof(responseStr));
    for (i = 0; i < num_strings; i++) {
        if (responseStr[i]) {
            free(responseStr[i]);
            responseStr[i] = NULL;
        }
    }
    free(responseStr);
    responseStr = NULL;
}

static void requestEnableModem(void* data, size_t datalen, RIL_Token t)
{
    (void)datalen;

    int err;
    ATResponse* p_response = NULL;

    if (data == NULL) {
        RLOGE("requestEnableModem data is null");
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    s_modem_enabled = *(int*)data;
    if (s_modem_enabled == 0) {
        err = at_send_command("AT+CFUN=0", &p_response);
        if (err != AT_ERROR_OK || !p_response || p_response->success != AT_OK) {
            RLOGE("Failure occurred in sending %s due to: %s", "AT+CFUN=0", at_io_err_str(err));
            at_response_free(p_response);
            RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        } else {
            setRadioState(RADIO_STATE_UNAVAILABLE);
        }
    } else if (s_modem_enabled == 1) {
        setRadioState(RADIO_STATE_OFF);
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

ModemInfo* getModemInfo(void)
{
    return sMdmInfo;
}

void initModem(void)
{
    sMdmInfo = calloc(1, sizeof(ModemInfo));

    if (!sMdmInfo) {
        RLOGE("Unable to alloc memory for ModemInfo");
    }
}

// TODO: Use all radio types
int techFromModemType(int mdmtype)
{
    int ret = -1;
    switch (mdmtype) {
    case MDM_CDMA:
        ret = RADIO_TECH_1xRTT;
        break;
    case MDM_EVDO:
        ret = RADIO_TECH_EVDO_A;
        break;
    case MDM_GSM:
        ret = RADIO_TECH_GPRS;
        break;
    case MDM_WCDMA:
        ret = RADIO_TECH_HSPA;
        break;
    case MDM_LTE:
        ret = RADIO_TECH_LTE;
    case MDM_NR:
        ret = RADIO_TECH_NR;
        break;
    }

    return ret;
}

void setRadioTechnology(ModemInfo* mdm, int newtech)
{
    RLOGD("setRadioTechnology(%d)", newtech);

    int oldtech = TECH(mdm);

    if (newtech != oldtech) {
        RLOGD("Tech change (%d => %d)", oldtech, newtech);
        TECH(mdm) = newtech;
        if (techFromModemType(newtech) != techFromModemType(oldtech)) {
            int tech = techFromModemType(TECH(sMdmInfo));
            if (tech > 0) {
                RIL_onUnsolicitedResponse(RIL_UNSOL_VOICE_RADIO_TECH_CHANGED,
                    &tech, sizeof(tech));
            }
        }
    }
}

/**
 * Parse the response generated by a +CTEC AT command
 * The values read from the response are stored in current and preferred.
 * Both current and preferred may be null. The corresponding value is ignored in that case.
 *
 * @return: -1 if some error occurs (or if the modem doesn't understand the +CTEC command)
 *          1 if the response includes the current technology only
 *          0 if the response includes both current technology and preferred mode
 */
int parse_technology_response(const char* response, int* current, int32_t* preferred)
{
    int err;
    char *line, *p;
    int ct;
    int pt = 0;

    line = p = strdup(response);
    RLOGD("Response: %s", line);
    err = at_tok_start(&p);
    if (err || !at_tok_hasmore(&p)) {
        RLOGE("err: %d. p: %s", err, p);
        free(line);
        return -1;
    }

    err = at_tok_nextint(&p, &ct);
    if (err) {
        RLOGE("Fail to parse ct");
        free(line);
        return -1;
    }

    if (current)
        *current = ct;
    RLOGD("line remaining after int: %s", p);

    err = at_tok_nexthexint(&p, &pt);
    if (err) {
        RLOGE("Fail to parse pt");
        free(line);
        return 1;
    }

    if (preferred) {
        *preferred = pt;
    }

    free(line);

    return 0;
}

/**
 * query_ctec. Send the +CTEC AT command to the modem to query the current
 * and preferred modes. It leaves values in the addresses pointed to by
 * current and preferred. If any of those pointers are NULL, the corresponding value
 * is ignored, but the return value will still reflect if retrieving and parsing of the
 * values succeeded.
 *
 * @mdm Currently unused
 * @current A pointer to store the current mode returned by the modem. May be null.
 * @preferred A pointer to store the preferred mode returned by the modem. May be null.
 * @return -1 on error (or failure to parse)
 *          1 if only the current mode was returned by modem (or failed to parse preferred)
 *          0 if both current and preferred were returned correctly
 */
int query_ctec(ModemInfo* mdm, int* current, int32_t* preferred)
{
    (void)mdm;

    ATResponse* p_response = NULL;
    int err;
    int res;

    RLOGD("query_ctec. current: %p, preferred: %p", current, preferred);
    err = at_send_command_singleline("AT+CTEC?", "+CTEC:", &p_response);
    if (!err && p_response && p_response->success) {
        res = parse_technology_response(p_response->p_intermediates->line, current, preferred);
        at_response_free(p_response);
        return res;
    }

    RLOGE("Error executing command: %d. response: %p. status: %d", err, p_response, p_response ? p_response->success : -1);
    at_response_free(p_response);
    return -1;
}

/* returns 1 if on, 0 if off, and -1 on error */
int isRadioOn(void)
{
    ATResponse* p_response = NULL;
    RIL_Errno ril_err = RIL_E_SUCCESS;
    int err = -1;
    char* line = NULL;
    char ret = 0;

    err = at_send_command_singleline("AT+CFUN?", "+CFUN:", &p_response);

    if (err != AT_ERROR_OK || !p_response || p_response->success == 0) {
        RLOGE("Failure occurred in sending %s due to: %s", "AT+CFUN?", at_io_err_str(err));
        ril_err = RIL_E_GENERIC_FAILURE;
        // assume radio is off
        goto on_exit;
    }

    line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) {
        RLOGE("Fail to parse line in %s", __func__);
        ril_err = RIL_E_GENERIC_FAILURE;
        goto on_exit;
    }

    err = at_tok_nextbool(&line, &ret);
    if (err < 0) {
        RLOGE("Fail to parse ret in %s", __func__);
        ril_err = RIL_E_GENERIC_FAILURE;
        goto on_exit;
    }

on_exit:
    at_response_free(p_response);
    return ril_err == RIL_E_SUCCESS ? ret : -1;
}

int isModemEnable(void)
{
    return s_modem_enabled;
}

void on_request_modem(int request, void* data, size_t datalen, RIL_Token t)
{
    switch (request) {
    case RIL_REQUEST_RADIO_POWER:
        requestRadioPower(data, datalen, t);
        break;
    case RIL_REQUEST_GET_IMEI:
        requestGetIMEI(data, datalen, t);
        break;
    case RIL_REQUEST_GET_IMEISV:
        requestGetIMEISV(data, datalen, t);
        break;
    case RIL_REQUEST_BASEBAND_VERSION:
        requestBaseBandVersion(data, datalen, t);
        break;
    case RIL_REQUEST_OEM_HOOK_RAW:
        // echo back data
        RIL_onRequestComplete(t, RIL_E_SUCCESS, data, datalen);
        break;
    case RIL_REQUEST_OEM_HOOK_STRINGS:
        requestOemHookStrings(data, datalen, t);
        break;
    case RIL_REQUEST_SCREEN_STATE:
        requestScreenState(data, datalen, t);
        break;
    case RIL_REQUEST_DEVICE_IDENTITY:
        requestDeviceIdentity(data, datalen, t);
        break;
    case RIL_REQUEST_GET_ACTIVITY_INFO:
        requestGetActivityInfo(data, datalen, t);
        break;
    case RIL_REQUEST_ENABLE_MODEM:
        requestEnableModem(data, datalen, t);
        break;
    case RIL_REQUEST_GET_MODEM_STATUS:
        requestGetModemStatus(data, datalen, t);
        break;
    default:
        RLOGE("Request not supported");
        RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
        break;
    }

    RLOGD("On request modem end");
}

bool try_handle_unsol_modem(const char* s)
{
    bool ret = false;

    RLOGD("unsol modem string: %s", s);

    if (strStartsWith(s, "+CTEC: ")) {
        RLOGI("Receive technology URC");
        int tech, mask;
        switch (parse_technology_response(s, &tech, NULL)) {
        case -1: // no argument could be parsed.
            RLOGE("invalid CTEC line %s\n", s);
            break;
        case 1: // current mode correctly parsed
        case 0: // preferred mode correctly parsed
            mask = 1 << tech;
            if (mask != MDM_GSM && mask != MDM_CDMA && mask != MDM_WCDMA && mask != MDM_LTE) {
                RLOGE("Unknown technology %d\n", tech);
            } else {
                setRadioTechnology(sMdmInfo, tech);
            }
            break;
        }

        ret = true;
    } else if (strStartsWith(s, "^MRINGTONE: ")) {
        RLOGI("Receive ring tone URC");
        unsolicitedRingBackTone(s);
        ret = true;
    } else if (strStartsWith(s, "+CFUN: 0")) {
        RLOGI("Receive radio off URC");
        setRadioState(RADIO_STATE_OFF);
        ret = true;
    } else {
        RLOGD("Can't match any unsol modem handlers");
    }

    return ret;
}