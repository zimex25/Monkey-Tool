# Monkey Tool

**A modding tool for EA's mobile racing games: browse the game files, convert models, textures and sounds, and put your own work back in.**

by **GM25** · Windows (GUI + command line) · the command-line core also builds on Linux and macOS

Monkey Tool reads the game files of:

| Series | Games |
|--------|-------|
| Need for Speed | Undercover, Shift, Hot Pursuit, Shift 2 Unleashed, Most Wanted (2012, mobile), No Limits, No Limits VR, Edge |
| Real Racing | Real Racing Next, Real Racing 3, Real Racing 2, Real Racing, Real Racing GTI |

It opens on a **game profile**: you pick the game and point it at that game's files, and the archives are indexed where they are. **Nothing is unpacked to disk.** An asset's bytes are read the moment you click it.

## Features

* **3D viewer** with textures, LOD levels, body kits, paint colours (NFS Shift and Shift 2), wheels placed the way each game places them, and animations. Cars and whole tracks, including the iPhone games' city maps.
* **Export**: models to **FBX / OBJ** (with textures), textures to **PNG / JPG / BMP / TGA / DDS**, sounds to **WAV**, data files to **text**.
* **Import**:
  * your own pictures back into the game's textures (`.sba`, `.pvr`, `.dds`, `.z`);
  * OBJ / FBX models into Real Racing 3 cars, part by part, or as a whole **new car** built on an existing one.
