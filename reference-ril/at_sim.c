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

#include <stdio.h>
#include <sys/cdefs.h>
#include <telephony/ril_log.h>
#include "atchannel.h"
#include "at_tok.h"
#include "misc.h"
#include "at_modem.h"
#include "at_network.h"
#include "at_ril.h"
#include "at_sim.h"

// CDMA Subscription Source
#define SSOURCE(mdminfo) ((mdminfo)->subscription_source)

static const struct timeval TIMEVAL_SIMPOLL = {1,0};
static int areUiccApplicationsEnabled = true;

// STK
static bool s_stkServiceRunning = false;
static char *s_stkUnsolResponse = NULL;
extern uint8_t *convertHexStringToBytes(void *response, size_t responseLen);

static int s_mcc = 0;
static int s_mnc = 0;
static int s_mncLength = 2;

typedef enum {
    STK_UNSOL_EVENT_UNKNOWN,
    STK_UNSOL_EVENT_NOTIFY,
    STK_UNSOL_PROACTIVE_CMD,
} StkUnsolEvent;

typedef enum {
    STK_RUN_AT              = 0x34,
    STK_SEND_DTMF           = 0x14,
    STK_SEND_SMS            = 0x13,
    STK_SEND_SS             = 0x11,
    STK_SEND_USSD           = 0x12,
    STK_PLAY_TONE           = 0x20,
    STK_OPEN_CHANNEL        = 0x40,
    STK_CLOSE_CHANNEL       = 0x41,
    STK_RECEIVE_DATA        = 0x42,
    STK_SEND_DATA           = 0x43,
    STK_GET_CHANNEL_STATUS  = 0x44,
    STK_REFRESH             = 0x01,
} StkCmdType;

static int parseSimResponseLine(char* line, RIL_SIM_IO_Response* response)
{
    int err;

    err = at_tok_start(&line);
    if (err < 0) return err;
    err = at_tok_nextint(&line, &response->sw1);
    if (err < 0) return err;
    err = at_tok_nextint(&line, &response->sw2);
    if (err < 0) return err;

    if (at_tok_hasmore(&line)) {
        err = at_tok_nextstr(&line, &response->simResponse);
        if (err < 0) return err;
    }

    return 0;
}

/** do post- SIM ready initialization */
static void onSIMReady(void)
{
    int err = at_send_command_singleline("AT+CSMS=1", "+CSMS:", NULL);
    if (err < 0) {
        return;
    }
    /*
     * Always send SMS messages directly to the TE
     *
     * mode = 1 // discard when link is reserved (link should never be
     *             reserved)
     * mt = 2   // most messages routed to TE
     * bm = 2   // new cell BM's routed to TE
     * ds = 1   // Status reports routed to TE
     * bfr = 1  // flush buffer
     */
    at_send_command("AT+CNMI=1,2,2,1,1", NULL);
}

static void requestOperator(void *data, size_t datalen, RIL_Token t)
{
    (void)data;
    (void)datalen;

    int err;
    int i;
    int skip;
    ATLine *p_cur;
    char *response[3];

    memset(response, 0, sizeof(response));
    ATResponse *p_response = NULL;

    err = at_send_command_multiline(
        "AT+COPS=3,0;+COPS?;+COPS=3,1;+COPS?;+COPS=3,2;+COPS?",
        "+COPS:", &p_response);

    /* we expect 3 lines here:
     * +COPS: 0,0,"T - Mobile"
     * +COPS: 0,1,"TMO"
     * +COPS: 0,2,"310170"
     */

    if (err != 0) goto error;

    for (i = 0, p_cur = p_response->p_intermediates
            ; p_cur != NULL
            ; p_cur = p_cur->p_next, i ++
    ) {
        char *line = p_cur->line;

        err = at_tok_start(&line);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &skip);
        if (err < 0) goto error;

        // If we're unregistered, we may just get
        // a "+COPS: 0" response
        if (!at_tok_hasmore(&line)) {
            response[i] = NULL;
            continue;
        }

        err = at_tok_nextint(&line, &skip);
        if (err < 0) goto error;

        // a "+COPS: 0, n" response is also possible
        if (!at_tok_hasmore(&line)) {
            response[i] = NULL;
            continue;
        }

        err = at_tok_nextstr(&line, &(response[i]));
        if (err < 0) goto error;
        // Simple assumption that mcc and mnc are 3 digits each
        int length = strlen(response[i]);
        if (length == 6) {
            s_mncLength = 3;
            if (sscanf(response[i], "%3d%3d", &s_mcc, &s_mnc) != 2) {
                RLOGE("requestOperator expected mccmnc to be 6 decimal digits");
            }
        } else if (length == 5) {
            s_mncLength = 2;
            if (sscanf(response[i], "%3d%2d", &s_mcc, &s_mnc) != 2) {
                RLOGE("requestOperator expected mccmnc to be 5 decimal digits");
            }
        }
    }

    if (i != 3) {
        /* expect 3 lines exactly */
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));
    at_response_free(p_response);

    return;

