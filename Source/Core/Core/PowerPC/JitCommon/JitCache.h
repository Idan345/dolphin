// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#pragma once

#include <bitset>
#include <map>
#include <vector>

#include "../Gekko.h"
#include "../PPCAnalyst.h"

// Define this in order to get VTune profile support for the Jit generated code.
// Add the VTune include/lib directories to the project directories to get this to build.
// #define USE_VTUNE

// emulate CPU with unlimited instruction cache
// the only way to invalidate a region is the "icbi" instruction
#define JIT_UNLIMITED_ICACHE

#define JIT_ICACHE_SIZE 0x2000000
#define JIT_ICACHE_MASK 0x1ffffff
#define JIT_ICACHEEX_SIZE 0x4000000
#define JIT_ICACHEEX_MASK 0x3ffffff
#define JIT_ICACHE_EXRAM_BIT 0x10000000
#define JIT_ICACHE_VMEM_BIT 0x20000000
// this corresponds to opcode 5 which is invalid in PowerPC
#define JIT_ICACHE_INVALID_BYTE 0x80
#define JIT_ICACHE_INVALID_WORD 0x80808080

struct JitBlock
{
	const u8 *checkedEntry;
	const u8 *normalEntry;

	u8 *exitPtrs[2];     // to be able to rewrite the exit jum
	u32 exitAddress[2];  // 0xFFFFFFFF == unknown

	u32 originalAddress;
	u32 codeSize;
	u32 originalSize;
	int runCount;  // for profiling.
	int flags;

	bool invalid;
	bool linkStatus[2];

#ifdef _WIN32
	// we don't really need to save start and stop
	// TODO (mb2): ticStart and ticStop -> "local var" mean "in block" ... low priority ;)
	u64 ticStart;		// for profiling - time.
	u64 ticStop;		// for profiling - time.
	u64 ticCounter;	// for profiling - time.
#endif

#ifdef USE_VTUNE
	char blockName[32];
#endif
};

typedef void (*CompiledCode)();


class JitBaseBlockCache
{
	const u8 **blockCodePointers;
	JitBlock *blocks;
	int num_blocks;
	std::multimap<u32, int> links_to;
	std::map<std::pair<u32,u32>, u32> block_map; // (end_addr, start_addr) -> number
	std::bitset<0x20000000 / 32> valid_block;
	enum
	{
		MAX_NUM_BLOCKS = 65536*2
	};

	bool RangeIntersect(int s1, int e1, int s2, int e2) const;
	void LinkBlockExits(int i);
	void LinkBlock(int i);
	void UnlinkBlock(int i);

	// Virtual for overloaded
	virtual void WriteLinkBlock(u8* location, const u8* address) = 0;
	virtual void WriteDestroyBlock(const u8* location, u32 address) = 0;

public:
	JitBaseBlockCache() :
		blockCodePointers(0), blocks(0), num_blocks(0),
		iCache(0), iCacheEx(0), iCacheVMEM(0) {}
	int AllocateBlock(u32 em_address);
	void FinalizeBlock(int block_num, bool block_link, const u8 *code_ptr);

	void Clear();
	void ClearSafe();
	void Init();
	void Shutdown();
	void Reset();

	bool IsFull() const;

	// Code Cache
	JitBlock *GetBlock(int block_num);
	int GetNumBlocks() const;
	const u8 **GetCodePointers();
	u8 *iCache;
	u8 *iCacheEx;
	u8 *iCacheVMEM;

	u32* GetICachePtr(u32 addr);

	// Fast way to get a block. Only works on the first ppc instruction of a block.
	int GetBlockNumberFromStartAddress(u32 em_address);

	u32 GetOriginalFirstOp(int block_num);
	CompiledCode GetCompiledCodeFromBlock(int block_num);

	// DOES NOT WORK CORRECTLY WITH INLINING
	void InvalidateICache(u32 address, const u32 length);
	void DestroyBlock(int block_num, bool invalidate);

	// Not currently used
	//void DestroyBlocksWithFlag(BlockFlag death_flag);
};

// x86 BlockCache
class JitBlockCache : public JitBaseBlockCache
{
private:
	void WriteLinkBlock(u8* location, const u8* address) override;
	void WriteDestroyBlock(const u8* location, u32 address) override;
};
