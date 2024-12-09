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

#define LOG_TAG "AT_RIL"
#define NDEBUG 1

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>

#include <log/log_radio.h>
#include <telephony/ril.h>

#include "at_call.h"
#include "at_data.h"
#include "at_modem.h"
#include "at_network.h"
#include "at_ril.h"
#include "at_sim.h"
#include "at_sms.h"
#include "at_tok.h"
#include "atchannel.h"
#include "misc.h"

static void onRequest(int request, void* data, size_t datalen, RIL_Token t);
static RIL_RadioState currentState(void);
static int onSupports(int requestCode);
static void onCancel(RIL_Token t);
static const char* getVersion(void);

static pthread_mutex_t s_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_state_cond = PTHREAD_COND_INITIALIZER;

static RIL_RadioState sState = RADIO_STATE_UNAVAILABLE;

/*** Static Variables ***/
static const RIL_RadioFunctions s_callbacks = {
    RIL_VERSION,
    onRequest,
    currentState,
    onSupports,
    onCancel,
    getVersion
};

typedef enum req_category {
    REQ_UKNOWN_TYPE = 0,
    REQ_MODEM_TYPE,
    REQ_CALL_TYPE,
    REQ_SMS_TYPE,
    REQ_SIM_TYPE,
    REQ_DATA_TYPE,
    REQ_NETWORK_TYPE,
    REQ_NOT_SUPPORTED,
} req_category_t;

static const struct timeval TIMEVAL_0 = { 0, 0 };
static const struct RIL_Env* s_rilenv;

/* trigger change to this with s_state_cond */
static int s_closed = 0;

