#include "pch.h"

#include <cassert>
#include <Windows.h>

#define byte char*
#define DEBUG

#ifdef DEBUG
#include <iostream>
#include <iomanip>
using namespace std;
#endif


class FSAllocator
{
    enum { MIN_BYTES = 16 };
    enum { CHUNK_SIZE = 4096 };

public:
    FSAllocator() {
        blockSize = 0;
        blocksInited = 0;
        pages = nullptr;
    };

    ~FSAllocator()
    {
        assert(state != State::Destroyed);
        destroy();
    }

    FSAllocator(const FSAllocator&) = delete;
    FSAllocator& operator=(const FSAllocator&) = delete;

    void init(size_t initialSize = MIN_BYTES) {
        blockSize = initialSize;
        void* newChunk = VirtualAlloc(NULL, CHUNK_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        pages = (Page*)newChunk;
        pages->Init((void*)((byte)newChunk + sizeof(Page)));
        state = State::Initialized;
    };

    void destroy() {
        while (pages) {
            auto temp = pages;
            pages = pages->next;
            VirtualFree(temp, 0, MEM_RELEASE);
        }
        blocksInited = 0;
    };

    void* alloc() {
        assert(state == State::Initialized);
        auto curPage = pages;
        while (curPage && !(blocksInited < (CHUNK_SIZE - sizeof(Page)) / blockSize) && curPage->FLIndex != -1) curPage = curPage->next;
        if (!curPage) {
            void* newChunk = VirtualAlloc(NULL, CHUNK_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);


            auto newPage = (Page*)newChunk;
            newPage->Init((void*)((byte)newChunk + sizeof(Page)), pages);
            pages = newPage;
            blocksInited = 0;
            return alloc();
        }

        if (curPage->FLIndex != -1) {
            char* temp = (char*)curPage->chunk + curPage->FLIndex * blockSize;
            curPage->FLIndex = *((int*)temp);
            return (void*)temp;

        }
        else {
            char* temp = (char*)curPage->chunk + blocksInited * blockSize;
            blocksInited++;
            return (void*)temp;
        }
    };

    void free(void* blockForFree) {
        assert(state == State::Initialized);
        auto curPage = pages;
        while (curPage && !((char*)blockForFree >= (char*)curPage->chunk && (char*)curPage->chunk + CHUNK_SIZE - sizeof(Page) > (char*)blockForFree)) {
            curPage = curPage->next;
        }

        if (!curPage) return;
        auto distance = (char*)blockForFree - (char*)curPage->chunk;
        *((int*)blockForFree) = curPage->FLIndex;
        curPage->FLIndex = distance / blockSize;

    };

#ifdef DEBUG
    void dumpStat() const {
        cout << "FSA " << blockSize << " bytes:" << endl;
        cout << "\tFree: ";
        int freeCount = CHUNK_SIZE / blockSize - blocksInited;
        auto curPage = pages;
        int allCount = curPage ? 0 : freeCount;
        while (curPage)
        {
            allCount += CHUNK_SIZE / blockSize;
            auto fIndex = curPage->FLIndex;
            while (fIndex != -1) {
                freeCount++;
                fIndex = *((int*)((char*)curPage->chunk + fIndex * blockSize));
            }
            curPage = curPage->next;
        }
        cout << freeCount << endl;
        cout << "\tEngaged: " << allCount - freeCount << endl;
    };
#endif

private:

    struct Page {
        Page(void* chunk, Page* next = nullptr) {
            this->next = next;
            this->chunk = chunk;
            FLIndex = -1;
        }

        void Init(void* chunk, Page* next = nullptr) {
            this->next = next;
            this->chunk = chunk;
            FLIndex = -1;
        }

        Page* next;
        void* chunk;
        int FLIndex;
    };

    

    Page* pages;
    size_t blockSize;
    size_t blocksInited;


#ifndef NDEBUG
    enum class State
    {
        NotInitialized,
        Initialized,
        Destroyed
    };
    State state = State::NotInitialized;
#endif
};

class CoalesceAllocator {
    struct BlockData
    {
        BlockData(size_t size) {
            this->size = size;
        }
        BlockData* next;
        BlockData* prev;
        size_t size;
    };
    enum { MIN_BYTES = sizeof(BlockData) };
    enum { BUFFER = (1024 * 1024 * 10) };

public:
    CoalesceAllocator() {
        pages = nullptr;
        engagetBlocks = 0;
        engagetSize = 0;
    };

    ~CoalesceAllocator()
    {
        assert(state != State::Destroyed);
        destroy();
    }

    CoalesceAllocator(const CoalesceAllocator&) = delete;
    CoalesceAllocator& operator=(const CoalesceAllocator&) = delete;

