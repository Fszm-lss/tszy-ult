#ifndef memchunk_hpp
#define memchunk_hpp

#include <cstring>
#include "mem_alloc.hpp"

namespace zbf {

struct memchunk : object_tracker<memchunk> {
    unsigned char* buffer;
    unsigned int buffersize;
    unsigned int datasize;
    unsigned int blocksize;

    memchunk(unsigned int blksz) {
        buffersize = blocksize = blksz;
        buffer = (unsigned char*) ZBF_MALLOC(buffersize);
        datasize = 0;
    }

    ~memchunk() {
        if (buffer) ZBF_FREE(buffer);
        buffer = nullptr;
    }

    int push(unsigned char* data, unsigned int sz) {
        if (!data || sz == 0) return -1;
        unsigned int target_datasize = datasize + sz;
        if (target_datasize > buffersize) {
            buffersize = (target_datasize/blocksize + 1) * blocksize;
            buffer = (unsigned char*) ZBF_REALLOC(buffer, buffersize);
        }
        if (!buffer) return -2;
        memcpy(buffer+datasize, data, sz);
        datasize = target_datasize;
        return datasize;
    }

    int pop(unsigned int sz) {
        if (sz > datasize || sz == 0) return -1;
        if (sz == datasize)
            memset(buffer, 0, datasize);
        else // sz < datasize
            memmove(buffer, buffer+sz, datasize-sz);
        datasize -= sz;
        return datasize;
    }
};

}

#endif
