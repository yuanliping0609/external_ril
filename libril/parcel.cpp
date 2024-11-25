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

#include <algorithm>
#include <atomic>
#include <endian.h>
#include <limits>
#include <stdlib.h>
#include <string.h>
#include <type_traits>

#include "parcel.h"

// This macro should never be used at runtime, as a too large value
// of s could cause an integer overflow. Instead, you should always
// use the wrapper function pad_size()
#define PAD_SIZE_UNSAFE(s) (((s) + 3) & ~3UL)

static size_t pad_size(size_t s)
{
    if (s > (std::numeric_limits<size_t>::max() - 3)) {
        abort();
    }

    return PAD_SIZE_UNSAFE(s);
}

static std::atomic<size_t> gParcelGlobalAllocCount;
static std::atomic<size_t> gParcelGlobalAllocSize;

Parcel::Parcel()
{
    initState();
}

Parcel::~Parcel()
{
    freeDataNoInit();
}

const uint8_t* Parcel::data() const
{
    return mData;
}

size_t Parcel::dataSize() const
{
    return (mDataSize > mDataPos ? mDataSize : mDataPos);
}

size_t Parcel::dataAvail() const
{
    size_t result = dataSize() - dataPosition();
    if (result > INT32_MAX) {
        abort();
    }

    return result;
}

size_t Parcel::dataPosition() const
{
    return mDataPos;
}

size_t Parcel::dataCapacity() const
{
    return mDataCapacity;
}

void Parcel::setDataPosition(size_t pos) const
{
    if (pos > INT32_MAX) {
        // don't accept size_t values which may have come from an
        // inadvertent conversion from a negative int.
        abort();
    }

    mDataPos = pos;
}

status_t Parcel::setDataCapacity(size_t size)
{
    if (size > INT32_MAX) {
        // don't accept size_t values which may have come from an
        // inadvertent conversion from a negative int.
        return BAD_VALUE;
    }

    if (size > mDataCapacity)
        return continueWrite(size);

    return NO_ERROR;
}

status_t Parcel::setData(const uint8_t* buffer, size_t len)
{
    if (len > INT32_MAX) {
        // don't accept size_t values which may have come from an
        // inadvertent conversion from a negative int.
        return BAD_VALUE;
    }

    status_t err = restartWrite(len);
    if (err == NO_ERROR) {
        memcpy(const_cast<uint8_t*>(data()), buffer, len);
        mDataSize = len;
    }

    return err;
}

status_t Parcel::appendFrom(const Parcel* parcel, size_t offset, size_t len)
{
    status_t err;
    const uint8_t* data = parcel->mData;

    if (len == 0) {
        return NO_ERROR;
    }

    if (len > INT32_MAX) {
        // don't accept size_t values which may have come from an
        // inadvertent conversion from a negative int.
        return BAD_VALUE;
    }

    // range checks against the source parcel size
    if ((offset > parcel->mDataSize) || (len > parcel->mDataSize)
        || (offset + len > parcel->mDataSize)) {
        return BAD_VALUE;
    }

    if ((mDataSize + len) > mDataCapacity) {
        // grow data
        err = growData(len);
        if (err != NO_ERROR) {
            return err;
        }
    }

    // append data
    memcpy(mData + mDataPos, data + offset, len);
    mDataPos += len;
    mDataSize += len;

    return NO_ERROR;
}

status_t Parcel::finishWrite(size_t len)
{
    if (len > INT32_MAX) {
        // don't accept size_t values which may have come from an
        // inadvertent conversion from a negative int.
        return BAD_VALUE;
    }

    mDataPos += len;
    if (mDataPos > mDataSize) {
        mDataSize = mDataPos;
    }

    return NO_ERROR;
}

status_t Parcel::write(const void* data, size_t len)
{
    if (len > INT32_MAX) {
        // don't accept size_t values which may have come from an
        // inadvertent conversion from a negative int.
        return BAD_VALUE;
    }

    void* const d = writeInplace(len);
    if (d) {
        memcpy(d, data, len);
        return NO_ERROR;
    }

    return mError;
}