    void init() {
        void* newChunk = VirtualAlloc(NULL, BUFFER, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        pages = (Page*)newChunk;
        pages->Init((void*)((byte)newChunk + sizeof(Page)));
        FreeList = ((BlockData*)newChunk);
        FreeList->size = BUFFER - sizeof(Page);
        FreeList->next = nullptr;
        FreeList->prev = nullptr;
    };

    void destroy() {
        while (pages) {
            auto temp = pages;
            pages = pages->next;
            VirtualFree(temp, 0, MEM_RELEASE);
        }
    };

    void* alloc(size_t size) {
        auto saveSize = size;
        size += sizeof(size_t);
        size += (8 - size % 8);

        size = size < MIN_BYTES ? size = MIN_BYTES : size;

        BlockData* first = nullptr;
        for (auto i = FreeList; i; i = i->next) {
            if (i->size >= size) {
                first = i;
                break;
            }
        };

        if (first) {
            if (first->size - size < MIN_BYTES)
                size = first->size;
            if (first->size - size != 0) {
                auto rest = (BlockData*)((char*)first + size);
                rest->next = first->next;
                rest->prev = first->prev;
                rest->size = first->size - size;
                if (first->next) first->next->prev = rest;
                if (first->prev) first->prev->next = rest;
                if (first == FreeList) FreeList = rest;
            }
            else {
                if (first->next) first->next->prev = first->prev;
                if (first->prev) first->prev->next = first->next;
                if (first == FreeList) FreeList = first->next;
            }
            auto mem = (BlockData*)(first)+1;
            engagetBlocks++;
            engagetSize += size;
            return (void*)mem;
        }
        else {
            void* newChunk = VirtualAlloc(NULL, BUFFER, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            pages = (Page*)newChunk;
            auto temp = FreeList;
            FreeList = ((BlockData*)newChunk);
            FreeList->size = BUFFER - sizeof(Page);
            FreeList->next = temp;
            temp->prev = FreeList;

            return alloc(saveSize);
        }
    };

    void free(void* blockForFree) {
        blockForFree = (void*)((BlockData*)blockForFree - 1);
        size_t size = ((BlockData*)blockForFree)->size;
        BlockData* next = nullptr;
        BlockData* prev = nullptr;
        for (auto i = FreeList; i; i = i->next) {
            if ((char*)i + i->size == (char*)blockForFree) prev = i;
            if ((char*)i == (char*)blockForFree + size) next = i;
        };

        if (prev) {
            prev->size += size;
            if (next) {
                prev->size += size;
                if (next->prev) next->prev->next = next->next;
                if (next->next) next->next->prev = next->prev;
                if (FreeList == next) FreeList = next->next;
            }
        }
        else if (next) {
            ((BlockData*)blockForFree)->next = next->next;
            ((BlockData*)blockForFree)->prev = next->prev;
            ((BlockData*)blockForFree)->size += next->size;
            if (next->prev) next->prev->next = (BlockData*)blockForFree;
            if (next->next) next->next->prev = (BlockData*)blockForFree;
            if (FreeList == next) FreeList = (BlockData*)blockForFree;
        }
        else {
            ((BlockData*)blockForFree)->next = FreeList;
            ((BlockData*)blockForFree)->prev = nullptr;
            ((BlockData*)blockForFree)->size += size;
            if (((BlockData*)blockForFree)->next) ((BlockData*)blockForFree)->next->prev = (BlockData*)blockForFree;
            FreeList = (BlockData*)blockForFree;
        }
        engagetBlocks--;
        engagetSize -= size;
    };

#ifdef DEBUG
    void dumpStat() const {

        cout << "CA " << BUFFER << " bytes:" << endl;
        cout << "\tEngaged size: " << engagetSize << endl;
        cout << "\tEngaged: " << engagetBlocks << endl;
    };
#endif

private:

    

    struct Page {
        Page(void* chunk, Page* next = nullptr) {
            this->next = next;
            this->chunk = chunk;
        }

        void Init(void* chunk, Page* next = nullptr) {
            this->next = next;
            this->chunk = chunk;
        }

        Page* next;
        void* chunk;
    };



    Page* pages;
    BlockData* FreeList;

    size_t engagetBlocks;
    size_t engagetSize;

#ifndef NDEBUG
    enum class State
    {
        NotInitialized,
        Initialized,
        Destroyed
    };
    State state = State::NotInitialized;
#endif
};

int sizes[] = { 16, 32, 64, 128, 256, 512 };

class MemoryAllocator
{
    CoalesceAllocator CA;
    FSAllocator FSAs[6];
    void* Base;

    MemoryAllocator(const MemoryAllocator&) = delete;
    MemoryAllocator& operator=(const MemoryAllocator&) = delete;
public:
    MemoryAllocator() {
        
    };
    ~MemoryAllocator() {

    };
    void init() {
        Base = VirtualAlloc(NULL, 1024 * 1024 * 100, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        blocks = nullptr;
        for (auto i = 0; i < 6; i++) {
            FSAs[i].init(sizes[i]);
        }
        CA.init();
    }

    void destroy()
    {
        for (auto i = 0; i < 6; i++) {
            FSAs[i].destroy();
        }
        CA.destroy();
        VirtualFree(Base, 0, MEM_RELEASE);
    }

    void* alloc(size_t nbytes)
    {
        void* result = nullptr;
        if (nbytes >= 10 * 1024 * 1024) {
            void* temp = VirtualAlloc(NULL, nbytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            auto tempBlock = (Block*)((byte)blocks + sizeof(Block) + blocks->size);
            tempBlock->Init(temp, nbytes, blocks);
            blocks = tempBlock;
            return temp;
        }
        if (nbytes >= 512)
            result = CA.alloc(nbytes);
        else {
            for (int i = 0; i < 6; i++) {
                if (nbytes < sizes[i]) {
                    result = FSAs[i].alloc();
                    break;
                }
            }
        }

        if (blocks) {

            auto tempBlock = (Block*)((byte)blocks + sizeof(Block) + blocks->size);
            tempBlock->Init(result, nbytes, blocks);
            blocks = tempBlock;
        }
        else {
            auto tempBlock = (Block*)Base;
            tempBlock->Init(result, nbytes, blocks);
            blocks = tempBlock;
        }

        return result;
    }

    void free(void* blockForFree) {

        auto nextBlock = blocks;
        auto curBlock = blocks;
        if (nextBlock->chunk != blockForFree) {
            while (curBlock && curBlock->next && !((char*)blockForFree == (char*)curBlock->next->chunk)) {
                curBlock = curBlock->next;
            }
            nextBlock = curBlock->next ? curBlock->next : curBlock;
        }

        if (nextBlock) {
            if (nextBlock->size >= 10 * 1024 * 1024) {
                VirtualFree(nextBlock->chunk, 0, MEM_RELEASE);
            }
            else if (nextBlock->size >= 512) {
                CA.free(blockForFree);
            }
            else {
                for (int i = 0; i < 6; i++) {
                    if (nextBlock->size < sizes[i]) {
                        FSAs[i].free(blockForFree);
                    }
                }
            }
        }
        
        if (curBlock == nextBlock) {
            blocks = blocks->next;
        }
        else if (nextBlock) {
            curBlock->next = nextBlock->next;
        }

    };

#ifdef DEBUG
    virtual void dumpStat() const {
        for (int i = 0; i < 6; i++) {
                FSAs[i].dumpStat();
            }
        CA.dumpStat();
        auto statBlocks = blocks;
        while (statBlocks) {
            if (statBlocks->size >= 10 * 1024 * 1024) {
                cout << "OC  block:" << endl;
                cout << "\tEngaged: " << statBlocks->size << endl;
            }
            statBlocks = statBlocks->next;
        }
    };
    virtual void dumpBlocks() const {
        auto statBlocks = blocks;
        cout << "Dump Blocks:" << endl;
        while (statBlocks) {
                cout << "\tBlock: " << (char*)statBlocks->chunk << ", size " << statBlocks->size << endl;
            statBlocks = statBlocks->next;
        }
    };
#endif
private:
    struct Block {
        Block(void* chunk, size_t size, Block* next = nullptr) {
        this->size = size;
        this->next = next;
        this->chunk = chunk;
        }

        void Init(void* chunk, size_t size, Block* next = nullptr) {
            this->size = size;
            this->next = next;
            this->chunk = chunk;
        }

        Block* next;
        size_t size;
        void* chunk;
    };
    Block* blocks = nullptr;
};

TEST(TestCaseName, TestName) {
    MemoryAllocator allocator;
    allocator.init();

    int* pi = (int*)allocator.alloc(sizeof(int));
    double* pd = (double*)allocator.alloc(sizeof(double));
    int* pa = (int*)allocator.alloc(10 * sizeof(int));
    allocator.dumpStat();
    allocator.dumpBlocks();
    allocator.free(pa);
    allocator.free(pd);
    allocator.free(pi);
    allocator.destroy();
  EXPECT_EQ(1, 1);
  EXPECT_TRUE(true);
}

TEST(TestCaseName2, TestName) {
    MemoryAllocator allocator;
    allocator.init();

    char* a = (char*)allocator.alloc(7);
    char* b = (char*)allocator.alloc(128);
    char* c = (char*)allocator.alloc(550);
    char* d = (char*)allocator.alloc(1020);
    char* e = (char*)allocator.alloc(1024*1024*11);
   
    allocator.dumpStat();
    allocator.dumpBlocks();
    allocator.destroy();

    allocator.init();

    char* f = (char*)allocator.alloc(7);
    char* g = (char*)allocator.alloc(128);
    char* h = (char*)allocator.alloc(550);
    char* i = (char*)allocator.alloc(1020);
    char* j = (char*)allocator.alloc(1024 * 1024 * 11);

    allocator.dumpStat();
    allocator.dumpBlocks();
    EXPECT_EQ(1, 1);
    EXPECT_TRUE(true);
}