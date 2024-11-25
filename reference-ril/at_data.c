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

#define LOG_TAG "AT_DATA"
#define NDEBUG 1

#include <alloca.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/cdefs.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <telephony/ril_log.h>

#include "at_data.h"
#include "at_modem.h"
#include "at_network.h"
#include "at_ril.h"
#include "at_sim.h"
#include "at_tok.h"
#include "atchannel.h"
#include "misc.h"

#define MAX_PDP 11 // max LTE bearers

/* pathname returned from RIL_REQUEST_SETUP_DATA_CALL / RIL_REQUEST_SETUP_DEFAULT_PDP */
// This is used if Wifi is not supported, plain old eth0
#define PPP_TTY_PATH_ETH0 "eth0"

// This is used for emulator
#define EMULATOR_RADIO_INTERFACE "eth0"

// Default MTU value
#define DEFAULT_MTU 1500

enum PDPState {
    PDP_IDLE,
    PDP_BUSY,
};

struct PDPInfo {
    int cid;
    enum PDPState state;
};

struct PDPInfo s_PDP[] = {
    { 1, PDP_IDLE },
    { 2, PDP_IDLE },
    { 3, PDP_IDLE },
    { 4, PDP_IDLE },
    { 5, PDP_IDLE },
    { 6, PDP_IDLE },
    { 7, PDP_IDLE },
    { 8, PDP_IDLE },
    { 9, PDP_IDLE },
    { 10, PDP_IDLE },
    { 11, PDP_IDLE },
};

enum InterfaceState {
    kInterfaceUp,
    kInterfaceDown,
};

static void requestOrSendDataCallList(int cid, RIL_Token* t);

static void set_Ip_Addr(const char* addr, const char* radioInterfaceName)
{
    RLOGD("%s %d setting ip addr %s on interface %s", __func__, __LINE__, addr,
        radioInterfaceName);
    struct ifreq request;
    struct sockaddr_in* sin = NULL;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock == -1) {
        RLOGE("Failed to open interface socket: %s (%d)", strerror(errno), errno);
        return;
    }

    memset(&request, 0, sizeof(request));
    strncpy(request.ifr_name, radioInterfaceName, sizeof(request.ifr_name));
    request.ifr_name[sizeof(request.ifr_name) - 1] = '\0';

    char* myaddr = strdup(addr);
    char* pch = NULL;
    pch = strchr(myaddr, '/');
    if (pch) {
        *pch = '\0';
        pch++;
        int subnet_mask = atoi(pch);
        sin = (struct sockaddr_in*)&request.ifr_addr;
        sin->sin_family = AF_INET;
        sin->sin_addr.s_addr = htonl(((uint32_t)(-1)) << (32 - subnet_mask));
        if (ioctl(sock, SIOCSIFNETMASK, &request) < 0) {
            RLOGE("%s: failed.", __func__);
        }
    }

    sin = (struct sockaddr_in*)&request.ifr_addr;
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = inet_addr(myaddr);
    if (ioctl(sock, SIOCSIFADDR, &request) < 0) {
        RLOGE("%s: failed.", __func__);
    }

    close(sock);
    free(myaddr);
    RLOGD("%s %d done.", __func__, __LINE__);
}

static void set_Gw_Addr(const char* gw, const char* radioInterfaceName)
{
    RLOGD("%s %d setting gateway addr %s on interface %s", __func__, __LINE__, gw,
        radioInterfaceName);
    struct ifreq request;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock == -1) {
        RLOGE("Failed to open interface socket: %s (%d)", strerror(errno), errno);
        return;
    }

    memset(&request, 0, sizeof(request));
    strncpy(request.ifr_name, radioInterfaceName, sizeof(request.ifr_name));
    request.ifr_name[sizeof(request.ifr_name) - 1] = '\0';

    struct sockaddr_in* sin = (struct sockaddr_in*)&request.ifr_addr;
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = inet_addr(gw);
    if (ioctl(sock, SIOCSIFDSTADDR, &request) < 0) {
        RLOGE("%s: failed.", __func__);
    }

    close(sock);
    RLOGD("%s %d done.", __func__, __LINE__);
}

