"""Are card1.mcd frames 1..15 byte-identical? If yes, the 'frame 1' label
in _card_bytes2.py output may actually be frames 1, 2, 3, 4 etc.
Print full hex for each first-byte-differing pair."""
with open('F:/Projects/TombaRecomp/saves/card1.mcd', 'rb') as fh:
    card = fh.read()

f1 = card[128:256]
for fr in range(0, 16):
    fb = card[fr*128:(fr+1)*128]
    # diff vs f1
    diffs = [(i, f1[i], fb[i]) for i in range(128) if f1[i] != fb[i]]
    if diffs:
        diff_str = ', '.join(f'@{i}(f1={a:02X},f{fr}={b:02X})' for i, a, b in diffs[:6])
    else:
        diff_str = '(IDENTICAL to frame 1)'
    print(f"Frame {fr:2d}: {diff_str}")
