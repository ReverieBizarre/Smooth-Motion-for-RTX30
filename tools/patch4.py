import io

p = 'src/proxy/proxy.cpp'
s = io.open(p, encoding='utf-8').read()

def sub(old, new, n=1):
    global s
    c = s.count(old)
    assert c == n, (c, old[:80])
    s = s.replace(old, new)

# ExecuteCommandLists returns void, not HRESULT
sub('''static HRESULT STDMETHODCALLTYPE hookExecLists(ID3D12CommandQueue* q, UINT n,
                                               ID3D12CommandList* const* lists)
{
    if (!g_gameQueue && q && q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
    {
        g_gameQueue = q;
        q->AddRef();
        LOGF("captured the game's DIRECT command queue");
    }
    return g_origExecLists(q, n, lists);
}''',
    '''static void STDMETHODCALLTYPE hookExecLists(ID3D12CommandQueue* q, UINT n,
                                            ID3D12CommandList* const* lists)
{
    if (!g_gameQueue && q && q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
    {
        g_gameQueue = q;
        q->AddRef();
        LOGF("captured the game's DIRECT command queue");
    }
    if (g_origExecLists) g_origExecLists(q, n, lists);
}''')

# g_initStarted is already declared next to g_swaps
sub('''static volatile LONG g_initStarted = 0;

static bool ensureReady(SwapState* s)''',
    '''static bool ensureReady(SwapState* s)''')

io.open(p, 'w', encoding='utf-8').write(s)
print('patched ok')
