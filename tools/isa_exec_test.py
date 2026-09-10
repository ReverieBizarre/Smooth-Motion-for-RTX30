# ---------------------------------------------------------------------------
#  Does a patched sm_89 cubin actually EXECUTE correctly on this sm_86 GPU?
#
#  Loading is not the same as running.  cuModuleLoadData returning 0 and
#  cuModuleGetFunction resolving a symbol only prove that the loader accepted
#  the image and parsed its symbol table.  This test goes the rest of the way:
#  it launches the kernel and checks the numerical result.
#
#  The kernel is chosen so the expected answer is layout independent, because
#  modelling the mma fragment mapping by hand is where this kind of test
#  usually goes wrong:
#
#      mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32
#      A = all 1.0h,  B = all 1.0h,  C = 0
#      => every element of D equals sum over k of 1.0 * 1.0 = 16.0
#
#  A = B = 1.0 in f16 is the bit pattern 0x3C00, so each f16x2 register is
#  0x3C003C00 and nothing needs to be loaded.  If the tensor core path runs at
#  all, D is exactly 16.0 in every lane; anything else means the SASS is not
#  doing what it says.
#
#  Three configurations:
#     control   : compiled for sm_86, unpatched        -> must be 16.0
#     rejected  : compiled for sm_89, unpatched        -> expect CUDA 209
#     patched   : compiled for sm_89, e_flags -> sm_86 -> the actual question
# ---------------------------------------------------------------------------
import ctypes as C
import os
import struct
import subprocess
import sys

CUDA_BIN = r'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8\bin'
PTXAS = os.path.join(CUDA_BIN, 'ptxas.exe')
SCRATCH = r'C:\Users\lsp\WorkBuddy\2026-09-10-18-04-16\sm86_smooth\build\isa_scratch'
os.makedirs(SCRATCH, exist_ok=True)

PTX = r'''
.version 8.0
.target {TARGET}
.address_size 64

.visible .entry k_mma(.param .u64 pOut)
{{
    .reg .b32 %a<4>, %b<2>;
    .reg .f32 %c<4>, %d<4>;
    .reg .b64 %rd<8>;
    .reg .u32 %r<4>;

    ld.param.u64 %rd1, [pOut];

    // 1.0h in both halves of every f16x2 fragment register
    mov.b32 %a0, 0x3C003C00;
    mov.b32 %a1, 0x3C003C00;
    mov.b32 %a2, 0x3C003C00;
    mov.b32 %a3, 0x3C003C00;
    mov.b32 %b0, 0x3C003C00;
    mov.b32 %b1, 0x3C003C00;
    mov.f32 %c0, 0f00000000;
    mov.f32 %c1, 0f00000000;
    mov.f32 %c2, 0f00000000;
    mov.f32 %c3, 0f00000000;

    mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32
        {{%d0,%d1,%d2,%d3}}, {{%a0,%a1,%a2,%a3}}, {{%b0,%b1}},
        {{%c0,%c1,%c2,%c3}};

    mov.u32   %r1, %tid.x;
    mul.wide.u32 %rd2, %r1, 16;
    add.s64   %rd3, %rd1, %rd2;
    st.global.f32 [%rd3+0],  %d0;
    st.global.f32 [%rd3+4],  %d1;
    st.global.f32 [%rd3+8],  %d2;
    st.global.f32 [%rd3+12], %d3;
    ret;
}}
'''

E_FLAGS_SM86 = 0x560556
E_FLAGS_SM89 = 0x590559


