import io

p = 'src/proxy/proxy.cpp'
s = io.open(p, encoding='utf-8').read()

def sub(old, new, n=1):
    global s
    c = s.count(old)
    assert c == n, (c, old[:70])
    s = s.replace(old, new)

# 1. the forward declaration must live in the anonymous namespace (anonymous
#    namespace blocks in one TU are the same namespace, so the definition in
#    the second block below still matches)
sub('''#include "vfi.h"

// defined at file scope below; declared here so the anonymous-namespace hooks
// can call it
static void registerSwapchain(IDXGISwapChain1* sc, const DXGI_SWAP_CHAIN_DESC1& d);

namespace {''',
    '''#include "vfi.h"

namespace {''')

sub('''    D3D12_RESOURCE_STATES bbState[16];
};
''',
    '''    D3D12_RESOURCE_STATES bbState[16];
};

static void registerSwapchain(IDXGISwapChain1* sc, const DXGI_SWAP_CHAIN_DESC1& d);
''')

# 2. reopen the anonymous namespace around the tail so its definitions are the
#    same entities as the declarations above
sub('''// ---------------------------------------------------------------------------
// swapchain registration + lazy engine bring-up
// ---------------------------------------------------------------------------
static void registerSwapchain(IDXGISwapChain1* sc, const DXGI_SWAP_CHAIN_DESC1& d)''',
    '''// ---------------------------------------------------------------------------
// swapchain registration + lazy engine bring-up
// ---------------------------------------------------------------------------
namespace {

static void registerSwapchain(IDXGISwapChain1* sc, const DXGI_SWAP_CHAIN_DESC1& d)''')

sub('''static DWORD WINAPI startupThread(LPVOID)
{
    installHooks();
    return 0;
}

BOOL WINAPI DllMain''',
    '''static DWORD WINAPI startupThread(LPVOID)
{
    installHooks();
    return 0;
}

} // namespace

BOOL WINAPI DllMain''')

io.open(p, 'w', encoding='utf-8').write(s)
print('patched ok')