static inline req_category_t request2eventtype(int request)
{
    req_category_t type = REQ_UKNOWN_TYPE;

    switch (request) {
    case RIL_REQUEST_GET_SIM_STATUS:
    case RIL_REQUEST_ENTER_SIM_PIN:
    case RIL_REQUEST_ENTER_SIM_PUK:
    case RIL_REQUEST_ENTER_SIM_PIN2:
    case RIL_REQUEST_ENTER_SIM_PUK2:
    case RIL_REQUEST_CHANGE_SIM_PIN:
    case RIL_REQUEST_CHANGE_SIM_PIN2:
    case RIL_REQUEST_GET_IMSI:
    case RIL_REQUEST_OPERATOR:
    case RIL_REQUEST_SIM_IO:
    case RIL_REQUEST_SEND_USSD:
    case RIL_REQUEST_CANCEL_USSD:
    case RIL_REQUEST_QUERY_FACILITY_LOCK:
    case RIL_REQUEST_SET_FACILITY_LOCK:
    case RIL_REQUEST_SET_SUPP_SVC_NOTIFICATION:
    case RIL_REQUEST_STK_SEND_ENVELOPE_COMMAND:
    case RIL_REQUEST_STK_SEND_TERMINAL_RESPONSE:
    case RIL_REQUEST_REPORT_STK_SERVICE_IS_RUNNING:
    case RIL_REQUEST_SIM_TRANSMIT_APDU_BASIC:
    case RIL_REQUEST_SIM_OPEN_CHANNEL:
    case RIL_REQUEST_SIM_CLOSE_CHANNEL:
    case RIL_REQUEST_SIM_TRANSMIT_APDU_CHANNEL:
    case RIL_REQUEST_ENABLE_UICC_APPLICATIONS:
    case RIL_REQUEST_GET_UICC_APPLICATIONS_ENABLEMENT:
        type = REQ_SIM_TYPE;
        break;
    case RIL_REQUEST_GET_CURRENT_CALLS:
    case RIL_REQUEST_DIAL:
    case RIL_REQUEST_HANGUP:
    case RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND:
    case RIL_REQUEST_HANGUP_FOREGROUND_RESUME_BACKGROUND:
    case RIL_REQUEST_SWITCH_WAITING_OR_HOLDING_AND_ACTIVE:
    case RIL_REQUEST_CONFERENCE:
    case RIL_REQUEST_UDUB:
    case RIL_REQUEST_LAST_CALL_FAIL_CAUSE:
    case RIL_REQUEST_DTMF:
    case RIL_REQUEST_GET_CLIR:
    case RIL_REQUEST_SET_CLIR:
    case RIL_REQUEST_QUERY_CALL_FORWARD_STATUS:
    case RIL_REQUEST_SET_CALL_FORWARD:
    case RIL_REQUEST_QUERY_CALL_WAITING:
    case RIL_REQUEST_SET_CALL_WAITING:
    case RIL_REQUEST_ANSWER:
    case RIL_REQUEST_CHANGE_BARRING_PASSWORD:
    case RIL_REQUEST_DTMF_START:
    case RIL_REQUEST_DTMF_STOP:
    case RIL_REQUEST_SEPARATE_CONNECTION:
    case RIL_REQUEST_SET_MUTE:
    case RIL_REQUEST_GET_MUTE:
    case RIL_REQUEST_EXPLICIT_CALL_TRANSFER:
    case RIL_REQUEST_QUERY_CLIP:
    case RIL_REQUEST_SET_TTY_MODE:
    case RIL_REQUEST_QUERY_TTY_MODE:
    case RIL_REQUEST_EXIT_EMERGENCY_CALLBACK_MODE:
    case RIL_REQUEST_VOICE_RADIO_TECH:
    case RIL_REQUEST_EMERGENCY_DIAL:
    case RIL_REQUEST_ADD_PARTICIPANT:
    case RIL_REQUEST_DIAL_CONFERENCE:
        type = REQ_CALL_TYPE;
        break;
    case RIL_REQUEST_ENTER_NETWORK_DEPERSONALIZATION:
    case RIL_REQUEST_SIGNAL_STRENGTH:
    case RIL_REQUEST_VOICE_REGISTRATION_STATE:
    case RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE:
    case RIL_REQUEST_SET_NETWORK_SELECTION_AUTOMATIC:
    case RIL_REQUEST_SET_NETWORK_SELECTION_MANUAL:
    case RIL_REQUEST_QUERY_AVAILABLE_NETWORKS:
    case RIL_REQUEST_SET_BAND_MODE:
    case RIL_REQUEST_QUERY_AVAILABLE_BAND_MODE:
    case RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE:
    case RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE:
    case RIL_REQUEST_GET_NEIGHBORING_CELL_IDS:
    case RIL_REQUEST_SET_LOCATION_UPDATES:
    case RIL_REQUEST_GET_CELL_INFO_LIST:
    case RIL_REQUEST_SET_UNSOL_CELL_INFO_LIST_RATE:
    case RIL_REQUEST_IMS_REGISTRATION_STATE:
    case RIL_REQUEST_IMS_REG_STATE_CHANGE:
    case RIL_REQUEST_IMS_SET_SERVICE_STATUS:
        type = REQ_NETWORK_TYPE;
        break;
    case RIL_REQUEST_DATA_REGISTRATION_STATE:
    case RIL_REQUEST_SETUP_DATA_CALL:
    case RIL_REQUEST_DEACTIVATE_DATA_CALL:
    case RIL_REQUEST_DATA_CALL_LIST:
    case RIL_REQUEST_SET_INITIAL_ATTACH_APN:
    case RIL_REQUEST_ALLOW_DATA:
    case RIL_REQUEST_SET_DATA_PROFILE:
        type = REQ_DATA_TYPE;
        break;
    case RIL_REQUEST_RADIO_POWER:
    case RIL_REQUEST_GET_IMEI:
    case RIL_REQUEST_GET_IMEISV:
    case RIL_REQUEST_BASEBAND_VERSION:
    case RIL_REQUEST_OEM_HOOK_RAW:
    case RIL_REQUEST_OEM_HOOK_STRINGS:
    case RIL_REQUEST_SCREEN_STATE:
    case RIL_REQUEST_GET_ACTIVITY_INFO:
    case RIL_REQUEST_DEVICE_IDENTITY:
    case RIL_REQUEST_ENABLE_MODEM:
    case RIL_REQUEST_GET_MODEM_STATUS:
        type = REQ_MODEM_TYPE;
        break;
    case RIL_REQUEST_SEND_SMS:
    case RIL_REQUEST_SEND_SMS_EXPECT_MORE:
    case RIL_REQUEST_SMS_ACKNOWLEDGE:
    case RIL_REQUEST_WRITE_SMS_TO_SIM:
    case RIL_REQUEST_DELETE_SMS_ON_SIM:
    case RIL_REQUEST_GET_SMSC_ADDRESS:
    case RIL_REQUEST_SET_SMSC_ADDRESS:
    case RIL_REQUEST_IMS_SEND_SMS:
    case RIL_REQUEST_GSM_GET_BROADCAST_SMS_CONFIG:
    case RIL_REQUEST_GSM_SET_BROADCAST_SMS_CONFIG:
        type = REQ_SMS_TYPE;
        break;
    case RIL_REQUEST_LAST_DATA_CALL_FAIL_CAUSE:
    case RIL_REQUEST_RESET_RADIO:
    case RIL_REQUEST_STK_GET_PROFILE:
    case RIL_REQUEST_STK_SET_PROFILE:
    case RIL_REQUEST_STK_HANDLE_CALL_SETUP_REQUESTED_FROM_SIM:
    case RIL_REQUEST_GSM_SMS_BROADCAST_ACTIVATION:
    case RIL_REQUEST_REPORT_SMS_MEMORY_STATUS:
    case RIL_REQUEST_ISIM_AUTHENTICATION:
    case RIL_REQUEST_ACKNOWLEDGE_INCOMING_GSM_SMS_WITH_PDU:
    case RIL_REQUEST_STK_SEND_ENVELOPE_WITH_STATUS:
    case RIL_REQUEST_REMOVE_PARTICIPANT:
        type = REQ_NOT_SUPPORTED;
        break;
    default:
        assert(type == REQ_UKNOWN_TYPE);
        break;
    }

    return type;
}

