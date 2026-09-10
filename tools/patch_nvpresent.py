# ============================================================================
#  patch_nvpresent.py - produce a fully re-host-ready copy of NvPresent64.dll.
#
#  Two families of patches, both verified:
#    1. sil=0 (force the FP16 kernel set, avoid the FP8 path that dies with
#       CUDA_ERROR_ILLEGAL_INSTRUCTION on Ampere).  The gate is
#         RVA 0xc41c:  cmp dword [rcx+0x14], 3
#         RVA 0xc437:  setge sil        (40 0F 9D C6)
#       forcing sil=0 selects the non-_fp8 kernels.  We patch
#         40 0F 9D C6  ->  40 32 F6 90   (xor sil,sil ; nop)
#    2. every embedded fatbinary: rewrite its per-entry arch fields (0x78/0x59)
#       to sm_86 and every inner ELF e_flags to 0x560556, so the images load on
#       an sm_86 device (verified: 37/37 status 0).
#
#  Writes to a COPY under build/ - the driver store is never touched.
# ============================================================================
import struct, re, os, sys, ctypes as C

SRC = r'C:\Windows\System32\DriverStore\FileRepository\nv_dispi.inf_amd64_a3944b54ff18b284\NvPresent64.dll'
DST = r'C:\Users\lsp\WorkBuddy\2026-09-10-18-04-16\sm86_smooth\build\NvPresent64.patched.dll'

buf = bytearray(open(SRC, 'rb').read())
print(f'loaded {len(buf)} bytes')

# ---- 1. gate patch: DOUBLE patch (verified against real disassembly) ----
# The `cmp dword [rcx+0x14],3` has TWO consumers:
#   0xc425: mov ebp, 4            ; ebp = error code
#   0xc42f: cmovge ebp, ebx       ; if tier>=3 -> ebp=0 (allowed)
#   0xc437: setge sil             ; if tier>=3 -> sil=1 (FP8 capable, returned)
# A sil-only patch would force FP8 off but leave ebp=4 (host bails).  So BOTH:
#   (a) allow tier 2:  cmp [rcx+0x14], 3  ->  cmp [rcx+0x14], 2   (0x03->0x02 @0xc41f)
#   (b) force FP8 off: setge sil           ->  xor sil,sil ; nop  (@0xc437)
CMP_IMM_OFF = 0xb81f   # file offset of the immediate (RVA 0xc41f -> 0xc41f-0xC00)
SETGE_OFF   = 0xb837   # file offset of `setge sil` (RVA 0xc437 -> 0xc437-0xC00)
assert buf[CMP_IMM_OFF] == 0x03, f'unexpected cmp imm: {buf[CMP_IMM_OFF]:#x}'
orig_setge = bytes(buf[SETGE_OFF:SETGE_OFF+4])
assert orig_setge == bytes.fromhex('40 0F 9D C6'), f'unexpected setge: {orig_setge.hex()}'
buf[CMP_IMM_OFF] = 0x02                 # 3 -> 2 : allow tier 2
buf[SETGE_OFF:SETGE_OFF+4] = bytes.fromhex('40 32 F6 90')   # setge sil -> xor sil,sil; nop
print(f'gate double-patched: cmp imm 3->2 @{CMP_IMM_OFF:#x}, {orig_setge.hex()} -> 40 32 F6 90')

# verify with capstone
try:
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    ins = list(md.disasm(bytes(buf[SETGE_OFF:SETGE_OFF+4]), 0))
    print('  disasm:', '; '.join(f'{i.mnemonic} {i.op_str}' for i in ins))
except ImportError:
    print('  (no capstone; byte-level patch applied)')

# ---- 2. fatbin patches ----
FATBIN_MAGIC = struct.pack('<I', 0xba55ed50)
fbs = [m.start() for m in re.finditer(re.escape(FATBIN_MAGIC), bytes(buf))]
print(f'{len(fbs)} fatbinaries')
for base in fbs:
    size = struct.unpack_from('<Q', buf, base+8)[0]
    b = buf[base:base+size]
    for off in range(0, size-3, 4):
        if b[off] in (0x78, 0x59) and b[off+1]==0 and b[off+2]==0 and b[off+3]==0:
            buf[base+off] = 0x56
    for m in re.finditer(rb'\x7fELF', bytes(b)):
        struct.pack_into('<I', buf, base+m.start()+0x30, 0x560556)

os.makedirs(os.path.dirname(DST), exist_ok=True)
open(DST, 'wb').write(buf)
print(f'wrote {DST} ({len(buf)} bytes)')

# ---- 3. verify every fatbin in the PATCHED copy loads on this sm_86 GPU ----
cu = C.WinDLL('nvcuda.dll'); VP = C.c_void_p
cu.cuInit(0); dev=C.c_int(); cu.cuDeviceGet(C.byref(dev),0); ctx=VP(); cu.cuCtxCreate_v2(C.byref(ctx),0,dev)
cu.cuModuleLoadData.argtypes=[C.POINTER(VP),C.c_void_p]
cu.cuGetErrorName.argtypes=[C.c_int,C.POINTER(C.c_char_p)]
def en(c):
    n=C.c_char_p(); cu.cuGetErrorName(c,C.byref(n)); return n.value.decode() if n.value else '?'
ok=0; fail=[]
for i,base in enumerate(fbs):
    size=struct.unpack_from('<Q', buf, base+8)[0]
    m=VP(); rc=cu.cuModuleLoadData(C.byref(m), bytes(buf[base:base+size]))
    if rc==0: ok+=1
    else: fail.append((i,base,rc,en(rc)))
print(f'verification: {ok}/{len(fbs)} fatbins load on sm_86')
for i,b,r,e in fail: print(f'  fb#{i} @{b:#x} -> {r} {e}')
print('\nDONE.  patched DLL ready; do NOT redistribute (contains NVIDIA IP).')
