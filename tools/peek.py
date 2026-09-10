#!/usr/bin/env python
# Disassemble a function at RVA (add image base) and show [rdx+imm] reads.
import struct, sys
from capstone import *
from capstone.x86 import X86_REG_RDX, X86_REG_RCX
DLL=r'C:\Users\lsp\WorkBuddy\2026-09-10-18-04-16\sm86_smooth\build\nvofapi64.dll'
data=open(DLL,'rb').read()
e_lfanew=struct.unpack_from('<I',data,0x3c)[0]; coff=e_lfanew+4
opt_off=coff+20; sizeof_opt=struct.unpack_from('<H',data,coff+16)[0]
sec_off=opt_off+sizeof_opt
secs=[]
for i in range(6):
    sh=sec_off+i*40
    name=data[sh:sh+8].split(b'\x00')[0].decode('latin1','replace')
    vsize,vaddr,rsize,raw=struct.unpack_from('<IIII',data,sh+8)
    secs.append((name,vaddr,rsize,raw))
ib=0x180000000
def off(rva):
    for n,v,r,ra in secs:
        if v<=rva<v+r: return ra+(rva-v)
def dis(rva,nb=400):
    o=off(rva)
    md=Cs(CS_ARCH_X86,CS_MODE_64); md.detail=True
    return list(md.disasm(data[o:o+nb], ib+rva))
rva=int(sys.argv[1],0)
nb=int(sys.argv[2],0) if len(sys.argv)>2 else 300
print("=== RVA %#x (VA %#x) ==="%(rva, ib+rva))
for ins in dis(rva,nb):
    tag=''
    if ins.mnemonic=='mov' and len(ins.operands)==2 and ins.operands[1].type==CS_OP_MEM:
        src=ins.operands[1]
        if src.mem.base==X86_REG_RDX and src.mem.disp:
            tag='  <<< [rdx+%#x]'%(src.mem.disp)
    print('%#010x: %-10s %s%s'%(ins.address,ins.mnemonic,ins.op_str,tag))
