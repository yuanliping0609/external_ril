/* //device/libs/telephony/ril_second_commands.h
**
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
// none
{ 0, NULL, NULL },
    // 200
    { 201, NULL, NULL },
    { 202, NULL, NULL },
    { 203, NULL, NULL },
    { 204, NULL, NULL },
    { RIL_REQUEST_EMERGENCY_DIAL, dispatchDial, responseVoid },
    { 206, NULL, NULL },
    { 207, NULL, NULL },
    { RIL_REQUEST_ENABLE_UICC_APPLICATIONS, dispatchInts, responseVoid },
    { RIL_REQUEST_GET_UICC_APPLICATIONS_ENABLEMENT, dispatchVoid, responseInts },
