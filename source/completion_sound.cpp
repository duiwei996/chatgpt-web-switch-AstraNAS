// SPDX-License-Identifier: GPL-3.0-or-later
#include "completion_sound.hpp"

#ifdef __SWITCH__
#include <switch.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <vector>

namespace astranas::completion_sound {
namespace {
constexpr std::size_t kAudioAlignment = 0x1000;
constexpr const char* kSuccessSoundPath = "romfs:/audio/fertig.wav";

std::uint16_t read_u16_le(const unsigned char* p) {
    return static_cast<std::uint16_t>(p[0]) |
           (static_cast<std::uint16_t>(p[1]) << 8);
}

std::uint32_t read_u32_le(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

std::size_t align_up(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

struct WavPcm16 {
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    std::vector<std::int16_t> samples;
};

bool load_pcm16_wav(const char* path, WavPcm16& wav) {
    std::FILE* fp = std::fopen(path, "rb");
    if (!fp) return false;

    unsigned char header[12]{};
    bool ok = std::fread(header, 1, sizeof(header), fp) == sizeof(header) &&
              std::memcmp(header, "RIFF", 4) == 0 &&
              std::memcmp(header + 8, "WAVE", 4) == 0;
    std::uint16_t audio_format = 0;
    std::uint16_t bits_per_sample = 0;
    long data_offset = -1;
    std::uint32_t data_size = 0;

    while (ok && data_offset < 0) {
        unsigned char chunk[8]{};
        if (std::fread(chunk, 1, sizeof(chunk), fp) != sizeof(chunk)) { ok = false; break; }
        const std::uint32_t size = read_u32_le(chunk + 4);
        if (std::memcmp(chunk, "fmt ", 4) == 0) {
            if (size < 16 || size > 4096) { ok = false; break; }
            std::vector<unsigned char> fmt(size);
            if (std::fread(fmt.data(), 1, size, fp) != size) { ok = false; break; }
            audio_format = read_u16_le(fmt.data());
            wav.channels = read_u16_le(fmt.data() + 2);
            wav.sample_rate = read_u32_le(fmt.data() + 4);
            bits_per_sample = read_u16_le(fmt.data() + 14);
            if (size & 1u) (void)std::fseek(fp, 1, SEEK_CUR);
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            data_offset = std::ftell(fp);
            data_size = size;
            break;
        } else {
            const long skip = static_cast<long>(size + (size & 1u));
            if (std::fseek(fp, skip, SEEK_CUR) != 0) { ok = false; break; }
        }
    }

    if (!ok || data_offset < 0 || audio_format != 1 || bits_per_sample != 16 ||
        (wav.channels != 1 && wav.channels != 2) || wav.sample_rate == 0 || data_size < 2) {
        std::fclose(fp);
        return false;
    }
    const std::size_t sample_count = data_size / sizeof(std::int16_t);
    wav.samples.resize(sample_count);
    if (std::fseek(fp, data_offset, SEEK_SET) != 0 ||
        std::fread(wav.samples.data(), sizeof(std::int16_t), sample_count, fp) != sample_count) {
        std::fclose(fp);
        return false;
    }
    std::fclose(fp);
    return sample_count >= wav.channels;
}

std::int16_t interpolate_sample(const WavPcm16& wav, std::size_t channel, double source_frame) {
    const std::size_t frame_count = wav.samples.size() / wav.channels;
    if (frame_count == 0) return 0;
    const std::size_t a = std::min(static_cast<std::size_t>(source_frame), frame_count - 1);
    const std::size_t b = std::min(a + 1, frame_count - 1);
    const double fraction = source_frame - static_cast<double>(a);
    const std::size_t source_channel = wav.channels == 1 ? 0 : std::min(channel, static_cast<std::size_t>(1));
    const double sa = wav.samples[a * wav.channels + source_channel];
    const double sb = wav.samples[b * wav.channels + source_channel];
    const double value = sa + (sb - sa) * fraction;
    return static_cast<std::int16_t>(std::max(-32768.0, std::min(32767.0, value)));
}
} // namespace

void play_success() noexcept {
    const Result romfs_result = romfsInit();
    const bool mounted_here = R_SUCCEEDED(romfs_result);

    WavPcm16 wav;
    const bool loaded = load_pcm16_wav(kSuccessSoundPath, wav);
    if (mounted_here) romfsExit();
    if (!loaded) return;

    if (R_FAILED(audoutInitialize())) return;
    void* raw = nullptr;
    bool started_here = false;
    do {
        const u32 output_rate = audoutGetSampleRate();
        const u32 output_channels = audoutGetChannelCount();
        if (output_rate == 0 || output_channels == 0 || output_channels > 8 ||
            audoutGetPcmFormat() != PcmFormat_Int16) break;

        const std::size_t input_frames = wav.samples.size() / wav.channels;
        const std::size_t output_frames = static_cast<std::size_t>(
            (static_cast<std::uint64_t>(input_frames) * output_rate + wav.sample_rate - 1) / wav.sample_rate);
        if (output_frames == 0) break;
        if (output_frames > static_cast<std::size_t>(-1) / output_channels / sizeof(std::int16_t)) break;

        const std::size_t data_bytes = output_frames * output_channels * sizeof(std::int16_t);
        const std::size_t buffer_bytes = align_up(data_bytes, kAudioAlignment);
        raw = memalign(kAudioAlignment, buffer_bytes);
        if (!raw) break;
        std::memset(raw, 0, buffer_bytes);

        auto* output = static_cast<std::int16_t*>(raw);
        const double source_step = static_cast<double>(wav.sample_rate) / static_cast<double>(output_rate);
        for (std::size_t frame = 0; frame < output_frames; ++frame) {
            const double source_frame = std::min(static_cast<double>(input_frames - 1), frame * source_step);
            for (u32 channel = 0; channel < output_channels; ++channel)
                output[frame * output_channels + channel] = interpolate_sample(wav, channel, source_frame);
        }

        if (audoutGetDeviceState() == AudioOutState_Stopped) {
            if (R_FAILED(audoutStartAudioOut())) break;
            started_here = true;
        }

        AudioOutBuffer buffer{};
        buffer.buffer = raw;
        buffer.buffer_size = buffer_bytes;
        buffer.data_size = data_bytes;
        buffer.data_offset = 0;
        AudioOutBuffer* released = nullptr;
        (void)audoutPlayBuffer(&buffer, &released);
    } while (false);

    if (started_here) (void)audoutStopAudioOut();
    std::free(raw);
    audoutExit();
}

} // namespace astranas::completion_sound
#endif
