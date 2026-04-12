import json
d = json.load(open('generated/instruction_inventory.json'))
print('synthetic seed walks:')
for s in d['walker']['synthetic_seeds']:
    print(' ', s)
print()
print('unknown buckets:')
for b in d['buckets']:
    if b['category'] == 'UNKNOWN':
        print(' ', b['mnemonic'], 'count=', b['count'], 'key=', b['key'])
        for e in b['examples']:
            print('   ', e)
