# MDEC numeric pin — finding (2026-06-27)

`mdec_pin.c` runs OUR mdec.c numeric pipeline (dequant/IDCT/YUV, verbatim from the
WIP transcription) vs BEETLE's verbatim pipeline (mdec.cpp) on identical synthetic
input, diffing each stage.

## Result: numeric core is BYTE-EXACT vs Beetle
    [scale matrix]   diffs: 0
    [dequant Coeff]  diffs: 0
    [idct block]     diffs: 0
    [yuv->rgb]       diffs: 0
So the D1/D2/D4/D5 transcription (dequant <<4 + bias, two-pass IDCT (sum+0x4000)>>15
+ Mask9ClampS8 + transposed >>3 matrix + Beetle ZigZag, YUV /256 + green mask) is a
faithful match to the oracle IN ISOLATION.

## BUT the integrated change REGRESSES the live FMV
Live Tomba 2 with the WIP change: the Whoopee Camp logo shows RAINBOW colour fringing.
Baseline (old numeric core) renders it CLEAN PINK (matching Beetle). So the numeric-only
change makes it worse on-screen even though the math is byte-exact.

## Root cause: layout pairing, not the math
Adopting Beetle's ZigZag + transposed IDCT changes the block's internal spatial layout.
The old numeric core and the old block-assembly (`append_color_macroblock` + the
multi-block Cr/Cb/Y flow) were a MATCHED PAIR producing correct output. Swapping only the
numeric core to Beetle's convention without transcribing the assembly breaks the pairing
→ chroma misaligns vs luma → rainbow fringing.

## Proper completion (do NOT merge until this passes)
1. Transcribe Beetle's EncodeImage assembly (mdec.cpp:324-419) too — block→output pixel
   mapping — so the assembly matches the new (Beetle) block layout.
2. Build a FULL-PATH pin: drive the REAL mdec.c end-to-end via its DMA API (mdec_write /
   mdec_dma_write_word / mdec_dma_read_word) on a synthetic full 6-block colour macroblock,
   diff the packed output against a Beetle-faithful full pipeline (decode + EncodeImage).
   The isolated pin here misses the integration; the full-path pin would have caught it.
3. Only when the full-path packed output is byte-exact AND the live FMV renders clean →
   merge.
