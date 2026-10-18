# Storage System Course Project

This framework code is used for the project development in the Storage Systems (XM_0092) course at VU Amsterdam. 

For more information about the course please see: [https://atlarge-research.com/courses/storage-systems-vu/](https://atlarge-research.com/courses/storage-systems-vu/)

# NVMe/ZNS Storage Stack

My C/C++ implementation of the five practical assignments from the
[VU Amsterdam Storage Systems course](https://animeshtrivedi.github.io/course-stosys/#practical-work),
built on the provided course framework.

1. **Device access:** QEMU-based ZNS emulation and NVMe operations using libnvme.
2. **FTL:** Hybrid log-structured address translation with page and zone mapping.
3. **Garbage collection:** Background cleaning synchronized with foreground I/O.
4. **Filesystem:** Custom filesystem integrated with RocksDB.
5. **Persistence:** Metadata restoration and data verification across orderly restarts.

**Implementation:** Amirhosein Yazdanpanah  
**Technologies:** C/C++ · Linux · QEMU · libnvme · RocksDB

# Authors
Animesh Trivedi, Matthijs Jansen, Krijn Doekemeijer, Sacheendra Talluri, Nick Tehrany, Zebin Ren

# License 
MIT License

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
