# starlet::nand

The Wii SDK's NAND calls on a save folder: the game's calls bind by name to the C-linkage functions
declared in `include/starlet/sdk/nand.h`. Moved from the Super Mario Galaxy port's platform layer.

## Storage

`start(directory, hooks)` takes an existing folder the port owns and creates `data/` and `tmp/` in
it. The game's paths resolve inside it and never reach the firmware's storage:

- relative paths and `/home/...` (the title's home, `NANDGetHomeDir` returns `/home`) to `data/`;
- `/tmp/...` to `tmp/`;
- `/shared2/...` to the folder itself (a missing Mii database reads as absent).

Other absolute paths are refused with `NAND_RESULT_ACCESS`. Paths are bounded to `NAND_MAX_PATH`
and eight components; empty, `.` and `..` components and symbolic links anywhere along the path are
refused. Files are private to the process owner; nonzero NAND attributes are refused.

## Calls

- Create keeps an existing file. Reads return the bytes read, 0 at the end. Close syncs a
  writer. Move renames a file into a directory and syncs both directories: a game that writes to
  `/tmp` and moves the file home replaces its save atomically.
- Sixteen handles, each tied to its exact `NANDFileInfo`; a caller's integer cannot close a
  descriptor it does not own. Files are bounded to 16 MiB.
- `NANDCheck` compares the request with the host file system's free space and inodes, not the
  console's quotas.
- Read-safe opens work; a write-safe open returns `NAND_RESULT_ACCESS` (no copy-and-commit
  transaction), never an ordinary write in its place.
- `NANDInitBanner` fills a banner as the SDK does.

## Asynchronous calls

One worker runs them in submission order from 32 fixed request slots. A command block that is
still pending cannot be submitted again (`NAND_RESULT_BUSY`); a callback may reuse its own block.
The callback runs on the worker after the request's slot is free, through `Hooks::complete` when
the port gives one: a port whose callbacks are interrupts disables its interrupts around it.
`stop()` refuses new work, runs the queued requests and their callbacks, joins the worker and
closes the remaining handles. Callers keep buffers and blocks alive until their completion.

`Hooks::enter` and `leave` bracket every section that holds the service's lock, for ports whose
threads can be cancelled.

## Tests

`tests/nand`: binary persistence across service lifetimes, missing files, access modes, concurrent
readers, handle limits, the temporary-banner move, path containment and symbolic links, a failed
start that leaves nothing open; asynchronous callback chains, seek and end of file, the worker
thread, queue bounds, shutdown draining and refused private writes.
