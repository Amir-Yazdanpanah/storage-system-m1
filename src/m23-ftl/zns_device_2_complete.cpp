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
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <unistd.h>

namespace {

// Only translation metadata is stored here. User data is never cached.
struct data_zone_mapping {
    uint64_t physical_start;
    bool used;
};

struct ftl_state {
    int fd;
    uint32_t nsid;
    uint32_t lba_bytes;
    uint64_t zone_size_lbas;     // Distance between physical zone starts.
    uint64_t zone_capacity_lbas; // Writable LBAs, which can be less than size.
    uint64_t log_capacity_lbas;
    uint64_t log_used_lbas;
    uint32_t max_active_zones;  // NVMe's zero-based MAR; UINT32_MAX = unlimited.
    int write_error;
    std::unordered_map<uint64_t, uint64_t> page_map;
    std::vector<data_zone_mapping> zone_map;
    std::mutex mutex;

    ftl_state() : fd(-1), nsid(0), lba_bytes(0), zone_size_lbas(0),
        zone_capacity_lbas(0), log_capacity_lbas(0), log_used_lbas(0),
        max_active_zones(0), write_error(0) {}

    ~ftl_state() {
        if (fd >= 0) {
            close(fd);
        }
    }
};

int command_result(int result) {
    // libnvme returns an NVMe status (> 0), or -1 with errno on syscall errors.
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
        address > device->capacity_bytes ||
        size > device->capacity_bytes - address) {
        return -EINVAL;
    }
    return 0;
}

int read_page(ftl_state &state, uint64_t physical_lba, void *buffer) {
    // NLB is zero based: 0 means exactly one LBA. One-page transfers are
    // deliberately conservative, satisfying MDTS without reading PCI BARs.
    return command_result(nvme_read(state.fd, state.nsid, physical_lba,
        0, 0, 0, 0, 0, 0, state.lba_bytes, buffer, 0, nullptr));
}

int write_page(ftl_state &state, uint64_t physical_lba, void *buffer) {
    const int result = command_result(nvme_write(state.fd, state.nsid,
        physical_lba, 0, 0, 0, 0, 0, 0, 0, state.lba_bytes, buffer, 0, nullptr));
    if (result != 0) {
        // A failed command can leave the hardware write pointer uncertain.
        // Do not reuse that physical address or publish a speculative map.
        state.write_error = result;
    }
    return result;
}

bool can_write_data_zone(const ftl_state &state, uint64_t logical_lba,
                         uint64_t pages_left, uint64_t log_used_lbas) {
    const uint64_t capacity = state.zone_capacity_lbas;
    if (logical_lba % capacity != 0 || pages_left < capacity ||
        state.zone_map[logical_lba / capacity].used) {
        return false;
    }
    // A partially filled log is an active zone even after being closed.
    // A controller permitting only one active zone cannot open a data zone
    // until that log is full. In that case retain the page-log path.
    return state.max_active_zones != 0 || log_used_lbas % capacity == 0;
}

int prepare_data_zone(ftl_state &state) {
    if (state.log_used_lbas % state.zone_capacity_lbas == 0) {
        return 0;
    }
    // Keep at most one OPEN zone: close a partial log before opening data.
    // A subsequent sequential write implicitly reopens this log zone.
    const uint64_t log_start = (state.log_used_lbas /
        state.zone_capacity_lbas) * state.zone_size_lbas;
    return command_result(nvme_zns_mgmt_send(state.fd, state.nsid, log_start,
        false, NVME_ZNS_ZSA_CLOSE, 0, nullptr));
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
        state.zone_map[index].physical_start =
            (index + params.log_zones) * state.zone_size_lbas;
        state.zone_map[index].used = false;
    }
    // M2 has no persistent metadata. All requested log zones are available;
    // M5 will need to reserve a metadata zone and adjust this policy.
    state.log_capacity_lbas = params.log_zones * state.zone_capacity_lbas;
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
    if (my_dev == nullptr) {
        return -EINVAL;
    }
    *my_dev = nullptr;
    if (params == nullptr || params->name == nullptr || params->name[0] == '\0' ||
        params->log_zones < 3 || params->gc_wmark < 0) {
        return -EINVAL;
    }
    // Volatile maps cannot be recovered safely. Never pretend to resume, and
    // never reset a namespace when the caller explicitly requested resume.
    if (!params->force_reset) {
        return -EOPNOTSUPP;
    }
    try {
        std::unique_ptr<user_zns_device> device(new user_zns_device());
        std::unique_ptr<ftl_state> state(new ftl_state());
        // Legacy nvme_open expects a basename; also accept the public API's
        // documented /dev/nvmeXnY format when called without the test driver.
        const char *name = params->name;
        if (std::strncmp(name, "/dev/", 5) == 0) {
            name += 5;
        }
        state->fd = nvme_open(name);
        if (state->fd < 0) {
            return -(errno != 0 ? errno : EIO);
        }
        int result = discover_geometry(*state, *params, *device);
        if (result != 0) {
            return result;
        }
        // Destructive reset is performed only after geometry and allocations
        // are validated, and only when explicitly requested by force_reset.
        result = command_result(nvme_zns_mgmt_send(state->fd, state->nsid,
            0, true, NVME_ZNS_ZSA_RESET, 0, nullptr));
        if (result != 0) {
            return result;
        }
        device->_private = state.release();
        *my_dev = device.release();
        return 0;
    } catch (const std::bad_alloc &) {
        return -ENOMEM;
    } catch (const std::length_error &) {
        return -EOVERFLOW;
    }
}