static void clearNetworkConfig(const char* interfaceName)
{
    struct ifreq request;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock == -1) {
        RLOGE("Failed to open interface socket: %s (%d)",
            strerror(errno), errno);
        return;
    }

    memset(&request, 0, sizeof(request));
    strncpy(request.ifr_name, interfaceName, sizeof(request.ifr_name));

    if (ioctl(sock, SIOCSIFADDR, &request) < 0) {
        RLOGE("Fail to clear IP addr");
    }

    if (ioctl(sock, SIOCSIFNETMASK, &request) < 0) {
        RLOGE("Fail to clear netmask");
    }

    if (ioctl(sock, SIOCSIFDSTADDR, &request) < 0) {
        RLOGE("Fail to clear gateway");
    }

    close(sock);
}

static RIL_Errno setInterfaceState(const char* interfaceName, enum InterfaceState state)
{
    struct ifreq request;
    int status = 0;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock == -1) {
        RLOGE("Failed to open interface socket: %s (%d)", strerror(errno), errno);
        return RIL_E_GENERIC_FAILURE;
    }

    memset(&request, 0, sizeof(request));
    strncpy(request.ifr_name, interfaceName, sizeof(request.ifr_name));
    request.ifr_name[sizeof(request.ifr_name) - 1] = '\0';
    status = ioctl(sock, SIOCGIFFLAGS, &request);
    if (status != 0) {
        RLOGE("Failed to get interface flags for %s: %s (%d)",
            interfaceName, strerror(errno), errno);
        close(sock);
        return RIL_E_RADIO_NOT_AVAILABLE;
    }

    bool isUp = (request.ifr_flags & IFF_UP);
    if ((state == kInterfaceUp && isUp) || (state == kInterfaceDown && !isUp)) {
        // Interface already in desired state
        close(sock);
        return RIL_E_SUCCESS;
    }

    if (state == kInterfaceDown) {
        request.ifr_flags &= ~(IFF_UP | IFF_RUNNING);
    } else
        request.ifr_flags |= IFF_UP;

    status = ioctl(sock, SIOCSIFFLAGS, &request);
    if (status != 0) {
        RLOGE("Failed to set interface flags for %s: %s (%d)",
            interfaceName, strerror(errno), errno);
        close(sock);
        return RIL_E_GENERIC_FAILURE;
    }

    close(sock);
    return RIL_E_SUCCESS;
}

void onDataCallListChanged(void* param)
{
    (void)param;
    requestOrSendDataCallList(-1, NULL);
}

static void requestDataCallList(void* data, size_t datalen, RIL_Token t)
{
    (void)data;
    (void)datalen;
    requestOrSendDataCallList(-1, &t);
}

static const char* getRadioInterfaceName(void)
{
    if (isInEmulator()) {
        return EMULATOR_RADIO_INTERFACE;
    }

    return PPP_TTY_PATH_ETH0;
}

