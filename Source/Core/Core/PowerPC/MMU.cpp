// Copyright 2003 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Common/Atomic.h"
#include "Common/BitSet.h"
#include "Common/CommonTypes.h"

#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/CPU.h"
#include "Core/HW/GPFifo.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"

#include "VideoCommon/VideoBackendBase.h"

#ifdef USE_GDBSTUB
#include "Core/PowerPC/GDBStub.h"
#endif

namespace PowerPC
{
#define HW_PAGE_SIZE 4096

// EFB RE
/*
GXPeekZ
80322de8: rlwinm    r0, r3, 2, 14, 29 (0003fffc)   a =  x << 2 & 0x3fffc
80322dec: oris      r0, r0, 0xC800                 a |= 0xc8000000
80322df0: rlwinm    r3, r0, 0, 20, 9 (ffc00fff)    x = a & 0xffc00fff
80322df4: rlwinm    r0, r4, 12, 4, 19 (0ffff000)   a = (y << 12) & 0x0ffff000;
80322df8: or        r0, r3, r0                     a |= x;
80322dfc: rlwinm    r0, r0, 0, 10, 7 (ff3fffff)    a &= 0xff3fffff
80322e00: oris      r3, r0, 0x0040                 x = a | 0x00400000
80322e04: lwz       r0, 0 (r3)                     r0 = *r3
80322e08: stw       r0, 0 (r5)                     z =
80322e0c: blr
*/

// =================================
// From Memmap.cpp
// ----------------

// Overloaded byteswap functions, for use within the templated functions below.
inline u8 bswap(u8 val)
{
	return val;
}
inline s8 bswap(s8 val)
{
	return val;
}
inline u16 bswap(u16 val)
{
	return Common::swap16(val);
}
inline s16 bswap(s16 val)
{
	return Common::swap16(val);
}
inline u32 bswap(u32 val)
{
	return Common::swap32(val);
}
inline u64 bswap(u64 val)
{
	return Common::swap64(val);
}
// =================

enum XCheckTLBFlag
{
	FLAG_NO_EXCEPTION,
	FLAG_READ,
	FLAG_WRITE,
	FLAG_OPCODE,
	FLAG_OPCODE_NO_EXCEPTION
};

static bool IsOpcodeFlag(XCheckTLBFlag flag)
{
	return flag == FLAG_OPCODE || flag == FLAG_OPCODE_NO_EXCEPTION;
}

static bool IsNoExceptionFlag(XCheckTLBFlag flag)
{
	return flag == FLAG_NO_EXCEPTION || flag == FLAG_OPCODE_NO_EXCEPTION;
}

struct TranslateAddressResult
{
	enum
	{
		BAT_TRANSLATED,
		PAGE_TABLE_TRANSLATED,
		DIRECT_STORE_SEGMENT,
		PAGE_FAULT
	} result;
	u32 address;
	bool Success() const { return result <= PAGE_TABLE_TRANSLATED; }
};
template <const XCheckTLBFlag flag>
static TranslateAddressResult TranslateAddress(const u32 address);

// Nasty but necessary. Super Mario Galaxy pointer relies on this stuff.
static u32 EFB_Read(const u32 addr)
{
	u32 var = 0;
	// Convert address to coordinates. It's possible that this should be done
	// differently depending on color depth, especially regarding PEEK_COLOR.
	int x = (addr & 0xfff) >> 2;
	int y = (addr >> 12) & 0x3ff;

	if (addr & 0x00800000)
	{
		ERROR_LOG(MEMMAP, "Unimplemented Z+Color EFB read @ 0x%08x", addr);
	}
	else if (addr & 0x00400000)
	{
		var = g_video_backend->Video_AccessEFB(PEEK_Z, x, y, 0);
		DEBUG_LOG(MEMMAP, "EFB Z Read @ %i, %i\t= 0x%08x", x, y, var);
	}
	else
	{
		var = g_video_backend->Video_AccessEFB(PEEK_COLOR, x, y, 0);
		DEBUG_LOG(MEMMAP, "EFB Color Read @ %i, %i\t= 0x%08x", x, y, var);
	}

	return var;
}

static void EFB_Write(u32 data, u32 addr)
{
	int x = (addr & 0xfff) >> 2;
	int y = (addr >> 12) & 0x3ff;

	if (addr & 0x00800000)
	{
		// It's possible to do a z-tested write to EFB by writing a 64bit value to this address range.
		// Not much is known, but let's at least get some loging.
		ERROR_LOG(MEMMAP, "Unimplemented Z+Color EFB write. %08x @ 0x%08x", data, addr);
	}
	else if (addr & 0x00400000)
	{
		g_video_backend->Video_AccessEFB(POKE_Z, x, y, data);
		DEBUG_LOG(MEMMAP, "EFB Z Write %08x @ %i, %i", data, x, y);
	}
	else
	{
		g_video_backend->Video_AccessEFB(POKE_COLOR, x, y, data);
		DEBUG_LOG(MEMMAP, "EFB Color Write %08x @ %i, %i", data, x, y);
	}
}

BatTable ibat_table;
BatTable dbat_table;

static void GenerateDSIException(u32 _EffectiveAddress, bool _bWrite);

template <XCheckTLBFlag flag, typename T, bool never_translate = false>
__forceinline static T ReadFromHardware(u32 em_address)
{
	if (!never_translate && UReg_MSR(MSR).DR)
	{
		auto translated_addr = TranslateAddress<flag>(em_address);
		if (!translated_addr.Success())
		{
			if (flag == FLAG_READ)
				GenerateDSIException(em_address, false);
			return 0;
		}
		if ((em_address & (HW_PAGE_SIZE - 1)) > HW_PAGE_SIZE - sizeof(T))
		{
			// This could be unaligned down to the byte level... hopefully this is rare, so doing it this
			// way isn't too terrible.
			// TODO: floats on non-word-aligned boundaries should technically cause alignment exceptions.
			// Note that "word" means 32-bit, so paired singles or doubles might still be 32-bit aligned!
			u32 em_address_next_page = (em_address + sizeof(T) - 1) & ~(HW_PAGE_SIZE - 1);
			auto addr_next_page = TranslateAddress<flag>(em_address_next_page);
			if (!addr_next_page.Success())
			{
				if (flag == FLAG_READ)
					GenerateDSIException(em_address_next_page, false);
				return 0;
			}
			T var = 0;
			u32 addr_translated = translated_addr.address;
			for (u32 addr = em_address; addr < em_address + sizeof(T); addr++, addr_translated++)
			{
				if (addr == em_address_next_page)
					addr_translated = addr_next_page.address;
				var = (var << 8) | ReadFromHardware<flag, u8, true>(addr_translated);
			}
			return var;
		}
		em_address = translated_addr.address;
	}

	// TODO: Make sure these are safe for unaligned addresses.

	// Locked L1 technically doesn't have a fixed address, but games all use 0xE0000000.
	if ((em_address >> 28) == 0xE && (em_address < (0xE0000000 + Memory::L1_CACHE_SIZE)))
	{
		return bswap((*(const T*)&Memory::m_pL1Cache[em_address & 0x0FFFFFFF]));
	}
	// In Fake-VMEM mode, we need to map the memory somewhere into
	// physical memory for BAT translation to work; we currently use
	// [0x7E000000, 0x80000000).
	if (Memory::bFakeVMEM && ((em_address & 0xFE000000) == 0x7E000000))
	{
		return bswap(*(T*)&Memory::m_pFakeVMEM[em_address & Memory::RAM_MASK]);
	}

	if (flag == FLAG_READ && (em_address & 0xF8000000) == 0x08000000)
	{
		if (em_address < 0x0c000000)
			return EFB_Read(em_address);
		else
			return (T)Memory::mmio_mapping->Read<typename std::make_unsigned<T>::type>(em_address);
	}

	if ((em_address & 0xF8000000) == 0x00000000)
	{
		// Handle RAM; the masking intentionally discards bits (essentially creating
		// mirrors of memory).
		// TODO: Only the first REALRAM_SIZE is supposed to be backed by actual memory.
		return bswap((*(const T*)&Memory::m_pRAM[em_address & Memory::RAM_MASK]));
	}

	if (Memory::m_pEXRAM && (em_address >> 28) == 0x1 &&
		(em_address & 0x0FFFFFFF) < Memory::EXRAM_SIZE)
	{
		return bswap((*(const T*)&Memory::m_pEXRAM[em_address & 0x0FFFFFFF]));
	}

	PanicAlert("Unable to resolve read address %x PC %x", em_address, PC);
	return 0;
}

template <XCheckTLBFlag flag, typename T, bool never_translate = false>
__forceinline static void WriteToHardware(u32 em_address, const T data)
{
	if (!never_translate && UReg_MSR(MSR).DR)
	{
		auto translated_addr = TranslateAddress<flag>(em_address);
		if (!translated_addr.Success())
		{
			if (flag == FLAG_WRITE)
				GenerateDSIException(em_address, true);
			return;
		}
		if ((em_address & (sizeof(T) - 1)) &&
			(em_address & (HW_PAGE_SIZE - 1)) > HW_PAGE_SIZE - sizeof(T))
		{
			// This could be unaligned down to the byte level... hopefully this is rare, so doing it this
			// way isn't too terrible.
			// TODO: floats on non-word-aligned boundaries should technically cause alignment exceptions.
			// Note that "word" means 32-bit, so paired singles or doubles might still be 32-bit aligned!
			u32 em_address_next_page = (em_address + sizeof(T) - 1) & ~(HW_PAGE_SIZE - 1);
			auto addr_next_page = TranslateAddress<flag>(em_address_next_page);
			if (!addr_next_page.Success())
			{
				if (flag == FLAG_WRITE)
					GenerateDSIException(em_address_next_page, true);
				return;
			}
			T val = bswap(data);
			u32 addr_translated = translated_addr.address;
			for (u32 addr = em_address; addr < em_address + sizeof(T);
				addr++, addr_translated++, val >>= 8)
			{
				if (addr == em_address_next_page)
					addr_translated = addr_next_page.address;
				WriteToHardware<flag, u8, true>(addr_translated, (u8)val);
			}
			return;
		}
		em_address = translated_addr.address;
	}

	// TODO: Make sure these are safe for unaligned addresses.

	// Locked L1 technically doesn't have a fixed address, but games all use 0xE0000000.
	if ((em_address >> 28 == 0xE) && (em_address < (0xE0000000 + Memory::L1_CACHE_SIZE)))
	{
		*(T*)&Memory::m_pL1Cache[em_address & 0x0FFFFFFF] = bswap(data);
		return;
	}

	// In Fake-VMEM mode, we need to map the memory somewhere into
	// physical memory for BAT translation to work; we currently use
	// [0x7E000000, 0x80000000).
	if (Memory::bFakeVMEM && ((em_address & 0xFE000000) == 0x7E000000))
	{
		*(T*)&Memory::m_pFakeVMEM[em_address & Memory::RAM_MASK] = bswap(data);
		return;
	}

	// Check for a gather pipe write.
	// Note that we must mask the address to correctly emulate certain games;
	// Pac-Man World 3 in particular is affected by this.
	if (flag == FLAG_WRITE && (em_address & 0xFFFFF000) == 0x0C008000)
	{
		switch (sizeof(T))
		{
		case 1:
			GPFifo::Write8((u8)data);
			return;
		case 2:
			GPFifo::Write16((u16)data);
			return;
		case 4:
			GPFifo::Write32((u32)data);
			return;
		case 8:
			GPFifo::Write64((u64)data);
			return;
		}
	}

	if (flag == FLAG_WRITE && (em_address & 0xF8000000) == 0x08000000)
	{
		if (em_address < 0x0c000000)
		{
			EFB_Write((u32)data, em_address);
			return;
		}
		else
		{
			Memory::mmio_mapping->Write(em_address, data);
			return;
		}
	}

	if ((em_address & 0xF8000000) == 0x00000000)
	{
		// Handle RAM; the masking intentionally discards bits (essentially creating
		// mirrors of memory).
		// TODO: Only the first REALRAM_SIZE is supposed to be backed by actual memory.
		*(T*)&Memory::m_pRAM[em_address & Memory::RAM_MASK] = bswap(data);
		return;
	}

	if (Memory::m_pEXRAM && (em_address >> 28) == 0x1 &&
		(em_address & 0x0FFFFFFF) < Memory::EXRAM_SIZE)
	{
		*(T*)&Memory::m_pEXRAM[em_address & 0x0FFFFFFF] = bswap(data);
		return;
	}

	PanicAlert("Unable to resolve write address %x PC %x", em_address, PC);
	return;
}
// =====================

// =================================
/* These functions are primarily called by the Interpreter functions and are routed to the correct
	 location through ReadFromHardware and WriteToHardware */
	 // ----------------

static void GenerateISIException(u32 effective_address);

u32 Read_Opcode(u32 address)
{
	TryReadInstResult result = TryReadInstruction(address);
	if (!result.valid)
	{
		GenerateISIException(address);
		return 0;
	}
	return result.hex;
}

TryReadInstResult TryReadInstruction(u32 address)
{
	bool from_bat = true;
	if (UReg_MSR(MSR).IR)
	{
		auto tlb_addr = TranslateAddress<FLAG_OPCODE>(address);
		if (!tlb_addr.Success())
		{
			return TryReadInstResult{ false, false, 0 };
		}
		else
		{
			address = tlb_addr.address;
			from_bat = tlb_addr.result == TranslateAddressResult::BAT_TRANSLATED;
		}
	}

	u32 hex;
	// TODO: Refactor this. This icache implementation is totally wrong if used with the fake vmem.
	if (Memory::bFakeVMEM && ((address & 0xFE000000) == 0x7E000000))
	{
		hex = bswap(*(const u32*)&Memory::m_pFakeVMEM[address & Memory::FAKEVMEM_MASK]);
	}
	else
	{
		hex = PowerPC::ppcState.iCache.ReadInstruction(address);
	}
	return TryReadInstResult{ true, from_bat, hex };
}

u32 HostRead_Instruction(const u32 address)
{
	UGeckoInstruction inst = HostRead_U32(address);
	return inst.hex;
}

static __forceinline void Memcheck(u32 address, u32 var, bool write, int size)
{
	if (PowerPC::memchecks.HasAny())
	{
		TMemCheck* mc = PowerPC::memchecks.GetMemCheck(address);
		if (mc)
		{
			if (CPU::IsStepping())
			{
				// Disable when stepping so that resume works.
				return;
			}
			mc->numHits++;
			bool pause = mc->Action(&PowerPC::debug_interface, var, address, write, size, PC);
			if (pause)
			{
				CPU::Break();
				// Fake a DSI so that all the code that tests for it in order to skip
				// the rest of the instruction will apply.  (This means that
				// watchpoints will stop the emulator before the offending load/store,
				// not after like GDB does, but that's better anyway.  Just need to
				// make sure resuming after that works.)
				// It doesn't matter if ReadFromHardware triggers its own DSI because
				// we'll take it after resuming.
				PowerPC::ppcState.Exceptions |= EXCEPTION_DSI | EXCEPTION_FAKE_MEMCHECK_HIT;
			}
		}
	}
}

u8 Read_U8(const u32 address)
{
	u8 var = ReadFromHardware<FLAG_READ, u8>(address);
	Memcheck(address, var, false, 1);
	return (u8)var;
}

u16 Read_U16(const u32 address)
{
	u16 var = ReadFromHardware<FLAG_READ, u16>(address);
	Memcheck(address, var, false, 2);
	return (u16)var;
}

u32 Read_U32(const u32 address)
{
	u32 var = ReadFromHardware<FLAG_READ, u32>(address);
	Memcheck(address, var, false, 4);
	return var;
}

u64 Read_U64(const u32 address)
{
	u64 var = ReadFromHardware<FLAG_READ, u64>(address);
	Memcheck(address, (u32)var, false, 8);
	return var;
}

double Read_F64(const u32 address)
{
	union {
		u64 i;
		double d;
	} cvt;

	cvt.i = Read_U64(address);
	return cvt.d;
}

float Read_F32(const u32 address)
{
	union {
		u32 i;
		float d;
	} cvt;

	cvt.i = Read_U32(address);
	return cvt.d;
}

u32 Read_U8_ZX(const u32 address)
{
	return (u32)Read_U8(address);
}

u32 Read_U16_ZX(const u32 address)
{
	return (u32)Read_U16(address);
}

void Write_U8(const u8 var, const u32 address)
{
	Memcheck(address, var, true, 1);
	WriteToHardware<FLAG_WRITE, u8>(address, var);
}

void Write_U16(const u16 var, const u32 address)
{
	Memcheck(address, var, true, 2);
	WriteToHardware<FLAG_WRITE, u16>(address, var);
}
void Write_U16_Swap(const u16 var, const u32 address)
{
	Memcheck(address, var, true, 2);
	Write_U16(Common::swap16(var), address);
}

void Write_U32(const u32 var, const u32 address)
{
	Memcheck(address, var, true, 4);
	WriteToHardware<FLAG_WRITE, u32>(address, var);
}
void Write_U32_Swap(const u32 var, const u32 address)
{
	Memcheck(address, var, true, 4);
	Write_U32(Common::swap32(var), address);
}

void Write_U64(const u64 var, const u32 address)
{
	Memcheck(address, (u32)var, true, 8);
	WriteToHardware<FLAG_WRITE, u64>(address, var);
}
void Write_U64_Swap(const u64 var, const u32 address)
{
	Memcheck(address, (u32)var, true, 8);
	Write_U64(Common::swap64(var), address);
}

void Write_F64(const double var, const u32 address)
{
	union {
		u64 i;
		double d;
	} cvt;
	cvt.d = var;
	Write_U64(cvt.i, address);
}

u8 HostRead_U8(const u32 address)
{
	u8 var = ReadFromHardware<FLAG_NO_EXCEPTION, u8>(address);
	return var;
}

u16 HostRead_U16(const u32 address)
{
	u16 var = ReadFromHardware<FLAG_NO_EXCEPTION, u16>(address);
	return var;
}

u32 HostRead_U32(const u32 address)
{
	u32 var = ReadFromHardware<FLAG_NO_EXCEPTION, u32>(address);
	return var;
}

u64 HostRead_U64(const u32 address)
{
	return ReadFromHardware<FLAG_NO_EXCEPTION, u64>(address);
}

void HostWrite_U8(const u8 var, const u32 address)
{
	WriteToHardware<FLAG_NO_EXCEPTION, u8>(address, var);
}

void HostWrite_U16(const u16 var, const u32 address)
{
	WriteToHardware<FLAG_NO_EXCEPTION, u16>(address, var);
}

void HostWrite_U32(const u32 var, const u32 address)
{
	WriteToHardware<FLAG_NO_EXCEPTION, u32>(address, var);
}

void HostWrite_U64(const u64 var, const u32 address)
{
	WriteToHardware<FLAG_NO_EXCEPTION, u64>(address, var);
}

std::string HostGetString(u32 address, size_t size)
{
	std::string s;
	do
	{
		if (!HostIsRAMAddress(address))
			break;
		u8 res = HostRead_U8(address);
		if (!res)
			break;
		s += static_cast<char>(res);
		++address;
	} while (size == 0 || s.length() < size);
	return s;
}

bool IsOptimizableRAMAddress(const u32 address)
{
	if (PowerPC::memchecks.HasAny())
		return false;

	if (!UReg_MSR(MSR).DR)
		return false;

	// TODO: This API needs to take an access size
	//
	// We store whether an access can be optimized to an unchecked access
	// in dbat_table.
	u32 bat_result = dbat_table[address >> BAT_INDEX_SHIFT];
	return (bat_result & 2) != 0;
}

bool HostIsRAMAddress(u32 address)
{
	bool performTranslation = UReg_MSR(MSR).DR;
	int segment = address >> 28;
	if (performTranslation)
	{
		auto translate_address = TranslateAddress<FLAG_NO_EXCEPTION>(address);
		if (!translate_address.Success())
			return false;
		address = translate_address.address;
		segment = address >> 28;
	}

	if (segment == 0x0 && (address & 0x0FFFFFFF) < Memory::REALRAM_SIZE)
		return true;
	else if (Memory::m_pEXRAM && segment == 0x1 && (address & 0x0FFFFFFF) < Memory::EXRAM_SIZE)
		return true;
	else if (Memory::bFakeVMEM && ((address & 0xFE000000) == 0x7E000000))
		return true;
	else if (segment == 0xE && (address < (0xE0000000 + Memory::L1_CACHE_SIZE)))
		return true;
	return false;
}

void DMA_LCToMemory(const u32 memAddr, const u32 cacheAddr, const u32 numBlocks)
{
	// TODO: It's not completely clear this is the right spot for this code;
	// what would happen if, for example, the DVD drive tried to write to the EFB?
	// TODO: This is terribly slow.
	// TODO: Refactor.
	// Avatar: The Last Airbender (GC) uses this for videos.
	if ((memAddr & 0x0F000000) == 0x08000000)
	{
		for (u32 i = 0; i < 32 * numBlocks; i += 4)
		{
			u32 data = bswap(*(u32*)(Memory::m_pL1Cache + ((cacheAddr + i) & 0x3FFFF)));
			EFB_Write(data, memAddr + i);
		}
		return;
	}

	// No known game uses this; here for completeness.
	// TODO: Refactor.
	if ((memAddr & 0x0F000000) == 0x0C000000)
	{
		for (u32 i = 0; i < 32 * numBlocks; i += 4)
		{
			u32 data = bswap(*(u32*)(Memory::m_pL1Cache + ((cacheAddr + i) & 0x3FFFF)));
			Memory::mmio_mapping->Write(memAddr + i, data);
		}
		return;
	}

	const u8* src = Memory::m_pL1Cache + (cacheAddr & 0x3FFFF);
	u8* dst = Memory::GetPointer(memAddr);
	if (dst == nullptr)
		return;

	memcpy(dst, src, 32 * numBlocks);
}

void DMA_MemoryToLC(const u32 cacheAddr, const u32 memAddr, const u32 numBlocks)
{
	const u8* src = Memory::GetPointer(memAddr);
	u8* dst = Memory::m_pL1Cache + (cacheAddr & 0x3FFFF);

	// No known game uses this; here for completeness.
	// TODO: Refactor.
	if ((memAddr & 0x0F000000) == 0x08000000)
	{
		for (u32 i = 0; i < 32 * numBlocks; i += 4)
		{
			u32 data = EFB_Read(memAddr + i);
			*(u32*)(Memory::m_pL1Cache + ((cacheAddr + i) & 0x3FFFF)) = bswap(data);
		}
		return;
	}

	// No known game uses this.
	// TODO: Refactor.
	if ((memAddr & 0x0F000000) == 0x0C000000)
	{
		for (u32 i = 0; i < 32 * numBlocks; i += 4)
		{
			u32 data = Memory::mmio_mapping->Read<u32>(memAddr + i);
			*(u32*)(Memory::m_pL1Cache + ((cacheAddr + i) & 0x3FFFF)) = bswap(data);
		}
		return;
	}

	if (src == nullptr)
		return;

	memcpy(dst, src, 32 * numBlocks);
}

void ClearCacheLine(u32 address)
{
	_dbg_assert_(POWERPC, (address & 0x1F) == 0);
	if (UReg_MSR(MSR).DR)
	{
		auto translated_address = TranslateAddress<FLAG_WRITE>(address);
		if (translated_address.result == TranslateAddressResult::DIRECT_STORE_SEGMENT)
		{
			// dcbz to direct store segments is ignored. This is a little
			// unintuitive, but this is consistent with both console and the PEM.
			// Advance Game Port crashes if we don't emulate this correctly.
			return;
		}
		if (translated_address.result == TranslateAddressResult::PAGE_FAULT)
		{
			// If translation fails, generate a DSI.
			GenerateDSIException(address, true);
			return;
		}
		address = translated_address.address;
	}

	// TODO: This isn't precisely correct for non-RAM regions, but the difference
	// is unlikely to matter.
	for (u32 i = 0; i < 32; i += 8)
		WriteToHardware<FLAG_WRITE, u64, true>(address + i, 0);
}

u32 IsOptimizableMMIOAccess(u32 address, u32 accessSize)
{
	if (PowerPC::memchecks.HasAny())
		return 0;

	if (!UReg_MSR(MSR).DR)
		return 0;

	// Translate address
	// If we also optimize for TLB mappings, we'd have to clear the
	// JitCache on each TLB invalidation.
	if (!TranslateBatAddess(dbat_table, &address))
		return 0;

	// Check whether the address is an aligned address of an MMIO register.
	bool aligned = (address & ((accessSize >> 3) - 1)) == 0;
	if (!aligned || !MMIO::IsMMIOAddress(address))
		return 0;

	return address;
}

bool IsOptimizableGatherPipeWrite(u32 address)
{
	if (PowerPC::memchecks.HasAny())
		return 0;

	if (!UReg_MSR(MSR).DR)
		return 0;

	// Translate address, only check BAT mapping.
	// If we also optimize for TLB mappings, we'd have to clear the
	// JitCache on each TLB invalidation.
	if (!TranslateBatAddess(dbat_table, &address))
		return 0;

	// Check whether the translated address equals the address in WPAR.
	return address == 0x0C008000;
}

TranslateResult JitCache_TranslateAddress(u32 address)
{
	if (!UReg_MSR(MSR).IR)
		return TranslateResult{ true, true, address };

	// TODO: We shouldn't use FLAG_OPCODE if the caller is the debugger.
	auto tlb_addr = TranslateAddress<FLAG_OPCODE>(address);
	if (!tlb_addr.Success())
	{
		return TranslateResult{ false, false, 0 };
	}

	bool from_bat = tlb_addr.result == TranslateAddressResult::BAT_TRANSLATED;
	return TranslateResult{ true, from_bat, tlb_addr.address };
}

// *********************************************************************************
// Warning: Test Area
//
// This code is for TESTING and it works in interpreter mode ONLY. Some games (like
// COD iirc) work thanks to this basic TLB emulation.
// It is just a small hack and we have never spend enough time to finalize it.
// Cheers PearPC!
//
// *********************************************************************************

/*
* PearPC
* ppc_mmu.cc
*
* Copyright (C) 2003, 2004 Sebastian Biallas (sb@biallas.net)
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2 as
* published by the Free Software Foundation.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#define PPC_EXC_DSISR_PAGE (1 << 30)
#define PPC_EXC_DSISR_PROT (1 << 27)
#define PPC_EXC_DSISR_STORE (1 << 25)

#define SDR1_HTABORG(v) (((v) >> 16) & 0xffff)
#define SDR1_HTABMASK(v) ((v)&0x1ff)
#define SDR1_PAGETABLE_BASE(v) ((v)&0xffff)
#define SR_T (1 << 31)
#define SR_Ks (1 << 30)
#define SR_Kp (1 << 29)
#define SR_N (1 << 28)
#define SR_VSID(v) ((v)&0xffffff)
#define SR_BUID(v) (((v) >> 20) & 0x1ff)
#define SR_CNTRL_SPEC(v) ((v)&0xfffff)

#define EA_SR(v) (((v) >> 28) & 0xf)
#define EA_PageIndex(v) (((v) >> 12) & 0xffff)
#define EA_Offset(v) ((v)&0xfff)
#define EA_API(v) (((v) >> 22) & 0x3f)

#define PA_RPN(v) (((v) >> 12) & 0xfffff)
#define PA_Offset(v) ((v)&0xfff)

#define PTE1_V (1 << 31)
#define PTE1_VSID(v) (((v) >> 7) & 0xffffff)
#define PTE1_H (1 << 6)
#define PTE1_API(v) ((v)&0x3f)

#define PTE2_RPN(v) ((v)&0xfffff000)
#define PTE2_R (1 << 8)
#define PTE2_C (1 << 7)
#define PTE2_WIMG(v) (((v) >> 3) & 0xf)
#define PTE2_PP(v) ((v)&3)

// Hey! these duplicate a structure in Gekko.h
union UPTE1 {
	struct
	{
		u32 API : 6;
		u32 H : 1;
		u32 VSID : 24;
		u32 V : 1;
	};
	u32 Hex;
};

union UPTE2 {
	struct
	{
		u32 PP : 2;
		u32 : 1;
					u32 WIMG : 4;
					u32 C : 1;
					u32 R : 1;
					u32 : 3;
								u32 RPN : 20;
	};
	u32 Hex;
};

static void GenerateDSIException(u32 effectiveAddress, bool write)
{
	// DSI exceptions are only supported in MMU mode.
	if (!SConfig::GetInstance().bMMU)
	{
		PanicAlert("Invalid %s 0x%08x, PC = 0x%08x ", write ? "write to" : "read from",
			effectiveAddress, PC);
		return;
	}

	if (effectiveAddress)
		PowerPC::ppcState.spr[SPR_DSISR] = PPC_EXC_DSISR_PAGE | PPC_EXC_DSISR_STORE;
	else
		PowerPC::ppcState.spr[SPR_DSISR] = PPC_EXC_DSISR_PAGE;

	PowerPC::ppcState.spr[SPR_DAR] = effectiveAddress;

	PowerPC::ppcState.Exceptions |= EXCEPTION_DSI;
}

static void GenerateISIException(u32 _EffectiveAddress)
{
	// Address of instruction could not be translated
	NPC = _EffectiveAddress;

	PowerPC::ppcState.Exceptions |= EXCEPTION_ISI;
	WARN_LOG(POWERPC, "ISI exception at 0x%08x", PC);
}

void SDRUpdated()
{
	u32 htabmask = SDR1_HTABMASK(PowerPC::ppcState.spr[SPR_SDR]);
	u32 x = 1;
	u32 xx = 0;
	int n = 0;
	while ((htabmask & x) && (n < 9))
	{
		n++;
		xx |= x;
		x <<= 1;
	}
	if (htabmask & ~xx)
	{
		return;
	}
	u32 htaborg = SDR1_HTABORG(PowerPC::ppcState.spr[SPR_SDR]);
	if (htaborg & xx)
	{
		return;
	}
	PowerPC::ppcState.pagetable_base = htaborg << 16;
	PowerPC::ppcState.pagetable_hashmask = ((xx << 10) | 0x3ff);
}

enum TLBLookupResult
{
	TLB_FOUND,
	TLB_NOTFOUND,
	TLB_UPDATE_C
};

static __forceinline TLBLookupResult LookupTLBPageAddress(const XCheckTLBFlag flag, const u32 vpa,
	u32* paddr)
{
	u32 tag = vpa >> HW_PAGE_INDEX_SHIFT;
	PowerPC::tlb_entry* tlbe = &PowerPC::ppcState.tlb[IsOpcodeFlag(flag)][tag & HW_PAGE_INDEX_MASK];
	if (tlbe->tag[0] == tag)
	{
		// Check if C bit requires updating
		if (flag == FLAG_WRITE)
		{
			UPTE2 PTE2;
			PTE2.Hex = tlbe->pte[0];
			if (PTE2.C == 0)
			{
				PTE2.C = 1;
				tlbe->pte[0] = PTE2.Hex;
				return TLB_UPDATE_C;
			}
		}

		if (!IsNoExceptionFlag(flag))
			tlbe->recent = 0;

		*paddr = tlbe->paddr[0] | (vpa & 0xfff);

		return TLB_FOUND;
	}
	if (tlbe->tag[1] == tag)
	{
		// Check if C bit requires updating
		if (flag == FLAG_WRITE)
		{
			UPTE2 PTE2;
			PTE2.Hex = tlbe->pte[1];
			if (PTE2.C == 0)
			{
				PTE2.C = 1;
				tlbe->pte[1] = PTE2.Hex;
				return TLB_UPDATE_C;
			}
		}

		if (!IsNoExceptionFlag(flag))
			tlbe->recent = 1;

		*paddr = tlbe->paddr[1] | (vpa & 0xfff);

		return TLB_FOUND;
	}
	return TLB_NOTFOUND;
}

static __forceinline void UpdateTLBEntry(const XCheckTLBFlag flag, UPTE2 PTE2, const u32 address)
{
	if (IsNoExceptionFlag(flag))
		return;

	int tag = address >> HW_PAGE_INDEX_SHIFT;
	PowerPC::tlb_entry* tlbe = &PowerPC::ppcState.tlb[IsOpcodeFlag(flag)][tag & HW_PAGE_INDEX_MASK];
	int index = tlbe->recent == 0 && tlbe->tag[0] != TLB_TAG_INVALID;
	tlbe->recent = index;
	tlbe->paddr[index] = PTE2.RPN << HW_PAGE_INDEX_SHIFT;
	tlbe->pte[index] = PTE2.Hex;
	tlbe->tag[index] = tag;
}

void InvalidateTLBEntry(u32 address)
{
	PowerPC::tlb_entry* tlbe =
		&PowerPC::ppcState.tlb[0][(address >> HW_PAGE_INDEX_SHIFT) & HW_PAGE_INDEX_MASK];
	tlbe->tag[0] = TLB_TAG_INVALID;
	tlbe->tag[1] = TLB_TAG_INVALID;
	PowerPC::tlb_entry* tlbe_i =
		&PowerPC::ppcState.tlb[1][(address >> HW_PAGE_INDEX_SHIFT) & HW_PAGE_INDEX_MASK];
	tlbe_i->tag[0] = TLB_TAG_INVALID;
	tlbe_i->tag[1] = TLB_TAG_INVALID;
}

// Page Address Translation
static __forceinline TranslateAddressResult TranslatePageAddress(const u32 address,
	const XCheckTLBFlag flag)
{
	// TLB cache
	// This catches 99%+ of lookups in practice, so the actual page table entry code below doesn't
	// benefit
	// much from optimization.
	u32 translatedAddress = 0;
	TLBLookupResult res = LookupTLBPageAddress(flag, address, &translatedAddress);
	if (res == TLB_FOUND)
		return TranslateAddressResult{ TranslateAddressResult::PAGE_TABLE_TRANSLATED, translatedAddress };

	u32 sr = PowerPC::ppcState.sr[EA_SR(address)];

	if (sr & 0x80000000)
		return TranslateAddressResult{ TranslateAddressResult::DIRECT_STORE_SEGMENT, 0 };

	// TODO: Handle KS/KP segment register flags.

	// No-execute segment register flag.
	if ((flag == FLAG_OPCODE || flag == FLAG_OPCODE_NO_EXCEPTION) && (sr & 0x10000000))
		return TranslateAddressResult{ TranslateAddressResult::PAGE_FAULT, 0 };

	u32 offset = EA_Offset(address);         // 12 bit
	u32 page_index = EA_PageIndex(address);  // 16 bit
	u32 VSID = SR_VSID(sr);                  // 24 bit
	u32 api = EA_API(address);               //  6 bit (part of page_index)

	// hash function no 1 "xor" .360
	u32 hash = (VSID ^ page_index);
	u32 pte1 = bswap((VSID << 7) | api | PTE1_V);

	for (int hash_func = 0; hash_func < 2; hash_func++)
	{
		// hash function no 2 "not" .360
		if (hash_func == 1)
		{
			hash = ~hash;
			pte1 |= PTE1_H << 24;
		}

		u32 pteg_addr =
			((hash & PowerPC::ppcState.pagetable_hashmask) << 6) | PowerPC::ppcState.pagetable_base;

		for (int i = 0; i < 8; i++, pteg_addr += 8)
		{
			if (pte1 == *(u32*)&Memory::physical_base[pteg_addr])
			{
				UPTE2 PTE2;
				PTE2.Hex = bswap((*(u32*)&Memory::physical_base[pteg_addr + 4]));

				// set the access bits
				switch (flag)
				{
				case FLAG_NO_EXCEPTION:
				case FLAG_OPCODE_NO_EXCEPTION:
					break;
				case FLAG_READ:
					PTE2.R = 1;
					break;
				case FLAG_WRITE:
					PTE2.R = 1;
					PTE2.C = 1;
					break;
				case FLAG_OPCODE:
					PTE2.R = 1;
					break;
				}

				if (!IsNoExceptionFlag(flag))
					*(u32*)&Memory::physical_base[pteg_addr + 4] = bswap(PTE2.Hex);

				// We already updated the TLB entry if this was caused by a C bit.
				if (res != TLB_UPDATE_C)
					UpdateTLBEntry(flag, PTE2, address);

				return TranslateAddressResult{ TranslateAddressResult::PAGE_TABLE_TRANSLATED,
																			(PTE2.RPN << 12) | offset };
			}
		}
	}
	return TranslateAddressResult{ TranslateAddressResult::PAGE_FAULT, 0 };
}

static void UpdateBATs(BatTable& bat_table, u32 base_spr)
{
	// TODO: Separate BATs for MSR.PR==0 and MSR.PR==1
	// TODO: Handle PP/WIMG settings.
	// TODO: Check how hardware reacts to overlapping BATs (including
	// BATs which should cause a DSI).
	// TODO: Check how hardware reacts to invalid BATs (bad mask etc).
	for (int i = 0; i < 4; ++i)
	{
		u32 spr = base_spr + i * 2;
		UReg_BAT_Up batu = PowerPC::ppcState.spr[spr];
		UReg_BAT_Lo batl = PowerPC::ppcState.spr[spr + 1];
		if (batu.VS == 0 && batu.VP == 0)
			continue;

		if ((batu.BEPI & batu.BL) != 0)
		{
			// With a valid BAT, the simplest way to match is
			// (input & ~BL_mask) == BEPI. For now, assume it's
			// implemented this way for invalid BATs as well.
			WARN_LOG(POWERPC, "Bad BAT setup: BEPI overlaps BL");
			continue;
		}
		if ((batl.BRPN & batu.BL) != 0)
		{
			// With a valid BAT, the simplest way to translate is
			// (input & BL_mask) | BRPN_address. For now, assume it's
			// implemented this way for invalid BATs as well.
			WARN_LOG(POWERPC, "Bad BAT setup: BPRN overlaps BL");
		}
		if (CountSetBits((u32)(batu.BL + 1)) != 1)
		{
			// With a valid BAT, the simplest way of masking is
			// (input & ~BL_mask) for matching and (input & BL_mask) for
			// translation. For now, assume it's implemented this way for
			// invalid BATs as well.
			WARN_LOG(POWERPC, "Bad BAT setup: invalid mask in BL");
		}
		for (u32 j = 0; j <= batu.BL; ++j)
		{
			// Enumerate all bit-patterns which fit within the given mask.
			if ((j & batu.BL) == j)
			{
				// This bit is a little weird: if BRPN & j != 0, we end up with
				// a strange mapping. Need to check on hardware.
				u32 address = (batl.BRPN | j) << BAT_INDEX_SHIFT;

				// The bottom bit is whether the translation is valid; the second
				// bit from the bottom is whether we can use the fastmem arena.
				u32 valid_bit = 0x1;
				if (Memory::bFakeVMEM && ((address & 0xFE000000) == 0x7E000000))
					valid_bit = 0x3;
				else if (address < Memory::REALRAM_SIZE)
					valid_bit = 0x3;
				else if (Memory::m_pEXRAM && (address >> 28) == 0x1 &&
					(address & 0x0FFFFFFF) < Memory::EXRAM_SIZE)
					valid_bit = 0x3;
				else if ((address >> 28) == 0xE && (address < (0xE0000000 + Memory::L1_CACHE_SIZE)))
					valid_bit = 0x3;

				// (BEPI | j) == (BEPI & ~BL) | (j & BL).
				bat_table[batu.BEPI | j] = address | valid_bit;
			}
		}
	}
}

static void UpdateFakeMMUBat(BatTable& bat_table, u32 start_addr)
{
	for (u32 i = 0; i < (0x10000000 >> BAT_INDEX_SHIFT); ++i)
	{
		// Map from 0x4XXXXXXX or 0x7XXXXXXX to the range
		// [0x7E000000,0x80000000).
		u32 e_address = i + (start_addr >> BAT_INDEX_SHIFT);
		u32 p_address = 0x7E000003 | ((i << BAT_INDEX_SHIFT) & Memory::FAKEVMEM_MASK);
		bat_table[e_address] = p_address;
	}
}

void DBATUpdated()
{
	dbat_table = {};
	UpdateBATs(dbat_table, SPR_DBAT0U);
	bool extended_bats = SConfig::GetInstance().bWii && HID4.SBE;
	if (extended_bats)
		UpdateBATs(dbat_table, SPR_DBAT4U);
	if (Memory::bFakeVMEM)
	{
		// In Fake-MMU mode, insert some extra entries into the BAT tables.
		UpdateFakeMMUBat(dbat_table, 0x40000000);
		UpdateFakeMMUBat(dbat_table, 0x70000000);
	}
	Memory::UpdateLogicalMemory(dbat_table);

	// IsOptimizable*Address and dcbz depends on the BAT mapping, so we need a flush here.
	JitInterface::ClearSafe();
}

void IBATUpdated()
{
	ibat_table = {};
	UpdateBATs(ibat_table, SPR_IBAT0U);
	bool extended_bats = SConfig::GetInstance().bWii && HID4.SBE;
	if (extended_bats)
		UpdateBATs(ibat_table, SPR_IBAT4U);
	if (Memory::bFakeVMEM)
	{
		// In Fake-MMU mode, insert some extra entries into the BAT tables.
		UpdateFakeMMUBat(ibat_table, 0x40000000);
		UpdateFakeMMUBat(ibat_table, 0x70000000);
	}
	JitInterface::ClearSafe();
}

// Translate effective address using BAT or PAT.  Returns 0 if the address cannot be translated.
// Through the hardware looks up BAT and TLB in parallel, BAT is used first if available.
// So we first check if there is a matching BAT entry, else we look for the TLB in
// TranslatePageAddress().
template <const XCheckTLBFlag flag>
TranslateAddressResult TranslateAddress(const u32 address)
{
	u32 bat_result = (flag == FLAG_OPCODE ? ibat_table : dbat_table)[address >> BAT_INDEX_SHIFT];
	if (bat_result & 1)
	{
		u32 result_addr = (bat_result & ~3) | (address & 0x0001FFFF);
		return TranslateAddressResult{ TranslateAddressResult::BAT_TRANSLATED, result_addr };
	}
	return TranslatePageAddress(address, flag);
}

}  // namespace
