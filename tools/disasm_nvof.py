#!/usr/bin/env python
# PE-aware capstone disassembler to extract NVOFA param-copy thunks from nvofapi64.dll
import sys, struct
from capstone import *

DLL = r"C:\Users\lsp\WorkBuddy\2026-09-10-18-04-16\sm86_smooth\build\nvofapi64.dll"

def load():
    data = open(DLL,'rb').read()
    return data

def parse_pe(data):
    assert data[:2]==b'MZ'
    e_lfanew = struct.unpack_from('<I', data, 0x3c)[0]
    assert data[e_lfanew:e_lfanew+4]==b'PE\x00\x00'
    # COFF header at e_lfanew+4
    coff = e_lfanew+4
    machine, nsec = struct.unpack_from('<HH', data, coff)
    opt_off = coff+20
    # optional header: magic at opt_off (2 bytes). PE32+ = 0x20b
    magic = struct.unpack_from('<H', data, opt_off)[0]
    is64 = (magic == 0x20b)
    if is64:
        image_base = struct.unpack_from('<Q', data, opt_off+24)[0]
    else:
        image_base = struct.unpack_from('<I', data, opt_off+28)[0]
    # section headers start after optional header. SizeOfOptionalHeader is in COFF header at coff+16.
    sizeof_opt = struct.unpack_from('<H', data, coff+16)[0]
    sec_off = opt_off + sizeof_opt
    sections=[]
    for i in range(nsec):
        sh = sec_off + i*40
        name = data[sh:sh+8].split(b'\x00')[0].decode('latin1','replace')
        vsize, vaddr, rsize, rawptr = struct.unpack_from('<IIII', data, sh+8)
        sections.append({'name':name,'vsize':vsize,'vaddr':vaddr,'rsize':rsize,'raw':rawptr})
    return image_base, sections

def rva_to_off(sections, rva):
    for s in sections:
        if s['vaddr']<=rva < s['vaddr']+s['rsize']:
            return s['raw'] + (rva - s['vaddr'])
    return None

def disasm_range(data, image_base, sections, rva, max_bytes=600):
    off = rva_to_off(sections, rva)
    if off is None: return []
    raw = data[off:off+max_bytes]
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    out=[]
    for ins in md.disasm(raw, image_base+rva):
        out.append(ins)
        if ins.mnemonic=='ret' or ins.mnemonic=='hlt':
            break
    return out

def disasm_full(data, image_base, sections, rva):
    # full .text-ish disasm until ret, bounded
    return disasm_range(data, image_base, sections, rva, max_bytes=2000)

def find_calls(insns):
    calls=[]
    for ins in insns:
        if ins.mnemonic=='call':
            # operand is relative
            op=ins.op_str
            # compute target VA
            # capstone rel: ins.address+ins.size+imm
            try:
                imm=int(op,16) if op.startswith('0x') else None
            except:
                imm=None
            if imm is None:
                # try parse
                try: imm=int(op,0)
                except: imm=None
            if imm is not None:
                target = imm  # capstone already gives absolute VA for relative calls? 
                # Actually op_str for call rel32 is the absolute address in capstone x86
                calls.append(target)
    return calls

def read_offsets_from_rdx(insns):
    # collect mov ..., [rdx+imm] offsets
    offs=[]
    for ins in insns:
        if ins.mnemonic=='mov' and len(ins.operands)==2:
            # dst op0, src op1
            dst=ins.operands[0]; src=ins.operands[1]
            if src.type==CS_OP_MEM and src.mem.base==X86_REG_RDX and src.mem.disp!=0:
                offs.append(src.mem.disp)
    return sorted(set(offs))

def is_copy_thunk(insns):
    offs=read_offsets_from_rdx(insns)
    return len(offs)>=3, offs