static void requestOrSendDataCallList(int cid, RIL_Token* t)
{
    ATResponse* p_response = NULL;
    ATLine* p_cur = NULL;
    int err = -1;
    int n = 0;
    char* out = NULL;
    const char* radioInterfaceName = getRadioInterfaceName();
    RIL_Errno ril_err = RIL_E_SUCCESS;

    err = at_send_command_multiline("AT+CGACT?", "+CGACT:", &p_response);
    if (err != AT_ERROR_OK || !p_response || p_response->success != AT_OK) {
        if (t != NULL) {
            RLOGE("Failure occurred in sending %s due to: %s", "AT+CGACT?", at_io_err_str(err));
            RIL_onRequestComplete(*t, RIL_E_GENERIC_FAILURE, NULL, 0);
        } else
            RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
                NULL, 0);
        return;
    }

    for (p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next)
        n++;

    RIL_Data_Call_Response_v11* responses = alloca(n * sizeof(RIL_Data_Call_Response_v11));

    int i;
    for (i = 0; i < n; i++) {
        responses[i].status = -1;
        responses[i].suggestedRetryTime = -1;
        responses[i].cid = -1;
        responses[i].active = -1;
        responses[i].type = "";
        responses[i].ifname = "";
        responses[i].addresses = "";
        responses[i].dnses = "";
        responses[i].gateways = "";
        responses[i].pcscf = "";
        responses[i].mtu = 0;
    }

    RIL_Data_Call_Response_v11* response = responses;
    for (p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next) {
        char* line = p_cur->line;

        err = at_tok_start(&line);
        if (err < 0) {
            RLOGE("Failed to parse line in %s", __func__);
            ril_err = RIL_E_GENERIC_FAILURE;
            goto error;
        }

        err = at_tok_nextint(&line, &response->cid);
        if (err < 0) {
            RLOGE("Failed to parse cid in %s", __func__);
            ril_err = RIL_E_GENERIC_FAILURE;
            goto error;
        }

        err = at_tok_nextint(&line, &response->active);
        if (err < 0) {
            RLOGE("Failed to parse active in %s", __func__);
            ril_err = RIL_E_GENERIC_FAILURE;
            goto error;
        }

        response++;
    }

    at_response_free(p_response);

    err = at_send_command_multiline("AT+CGDCONT?", "+CGDCONT:", &p_response);
    if (err != AT_ERROR_OK || !p_response || p_response->success != AT_OK) {
        if (t != NULL) {
            RLOGE("Failure occurred in sending %s due to: %s", "AT+CGDCONT?", at_io_err_str(err));
            RIL_onRequestComplete(*t, RIL_E_GENERIC_FAILURE, NULL, 0);
        } else
            RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
                NULL, 0);
        return;
    }

    for (p_cur = p_response->p_intermediates; p_cur != NULL;
         p_cur = p_cur->p_next) {
        char* line = p_cur->line;
        int ncid;

        err = at_tok_start(&line);
        if (err < 0) {
            RLOGE("Failed to parse line in %s", __func__);
            ril_err = RIL_E_GENERIC_FAILURE;
            goto error;
        }

        err = at_tok_nextint(&line, &ncid);
        if (err < 0) {
            RLOGE("Failed to parse line in %s", __func__);
            ril_err = RIL_E_GENERIC_FAILURE;
            goto error;
        }

        if (cid != ncid)
            continue;

        i = ncid - 1;
        // Assume no error
        responses[i].status = 0;

        // type
        err = at_tok_nextstr(&line, &out);
        if (err < 0) {
            RLOGE("Failed to parse type in %s", __func__);
            ril_err = RIL_E_GENERIC_FAILURE;
            goto error;
        }

        int type_size = strlen(out) + 1;
        responses[i].type = alloca(type_size);
        strlcpy(responses[i].type, out, type_size);

        // APN ignored for v5
        err = at_tok_nextstr(&line, &out);
        if (err < 0) {
            RLOGE("Failed to parse apn in %s", __func__);
            ril_err = RIL_E_GENERIC_FAILURE;
            goto error;
        }

        int ifname_size = strlen(radioInterfaceName) + 1;
        responses[i].ifname = alloca(ifname_size);
        strlcpy(responses[i].ifname, radioInterfaceName, ifname_size);

        err = at_tok_nextstr(&line, &out);
        if (err < 0) {
            RLOGE("Failed to parse address in %s", __func__);
            ril_err = RIL_E_GENERIC_FAILURE;
            goto error;
        }

        int addresses_size = strlen(out) + 1;
        responses[i].addresses = alloca(addresses_size);
        strlcpy(responses[i].addresses, out, addresses_size);
        set_Ip_Addr(responses[i].addresses, radioInterfaceName);

        responses[i].dnses = "8.8.8.8 8.8.4.4";
        responses[i].gateways = "0.0.0.0";
    }

    // If cid = -1, return the data call list without processing CGCONTRDP (setupDataCall)
    if (cid == -1) {
        if (t != NULL) {
            if (n != 0)
                RIL_onRequestComplete(*t, RIL_E_SUCCESS, &responses[0], sizeof(RIL_Data_Call_Response_v11));
            else
                RIL_onRequestComplete(*t, RIL_E_SUCCESS, NULL, 0);
        } else
            RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED, responses,
                n * sizeof(RIL_Data_Call_Response_v11));

        clearNetworkConfig(radioInterfaceName);
        at_response_free(p_response);
        p_response = NULL;
        return;
    }

    at_response_free(p_response);
    p_response = NULL;

    char* cmd = NULL;

    if (asprintf(&cmd, "AT+CGCONTRDP=%d", cid) < 0) {
        RLOGE("Failed to allocate memory");
        ril_err = RIL_E_NO_MEMORY;
        goto error;
    }

    err = at_send_command_singleline(cmd, "+CGCONTRDP:", &p_response);
    if (err != AT_ERROR_OK || !p_response || p_response->success != AT_OK) {
        RLOGE("Failure occurred in sending %s due to: %s", cmd, at_io_err_str(err));
        ril_err = RIL_E_GENERIC_FAILURE;
        goto error;
    }

    int skip = 0;
    char* sskip = NULL;
    char* input = p_response->p_intermediates->line;

    int ncid = -1;
    err = at_tok_start(&input);
    if (err < 0) {
        RLOGE("Failed to parse line in %s", __func__);
        ril_err = RIL_E_GENERIC_FAILURE;
        goto error;
    }

    err = at_tok_nextint(&input, &ncid); // cid
    if (err < 0) {
        RLOGE("Failed to parse ncid in %s", __func__);
        ril_err = RIL_E_GENERIC_FAILURE;
        goto error;
    }

    if (cid != ncid) {
        RLOGE("cid is error");
        ril_err = RIL_E_GENERIC_FAILURE;
        goto error;
    }

    i = ncid - 1;

    err = at_tok_nextint(&input, &skip); // bearer_id
    if (err < 0) {
        RLOGE("Failed to parse bearer_id in %s", __func__);
        ril_err = RIL_E_GENERIC_FAILURE;
        goto error;
    }

    err = at_tok_nextstr(&input, &sskip); // apn
    if (err < 0) {
        RLOGE("Failed to parse apn in %s", __func__);
        ril_err = RIL_E_GENERIC_FAILURE;
        goto error;
    }

    err = at_tok_nextstr(&input, &sskip); // local_addr_and_subnet_mask
    if (err < 0) {
        RLOGE("Failed to parse local_addr_and_subnet_mask in %s", __func__);
        ril_err = RIL_E_GENERIC_FAILURE;
        goto error;
    }

    err = at_tok_nextstr(&input, &responses[i].gateways); // gw_addr
    if (err < 0) {
        RLOGE("Failed to parse gw_addr in %s", __func__);
        ril_err = RIL_E_GENERIC_FAILURE;
        goto error;
    }

    set_Gw_Addr(responses[i].gateways, radioInterfaceName);
    err = at_tok_nextstr(&input, &responses[i].dnses); // dns_prim_addr
    if (err < 0) {
        RLOGE("Failed to parse dns in %s", __func__);
        ril_err = RIL_E_GENERIC_FAILURE;
        goto error;
    }

    if (t != NULL)
        RIL_onRequestComplete(*t, ril_err, &responses[i],
            sizeof(RIL_Data_Call_Response_v11));
    else
        RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED,
            responses, n * sizeof(RIL_Data_Call_Response_v11));

    at_response_free(p_response);
    free(cmd);

    return;

