/*
 * MIT License
Copyright (c) 2021 - current
Authors:  Animesh Trivedi, Malek Kanaan
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

// When to use this file? If by the M3 deadline you can not get your M3 implementation working 
// you can use this file as a backup to continue to finish M4 and M5. 
// IMPORTANT: you will not 100% of points if you choose this option 

#include <cstdlib>
#include <stdio.h>
#include <cerrno>
#include <libnvme.h>
#include "zns_device.h"

#ifdef M3_BACKUP_FILE_IMPL 
extern "C" {

    int init_ss_zns_device(struct zdev_init_params *params, struct user_zns_device **my_dev) {
        struct user_zns_device *dev = (struct user_zns_device *)calloc(1, sizeof(struct user_zns_device));
        *my_dev = dev;

        int fd = nvme_open(params->name);
        if (fd < 0) {
            printf("Failed to open device %s, open returned %d with errno %d", params->name, fd, errno);
            return -fd;
        }

        // try to retrieve the NVMe namespace details
        uint32_t nsid;
        int ret = -1;
        ret = nvme_get_nsid(fd, &nsid);
        if (ret != 0) {
            printf("ERROR: failed to retrieve the nsid %d \n", ret);
            return ret;
        }

        // with the id now we can query the identify namespace - see figure 249, section 5.15.2 in the NVMe specification
        nvme_id_ns ns;
        ret = nvme_identify_ns(fd, nsid, &ns);
        if (ret) {
            printf("ERROR: failed to retrieve the nsid %d \n", ret);
            return ret;
        }

        // extract the in-use LBA size, it could be the case that the device supports multiple LBA size
        dev->tparams.zns_lba_size = 1 << ns.lbaf[(ns.flbas & 0xf)].ds;

        nvme_zns_id_ns zns_ns;

        ret = nvme_zns_identify_ns(fd, nsid, &zns_ns);
        if (ret) {
            printf("ERROR: failed to retrieve the nsid %d \n", ret);
            return ret;
        }

        // get Zone size
        dev->tparams.zns_zone_capacity = le64_to_cpu(zns_ns.lbafe[(ns.flbas & 0xf)].zsze) * dev->tparams.zns_lba_size;

        // -- Find number of zones
        struct nvme_zone_report zns_report;
        ret = nvme_zns_mgmt_recv(fd, nsid, 0,
            NVME_ZNS_ZRA_REPORT_ZONES, NVME_ZNS_ZRAS_REPORT_ALL,
            0, sizeof(zns_report), (void *)&zns_report);
        if (ret != 0) {
            fprintf(stderr, "failed to report zones, ret %d \n", ret);
            return ret;
        }
        // find how many zones, and substract the number of log zones to give an impression of working device 
        dev->tparams.zns_num_zones = (le64_to_cpu(zns_report.nr_zones) - params->log_zones);
        dev->lba_size_bytes = dev->tparams.zns_lba_size;
        dev->capacity_bytes = (dev->tparams.zns_num_zones * dev->tparams.zns_zone_capacity);

        // create a sparse file to emulate the disk
        FILE *fp = fopen(params->name, "w+");
        ret = fseek(fp, dev->capacity_bytes, SEEK_SET);
        if (ret < 0) {
            printf("ERROR: failed to seek to the end of the file %d \n", ret);
            return ret;
        }

        ret = fputc('\0', fp);
        if (ret == EOF) {
            printf("ERROR: failed to write to the end of the file %d \n", ret);
            return ret;
        }

        ret = fseek(fp, 0, SEEK_SET);
        if (ret < 0) {
            printf("ERROR: failed to seek to the start of the file %d \n", ret);
            return ret;
        }

        dev->_private = (void *)fp;
        return 0;
    }

    int zns_udevice_read(struct user_zns_device *my_dev, uint64_t address, void *buffer, uint32_t size) {
        int32_t ret = -1;
        FILE *fp = (FILE *)(my_dev->_private);

        // check whether the address is block aligned

        if (address % my_dev->lba_size_bytes != 0 || size % my_dev->lba_size_bytes != 0) {
            printf("ERROR: read request is not block aligned \n");
            return -EINVAL;
        }

        ret = fseek(fp, address, SEEK_SET);
        if (ret < 0) {
            printf("ERROR: failed to seek to address %lu \n", address);
            return ret;
        }

        if (fread(buffer, 1, size, fp) < size) {
            printf("ERROR: failed to read %u bytes from address %lu \n", size, address);
            return -1;
        }
        return 0;
    }

    int zns_udevice_write(struct user_zns_device *my_dev, uint64_t address, void *buffer, uint32_t size) {
        // check if the write is within the device capacity
        if (address + size > my_dev->capacity_bytes) {
            printf("ERROR: write request is outside the device capacity \n");
            return -EINVAL;
        }

        // check whether the address is block aligned
        if (address % my_dev->lba_size_bytes != 0 || size % my_dev->lba_size_bytes != 0) {
            printf("ERROR: write request is not block aligned \n");
            return -EINVAL;
        }

        int32_t ret = -1;
        FILE *fp = (FILE *)(my_dev->_private);
        ret = fseek(fp, address, SEEK_SET);
        if (ret < 0) {
            printf("ERROR: failed to seek to address %lu \n", address);
            return ret;
        }

        if (fwrite(buffer, 1, size, fp) < size) {
            printf("ERROR: failed to write %u bytes to address %lu \n", size, address);
            return -1;
        }
        return 0;
    }

    int deinit_ss_zns_device(struct user_zns_device *my_dev) {
        fclose((FILE *)(my_dev->_private));
        free(my_dev);
        return 0;
    }
}

#endif 