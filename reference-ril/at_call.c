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

#define LOG_TAG "AT_CALL"
#define NDEBUG 1

#include <assert.h>
#include <stdio.h>
#include <sys/cdefs.h>

#include <log/log_radio.h>

#include "at_call.h"
#include "at_modem.h"
#include "at_ril.h"
#include "at_sim.h"
#include "at_tok.h"
#include "atchannel.h"
#include "misc.h"

#ifdef USE_TI_COMMANDS

// Enable a workaround
// 1) Make incoming call, do not answer
// 2) Hangup remote end
// Expected: call should disappear from CLCC line
// Actual: Call shows as "ACTIVE" before disappearing
#define WORKAROUND_ERRONEOUS_ANSWER 1

// Some variants of the TI stack do not support the +CGEV unsolicited
// response. However, they seem to send an unsolicited +CME ERROR: 150
#define WORKAROUND_FAKE_CGEV 1
#endif

#ifdef WORKAROUND_ERRONEOUS_ANSWER
// Max number of times we'll try to repoll when we think
// we have a AT+CLCC race condition
#define REPOLL_CALLS_COUNT_MAX 4

// Line index that was incoming or waiting at last poll, or -1 for none
static int s_incomingOrWaitingLine = -1;
// Number of times we've asked for a repoll of AT+CLCC
static int s_repollCallsCount = 0;
// Should we expect a call to be answered in the next CLCC?
static int s_expectAnswer = 0;
#endif /* WORKAROUND_ERRONEOUS_ANSWER */

static int clccStateToRILState(int state, RIL_CallState* p_state)
{
    switch (state) {
    case 0:
        *p_state = RIL_CALL_ACTIVE;
        return 0;
    case 1:
        *p_state = RIL_CALL_HOLDING;
        return 0;
    case 2:
        *p_state = RIL_CALL_DIALING;
        return 0;
    case 3:
        *p_state = RIL_CALL_ALERTING;
        return 0;
    case 4:
        *p_state = RIL_CALL_INCOMING;
        return 0;
    case 5:
        *p_state = RIL_CALL_WAITING;
        return 0;
    default:
        return -1;
    }
}

/**
 * Note: directly modified line and has *p_call point directly into
 * modified line
 */
static int callFromCLCCLine(char* line, RIL_Call* p_call)
{
    // +CLCC: 1,0,2,0,0,\"+18005551212\",145
    //     index,isMT,state,mode,isMpty(,number,TOA)?

    int err;
    int state;
    int mode;

    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &(p_call->index));
    if (err < 0)
        goto error;

    err = at_tok_nextbool(&line, &(p_call->isMT));
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &state);
    if (err < 0)
        goto error;

    err = clccStateToRILState(state, &(p_call->state));
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &mode);
    if (err < 0)
        goto error;

    p_call->isVoice = (mode == 0);

    err = at_tok_nextbool(&line, &(p_call->isMpty));
    if (err < 0)
        goto error;

    if (at_tok_hasmore(&line)) {
        err = at_tok_nextstr(&line, &(p_call->number));

        /* tolerate null here */
        if (err < 0)
            return 0;

        // Some lame implementations return strings
        // like "NOT AVAILABLE" in the CLCC line
        if (p_call->number != NULL
            && 0 == strspn(p_call->number, "+0123456789")) {
            p_call->number = NULL;
        }

        err = at_tok_nextint(&line, &p_call->toa);
        if (err < 0)
            goto error;
    }

    p_call->uusInfo = NULL;

    return 0;

error:
    RLOGE("invalid CLCC line");
    return -1;
}

static void requestCallFailCause(void* data, size_t datalen, RIL_Token t)
{
    (void)data;
    (void)datalen;

    int err = -1;
    int response;
    char* line = NULL;
    ATResponse* p_response = NULL;

    err = at_send_command_singleline("AT+CEER?", "+CEER:", &p_response);
    if (err < 0 || p_response->success == 0) {
        goto error;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &response);
    if (err < 0)
        goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(response));
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

