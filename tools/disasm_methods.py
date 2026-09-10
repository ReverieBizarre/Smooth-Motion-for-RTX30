#!/usr/bin/env python
# Disassemble the Register (vtable[9]=0x2440) and Execute (vtable[11]=0x2e00)
# methods of nvofapi64.dll and dump the param-struct field reads / early
# pointer checks, so we can place pResource/bufferUsage/handles at the right
# offsets and understand the INVALID_PTR (4) return.
import struct
from capstone import *
from capstone.x86 import X86_REG_RDX, X86_REG_RCX, X86_REG_R8, X86_REG_RAX

DLL = r"C:\Users\lsp\WorkBuddy\2026-09-10-18-04-16\sm86_smooth\build\nvofapi64.dll"

def load(): return open(DLL,'rb').read()

def parse_pe(data):
    e_lfanew = struct.unpack_from('<I', data, 0x3c)[0]
    coff = e_lfanew+4
    opt_off = coff+20
    magic = struct.unpack_from('<H', data, opt_off)[0]
    is64 = (magic == 0x20b)
    ib = struct.unpack_from('<Q', data, opt_off+24)[0] if is64 else struct.unpack_from('<I', data, opt_off+28)[0]
    sizeof_opt = struct.unpack_from('<H', data, coff+16)[0]
    sec_off = opt_off + sizeof_opt
    nsec = struct.unpack_from('<H', data, coff+2)[0]
    sections=[]
    for i in range(nsec):
        sh = sec_off + i*40
        name = data[sh:sh+8].split(b'\x00')[0].decode('latin1','replace')
        vsize, vaddr, rsize, rawptr = struct.unpack_from('<IIII', data, sh+8)
        sections.append({'name':name,'vsize':vsize,'vaddr':vaddr,'rsize':rsize,'raw':rawptr})
    return ib, sections

def rva_to_off(sections, rva):
    for s in sections:
        if s['vaddr']<=rva < s['vaddr']+s['rsize']:
            return s['raw'] + (rva - s['vaddr'])
    return None

REG_RVA = 0x2440
EXE_RVA = 0x2e00

def disasm(data, ib, sections, rva, max_bytes=3000):
    off = rva_to_off(sections, rva)
    raw = data[off:off+max_bytes]
    md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail=True
    out=[]; depth=0
    for ins in md.disasm(raw, ib+rva):
        out.append(ins)
        if ins.mnemonic=='ret': break
        if ins.mnemonic in ('call','jmp'):  # stop at first call into driver internals
            depth+=1
            if depth>=1:  # only show up to first call (that's where copy/checks live)
                pass
    return out

def show(label, rva, sections, ib, data):
    print("\n========== %s  (RVA %#x) ==========" % (label, rva))
    insns = disasm(data, ib, sections, rva)
    for ins in insns:
        # show mem reads from rdx (param struct) and rcx (handle/this)
        note=[]
        if ins.mnemonic=='mov' and len(ins.operands)==2:
            op1=ins.operands[1]
            if op1.type==CS_OP_MEM:
                base = {X86_REG_RDX:'rdx',X86_REG_RCX:'rcx',X86_REG_R8:'r8',X86_REG_RAX:'rax'}.get(op1.mem.base,'?')
                if base in ('rdx','rcx','r8'):
                    note.append("READ [%s+%#x]" % (base, op1.mem.disp))
        # flag any comparison that may gate INVALID_PTR
        if ins.mnemonic in ('cmp','test'):
            note.append("CMP/TEST")
        if note:
            print("  %-28s %-10s %-22s %s" % (("%#x"%(ins.address)), ins.mnemonic, ins.op_str, " ".join(note)))
        else:
            print("  %-28s %-10s %s" % ("%#x"%(ins.address), ins.mnemonic, ins.op_str))

def main():
    data=load(); ib, sections = parse_pe(data)
    print("ImageBase %#x" % ib)
    show("RegisterResource method", REG_RVA, sections, ib, data)
    show("Execute method", EXE_RVA, sections, ib, data)

if __name__=='__main__':
    main()