static const char* getVersion(void)
{
    return "android reference-ril 1.0";
}

static int query_supported_techs(ModemInfo* mdm, int* supported)
{
    (void)mdm;

    ATResponse* p_response = NULL;
    RIL_Errno ril_err = RIL_E_SUCCESS;
    int err = -1;
    int val = 0;
    int techs = 0;
    char* line = NULL;

    RLOGD("query_supported_techs");
    err = at_send_command_singleline("AT+CTEC=?", "+CTEC:", &p_response);
    if (err != AT_ERROR_OK || !p_response || p_response->success != AT_OK) {
        RLOGE("Failure occurred in sending %s due to: %s", "AT+CTEC=?", at_io_err_str(err));
        ril_err = RIL_E_GENERIC_FAILURE;
        goto on_exit;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err || !at_tok_hasmore(&line)) {
        RLOGE("Fail to parse line in %s", __func__);
        ril_err = RIL_E_GENERIC_FAILURE;
        goto on_exit;
    }

    while (!at_tok_nextint(&line, &val)) {
        techs |= (1 << val);
    }

    if (supported)
        *supported = techs;

on_exit:
    at_response_free(p_response);
    return ril_err == RIL_E_SUCCESS ? 0 : -1;
}

static int is_multimode_modem(ModemInfo* mdm)
{
    int tech;
    int32_t preferred;

    if (query_ctec(mdm, &tech, &preferred) == 0) {
        mdm->currentTech = tech;
        mdm->preferredNetworkMode = preferred;
        if (query_supported_techs(mdm, &mdm->supportedTechs)) {
            return 0;
        }

        return 1;
    }

    return 0;
}

/* Find out if our modem is GSM, LTE or both (Multimode) */
static void probeForModemMode(ModemInfo* info)
{
    assert(info);
    // Currently, our only known multimode modem is qemu's android modem,
    // which implements the AT+CTEC command to query and set mode.
    // Try that first

    if (is_multimode_modem(info)) {
        RLOGI("Found Multimode Modem. Supported techs mask: %8.8x. Current tech: %d",
            info->supportedTechs, info->currentTech);
        return;
    }

    /* Being here means that our modem is not multimode */
    info->isMultimode = 0;

    // TODO: find out if modem really supports LTE
    info->supportedTechs = MDM_GSM | MDM_LTE;
    info->currentTech = MDM_LTE;
    RLOGI("Found LTE Modem");
}

