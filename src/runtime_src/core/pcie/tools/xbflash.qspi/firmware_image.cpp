/**
 * Copyright (C) 2020-2021 Xilinx, Inc
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You may
 * not use this file except in compliance with the License. A copy of the
 * License is located at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

#include <fstream>
#include "firmware_image.h"

firmwareImage::firmwareImage(const char *file) :
    mBuf(nullptr)
{
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in.is_open())
    {
        this->setstate(failbit);
        std::cout << "Can't open " << file << std::endl;
        return;
    }
    auto bufsize = in.tellg();
    in.seekg(0);

    // For non-dsabin file, the entire file is the image.
    //
    // This used to allocate its own buffer and install it with
    // rdbuf()->pubsetbuf(). For a std::stringbuf that call is
    // IMPLEMENTATION-DEFINED: libstdc++ adopts the buffer, libc++ ignores it. On
    // macOS the stream therefore stayed empty, parseMCS found 0 records, and
    // xbflash printed "Erasing flash"/"Programming flash" and exited 0 having sent
    // no command at all - a silent no-op, which is a worse failure than an error.
    // str() is specified and behaves the same on both.
    std::string data(static_cast<size_t>(bufsize), '\0');
    if (bufsize > 0)
        in.read(&data[0], bufsize);
    if (in.gcount() != bufsize) {
        this->setstate(failbit);
        std::cout << "Short read on " << file << ": got " << in.gcount()
                  << " of " << bufsize << " bytes" << std::endl;
        return;
    }
    this->str(data);
}

firmwareImage::~firmwareImage()
{
    delete[] mBuf;
}