def main():
    data=load()
    ib, sections = parse_pe(data)
    print("ImageBase %#x  sections: %s" % (ib, [s['name'] for s in sections]))
    # locate exports
    # export table RVA in optional header data directory[0]
    opt_off = None
    # recompute opt_off
    e_lfanew = struct.unpack_from('<I', data, 0x3c)[0]
    coff = e_lfanew+4
    magic = struct.unpack_from('<H', data, coff+20)[0]
    is64 = magic==0x20b
    opt_off = coff+20
    if is64:
        edat_rva = struct.unpack_from('<I', data, opt_off+0x70+0*8)[0]  # export dir at index 0
    else:
        edat_rva = struct.unpack_from('<I', data, opt_off+0x60+0*8)[0]
    print("export dir rva %#x" % edat_rva)
    eoff = rva_to_off(sections, edat_rva)
    nfunc = struct.unpack_from('<I', data, eoff+0x14)[0]
    funcs_rva = struct.unpack_from('<I', data, eoff+0x1c)[0]
    ord_rva   = struct.unpack_from('<I', data, eoff+0x24)[0]
    names_rva = struct.unpack_from('<I', data, eoff+0x20)[0]
    nameord_rva = struct.unpack_from('<I', data, eoff+0x28)[0]
    # build name->rva
    name2rva={}
    for i in range(nfunc):
        nr = struct.unpack_from('<I', data, rva_to_off(sections,names_rva)+i*4)[0]
        no = struct.unpack_from('<H', data, rva_to_off(sections,ord_rva)+i*2)[0]
        fr = struct.unpack_from('<I', data, rva_to_off(sections,funcs_rva)+no*4)[0]
        nm = data[rva_to_off(sections,nr):].split(b'\x00')[0].decode('latin1')
        name2rva[nm]=fr
    print("exports with NvOF:")
    for k in sorted(name2rva):
        if 'NvOF' in k or 'nvOF' in k:
            print("   %-40s %#x" % (k, name2rva[k]))

    # Disassemble NvOFAPICreateInstanceD3D12 to find function-list slots
    ci = name2rva.get('NvOFAPICreateInstanceD3D12')
    if ci is None:
        print("no CreateInstance export"); return
    insns = disasm_full(data, ib, sections, ci)
    # find 'mov [rcx+off], rax' where rax loaded from lea [rip+...]
    # Specifically, the list is first arg (rcx). Slots written via mov [rcx+0x20] etc.
    print("\n--- CreateInstance writes to function list (rcx+off) ---")
    list_writes={}
    for ins in insns:
        if ins.mnemonic=='mov' and len(ins.operands)==2:
            dst=ins.operands[0]
            if dst.type==CS_OP_MEM and dst.mem.base==X86_REG_RCX and dst.mem.disp not in (0,):
                list_writes[dst.mem.disp]=True
    for off in sorted(list_writes):
        print("   list slot at rcx+%#x" % off)

    # Now for register (slot 4 -> +0x20) and execute (slot 6 -> +0x30): we need the
    # function RVAs. They come from lea rax,[rip+X] then mov [rcx+off],rax.
    # Reconstruct: walk instructions, track last 'lea rax,[rip+imm]' target, see following store.
    print("\n--- slot -> function RVA mapping ---")
    slot_fn={}
    last_lea=None
    for ins in insns:
        if ins.mnemonic=='lea' and len(ins.operands)==2:
            dst=ins.operands[0]; src=ins.operands[1]
            if dst.reg==X86_REG_RAX and src.type==CS_OP_MEM and src.mem.base==X86_REG_RIP:
                # target VA
                target = ins.address + ins.size + src.mem.disp
                last_lea=target
        elif ins.mnemonic=='mov' and len(ins.operands)==2:
            dst=ins.operands[0]
            if dst.type==CS_OP_MEM and dst.mem.base==X86_REG_RCX and last_lea is not None:
                slot_fn[dst.mem.disp]=last_lea
                last_lea=None
    for off in sorted(slot_fn):
        print("   slot rcx+%#x -> fn %#x" % (off, slot_fn[off]))
        # also print a name guess
    # We care about +0x20 (register) and +0x30 (execute)
    targets={}
    for off in (0x20,0x30):
        if off in slot_fn: targets[off]=slot_fn[off]

    # Disassemble register/execute, follow calls to find copy thunks
    all_calls=set()
    for off,fn in targets.items():
        print("\n=== function at %#x (slot %#x) ===" % (fn, off))
        fi = disasm_full(data, ib, sections, fn)
        calls = find_calls(fi)
        for c in calls:
            if c not in all_calls:
                all_calls.add(c)
                # try to see if it's a copy thunk (reads [rdx+imm])
                ti = disasm_full(data, ib, sections, c)
                ok, offs = is_copy_thunk(ti)
                tag = "COPY-THUNK(src rdx)" if ok else "?"
                print("   call -> %#x  %s  read-offsets=%s" % (c, tag, offs if ok else ""))
    print("\nDone.")

if __name__=='__main__':
    main()