error:
    if (t != NULL)
        RIL_onRequestComplete(*t, ril_err, NULL, 0);
    else
        RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED, NULL, 0);

    at_response_free(p_response);
    free(cmd);
}

static void putPDP(int cid)
{
    if (cid < 1 || cid > MAX_PDP) {
        return;
    }

    s_PDP[cid - 1].state = PDP_IDLE;
}

#define REG_DATA_STATE_LEN 14
static void requestDataRegistrationState(void* data, size_t datalen, RIL_Token t)
{
    (void)data;
    (void)datalen;

    int err;
    int* registration = NULL;
    char** responseStr = NULL;
    ATResponse* p_response = NULL;
    const char* cmd;
    const char* prefix;
    char* line;
    int i = 0, j, numElements = 0;
    int count = 3;
    int type = 0;
    int startfrom = 0;
    RIL_Errno ril_err = RIL_E_SUCCESS;

    cmd = "AT+CGREG?";
    prefix = "+CGREG:";
    numElements = REG_DATA_STATE_LEN;
    if (TECH_BIT(getModemInfo()) == MDM_LTE) {
        cmd = "AT+CEREG?";
        prefix = "+CEREG:";
    }

    err = at_send_command_singleline(cmd, prefix, &p_response);

    if (err != AT_ERROR_OK || !p_response->success || p_response->success != AT_OK) {
        RLOGE("Failure occurred in sending %s due to: %s", cmd, at_io_err_str(err));
        ril_err = RIL_E_GENERIC_FAILURE;
        goto error;
    }

    line = p_response->p_intermediates->line;

    if (parseRegistrationState(line, &type, &count, &registration)) {
        RLOGE("Failure to parse registration state");
        ril_err = RIL_E_GENERIC_FAILURE;
        goto error;
    }

    responseStr = malloc(numElements * sizeof(char*));
    if (!responseStr) {
        RLOGE("Failed to allocate memory");
        ril_err = RIL_E_NO_MEMORY;
        goto error;
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
        // Available data radio technology
        if (asprintf(&responseStr[3], "8") < 0) {
            RLOGE("Failed to allocate memory");
            ril_err = RIL_E_NO_MEMORY;
            goto error;
        }
    } else { // type == RADIO_TECH_3GPP
        RLOGD("registration state type: 3GPP");
        startfrom = 0;

        if (count > 1) {
            if (asprintf(&responseStr[1], "%x", registration[1]) < 0) {
                RLOGE("Failed to allocate memory");
                ril_err = RIL_E_NO_MEMORY;
                goto error;
            }
        }
        if (count > 2) {
            if (asprintf(&responseStr[2], "%x", registration[2]) < 0) {
                RLOGE("Failed to allocate memory");
                ril_err = RIL_E_NO_MEMORY;
                goto error;
            }
        }
        if (count > 3) {
            if (asprintf(&responseStr[3], "%d", mapNetworkRegistrationResponse(registration[3])) < 0) {
                RLOGE("Failed to allocate memory");
                ril_err = RIL_E_NO_MEMORY;
                goto error;
            }
        }
    }

    if (asprintf(&responseStr[0], "%d", registration[0]) < 0) {
        RLOGE("Failed to allocate memory");
        ril_err = RIL_E_NO_MEMORY;
        goto error;
    }

    /**
     * Optional bytes for DATA_REGISTRATION_STATE request
     * 4th byte : Registration denial code
     * 5th byte : The max. number of simultaneous Data Calls
     */
    // asprintf(&responseStr[4], "3");
    // asprintf(&responseStr[5], "1");
    if (asprintf(&responseStr[11], "%d", getMcc()) < 0) {
        RLOGE("Failed to allocate memory");
        ril_err = RIL_E_NO_MEMORY;
        goto error;
    }

    if (asprintf(&responseStr[12], "%d", getMnc()) < 0) {
        RLOGE("Failed to allocate memory");
        ril_err = RIL_E_NO_MEMORY;
        goto error;
    }

    if (getMncLength() == 2) {
        if (asprintf(&responseStr[13], "%03d%02d", getMcc(), getMnc()) < 0) {
            RLOGE("Failed to allocate memory");
            ril_err = RIL_E_NO_MEMORY;
            goto error;
        }
    } else {
        if (asprintf(&responseStr[13], "%03d%03d", getMcc(), getMnc()) < 0) {
            RLOGE("Failed to allocate memory");
            ril_err = RIL_E_NO_MEMORY;
            goto error;
        }
    }

    for (j = startfrom; j < numElements; j++) {
        if (!responseStr[i]) {
            RLOGE("Failed to allocate memory");
            ril_err = RIL_E_NO_MEMORY;
            goto error;
        }
    }

    free(registration);
    registration = NULL;
    RIL_onRequestComplete(t, ril_err, responseStr, numElements * sizeof(char*));

    for (j = 0; j < numElements; j++) {
        free(responseStr[j]);
        responseStr[j] = NULL;
    }

    free(responseStr);
    responseStr = NULL;
    at_response_free(p_response);

    return;
error:
    if (responseStr) {
        for (j = 0; j < numElements; j++) {
            free(responseStr[j]);
            responseStr[j] = NULL;
        }

        free(responseStr);
        responseStr = NULL;
    }

    if (registration != NULL) {
        free(registration);
        registration = NULL;
    }

    RLOGE("requestDataRegistrationState must never return an error when radio is on");
    RIL_onRequestComplete(t, ril_err, NULL, 0);
    at_response_free(p_response);
}

