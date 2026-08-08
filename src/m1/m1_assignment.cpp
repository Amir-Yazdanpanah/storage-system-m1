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

#include <assert.h>
#include <errno.h>
#include <stdio.h>

#include "m1_assignment.h"
#include "../common/unused.h"

#include <stdarg.h>

extern "C"
{

int ss_nvme_device_io_with_mdts(
    int fd,
    uint32_t nsid,
    uint64_t slba,
    uint16_t numbers,
    void *buffer,
    uint64_t buf_size,
    uint64_t lba_size,
    uint64_t mdts_size,
    bool read)
{
    if (fd < 0 ||
        buffer == nullptr ||
        numbers == 0 ||
        buf_size == 0 ||
        lba_size == 0 ||
        mdts_size == 0) {
        return -EINVAL;
    }

    if (buf_size != static_cast<uint64_t>(numbers) * lba_size) {
        return -EINVAL;
    }

    uint64_t max_lbas_per_command = mdts_size / lba_size;

    if (max_lbas_per_command == 0) {
        return -EINVAL;
    }

    // nvme_read()/nvme_write() use a 16-bit NLB field.
    // NLB is zero-based, so the largest representable
    // transfer is 65536 LBAs (NLB = 65535).
    if (max_lbas_per_command > 65536) {
        max_lbas_per_command = 65536;
    }

    uint64_t lbas_remaining = numbers;
    uint64_t current_slba = slba;
    uint8_t *current_buffer = static_cast<uint8_t *>(buffer);

    while (lbas_remaining > 0) {
        uint64_t lbas_this_command = (lbas_remaining > max_lbas_per_command) ? max_lbas_per_command : lbas_remaining;
        uint64_t bytes_this_command = lbas_this_command * lba_size;

        int ret;
        if (read) {
            ret = ss_nvme_device_read(
                fd,
                nsid,
                current_slba,
                static_cast<uint16_t>(lbas_this_command),
                current_buffer,
                bytes_this_command
            );
        } else {
            ret = ss_nvme_device_write(
                fd,
                nsid,
                current_slba,
                static_cast<uint16_t>(lbas_this_command),
                current_buffer,
                bytes_this_command
            );
        }

        if (ret != 0) {
            printf("Erorr: %s failed at SLBA 0x%lx, numbers = %lu, ret = %d\n", read ? "read" : "write", current_slba, lbas_this_command, ret);
            return ret;
        }

        // next chunk
        current_slba += lbas_this_command;
        current_buffer += bytes_this_command;
        lbas_remaining -= lbas_this_command;
    }

    return 0;
}

int ss_nvme_device_read(int fd, uint32_t nsid, uint64_t slba, uint16_t numbers, void *buffer, uint64_t buf_size) {
    if (fd < 0 || buffer == nullptr || numbers ==0) {
	return -EINVAL;
	}
    int ret = nvme_read(
	fd,
	nsid,
	slba,
	numbers - 1,
	0,
	0,
	0,
	0,
	0,
	buf_size,
	buffer,
	0,
	NULL
	);

    return ret;
}

int ss_nvme_device_write(int fd, uint32_t nsid, uint64_t slba, uint16_t numbers, void *buffer, uint64_t buf_size) {
	
    if (fd < 0 || buffer == nullptr || numbers == 0) {
	return -EINVAL;
	}

    int ret =  nvme_write(
	fd,
	nsid,
	slba,
	numbers - 1,
	0,
	0,
	0,
	0,
	0,
	0,
	buf_size,
	buffer,
	0,
	NULL
	);

    return ret;
}

int ss_zns_device_zone_reset(int fd, uint32_t nsid, uint64_t slba) {
    
    int ret;
    ret = nvme_zns_mgmt_send(
		    fd,
		    nsid,
		    slba,
		    false,
		    NVME_ZNS_ZSA_RESET,
		    0,
		    NULL
		    );
    
    return ret;
}

// this does not take slba because it will return that
int ss_zns_device_zone_append(int fd, uint32_t nsid, uint64_t zslba, int numbers, void *buffer, uint32_t buf_size, uint64_t *written_slba){
    if (fd < 0 || buffer == nullptr || numbers <= 0) {
	return -EINVAL;
	}

    int ret =  nvme_zns_append(
	fd,
	nsid, 
	zslba, 
	static_cast<uint16_t>(numbers - 1), 
	0,
	0, 
	0, 
	0, 
	numbers * buf_size, 
	buffer, 
	0, 
	NULL, 
	(__u64 *)written_slba
	);

    return ret;
}

void update_lba(uint64_t &write_lba, const uint32_t lba_size, const int count){
    (void)lba_size;
    write_lba += count;
}

// see 5.15.2.2 Identify Controller data structure (CNS 01h)
// see how to pass any number of variables in a C/C++ program https://stackoverflow.com/questions/1579719/variable-number-of-parameters-in-function-in-c
// feel free to pass any relevant function parameter to this function extract MDTS 
// you must return the MDTS as the return value of this function 
/*uint64_t get_mdts_size(int count, ...){    
    UNUSED(count);
    //FIXME 
    return 4096;
}*/

uint64_t get_mdts_size(int count, ...)
{
    // We expect one additional argument:
    // the NVMe device file descriptor.
    if (count < 1) {
        return 0;
    }

    va_list args;
    va_start(args, count);

    int fd = va_arg(args, int);

    va_end(args);

    if (fd < 0) {
        return 0;
    }

    // Identify Controller data structure
    struct nvme_id_ctrl ctrl = {};

    // Ask the NVMe controller for its Identify Controller data.
    int ret = nvme_identify_ctrl(fd, &ctrl);

    if (ret != 0) {
        printf("ERROR: nvme_identify_ctrl failed, ret = %d\n", ret);
        return 0;
    }

    // mdts is an exponent.
    //
    // Maximum Data Transfer Size =
    //
    //     2^mdts * minimum memory page size
    //
    // Our system page size is 4096 bytes.
    uint64_t page_size = 4096;

    uint64_t mdts_size =
        (1ULL << ctrl.mdts) * page_size;

    printf("NVMe controller MDTS field = %u\n", ctrl.mdts);
    printf("Maximum Data Transfer Size = %lu bytes\n", mdts_size);

    return mdts_size;
}

}