error:
    RLOGE("requestOperator must not return error when radio is on");
    s_mncLength = 0;
    s_mcc = 0;
    s_mnc = 0;
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestSimOpenChannel(void *data, size_t datalen, RIL_Token t)
{
    ATResponse *p_response = NULL;
    int32_t session_id;
    int err;
    char cmd[64] = {0};
    char complex;
    char *line = NULL;
    char *aidPtr = NULL;
    int err_no = RIL_E_GENERIC_FAILURE;
    RIL_Sim_Open_Channel ril_response ={0};

    aidPtr = (char *)data;
    if (NULL == aidPtr) {
        goto error;
    } else {
        snprintf(cmd, sizeof(cmd), "AT+CCHO=%s", aidPtr);
        err = at_send_command_numeric(cmd, &p_response);
    }

    if (err < 0 || p_response == NULL || p_response->success == 0) {
        RLOGE("Error %d opening logical channel: %d",
            err, p_response ? p_response->success : 0);
        goto error;
    }

    // Ensure integer only by scanning for an extra char but expect one result
    line = p_response->p_intermediates->line;
    if (aidPtr == NULL) {
        goto error;
    } else {
        if (sscanf(line, "%" SCNd32 "%c", &session_id, &complex) != 1) {
           RLOGE("Invalid AT response, expected integer, was '%s'", line);
           goto error;
        }
    }

    ril_response.session_id = session_id;
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &ril_response, sizeof(ril_response));
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, err_no, NULL, 0);
    at_response_free(p_response);
    return;
}

