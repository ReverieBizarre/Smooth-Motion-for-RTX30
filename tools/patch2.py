import io

p = 'src/shaders/vfi.hlsl'
s = io.open(p, encoding='utf-8').read()

# the fill shader needs tF (t6) and its own tC (t7); move tF's declaration up
# so it exists before cs_flow_fill, and drop the later duplicate.
old_decl = '''#define FILL_THRESH 0.35f

Texture2D<float> tC : register(t7);   // confidence, same grid as tF
'''
new_decl = '''#define FILL_THRESH 0.35f

Texture2D<float2> tF : register(t6);   // flow (also used by the median pass)
Texture2D<float>  tC : register(t7);   // confidence, same grid as tF
'''
assert s.count(old_decl) == 1
s = s.replace(old_decl, new_decl)

old_dup = '''Texture2D<float2> tF : register(t6);

[numthreads(TG, TG, 1)]
void cs_median3f'''
new_dup = '''[numthreads(TG, TG, 1)]
void cs_median3f'''
assert s.count(old_dup) == 1
s = s.replace(old_dup, new_dup)

io.open(p, 'w', encoding='utf-8').write(s)
print('patched ok')