static void waitForClose(void)
{
    pthread_mutex_lock(&s_state_mutex);

    while (s_closed == 0) {
        pthread_cond_wait(&s_state_cond, &s_state_mutex);
    }

    pthread_mutex_unlock(&s_state_mutex);
}

/* do post-AT+CFUN=1 initialization */
static void onRadioPowerOn(void)
{
    pollSIMState(NULL);
}

/**
 * Initialize everything that can be configured while we're still in
 * AT+CFUN=0
 */
static void initializeCallback(void* param)
{
    (void)param;

    ATResponse* p_response = NULL;
    int err;

    setRadioState(RADIO_STATE_OFF);

    at_handshake();

    probeForModemMode(getModemInfo());
    /* note: we don't check errors here. Everything important will
       be handled in onATTimeout and onATReaderClosed */

    /*  atchannel is tolerant of echo but it must */
    /*  have verbose result codes */
    at_send_command("ATE0Q0V1", NULL);

    /*  No auto-answer */
    at_send_command("ATS0=0", NULL);

    /*  Extended errors */
    at_send_command("AT+CMEE=1", NULL);

    /*  Network registration events */
    err = at_send_command("AT+CREG=2", &p_response);

    /* some handsets -- in tethered mode -- don't support CREG=2 */
    if (err < 0 || p_response->success == 0) {
        at_send_command("AT+CREG=1", NULL);
    }

    at_response_free(p_response);

    /*  GPRS registration events */
    at_send_command("AT+CGREG=1", NULL);

    /*  Call Waiting notifications */
    at_send_command("AT+CCWA=1", NULL);

    /*  Alternating voice/data off */
    at_send_command("AT+CMOD=0", NULL);

    /*  Not muted */
    at_send_command("AT+CMUT=0", NULL);

    /*  +CSSU unsolicited supp service notifications */
    at_send_command("AT+CSSN=0,1", NULL);

    /*  no connected line identification */
    at_send_command("AT+COLP=0", NULL);

    /*  HEX character set */
    at_send_command("AT+CSCS=\"HEX\"", NULL);

    /*  USSD unsolicited */
    at_send_command("AT+CUSD=1", NULL);

    /*  Enable +CGEV GPRS event notifications, but don't buffer */
    at_send_command("AT+CGEREP=1,0", NULL);

    /*  SMS PDU mode */
    at_send_command("AT+CMGF=0", NULL);

    /* assume radio is off on error */
    if (isRadioOn() > 0) {
        setRadioState(RADIO_STATE_ON);
    }
}

/**
 * Called by atchannel when an unsolicited line appears
 * This is called on atchannel's reader thread. AT commands may
 * not be issued here
 */
static void onUnsolicited(const char* s, const char* sms_pdu)
{
    if (isModemEnable() == 0) {
        RLOGW("Modem is not alive");
        return;
    }

    /* Ignore unsolicited responses until we're initialized.
     * This is OK because the RIL library will poll for initial state */
    if (getRadioState() == RADIO_STATE_UNAVAILABLE) {
        RLOGW("Radio unavailable");
        return;
    }

    if (sms_pdu) {
        RLOGI("Handling sms notification");
    }

    if (try_handle_unsol_call(s)) {
        return;
    }

    if (try_handle_unsol_modem(s)) {
        return;
    }

    if (try_handle_unsol_net(s)) {
        return;
    }

    if (try_handle_unsol_sms(s, sms_pdu)) {
        return;
    }

    if (try_handle_unsol_data(s)) {
        return;
    }

    if (try_handle_unsol_sim(s)) {
        return;
    }

    RLOGD("Can't handle AT line: %s", s);
}

/* Called on command or reader thread */
static void onATReaderClosed(void)
{
    RLOGI("AT channel closed");
    at_close();
    s_closed = 1;

    setRadioState(RADIO_STATE_UNAVAILABLE);
}

/* Called on command thread */
static void onATTimeout(void)
{
    RLOGI("AT channel timeout; closing");
    at_close();

    s_closed = 1;

    /* FIXME cause a radio reset here */

    setRadioState(RADIO_STATE_UNAVAILABLE);
}

