# Changelog

Every release of Monkey Tool, newest first.

## 1.2.2

### NFS No Limits
* **Cars in their real colours.** The viewer paints a car the way the game does: body, rims
  and brake calipers in the colours of the car's stock setup (`data/car_setups/<car>/Bodykits/
  <X>_STOCK.sb`, matched against `data/colours/colours.sb`). The 2015 GT3 (991) now opens in
  GT3 Orange with silver rims and yellow calipers instead of the red stand-in and white rims.
  The list under the view offers the car's other setups (LTS stock, the AI and custom
  versions) and "textures" for the plain textures.
* **Export asks about the colours.** Saving a car asks whether the rims, brake calipers and
  body come out painted (as in the game) or in the textures' default colours. Painted, the
  textures are written already coloured (`texture_wheel_x_painted_c0c0c0.png`, `paint_e02b07.png`).
  Saving a rim texture on its own asks the same, with the colour the game gives that wheel
  (its car's stock setup, or the wheel's `DefaultColour` in the visual parts); a brake caliper
  texture uses the colour of the car open in the viewer.
* **UI texture packs load.** `texturepacks/ui/*.sba` (LTS cards, menus, crates, crews) showed
  nothing: their pictures are palette images packed with LZ4, inside a `TexturePack` the reader
  did not look into. All 252 packs sent in now open, and saving one as PNG offers every picture
  in it as its own file, under the game's own name (`FrontEnd/LTS/General/xrc_art.png`). The
  vector icons (`.svg`) in some packs are listed but not drawn.
* **Limited-time layers on tracks.** A region's event decorations (Halloween 2022-2024,
  Christmas 2022-2023, Lunar 2022, the 2024 Paradyne finale ...) are read from its `.scene.sb`
  and can be switched on in the list under the view: the models the layer places, and its own
  geometry (`<layer>.scene_static.sba`) when that file is in the library - the status line says
  what was found. Exporting the track then asks whether to take the layer along.

### The tool
* Command line: `paint <setup.sb> <colours.sb>` (a setup's colours), `pack <texturepack.sba>
  [folder]` (every picture of a pack), `lts <region.scene.sb>` (a region's limited-time
  layers), and `render --setup <setup.sb> --colours <colours.sb>` to render a car painted.

### Two packages
* `MonkeyTool_1.2.2.zip` - the classic download: the source and the one-click build
  (`BUILD.bat`), nothing else.
* `MonkeyTool_1.2.2_GitHub.zip` - the repository as it goes on GitHub: the same source plus the
  CI workflow, issue templates, `.gitignore`, contributor notes, the Windows syntax check and
  the tools.

## 1.2.1

### Fixes
* **NFS No Limits: wheels and ride height.** Every wheel hangs from the car's suspension joint
  again, with the tyre radius from the car's prefab. All four tyres now touch the same ground:
  the Porsche 718 Cayman GT4 no longer sits high at the front and its front wheels are back in
  the arches, and the Lotus Elise no longer sits high at the rear. (1.2 took some wheels from
  the visual locators, which do not share a common ground.)

### The tool
* **Paint colours for NFS Shift and Shift 2.** A paint list sits next to the body kit list
  in the viewer; picking a colour swaps the body, body-kit and bonnet textures to that paint
  (`_01` to `_07`). It shows only for cars that have more than one paint.
* **Player buttons are icons.** Play / Pause, Stop and the track buttons are drawn as icons
  in the theme's colours, so they read well in both the dark and the light theme.
* **Faster.**
  * The viewer decodes a model's textures on every processor core; big Real Racing 3 and
    Shift tracks open several times faster.
  * Texture and asset lookups use sorted indexes instead of scanning the whole asset list,
    which speeds up opening cars and tracks in large games.

## 1.2

### The tool
* **Video player.** NFS Undercover's `.m4v` and Shift's `.mov` films play in the viewer's place,
  with the player controls under it: Play / Pause, Stop, a seek bar you can drag, and the time
  out of the film's length. A click on the picture pauses. It uses Windows' own Media Foundation
  decoders; where those are missing the film opens in the system player instead.
* **Select several assets.** Ctrl+click adds or removes a row, Shift+click takes a range, as in
  Windows Explorer. Right-click the selection, or use **File > Export selected** / **Extract
  selected**, to write them all at once. A selected folder counts as everything in it.

### New games: NFS Undercover, NFS Shift and NFS Shift 2 Unleashed (iPhone)
* Pick the game, then the folder with its `.ipa`. The `.ipa` is read in place.
* **A new M3G reader** for these three games. Their `.m3g` files are standard JSR-184, and
  Shift's are gzip-packed.
  * Each part is placed by the scene's node tree.
  * The textures a file carries inside it are decoded and shown, and exported with FBX / OBJ as
    PNGs.
* **Textures:** `texture_*.m3g` in EA's PVRTC 4 bpp format (codes 124 / 125) now decode, and so
  do Apple's iPhone PNGs (CgBI).
* **Cars:**
  * Each part draws from the texture the game gives its appearance: the car's own
    (`texture_car_<name>.m3g`, the body kit's body included), the arches and sills from
    `_bodykit`, the racing livery and decals from `_livery`, Undercover's shop spoilers from
    `texture_spoilers.m3g` and its vinyls from `texture_vinyls_01.m3g`. Before, the kit's body
    took the wrong picture and the cars looked like a patchwork.
  * Decals and badges are cut out by the texture's alpha. This removes the rainbow noise around
    the "MEGANE" letters on Shift 2's Mégane RS.
  * **Wheels** from `wheels.m3g`, on the car's own wheel locators, with their textures.
  * **Traffic and police cars** (`car_civ_*`, `car_cop_*`) get
    `texture_civilian_and_cop_mastertexture.m3g`, and so do Undercover's road-side props
    (`objects.m3g`).
  * **Stock is the car as it ships.** The wide arches and sills (parts 25 and 126) were shown on
    the stock car; they are body kit parts. Kit B is the first body kit (25, 100-137), kit C the
    second (126, 160-199), kit + the shop parts, liveries and vinyls. The shadow cards are hidden.
* **Cockpits (Shift):** `cockpit_<car>.m3g` gets `texture_cockpit_<car>` and the steering wheel
  `texture_steeringwheel_<car>`, `bonnet_<car>.m3g` gets `texture_bonnet_<car>`, the driver's
  arms `texture_cockpit_arm_01`. Shift 2's windscreen cracks are an option (kit +).
* **Tracks are laid out.** A location's `.m3g` is a kit of road pieces stacked at the origin,
  which is why the maps looked like a mess. The event's layout file (`causeway_*.bin`,
  `chicago_0N.bin`, `locationc_01.bin` ...) places every piece on its grid, turned and mirrored;
  the viewer uses the location's most complete layout. Pieces turned a quarter turn are turned
  the right way (some corners pointed the wrong way), and a stray marker Shift's Tokyo keeps
  kilometres away no longer shrinks the map to a dot.
* **Shift 2's track textures** are now read from `texturelist_<track>.bin`, each appearance to
  its own atlas: the road, the scenery atlases, the people and trees, the tyre marks.
* **Skies:** `skydome_<location>.m3g` and Undercover's `skydomes.m3g` get their
  `texture_skyline_*` pictures.

### NFS No Limits
* **Porsche 911 GT3 RS (2023): rear wheels sunk into the body.** The car shares one wheel model
  between both axles, so the much wider rear tyre sat inside the wing. A wheel hung on its joint
  now comes out to the body side over it (by at most 8 cm). The car's visual parts file
  (`C_Porsche_911_GT3_RS_2023_1.sb`) is now found too: `gt3_rs` and `gt3rs` are the same name.
* **Lotus Elise (and cars like it): wheel position and ride height.** The wheel joint of some
  cars is the suspension hanging free, several centimetres below and behind the arch, so the
  car looked jacked up. The wheel now goes where the game draws it
  (`locator_visual_wheel_*`) when the arch's own locator agrees, and the brakes move with it.
* **Garbled textures (Ford GT and others).** Some `.sba` files store a format number that fits
  two codecs of the same size, such as PVRTC 4 bpp or DXT1, and DXT3 or DXT5. The tool now
  decodes both and keeps the one that forms a picture rather than block noise.

### NFS No Limits VR
* **LZHAM is built in.** The decoder for No Limits' `.m3g` models and No Limits VR's packed files
  is now compiled into the program.
  * There is no `lzham_x64.dll` any more, so there is no "missing module" error and no
    "invalid file" when picking one.
  * `BUILD_LZHAM.bat` is gone.

### NFS Most Wanted
* **Downloadable cars (Jaguar C-X16 and others) had no textures.** They carry 8x8 stand-in
  pictures where other cars name their textures. Their textures are now found by the car's name
  (`textures/cars/<car>/texture_<car>_diffuse_00` and `_alpha`), as the game does. The car's
  texture folder has to be among the game files.

### Real Racing 3
* **Track objects whose picture has a sibling's name.** `common/org/org_tree_oak_d/` holds
  `org_tree_oak_c.etc.dds.z`, `common/veh/veh_caravan_e/` holds `veh_caravan_d`. They are now
  found by their folder, and the billboard trees by their `billboarded/` folder. Brands Hatch is
  fully textured when the game's `Tracks/common` folder is there.
* **The sky is drawn with the track.** Opening `processed/high/<track>.m3g` also loads
  `processed/<track>_sky.m3g`.
* **Trees, fences and the sky ring on tracks are see-through.** Their alpha sits in a second
  file, `<name>_alpha.etc.dds.z`, which the viewer and the preview now apply.
* **Sky:** a sky copied from another track (Brands Hatch's `hatch_sky.m3g` asks for
  `cota_sky_hills`) now gets the track's own `hatch_sky_hills` picture.

### Real Racing and Real Racing GTI
* **Untextured wheels and skies** (`exotic_wheel_01.m3g`, `gti_wheel_04.m3g`, `forest_sky.m3g`):
  these files keep their one texture's name where the reader did not look. It is now found, and
  `exotic_wheel_01.pvr` beside the wheel is used.
* The cut-scene cars (`car_sedan_cutscene.m3g`) use the car's own texture (`car_sedan_ext_01`).
* A texture is now found even when the model spells its name in a different letter case than
  the file, as iOS allows.

### Game picker
* The Need for Speed row now holds eight games, oldest first.
* A folder holding one of the new `.ipa`s is recognised as that game.

## 1.1.5

### NFS No Limits - rim paint (CAR COLOR)
* **Paint a wheel texture in any of the game's rim colours.**
  * Select a wheel texture (`textures/cars/wheels/texture_wheel_<name>.sba`). A **CAR COLOR**
    list appears under the picture.
  * The list holds the game's own rim colours from `data/colours/colours.sb` (116 common ones,
    plus the brand colours such as Ferrari's). The last entry is **Custom colour...**.
  * Picking a colour shows the painted texture straight away.
  * **Put into texture** keeps it. **Save** (toolbar or Ctrl+S) writes it into the game file, the
    same way as an imported picture.
* **Only the rim is painted.** The tool finds the wheel's model (`wheel_<name>.sb3d`) and paints
  just the texels that the model's tinted rim parts use. Logos and centre caps keep their own
  colours.
  * Without the model, every grey texel is painted and colourful ones are left alone.

### NFS No Limits - wheels
* **Tyre width, second fix.** `TireWidthOffset` is now applied to each side of the tyre, and only
  to the tyre.
  * The Beck Kustoms F132's rear tyres are now as wide as in the game: +11 cm on each side.
  * The rim stays where it is and sits deep inside the tyre, as the game shows it.
* `WheelOffset` (the wheel pushed further out) from the same file is applied too.

### Real Racing Next - wheels
* **Wheels in the right place and at the right size.** They now go on the car's own
  `POINT_WHEEL_FL/FR/RL/RR` carpoints: the centre of the wheel's face, with the right-hand ones
  turned half a turn.
  * These points match the low-detail `LOD_F_MERGED_WHEELS` exactly.
  * Before, the tool guessed from the wheel arches and the body's floor. That put the wheels about
    10 cm inside the body and too high: on the Camaro the tyres reach 17 cm below the floor.

### Real Racing 3 - New car and Import
* **Lights showing the wrong texture in the game.** Every car the game ships has a second,
  unnamed texture-coordinate array per part, and it is all zeros.
  * The tool used to fill that array with a copy of the first set. The game uses it for a second
    texture, so the lights showed another picture.
  * It is now written as zeros, like the game's own files, unless your model brings a second UV
    set of its own.

### Saving
* **Save as... > Original file** now saves the file exactly as the game has it. Before, an
  `.sb3d` or `.m3g` came out as an FBX.

## 1.1.4

### Tyre width (NFS No Limits)
* **Tyres are as wide as in the game.** A No Limits car shares one standard wheel model. The game
  widens the tyres per car, and the amounts are in `data/visualparts/C_<car>_1.sb`
  (`FrontTireWidthOffset` / `RearTireWidthOffset`). The tool now reads that file too.
  * Example: the Beck Kustoms F132 gets +6 cm at the front and +11 cm at the rear.
  * The wheel is pulled apart at the middle of the tread, and the rim barrel stretches with it.
* `WheelRadiusScale` and `TireProfileOffset` from the same file are applied as well.
* The info panel says how much each axle was widened.

### Real Racing Next viewer
* **UV mapping fixed.** Real Racing Next counts texture rows from the top down.
  * Before, every part took its picture from the wrong half of its atlas. For example, the
    headlight lens showed the red tail-light bars.
  * Headlights, tail lights and badges now show the right pictures.
  * FBX / OBJ exports get the corrected coordinates too.

### Real Racing 3 - New car
* **The in-game crash after about 5 seconds.** The rebuilt file now follows the original car's
  layout more closely:
  * The parts are written in the original car's order.
  * Each part is cloned from the original part with the same name, instead of from any part
    that shares its material.
  * The material list is kept as the last object, as in the game's own files.
* **Lights look wrong or are missing?** Your model's texture coordinates point into the textures
  of the car it came from. On a different car's atlas they pick the wrong pixels. Import your
  model's own textures into that car's `.pvr` / `.dds` atlases, or lay the model's UVs out on the
  target car's atlas.

### Profile pictures
* **NFS No Limits:** the picture now goes into a raster avatar
  (`2x/textures/ui/defaultavatars/texture_avatar_default.sba`).
  * `texture_icon_common_default_profile.sba` is a vector (SVG) drawing, so it cannot take a
    picture. The tool now says so instead of failing.
* **Real Racing 3:** the saves hold no picture. The profile picture comes from your online
  account (Facebook / Game Center / Google Play), so it has to be changed there. The tool now
  says so.

## 1.1.3

### Real Racing 3 - New car and Import
* **Names are checked before a model goes in.** Before building, the tool compares every part and
  material name in your OBJ / FBX with the names the car uses. Each wrong one is listed:
  `FOUND INCORRECT PART NAME: LOD_A_BODY_mm_badges - correct name: LOD_A_BOOT_mm_badges`.
  * **Yes** uses the car's names, **No** keeps yours, **Cancel** stops.
  * Suggestions go by the same spelling first (case, spaces and `_` ignored), then by the same
    `_mm_<atlas>` ending and the nearest part name.
* **No empty parts.** 1.1.2 wrote parts your model did not have as empty stand-ins, and that is
  the likely cause of the crash.
  * A damaged or broken part (`..._DAMAGE`, `..._BROKEN_A`, the bonnet-cam hood) is now a copy of
    your intact part, the way the game's own files pair them.
  * Any other part the model has no counterpart for becomes a 1 cm triangle hidden inside the
    body.
  * The result has exactly the part list of the original car.

### Real Racing 3 viewer
* **Wheels on lower detail levels.** A car's `_b` to `_h` files hold one detail level each, and
  the wheels now go on that level. Before, they stayed on LOD00: on `_d.m3g` the wheels showed
  alone, and vanished when LOD03 was picked.

### Real Racing Next
* **The smeared rear bumper was the car's shadow mesh** (`LOD_A_SHADOW`) drawn as a solid part.
  Shadow meshes are now helpers, hidden like collision meshes.
* **Light lenses** use the car's see-through `<car>_lights_glass.sba`, so the lights behind show.
* **Wheels.** The wheel is modelled once at the origin. It is now placed at the four wheel arches
  from `<car>_carpoints.sb`, and mirrored for the right side.
* **Texture coordinates were checked**: 99% of vertices sit in 0..2 as they should. The values
  around 16 belong to tiled materials (leather, carbon, glass), which repeat on purpose.

### Need for Speed: No Limits VR
* **The Jaguar F-Type interior shows.** The tool read the "f" in `jaguar_f_typer` as a body-kit
  letter, and the default Stock kit view hid every part. Kit letters now count only when the car
  has a stock (`a`) kit.
* **Skinned meshes** (the driver's arms on the steering wheel) no longer get garbage after their
  names.

## 1.1.2

### Need for Speed: No Limits VR
* **Textures and materials load from the `.obb`.** The VR game packs its files with LZHAM, not
  the Brotli Edge uses. Each file entry states its codec (0 stored, 1 deflate, 2 LZHAM, 4 Brotli),
  and the tool now uses the one stated, trying the others if that fails. This is the same rule
  your FMOBB Dumper follows.
  * LZHAM needs `lzham_x64.dll` next to MonkeyTool.exe, the same one No Limits already uses.
  * The 1043 car textures of the dump you sent all decode.

### Real Racing 3 - New car and Import
* **Cars built with New car show up in the game.** Two things made the game drop the meshes:
  * **Unknown material names.** 3ds Max had renamed `Vehicle Exterior_mm_ext` to
    `Vehicle_Exterior_mm_ext`, and the tool added those as seven new materials the game does not
    know. Materials are now matched regardless of case, spaces, `_`, `-` and `.001`. When that
    fails, the `_mm_<atlas>` ending is matched. An unknown material is never added.
  * **An array left behind.** Real Racing 3 writes five arrays in front of every vertex buffer
    but names only four; the fifth (a second UV set) is read by position. The tool cloned only
    the four, so the fifth kept the old car's vertex count. All five are now cloned and written,
    and the file has the same object layout as the game's own (arrays, buffer, indices, mesh; the
    material list last).
  * This also fixes Import (replace) on cars laid out this way.
* **Every part name is kept.** Parts of the original car that your model does not have (damaged
  panels, broken lenses, the bonnet-cam hood, the steering wheel) are written as empty stand-ins,
  so the game still finds each name it looks for.

### Real Racing and Real Racing GTI
* **Wheels sit flush.** A Real Racing 1 wheel point marks the wheel's outer face, not its hub,
  so the wheel now goes inward from it instead of sticking out of the body.
* **Golf GTI brake lights** use `gti_lights.pvr`, which the car's `.rr_car` names, not the body
  texture.

### Real Racing Next
* **Cars are textured** from `texture_astc/vehicles/<car>/car_textures/` (open a folder that
  holds both `vehicles` and `texture_astc`).
* **Paint is no longer black.** Paint parts are coloured by the game's paint shader, and
  `<car>_ext.sba` is black under the paint. Paint is now left untextured, so it shows grey.
* **Wheels are missing.** The wheels are separate models that were not among the files you
  sent.

## 1.1.1

### Saving your changes
* **Imports stay in the tool until you click Save.** When you import a picture into a texture, or
  a model into a car, the result shows in the preview and the 3D view right away. Nothing is
  written yet.
  * The window title gets a `*`, and the new **Save** button (blue disk, Ctrl+S, also under File)
    lights up.
  * Save writes each file over the original in the game folder. The original is kept once as
    `.bak`.
  * A file that lives inside a `.pack` or `.obb` is saved where you choose.
  * Closing the tool or opening another game asks about unsaved imports first.

### Real Racing 3
* **New car** (the car-with-NEW button, Real Racing 3 only). Select a car's `<car>_a.m3g`, click
  New car, and pick an OBJ or FBX. The whole model becomes a new `.m3g` in that car's place. It is
  built on the selected car's own frame (header, materials and mesh layout), so the game reads it
  like the original.
  * Each object becomes a mesh. Name them `LOD_A_...` for the detailed model, `LOD_B_...` for the
    next level and so on; names without the prefix get `LOD_A_`.
  * Materials are matched by `usemtl` name. For example, `Vehicle Exterior_mm_ext` is textured
    from the car's `_ext` texture. A material the car does not have is added to its list.
  * Centimetre and inch models are scaled to metres, and 3ds Max's Z-up is turned to the game's
    Y-up. The car is set on the ground.
  * Objects that share a name (3ds Max and Blender split a mesh per material) are joined into one
    mesh. This applies to Import (replace) as well.
* **The driver** (`driver/driver_lod_*.m3g`) is posed. His fifteen parts are placed by
  `driver.banim` (or `driver_<car>.banim`), and Animate shows him steering.
* **Finish the .nct pad** no longer crashes. The 14 MB `events.dat.nct` asked for 14 GB of memory;
  the pad is now learned only as far as enough files reach.

### Real Racing and Real Racing GTI
* **Textures are right.** These games state their texture scale as it is. The tool divided it by
  four, as Real Racing 2 and 3 need, so only a corner of each texture showed. The tool now
  checks this per file.
* **Wheels are textured.** Many `.rr_car` records name a texture that the game no longer ships
  (`muscle_wheel.pvr`). The wheel model's own `<wheel>.pvr` is used instead.
* **GTI cars get their points, wheels and textures.** `car_2door_gti.m3g` uses `car_gti.points`,
  which only its `.rr_car` names.

### Real Racing Next
* **Materials are matched to textures** using the table in the game's own
  `common_materials/atlas/*.sbma`. For example, misc_interior uses `_int`, caliper and chassis use
  `_ext`, and glass, mirrors and tyres use the common textures.
* **Car textures are still missing.** The `vehicles` folder you sent has no `car_textures`
  folders, which is where the game keeps each car's textures. They need to be in the folder the
  tool opens.

### Look
* **Loading.** Only the loading window shows progress now; the status line and the bar in the
  corner are gone.
* **Icons.** There is a new Save Editor icon, plus new Most Wanted and Edge profile icons.

## 1.1

### The tool itself
* **Game picker.** The picker is now a compact window like Frosty's "Load Profile": each game is
  a tile showing only its icon and name. The Need for Speed games are on the first row (Hot
  Pursuit, Most Wanted, No Limits, No Limits VR, Edge). The Real Racing games are on the second
  (Real Racing, GTI, Real Racing 2, Real Racing 3, Real Racing Next). Double-click a tile, or
  select one and press Enter.
* **Opens maximised.** You can still make the window smaller and move it around.
* **Loading window.** While a game's files are indexed, a Frosty-style window shows the tool's
  icon (large), the game's picture, a gradient bar and what is being read. It closes when the
  tree is ready.
* **Icons.**
  * The asset tree has an icon for each kind of asset: folders, textures, models, sounds, data,
    scripts, archives and animations.
  * A toolbar above the tree has **Open**, **Export** and **Import** buttons.
* **Bigger text** in the asset tree (13 pt).
* **Dark mode.** Labels such as "Textures" are readable now.
* **Unsupported files** now show the line "Monkey Tool does not support this format" instead of
  a hex dump. Double-click still saves the file as it is.
* **Export choices.** Double-click an asset (or use Export) and the Save As dialog offers
  everything the asset can become, plus **Original file**:
  * models: FBX or OBJ;
  * textures: PNG, JPG, BMP, TGA or DDS (DXT1, or DXT5 when there is alpha);
  * sounds: WAV;
  * data files: TXT, and XML where the file is XML.
* **Faster.** Texture and sidecar look-ups use an index by file name instead of a walk over the
  whole library.

### Save Editor
* **Player name**, in both games. For Real Racing 3 it is `m_name` in `character.2.dat`. For No
  Limits it is the name field of the profile section: the new name goes into that section's
  string table, and the section and the manifest are rewritten.
  * If a save keeps no name (the game takes it from your account), the box says so.

### Real Racing 3 - car mods
* **Model import (Import on a car `.m3g`).**
  1. Export the car as OBJ or FBX.
  2. Edit it in Blender, 3ds Max or another 3D program.
  3. Import the result. Objects replace the car's meshes of the same name, such as
     `LOD_A_BODY_mm_ext`.
  * Objects the car does not have yet are added. Each new mesh is cloned from one of the car's
    meshes with the same material (`usemtl`), so the game draws it with that material.
  * Parts the tool placed on their `.points` (steering wheel, hands) are moved back where the
    file keeps them.
  * A model saved in centimetres is scaled back to metres.
  * The file is written as the game wrote it: section lengths, file size and Adler-32
    checksums are recomputed. Exporting and importing a car without changes gives back the
    same bytes.
  * The original is kept as `.bak`.
  * OBJ is the easiest route through Blender (Blender does not open ASCII FBX). The importer
    reads both binary FBX (Blender, 3ds Max, Maya) and ASCII FBX.
* **Textures.** Import a PNG or JPG into any `.dds.z` texture. It is rebuilt at the texture's own
  size and codec and re-wrapped as `.z`.
* **Data editor (Import on a data file).** The file opens as text; **Save to game format** writes
  it back:
  * `.sounddef` files by field: name, group, settings and the `.wav` files the sound plays. Add
    or remove `sample` lines to change the sound. Selecting a `.sounddef` also plays its first
    `.wav`.
  * Track `.evt` files (grid and camera positions), `.dat` tables and sound-mix `.bin` files as
    a line-by-line listing (`str`, `f32`, `u32`, `hex`).
  * JSON, XML and text files as they are.
  * Unchanged text gives back the identical file; this was checked on 677 files from the game.
  * **Save as .txt** and **Load text file** let you edit in your own editor.
* **What cannot be edited.** The `.nct` tables (cars, series, events, liveries), `.cc_cust`,
  `prof.dat` and `upgrades.dat` are encrypted with a key only the game holds. The editor says
  so; it does not show noise. So adding a car or a racing series is not possible yet.

### Need for Speed: No Limits
* **Lotus Elise wheels** are sized from the car's prefab (`RaycastAxle` `WheelRadius`, front and
  rear separately) instead of the wheel model's own size.
* **Body kits.** When a custom part is chosen, the stock part in the same slot is hidden.
* **Cop cars.** Models hung on the car in its prefab, such as the roof light bar, are loaded and
  placed where the prefab puts them.

### Need for Speed: Most Wanted
* **Animated parts**, as in Real Racing 3. The M3G keyframe tracks (spoilers, flaps) play in the
  viewer on every LOD.
* **FMOD `.fev` / `.fsb`.** The sound bank lists its sounds by name; pick one and press Play
  (through vgmstream).

### Real Racing Next
* **Models load.** The shared vertex declarations of format 21 are read. `LOD_A_` becomes
  LOD00, and the bonnet cam, interior cam, damage and collision meshes get groups of their own.
* **Textures** are found in `vehicles/<car>/car_textures/`.

### Real Racing 2
* **Cars are solid.** Body parts (`MESH_...`) are opaque; only glass, decals, grilles and nets
  use alpha.
* **Detail levels.** More LOD suffixes are recognised (`_LOD0`-`_LOD5`, `_FAR`, `_DISTANT`,
  `_SIMPLE`), so LOD 0 no longer picks up low-detail meshes.

### Real Racing and Real Racing GTI
* **Organised model.** Body LOD 0-3, brake-light glow and cockpit are each a group of their own,
  instead of 36 unnamed meshes.
* **Steering wheel and hands** sit on the car's steering point, not in the middle of the car.
* **Textures.** The textures the car's `.rr_car` file names (body, interior, steering wheel,
  wheels) are applied.

### Known limits
* **No Limits VR textures.** The VR game uses a different `.sba` layout; a sample from
  `textures\cars` is needed to add it. The models load.
* **Real Racing Next sounds** are in a format not identified yet.

## 1.0

### New games
* **Need for Speed: Edge.** Point the tool at the folder with the Edge
  `.apk`, or at the `main.<n>.com.ddl.eanfs.qh.obb.png` taken out of it. That
  file is not a picture: it is an "FMOBB-02" archive, one file with the whole
  game inside. The tool reads it in place, even while it is still inside the
  `.apk`. There are 6926 files: models (`.m3g`), textures (`.sba`), data
  (`.sb`, `.lua`) and FMOD audio. The packed files are Brotli-compressed; the
  decoder is built into the tool (Google's reference code, MIT licence, in
  `src/brotli`).
  * Edge's ETC textures keep their alpha in a second texture,
    `<name>_ETCAlpha.sba`. The preview and the 3D view put the two back
    together.
  * Cars get their wheels from `models/cars/wheels/wheel_<car>.m3g`, hung on
    the car's own wheel joints.
* **Real Racing Next.** The profile is enabled. It reads No Limits-style
  `.pack` archives (LZHAM and zstd), SB3D models with float positions, and
  ASTC textures, which are decoded by the tool itself.
* **Real Racing** and **Real Racing GTI** (iOS only). Point the tool at the
  folder with the `.ipa`, which is read in place as a zip. Their `.points`
  files (a count, then int16 millimetres) give the wheel, light, exhaust and
  cockpit points. GTI's `gti_wheel_0N.m3g` wheels go on those points.
* **Need for Speed: No Limits VR.** A new profile. Its `.obb` is read as
  whatever it turns out to be: an FMOBB archive like Edge's, a zip, or
  `.pack` files.

### Need for Speed: No Limits
* **Cop cars get their wheels, and the One-77's wheels are placed right.**
  Wheels now hang on the car rig's own joints (`J_wheel_front_left` and the
  rest), which is where the game puts them. The brake-disc measurement is
  only a fallback now; cop cars have no discs at all.
* **City roads are textured.** A region's material list
  (`.lightmaps.sba`) is found by the region's name wherever it is in the
  library. It lives under `texture_etc/prefabs/tracks/`, not beside the track,
  which is why 0.9.6 reported it missing.

### Real Racing 3
* **Driver arms and hands are solid.** A Real Racing 3 part (`_mm_...`) is
  never drawn half see-through unless it is glass, a light or a shadow. The
  alpha in those textures is a gloss mask.
* **Shaders.** The game's `shaders/fresnel_<x>.rgb.pvr` and `spec_<x>.rgb.pvr`
  lookup ramps are used by the 3D view. Each part gets the reflection and
  highlight of its material type: gloss paint, chrome, glass, tyres, discs,
  carbon, cloth, dashboard and so on. The material-to-ramp matching is by
  part and material name; the game's own table for it is not in the files.
* **Evija wing.** Its four moving parts (`WING`, `WING_REAR`,
  `WING_REAR_LEFT`, `WING_REAR_RIGHT`) each follow their own animation node. Before, they
  were all stacked on the wing. Rotations about the fore and up axes now turn
  the right way too.

### All games
* **Sounds show their waveform** instead of the file's bytes as hex. This
  covers RR2 `.sps`, `.wem`, bank sounds, `.ogg`, `.mp3`, `.wav`, `.fsb` and
  the rest.
* **Save Editor: Profile picture...** takes a PNG, JPG or BMP.
  * If a Real Racing 3 save keeps a picture of its own (a PNG/JPEG blob), the
    new picture replaces it at the same size, and "Save changes" writes it.
  * Otherwise the game draws your avatar from one of its textures (for
    example No Limits' default profile icon, or the avatar the save names).
    With the game open in the tool, your picture is put into that texture and
    saved as a new file.

## 0.9.6.1

* Fixes the 0.9.6 build error ("expected unqualified-id before '='" in
  saveAsset). A variable was named `far`, which the Windows headers define
  as an empty macro. It is renamed. The test headers now define those
  macros too, so this kind of mistake is caught before a release.

## 0.9.6

### Need for Speed: No Limits
* **Track textures are the real ones.** Each region's
  `<track>.lit_scene_<light>.scene_static.lightmaps.sba` (next to the track
  in `prefabs/tracks`) lists every part's material. For each part it names
  the diffuse texture, such as
  `textures/environments/road_new/texture_ashphalt_diffuse.sba`. The viewer
  and the FBX/OBJ export now read that file. In region 04 all 1,158 parts
  get the texture the game uses: asphalt, sidewalks, buildings, bridges,
  cliffs, palms and signs. The day lighting's file is preferred. The old
  guess by material name is used only when no lightmaps file is in the
  library.
* **Exporting a track asks what to take:** the whole area (road, buildings,
  trees, terrain, signs) or the road surface only. For the whole area it
  also asks whether to include the distant low-detail backdrop. Then it
  asks about the textures, as before.
* **Car LODs are LOD00 to LOD05, as in the game.** The numbers at the end of
  mesh names are artist leftovers (`_lod148`, `_lod52`, a `lod_01` group
  full of meshes still called `_lod00`). The level now comes from the car
  file's own node tree (`body/standard/lod_02/...`). The Porsche 911 (991)
  went from 36 "LODs" to 7.
* **Body kits come from the same tree** (`standard_type_a`, `type_b`,
  `kit_y`, `pulled`, `large`, `..._carbon`). The Stock view no longer mixes
  in kit parts at the lower LODs.

### Real Racing 3
* **Tyres show again.** The tyre tread texture keeps a mask in its alpha
  channel, and 0.9.5.1 drew it as see-through. Tyres, paint, rims, discs
  and calipers are now always drawn solid.
* **Rear wings sit where they belong, and move.** The `.banim` format is now
  read fully: every moving part, its position and its tilt. This covers
  both the old and the new header (the McLaren P1 uses the old one). A car
  with a wing animation gets an **Animate** button in the viewer that plays
  it out and back. Checked on the 911 Turbo S, P1, LaFerrari, Nevera and
  Agera R.

### Real Racing 2
* **`.sps` sounds play and save as WAV.** They are EA's SPS streams
  (EA-XAS). The tool decodes them itself, so vgmstream is not needed.
  Checked against a reference decoder on the Z4 engine sounds.
* **LODs:** RR2 names detail levels at the end (`MESH_BODY_HIGH`, `_LOW`).
  These now become LOD00/LOD02. A car with damage parts but no levels opens
  on the normal car, not on the damaged shell laid over it. Parts with no
  level show at every level.
* **Wheels:** the car's own `MESH_WHEEL_*` and `MESH_BRAKE_*` are placed on
  all four wheel points, mirrored on the right. Before, they stayed at the
  middle of the car while another car's wheel was fitted.

## 0.9.5.1

* **See-through parts in the viewer.** Glass, headlight lenses and window
  tints are blended over what is behind them, so you see the interior
  through the windows. Badges, grilles, number plates and track foliage are
  cut out where their texture is clear. No Limits materials say which they
  are (`_opaque`, `_alpha`). For the other games the tool goes by the
  texture's alpha: mostly fully clear or fully solid pixels means a
  cut-out, lots of in-between pixels means see-through. Glass without an
  alpha texture is drawn half see-through. See-through parts cast no shadow.
* **Real Racing 3 Megane RS (and similar names):** the car got someone
  else's wheel. The tool found `<car>_shared.m3g` by taking letters off the
  file name, and `2015_renault_megane_trophy_r_a` lost its `_r` too, so the
  real wheel file was missed. Only the one detail letter comes off now.
* **Real Racing 3 wheel size.**
  - The tyre is sized to reach the ground under the hub, and also to fill
    the wheel arch to about 3.5 cm of its lip. The larger of the two wins.
    Cars like the Zenvo Aurora had model tyres smaller than their arches.
  - Only the diameter grows: the tyre's width stays as modelled.
  - Cars with wider rear tyres (`TYRE_REAR`, as on the F1 cars) use them at
    the back.
  - F1 cars' front wheel guards (`WHEELGUARD_FRONT_*`) are put on the front
    wheels instead of at the middle of the car. There they had pulled the
    floor down and left the car floating.
* **Real Racing 3 wheel textures** named after a colour
  (`<car>_wheel_red.etc.dds`) are found, so these rims no longer show white.
* Command line: `render` puts a Real Racing 3 car's wheels on from its
  `_shared.m3g`.

## 0.9.5

* **Extract / Export from a folder works.** In 0.9.4 it reported "Wrote 0
  file(s), N could not be written". The export marks the tool busy while it
  runs, and the asset reader refused every read while the tool was busy. The
  export now reads the files itself. If any file still fails, the first 20
  are named in `MonkeyTool.log` next to the exe.
* **Shadow in the viewer.** The model casts a soft shadow onto the floor
  from a light almost straight overhead, plus a faint contact shade under
  it. This applies to cars and props only. Whole track maps are their own
  ground, so they get no shadow.
* **See-through floor.** The floor is now a light tint and the grid, drawn
  over the model without hiding it, so the car shows through from below.
* **No Limits wheels sit further out.** Wheels are still hung on the brake
  discs. On some cars (the Bugatti EB110, for one) the discs sit deep inside
  the car and the wheels ended up inside their arches. Now each tyre's
  outer face is checked against the stock body over that wheel. When it is
  more than 3 cm inside, the wheel is moved out to about 1.5 cm inside. The
  info panel says when this happened.
* Command line: `render` takes `--wheel wheel_x.sb3d` to draw a No Limits
  car with its wheels.

## 0.9.4

* **No Limits track maps load.** The maps are the
  `prefabs/tracks/*.scene_static.sba` files. Each one is a scene of placed
  mesh instances. They are now listed as models, open in the viewer, and
  export to FBX/OBJ with every instance in its world position. For example,
  region 03 has 439 parts and 158,000 triangles, and region 04 has 1,158 parts
  and 469,000 triangles. The scene files do not name their textures, so the
  tool matches each part's material name against the `.sba` textures under
  `textures/`. This is a best guess, and some parts may stay untextured or
  pick a close relative.
* **Right-click a folder** in the tree to get **Extract all assets from
  this folder** (files as they are) or **Export all assets from this
  folder** (converted: textures to PNG, models to FBX, sounds to WAV).
  Right-clicking a file offers **Save as...**. The folder structure is kept.
* **Exporting shows the progress bar**, the same one used while loading
  assets, with a running count. The tool stays responsive meanwhile, and a
  summary appears at the end.
* **Faster viewer.** It now draws with OpenGL on the graphics card, so
  turning a whole track is smooth rather than a slideshow. If OpenGL is not
  available, the old software view is still used.
* **Frosty-style viewer.** It has a blue sky backdrop and a light floor with
  a grid under the model, sized to fit the model.
* **Model statistics.** The top-left corner shows polygons, vertices and
  parts for what is shown.
* **Textures on a checkerboard.** The texture preview draws on a grey
  checkerboard, so transparent areas are visible. It is scaled to fit the
  pane and shows the size.

## 0.9.3

* **Track textures load in the viewer.** The viewer loaded at most 64
  textures per model, which covers a car. A Real Racing 3 track uses
  hundreds: with the Tracks folder you sent, Laguna Seca uses 2,610 textured
  parts and Monaco 2,197. That cap is gone. When a model uses more than 64
  textures, each is shrunk to at most 256 pixels for the viewer only, so a
  whole circuit fits in memory. The files themselves and the exports are
  untouched. Real Racing 2 tracks had the same cap and are fixed the same
  way. The status bar counts the textures as they load.
* **Big tracks open quickly.** Looking up a track's textures by name walked
  the whole library once per name: 500 names × 40,000 files, long enough to
  look like a hang (Laguna Seca "did not load"). The names are now indexed
  once.
* **Real Racing 2 `_hd` textures:** a model that asks for `x_hd.pvr` gets
  `x.pvr` when only that one is in the build, and the other way round.
* **No Limits:** files in the `_unnamed` folder (SBIN files the packs give
  no name) that contain geometry, as the cars' `.sb3d` do, are now listed as
  `.sb3d` models and open in the viewer, instead of as plain `.sbin` data.

## 0.9.2

* **Choosing the wrong game no longer loads nothing.** When the folder you
  pick belongs to a different game (say `com.ea.game.realracing2_OTD_row`
  while Real Racing 3 is selected), the tool notices and offers to switch. It
  goes by the Android package name in the path, or by what the folder holds.
  If the selected game expects archives and the folder has none, its files
  are read as loose files instead of showing an empty tree.
* **FBX export can bring the textures along.** When a model uses textures
  the tool can find, the export asks whether to save them too. **Yes**
  writes every texture as a PNG next to the FBX/OBJ, and the model refers to
  them by file name, so Blender or 3ds Max find them with no path fixing.
  **No** saves the model only.
* **A real sound player.** The buttons are |< (previous sound), << (5 s
  back), Play/Pause, Stop, >> (5 s on), >| (next sound), plus a position bar
  you can drag and the time played / total length. Previous and next move to
  the sound above or below in the tree and play it straight away.
* **vgmstream is included.** The Windows build of vgmstream and its DLLs are
  in `tools\vgmstream`. BUILD.bat copies them to `dist\vgmstream`, and the
  tool finds them there, so you no longer need to point at them.
* **MP3, Ogg, FLAC, AAC/M4A, Opus, WMA, FSB, ADX and plain WAV** files play
  in the tool and save as WAV. PCM WAV is played directly; the rest go
  through vgmstream.
* **Sort the file list by Name, Ext, Size or Date**, as in Total Commander:
  the strip above the tree has a button for each column. Click once to sort,
  click again to reverse the order. Folders stay on top.
* **Real Racing 3 textures on every model.** The V fix from 0.9.1 applied
  only to cars. It now covers every Real Racing 3 file (cars, interiors,
  tracks, skies): these are the ones that carry Real Racing 3's material
  list. Checked on the M1's livery and on the facade of the Monaco casino.
* **Real Racing 3 tracks are textured.** A track's materials still name the
  artist's source files (`C:\...\Tracks\monaco\resources\arc\
  arc_monaco_common\arc_common_monaco_wall_06.psd`). The game ships each of
  them as `<same name>.etc.dds`, under the track's `resources` folder or
  `Tracks\common`, and the tool finds them by that name. When no source
  file is named, the texture is looked up from the material name,
  `<shader>-<texture>`.
* **Real Racing 2 tracks.** Their textures are external references inside
  the `.m3g` (`alkeisha_shops_hd.pvr` and so on). These are now read, so a
  track such as `rr_track02_high.m3g` is textured from the game's `.pvr`
  files.
* **No Limits scenes:** the `.m3g` files in `models/environments/scenes` are
  the backdrops seen around the car in menus and at a race's start. They are
  small and they load completely: every part in them is shown. The roads of
  the open world are not in any `.m3g` in the packs checked so far (see
  below).

## 0.9.1

* **Need for Speed: Hot Pursuit (2010, mobile) is supported.** Pick it in the
  game list and point it at `com.eamobile.nfshp_row_wf` (or `_row`).
  * Models (`car_*.m3g`) use a fourth spelling of the M3G identifier,
    `«IM-M3G»`, with Most Wanted's object layout plus two things from plain
    JSR-184: vertices stored as deltas from the vertex before, and triangle
    strip arrays. Both are read. `MESH_metal_lod` is filed as LOD01 and
    `MESH_cop` (the police parts) as a **COP** group. The files carry the
    right-hand wheels only, and the game mirrors them, so the tool adds the
    left-hand pair.
  * Textures are `.m3g` files too, each holding one picture. They show in
    the tree, save as PNG, and appear on the models in the viewer.
  * The `.m3g.sb` prefabs (SBIN version 2) show their structure with names,
    including the wheel, light and nitro locators.
* **Real Racing 3 textures are the right way up on the model.** The V
  direction was flipped for Real Racing 3. On the BMW M1 the Procar stripes
  and the roundel only land on the body with V turned over. Real Racing 2's
  models were already right and are unchanged.
* **Engine sounds (`.gnsu`) play and save as WAV.** They are EA's granular
  engine banks (`Gnsu20`), and the audio is EA-XAS: 19-byte frames of 32
  samples. The tool now decodes that itself, with no vgmstream needed. Press
  **Play** on an engine sound, inside a bank or as a loose `.gnsu` file, to
  hear all of its grains in order, from idle to redline. Saving a bank
  writes each engine as `.wav` and as the original `.gnsu`. The text view
  shows the rev range, the number of grains and the length.
* **No Limits sound names.** A loose `android/audio/<number>.wem` has no
  other name in the game: Wwise keeps the text names in the authoring
  project, and the game ships no `SoundbanksInfo.xml`. What the banks do
  say is which bank plays each file. The tool now reads that from every
  bank's object list and files each loose sound under its bank: `Music/`,
  `Ambience/`, `Police/`, and so on. In the packs checked, 48 of 78 loose
  sounds were matched. The rest are played by banks that were not in those
  packs.
* **No Limits `.m3g` files with no geometry now export.** Overlays,
  searchlights and camera paths are only groups and animation. They now
  save as FBX/OBJ with their groups as empties, named and placed, instead
  of failing.
* **Most Wanted cop cars show.** They carry only LOD03-LOD05. Their light
  bars have no level in their names, so they were filed as LOD00, and the
  viewer opened on LOD00 with nothing else in it. Parts without a level now
  join the most detailed level the file has, and the camera frames that
  level.

## 0.8.1

* **Real Racing 3 cars render properly.** A car file carries every part in
  more than one state: whole and damaged (`HOOD` / `HOOD_DAMAGE`), headlight
  lenses whole and broken, a scratch shell over the paint, cracked and
  shattered glass, and a bonnet-camera version of the front. Up to 0.8.0 they
  were all drawn at once, inside each other, which broke the paint up into
  black and grey patches. They now have groups of their own in the LOD list,
  after the detail levels: **DAMAGE** and **BONNETCAM**. They are still in the
  FBX, hidden, under those names.
* **Shared textures from `vehicles/common`.** Glass (`car_windows_02`), cracked
  and shattered glass, tyre treads (`car_tyre_tread_<year>`) and the interior
  materials (`..._mat04`) come from the game's common folder, and the tool now
  finds them there. When the car's `<car>.liveries.bin` is in the library, its
  texture list is used first: it names the exact shared files the car draws
  on, and its default paint scheme (`livery/<car>_ext_NN`).
* **Steering wheel:** the cockpit's steering point is also taken when the
  `_int.points` file spells it differently. The info line under the viewer now
  says where the point came from, or which points the cockpit file has, so a
  car that still misses it can be fixed from a screenshot.
* The info line also lists the materials that found no texture.
* **Brighter viewer.** A fill light now comes from the camera, so a car seen
  from behind or below is no longer nearly black.

## 0.8.0

* **Need for Speed: Most Wanted (2012, mobile) `.m3g` models open and export.**
  Cars and scenes are read with their full scene graph: every group's
  position, rotation and scale is applied, and the skinned parts (a car's
  wheels and suspension) sit where the game puts them. Each part keeps its
  name, material and texture path, and gets its LOD from the name
  (`mesh_opaque_lod_00` is LOD00). Collision hulls go to HELPERS. The ground
  light cards (`Light_Falloffs`) are kept in the export but hidden in the
  viewer, like the other glow cards. Locators, pivots and joints become
  empties.
* **No Limits `.m3g` models export to FBX.** The garage, the mod shop, skydomes
  and the other scenes in `models/scenes/` and `models/environments/` now
  convert to FBX/OBJ with their textures and materials, and show in the
  viewer. Until now no `.m3g` from No Limits ever unpacked: the LZHAM reader
  treated LZHAM's "success" code (3) as an error and read the checksum in the
  wrong byte order. Both are fixed; every sample decodes. Save one as **raw**
  to get the unpacked M3G itself. You still need `lzham_x64.dll` next to the
  program (`BUILD_LZHAM.bat` builds it).
  Files that hold only transforms (`transform_roadblock_...`,
  `transform_helicopter_...`) have no geometry and say so.
* **Real Racing 3: the steering wheel is in place.** The car's own `.points`
  often has no steering point, and the wheel stayed at the origin, under the
  car. The tool now takes `POINT_STEERING_WHEEL` from the cockpit's
  `<car>_int.points` beside it (same coordinate space). With no cockpit file,
  the driver's point is used, which on the 935 is within 5 cm.
* **Real Racing 3: textures and materials in the viewer and the export.** RR3
  models name no texture files; a material is called
  `Vehicle Exterior_mm_misc`, and the texture is the car's
  `<car>_misc.etc.dds`. The tool now pairs them the same way:

  | material `_mm_` name | texture file |
  |---|---|
  | `ext` (paint) | `<car>_ext_01`, then any `<car>_ext...` (not the shadow maps) |
  | `misc`, `badges`, `lights`, `cab`, `chassis`, `banner`, `wheel`, `wheel_blur` | `<car>_<same name>` |
  | `tyre`, `rotor` | `<car>_wheel` |
  | `sw` (interior steering wheel) | `<car>_sw`, then `<car>_int` |
  | `int` and anything else inside | `<car>_<name>`, then `<car>_int` |
  | `windows` | `<car>_windows` if it exists, otherwise dark tinted glass |

  The `.etc.dds` build is preferred, and `.pvr` / `.dxt.dds` are used when a
  different build ships those. The FBX lists every material with its texture
  and render state (glass and scratches are alpha, the headlight glow is
  additive). From the command line, `monkeytool_cli one <car>_a.m3g out.fbx`
  picks the textures up from the model's folder, and `render ... --tex <folder>`
  shows them.
* The text view of a `.m3g` lists the objects of all three layouts (below),
  including No Limits' after unpacking, with group and part names.

### The three `.m3g` layouts, side by side

All three games use Firemonkeys' version of the JSR-184 (Mobile 3D Graphics)
container: the same 12-byte identifier with a different name in it, and the
same kinds of objects. What differs:

| | Real Racing 3 | NFS Most Wanted 2012 | NFS No Limits |
|---|---|---|---|
| identifier | `«JSR184»` | `«IM2M3G»` | `«IM4M3G»` |
| on disk | plain (sometimes gzip) | plain | `DA BD` wrapper + LZHAM |
| framing | sections, each with a size and checksum | one run of objects from byte 21 to a 4-byte checksum | as Most Wanted |
| object header | type, size | type, size | type, size, and **one more 32-bit word** after the first field, which moves everything after it by 4 |
| meshes | Firemint's own mesh record, a material-name list (type 24), triangle strips (type 11) | mesh → submeshes (type 100), each with an index buffer (type 101) and an appearance | as Most Wanted |
| placement | vertices already in car space; moving parts at the origin, placed from `.points` | a scene graph: groups with translation, rotation (angle + axis), scale and an optional 4x4 | as Most Wanted |
| cars | exterior, `_int` cockpit and `_shared` wheels as separate files | one file; the wheels are skinned (cloned group, bone indices and weights) and stored in their rest pose | cars are `.sb3d`, not `.m3g` |
| textures | named after the material, not stored in the file | Image2D objects give the path (`...texture_x.m3g`, shipped as `.sba`) | as Most Wanted |
| vertex data | positions, normals, colours, UVs; 8/16-bit or float, with a scale and bias | the same, plus a vertex-buffer texture count of `-1` meaning one set | as Most Wanted |

So a reader for Most Wanted reads No Limits once it skips the extra word,
which is what the tool does. It checks this per file (the first vertex array
has to add up to its size) instead of trusting the name.

## 0.7.9

* **Import PNG / JPG into a texture.** Select a texture and use
  **Tools > Import PNG/JPG** (Ctrl+I). Your picture is resized to the
  texture's size and encoded in the codec the texture already uses (ETC1, DXT1/3/5,
  ATC, RGB, RGBA, RGB565, RGBA4444, PNG, JPEG), for every mip level, in the
  same container: No Limits and Most Wanted `.sba`, Real Racing 2 `.pvr`, Real
  Racing 3 `.dds` / `.dds.z`, and plain `.png` / `.jpg`. The game reads it
  exactly the way it read the original. A loose file (Real Racing, Most Wanted)
  can be saved over the original, which is kept as `.bak`. A texture inside a
  No Limits `.pack` is saved as a file of its own (the tool does not rewrite
  `.pack` archives). The 3D view shows your picture on the model for the rest
  of the session. PVRTC cannot be written: a `.pvr` / `.dds` holding it is
  switched to uncompressed RGBA, and an `.sba` holding it is reported.
  The same from the command line: `monkeytool_cli import <texture> <picture> <out>`.
* **PNG and JPG files open in the tool:** File > Open a picture, and loose
  `.png` / `.jpg` files in the tree are shown and convert like textures. The
  tool now reads every kind of PNG (all bit depths, palettes, interlaced) and
  baseline JPEG itself. On Windows it also uses the system's image codecs,
  which read progressive JPEG, BMP, GIF and TIFF.
* **No Limits' `.sb` data files are readable.** Nearly all of them (27,405 of
  29,665) are encrypted with the save files' key, around a gzip wrapper. They
  are now decrypted and shown as their objects: race events, body kits, car
  setups, series and tuning. Save as `.sbin` for the decrypted file;
  `monkeytool_cli sb encode` puts an edited one back.
* **Need for Speed: Most Wanted (2012, mobile) is supported**, built on the
  knowledge in Hypercycle's NFSMW12MobileTools
  (github.com/hypernucle/NFSMW12MobileTools):
  * SBIN version 3 `.sb` files are shown object by object, using the field
    types, maps and enums that tool worked out.
  * `.sba` textures are read from their own `Image` descriptions (width,
    height, format, data).
  * IM2 `.m3g` models take their part names and texture names from their
    submeshes and Image2D objects.
  * The tool reads the Android `.obb` (a zip) directly, and any `.zip` /
    `.apk` in a loose game folder too.
* **Every SBIN chunk's hash is checked.** The hash is the FNV-1 of the
  chunk's data, and a damaged or hand-edited file shows up in the text view.
  Anything the tool writes gets correct hashes.
* **M3G files list their objects** in the text view: type, size and the names
  they carry, as NFSMW12MobileTools' `map` command does.
* Exported PNG files are much smaller: they are now properly compressed.

## 0.7.8

* **Real Racing 2 is supported.** Pick it in the game list and point it at
  `com.ea.game.realracing2_OTD_row`. Its textures are ATC, the compression the
  Android build uses, and are now decoded (along with the legacy PVR formats:
  PVRTC, DXT, ETC, RGBA 4444/5551/8888, RGB 565). Its `.points` files store
  positions as fixed-point numbers, which are now read, so the steering
  wheel, hands and needles land in place. A model is textured by file name:
  `_int.pvr` for the interior, `_sw.pvr` for the steering wheel, `_ext_01.pvr`
  for the paint.
* **LOD00 is the default again.** 0.7.7 put the new HELPERS group first in
  the LOD list, so a car opened on its collision hulls. HELPERS is now last.
* **VW Beetle 1963:** the front wings and bonnet came out black. The model
  file puts that panel on the plastic material, but it carries the paint's
  livery UVs and is body colour in the game, so it now gets the paint.

## 0.7.7

* **Body kits (No Limits).** A car file holds every version of its parts: the
  stock bumpers, the kit bumpers (Y, Z ...) and the tuning parts (wings,
  splitters, overfenders). Up to 0.7.6 all of them were drawn at once, so a
  car looked like a different model (the 190E Evo II with the Evo parts on
  top). The viewer now opens on **Stock**. A new box next to the LOD box
  switches to **All parts** or one kit (**Kit Y**, **Kit Z** ...). FBX exports
  keep every part, but anything that is not stock is imported hidden. The CLI
  `render` command takes `--kit stock|all|<letter>`.
* **Real materials from the model file.** Every part now takes its material
  from the table inside the .sb3d instead of guessing from the mesh name.
  Paint is paint, chrome is chrome, and bronze chrome or gloss black parts
  (which have no texture) are shown in their own colour, which also goes into
  the FBX. This fixes the black Beetle front and the wrong textures on some
  parts.
* **Carbon parts** that the model leaves on the plastic material (the Evo X
  splitter, diffuser and side skirts) now get the carbon texture, the way the
  game does.
* **Missing parts.** A mesh with more than one material lost everything after
  its first material. It now splits into one part per material.
* **Mirrored view fixed.** The viewer drew every model as a mirror image,
  which put number plates and badges back to front. Exports were never
  mirrored.
* **Collision hulls and the decal shell** (a grey copy of the body the game
  uses to place liveries) are put in a HELPERS group and imported hidden. In
  Blender they used to sit over the paint and make the car look grey and dirty.

## 0.7.6

* **Every preview says which file the asset comes from**, for example
  `In 5359482db05ccebb7600a8c539d5bc58.pack (cabinet 1)`. When the cabinet is
  stored separately, it names the `.cab` too. A car's preview also names the
  wheel model it used and that wheel's pack. So when something is wrong, you
  know which file to send.
* **Tools → Show the file this asset is in** (Ctrl+L) opens Explorer with
  that `.pack` (or `.cab`) selected.
* The command line has the same: `monkeytool_cli where <gameFolder> <name>`
  lists every matching asset and its pack.
* The text under the preview is taller, so its last lines are no longer cut
  off.

## 0.7.5

* **No more seams between parts (No Limits).** Every mesh stores its own
  scale and offset for its packed vertex positions. Up to 0.7.4 the tool used
  the mesh's bounding box instead, which is about 0.03% off: enough to open
  0.2 mm gaps where two parts meet, and to put a symmetric bumper 0.3 mm off
  the centre line (a modder spotted it in Blender). Measured on the Audi TT RS
  and the Cayenne, the median gap between parts that meet dropped from
  0.22 mm to 0.02 mm, which is one quantisation step. Symmetric panels now
  centre on x = 0 to within 0.004 mm.
* **Wheels on every corner.** The brake discs are now found by where they
  are, not by what they are called. The names cannot be trusted: the Nissan Z
  calls all four discs `rotor_front_left`, and on the Lotus Evora GT430 the
  names matched one corner and put that wheel in the middle of the door.
* **Save Editor in the dark theme**: the white panels are gone, and the
  buttons, title bar and scroll bar follow the theme. The main window's title
  bar follows it too.
* **A bigger Save Editor button** beside the search box.

## 0.7.4

* **No Limits cars have their wheels.** A car's model has no wheels in it:
  the wheel is its own model, `models/cars/wheels/wheel_<car>.sb3d`, which
  the game puts on each brake disc. The viewer and the FBX/OBJ export now do
  the same (see *Wheels on No Limits cars* below).
* **The Save Editor is part of the tool.** The floppy-disk button opens it
  inside Monkey Tool: no second program, no "unknown publisher" warning. It is
  the same editor, ported to C++. Tested against the original on made-up No
  Limits and Real Racing 3 saves, it writes byte-identical files.
* **Folders with non-English letters** (`C:\Users\Żaneta\...`) open now. Paths
  were handed to Windows in the wrong code page before.

## 0.7.3

* **Every asset in a `.pack` is listed now**, including the ones whose
  cabinets sit in a separate `.cab` file. The Toyota AE86, the Honda S2000,
  the Ford Falcon and some maps showed only their scripts before; see
  *Packs with external cabinets* below.
* **Assets that are not on disk are not listed.** A cabinet that was never
  downloaded used to give names in the tree that failed when clicked.
* **Vertex colours** are read and exported: FBX `LayerElementColor`, OBJ
  `v x y z r g b`. On No Limits cars this is baked ambient occlusion. The
  viewer has a **Vertex colours** switch to show it.
* **A second UV set** on No Limits paint meshes (the livery layout) goes into
  the FBX as `LiveryUV`.
* **Real Racing 3 wheels are built from `<car>_shared.m3g`**: tyre, rim, brake
  disc and the right caliper for each corner, placed on the outer face of the
  wheel with nothing centred. The right side is the left wheel turned round,
  not mirrored, so rim lettering still reads the right way.
* **Steering wheel and arms**: the driver's arms are placed with the steering
  wheel, and a part that is still sitting at the origin is named in the
  preview.
* **PVR** (`.pvr`, and Real Racing 3's `.ptc.pvr.z` livery textures) previews
  and saves as PNG.
* **`.gui`** menus decode to XML. **`.nct` liveries** show their texture table
  and livery entries, and **Tools → Encode an edited file back** turns an
  edited `.bin` / `.xml` into a `.nct` / `.gui` again.
* The `.nct` pad learner is rebuilt (car names, record layout, struct padding
  and XML), and the pad it ships with was re-learned: 1657 certain bytes, up
  from 1505.
* **Save Editor**: the floppy-disk button beside the search box (or **Tools →
  Save Editor**, Ctrl+E) opens it. (In 0.7.3 this started a separate
  program; from 0.7.4 it is built in.)
* The search box now sits exactly above the file list: same left edge, same
  width.
