// SPDX-License-Identifier: GPL-3.0-or-later
#include "buffered_stream.hpp"
#include "../install_performance.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

namespace astranas::title_backend {

bool stream_buffered_read_ahead(PackageSource& source,
                                std::uint64_t source_offset,
                                std::uint64_t total_size,
                                const BufferedStreamConsumer& consumer,
                                std::string& error,
                                const BufferedStreamHeartbeat& heartbeat,
                                std::size_t chunk_size,
                                std::size_t slot_count,
                                bool record_install_metrics,
                                BufferedStreamTelemetry* telemetry) {
    error.clear();
    if (!consumer) { error = "预读缓冲消费者不可用"; return false; }
    if (chunk_size == 0) { error = "预读缓冲块大小不能为零"; return false; }
    if (slot_count < 2) { error = "预读缓冲槽位不足"; return false; }
    if (slot_count > std::numeric_limits<std::size_t>::max() / chunk_size) {
        error = "预读缓冲容量溢出";
        return false;
    }
    if (telemetry) {
        telemetry->produced.store(0, std::memory_order_relaxed);
        telemetry->consumed.store(0, std::memory_order_relaxed);
        telemetry->active_read_ns.store(0, std::memory_order_relaxed);
        telemetry->backpressure_ns.store(0, std::memory_order_relaxed);
        telemetry->capacity_bytes = static_cast<std::uint64_t>(slot_count * chunk_size);
    }
    if (total_size == 0) return true;
    if (source_offset > source.size() || total_size > source.size() - source_offset) {
        error = "预读缓冲范围超出安装包";
        return false;
    }

    struct Slot {
        std::vector<unsigned char> data;
        std::size_t size = 0;
        std::uint64_t logical_offset = 0;
        bool ready = false;
    };

    std::vector<Slot> slots;
    try {
        slots.resize(slot_count);
        for (auto& slot : slots) slot.data.resize(chunk_size);
    } catch (const std::bad_alloc&) {
        if (slot_count == kInstallReadAheadSlots && chunk_size == kInstallStreamChunkSize)
            error = "无法分配 1 GiB 网络直装预读缓冲；请确认从完整应用模式启动 AstraNAS";
        else
            error = "无法分配高速传输预读缓冲";
        return false;
    }

    std::mutex mutex;
    std::condition_variable cv;
    bool stop = false;
    bool producer_done = false;
    bool producer_failed = false;
    std::string producer_error;

    std::thread producer([&] {
        const auto begin = std::chrono::steady_clock::now();
        std::uint64_t produced = 0;
        std::uint64_t backpressure_ns = 0;
        std::uint64_t reported_produced = 0;
        std::uint64_t reported_active_read_ns = 0;
        std::uint64_t reported_backpressure_ns = 0;
        std::size_t slot_index = 0;

        auto publish_timing = [&] {
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now - begin).count());
            const std::uint64_t active_read_ns = elapsed > backpressure_ns ? elapsed - backpressure_ns : elapsed;
            if (telemetry) {
                telemetry->backpressure_ns.store(backpressure_ns, std::memory_order_relaxed);
                telemetry->active_read_ns.store(active_read_ns, std::memory_order_relaxed);
            }
            if (record_install_metrics) {
                if (produced >= reported_produced && active_read_ns >= reported_active_read_ns) {
                    astranas::install_performance::add_source_read(
                        produced - reported_produced, active_read_ns - reported_active_read_ns);
                    reported_produced = produced;
                    reported_active_read_ns = active_read_ns;
                }
                if (backpressure_ns >= reported_backpressure_ns) {
                    astranas::install_performance::add_source_backpressure(
                        backpressure_ns - reported_backpressure_ns);
                    reported_backpressure_ns = backpressure_ns;
                }
            }
        };

        auto wait_for_slot = [&]() -> bool {
            const auto wait_begin = std::chrono::steady_clock::now();
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [&] { return stop || !slots[slot_index].ready; });
            const auto wait_end = std::chrono::steady_clock::now();
            backpressure_ns += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(wait_end - wait_begin).count());
            publish_timing();
            return !stop;
        };

        auto advance_slot = [&] { slot_index = (slot_index + 1u) % slots.size(); };

        auto publish_stream_bytes = [&](const unsigned char* data, std::size_t size,
                                        std::uint64_t logical_offset) -> bool {
            if (!data && size != 0) return false;
            if (logical_offset != produced) {
                producer_error = "远程连续流返回了非连续偏移";
                return false;
            }
            std::size_t cursor = 0;
            while (cursor < size) {
                if (!wait_for_slot()) return false;
                Slot& slot = slots[slot_index];
                if (slot.size == 0) slot.logical_offset = produced;
                const std::size_t copy = std::min(size - cursor, chunk_size - slot.size);
                std::memcpy(slot.data.data() + slot.size, data + cursor, copy);
                slot.size += copy;
                produced += copy;
                cursor += copy;
                if (telemetry) telemetry->produced.store(produced, std::memory_order_relaxed);
                publish_timing();
                if (slot.size == chunk_size || produced == total_size) {
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        if (stop) return false;
                        slot.ready = true;
                    }
                    advance_slot();
                    cv.notify_all();
                }
            }
            return true;
        };

        bool ok = true;
        std::string read_error;
        if (source.supports_contiguous_stream()) {
            ok = source.stream_contiguous(
                source_offset, total_size, publish_stream_bytes,
                [&](std::uint64_t, std::uint64_t) {
                    publish_timing();
                    std::lock_guard<std::mutex> lock(mutex);
                    return !stop;
                }, read_error);
        } else {
            while (produced < total_size && ok) {
                if (!wait_for_slot()) return;
                Slot& slot = slots[slot_index];
                slot.size = 0;
                slot.logical_offset = produced;
                const std::size_t want = static_cast<std::size_t>(
                    std::min<std::uint64_t>(chunk_size, total_size - produced));
                std::size_t actual = 0;
                ok = source.read_at(source_offset + produced, slot.data.data(), want, actual, read_error);
                if (!ok || actual != want) {
                    if (read_error.empty()) read_error = "安装包源返回的数据长度不足";
                    break;
                }
                slot.size = actual;
                produced += actual;
                if (telemetry) telemetry->produced.store(produced, std::memory_order_relaxed);
                publish_timing();
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (stop) return;
                    slot.ready = true;
                }
                advance_slot();
                cv.notify_all();
            }
        }

        publish_timing();
        if (telemetry) {
            telemetry->produced.store(produced, std::memory_order_relaxed);
        }

        std::lock_guard<std::mutex> lock(mutex);
        if (stop) return;
        if (!ok || produced != total_size) {
            producer_error = read_error.empty() ?
                (producer_error.empty() ? "安装包源连续读取未完成" : producer_error) : read_error;
            producer_failed = true;
            stop = true;
        } else {
            producer_done = true;
        }
        cv.notify_all();
    });

    std::uint64_t consumed = 0;
    std::size_t slot_index = 0;
    try {
        while (consumed < total_size) {
            const auto wait_begin = std::chrono::steady_clock::now();
            std::unique_lock<std::mutex> lock(mutex);
            const auto ready = [&] {
                return slots[slot_index].ready || producer_failed || producer_done || stop;
            };
            while (!ready()) {
                if (cv.wait_for(lock, std::chrono::milliseconds(25), ready)) break;
                lock.unlock();
                const bool keep_running = !heartbeat || heartbeat(consumed, total_size);
                lock.lock();
                if (!keep_running) {
                    error = "installation cancelled";
                    stop = true;
                    cv.notify_all();
                    break;
                }
            }
            const auto wait_end = std::chrono::steady_clock::now();
            if (record_install_metrics) {
                astranas::install_performance::add_source_wait(static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(wait_end - wait_begin).count()));
            }

            if (stop && !producer_failed) break;
            if (producer_failed) {
                error = producer_error;
                stop = true;
                cv.notify_all();
                break;
            }

            Slot& slot = slots[slot_index];
            if (!slot.ready) {
                error = "预读缓冲在完成请求范围前提前结束";
                stop = true;
                cv.notify_all();
                break;
            }

            const std::size_t size = slot.size;
            const std::uint64_t logical = slot.logical_offset;
            lock.unlock();
            consumer(slot.data.data(), size, logical);
            lock.lock();
            slot.ready = false;
            slot.size = 0;
            consumed += size;
            if (telemetry) telemetry->consumed.store(consumed, std::memory_order_relaxed);
            slot_index = (slot_index + 1u) % slots.size();
            lock.unlock();
            cv.notify_all();

            if (heartbeat && !heartbeat(consumed, total_size)) {
                std::lock_guard<std::mutex> stop_lock(mutex);
                error = "installation cancelled";
                stop = true;
                cv.notify_all();
                break;
            }
        }
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
            cv.notify_all();
        }
        if (producer.joinable()) producer.join();
        throw;
    }

    {
        std::lock_guard<std::mutex> lock(mutex);
        stop = true;
        cv.notify_all();
    }
    if (producer.joinable()) producer.join();
    return error.empty() && consumed == total_size;
}

} // namespace astranas::title_backend
