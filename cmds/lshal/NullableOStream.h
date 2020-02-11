/*
 * Copyright (C) 2016 The Android Open Source Project
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

#pragma once

#include <iostream>

#include <hwbinder/BufferedTextOutput.h>

namespace android {
namespace lshal {

template<typename S>
class NullableOStream : public ::android::hardware::BufferedTextOutput {
public:
    explicit NullableOStream(S &os) : BufferedTextOutput(MULTITHREADED), mOs(&os) {}
    explicit NullableOStream(S *os) : BufferedTextOutput(MULTITHREADED), mOs(os) {}
    virtual ~NullableOStream() {};
    NullableOStream &operator=(S &os) {
        mOs = &os;
        return *this;
    }
    NullableOStream &operator=(S *os) {
        mOs = os;
        return *this;
    }
    template<typename Other>
    NullableOStream &operator=(const NullableOStream<Other> &other) {
        mOs = other.mOs;
        return *this;
    }

//     const NullableOStream &operator<<(std::ostream& (*pf)(std::ostream&)) const {
//         if (mOs) {
//             (*mOs) << pf;
//         }
//         return *this;
//     }
//     template<typename T>
//     const NullableOStream &operator<<(const T &rhs) const {
//         if (mOs) {
//             (*mOs) << rhs;
//         }
//         return *this;
//     }
    S& buf() const {
        return *mOs;
    }
    operator bool() const { // NOLINT(google-explicit-constructor)
        return mOs != nullptr;
    }

protected:
    virtual status_t writeLines(const struct iovec& vec, size_t N) {
        // TODO(hknepshield) for some reason, last line is empty
        if (mOs) {
          if (N != 1) *mOs << "N > 1: " << N << "\n";
          *mOs << std::string((const char*)vec.iov_base, vec.iov_len);
        }
        return OK;
    }

private:
    template<typename>
    friend class NullableOStream;

    S *mOs = nullptr;
};

}  // namespace lshal
}  // namespace android
