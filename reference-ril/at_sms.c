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

#define LOG_TAG "AT_SMS"
#define NDEBUG 1

#include <stdio.h>
#include <string.h>
#include <sys/cdefs.h>

#include <log/log_radio.h>

#include "at_network.h"
#include "at_ril.h"
#include "at_sim.h"
#include "at_sms.h"
#include "at_tok.h"
#include "atchannel.h"
#include "misc.h"

static int s_ims_cause_retry = 0; // 1==causes sms over ims to temp fail
static int s_ims_cause_perm_failure = 0; // 1==causes sms over ims to permanent fail
static int s_ims_gsm_retry = 0; // 1==causes sms over gsm to temp fail
static int s_ims_gsm_fail = 0; // 1==causes sms over gsm to permanent fail

static void requestWriteSmsToSim(void* data, size_t datalen, RIL_Token t)
{
    (void)datalen;

    RIL_SMS_WriteArgs* p_args;
    char* cmd;
    int length;
    int err;
    ATResponse* p_response = NULL;

    if (getSIMStatus() == SIM_ABSENT) {
        RIL_onRequestComplete(t, RIL_E_SIM_ABSENT, NULL, 0);
        return;
    }

    p_args = (RIL_SMS_WriteArgs*)data;
    length = strlen(p_args->pdu) / 2;
    asprintf(&cmd, "AT+CMGW=%d,%d", length, p_args->status);

    err = at_send_command_sms(cmd, p_args->pdu, "+CMGW:", &p_response);

    if (err != 0 || p_response->success == 0) {
        RLOGE("Failure occurred in sending %s due to: %s", cmd, at_io_err_str(err));
        free(cmd);
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    at_response_free(p_response);
    free(cmd);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestCdmaSendSMS(void* data, size_t datalen, RIL_Token t)
{
    int err = 1; // Set to go to error:
    RIL_SMS_Response response;

    memset(&response, 0, sizeof(response));
    if (getSIMStatus() == SIM_ABSENT) {
        RIL_onRequestComplete(t, RIL_E_SIM_ABSENT, NULL, 0);
        return;
    }

    RLOGD("requestCdmaSendSMS datalen=%zu, sizeof(RIL_CDMA_SMS_Message)=%zu",
        datalen, sizeof(RIL_CDMA_SMS_Message));

    if (err != 0)
        goto error;

    // Cdma Send SMS implementation will go here:
    // But it is not implemented yet.

    response.messageRef = 1;
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(response));
    return;

error:
    // Cdma Send SMS will always cause send retry error.
    response.messageRef = -1;
    RIL_onRequestComplete(t, RIL_E_SMS_SEND_FAIL_RETRY, &response, sizeof(response));
}

static void requestSendSMS(void* data, size_t datalen, RIL_Token t)
{
    int err;
    const char* smsc;
    const char* pdu;
    int tpLayerLength;
    char *cmd1, *cmd2;
    RIL_SMS_Response response;
    ATResponse* p_response = NULL;

    if (getSIMStatus() == SIM_ABSENT) {
        RIL_onRequestComplete(t, RIL_E_SIM_ABSENT, NULL, 0);
        return;
    }

    memset(&response, 0, sizeof(response));
    RLOGD("requestSendSMS datalen =%zu", datalen);

    if (s_ims_gsm_fail != 0)
        goto error;
    if (s_ims_gsm_retry != 0)
        goto error2;

    smsc = ((const char**)data)[0];
    pdu = ((const char**)data)[1];

    tpLayerLength = strlen(pdu) / 2;

    // "NULL for default SMSC"
    if (smsc == NULL) {
        smsc = "00";
    }

    asprintf(&cmd1, "AT+CMGS=%d", tpLayerLength);
    asprintf(&cmd2, "%s%s", smsc, pdu);

    err = at_send_command_sms(cmd1, cmd2, "+CMGS:", &p_response);

    free(cmd1);
    free(cmd2);

    if (err != 0 || p_response->success == 0) {
        RLOGE("Failure occurred in sending %s, due to: %s", "AT+CMGS", at_io_err_str(err));
        goto error;
    }

    int messageRef = 1;
    char* line = p_response->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &messageRef);
    if (err < 0)
        goto error;

    /* FIXME fill in ackPDU */
    response.messageRef = messageRef;
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(response));
    at_response_free(p_response);

    return;

