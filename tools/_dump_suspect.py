import json
d = json.load(open('generated/instruction_inventory.json'))
for b in d['buckets']:
    if b['category'] != 'CPU':
        print(b['mnemonic'], b['count'])
        for e in b['examples']:
            print(' ', e)
