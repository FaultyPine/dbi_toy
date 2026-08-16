#ifndef PEONY_X64_EMITTER_H
#define PEONY_X64_EMITTER_H

#include "shared_defines.h"
#include <string.h>
#include "Zydis.h"

// x64 emitter helper
// NOTE: this is unused currently, in favor of using DynAsm

// https://www.felixcloutier.com/x86/
// https://wiki.osdev.org/X86-64_Instruction_Encoding

static bool X64EmitByte(uint8_t** out, uint8_t byte)
{
    *(*out)++ = byte;
    return true;
}

static bool X64EmitBytes(uint8_t** out, const void* bytes, size_t length)
{
    memcpy(*out, bytes, length);
    *out += length;
    return true;
}


static bool X64EmitU32(uint8_t** out, uint32_t value)
{
    memcpy(*out, &value, sizeof(value));
    *out += sizeof(value);
    return true;
}

static bool X64EmitU64(uint8_t** out, uint64_t value)
{
    memcpy(*out, &value, sizeof(value));
    *out += sizeof(value);
    return true;
}

static int X64Gpr64Index(ZydisRegister reg)
{
    switch (reg)
    {
        case ZYDIS_REGISTER_RAX: return 0;
        case ZYDIS_REGISTER_RCX: return 1;
        case ZYDIS_REGISTER_RDX: return 2;
        case ZYDIS_REGISTER_RBX: return 3;
        case ZYDIS_REGISTER_RSP: return 4;
        case ZYDIS_REGISTER_RBP: return 5;
        case ZYDIS_REGISTER_RSI: return 6;
        case ZYDIS_REGISTER_RDI: return 7;
        case ZYDIS_REGISTER_R8:  return 8;
        case ZYDIS_REGISTER_R9:  return 9;
        case ZYDIS_REGISTER_R10: return 10;
        case ZYDIS_REGISTER_R11: return 11;
        case ZYDIS_REGISTER_R12: return 12;
        case ZYDIS_REGISTER_R13: return 13;
        case ZYDIS_REGISTER_R14: return 14;
        case ZYDIS_REGISTER_R15: return 15;
        default:                 return -1;
    }
}


#endif