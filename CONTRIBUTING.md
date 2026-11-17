# Contributing

Contributions are welcome: bug reports, fixes, hardware and SDK accuracy, and performance work.

## Understand what you submit

Be ready to explain every line you add or remove, and why, in your own words. AI assistants
are fine to use: review what they write as you would anyone else's code, and say in the pull
request that you used one. If you can't explain a change yet, open an issue or a draft pull
request and ask; that is a good way to start.

## What belongs in Starlet

Starlet models the console and its SDK: GX, the OS, disc, memory card, audio, input and the
host services ports share. Behaviour one game needs belongs in that game's port. Ask in an
issue when you're unsure where a change fits.

## Pull requests

- Keep each pull request to one change a reviewer can read line by line. Keep related changes
  together and unrelated ones apart.
- Fix a reported problem or a clear gap. For larger features, open an issue first to agree on
  the approach before building it.
- Follow the surrounding code.
- Add or update a test for each behaviour you change. Renderer tests run against the
  recording GL fake in `tests/gx/gles`.
- Say in your own words what the change does and how you tested it. For performance changes,
  include before/after numbers and the device.

## Testing

- Run the tests: `cmake --build build && ctest --test-dir build`.
- Test renderer and performance changes in a port on a handheld as well, and name the GPU
  driver (for example Mali r20p0 or Panfrost). Several ports share this code, so say which
  ones you checked.

## Comments

Comments are rare, factual and justified. Write one only for a non-obvious hardware or format
fact, an external constraint, an invariant or an edge case, at the line it explains. Put API
contracts (thread and context ownership, lifetime, failure) in one or two lines on the public
declaration. Hardware references with their sources and designs that span files go in `docs/`;
decisions that would be costly to reverse go in `docs/adr/`. Leave out narration, restated
names and history: git has the history.

Bad, the comment restates the name:

```cpp
// Resident copies whose memory bytes the caller supplied (copy_texture_known).
std::uint64_t known_copies = 0;
```

Good, the comment states a fact the code cannot show:

```cpp
// A channel with no vertex color reads white, following Dolphin; hardware unverified.
```

## Game data

Keep game files out of the repository: disc images, executables, extracted assets, saves,
and code copied from a game or the console SDK.

## License

Contributions are licensed under the GNU General Public License v3.0, like the rest of the
project.

## Documentation

Docs live in `docs/`. Each push to master mirrors them to the GitHub wiki, so edit them here, not in the wiki.