void* Parcel::writeInplace(size_t len)
{
    if (len > INT32_MAX) {
        // don't accept size_t values which may have come from an
        // inadvertent conversion from a negative int.
        return nullptr;
    }

    const size_t padded = pad_size(len);

    // check for integer overflow
    if (mDataPos + padded < mDataPos) {
        return nullptr;
    }

    if ((mDataPos + padded) <= mDataCapacity) {
    restart_write:
        uint8_t* const data = mData + mDataPos;

        // Need to pad at end?
        if (padded != len) {
#if BYTE_ORDER == BIG_ENDIAN
            static const uint32_t mask[4] = { 0x00000000, 0xffffff00, 0xffff0000, 0xff000000 };
#endif
#if BYTE_ORDER == LITTLE_ENDIAN
            static const uint32_t mask[4] = { 0x00000000, 0x00ffffff, 0x0000ffff, 0x000000ff };
#endif
            *reinterpret_cast<uint32_t*>(data + padded - 4) &= mask[padded - len];
        }

        finishWrite(padded);
        return data;
    }

    status_t err = growData(padded);
    if (err == NO_ERROR)
        goto restart_write;

    return nullptr;
}

status_t Parcel::writeInt32(int32_t val)
{
    return writeAligned(val);
}

status_t Parcel::writeInt64(int64_t val)
{
    return writeAligned(val);
}

status_t Parcel::writeString16(const char16_t* str, size_t len)
{
    if (str == nullptr)
        return writeInt32(-1);

    // NOTE: Keep this logic in sync with android_os_Parcel.cpp
    status_t err = writeInt32(len);
    if (err == NO_ERROR) {
        len *= sizeof(char16_t);
        uint8_t* data = (uint8_t*)writeInplace(len + sizeof(char16_t));
        if (data) {
            memcpy(data, str, len);
            *reinterpret_cast<char16_t*>(data + len) = 0;
            return NO_ERROR;
        }
        err = mError;
    }

    return err;
}

status_t Parcel::read(void* outData, size_t len) const
{
    if (len > INT32_MAX) {
        // don't accept size_t values which may have come from an
        // inadvertent conversion from a negative int.
        return BAD_VALUE;
    }

    if ((mDataPos + pad_size(len)) >= mDataPos && (mDataPos + pad_size(len)) <= mDataSize
        && len <= pad_size(len)) {
        memcpy(outData, mData + mDataPos, len);
        // Still increment the data position by the expected length
        mDataPos += pad_size(len);
        return NO_ERROR;
    }

    return NOT_ENOUGH_DATA;
}

const void* Parcel::readInplace(size_t len) const
{
    if (len > INT32_MAX) {
        // don't accept size_t values which may have come from an
        // inadvertent conversion from a negative int.
        return nullptr;
    }

    if ((mDataPos + pad_size(len)) >= mDataPos && (mDataPos + pad_size(len)) <= mDataSize
        && len <= pad_size(len)) {
        const void* data = mData + mDataPos;
        // Still increment the data position by the expected length
        mDataPos += pad_size(len);
        return data;
    }

    return nullptr;
}

template <class T>
status_t Parcel::readAligned(T* pArg) const
{
    static_assert(PAD_SIZE_UNSAFE(sizeof(T)) == sizeof(T));
    static_assert(std::is_trivially_copyable_v<T>);

    if ((mDataPos + sizeof(T)) <= mDataSize) {
        const void* data = mData + mDataPos;
        // Still increment the data position by the expected length
        mDataPos += sizeof(T);
        *pArg = *reinterpret_cast<const T*>(data);
        return NO_ERROR;
    } else {
        return NOT_ENOUGH_DATA;
    }
}

template <class T>
T Parcel::readAligned() const
{
    T result;
    if (readAligned(&result) != NO_ERROR) {
        result = 0;
    }

    return result;
}

template <class T>
status_t Parcel::writeAligned(T val)
{
    static_assert(PAD_SIZE_UNSAFE(sizeof(T)) == sizeof(T));
    static_assert(std::is_trivially_copyable_v<T>);

    if ((mDataPos + sizeof(val)) <= mDataCapacity) {
    restart_write:
        *reinterpret_cast<T*>(mData + mDataPos) = val;
        return finishWrite(sizeof(val));
    }

    status_t err = growData(sizeof(val));
    if (err == NO_ERROR)
        goto restart_write;

    return err;
}

