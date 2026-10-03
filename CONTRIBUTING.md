# Contributing to Monkey Tool

Thanks for helping! Bug reports, new game formats and code are all welcome.

## Reporting a problem

Open an issue and include:

* **the game and platform** (for example *NFS No Limits, Android 7.x*);
* **the asset path** shown in the tool's info panel (for example `models/cars/lotus_elise/lotus_elise.sb3d`);
* **two screenshots**: the tool, and the same thing in the game;
* **`MonkeyTool.log`**, which sits next to `MonkeyTool.exe`.

If you can, attach the file involved. Only attach files you're allowed to share.

## Building and checking your change

```
cmake -B build && cmake --build build          # the command-line tool, any OS
bash tests/check_windows_syntax.sh             # the Windows sources against stub headers
```

The Windows GUI is built with `BUILD.bat` or with CMake on Windows. Every push is also built by GitHub Actions.

## Code

* C++17 with no external libraries. Formats are decoded in `src/`, and the only runtime DLLs are zstd and LZHAM (see the README).
* `src/nfsnl.h` is the core API and `main_win32.cpp` is the GUI. `main_cli.cpp` is the command-line tool; it's the easiest place to try a new format.
* Comments say **why**: what the file does and how you know. See the notes in `docs/FORMATS.md`.
* Add a line for your change to `CHANGELOG.md`.

Contributions are accepted under the project's [MIT License](LICENSE).
