/*
 * Copyright (C) 2023 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef _RIL_LOG_H
#define _RIL_LOG_H

#include <stdlib.h>
#include <stdint.h>
#include <syslog.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef RIL_DEBUG
#define RLOGD(fmt, ...)     do { syslog(LOG_DEBUG, fmt, ##__VA_ARGS__); } while(0)
#define RLOGI(fmt, ...)     do { syslog(LOG_INFO, fmt, ##__VA_ARGS__); } while(0)
#define RLOGW(fmt, ...)     do { syslog(LOG_WARNING, fmt, ##__VA_ARGS__); } while(0)
#define RLOGE(fmt, ...)     do { syslog(LOG_ERR, fmt, ##__VA_ARGS__); } while(0)
#define ALOGE(fmt, ...)     do { syslog(LOG_ERR, fmt, ##__VA_ARGS__); } while(0)
#else
#define RLOGD(fmt, ...)
#define RLOGI(fmt, ...)
#define RLOGW(fmt, ...)
#define RLOGE(fmt, ...)
#define ALOGE(fmt, ...)
#endif

#ifdef __cplusplus
}
#endif

#endif /*_RIL_LOG_H*/
