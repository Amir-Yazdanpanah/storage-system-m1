/*
 * MIT License
Copyright (c) 2021 - current
Authors:  Animesh Trivedi
This code is part of the Storage System Course at VU Amsterdam
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

#include "zns_device.h"

#include <cerrno>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>
#include <unistd.h>

namespace {

struct ftl_state {
    int fd = -1;
    uint32_t nsid = 0;
    uint32_t lba_bytes = 0;
    uint64_t zone_size_lbas = 0;     // Physical distance between zone starts.
    uint64_t zone_capacity_lbas = 0; // Writable pages per zone, not zone size.
    uint32_t max_active_zones = 0;   // NVMe MAR: zero based, UINT32_MAX unlimited.

    std::unordered_map<uint64_t, uint64_t> page_map; // Logical page -> log LBA.
    std::vector<uint64_t> zone_map;                // Logical zone -> data start.
    std::vector<uint64_t> log_used;                // Used pages in each log zone.
    uint64_t spare_zone = 0;                       // Empty GC destination.
    int active_log = -1;
    bool active_open = false;
    int gc_wmark = 1;
    uint64_t gc_events = 0;
    size_t next_victim = 0;
    int error = 0;
    bool stopping = false;

    std::mutex mutex;
    std::condition_variable changed;
    std::thread gc_thread;
    std::vector<unsigned char> copy_buffer; // One-page GC scratch, not a cache.

    ~ftl_state() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            changed.notify_all();
        }
        if (gc_thread.joinable()) gc_thread.join();
        if (fd >= 0) close(fd);
    }
};

int command_result(int result) {
    return result == -1 ? -(errno != 0 ? errno : EIO) : result;
}

int validate_io(user_zns_device *device, uint64_t address, const void *buffer,
                uint32_t size) {
    if (device == nullptr || device->_private == nullptr ||
        device->lba_size_bytes == 0 || (size != 0 && buffer == nullptr)) {
        return -EINVAL;
    }
    if (address % device->lba_size_bytes != 0 ||
        size % device->lba_size_bytes != 0 ||
        address > device->capacity_bytes || size > device->capacity_bytes - address) {
        return -EINVAL;
    }
    return 0;
}

int read_page(ftl_state &state, uint64_t physical_lba, void *buffer) {
    // NLB is zero based: 0 means one LBA. This conservatively respects MDTS.
    return command_result(nvme_read(state.fd, state.nsid, physical_lba,
        0, 0, 0, 0, 0, 0, state.lba_bytes, buffer, 0, nullptr));
}

int write_page(ftl_state &state, uint64_t physical_lba, void *buffer) {
    return command_result(nvme_write(state.fd, state.nsid, physical_lba,
        0, 0, 0, 0, 0, 0, 0, state.lba_bytes, buffer, 0, nullptr));
}

int zone_command(ftl_state &state, uint64_t start, nvme_zns_send_action action) {
    return command_result(nvme_zns_mgmt_send(state.fd, state.nsid, start,
        false, action, 0, nullptr));
}

uint64_t physical_page(const ftl_state &state, uint64_t logical_lba) {
    const auto entry = state.page_map.find(logical_lba);
    if (entry != state.page_map.end()) return entry->second;
    return state.zone_map[logical_lba / state.zone_capacity_lbas] +
        logical_lba % state.zone_capacity_lbas;
}

int free_logs(const ftl_state &state) {
    int count = 0;
    for (size_t index = 0; index < state.log_used.size(); ++index) {
        if (state.log_used[index] == 0 && static_cast<int>(index) != state.active_log) {
            ++count;
        }
    }
    return count;
}

int full_log(const ftl_state &state) {
    for (size_t offset = 0; offset < state.log_used.size(); ++offset) {
        const size_t index = (state.next_victim + offset) % state.log_used.size();
        if (state.log_used[index] == state.zone_capacity_lbas) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

// Keep just one zone OPEN. A partially written, closed log is still ACTIVE.
int make_room_for_merge(ftl_state &state) {
    if (state.active_log < 0) return 0;
    const uint64_t start = state.active_log * state.zone_size_lbas;
    if (state.max_active_zones == 0) {
        // With a one-active-zone controller, finish the partial log before
        // opening the spare. Its unused tail is discarded, never live data.
        const int result = zone_command(state, start, NVME_ZNS_ZSA_FINISH);
        if (result != 0) return result;
        state.log_used[state.active_log] = state.zone_capacity_lbas;
        state.active_log = -1;
        state.active_open = false;
    } else if (state.active_open) {
        const int result = zone_command(state, start, NVME_ZNS_ZSA_CLOSE);
        if (result != 0) return result;
        state.active_open = false;
    }
    return 0;
}

// Called only by the GC thread, with the mutex held for ONE logical-zone merge.
int merge_data_zone(ftl_state &state, uint64_t logical_zone) {
    int result = make_room_for_merge(state);
    if (result != 0) return result;
    const uint64_t first_lba = logical_zone * state.zone_capacity_lbas;
    const uint64_t old_data_zone = state.zone_map[logical_zone];
    for (uint64_t page = 0; page < state.zone_capacity_lbas; ++page) {
        result = read_page(state, physical_page(state, first_lba + page),
                           state.copy_buffer.data());
        if (result != 0) return result;
        result = write_page(state, state.spare_zone + page, state.copy_buffer.data());
        if (result != 0) return result;
    }
    // The old data zone is intact until the complete new version is on device.
    state.zone_map[logical_zone] = state.spare_zone;
    for (uint64_t page = 0; page < state.zone_capacity_lbas; ++page) {
        state.page_map.erase(first_lba + page);
    }
    result = zone_command(state, old_data_zone, NVME_ZNS_ZSA_RESET);
    if (result != 0) return result;
    state.spare_zone = old_data_zone;
    return 0;
}

void garbage_collector(ftl_state *state) {
    try {
        std::unique_lock<std::mutex> lock(state->mutex);
        int victim = -1;
        while (true) {
            state->changed.wait(lock, [&]() {
                return state->stopping || state->error != 0 || victim >= 0 ||
                    (free_logs(*state) + 1 <= state->gc_wmark && full_log(*state) >= 0);
            });
            if (state->stopping || state->error != 0) return;
            if (victim < 0) {
                victim = full_log(*state);
                state->next_victim = (victim + 1) % state->log_used.size();
            }

            // Find one logical zone with a latest page in this sealed log.
            // Stale physical copies do not have page-map entries and need no copy.
            bool found = false;
            uint64_t logical_zone = 0;
            for (const auto &entry : state->page_map) {
                if (entry.second / state->zone_size_lbas == static_cast<uint64_t>(victim)) {
                    logical_zone = entry.first / state->zone_capacity_lbas;
                    found = true;
                    break;
                }
            }
            int result;
            if (found) {
                result = merge_data_zone(*state, logical_zone);
            } else {
                // All latest pages have safe data-zone or other-log versions.
                result = zone_command(*state, victim * state->zone_size_lbas,
                                      NVME_ZNS_ZSA_RESET);
                if (result == 0) {
                    state->log_used[victim] = 0;
                    ++state->gc_events;
                    victim = -1;
                }
            }
            if (result != 0) state->error = result;
            state->changed.notify_all();
            // Give user I/O a chance between merges, not only after draining a log.
            lock.unlock();
            std::this_thread::yield();
            lock.lock();
        }
    } catch (const std::exception &) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->error = -EIO;
        state->changed.notify_all();
    }
}

int discover_geometry(ftl_state &state, const zdev_init_params &params,
                      user_zns_device &device) {
    nvme_id_ns ns = {};
    int result = command_result(nvme_get_nsid(state.fd, &state.nsid));
    if (result != 0) {
        return result;
    }
    result = command_result(nvme_identify_ns(state.fd, state.nsid, &ns));
    if (result != 0) {
        return result;
    }
    const unsigned int format = ns.flbas & 0x0f;
    if (format > ns.nlbaf || ns.lbaf[format].ds < 9 ||
        ns.lbaf[format].ds >= 32 || le16_to_cpu(ns.lbaf[format].ms) != 0 ||
        (ns.dps & 0x07) != 0) {
        return -EOPNOTSUPP; // The course uses ordinary LBAs without metadata/PI.
    }
    state.lba_bytes = uint32_t(1) << ns.lbaf[format].ds;

    nvme_zns_id_ns zns = {};
    result = command_result(nvme_zns_identify_ns(state.fd, state.nsid, &zns));
    if (result != 0) {
        return result;
    }
    state.zone_size_lbas = le64_to_cpu(zns.lbafe[format].zsze);
    state.max_active_zones = le32_to_cpu(zns.mar);
    if (state.zone_size_lbas == 0) {
        return -EINVAL;
    }

    // 4 KiB contains the 64-byte report header plus 63 descriptors. Paginate
    // rather than assuming the entire namespace fits in one report/MDTS.
    std::vector<uint64_t> report_storage(4096 / sizeof(uint64_t), 0);
    nvme_zone_report *report = reinterpret_cast<nvme_zone_report *>(
        report_storage.data());
    result = command_result(nvme_zns_mgmt_recv(state.fd, state.nsid, 0,
        NVME_ZNS_ZRA_REPORT_ZONES, NVME_ZNS_ZRAS_REPORT_ALL, false,
        4096, report));
    if (result != 0) {
        return result;
    }
    const uint64_t zone_count = le64_to_cpu(report->nr_zones);
    if (zone_count <= static_cast<uint64_t>(params.log_zones)) {
        return -EINVAL;
    }
    if (zone_count > std::numeric_limits<uint32_t>::max() ||
        zone_count > std::numeric_limits<uint64_t>::max() / state.zone_size_lbas ||
        zone_count * state.zone_size_lbas != le64_to_cpu(ns.nsze)) {
        return -EOVERFLOW;
    }
    const size_t descriptors_per_report =
        (4096 - sizeof(nvme_zone_report)) / sizeof(nvme_zns_desc);
    uint64_t processed = 0;
    while (processed < zone_count) {
        uint64_t count = zone_count - processed;
        if (processed != 0) {
            result = command_result(nvme_zns_mgmt_recv(state.fd, state.nsid,
                processed * state.zone_size_lbas, NVME_ZNS_ZRA_REPORT_ZONES,
                NVME_ZNS_ZRAS_REPORT_ALL, true, 4096, report));
            if (result != 0) {
                return result;
            }
            count = le64_to_cpu(report->nr_zones);
            if (count == 0 || count > zone_count - processed) {
                return -EIO;
            }
        }
        if (count > descriptors_per_report) {
            count = descriptors_per_report;
        }
        for (uint64_t index = 0; index < count; ++index) {
            const nvme_zns_desc &descriptor = report->entries[index];
            const uint64_t capacity = le64_to_cpu(descriptor.zcap);
            if ((descriptor.zt & 0x0f) != NVME_ZONE_TYPE_SEQWRITE_REQ ||
                capacity == 0 || capacity > state.zone_size_lbas ||
                le64_to_cpu(descriptor.zslba) !=
                    (processed + index) * state.zone_size_lbas) {
                return -EOPNOTSUPP;
            }
            if (processed == 0 && index == 0) {
                state.zone_capacity_lbas = capacity;
            } else if (capacity != state.zone_capacity_lbas) {
                return -EOPNOTSUPP; // The public test API assumes equal capacities.
            }
        }
        processed += count;
    }
    // The supplied testing ABI represents zone capacity in 32-bit BYTES.
    if (state.zone_capacity_lbas >
        std::numeric_limits<uint32_t>::max() / state.lba_bytes) {
        return -EOVERFLOW;
    }
    const uint64_t capacity_bytes = state.zone_capacity_lbas * state.lba_bytes;
    const uint64_t data_zones = zone_count - params.log_zones;
    if (data_zones > std::numeric_limits<uint64_t>::max() / capacity_bytes) {
        return -EOVERFLOW;
    }
    state.zone_map.resize(static_cast<size_t>(data_zones));
    for (uint64_t index = 0; index < data_zones; ++index) {
        state.zone_map[index] = (index + params.log_zones) * state.zone_size_lbas;
    }
    // One overhead zone is a rotating, empty destination for safe GC merges.
    state.spare_zone = (params.log_zones - 1) * state.zone_size_lbas;
    state.log_used.assign(static_cast<size_t>(params.log_zones - 1), 0);
    state.gc_wmark = params.gc_wmark;
    state.copy_buffer.resize(state.lba_bytes);
    device.lba_size_bytes = state.lba_bytes;
    device.capacity_bytes = data_zones * capacity_bytes;
    device.tparams.zns_lba_size = state.lba_bytes;
    device.tparams.zns_zone_capacity = static_cast<uint32_t>(capacity_bytes);
    device.tparams.zns_num_zones = static_cast<uint32_t>(zone_count);
    return 0;
}

} // namespace

extern "C" {

int init_ss_zns_device(zdev_init_params *params, user_zns_device **my_dev) {
    if (my_dev == nullptr) return -EINVAL;
    *my_dev = nullptr;
    if (params == nullptr || params->name == nullptr || params->name[0] == '\0' ||
        params->log_zones < 3 || params->gc_wmark < 1 ||
        params->gc_wmark >= params->log_zones) {
        return -EINVAL;
    }
    if (!params->force_reset) return -EOPNOTSUPP; // Persistence is Milestone 5.
    try {
        std::unique_ptr<user_zns_device> device(new user_zns_device());
        std::unique_ptr<ftl_state> state(new ftl_state());
        const char *name = params->name;
        if (std::strncmp(name, "/dev/", 5) == 0) name += 5;
        state->fd = nvme_open(name);
        if (state->fd < 0) return -(errno != 0 ? errno : EIO);
        int result = discover_geometry(*state, *params, *device);
        if (result != 0) return result;
        result = command_result(nvme_zns_mgmt_send(state->fd, state->nsid,
            0, true, NVME_ZNS_ZSA_RESET, 0, nullptr));
        if (result != 0) return result;
        state->gc_thread = std::thread(garbage_collector, state.get());
        device->_private = state.release();
        *my_dev = device.release();
        return 0;
    } catch (const std::bad_alloc &) {
        return -ENOMEM;
    } catch (const std::length_error &) {
        return -EOVERFLOW;
    } catch (const std::system_error &) {
        return -EAGAIN;
    }
}

int zns_udevice_read(user_zns_device *my_dev, uint64_t address, void *buffer,
                     uint32_t size) {
    const int result = validate_io(my_dev, address, buffer, size);
    if (result != 0 || size == 0) return result;
    ftl_state &state = *static_cast<ftl_state *>(my_dev->_private);
    std::lock_guard<std::mutex> lock(state.mutex);
    unsigned char *bytes = static_cast<unsigned char *>(buffer);
    const uint64_t first_lba = address / state.lba_bytes;
    for (uint64_t page = 0; page < size / state.lba_bytes; ++page) {
        const int status = read_page(state, physical_page(state, first_lba + page),
                                     bytes + page * state.lba_bytes);
        if (status != 0) return status;
    }
    return 0;
}

int zns_udevice_write(user_zns_device *my_dev, uint64_t address, void *buffer,
                      uint32_t size) {
    const int result = validate_io(my_dev, address, buffer, size);
    if (result != 0 || size == 0) return result;
    ftl_state &state = *static_cast<ftl_state *>(my_dev->_private);
    unsigned char *bytes = static_cast<unsigned char *>(buffer);
    const uint64_t first_lba = address / state.lba_bytes;
    try {
        for (uint64_t page = 0; page < size / state.lba_bytes; ++page) {
            std::unique_lock<std::mutex> lock(state.mutex);
            // Wait for the background worker; the user thread never performs GC.
            state.changed.wait(lock, [&]() {
                return state.stopping || state.error != 0 || state.active_log >= 0 ||
                    free_logs(state) + 1 > state.gc_wmark;
            });
            if (state.error != 0) return state.error;
            if (state.stopping) return -ESHUTDOWN;
            if (state.active_log < 0) {
                for (size_t index = 0; index < state.log_used.size(); ++index) {
                    if (state.log_used[index] == 0) {
                        state.active_log = static_cast<int>(index);
                        break;
                    }
                }
            }
            const int log = state.active_log;
            const uint64_t physical_lba = log * state.zone_size_lbas + state.log_used[log];
            const uint64_t logical_lba = first_lba + page;
            // Allocate metadata before the write; commit it only after success.
            auto entry = state.page_map.emplace(logical_lba, uint64_t(0));
            const int status = write_page(state, physical_lba, bytes + page * state.lba_bytes);
            if (status != 0) {
                if (entry.second) state.page_map.erase(entry.first);
                state.error = status; // Do not reuse an uncertain write pointer.
                state.changed.notify_all();
                return status;
            }
            entry.first->second = physical_lba;
            state.active_open = true;
            if (++state.log_used[log] == state.zone_capacity_lbas) {
                state.active_log = -1;
                state.active_open = false;
            }
            state.changed.notify_all();
            // The lock is released on each page, so GC can run during large I/O.
        }
        return 0;
    } catch (const std::bad_alloc &) {
        return -ENOMEM;
    } catch (const std::length_error &) {
        return -EOVERFLOW;
    }
}

int deinit_ss_zns_device(user_zns_device *my_dev) {
    if (my_dev == nullptr || my_dev->_private == nullptr) return -EINVAL;
    ftl_state *state = static_cast<ftl_state *>(my_dev->_private);
    // Callers must stop/join their I/O threads before closing this device.
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->stopping = true;
        state->changed.notify_all();
    }
    state->gc_thread.join();
    const int worker_error = state->error;
    std::printf("[stosys-stats] GC reclaimed %llu log zones\n",
                static_cast<unsigned long long>(state->gc_events));
    const int result = close(state->fd);
    const int saved_errno = errno;
    state->fd = -1;
    delete state;
    delete my_dev;
    if (worker_error != 0) return worker_error;
    return result == 0 ? 0 : -(saved_errno != 0 ? saved_errno : EIO);
}

} // extern "C"

