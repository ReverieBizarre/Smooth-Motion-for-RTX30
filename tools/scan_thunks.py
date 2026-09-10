#!/usr/bin/env python
# Scan whole .text: group consecutive [rdx+imm] reads into copy thunks.
import struct
from capstone import *
from capstone.x86 import X86_REG_RDX
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
tn,tv,tr,traw=[s for s in secs if s[0]=='.text'][0]
code=data[traw:traw+tr]
md=Cs(CS_ARCH_X86,CS_MODE_64); md.detail=True
insns=list(md.disasm(code, ib+tv))

# function entry = instruction preceded by ret/int3/hlt/jmp(uncond) or first
def is_entry(prev):
    return prev is None or prev.mnemonic in ('ret','int3','hlt','jmp')
# collect reads grouped by function
func_reads={}  # start_va -> set(offsets)
cur=None
for i,ins in enumerate(insns):
    prev=insns[i-1] if i>0 else None
    if is_entry(prev):
        cur=ins.address
        func_reads.setdefault(cur,set())
    if ins.mnemonic=='mov' and len(ins.operands)==2 and ins.operands[1].type==CS_OP_MEM:
        src=ins.operands[1]
        if src.mem.base==X86_REG_RDX and src.mem.disp>0 and src.mem.disp<0x400:
            func_reads.setdefault(cur,set()).add(src.mem.disp)

print("Copy-thunk candidates (function entry -> [rdx+imm] read offsets):")
for start in sorted(func_reads):
    r=sorted(func_reads[start])
    if len(r)>=4:
        print("  fn %#x  (%d reads)  %s"%(start,len(r),r))