error:
    response.messageRef = -2;
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, &response, sizeof(response));
    at_response_free(p_response);
    return;

error2:
    // send retry error.
    response.messageRef = -1;
    RIL_onRequestComplete(t, RIL_E_SMS_SEND_FAIL_RETRY, &response, sizeof(response));
    at_response_free(p_response);
    return;
}

static void requestImsSendSMS(void* data, size_t datalen, RIL_Token t)
{
    RIL_IMS_SMS_Message* p_args;
    RIL_SMS_Response response;

    memset(&response, 0, sizeof(response));

    // figure out if this is gsm/cdma format
    // then route it to requestSendSMS vs requestCdmaSendSMS respectively
    p_args = (RIL_IMS_SMS_Message*)data;

    if (0 != s_ims_cause_perm_failure)
        goto error;

    // want to fail over ims and this is first request over ims
    if (0 != s_ims_cause_retry && 0 == p_args->retry)
        goto error2;

    if (RADIO_TECH_3GPP == p_args->tech) {
        return requestSendSMS(p_args->message.gsmMessage,
            datalen - sizeof(RIL_RadioTechnologyFamily), t);
    } else if (RADIO_TECH_3GPP2 == p_args->tech) {
        return requestCdmaSendSMS(p_args->message.cdmaMessage,
            datalen - sizeof(RIL_RadioTechnologyFamily), t);
    } else {
        RLOGE("requestImsSendSMS invalid format value =%d", p_args->tech);
    }

error:
    response.messageRef = -2;
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, &response, sizeof(response));
    return;

error2:
    response.messageRef = -1;
    RIL_onRequestComplete(t, RIL_E_SMS_SEND_FAIL_RETRY, &response, sizeof(response));
}