static void* mainLoop(void* param)
{
    (void)param;

    int fd;
    int ret;

    AT_DUMP("== ", "entering mainLoop()", -1);
    at_set_on_reader_closed(onATReaderClosed);
    at_set_on_timeout(onATTimeout);

    for (;;) {
        fd = -1;
        while (fd < 0) {
            if (isInEmulator()) {
                fd = open("/dev/ttyV0", O_RDWR);
                RLOGI("opening qemu_modem_port %d!", fd);
            }

            if (fd < 0) {
                RLOGE("opening AT interface. retrying...");
                sleep(10);
                /* never returns */
            }
        }

        s_closed = 0;
        ret = at_open(fd, onUnsolicited);

        if (ret < 0) {
            RLOGE("AT error %d on at_open\n", ret);
            return 0;
        }

        RIL_requestTimedCallback(initializeCallback, NULL, &TIMEVAL_0);

        // Give initializeCallback a chance to dispatched, since
        // we don't presently have a cancellation mechanism
        sleep(1);

        waitForClose();
        RLOGI("Re-opening after close");
    }
}

pthread_t s_tid_mainloop;

const RIL_RadioFunctions* RIL_Init(const struct RIL_Env* env, int argc, char** argv)
{
    int ret;
    pthread_attr_t attr;

    s_rilenv = env;

    RLOGI("RIL_Init");

    initModem();
    if (!getModemInfo()) {
        RLOGE("Unable to alloc memory for ModemInfo");
        return NULL;
    }

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    ret = pthread_create(&s_tid_mainloop, &attr, mainLoop, NULL);
    if (ret < 0) {
        RLOGE("pthread_create: %s:", strerror(errno));
        return NULL;
    }

    return &s_callbacks;
}

/**
 * Synchronous call from the RIL to us to return current radio state.
 * RADIO_STATE_UNAVAILABLE should be the initial state.
 */
static RIL_RadioState currentState(void)
{
    return getRadioState();
}

/**
 * Call from RIL to us to find out whether a specific request code
 * is supported by this implementation.
 *
 * Return 1 for "supported" and 0 for "unsupported"
 */
static int onSupports(int requestCode)
{
    (void)requestCode;

    //@@@ todo

    return 1;
}

static void onCancel(RIL_Token t)
{
    (void)t;

    //@@@todo
}

/*** Callback methods from the RIL library to us ***/

/**
 * Call from RIL to us to make a RIL_REQUEST
 *
 * Must be completed with a call to RIL_onRequestComplete()
 *
 * RIL_onRequestComplete() may be called from any thread, before or after
 * this function returns.
 *
 * Because onRequest function could be called from multiple different thread,
 * we must ensure that the underlying at_send_command_* function
 * is atomic.
 */