// Hang up, reject, conference, call waiting
static void requestCallSelection(void* data, size_t datalen, RIL_Token t, int request)
{
    (void)data;
    (void)datalen;

    // 3GPP 22.030 6.5.5
    char hangupWaiting[] = "AT+CHLD=0";
    char hangupForeground[] = "AT+CHLD=1";
    char switchWaiting[] = "AT+CHLD=2";
    char conference[] = "AT+CHLD=3";
    char reject[] = "ATH";

    char* atCommand;

    if (getSIMStatus() == SIM_ABSENT) {
        RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
        return;
    }

    switch (request) {
    case RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND:
        // "Releases all held calls or sets User Determined User Busy
        //  (UDUB) for a waiting call."
        atCommand = hangupWaiting;
        break;
    case RIL_REQUEST_HANGUP_FOREGROUND_RESUME_BACKGROUND:
        // "Releases all active calls (if any exist) and accepts
        //  the other (held or waiting) call."
        atCommand = hangupForeground;
        break;
    case RIL_REQUEST_SWITCH_WAITING_OR_HOLDING_AND_ACTIVE:
        // "Places all active calls (if any exist) on hold and accepts
        //  the other (held or waiting) call."
        atCommand = switchWaiting;
#ifdef WORKAROUND_ERRONEOUS_ANSWER
        s_expectAnswer = 1;
#endif /* WORKAROUND_ERRONEOUS_ANSWER */
        break;
    case RIL_REQUEST_CONFERENCE:
        // "Adds a held call to the conversation"
        atCommand = conference;
        break;
    case RIL_REQUEST_UDUB:
        // User determined user busy (reject)
        atCommand = reject;
        break;
    default:
        assert(0);
    }

    at_send_command(atCommand, NULL);
    // Success or failure is ignored by the upper layer here.
    // It will call GET_CURRENT_CALLS and determine success that way.
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestGetCurrentCalls(void* data, size_t datalen, RIL_Token t)
{
    (void)data;
    (void)datalen;

    int err;
    ATResponse* p_response;
    ATLine* p_cur;
    int countCalls;
    int countValidCalls;
    RIL_Call* p_calls;
    RIL_Call** pp_calls;
    int i;
    int needRepoll = 0;

#ifdef WORKAROUND_ERRONEOUS_ANSWER
    int prevIncomingOrWaitingLine;

    prevIncomingOrWaitingLine = s_incomingOrWaitingLine;
    s_incomingOrWaitingLine = -1;
#endif /*WORKAROUND_ERRONEOUS_ANSWER*/

    err = at_send_command_multiline("AT+CLCC", "+CLCC:", &p_response);

    if (err != 0 || p_response->success == 0) {
        RLOGE("Fail to send AT+CLCC due to: %s", at_io_err_str(err));
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    /* count the calls */
    for (countCalls = 0, p_cur = p_response->p_intermediates; p_cur != NULL; p_cur = p_cur->p_next) {
        countCalls++;
    }

    /* yes, there's an array of pointers and then an array of structures */

    pp_calls = (RIL_Call**)alloca(countCalls * sizeof(RIL_Call*));
    p_calls = (RIL_Call*)alloca(countCalls * sizeof(RIL_Call));
    memset(p_calls, 0, countCalls * sizeof(RIL_Call));

    /* init the pointer array */
    for (i = 0; i < countCalls; i++) {
        pp_calls[i] = &(p_calls[i]);
    }

    for (countValidCalls = 0, p_cur = p_response->p_intermediates; p_cur != NULL; p_cur = p_cur->p_next) {
        err = callFromCLCCLine(p_cur->line, p_calls + countValidCalls);

        if (err != 0) {
            RLOGE("Failed to parse the CLCC line");
            continue;
        }

#ifdef WORKAROUND_ERRONEOUS_ANSWER
        if (p_calls[countValidCalls].state == RIL_CALL_INCOMING
            || p_calls[countValidCalls].state == RIL_CALL_WAITING) {
            s_incomingOrWaitingLine = p_calls[countValidCalls].index;
        }
#endif /*WORKAROUND_ERRONEOUS_ANSWER*/

        if (p_calls[countValidCalls].state != RIL_CALL_ACTIVE
            && p_calls[countValidCalls].state != RIL_CALL_HOLDING) {
            needRepoll = 1;
        }

        countValidCalls++;
    }

#ifdef WORKAROUND_ERRONEOUS_ANSWER
    // Basically:
    // A call was incoming or waiting
    // Now it's marked as active
    // But we never answered it
    //
    // This is probably a bug, and the call will probably
    // disappear from the call list in the next poll
    if (prevIncomingOrWaitingLine >= 0
        && s_incomingOrWaitingLine < 0
        && s_expectAnswer == 0) {
        for (i = 0; i < countValidCalls; i++) {

            if (p_calls[i].index == prevIncomingOrWaitingLine
                && p_calls[i].state == RIL_CALL_ACTIVE
                && s_repollCallsCount < REPOLL_CALLS_COUNT_MAX) {
                RLOGI(
                    "Hit WORKAROUND_ERRONOUS_ANSWER case."
                    " Repoll count: %d\n",
                    s_repollCallsCount);
                s_repollCallsCount++;
                goto error;
            }
        }
    }

    s_expectAnswer = 0;
    s_repollCallsCount = 0;
#endif /*WORKAROUND_ERRONEOUS_ANSWER*/

    RIL_onRequestComplete(t, RIL_E_SUCCESS, pp_calls,
        countValidCalls * sizeof(RIL_Call*));

    at_response_free(p_response);

#ifdef POLL_CALL_STATE
    if (countValidCalls) { // We don't seem to get a "NO CARRIER" message from
                           // smd, so we're forced to poll until the call ends.
#else
    if (needRepoll) {
#endif
        // RIL_requestTimedCallback (sendCallStateChanged, NULL, &TIMEVAL_CALLSTATEPOLL);
    }

    return;
#ifdef WORKAROUND_ERRONEOUS_ANSWER
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
#endif
}

static void requestDtmfStart(void* data, size_t datalen, RIL_Token t)
{
    (void)datalen;

    char c_key;
    char* cmd;
    ATResponse* p_response = NULL;
    int err = -1;

    if (NULL == data) {
        RLOGE("data is NULL!");
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    c_key = ((char*)data)[0];
    if (!(c_key >= '0' && c_key <= '9') && c_key != '#' && c_key != '*' && !(c_key >= 'A' && c_key <= 'D')) {
        RLOGE("Invalid argument in RIL");
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    asprintf(&cmd, "AT+VTS=%c", c_key);
    err = at_send_command(cmd, &p_response);
    free(cmd);

    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }

    at_response_free(p_response);
}

static void requestDtmfStop(void* data, size_t datalen, RIL_Token t)
{
    (void)datalen;

    ATResponse* p_response = NULL;
    int err = -1;

    if (NULL != data) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    err = at_send_command("AT+VTS=", &p_response);

    if (err < 0 || p_response->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }

    at_response_free(p_response);
}

static void requestDial(void* data, size_t datalen, RIL_Token t)
{
    (void)datalen;

    RIL_Dial* p_dial;
    char* cmd;
    const char* clir;

    p_dial = (RIL_Dial*)data;

    switch (p_dial->clir) {
    case 1:
        clir = "I";
        break; /* invocation */
    case 2:
        clir = "i";
        break; /* suppression */
    default:
    case 0:
        clir = "";
        break; /* subscription default */
    }

    asprintf(&cmd, "ATD%s%s;", p_dial->address, clir);
    at_send_command(cmd, NULL);
    free(cmd);

    /* success or failure is ignored by the upper layer here.
       it will call GET_CURRENT_CALLS and determine success that way */
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestHangup(void* data, size_t datalen, RIL_Token t)
{
    (void)datalen;

    int* p_line;
    char* cmd;

    p_line = (int*)data;

    // 3GPP 22.030 6.5.5
    // "Releases a specific active call X"
    asprintf(&cmd, "AT+CHLD=1%d", p_line[0]);
    at_send_command(cmd, NULL);
    free(cmd);

    /* success or failure is ignored by the upper layer here.
     * it will call GET_CURRENT_CALLS and determine success that way */
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestEccDial(void* data, size_t datalen, RIL_Token t)
{
    (void)datalen;

    char cmd[64] = { 0 };
    const char* clir = NULL;
    int err = -1;
    RIL_EmergencyDial* p_eccDial = (RIL_EmergencyDial*)data;

    switch (p_eccDial->dialInfo.clir) {
    case 0: /* subscription default */
        clir = "";
        break;
    case 1: /* invocation */
        clir = "I";
        break;
    case 2: /* suppression */
        clir = "i";
        break;
    default:
        break;
    }

    if (p_eccDial->routing == ROUTING_MERGENCY || p_eccDial->routing == ROUTING_UNKNOWN) {
        if (p_eccDial->categories == CATEGORY_UNSPECIFIED) {
            snprintf(cmd, sizeof(cmd), "ATD%s@,#%s;", p_eccDial->dialInfo.address, clir);
        } else {
            snprintf(cmd, sizeof(cmd), "ATD%s@%d,#%s;", p_eccDial->dialInfo.address,
                p_eccDial->categories, clir);
        }
    } else { // ROUTING_NORMAL
        snprintf(cmd, sizeof(cmd), "ATD%s%s;", p_eccDial->dialInfo.address, clir);
    }

    err = at_send_command(cmd, NULL);
    if (err != 0) {
        RLOGE("Failure occurred in sending %s due to: %s", cmd, at_io_err_str(err));
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

/**
 * @brief send the RIL_UNSOL_SUPP_SVC_NOTIFICATION to ofono
 *
 * @param notificationType        0 = MO intermediate result code; 1 = MT unsolicited result code
 * @param code      See 27.007 7.17,"code1" for MO , "code2" for MT.
 * @param index     CUG index. See 27.007 7.17.
 * @param type      "type" from 27.007 7.17 (MT only).
 * @param number    "number" from 27.007 7.17( MT only, may be NULL.)
 */
static void unsolicitedSuppSvcNotification(int notificationType,
    int code, int index, int type, const char* number)
{
    RIL_SuppSvcNotification response = { 0 };

    if ((notificationType != 0) && (notificationType != 1)) {
        RLOGW("unsolicitedSuppSvcNotification notificationType is out of range!");
        return;
    }

    RLOGD("unsolicitedSuppSvcNotification notification code is [%d]!", code);

    response.notificationType = notificationType;
    response.code = code;
    response.index = index;
    response.type = type;

    if (number != NULL) {
        RLOGD("unsolicitedSuppSvcNotification response number [%s]!", number);
        response.number = (char*)number;
    } else {
        response.number = NULL;
    }

    RIL_onUnsolicitedResponse(RIL_UNSOL_SUPP_SVC_NOTIFICATION, &response, sizeof(RIL_SuppSvcNotification));
}

static void requestChangeBarringPassword(char** data, size_t datalen, RIL_Token t)
{
    int err = -1;
    char cmd[64] = { 0 };
    ATResponse* p_response = NULL;

    if (datalen != 3 * sizeof(char*)
        || data[0] == NULL || data[1] == NULL
        || data[2] == NULL || strlen(data[0]) == 0
        || strlen(data[1]) == 0 || strlen(data[2]) == 0) {
        RIL_onRequestComplete(t, RIL_E_INVALID_ARGUMENTS, NULL, 0);
        return;
    }

    snprintf(cmd, sizeof(cmd), "AT+CPWD=\"%s\",\"%s\",\"%s\"", data[0], data[1], data[2]);

    err = at_send_command(cmd, &p_response);
    if (err < 0 || p_response->success == 0) {
        RLOGE("Failure occurred in sending %s due to: %s", cmd, at_io_err_str(err));
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_PASSWORD_INCORRECT, NULL, 0);
    at_response_free(p_response);
}

static void requestSetCallWaiting(void* data, size_t datalen, RIL_Token t)
{
    (void)datalen;

    ATResponse* p_response = NULL;
    int err = -1;
    char cmd[32] = { 0 };
    int enable = ((int*)data)[0];
    int serviceClass = ((int*)data)[1];

    if (serviceClass == 0) {
        snprintf(cmd, sizeof(cmd), "AT+CCWA=1,%d", enable);
    } else {
        snprintf(cmd, sizeof(cmd), "AT+CCWA=1,%d,%d", enable, serviceClass);
    }

    err = at_send_command(cmd, &p_response);
    if (err < 0 || p_response->success == 0) {
        RLOGE("Failure occurred in sending %s due to: %s", cmd, at_io_err_str(err));
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }

    at_response_free(p_response);
}

static void requestQueryCallWaiting(void* data, size_t datalen, RIL_Token t)
{
    (void)datalen;

    int err = -1, mode = 0;
    int serviceClass = ((int*)data)[0];
    int response[2] = { 0, 0 };
    char cmd[32] = { 0 };
    char* line;
    ATLine* p_cur;
    ATResponse* p_response = NULL;

    if (serviceClass == 0) {
        snprintf(cmd, sizeof(cmd), "AT+CCWA=1,2");
    } else {
        snprintf(cmd, sizeof(cmd), "AT+CCWA=1,2,%d", serviceClass);
    }

    err = at_send_command_multiline(cmd, "+CCWA:", &p_response);
    if (err < 0 || p_response->success == 0) {
        RLOGE("Failure occurred in sending %s due to: %s", cmd, at_io_err_str(err));
        goto error;
    }

    for (p_cur = p_response->p_intermediates; p_cur != NULL; p_cur = p_cur->p_next) {
        line = p_cur->line;
        err = at_tok_start(&line);
        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &mode);
        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &serviceClass);
        if (err < 0)
            goto error;

        response[0] = mode;
        response[1] |= serviceClass;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static int forwardFromCCFCULine(char* line, RIL_CallForwardInfo* p_forward)
{
    int err = -1;
    int i = 0;

    if (line == NULL || p_forward == NULL) {
        goto error;
    }

    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &(p_forward->status));
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &(p_forward->serviceClass));
    if (err < 0)
        goto error;

    if (at_tok_hasmore(&line)) {
        int numberType = 0;
        err = at_tok_nextint(&line, &numberType);
        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &p_forward->toa);
        if (err < 0)
            goto error;

        err = at_tok_nextstr(&line, &(p_forward->number));

        /* tolerate null here */
        if (err < 0)
            return 0;

        if (at_tok_hasmore(&line)) {
            for (i = 0; i < 2; i++) {
                skipNextComma(&line);
            }

            if (at_tok_hasmore(&line)) {
                err = at_tok_nextint(&line, &p_forward->timeSeconds);
                if (err < 0) {
                    p_forward->timeSeconds = 0;
                }
            }
        }
    }

    return 0;

error:
    return -1;
}

static void requestQueryCallForward(RIL_CallForwardInfo* data,
    size_t datalen, RIL_Token t)
{
    int err = -1;
    char cmd[128] = { 0 };
    ATResponse* p_response = NULL;
    ATLine* p_cur = NULL;

    if (datalen != sizeof(*data)) {
        goto error;
    }

    snprintf(cmd, sizeof(cmd), "AT+CCFCU=%d,2,%d,%d,\"%s\",%d", data->reason, 2,
        data->toa, data->number ? data->number : "", data->serviceClass);

    err = at_send_command_multiline(cmd, "+CCFCU:", &p_response);
    if (err < 0 || p_response->success == 0) {
        RLOGE("Failure occurred in sending %s due to: %s", cmd, at_io_err_str(err));
        goto error;
    }

    RIL_CallForwardInfo **forwardList = NULL, *forwardPool = NULL;
    int forwardCount = 0;
    int validCount = 0;
    int i = 0;

    for (p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next, forwardCount++) {
    }

    forwardList = (RIL_CallForwardInfo**)
        alloca(forwardCount * sizeof(RIL_CallForwardInfo*));

    forwardPool = (RIL_CallForwardInfo*)
        alloca(forwardCount * sizeof(RIL_CallForwardInfo));

    memset(forwardPool, 0, forwardCount * sizeof(RIL_CallForwardInfo));

    /* init the pointer array */
    for (i = 0; i < forwardCount; i++) {
        forwardList[i] = &(forwardPool[i]);
    }

    for (p_cur = p_response->p_intermediates;
         p_cur != NULL; p_cur = p_cur->p_next) {
        err = forwardFromCCFCULine(p_cur->line, forwardList[validCount]);
        forwardList[validCount]->reason = data->reason;
        if (err == 0)
            validCount++;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, validCount ? forwardList : NULL,
        validCount * sizeof(RIL_CallForwardInfo*));
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestSetCallForward(RIL_CallForwardInfo* data,
    size_t datalen, RIL_Token t)
{
    int err = -1;
    char cmd[128] = { 0 };
    size_t offset = 0;
    ATResponse* p_response = NULL;

    if (datalen != sizeof(*data) || (data->status == 3 && data->number == NULL)) {
        goto error;
    }

    snprintf(cmd, sizeof(cmd), "AT+CCFCU=%d,%d,%d,%d,\"%s\",%d",
        data->reason, data->status, 2, data->toa,
        data->number ? data->number : "", data->serviceClass);
    offset += strlen(cmd);

    if (data->serviceClass == 0) {
        if (data->timeSeconds != 0 && data->status == 3) {
            snprintf(cmd + offset, sizeof(cmd) - offset, ",\"\",\"\",,%d",
                data->timeSeconds);
        }
    } else {
        if (data->timeSeconds != 0 && data->status == 3) {
            snprintf(cmd + offset, sizeof(cmd) - offset, ",\"\",\"\",,%d",
                data->timeSeconds);
        } else {
            strlcat(cmd, ",\"\"", sizeof(cmd) - offset);
        }
    }

    err = at_send_command_multiline(cmd, "+CCFCU:", &p_response);
    if (err < 0 || p_response->success == 0) {
        RLOGE("Failure occurred in sending %s due to: %s", cmd, at_io_err_str(err));
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestSetClir(void* data, size_t datalen, RIL_Token t)
{
    (void)datalen;

    int err = -1;
    int n = ((int*)data)[0];
    char cmd[64] = { 0 };
    ATResponse* p_response = NULL;

    snprintf(cmd, sizeof(cmd), "AT+CLIR=%d", n);
    err = at_send_command(cmd, &p_response);
    if (err < 0 || p_response->success == 0) {
        RLOGE("Failure occurred in sending %s due to: %s", cmd, at_io_err_str(err));
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }

    at_response_free(p_response);
}

static void requestQueryClir(void* data, size_t datalen, RIL_Token t)
{
    (void)datalen;
    (void)data;

    int err = -1;
    int response[2] = { 1, 1 };
    char* line = NULL;
    ATResponse* p_response = NULL;

    if (getSIMStatus() == SIM_ABSENT) {
        RIL_onRequestComplete(t, RIL_E_MODEM_ERR, NULL, 0);
        return;
    }

    err = at_send_command_singleline("AT+CLIR?", "+CLIR:", &p_response);
    if (err < 0 || p_response->success == 0) {
        RLOGE("Failure occurred in sending %s due to: %s", "AT+CLIR?", at_io_err_str(err));
        goto error;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &response[0]);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &response[1]);
    if (err < 0)
        goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestQueryClip(void* data, size_t datalen, RIL_Token t)
{
    (void)datalen;
    (void)data;

    int err = -1;
    int skip = 0;
    int response = 0;
    char* line = NULL;
    ATResponse* p_response = NULL;

    if (getSIMStatus() == SIM_ABSENT) {
        RIL_onRequestComplete(t, RIL_E_MODEM_ERR, NULL, 0);
        return;
    }

    err = at_send_command_singleline("AT+CLIP?", "+CLIP:", &p_response);
    if (err < 0 || p_response->success == 0) {
        RLOGE("Failure occurred in sending %s due to: %s", "AT+CLIP?", at_io_err_str(err));
        goto error;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &skip);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &response);
    if (err < 0)
        goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(response));
    at_response_free(p_response);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

static void requestGetMute(void* data, size_t datalen, RIL_Token t)
{
    (void)data;
    (void)datalen;

    int err = -1;
    int muteResponse = 0; // Mute disabled
    char* line = NULL;
    ATResponse* p_response = NULL;

    err = at_send_command_singleline("AT+CMUT?", "+CMUT:", &p_response);
    if (err < 0 || p_response->success) {
        RLOGE("Failure occurred in sending %s due to: %s", "AT+CMUT?", at_io_err_str(err));
        goto done;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0)
        goto done;

    at_tok_nextint(&line, &muteResponse);

done:
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &muteResponse, sizeof(muteResponse));
    at_response_free(p_response);
}

static void requestSetMute(void* data, size_t datalen, RIL_Token t)
{
    (void)datalen;

    int err = -1;
    char cmd[64] = { 0 };
    ATResponse* p_response = NULL;

    snprintf(cmd, sizeof(cmd), "AT+CMUT=%d", ((int*)data)[0]);
    err = at_send_command(cmd, &p_response);

    if (err < 0 || p_response->success) {
        RLOGE("Failure occurred in sending %s due to: %s", cmd, at_io_err_str(err));
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }

    at_response_free(p_response);
}

static void requestExitEmergencyMode(void* data, size_t datalen, RIL_Token t)
{
    (void)data;
    (void)datalen;

    int err;
    ATResponse* p_response = NULL;

    err = at_send_command("AT+WSOS=0", &p_response);

    if (err < 0 || p_response->success == 0) {
        RLOGE("Failure occurred in sending %s due to: %s", "AT+WSOS=0", at_io_err_str(err));
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestAnswer(void* data, size_t datalen, RIL_Token t)
{
    (void)data;
    (void)datalen;

    at_send_command("ATA", NULL);

#ifdef WORKAROUND_ERRONEOUS_ANSWER
    s_expectAnswer = 1;
#endif /* WORKAROUND_ERRONEOUS_ANSWER */
    // Success or failure is ignored by the upper layer here.
    // It will call GET_CURRENT_CALLS and determine success that way.
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
}

static void requestSeparateConnection(void* data, size_t datalen, RIL_Token t)
{
    char cmd[12];
    int party = ((int*)data)[0];

    // Make sure that party is in a valid range.
    // (Note: The Telephony middle layer imposes a range of 1 to 7.
    // It's sufficient for us to just make sure it's single digit.)
    if (party > 0 && party < 10) {
        sprintf(cmd, "AT+CHLD=2%d", party);
        at_send_command(cmd, NULL);
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    }
}

static void requestExitEmergencyCallbackMode(void* data, size_t datalen, RIL_Token t)
{
    if (TECH_BIT(getModemInfo()) == MDM_CDMA) {
        requestExitEmergencyMode(data, datalen, t);
    } else {
        // VTS tests expect us to silently do nothing
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    }
}

static void requestGetVoiceRadioTech(void* data, size_t datalen, RIL_Token t)
{
    int tech = techFromModemType(TECH(getModemInfo()));
    if (tech < 0)
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    else
        RIL_onRequestComplete(t, RIL_E_SUCCESS, &tech, sizeof(tech));
}

static void requestGetTtyMode(void* data, size_t datalen, RIL_Token t)
{
    (void)data;
    (void)datalen;

    int ttyModeResponse;
    // 1 is TTY Full, 0 is TTY Off
    ttyModeResponse = (getSIMStatus() == SIM_READY) ? 1 : 0;
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &ttyModeResponse, sizeof(ttyModeResponse));
}

void on_request_call(int request, void* data, size_t datalen, RIL_Token t)
{
    switch (request) {
    case RIL_REQUEST_GET_CURRENT_CALLS:
        requestGetCurrentCalls(data, datalen, t);
        break;
    case RIL_REQUEST_DIAL:
        requestDial(data, datalen, t);
        break;
    case RIL_REQUEST_HANGUP:
        requestHangup(data, datalen, t);
        break;
    case RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND:
    case RIL_REQUEST_HANGUP_FOREGROUND_RESUME_BACKGROUND:
    case RIL_REQUEST_SWITCH_WAITING_OR_HOLDING_AND_ACTIVE:
    case RIL_REQUEST_CONFERENCE:
    case RIL_REQUEST_UDUB:
        requestCallSelection(data, datalen, t, request);
        break;
    case RIL_REQUEST_LAST_CALL_FAIL_CAUSE:
        requestCallFailCause(data, datalen, t);
        break;
    case RIL_REQUEST_DTMF:
        requestDtmfStart(data, datalen, t);
        break;
    case RIL_REQUEST_GET_CLIR:
        requestQueryClir(data, datalen, t);
        break;
    case RIL_REQUEST_SET_CLIR:
        requestSetClir(data, datalen, t);
        break;
    case RIL_REQUEST_QUERY_CALL_FORWARD_STATUS:
        requestQueryCallForward(data, datalen, t);
        break;
    case RIL_REQUEST_SET_CALL_FORWARD:
        requestSetCallForward(data, datalen, t);
        break;
    case RIL_REQUEST_QUERY_CALL_WAITING:
        requestQueryCallWaiting(data, datalen, t);
        break;
    case RIL_REQUEST_SET_CALL_WAITING:
        requestSetCallWaiting(data, datalen, t);
        break;
    case RIL_REQUEST_ANSWER:
        requestAnswer(data, datalen, t);
        break;
    case RIL_REQUEST_CHANGE_BARRING_PASSWORD:
        requestChangeBarringPassword(data, datalen, t);
        break;
    case RIL_REQUEST_DTMF_START:
        requestDtmfStart(data, datalen, t);
        break;
    case RIL_REQUEST_DTMF_STOP:
        requestDtmfStop(data, datalen, t);
        break;
    case RIL_REQUEST_SEPARATE_CONNECTION:
        requestSeparateConnection(data, datalen, t);
        break;
    case RIL_REQUEST_SET_MUTE:
        requestSetMute(data, datalen, t);
        break;
    case RIL_REQUEST_GET_MUTE:
        requestGetMute(data, datalen, t);
        break;
    case RIL_REQUEST_QUERY_CLIP:
        requestQueryClip(data, datalen, t);
        break;
    case RIL_REQUEST_SET_TTY_MODE:
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
        break;
    case RIL_REQUEST_QUERY_TTY_MODE:
        requestGetTtyMode(data, datalen, t);
        break;
    case RIL_REQUEST_EXIT_EMERGENCY_CALLBACK_MODE:
        requestExitEmergencyCallbackMode(data, datalen, t);
        break;
    case RIL_REQUEST_VOICE_RADIO_TECH:
        requestGetVoiceRadioTech(data, datalen, t);
        break;
    case RIL_REQUEST_EMERGENCY_DIAL:
        requestEccDial(data, datalen, t);
        break;
    default:
        RLOGE("Request not supported");
        RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
        break;
    }

    RLOGD("On request call end\n");
}

bool try_handle_unsol_call(const char* s)
{
    bool ret = false;
    char *line = NULL, *p;

    RLOGD("unsol call string: %s", s);

    if (strStartsWith(s, "+CRING:")
        || strStartsWith(s, "RING")
        || strStartsWith(s, "NO CARRIER")
        || strStartsWith(s, "+CCWA")
        || strStartsWith(s, "ALERTING")) {
        RLOGI("Receive call state changed URC");
        RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED, NULL, 0);
#ifdef WORKAROUND_FAKE_CGEV
        RIL_requestTimedCallback(onDataCallListChanged, NULL, NULL); // TODO use new function
#endif /* WORKAROUND_FAKE_CGEV */

        ret = true;
    } else if (strStartsWith(s, "HOLD")) {
        RLOGI("Receive supplementary service URC(Remote HOLD)");
        unsolicitedSuppSvcNotification(1, 2, 0, 0, NULL);
        ret = true;
    } else if (strStartsWith(s, "UNHOLD")) {
        RLOGI("Receive supplementary service URC(Remote UNHOLD)");
        unsolicitedSuppSvcNotification(1, 3, 0, 0, NULL);
        ret = true;
    } else if (strStartsWith(s, "+WSOS: ")) {
        RLOGI("Receive emergency mode changed URC");
        char state = 0;
        int unsol;
        line = p = strdup(s);
        if (!line) {
            RLOGE("+WSOS: Unable to allocate memory");
            return true;
        }
        if (at_tok_start(&p) < 0) {
            RLOGE("invalid response string");
            free(line);
            return true;
        }
        if (at_tok_nextbool(&p, &state) < 0) {
            RLOGE("invalid +WSOS response: %s", line);
            free(line);
            return true;
        }
        free(line);

        unsol = state ? RIL_UNSOL_ENTER_EMERGENCY_CALLBACK_MODE : RIL_UNSOL_EXIT_EMERGENCY_CALLBACK_MODE;

        RIL_onUnsolicitedResponse(unsol, NULL, 0);
        ret = true;
    } else {
        RLOGI("Can't match any unsol call handlers");
    }

    return ret;
}