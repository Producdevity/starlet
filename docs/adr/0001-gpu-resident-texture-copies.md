# 1. Keep EFB texture copies GPU-resident and materialize them lazily

Date: 2026-11-18

Status: Accepted

## Context
A console texture copy writes encoded texels to main memory. Reading the EFB back after every copy stalls the GL
thread on Mali (a fence wait and a slow mapped read), and most copies are sampled again only on the GPU.

## Decision
With the direct path, texture copies render into GPU textures and memory is left stale. Lookups sample the copy
(or a GPU crop of it); any other overlap, host DMA read or release materializes it with a readback and the CPU
encoder. Validity is tracked by write generations, stamps or expected bytes, and copies a game's CPU reads
are declared with `write_back`.

## Consequences
- Memory at a copy's destination is wrong until materialized; every reader of that memory must go through
  `before_dma_read`, lookups or `write_back`.
- Stamps, covered ranges and shadowed copies add state that must stay consistent with memory.
- Reversing it means a readback and CPU encode per copy; each readback costs the Mali GL thread about 1 ms whatever its size (GLES-renderer.md, Write-backs).
