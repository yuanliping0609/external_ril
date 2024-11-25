/*
 * Copyright (C) 2005 The Android Open Source Project
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

#ifndef __PARCEL_H_
#define __PARCEL_H_

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

typedef int status_t;

#define NO_ERROR 0
#define NO_MEMORY (-ENOMEM)
#define BAD_VALUE (-EINVAL)
#define NOT_ENOUGH_DATA (-ENODATA)

class Parcel {
public:
    Parcel();
    ~Parcel();

    const uint8_t* data() const;
    size_t dataSize() const;
    size_t dataAvail() const;
    size_t dataPosition() const;
    size_t dataCapacity() const;

    status_t setDataSize(size_t size);
    void setDataPosition(size_t pos) const;
    status_t setDataCapacity(size_t size);
    status_t setData(const uint8_t* buffer, size_t len);
    status_t appendFrom(const Parcel* parcel, size_t start, size_t len);

    void freeData();

    status_t write(const void* data, size_t len);
    void* writeInplace(size_t len);
    status_t writeInt32(int32_t val);
    status_t writeInt64(int64_t val);
    status_t writeString16(const char16_t* str, size_t len);

    status_t read(void* outData, size_t len) const;
    const void* readInplace(size_t len) const;
    int32_t readInt32() const;
    status_t readInt32(int32_t* pArg) const;
    const char16_t* readString16Inplace(size_t* outLen) const;

    status_t finishWrite(size_t len);
    status_t growData(size_t len);
    status_t restartWrite(size_t desired);
    status_t continueWrite(size_t desired);

    template <class T>
    status_t readAligned(T* pArg) const;
    template <class T>
    T readAligned() const;
    template <class T>
    status_t writeAligned(T val);

private:
    status_t mError;
    uint8_t* mData;
    size_t mDataSize;
    size_t mDataCapacity;
    mutable size_t mDataPos;

    void freeDataNoInit();
    void initState();
};

#endif