def build(target):
    ptx_path = os.path.join(SCRATCH, 'k_%s.ptx' % target)
    cub_path = os.path.join(SCRATCH, 'k_%s.cubin' % target)
    with open(ptx_path, 'w') as f:
        f.write(PTX.format(TARGET=target))
    r = subprocess.run([PTXAS, '-arch=' + target, ptx_path, '-o', cub_path],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print('  ptxas failed for %s: %s' % (target, (r.stderr or r.stdout).strip()))
        return None, None
    data = open(cub_path, 'rb').read()
    fl = struct.unpack_from('<I', data, 0x30)[0]
    print('  built %-30s e_flags=%#010x  %d bytes' % (os.path.basename(cub_path), fl, len(data)))
    return cub_path, data


print('=' * 78)
print('STEP 1  build the same source for sm_86 and sm_89')
print('=' * 78)
p86, d86 = build('sm_86')
p89, d89 = build('sm_89')
if not d86 or not d89:
    sys.exit('build failed')

# ---- CUDA driver bindings -------------------------------------------------
cuda = C.WinDLL('nvcuda.dll')

P = C.c_void_p
cuda.cuInit.argtypes = [C.c_uint]
cuda.cuInit.restype = C.c_int
cuda.cuDeviceGet.argtypes = [C.POINTER(C.c_int), C.c_int]
cuda.cuDeviceGet.restype = C.c_int
cuda.cuCtxCreate_v2.argtypes = [C.POINTER(P), C.c_uint, C.c_int]
cuda.cuCtxCreate_v2.restype = C.c_int
cuda.cuModuleLoadData.argtypes = [C.POINTER(P), C.c_void_p]
cuda.cuModuleLoadData.restype = C.c_int
cuda.cuModuleGetFunction.argtypes = [C.POINTER(P), P, C.c_char_p]
cuda.cuModuleGetFunction.restype = C.c_int
cuda.cuMemAlloc_v2.argtypes = [C.POINTER(C.c_ulonglong), C.c_size_t]
cuda.cuMemAlloc_v2.restype = C.c_int
cuda.cuMemcpyDtoH_v2.argtypes = [C.c_void_p, C.c_ulonglong, C.c_size_t]
cuda.cuMemcpyDtoH_v2.restype = C.c_int
cuda.cuLaunchKernel.argtypes = [P, C.c_uint, C.c_uint, C.c_uint,
                                C.c_uint, C.c_uint, C.c_uint,
                                C.c_uint, P, C.POINTER(C.c_void_p), C.POINTER(C.c_void_p)]
cuda.cuLaunchKernel.restype = C.c_int
cuda.cuCtxSynchronize.restype = C.c_int
cuda.cuGetErrorName.argtypes = [C.c_int, C.POINTER(C.c_char_p)]
cuda.cuGetErrorName.restype = C.c_int


def err_name(code):
    name = C.c_char_p()
    cuda.cuGetErrorName(code, C.byref(name))
    return name.value.decode() if name.value else '?'


assert cuda.cuInit(0) == 0, 'cuInit failed'
dev = C.c_int()
assert cuda.cuDeviceGet(C.byref(dev), 0) == 0
ctx = P()
assert cuda.cuCtxCreate_v2(C.byref(ctx), 0, dev) == 0, 'cuCtxCreate failed'
print('  CUDA context created on device 0')

NTHREADS = 32
NOUT = NTHREADS * 4


def run(label, data, patch_to=None):
    blob = bytearray(data)
    if patch_to is not None:
        struct.pack_into('<I', blob, 0x30, patch_to)
    mod = P()
    code = cuda.cuModuleLoadData(C.byref(mod), bytes(blob))
    print('\n--- %s' % label)
    print('    cuModuleLoadData -> %d (%s)' % (code, err_name(code)))
    if code != 0:
        return None
    fn = P()
    code = cuda.cuModuleGetFunction(C.byref(fn), mod, b'k_mma')
    print('    cuModuleGetFunction -> %d (%s)' % (code, err_name(code)))
    if code != 0:
        return None

    out = C.c_ulonglong()
    assert cuda.cuMemAlloc_v2(C.byref(out), NOUT * 4) == 0
    # kernelParams is an array of POINTERS TO argument values: the driver reads
    # 8 bytes from each element.  Passing the device address itself makes the
    # driver dereference it as a host pointer.
    arg0 = C.c_ulonglong(out.value)
    params = (C.c_void_p * 1)(C.cast(C.byref(arg0), C.c_void_p))
    code = cuda.cuLaunchKernel(fn, 1, 1, 1, NTHREADS, 1, 1, 0, None,
                               params, None)
    print('    cuLaunchKernel     -> %d (%s)' % (code, err_name(code)))
    if code != 0:
        return None
    code = cuda.cuCtxSynchronize()
    print('    cuCtxSynchronize   -> %d (%s)' % (code, err_name(code)))
    if code != 0:
        return None

    host = (C.c_float * NOUT)()
    assert cuda.cuMemcpyDtoH_v2(C.cast(host, C.c_void_p), out.value, NOUT * 4) == 0
    vals = list(host)
    good = sum(1 for v in vals if abs(v - 16.0) < 1e-6)
    print('    result: %d/%d elements == 16.0 exactly' % (good, NOUT))
    print('    sample: %s' % [round(v, 6) for v in vals[:6]])
    if good != NOUT:
        uniq = sorted(set(round(v, 6) for v in vals))[:6]
        print('    NOT as expected. distinct values: %s' % uniq)
    return good == NOUT


print()
print('=' * 78)
print('STEP 2  load + launch')
print('=' * 78)

ok_ctrl = run('control: native sm_86, unpatched', d86)
ok_rej = run('compiled for sm_89, unpatched (expect rejection)',
             d89, patch_to=E_FLAGS_SM89)
ok_patch = run('compiled for sm_89, e_flags patched to sm_86', d89,
               patch_to=E_FLAGS_SM86)

print()
print('=' * 78)
print('VERDICT')
print('=' * 78)
print('  native sm_86 control            : %s' % ('PASS (16.0 everywhere)' if ok_ctrl else 'not established'))
print('  sm_89 image, unpatched          : %s' % ('rejected by the driver' if ok_rej is None else 'ACCEPTED (unexpected)'))
print('  sm_89 image, e_flags patched    : %s' % ('PASS (16.0 everywhere)' if ok_patch
                                                 else ('loaded but produced wrong values' if ok_patch is False
                                                       else 'failed to load or launch')))