static int getPDP(void)
{
    int ret = -1;

    for (int i = 0; i < MAX_PDP; i++) {
        if (s_PDP[i].state == PDP_IDLE) {
            s_PDP[i].state = PDP_BUSY;
            ret = s_PDP[i].cid;
            break;
        }
    }

    return ret;
}

static void requestSetupDataCall(void* data, size_t datalen, RIL_Token t)
{
    const char* apn = NULL;
    char* cmd = NULL;
    int err = -1;
    int cid = -1;
    ATResponse* p_response = NULL;
    RIL_Errno ril_err = RIL_E_SUCCESS;

    if (data == NULL) {
        RLOGE("requestSetupDataCall data is null!");
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    apn = ((const char**)data)[2];

    int fd, qmistatus;
    size_t cur = 0;
    size_t len;
    ssize_t written, rlen;
    char status[32] = { 0 };
    int retry = 10;
    const char* pdp_type;

    RLOGD("requesting data connection to APN '%s'", apn);

    fd = open("/dev/qmi", O_RDWR);
    if (fd >= 0) { /* the device doesn't exist on the emulator */

        RLOGD("opened the qmi device\n");
        if (asprintf(&cmd, "up:%s", apn) < 0) {
            RLOGE("Failed to allocate memory");
            ril_err = RIL_E_NO_MEMORY;
            goto error;
        }

        len = strlen(cmd);

        while (cur < len) {
            do {
                written = write(fd, cmd + cur, len - cur);
            } while (written < 0 && errno == EINTR);

            if (written < 0) {
                RLOGE("### ERROR writing to /dev/qmi");
                ril_err = RIL_E_GENERIC_FAILURE;
                close(fd);
                goto error;
            }

            cur += written;
        }

        // wait for interface to come online

        do {
            sleep(1);
            do {
                rlen = read(fd, status, 31);
            } while (rlen < 0 && errno == EINTR);

            if (rlen < 0) {
                RLOGE("### ERROR reading from /dev/qmi");
                ril_err = RIL_E_GENERIC_FAILURE;
                close(fd);
                goto error;
            } else {
                status[rlen] = '\0';
                RLOGD("### status: %s", status);
            }
        } while (strncmp(status, "STATE=up", 8) && strcmp(status, "online") && --retry);

        close(fd);

        if (retry == 0) {
            RLOGE("### Failed to get data connection up\n");
            ril_err = RIL_E_GENERIC_FAILURE;
            goto error;
        }

        qmistatus = system("netcfg rmnet0 dhcp");

        RLOGD("netcfg rmnet0 dhcp: status %d\n", qmistatus);

        if (qmistatus < 0) {
            RLOGE("qmistatus < 0");
            ril_err = RIL_E_GENERIC_FAILURE;
            goto error;
        }

    } else {
        const char* radioInterfaceName = getRadioInterfaceName();
        if (setInterfaceState(radioInterfaceName, kInterfaceUp) != RIL_E_SUCCESS) {
            RLOGE("set network interface state error");
            ril_err = RIL_E_GENERIC_FAILURE;
            goto error;
        }

        if (datalen > 6 * sizeof(char*)) {
            pdp_type = ((const char**)data)[6];
        } else {
            pdp_type = "IP";
        }

        cid = getPDP();
        if (cid < 1) {
            RLOGE("SETUP_DATA_CALL MAX_PDP reached.");
            RIL_Data_Call_Response_v11 response;
            response.status = 0x41 /* PDP_FAIL_MAX_ACTIVE_PDP_CONTEXT_REACHED */;
            response.suggestedRetryTime = -1;
            response.cid = cid;
            response.active = -1;
            response.type = "";
            response.ifname = "";
            response.addresses = "";
            response.dnses = "";
            response.gateways = "";
            response.pcscf = "";
            response.mtu = 0;
            RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(RIL_Data_Call_Response_v11));
            return;
        }

        if (asprintf(&cmd, "AT+CGDCONT=%d,\"%s\",\"%s\",,0,0", cid, pdp_type, apn) < 0) {
            RLOGE("Failed to allocate memory");
            ril_err = RIL_E_NO_MEMORY;
            goto error;
        }

        // FIXME check for error here
        err = at_send_command(cmd, &p_response);
        if (err != AT_ERROR_OK || !p_response || p_response->success != AT_OK) {
            RLOGE("Failure occurred in sending %s due to: %s", cmd, at_io_err_str(err));
            ril_err = RIL_E_GENERIC_FAILURE;
            goto error;
        }

        at_response_free(p_response);
        p_response = NULL;

        // Set required QoS params to default
        err = at_send_command("AT+CGQREQ=1", &p_response);
        if (err != AT_ERROR_OK || !p_response || p_response->success != AT_OK) {
            RLOGE("Failure occurred in sending AT+CGQREQ=1 due to: %s", at_io_err_str(err));
            ril_err = RIL_E_GENERIC_FAILURE;
            goto error;
        }

        at_response_free(p_response);
        p_response = NULL;

        // Set minimum QoS params to default
        err = at_send_command("AT+CGQMIN=1", &p_response);
        if (err != AT_ERROR_OK || !p_response || p_response->success != AT_OK) {
            RLOGE("Failure occurred in sending AT+CGQMIN=1 due to: %s", at_io_err_str(err));
            ril_err = RIL_E_GENERIC_FAILURE;
            goto error;
        }

        at_response_free(p_response);
        p_response = NULL;

        // packet-domain event reporting
        err = at_send_command("AT+CGEREP=1,0", &p_response);
        if (err != AT_ERROR_OK || !p_response || p_response->success != AT_OK) {
            RLOGE("Failure occurred in sending AT+CGEREP=1,0 due to: %s", at_io_err_str(err));
            ril_err = RIL_E_GENERIC_FAILURE;
            goto error;
        }

        at_response_free(p_response);
        p_response = NULL;

        // Hangup anything that's happening there now
        err = at_send_command("AT+CGACT=1,0", &p_response);
        if (err != AT_ERROR_OK || !p_response || p_response->success != AT_OK) {
            RLOGE("Failure occurred in sending AT+CGACT=1,0 due to: %s", at_io_err_str(err));
            ril_err = RIL_E_GENERIC_FAILURE;
            goto error;
        }

        at_response_free(p_response);
        p_response = NULL;

        // Start data on PDP context 1
        err = at_send_command("ATD*99***1#", &p_response);
        if (err != AT_ERROR_OK || !p_response || p_response->success != AT_OK) {
            RLOGE("Failure occurred in sending ATD*99***1# due to: %s", at_io_err_str(err));
            ril_err = RIL_E_GENERIC_FAILURE;
            goto error;
        }
    }

    requestOrSendDataCallList(cid, &t);
    at_response_free(p_response);
    free(cmd);

    return;
error:
    RIL_onRequestComplete(t, ril_err, NULL, 0);
    at_response_free(p_response);
    free(cmd);
}