* **Car colours** for NFS No Limits: cars open in the colours the game gives them (body, rims, brake calipers, from the car's own setups), and any other setup of the car can be picked. Exports ask whether rims, calipers and body come out painted or in the textures' default colours.
* **Rim paint** for NFS No Limits: paint a wheel texture in any of the game's own rim colours, or a colour of your own.
* **Limited-time layers** on NFS No Limits tracks: the Halloween, Christmas and event decorations a region carries, switched on in the viewer and taken along on export.
* **UI texture packs** (NFS No Limits `texturepacks/ui`): every picture of a pack saved under the game's own name.
* **Save Editor** for NFS No Limits and Real Racing 3.
* **Data editor**: Real Racing 3 data files (`.sounddef`, `.evt`, tables) as editable text.
* **Video player** for the games' films (NFS Undercover's `.m4v`, Shift's `.mov`).
* **Several assets at once**: Ctrl+click and Shift+click select rows as in Windows Explorer,
  then export or extract them together.
* A **command-line tool** (`monkeytool_cli`) for batch work.

Changes are kept as *pending* until you press **Save**. The original file is kept once as `.bak`.

```
models/cars/jaguar_xe_sv/jaguar_xe_sv.sb3d        ->  .fbx / .obj
textures/cars/bmw_m3_e30_1992/..._alpha.sba       ->  .png / .jpg / .bmp / .tga / .dds
audio/<bank>.bnk                                  ->  the sounds it holds, as .wav
cars/1979_porsche_935/1979_porsche_935_a.m3g      ->  .fbx / .obj (Real Racing 3)
cars/1979_porsche_935/..._misc.etc.dds.z          ->  .png (Real Racing 3)
vehicles/<car>/<car>.sb3d                         ->  .fbx / .obj (Real Racing Next)
```

## Download

Ready-made builds are on the **[Releases](../../releases)** page: unzip and run `MonkeyTool.exe`.

Every push is also built by GitHub Actions. To get one of those builds, open the **Actions** tab, pick a run, and download the artifact.

## Building it yourself

### Windows: double-click `BUILD.bat`

You need nothing installed. If no C++ compiler is found, the script downloads a portable one (w64devkit, about 90 MB) into this folder. It uses it from there and installs nothing on your system. It also fetches `libzstd.dll`.

```
dist\MonkeyTool.exe        the program
dist\monkeytool_cli.exe    the command-line version
dist\libzstd.dll           needed for 3D models
```

The **LZHAM** decoder (No Limits' `.m3g` models, No Limits VR's packed files) is compiled into the program, so no extra DLL is needed.

> Don't click inside the build window while it runs. Selecting text makes Windows pause the console, and the build looks frozen until you press **Esc**.

### CMake (Visual Studio, MinGW, or Linux/macOS for the command-line tool)

```
cmake -B build
cmake --build build --config Release
```

On Linux and macOS only `monkeytool_cli` is built; the GUI is Windows-only. `tests/check_windows_syntax.sh` checks that the Windows sources compile against stub headers, so you can catch errors without a Windows machine.

### Runtime libraries

| File | Needed for | Where it comes from |
|------|------------|---------------------|
| `libzstd.dll` | 3D models and some texture cabinets | [zstd releases](https://github.com/facebook/zstd/releases), fetched by `BUILD.bat` and CI |
| `tools/vgmstream/` | playing and converting Wwise `.wem` sounds | [vgmstream](https://github.com/vgmstream/vgmstream), included |

The tool has its own code for DEFLATE, PNG (including Apple's iPhone PNGs), JPEG, ETC1/ETC2, DXT, ASTC, PVRTC, Brotli and LZHAM, so it needs no other libraries.

## Using it

1. **Choose a game.** The tool starts on the profile picker.
   **File → Change game...** brings the picker back.

2. **Point it at the game's files.** For No Limits, pick the app folder or its
   `files\packs` subfolder; for Real Racing 3, the app folder or its
   `files\.depot` — it finds the right one either way and searches subfolders.
   Only the manifests are read, a few kilobytes per archive, so a full install
   is ready in seconds and costs **no disk space at all**.

3. The tree shows every asset under its real in-game path. A **progress bar**
   in the status strip fills as the files are read, so a big install shows how
   far along it is rather than just sitting there. **Type in the
   Search box above it** to narrow the list: `bmw` leaves only the BMW assets,
   `porsche dds` only their textures. It matches the file's own name, not the
   whole path, and the results are expanded for you. Clear the box to get the
   whole tree back.

4. **Double-click any asset to save it.** The file-type dropdown in the Save
   dialog chooses the format:

   | Asset         | Formats offered                      |
   |---------------|--------------------------------------|
   | `.sb3d`       | FBX, OBJ                             |
   | `.m3g`        | FBX, OBJ                             |
   | `.sba`        | PNG, JPEG, BMP, TGA, DDS, original   |
   | `.wem`        | WAV, original                        |
   | `.bnk`        | every sound inside, WAV, text, original |
   | `.sbfx`, `.sb` | text, original                      |
   | `.etc.dds` (RR3) | PNG, JPEG, BMP, TGA, original     |
   | `.points` (RR3) | text, original                     |
   | `.pvr`, `.ptc.pvr` | PNG, JPEG, BMP, TGA, original   |
   | `.nct` (RR3)  | text, decoded `.bin`, original       |
   | `.gui` (RR3)  | decoded XML, text, original          |
   | anything else | text, original data                  |

5. **Select a model** and it appears in the 3D viewer:

   | Action | What it does |
   |--------|--------------|
   | drag | turn the model |
   | right-drag | slide the view |
   | wheel | zoom |
   | **Ctrl** + drag | move the model, X and Y |
   | X / Y / Z boxes | move the model by an exact amount |
   | LOD dropdown | show one LOD, or all of them at once |
   | Textures | switch between textured and plain shading |
   | Vertex colours | multiply in the mesh colours (baked occlusion) |
   | Reset view | back to the starting camera and position 0,0,0 |

   The viewer draws with the model's own textures and LOD levels. Panels are drawn
   two-sided: the game's shells are single-sided and their winding is not
   consistent, so culling them left holes and the car looked see-through.
   The additive light-glow cards are left out of the viewer, because drawn
   solid they sit over the lights as grey slabs.

6. **Select a sound or a bank** and the pane shows it as text, with **Play**
   and **Stop**. A bank lists every sound it holds in the dropdown - pick one
   and play it. Saving the bank writes them all as `.wav` (see [Audio](docs/FORMATS.md#audio-bnk-and-wem) in the format notes).
   Data files are shown as text too.

7. **File → Convert all shown to a folder...** does the whole list at once.
   To write only some assets, **Ctrl+click** rows to add or remove them and **Shift+click** to
   take a range, then right-click the selection or use **File → Export selected**.

   A film (`.m4v`, `.mov`) plays in the viewer's place, with Play, Stop and a seek bar under it.

8. **View** menu: filter to textures / models / sounds, and switch between
   **Light** and **Dark** theme. The choice is remembered between runs; on
   first launch it follows your Windows theme.

9. **Save Editor.** The floppy-disk button to the right of the search box,
   **Tools → Save Editor**, or Ctrl+E opens the save editor in its own
   window, which you can keep open beside the tool. Pick the save folder (No
   Limits: `saves-encrypted`; Real Racing 3: `doc`; a parent folder works
   too) or drag it onto the window.
   * **Quick edit** has the usual fields: cash, gold, level, fuel and so on
     for No Limits; R$, Gold, M$ and driver level for Real Racing 3.
   * **All fields** lists everything in the save, with a search box.
   * **Saving**: every save first backs up the original files to a
     `save_editor_backup_<date>` folder beside them. The No Limits manifest
     hashes are updated, and so is Real Racing 3's `TempSaveGame.dat` copy.
   * **Real Racing 3 calibration**: the game hides money and level behind a
     key, so type the values the game shows once and click **Calibrate from
     game**. It is remembered in `%APPDATA%\MonkeyTool\rr3_keys.txt`, and a
     calibration made with the old separate editor is picked up
     automatically.

### Command line

```
monkeytool_cli list    <gameFolder>
monkeytool_cli extract <gameFolder> <outFolder>
monkeytool_cli convert <gameFolder> <outFolder> --tex png --mdl fbx
monkeytool_cli one     <file.sb3d>  <out.obj>   --format obj
```

`--only textures|models|sounds` limits what gets written.

## Documentation

* **[CHANGELOG.md](CHANGELOG.md)**: what changed in each version.
* **[docs/FORMATS.md](docs/FORMATS.md)**: notes on every game format the tool reads and writes (archives, models, textures, audio, saves, the Real Racing 3 car files). They're useful if you want to write your own tools.

## Contributing

Bug reports and pull requests are welcome. See **[CONTRIBUTING.md](CONTRIBUTING.md)**. When something looks wrong, a screenshot from the tool next to one from the game, plus the file involved, is usually all that's needed.

## License

Monkey Tool is released under the **[MIT License](LICENSE)**.

It includes or downloads third-party code under its own licenses (Brotli, vgmstream, zstd, LZHAM). See **[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)**.

## Disclaimer

Monkey Tool is an unofficial fan project. It is not affiliated with, endorsed by or connected to Electronic Arts, Firemonkeys, Criterion or any other rights holder.

*Need for Speed*, *Real Racing* and the related names and logos are trademarks of their owners. The game files themselves are **not** part of this project, so use the tool on files from games you own.

Changing save files or game files of online games may break the game's terms of service. You do that at your own risk.