static void onRequest(int request, void* data, size_t datalen, RIL_Token t)
{
    int req_type = 0;

    req_type = request2eventtype(request);
    RLOGI("onRequest: %d<->%s, reqtype: %d", request, requestToString(request), req_type);

    if (req_type < 1) {
        RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
        return;
    }

    RLOGD("onRequest: %d, RadioState: %d", request, getRadioState());
    if (isModemEnable() == 0 && request != RIL_REQUEST_ENABLE_MODEM
        && request != RIL_REQUEST_GET_MODEM_STATUS) {
        RLOGE("The modem is disabled");
        RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
        return;
    }

    /* Ignore all requests except RIL_REQUEST_GET_SIM_STATUS
     * when RADIO_STATE_UNAVAILABLE. */
    if (getRadioState() == RADIO_STATE_UNAVAILABLE
        && request != RIL_REQUEST_GET_SIM_STATUS
        && request != RIL_REQUEST_ENABLE_MODEM
        && request != RIL_REQUEST_GET_MODEM_STATUS) {
        RLOGE("Radio unavailable");
        RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
        return;
    }

    /* Ignore all non-power requests when RADIO_STATE_OFF
     * (except RIL_REQUEST_GET_SIM_STATUS) */
    if (getRadioState() == RADIO_STATE_OFF) {
        switch (request) {
        case RIL_REQUEST_BASEBAND_VERSION:
        case RIL_REQUEST_DEVICE_IDENTITY:
        case RIL_REQUEST_EXIT_EMERGENCY_CALLBACK_MODE:
        case RIL_REQUEST_GET_ACTIVITY_INFO:
        case RIL_REQUEST_GET_CURRENT_CALLS:
        case RIL_REQUEST_GET_IMEI:
        case RIL_REQUEST_GET_IMEISV:
        case RIL_REQUEST_GET_MUTE:
        case RIL_REQUEST_SET_MUTE:
        case RIL_REQUEST_GET_NEIGHBORING_CELL_IDS:
        case RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE:
        case RIL_REQUEST_GET_SIM_STATUS:
        case RIL_REQUEST_QUERY_AVAILABLE_BAND_MODE:
        case RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE:
        case RIL_REQUEST_QUERY_TTY_MODE:
        case RIL_REQUEST_RADIO_POWER:
        case RIL_REQUEST_OEM_HOOK_STRINGS:
        case RIL_REQUEST_SET_BAND_MODE:
        case RIL_REQUEST_SET_LOCATION_UPDATES:
        case RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE:
        case RIL_REQUEST_SET_TTY_MODE:
        case RIL_REQUEST_SET_UNSOL_CELL_INFO_LIST_RATE:
        case RIL_REQUEST_VOICE_RADIO_TECH:
        case RIL_REQUEST_SCREEN_STATE:
        case RIL_REQUEST_ENABLE_MODEM:
        case RIL_REQUEST_GET_MODEM_STATUS:
        case RIL_REQUEST_GSM_GET_BROADCAST_SMS_CONFIG:
            // Process all the above, even though the radio is off
            break;

        default:
            // For all others, say NOT_AVAILABLE because the radio is off
            RLOGE("Radio has been turned off");
            RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
            return;
        }
    }

    switch (req_type) {
    case REQ_MODEM_TYPE:
        on_request_modem(request, data, datalen, t);
        break;
    case REQ_CALL_TYPE:
        on_request_call(request, data, datalen, t);
        break;
    case REQ_SMS_TYPE:
        on_request_sms(request, data, datalen, t);
        break;
    case REQ_SIM_TYPE:
        on_request_sim(request, data, datalen, t);
        break;
    case REQ_DATA_TYPE:
        on_request_data(request, data, datalen, t);
        break;
    case REQ_NETWORK_TYPE:
        on_request_network(request, data, datalen, t);
        break;
    case REQ_NOT_SUPPORTED:
        RLOGE("Request not supported");
        RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
        break;
    default:
        RLOGE("Unknown Request");
        RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
        break;
    }

    RLOGD("On request end\n");
}

int isConnectionClosed(void)
{
    return s_closed;
}

const struct RIL_Env* getRilEnv(void)
{
    return s_rilenv;
}

void setRadioState(RIL_RadioState newState)
{
    RLOGD("setRadioState(%d)", newState);
    RIL_RadioState oldState;

    pthread_mutex_lock(&s_state_mutex);
    oldState = sState;

    if (isConnectionClosed() > 0) {
        // If we're closed, the only reasonable state is
        // RADIO_STATE_UNAVAILABLE
        // This is here because things on the main thread
        // may attempt to change the radio state after the closed
        // event happened in another thread
        newState = RADIO_STATE_UNAVAILABLE;
    }

    if (sState != newState || isConnectionClosed() > 0) {
        sState = newState;
        pthread_cond_broadcast(&s_state_cond);
    }

    pthread_mutex_unlock(&s_state_mutex);

    /* do these outside of the mutex */
    if (sState != oldState) {
        RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED,
            NULL, 0);
        // Sim state can change as result of radio state change
        RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED,
            NULL, 0);

        /* FIXME onSimReady() and onRadioPowerOn() cannot be called
         * from the AT reader thread
         * Currently, this doesn't happen, but if that changes then these
         * will need to be dispatched on the request thread */
        if (sState == RADIO_STATE_ON) {
            onRadioPowerOn();
        }
    }
}

RIL_RadioState getRadioState(void)
{
    return sState;
}