static void requestDeactivateDataCall(void* data, size_t datalen, RIL_Token t)
{
    (void)datalen;

    if (data == NULL) {
        RLOGE("requestDeactivateDataCall data is null");
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    const char* p_cid = ((const char**)data)[0];
    int cid = p_cid ? atoi(p_cid) : -1;
    RIL_Errno rilErrno = RIL_E_GENERIC_FAILURE;
    if (cid < 1 || cid > MAX_PDP) {
        RLOGE("Invalid cid found");
        RIL_onRequestComplete(t, rilErrno, NULL, 0);
        return;
    }

    const char* radioInterfaceName = getRadioInterfaceName();
    rilErrno = setInterfaceState(radioInterfaceName, kInterfaceDown);
    RIL_onRequestComplete(t, rilErrno, NULL, 0);
    putPDP(cid);
    requestOrSendDataCallList(-1, NULL);
}

void on_request_data(int request, void* data, size_t datalen, RIL_Token t)
{
    switch (request) {
    case RIL_REQUEST_DATA_REGISTRATION_STATE:
        requestDataRegistrationState(data, datalen, t);
        break;
    case RIL_REQUEST_SETUP_DATA_CALL:
        requestSetupDataCall(data, datalen, t);
        break;
    case RIL_REQUEST_DEACTIVATE_DATA_CALL:
        requestDeactivateDataCall(data, datalen, t);
        break;
    case RIL_REQUEST_DATA_CALL_LIST:
        requestDataCallList(data, datalen, t);
        break;
    case RIL_REQUEST_SET_INITIAL_ATTACH_APN:
    case RIL_REQUEST_ALLOW_DATA:
    case RIL_REQUEST_SET_DATA_PROFILE:
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
        break;
    default:
        RLOGE("Request not supported");
        RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
        break;
    }

    RLOGD("On request data end");
}

bool try_handle_unsol_data(const char* s)
{
    int ret = false;

    RLOGD("unsol data string: %s", s);

    if (strStartsWith(s, "+CGEV:")) {
        RLOGI("Receive data call list changed URC");
        /* Really, we can ignore NW CLASS and ME CLASS events here,
         * but right now we don't since extranous
         * RIL_UNSOL_DATA_CALL_LIST_CHANGED calls are tolerated
         */
        /* can't issue AT commands here -- call on main thread */
        RIL_requestTimedCallback(onDataCallListChanged, NULL, NULL);
        ret = true;
    } else {
        RLOGD("Can't match any unsol data handlers");
    }

    return ret;
}