static void requestSMSAcknowledge(void* data, size_t datalen, RIL_Token t)
{
    (void)datalen;

    int ackSuccess;
    int err;

    if (getSIMStatus() == SIM_ABSENT) {
        RIL_onRequestComplete(t, RIL_E_SIM_ABSENT, NULL, 0);
        return;
    }

    ackSuccess = ((int*)data)[0];
    if (ackSuccess == 1) {
        err = at_send_command("AT+CNMA=1", NULL);
        if (err < 0) {
            goto error;
        }
    } else if (ackSuccess == 0) {
        err = at_send_command("AT+CNMA=2", NULL);
        if (err < 0) {
            goto error;
        }
    } else {
        RLOGE("unsupported arg to RIL_REQUEST_SMS_ACKNOWLEDGE\n");
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void requestGetSmsBroadcastConfig(void* data, size_t datalen, RIL_Token t)
{
    (void)data;
    (void)datalen;

    ATResponse* p_response = NULL;
    int err = -1, mode, commas = 0, i = 0;
    char* line = NULL;
    char *serviceIds = NULL, *codeSchemes = NULL, *p = NULL;
    char *serviceId = NULL, *codeScheme = NULL;

    err = at_send_command_singleline("AT+CSCB?", "+CSCB:", &p_response);
    if (err < 0 || p_response->success == 0) {
        RLOGE("Failure occurred in sending %s due to: %s", "AT+CSCB?", at_io_err_str(err));
        goto error;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &mode);
    if (err < 0)
        goto error;

    err = at_tok_nextstr(&line, &serviceIds);
    if (err < 0)
        goto error;

    err = at_tok_nextstr(&line, &codeSchemes);
    if (err < 0)
        goto error;

    for (p = serviceIds; *p != '\0'; p++) {
        if (*p == ',') {
            commas++;
        }
    }

    RIL_GSM_BroadcastSmsConfigInfo** pGsmBci = (RIL_GSM_BroadcastSmsConfigInfo**)alloca((commas + 1)
        * sizeof(RIL_GSM_BroadcastSmsConfigInfo*));
    memset(pGsmBci, 0, (commas + 1) * sizeof(RIL_GSM_BroadcastSmsConfigInfo*));

    for (i = 0; i < commas + 1; i++) {
        pGsmBci[i] = (RIL_GSM_BroadcastSmsConfigInfo*)alloca(
            sizeof(RIL_GSM_BroadcastSmsConfigInfo));
        memset(pGsmBci[i], 0, sizeof(RIL_GSM_BroadcastSmsConfigInfo));

        err = at_tok_nextstr(&serviceIds, &serviceId);
        if (err < 0)
            goto error;

        pGsmBci[i]->toServiceId = pGsmBci[i]->fromServiceId = 0;
        if (strstr(serviceId, "-")) {
            sscanf(serviceId, "%d-%d", &pGsmBci[i]->fromServiceId,
                &pGsmBci[i]->toServiceId);
        }

        err = at_tok_nextstr(&codeSchemes, &codeScheme);
        if (err < 0)
            goto error;

        pGsmBci[i]->toCodeScheme = pGsmBci[i]->fromCodeScheme = 0;
        if (strstr(codeScheme, "-")) {
            sscanf(codeScheme, "%d-%d", &pGsmBci[i]->fromCodeScheme,
                &pGsmBci[i]->toCodeScheme);
        }

        pGsmBci[i]->selected = (mode == 0 ? false : true);
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, pGsmBci,
        (commas + 1) * sizeof(RIL_GSM_BroadcastSmsConfigInfo*));
    at_response_free(p_response);
    return;

error:
    at_response_free(p_response);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

static void setGsmBroadcastConfigData(int from, int to, int id, int outStrSize, char* outStr)
{
    if (from < 0 || from > 0xffff || to < 0 || to > 0xffff) {
        RLOGE("setGsmBroadcastConfig data is invalid, [%d, %d]", from, to);
        return;
    }

    if (id != 0) {
        strlcat(outStr, ",", outStrSize);
    }

    int len = strlen(outStr);
    if (from == to) {
        snprintf(outStr + len, outStrSize - len, "%d", from);
    } else {
        snprintf(outStr + len, outStrSize - len, "%d-%d", from, to);
    }
}

static void requestSetSmsBroadcastConfig(void* data, size_t datalen,
    RIL_Token t)
{
    int i = 0;
    int count = datalen / sizeof(RIL_GSM_BroadcastSmsConfigInfo*);
    int size = count * 16;
    char cmd[256] = { 0 };
    char* channel = (char*)alloca(size);
    char* languageId = (char*)alloca(size);
    ATResponse* p_response = NULL;
    RIL_GSM_BroadcastSmsConfigInfo** pGsmBci = (RIL_GSM_BroadcastSmsConfigInfo**)data;
    RIL_GSM_BroadcastSmsConfigInfo gsmBci = { 0 };

    memset(channel, 0, size);
    memset(languageId, 0, size);
    RLOGD("requestSetGsmBroadcastConfig %zu, count %d", datalen, count);

    for (i = 0; i < count; i++) {
        gsmBci = *(pGsmBci[i]);
        setGsmBroadcastConfigData(gsmBci.fromServiceId, gsmBci.toServiceId, i,
            size, channel);
        setGsmBroadcastConfigData(gsmBci.fromCodeScheme, gsmBci.toCodeScheme, i,
            size, languageId);
    }

    snprintf(cmd, sizeof(cmd), "AT+CSCB=%d,\"%s\",\"%s\"",
        (*pGsmBci[0]).selected ? 0 : 1, channel, languageId);
    int err = at_send_command_singleline(cmd, "+CSCB:", &p_response);

    if (err < 0 || p_response->success == 0) {
        RLOGE("Failure occurred in sending %s due to: %s", cmd, at_io_err_str(err));
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }

    at_response_free(p_response);
}

static void requestGetSmscAddress(void* data, size_t datalen, RIL_Token t)
{
    (void)data;
    (void)datalen;

    ATResponse* p_response = NULL;
    int err = -1;
    char* decidata = NULL;

    err = at_send_command_singleline("AT+CSCA?", "+CSCA:", &p_response);
    if (err < 0 || p_response->success == 0) {
        RLOGE("Failure occurred in sending %s due to: %s", "AT+CSCA?", at_io_err_str(err));
        goto error;
    }

    char* line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextstr(&line, &decidata);
    if (err < 0)
        goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, decidata, strlen(decidata) + 1);
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestSetSmscAddress(void* data, size_t datalen, RIL_Token t)
{
    ATResponse* p_response = NULL;
    char cmd[64] = { 0 };
    int err = -1;

    if (getSIMStatus() != SIM_READY) {
        RIL_onRequestComplete(t, RIL_E_SIM_ABSENT, NULL, 0);
        return;
    }

    if (data == NULL || strlen(data) == 0) {
        RLOGE("SET_SMSC_ADDRESS invalid address: %s", (char*)data);
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    snprintf(cmd, sizeof(cmd), "AT+CSCA=%s,%d", (char*)data, (int)datalen);
    err = at_send_command(cmd, &p_response);
    if (err < 0 || p_response->success == 0) {
        RLOGE("Failure occurred in sending %s due to: %s", cmd, at_io_err_str(err));
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }

    at_response_free(p_response);
}

static void requestDeleteSmsOnSim(void* data, size_t datalen, RIL_Token t)
{
    ATResponse* p_response = NULL;
    int err = -1;

    char* cmd;
    p_response = NULL;
    asprintf(&cmd, "AT+CMGD=%d", ((int*)data)[0]);
    err = at_send_command(cmd, &p_response);
    free(cmd);

    if (err < 0 || p_response->success == 0) {
        RLOGE("Failure occurred in sending %s due to: %s", "AT+CMGD=%d", at_io_err_str(err));
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }

    at_response_free(p_response);
}

void on_request_sms(int request, void* data, size_t datalen, RIL_Token t)
{
    switch (request) {
    case RIL_REQUEST_SEND_SMS:
    case RIL_REQUEST_SEND_SMS_EXPECT_MORE:
        requestSendSMS(data, datalen, t);
        break;
    case RIL_REQUEST_SMS_ACKNOWLEDGE:
        requestSMSAcknowledge(data, datalen, t);
        break;
    case RIL_REQUEST_WRITE_SMS_TO_SIM:
        requestWriteSmsToSim(data, datalen, t);
        break;
    case RIL_REQUEST_DELETE_SMS_ON_SIM:
        requestDeleteSmsOnSim(data, datalen, t);
        break;
    case RIL_REQUEST_GSM_GET_BROADCAST_SMS_CONFIG:
        requestGetSmsBroadcastConfig(data, datalen, t);
        break;
    case RIL_REQUEST_GSM_SET_BROADCAST_SMS_CONFIG:
        requestSetSmsBroadcastConfig(data, datalen, t);
        break;
    case RIL_REQUEST_GET_SMSC_ADDRESS:
        requestGetSmscAddress(data, datalen, t);
        break;
    case RIL_REQUEST_SET_SMSC_ADDRESS:
        requestSetSmscAddress(data, datalen, t);
        break;
    case RIL_REQUEST_IMS_SEND_SMS:
        requestImsSendSMS(data, datalen, t);
        break;
    default:
        RLOGE("SMS request not supported");
        RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
        break;
    }

    RLOGD("SMS on request sms end");
}

bool try_handle_unsol_sms(const char* s, const char* sms_pdu)
{
    bool ret = false;

    RLOGD("unsol sms string: %s", s);

    if (strStartsWith(s, "+CMT:")) {
        RLOGI("Receive incoming sms URC");
        RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_NEW_SMS,
            sms_pdu, strlen(sms_pdu));
        ret = true;
    } else if (strStartsWith(s, "+CDS:")) {
        RLOGI("Receive sms status report URC");
        RIL_onUnsolicitedResponse(
            RIL_UNSOL_RESPONSE_NEW_SMS_STATUS_REPORT,
            sms_pdu, strlen(sms_pdu));
        ret = true;
    } else {
        RLOGD("Can't match any unsol sms handlers");
    }

    return ret;
}