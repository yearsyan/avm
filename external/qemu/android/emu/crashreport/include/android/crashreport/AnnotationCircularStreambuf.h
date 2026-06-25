// Copyright 2022 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
#pragma once
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <streambuf>
#include <string>

namespace android {
namespace crashreport {

// #define DEBUG_CRASH
#ifdef DEBUG_CRASH
#define DD_AN(fmt, ...)                                                        \
    fprintf(stderr, "AnnotationCircularStreambuf: %s:%d| " fmt "\n", __func__, \
            __LINE__, ##__VA_ARGS__)
#else
#define DD_AN(...) (void)0
#endif

template <std::size_t MaxSize>
class AnnotationCircularStreambuf : public std::streambuf {
public:
    AnnotationCircularStreambuf(const AnnotationCircularStreambuf&) = delete;
    AnnotationCircularStreambuf& operator=(const AnnotationCircularStreambuf&) =
            delete;

    explicit AnnotationCircularStreambuf(std::string name) : mName(name) {
        setp(mBuffer, mBuffer + MaxSize);
    }

    // Mainly for testing, sets the read buffers properly.
    int sync() override {
        setg(mBuffer, mBuffer, mBuffer + mSize);
        return 0;
    }

protected:
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        std::lock_guard<std::mutex> lock(mBufferAccess);
        std::streamsize toWrite = n;
        std::streamsize written = 0;
        const char* p = s;
        DD_AN("Writing %.*s\n", n, s);
        // Optimization, we are going to overwrite these anyways, so who cares?
        if (toWrite > MaxSize) {
            // No need to overwrite ourselves multiple times..
            // We can skip the characters that we know will be overwritten
            p += (toWrite - MaxSize);
            written = toWrite - MaxSize;
            toWrite = MaxSize;
        }

        while (toWrite > 0) {
            if (pptr() == epptr()) {
                setp(mBuffer, mBuffer + MaxSize);
                mSize = MaxSize;
            }
            std::streamsize available = epptr() - pptr();
            std::streamsize toCopy = std::min(toWrite, available);
            assert(available > 0);
            assert(toCopy > 0);

            std::memcpy(pptr(), p, static_cast<std::size_t>(toCopy));
            pbump(static_cast<int>(toCopy));

            p += toCopy;
            written += toCopy;
            toWrite -= toCopy;
        }

        if (mSize != MaxSize) {
            mSize = pptr() - pbase();
        }
        return written;
    };

    int_type overflow(int_type ch) override { return traits_type::eof(); }

    int_type underflow() override {
        return gptr() == egptr() ? traits_type::eof() : *gptr();
    }

private:
    std::mutex mBufferAccess;
    std::string mName;
    char mBuffer[MaxSize];
    int mSize = 0;
};

// A stream buffer that can hold 8kb of annotation data.
using DefaultAnnotationCircularStreambuf = AnnotationCircularStreambuf<8192>;
}  // namespace crashreport
}  // namespace android