static void requestSimCloseChannel(void *data, size_t datalen, RIL_Token t)
{
    ATResponse *p_response = NULL;
    int32_t session_id;
    int err;
    char cmd[32];

    if (data == NULL || datalen != sizeof(session_id)) {
        ALOGE("Invalid data passed to requestSimCloseChannel");
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    session_id = ((int32_t *)data)[0];
    if (session_id == 0) {
        RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
        return;
    }

    snprintf(cmd, sizeof(cmd), "AT+CCHC=%" PRId32, session_id);
    err = at_send_command_singleline(cmd, "+CCHC", &p_response);
    if (err < 0 || p_response == NULL || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        at_response_free(p_response);
        return;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    at_response_free(p_response);
}

static void requestSimTransmitApduChannel(void *data,
    size_t datalen, RIL_Token t)
{
    ATResponse *p_response = NULL;
    int err;
    int len = 0;
    char *cmd;
    char *line = NULL;
    size_t cmd_size;
    RIL_SIM_IO_Response sr = {0};
    RIL_SIM_APDU *apdu = (RIL_SIM_APDU *)data;

    if (apdu == NULL || datalen != sizeof(RIL_SIM_APDU)) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    cmd_size = 10 + (apdu->data ? strlen(apdu->data) : 0);
    asprintf(&cmd, "AT+CGLA=%d,%zu,%02x%02x%02x%02x%02x%s",
        apdu->sessionid, cmd_size, apdu->cla, apdu->instruction,
        apdu->p1, apdu->p2, apdu->p3, apdu->data ? apdu->data : "");

    err = at_send_command_singleline(cmd, "+CGLA:", &p_response);
    free(cmd);
    if (err < 0 || p_response == NULL || p_response->success == 0) {
        ALOGE("Error %d transmitting APDU: %d",
            err, p_response ? p_response->success : 0);
        goto error;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &len);
    if (err < 0) goto error;

    err = at_tok_nextstr(&line, &(sr.simResponse));
    if (err < 0) goto error;

    len = strlen(sr.simResponse);
    if (len < 4) goto error;

    sscanf(&(sr.simResponse[len - 4]), "%02x%02x", &(sr.sw1), &(sr.sw2));
    sr.simResponse[len - 4] = '\0';

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &sr, sizeof(sr));
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestTransmitApduBasic(void *data, size_t datalen, RIL_Token t)
{
    (void)datalen;

    int err, len;
    char *cmd = NULL;
    char *line = NULL;
    RIL_SIM_APDU *p_args = NULL;
    ATResponse *p_response = NULL;
    RIL_SIM_IO_Response sr;

    memset(&sr, 0, sizeof(sr));
    p_args = (RIL_SIM_APDU *)data;

    if ((p_args->data == NULL) || (strlen(p_args->data) == 0)) {
        if (p_args->p3 < 0) {
            asprintf(&cmd, "AT+CSIM=%d,\"%02x%02x%02x%02x\"", 8, p_args->cla,
                p_args->instruction, p_args->p1, p_args->p2);
        } else {
            asprintf(&cmd, "AT+CSIM=%d,\"%02x%02x%02x%02x%02x\"", 10,
                p_args->cla, p_args->instruction, p_args->p1, p_args->p2,
                p_args->p3);
        }
    } else {
        asprintf(&cmd, "AT+CSIM=%d,\"%02x%02x%02x%02x%02x%s\"",
            10 + (int)strlen(p_args->data), p_args->cla,
            p_args->instruction, p_args->p1, p_args->p2, p_args->p3,
            p_args->data);
    }

    err = at_send_command_singleline(cmd, "+CSIM:", &p_response);
    free(cmd);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &len);
    if (err < 0) goto error;

    err = at_tok_nextstr(&line, &(sr.simResponse));
    if (err < 0) goto error;

    sscanf(&(sr.simResponse[len - 4]), "%02x%02x", &(sr.sw1), &(sr.sw2));
    sr.simResponse[len - 4] = '\0';

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &sr, sizeof(sr));
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

#define TYPE_EF                                 4
#define RESPONSE_EF_SIZE                        15
#define TYPE_FILE_DES_LEN                       5
#define RESPONSE_DATA_FILE_DES_FLAG             2
#define RESPONSE_DATA_FILE_DES_LEN_FLAG         3
#define RESPONSE_DATA_FILE_TYPE                 6
#define RESPONSE_DATA_FILE_SIZE_1               2
#define RESPONSE_DATA_FILE_SIZE_2               3
#define RESPONSE_DATA_STRUCTURE                 13
#define RESPONSE_DATA_RECORD_LENGTH             14
#define RESPONSE_DATA_FILE_RECORD_LEN_1         6
#define RESPONSE_DATA_FILE_RECORD_LEN_2         7
#define EF_TYPE_TRANSPARENT                     0x01
#define EF_TYPE_LINEAR_FIXED                    0x02
#define EF_TYPE_CYCLIC                          0x06
#define USIM_DATA_OFFSET_2                      2
#define USIM_DATA_OFFSET_3                      3
#define USIM_FILE_DES_TAG                       0x82
#define USIM_FILE_SIZE_TAG                      0x80

/** Returns SIM_NOT_READY on error */
SIM_Status getSIMStatus(void)
{
    ATResponse *p_response = NULL;
    int err;
    int ret;
    char *cpinLine;
    char *cpinResult;

    RLOGD("getSIMStatus(). RadioState: %d", getRadioState());
    err = at_send_command_singleline("AT+CPIN?", "+CPIN:", &p_response);

    if (err != 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    switch (at_get_cme_error(p_response)) {
        case CME_SUCCESS:
            break;

        case CME_SIM_NOT_INSERTED:
            ret = SIM_ABSENT;
            goto done;

        default:
            ret = SIM_NOT_READY;
            goto done;
    }

    /* CPIN? has succeeded, now look at the result */

    cpinLine = p_response->p_intermediates->line;
    err = at_tok_start(&cpinLine);

    if (err < 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    err = at_tok_nextstr(&cpinLine, &cpinResult);

    if (err < 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    if (0 == strcmp(cpinResult, "SIM PIN")) {
        ret = SIM_PIN;
        goto done;
    } else if (0 == strcmp(cpinResult, "SIM PUK")) {
        ret = SIM_PUK;
        goto done;
    } else if (0 == strcmp(cpinResult, "PH-NET PIN")) {
        return SIM_NETWORK_PERSONALIZATION;
    } else if (0 != strcmp(cpinResult, "READY")) {
        /* we're treating unsupported lock types as "sim absent" */
        ret = SIM_ABSENT;
        goto done;
    }

    at_response_free(p_response);
    p_response = NULL;
    cpinResult = NULL;

    ret = (getRadioState() == RADIO_STATE_ON) ? SIM_READY : SIM_NOT_READY;

done:
    at_response_free(p_response);
    return ret;
}

/**
 * SIM ready means any commands that access the SIM will work, including:
 *  AT+CPIN, AT+CSMS, AT+CNMI, AT+CRSM
 *  (all SMS-related commands)
 */
void pollSIMState(void *param)
{
    (void)param;

    if (getRadioState() != RADIO_STATE_UNAVAILABLE) {
        // no longer valid to poll
        return;
    }

    switch (getSIMStatus()) {
        case SIM_ABSENT:
        case SIM_PIN:
        case SIM_PUK:
        case SIM_NETWORK_PERSONALIZATION:
        default:
            RLOGI("SIM ABSENT or LOCKED");
            RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED, NULL, 0);
        return;

        case SIM_NOT_READY:
            RIL_requestTimedCallback (pollSIMState, NULL, &TIMEVAL_SIMPOLL);
        return;

        case SIM_READY:
            RLOGI("SIM_READY");
            onSIMReady();
            RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED, NULL, 0);
        return;
    }
}

/**
 * Free the card status returned by getCardStatus
 */
static void freeCardStatus(RIL_CardStatus_v1_5 *p_card_status)
{
    if (p_card_status == NULL) {
        return;
    }

    free(p_card_status->base.base.iccid);
    free(p_card_status);
}

static void getIccId(char *iccid, int size)
{
    int err = 0;
    ATResponse *p_response = NULL;

    if (iccid == NULL) {
        RLOGE("iccid buffer is null");
        return;
    }
    err = at_send_command_numeric("AT+CICCID", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    snprintf(iccid, size, "%s", p_response->p_intermediates->line);

error:
    at_response_free(p_response);
}

/**
 * Get the current card status.
 *
 * This must be freed using freeCardStatus.
 * @return: On success returns RIL_E_SUCCESS
 */
static int getCardStatus(RIL_CardStatus_v1_5 **pp_card_status)
{
    static RIL_AppStatus app_status_array[] = {
        // SIM_ABSENT = 0
        { RIL_APPTYPE_UNKNOWN, RIL_APPSTATE_UNKNOWN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // SIM_NOT_READY = 1
        { RIL_APPTYPE_USIM, RIL_APPSTATE_DETECTED, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // SIM_READY = 2
        { RIL_APPTYPE_USIM, RIL_APPSTATE_READY, RIL_PERSOSUBSTATE_READY,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // SIM_PIN = 3
        { RIL_APPTYPE_USIM, RIL_APPSTATE_PIN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },
        // SIM_PUK = 4
        { RIL_APPTYPE_USIM, RIL_APPSTATE_PUK, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_BLOCKED, RIL_PINSTATE_UNKNOWN },
        // SIM_NETWORK_PERSONALIZATION = 5
        { RIL_APPTYPE_USIM, RIL_APPSTATE_SUBSCRIPTION_PERSO, RIL_PERSOSUBSTATE_SIM_NETWORK,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },
        // RUIM_ABSENT = 6
        { RIL_APPTYPE_UNKNOWN, RIL_APPSTATE_UNKNOWN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // RUIM_NOT_READY = 7
        { RIL_APPTYPE_RUIM, RIL_APPSTATE_DETECTED, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // RUIM_READY = 8
        { RIL_APPTYPE_RUIM, RIL_APPSTATE_READY, RIL_PERSOSUBSTATE_READY,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // RUIM_PIN = 9
        { RIL_APPTYPE_RUIM, RIL_APPSTATE_PIN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },
        // RUIM_PUK = 10
        { RIL_APPTYPE_RUIM, RIL_APPSTATE_PUK, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_BLOCKED, RIL_PINSTATE_UNKNOWN },
        // RUIM_NETWORK_PERSONALIZATION = 11
        { RIL_APPTYPE_RUIM, RIL_APPSTATE_SUBSCRIPTION_PERSO, RIL_PERSOSUBSTATE_SIM_NETWORK,
           NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },
        // ISIM_ABSENT = 12
        { RIL_APPTYPE_UNKNOWN, RIL_APPSTATE_UNKNOWN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // ISIM_NOT_READY = 13
        { RIL_APPTYPE_ISIM, RIL_APPSTATE_DETECTED, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // ISIM_READY = 14
        { RIL_APPTYPE_ISIM, RIL_APPSTATE_READY, RIL_PERSOSUBSTATE_READY,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        // ISIM_PIN = 15
        { RIL_APPTYPE_ISIM, RIL_APPSTATE_PIN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },
        // ISIM_PUK = 16
        { RIL_APPTYPE_ISIM, RIL_APPSTATE_PUK, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_BLOCKED, RIL_PINSTATE_UNKNOWN },
        // ISIM_NETWORK_PERSONALIZATION = 17
        { RIL_APPTYPE_ISIM, RIL_APPSTATE_SUBSCRIPTION_PERSO, RIL_PERSOSUBSTATE_SIM_NETWORK,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },
    };

    RIL_CardState card_state;
    int num_apps;

    int sim_status = getSIMStatus();
    if (sim_status == SIM_ABSENT) {
        card_state = RIL_CARDSTATE_ABSENT;
        num_apps = 0;
    } else {
        card_state = RIL_CARDSTATE_PRESENT;
        num_apps = 3;
    }

    // Allocate and initialize base card status.
    RIL_CardStatus_v1_5 *p_card_status = calloc(1, sizeof(RIL_CardStatus_v1_5));
    p_card_status->base.base.base.card_state = card_state;
    p_card_status->base.base.base.universal_pin_state = RIL_PINSTATE_UNKNOWN;
    p_card_status->base.base.base.gsm_umts_subscription_app_index = -1;
    p_card_status->base.base.base.cdma_subscription_app_index = -1;
    p_card_status->base.base.base.ims_subscription_app_index = -1;
    p_card_status->base.base.base.num_applications = num_apps;
    p_card_status->base.base.physicalSlotId = 0;
    p_card_status->base.base.atr = NULL;
    p_card_status->base.base.iccid = NULL;
    p_card_status->base.eid = "";
    if (sim_status != SIM_ABSENT) {
        p_card_status->base.base.iccid = (char *)calloc(64, sizeof(char));
        getIccId(p_card_status->base.base.iccid, 64);
    }

    // Initialize application status
    int i;
    for (i = 0; i < RIL_CARD_MAX_APPS; i ++) {
        p_card_status->base.base.base.applications[i] = app_status_array[SIM_ABSENT];
    }

    RLOGD("enter getCardStatus module, num_apps= %d",num_apps);
    // Pickup the appropriate application status
    // that reflects sim_status for gsm.
    if (num_apps != 0) {
        p_card_status->base.base.base.num_applications = 3;
        p_card_status->base.base.base.gsm_umts_subscription_app_index = 0;
        p_card_status->base.base.base.cdma_subscription_app_index = 1;
        p_card_status->base.base.base.ims_subscription_app_index = 2;

        // Get the correct app status
        p_card_status->base.base.base.applications[0] = app_status_array[sim_status];
        p_card_status->base.base.base.applications[1] = app_status_array[sim_status + RUIM_ABSENT];
        p_card_status->base.base.base.applications[2] = app_status_array[sim_status + ISIM_ABSENT];
    }

    *pp_card_status = p_card_status;
    return RIL_E_SUCCESS;
}

static void requestStksendTerminalResponse(void *data, size_t datalen, RIL_Token t)
{
    (void)datalen;

    int err = -1;
    char cmd[128] = {0};
    ATResponse *p_response = NULL;

    if (data == NULL || strlen((char *)data) == 0) {
        RLOGE("STK sendTerminalResponse data is invalid");
        RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
        return;
    }

    snprintf(cmd, sizeof(cmd), "AT+CUSATT=\"%s\"", (char *)data);
    err = at_send_command_singleline(cmd, "+CUSATT:", &p_response);
    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }

    at_response_free(p_response);
}

static void requestStkSendEnvelope(void *data, size_t datalen, RIL_Token t)
{
    (void)datalen;

    int err = -1;
    char cmd[128] = {0};
    ATResponse *p_response = NULL;

    if (data == NULL || strlen((char *)data) == 0) {
        RLOGE("STK sendEnvelope data is invalid");
        RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
        return;
    }

    snprintf(cmd, sizeof(cmd), "AT+CUSATE=\"%s\"", (char *)data);
    err = at_send_command_singleline(cmd, "+CUSATE:", &p_response);
    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);

        // type of alpha data is 85, such as 850C546F6F6C6B6974204D656E75
        char *p = strstr(p_response->p_intermediates->line, "85");
        if (p != NULL) {
            char alphaStrHexLen[3] = {0};
            char alphaStr[1024] = {0};
            uint8_t *alphaBytes = NULL;
            int len = 0;

            p = p + strlen("85");
            strncpy(alphaStrHexLen, p, 2);
            len = strtoul(alphaStrHexLen, NULL, 16);
            strncpy(alphaStr, p + 2, len * 2);
            alphaBytes = convertHexStringToBytes(alphaStr, strlen(alphaStr));
            RIL_onUnsolicitedResponse(RIL_UNSOL_STK_CC_ALPHA_NOTIFY, alphaBytes,
                strlen((char *)alphaBytes));
            free(alphaBytes);
        }
    }

    at_response_free(p_response);
}

static void requestStkServiceIsRunning(void *data, size_t datalen, RIL_Token t)
{
    (void)data;
    (void)datalen;

    int err = -1;
    ATResponse *p_response = NULL;

    s_stkServiceRunning = true;
    if (NULL != s_stkUnsolResponse) {
       RIL_onUnsolicitedResponse(RIL_UNSOL_STK_PROACTIVE_COMMAND,
            s_stkUnsolResponse, strlen(s_stkUnsolResponse) + 1);
       free(s_stkUnsolResponse);
       s_stkUnsolResponse = NULL;
       RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
       return;
    }

    err = at_send_command_singleline("AT+CUSATD?", "+CUSATD:", &p_response);

    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }

    at_response_free(p_response);
}

static int getSimlockRemainTimes(const char* type)
{
    int err = -1;
    int remain_times = -1;
    char cmd[32] = {0};
    char *line = NULL;
    char *lock_type = NULL;
    ATResponse *p_response = NULL;

    snprintf(cmd, sizeof(cmd), "AT+CPINR=\"%s\"", type);
    err = at_send_command_multiline(cmd, "+CPINR:", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0) goto error;

    err = at_tok_nextstr(&line, &lock_type);
    if (err < 0) goto error;

    err = at_tok_nextint(&line, &remain_times);
    if (err < 0) goto error;

error:
    at_response_free(p_response);
    return remain_times;
}

static void requestFacilityLock(int request, char **data,
    size_t datalen, RIL_Token t)
{
    int err = -1;
    int status = 0;
    int serviceClass = 0;
    int remainTimes = 10;
    char cmd[128] = {0};
    char *line = NULL;
    ATResponse *p_response = NULL;
    RIL_Errno errnoType = RIL_E_GENERIC_FAILURE;

    if (datalen != 5 * sizeof(char *)) {
        goto error;
    }
    if (data[0] == NULL || data[1] == NULL ||
       (data[2] == NULL && request == RIL_REQUEST_SET_FACILITY_LOCK) ||
        strlen(data[0]) == 0 || strlen(data[1]) == 0 ||
       (request == RIL_REQUEST_SET_FACILITY_LOCK && strlen(data[2]) == 0 )) {
        errnoType = RIL_E_INVALID_ARGUMENTS;
        RLOGE("FacilityLock invalid arguments");
        goto error;
    }

    serviceClass = atoi(data[3]);
    if (serviceClass == 0) {
        snprintf(cmd, sizeof(cmd), "AT+CLCK=\"%s\",%c,\"%s\"", data[0], *data[1],
            data[2]);
    } else {
        snprintf(cmd, sizeof(cmd), "AT+CLCK=\"%s\",%c,\"%s\",%s", data[0],
            *data[1], data[2], data[3]);
    }

    if (*data[1] == '2') {          // query status
        err = at_send_command_multiline(cmd, "+CLCK: ", &p_response);
        if (err < 0 || p_response->success == 0) {
            goto error;
        }
        line = p_response->p_intermediates->line;

        err = at_tok_start(&line);
        if (err < 0) goto error;

        err = at_tok_nextint(&line, &status);
        if (err < 0) goto error;

        RIL_onRequestComplete(t, RIL_E_SUCCESS, &status, sizeof(int));
        at_response_free(p_response);
        return;
    } else {            // unlock/lock this facility
        err = at_send_command(cmd, &p_response);
        if (err < 0 || p_response->success == 0) {
            errnoType = RIL_E_PASSWORD_INCORRECT;
            goto error;
        }

        errnoType = RIL_E_SUCCESS;
    }

error:
    if (!strcmp(data[0], "SC")) {
        remainTimes = getSimlockRemainTimes("SIM PIN");
    } else if (!strcmp(data[0], "FD")) {
        remainTimes = getSimlockRemainTimes("SIM PIN2");
    } else {
        remainTimes = 1;
    }

    RIL_onRequestComplete(t, errnoType, &remainTimes, sizeof(remainTimes));
    at_response_free(p_response);
}

static void requestSetSuppServiceNotifications(void *data, size_t datalen,
    RIL_Token t)
{
    (void)datalen;

    int err = 0;
    ATResponse *p_response = NULL;
    int mode = ((int *)data)[0];
    char cmd[32] = {0};

    snprintf(cmd, sizeof(cmd), "AT+CSSN=%d,%d", mode, mode);
    err = at_send_command(cmd, &p_response);
    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }

    at_response_free(p_response);
}

static void requestSendUSSD(void *data, size_t datalen, RIL_Token t)
{
    (void)datalen;

    int err = -1;
    char cmd[128] = {0};
    const char *ussdRequest = (char *)(data);
    ATResponse *p_response = NULL;

    snprintf(cmd, sizeof(cmd), "AT+CUSD=1,\"%s\"", ussdRequest);
    err = at_send_command(cmd, &p_response);
    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }

    at_response_free(p_response);
}

static void requestChangeSimPin2(void *data, size_t datalen, RIL_Token t)
{
    int err;
    int remaintime = -1;
    char cmd[64] = {0};
    const char **strings = (const char **)data;
    ATResponse *p_response = NULL;

    if (datalen != 3 * sizeof(char *)) {
        goto error;
    }

    snprintf(cmd, sizeof(cmd), "AT+CPWD=\"P2\",\"%s\",\"%s\"", strings[0],
        strings[1]);
    err = at_send_command(cmd, &p_response);
    if (err < 0 || p_response->success == 0) {
        remaintime = getSimlockRemainTimes("SIM PIN2");
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_PASSWORD_INCORRECT, &remaintime,
        sizeof(remaintime));
    at_response_free(p_response);
}

static void  requestChangeSimPin(int request, void*  data, size_t  datalen, RIL_Token  t)
{
    ATResponse *p_response = NULL;
    int err;
    int remaintimes = -1;
    char *cmd = NULL;
    const char **strings = (const char **)data;;

    if (datalen == 2 * sizeof(char *) || datalen == 3 * sizeof(char *)) {
        asprintf(&cmd, "AT+CPIN=%s,%s", strings[0], strings[1]);
    } else
        goto error;

    err = at_send_command_singleline(cmd, "+CPIN:", &p_response);
    free(cmd);

    if (err < 0 || p_response->success == 0) {
error:
        if (request == RIL_REQUEST_CHANGE_SIM_PIN) {
            remaintimes = getSimlockRemainTimes("SIM PIN");
        } else if (request == RIL_REQUEST_ENTER_SIM_PUK) {
            remaintimes = getSimlockRemainTimes("SIM PUK");
        } else if (request == RIL_REQUEST_ENTER_SIM_PUK2) {
          remaintimes = getSimlockRemainTimes("SIM PUK2");
        }

        RIL_onRequestComplete(t, RIL_E_PASSWORD_INCORRECT, &remaintimes,
            sizeof(remaintimes));
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }

    at_response_free(p_response);
}

static void requestEnterSimPin(int request, void *data, size_t datalen, RIL_Token t)
{
    ATResponse *p_response = NULL;
    int err;
    int remaintimes = -1;
    char *cmd = NULL;
    const char **strings = (const char **)data;;

    if (datalen == sizeof(char *) || datalen == 2 * sizeof(char *)) {
        asprintf(&cmd, "AT+CPIN=%s", strings[0]);
    } else
        goto error;

    err = at_send_command(cmd, &p_response);
    free(cmd);

    if (err < 0 || p_response->success == 0) {
error:
        if (request == RIL_REQUEST_ENTER_SIM_PIN) {
            remaintimes = getSimlockRemainTimes("SIM PIN");
        } else if (request == RIL_REQUEST_ENTER_SIM_PIN2) {
            remaintimes = getSimlockRemainTimes("SIM PIN2");
        }

        RIL_onRequestComplete(t, RIL_E_PASSWORD_INCORRECT, &remaintimes,
            sizeof(remaintimes));
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }

    at_response_free(p_response);
}

static void requestSIM_IO(void *data, size_t datalen, RIL_Token t)
{
    (void)datalen;

    ATResponse *p_response = NULL;
    RIL_SIM_IO_Response sr;
    int err;
    char *cmd = NULL;
    RIL_SIM_IO_v6 *p_args;
    char *line;

    memset(&sr, 0, sizeof(sr));

    p_args = (RIL_SIM_IO_v6 *)data;

    /* FIXME handle pin2 */

    if (p_args->data == NULL) {
        asprintf(&cmd, "AT+CRSM=%d,%d,%d,%d,%d",
            p_args->command, p_args->fileid,
            p_args->p1, p_args->p2, p_args->p3);
    } else {
        asprintf(&cmd, "AT+CRSM=%d,%d,%d,%d,%d,%s",
            p_args->command, p_args->fileid,
            p_args->p1, p_args->p2, p_args->p3, p_args->data);
    }

    err = at_send_command_singleline(cmd, "+CRSM:", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;
    err = parseSimResponseLine(line, &sr);
    if (err < 0) {
        goto error;
    }

    if (sr.simResponse != NULL &&       // Default to be USIM card
        p_args->command == 192) {       // Get response
        uint8_t *bytes = convertHexStringToBytes(sr.simResponse, strlen(sr.simResponse));
        if (bytes == NULL) {
            goto error;
        }

        if (bytes[0] != 0x62) {
            free(bytes);
            goto error;
        }

        free(bytes);
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &sr, sizeof(sr));
    at_response_free(p_response);
    free(cmd);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
    free(cmd);
}

static void requestGetSimStatus(void *data, size_t datalen, RIL_Token t)
{
    RIL_CardStatus_v1_5 *p_card_status;
    char *p_buffer;
    int buffer_size;

    int result = getCardStatus(&p_card_status);
    if (result == RIL_E_SUCCESS) {
        p_buffer = (char *)p_card_status;
        buffer_size = sizeof(*p_card_status);
    } else {
        p_buffer = NULL;
        buffer_size = 0;
    }

    RIL_onRequestComplete(t, result, p_buffer, buffer_size);
    freeCardStatus(p_card_status);
}

static void requestGetIMSI(void *data, size_t datalen, RIL_Token t)
{
    ATResponse *p_response = NULL;
    int err = at_send_command_numeric("AT+CIMI", &p_response);

    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS,
            p_response->p_intermediates->line, sizeof(char *));
    }

    at_response_free(p_response);
}

static void requestCancelUSSD(void *data, size_t datalen, RIL_Token t)
{
    ATResponse *p_response = NULL;
    int err = at_send_command_numeric("AT+CUSD=2", &p_response);

    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS,
            p_response->p_intermediates->line, sizeof(char *));
    }

    at_response_free(p_response);
}

static void requestEnableUICCApplication(void *data, size_t datalen, RIL_Token t)
{
    if (data == NULL || datalen != sizeof(int)) {
        RIL_onRequestComplete(t, RIL_E_INTERNAL_ERR, NULL, 0);
        return;
    }

    areUiccApplicationsEnabled = *(int *)(data);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestQueryUICCApplication(void *data, size_t datalen, RIL_Token t)
{
    (void)data;
    (void)datalen;
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &areUiccApplicationsEnabled,
        sizeof(areUiccApplicationsEnabled));
}

static int parseProactiveCmdInd(char *response)
{
    int typePos = 0;
    int cmdType = 0;
    char tempStr[3] = {0};
    char *end = NULL;
    StkUnsolEvent ret = STK_UNSOL_EVENT_UNKNOWN;

    if (response == NULL || strlen(response) < 3) {
        return ret;
    }

    if (response[2] <= '7') {
       typePos = 10;
    } else {
       typePos = 12;
    }

    if ((int)strlen(response) < typePos + 1) {
        return ret;
    }

    memcpy(tempStr, &(response[typePos]), 2);
    cmdType = strtoul(tempStr, &end, 16);
    cmdType = 0xFF & cmdType;
    RLOGD("cmdType: %d",cmdType);

    switch (cmdType) {
       case STK_RUN_AT:
       case STK_SEND_DTMF:
       case STK_SEND_SMS:
       case STK_SEND_SS:
       case STK_SEND_USSD:
       case STK_PLAY_TONE:
       case STK_CLOSE_CHANNEL:
           ret = STK_UNSOL_EVENT_NOTIFY;
           break;
       case STK_REFRESH:
            if (strncasecmp(&(response[typePos + 2]), "04", 2) == 0) {  // SIM_RESET
                RLOGD("Type of Refresh is SIM_RESET");
                s_stkServiceRunning = false;
                ret = STK_UNSOL_PROACTIVE_CMD;
            } else {
                ret = STK_UNSOL_EVENT_NOTIFY;
            }
            break;
       default:
            ret = STK_UNSOL_PROACTIVE_CMD;
            break;
    }

    if (getSIMStatus() == SIM_ABSENT && s_stkServiceRunning) {
        s_stkServiceRunning = false;
    }

    if (false == s_stkServiceRunning) {
        ret = STK_UNSOL_EVENT_UNKNOWN;
        s_stkUnsolResponse = (char *)calloc((strlen(response) + 1), sizeof(char));
        snprintf(s_stkUnsolResponse, strlen(response) + 1, "%s", response);
        RLOGD("STK service is not running [%s]", s_stkUnsolResponse);
    }

    return ret;
}

int getMcc(void)
{
    return s_mcc;
}

int getMnc(void)
{
    return s_mnc;
}

int getMncLength(void)
{
    return s_mncLength;
}

void on_request_sim(int request, void *data, size_t datalen, RIL_Token t)
{
    switch (request) {
    case RIL_REQUEST_GET_SIM_STATUS:
        requestGetSimStatus(data, datalen, t);
        break;
    case RIL_REQUEST_ENTER_SIM_PIN:
    case RIL_REQUEST_ENTER_SIM_PIN2:
        requestEnterSimPin(request, data, datalen, t);
        break;
    case RIL_REQUEST_ENTER_SIM_PUK:
    case RIL_REQUEST_ENTER_SIM_PUK2:
    case RIL_REQUEST_CHANGE_SIM_PIN:
        requestChangeSimPin(request, data, datalen, t);
        break;
    case RIL_REQUEST_CHANGE_SIM_PIN2:
        requestChangeSimPin2(data, datalen, t);
        break;
    case RIL_REQUEST_GET_IMSI:
        requestGetIMSI(data, datalen, t);
        break;
    case RIL_REQUEST_OPERATOR:
        requestOperator(data, datalen, t);
        break;
    case RIL_REQUEST_SIM_IO:
        requestSIM_IO(data, datalen, t);
        break;
    case RIL_REQUEST_SEND_USSD:
        requestSendUSSD(data, datalen, t);
        break;
    case RIL_REQUEST_CANCEL_USSD:
        requestCancelUSSD(data, datalen, t);
        break;
    case RIL_REQUEST_QUERY_FACILITY_LOCK:
        char *lockData[4];
        lockData[0] = ((char **)data)[0];
        lockData[1] = "2";
        lockData[2] = ((char **)data)[1];
        lockData[3] = ((char **)data)[2];
        requestFacilityLock(request, lockData, datalen + sizeof(char *), t);
        break;
    case RIL_REQUEST_SET_FACILITY_LOCK:
        requestFacilityLock(request, data, datalen, t);
        break;
    case RIL_REQUEST_SET_SUPP_SVC_NOTIFICATION:
        requestSetSuppServiceNotifications(data, datalen, t);
        break;
    case RIL_REQUEST_STK_SEND_ENVELOPE_COMMAND:
        requestStkSendEnvelope(data, datalen, t);
        break;
    case RIL_REQUEST_STK_SEND_TERMINAL_RESPONSE:
        requestStksendTerminalResponse(data, datalen, t);
        break;
    case RIL_REQUEST_REPORT_STK_SERVICE_IS_RUNNING:
        requestStkServiceIsRunning(data, datalen, t);
        break;
    case RIL_REQUEST_SIM_TRANSMIT_APDU_BASIC:
        requestTransmitApduBasic(data, datalen, t);
        break;
    case RIL_REQUEST_SIM_OPEN_CHANNEL:
        requestSimOpenChannel(data, datalen, t);
        break;
    case RIL_REQUEST_SIM_CLOSE_CHANNEL:
        requestSimCloseChannel(data, datalen, t);
        break;
    case RIL_REQUEST_SIM_TRANSMIT_APDU_CHANNEL:
        requestSimTransmitApduChannel(data, datalen, t);
        break;
    case RIL_REQUEST_ENABLE_UICC_APPLICATIONS:
        requestEnableUICCApplication(data, datalen, t);
        break;
    case RIL_REQUEST_GET_UICC_APPLICATIONS_ENABLEMENT:
        requestQueryUICCApplication(data, datalen, t);
        break;
    default:
        RLOGD("Request not supported");
        RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
        break;
    }

    RLOGD("On request sim end\n");
}

bool try_handle_unsol_sim(const char *s)
{
    bool ret = false;
    char *line = NULL, *p;

    if (strStartsWith(s, "+CCSS: ")) {
        int source = 0;
        line = p = strdup(s);
        if (!line) {
            RLOGE("+CCSS: Unable to allocate memory");
            return true;
        }

        if (at_tok_start(&p) < 0) {
            free(line);
            return true;
        }

        if (at_tok_nextint(&p, &source) < 0) {
            RLOGE("invalid +CCSS response: %s", line);
            free(line);
            return true;
        }

        SSOURCE(getModemInfo()) = source;
        RIL_onUnsolicitedResponse(RIL_UNSOL_CDMA_SUBSCRIPTION_SOURCE_CHANGED,
            &source, sizeof(source));
        free(line);

        ret = true;
    } else if (strStartsWith(s, "+CUSATEND")) {         // session end
        RIL_onUnsolicitedResponse(RIL_UNSOL_STK_SESSION_END, NULL, 0);
        ret = true;
    } else if (strStartsWith(s, "+CUSATP:")) {
        line = p = strdup(s);
        if (!line) {
            RLOGE("+CUSATP: Unable to allocate memory");
            return true;
        }

        if (at_tok_start(&p) < 0) {
            RLOGE("invalid +CUSATP response: %s", s);
            free(line);
            return true;
        }

        char *response = NULL;
        if (at_tok_nextstr(&p, &response) < 0) {
            RLOGE("%s fail", s);
            free(line);
            return true;
        }

        StkUnsolEvent event = parseProactiveCmdInd(response);
        if (event == STK_UNSOL_EVENT_NOTIFY) {
            RIL_onUnsolicitedResponse(RIL_UNSOL_STK_EVENT_NOTIFY, response,
                strlen(response) + 1);
        } else if (event == STK_UNSOL_PROACTIVE_CMD) {
            RIL_onUnsolicitedResponse(RIL_UNSOL_STK_PROACTIVE_COMMAND, response,
                strlen(response) + 1);
        }

        free(line);
        ret = true;
    }

    return ret;
}