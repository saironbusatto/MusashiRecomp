import json
with open('F:/Projects/TombaRecomp/psx_freeze_dump_psx-runtime_1778800465.json') as f:
    d = json.load(f)
for k in d:
    v = d[k]
    if isinstance(v, list):
        print(f'{k}: list len {len(v)}')
        if v and isinstance(v[0], dict):
            print(f'   first entry keys: {list(v[0].keys())}')
    elif isinstance(v, dict):
        print(f'{k}: dict with keys: {list(v.keys())[:20]}')
    else:
        print(f'{k}: {type(v).__name__} = {v}')
