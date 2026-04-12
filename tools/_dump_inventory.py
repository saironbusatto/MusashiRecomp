import json
d = json.load(open('generated/instruction_inventory.json'))
print('verification:')
for v in d['walker']['verification']:
    print(' ', v)
print()
print('totals:', d['totals'])
print()
print(f'{"category":<22} {"mnemonic":<26} {"prim":<6} {"sub":<12} {"subv":<6} {"count":>8}')
print('-' * 86)
for b in d['buckets']:
    k = b['key']
    print(f'{b["category"]:<22} {b["mnemonic"]:<26} {k["primary_opcode_hex"]:<6} {k["sub_kind"]:<12} {k["sub_value_hex"]:<6} {b["count"]:>8}')
