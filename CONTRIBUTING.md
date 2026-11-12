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
- Follow the surrounding code. Add a comment only where the code can't explain itself, such
  as a hardware quirk or an external constraint.
- Add or update a test for each behaviour you change. Renderer tests run against the
  recording GL fake in `tests/gx/gles`.
- Write commit titles as `type(scope): summary`, for example `fix(gx): ...`, in at most 72
  characters.
- Describe the change briefly in your own words, with what you tested: commands, device,
  firmware and GPU driver, and before/after numbers for performance changes.

## Testing

- Run the tests: `cmake --build build && ctest --test-dir build`.
- Test renderer and performance changes in a port on a handheld as well, and name the GPU
  driver (for example Mali r20p0 or Panfrost). Several ports share this code, so say which
  ones you checked.

## Game data

Keep game files out of the repository: disc images, executables, extracted assets, saves,
and code copied from a game or the console SDK.

## License

Contributions are licensed under the GNU General Public License v3.0, like the rest of the
project.
