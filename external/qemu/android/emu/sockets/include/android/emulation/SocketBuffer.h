/* Copyright 2021 The Android Open Source Project
**
** This software is licensed under the terms of the GNU General Public
** License version 2, as published by the Free Software Foundation, and
** may be copied, distributed, and modified under those terms.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
*/
#pragma once

#include <memory>
#include <stdint.h>
#include "aemu/base/files/Stream.h"

namespace android {
namespace emulation {

struct SocketBuffer {
    constexpr static size_t kLargeCapacityReleaseIfEmpty = size_t(4) << 20;  // 4M

    bool isEmpty() const { return mSize == 0; }

    size_t append(const void* const appendData, const size_t appendSize) {
        assert(mSize <= mCapacity);

        const size_t newSize = mSize + appendSize;
        if (newSize > mCapacity) {
            const size_t newCapacity = getCapacity(newSize);
            assert(newCapacity >= newSize);
            std::unique_ptr<char[]> newData = std::make_unique<char[]>(newCapacity);

            if (mSize > 0) {
                assert(mConsume < mCapacity);

                if ((mConsume + mSize) <= mCapacity) {
                    memcpy(&newData[0], &mData[mConsume], mSize);
                } else {
                    const size_t sz = mCapacity - mConsume;
                    memcpy(&newData[0], &mData[mConsume], sz);
                    memcpy(&newData[sz], &mData[0], mSize - sz);
                }
            }

            memcpy(&newData[mSize], appendData, appendSize);

            mData = std::move(newData);
            mCapacity = newCapacity;
            mProduce = newSize;
            mConsume = 0;
        } else if (newSize == 0) {
            // do nothing
        } else if ((mProduce + appendSize) <= mCapacity) {
            assert(mCapacity > 0);
            assert(mProduce < mCapacity);

            memcpy(&mData[mProduce], appendData, appendSize);
            mProduce = (mProduce + appendSize) % mCapacity;
        } else {
            assert(mCapacity > 0);
            assert(mProduce < mCapacity);

            const char* appendData8 = static_cast<const char*>(appendData);
            const size_t sz1 = mCapacity - mProduce;
            assert(appendSize > sz1);
            const size_t sz2 = appendSize - sz1;

            memcpy(&mData[mProduce], appendData8, sz1);
            memcpy(&mData[0], appendData8 + sz1, sz2);
            mProduce = sz2;
        }

        mSize = newSize;
        return newSize;
    }

    // Returns the contiguous portion of the buffer, it could be shorter than the whole buffer.
    std::pair<const void*, size_t> peek() const {
        assert(mSize <= mCapacity);
        if (mSize > 0) {
            assert(mConsume < mCapacity);
            return { &mData[mConsume], std::min(mSize, mCapacity - mConsume) };
        } else {
            return { nullptr, 0 };
        }
    }

    // Consumes the `size` bytes from the buffer.
    // It must the less or equal than the value returned by `peek`.
    void consume(const size_t size) {
        assert(mSize <= mCapacity);
        assert(size <= mSize);

        if (mCapacity) {
            if (mSize == size) {
                // make more contiguous space for the next `append`
                clear(mCapacity >= kLargeCapacityReleaseIfEmpty);
            } else {
                mSize -= size;
                mConsume = (mConsume + size) % mCapacity;
            }
        } else {
            assert(size == 0);
        }
    }

    void clear(const bool alsoFreeMemory = false) {
        mSize = 0;
        mProduce = 0;
        mConsume = 0;

        if (alsoFreeMemory) {
            mData.reset();
            mCapacity = 0;
        }
    }

    void save(base::Stream* stream) const {
        assert(mSize <= mCapacity);

        stream->putBe32(mSize);
        if (mSize) {
            assert(mConsume < mCapacity);

            if ((mConsume + mSize) <= mCapacity) {
                stream->write(&mData[mConsume], mSize);
            } else {
                const size_t sz = mCapacity - mConsume;
                stream->write(&mData[mConsume], sz);
                stream->write(&mData[0], mSize - sz);
            }
        }
    }

    bool load(base::Stream* stream) {
        const size_t newSize = stream->getBe32();
        if (newSize == 0) {
            clear(true);
            return true;
        }

        const size_t newCapacity = getCapacity(newSize);
        assert(newCapacity >= newSize);
        std::unique_ptr<char[]> newData = std::make_unique<char[]>(newCapacity);

        if (stream->read(newData.get(), newSize) != newSize) {
            return false;
        }

        mData = std::move(newData);
        mCapacity = newCapacity;
        mSize = newSize;
        mProduce = newSize;
        mConsume = 0;
        return true;
    }

private:
    static constexpr size_t kMinCapacity = 1024;

    static size_t getCapacity(const size_t size) {
        return std::max(kMinCapacity, size * 3U / 2U);
    }

    std::unique_ptr<char[]> mData;
    size_t mCapacity = 0;
    size_t mSize = 0;
    size_t mProduce = 0;
    size_t mConsume = 0;
};

}  // namespace emulation
}  // namespace android