int zns_udevice_read(user_zns_device *my_dev, uint64_t address, void *buffer,
                     uint32_t size) {
    const int validation = validate_io(my_dev, address, buffer, size);
    if (validation != 0 || size == 0) {
        return validation;
    }
    ftl_state &state = *static_cast<ftl_state *>(my_dev->_private);
    std::lock_guard<std::mutex> lock(state.mutex);
    const uint64_t first_lba = address / state.lba_bytes;
    const uint64_t count = size / state.lba_bytes;
    unsigned char *bytes = static_cast<unsigned char *>(buffer);
    for (uint64_t index = 0; index < count; ++index) {
        const uint64_t logical_lba = first_lba + index;
        const auto entry = state.page_map.find(logical_lba);
        const uint64_t physical_lba = entry != state.page_map.end() ?
            entry->second :
            state.zone_map[logical_lba / state.zone_capacity_lbas].physical_start +
                logical_lba % state.zone_capacity_lbas;
        const int result = read_page(state, physical_lba,
            bytes + index * state.lba_bytes);
        if (result != 0) {
            return result;
        }
    }
    return 0;
}

int zns_udevice_write(user_zns_device *my_dev, uint64_t address, void *buffer,
                      uint32_t size) {
    const int validation = validate_io(my_dev, address, buffer, size);
    if (validation != 0 || size == 0) {
        return validation;
    }
    ftl_state &state = *static_cast<ftl_state *>(my_dev->_private);
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.write_error != 0) {
        return state.write_error;
    }
    const uint64_t first_lba = address / state.lba_bytes;
    const uint64_t count = size / state.lba_bytes;
    unsigned char *bytes = static_cast<unsigned char *>(buffer);

    // Preflight space, so ordinary ENOSPC cannot produce a partial write.
    uint64_t log_pages_needed = 0;
    for (uint64_t index = 0; index < count;) {
        if (can_write_data_zone(state, first_lba + index, count - index,
                                state.log_used_lbas + log_pages_needed)) {
            index += state.zone_capacity_lbas;
        } else {
            ++log_pages_needed;
            ++index;
        }
    }
    if (log_pages_needed > state.log_capacity_lbas - state.log_used_lbas) {
        return -ENOSPC; // GC belongs to M3; never erase live data to free space.
    }
    try {
        for (uint64_t index = 0; index < count;) {
            const uint64_t logical_lba = first_lba + index;
            if (can_write_data_zone(state, logical_lba, count - index,
                                    state.log_used_lbas)) {
                int result = prepare_data_zone(state);
                if (result != 0) {
                    return result;
                }
                data_zone_mapping &mapping =
                    state.zone_map[logical_lba / state.zone_capacity_lbas];
                mapping.used = true; // Never retry a partly written data zone.
                for (uint64_t page = 0; page < state.zone_capacity_lbas; ++page) {
                    result = write_page(state, mapping.physical_start + page,
                        bytes + (index + page) * state.lba_bytes);
                    if (result != 0) {
                        return result;
                    }
                }
                // Publish the block version only after all pages are on disk.
                // It supersedes any older page-log version of this range.
                for (uint64_t page = 0; page < state.zone_capacity_lbas; ++page) {
                    state.page_map.erase(logical_lba + page);
                }
                index += state.zone_capacity_lbas;
                continue;
            }
            const uint64_t physical_lba =
                (state.log_used_lbas / state.zone_capacity_lbas) *
                    state.zone_size_lbas +
                state.log_used_lbas % state.zone_capacity_lbas;
            // Allocate metadata BEFORE issuing I/O. A failed write must retain
            // an existing mapping, or remove an uncommitted new entry.
            auto slot = state.page_map.emplace(logical_lba, uint64_t(0));
            const int result = write_page(state, physical_lba,
                bytes + index * state.lba_bytes);
            if (result != 0) {
                if (slot.second) {
                    state.page_map.erase(slot.first);
                }
                return result;
            }
            slot.first->second = physical_lba;
            ++state.log_used_lbas;
            ++index;
        }
        return 0;
    } catch (const std::bad_alloc &) {
        return -ENOMEM;
    } catch (const std::length_error &) {
        return -EOVERFLOW;
    }
}

int deinit_ss_zns_device(user_zns_device *my_dev) {
    if (my_dev == nullptr || my_dev->_private == nullptr) {
        return -EINVAL;
    }
    // As for close(2), callers must first stop/join threads using this device.
    ftl_state *state = static_cast<ftl_state *>(my_dev->_private);
    const int result = close(state->fd);
    const int saved_errno = errno;
    state->fd = -1;
    delete state;
    delete my_dev;
    return result == 0 ? 0 : -(saved_errno != 0 ? saved_errno : EIO);
}

} // extern "C"

