import json,os,glob
out_dir = os.path.join(os.environ.get('TEMP','/tmp'),'optsess')
p = sorted(glob.glob(os.path.join(out_dir,'options_baseline_*.json')))[-1]
print('file:', p)
s = json.load(open(p))

print()
print('--- runtime: first 6 writes from PC=0x000000BC ---')
ents = s['runtime']['wtrace_all_dump_optstate']['entries']
for e in [x for x in ents if x['pc']=='0x000000BC'][:6]:
    print(e)
print()
print('--- runtime: writes from PC=0xBFC11BF0 (last 3) ---')
for e in [x for x in ents if x['pc']=='0xBFC11BF0'][-3:]:
    print(e)
print()
print('--- beetle: first 4 writes (cell DB8 stream) ---')
ents_b = s['beetle']['wtrace_all_dump_optstate']['entries']
for e in ents_b[:4]:
    print(e)
print()
print('--- beetle: last 2 writes ---')
for e in ents_b[-2:]:
    print(e)
print()
print('--- runtime: write SIZES distribution ---')
from collections import Counter
print(Counter(e['w'] for e in ents))
print('--- beetle: write SIZES distribution ---')
print(Counter(e['w'] for e in ents_b))
