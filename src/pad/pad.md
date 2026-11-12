# starlet::pad and starlet::ar

`starlet::pad`: PADRead returns the controller states the port publishes from its input sampling
(`starlet::host::to_gamecube`), already within the console's calibrated ranges.

`starlet::ar`: 16 MiB of ARAM and its DMA (AR and ARQ). Transfers copy when posted and complete as
interrupts, one timing the hardware's queued DMA can produce. Main-memory addresses resolve through
`starlet::os::hardware_to_native`.

Both moved from the Pikmin 2 port.