status_t Parcel::readInt32(int32_t* pArg) const
{
    return readAligned(pArg);
}

int32_t Parcel::readInt32() const
{
    return readAligned<int32_t>();
}

const char16_t* Parcel::readString16Inplace(size_t* outLen) const
{
    int32_t size = readInt32();
    // watch for potential int overflow from size + 1
    if (size >= 0 && size < INT32_MAX) {
        *outLen = size;
        const char16_t* str = (const char16_t*)readInplace((size + 1) * sizeof(char16_t));
        if (str != nullptr) {
            if (str[size] == u'\0') {
                return str;
            }
        }
    }
    *outLen = 0;

    return nullptr;
}

void Parcel::freeData()
{
    freeDataNoInit();
    initState();
}

void Parcel::freeDataNoInit()
{
    if (mData) {
        gParcelGlobalAllocSize -= mDataCapacity;
        gParcelGlobalAllocCount--;
        free(mData);
    }
}

status_t Parcel::growData(size_t len)
{
    if (len > INT32_MAX) {
        // don't accept size_t values which may have come from an
        // inadvertent conversion from a negative int.
        return BAD_VALUE;
    }

    if (len > SIZE_MAX - mDataSize)
        return NO_MEMORY; // overflow

    if (mDataSize + len > SIZE_MAX / 3)
        return NO_MEMORY; // overflow

    size_t newSize = ((mDataSize + len) * 3) / 2;

    return (newSize <= mDataSize) ? (status_t)NO_MEMORY : continueWrite(std::max(newSize, (size_t)128));
}

status_t Parcel::restartWrite(size_t desired)
{
    if (desired > INT32_MAX) {
        // don't accept size_t values which may have come from an
        // inadvertent conversion from a negative int.
        return BAD_VALUE;
    }

    uint8_t* data = (uint8_t*)realloc(mData, desired);
    if (!data && desired > mDataCapacity) {
        mError = NO_MEMORY;
        return NO_MEMORY;
    }

    if (data || desired == 0) {
        if (mDataCapacity > desired) {
            gParcelGlobalAllocSize -= (mDataCapacity - desired);
        } else {
            gParcelGlobalAllocSize += (desired - mDataCapacity);
        }

        if (!mData) {
            gParcelGlobalAllocCount++;
        }

        mData = data;
        mDataCapacity = desired;
    }

    mDataSize = mDataPos = 0;

    return NO_ERROR;
}

status_t Parcel::continueWrite(size_t desired)
{
    if (desired > INT32_MAX) {
        // don't accept size_t values which may have come from an
        // inadvertent conversion from a negative int.
        return BAD_VALUE;
    }

    // If shrinking, first adjust for any objects that appear
    // after the new data size.
    if (mData) {
        // We own the data, so we can just do a realloc().
        if (desired > mDataCapacity) {
            uint8_t* data = (uint8_t*)realloc(mData, desired);
            if (data) {
                gParcelGlobalAllocSize += desired;
                gParcelGlobalAllocSize -= mDataCapacity;
                mData = data;
                mDataCapacity = desired;
            } else {
                mError = NO_MEMORY;
                return NO_MEMORY;
            }
        } else {
            if (mDataSize > desired) {
                mDataSize = desired;
            }
            if (mDataPos > desired) {
                mDataPos = desired;
            }
        }
    } else {
        // This is the first data. Easy!
        uint8_t* data = (uint8_t*)malloc(desired);
        if (!data) {
            mError = NO_MEMORY;
            return NO_MEMORY;
        }

        gParcelGlobalAllocSize += desired;
        gParcelGlobalAllocCount++;

        mData = data;
        mDataSize = mDataPos = 0;
        mDataCapacity = desired;
    }

    return NO_ERROR;
}

void Parcel::initState()
{
    mError = NO_ERROR;
    mData = nullptr;
    mDataSize = 0;
    mDataCapacity = 0;
    mDataPos = 0;
}