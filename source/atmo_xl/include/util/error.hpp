/*
Copyright (c) 2017-2018 Adubbz

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#pragma once

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#define ASSERT_OK(res_expr, desc) \
    ({ \
        const auto tmp_rc = (res_expr); \
        if (R_FAILED(tmp_rc)) { \
            char msg[256] = {}; std::snprintf(msg, sizeof(msg)-1, "%s:%u: %s.  Error code: 0x%08x\n", __func__, __LINE__, desc, tmp_rc); \
            throw std::runtime_error(msg); \
        } \
    })

namespace tin::util
{
    template <typename... Args>
    [[noreturn]] inline void throw_formatted_error(const char* function,
                                                   unsigned int line,
                                                   const char* format,
                                                   Args... args)
    {
        const int payloadSize = std::snprintf(nullptr, 0, format, args...);
        const std::string prefix = std::string(function ? function : "?") + ":" +
                                   std::to_string(line) + ": ";
        if (payloadSize < 0)
            throw std::runtime_error(prefix + "error formatting failed");

        std::vector<char> payload(static_cast<std::size_t>(payloadSize) + 1U, '\0');
        std::snprintf(payload.data(), payload.size(), format, args...);
        throw std::runtime_error(prefix + payload.data());
    }
}

// Nested install diagnostics can exceed 1 KiB. Dynamic formatting prevents the
// source-audit verdict/hash tail from being silently lost by an intermediate
// exception wrapper.
#define THROW_FORMAT(...) \
    ::tin::util::throw_formatted_error(__func__, __LINE__, __VA_ARGS__)

#ifdef NXLINK_DEBUG
#define LOG_DEBUG(format, ...) { std::printf("%s:%u: ", __func__, __LINE__); std::printf(format, ##__VA_ARGS__); }
#else
#define LOG_DEBUG(format, ...) do { } while (0)
#endif
