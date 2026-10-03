# =====================================================================
#  Monkey Tool 1.2.2 - automatic Windows build
#
#  You do NOT need Visual Studio, CMake or anything else installed.
#  If no compiler is found this downloads w64devkit (a portable MinGW
#  that needs no installation) into this folder and builds with it.
#
#  Run it by double-clicking BUILD.bat
#  (LZHAM is compiled in since 1.2; -LzhamOnly still builds the old DLL.)
# =====================================================================

param([switch]$LzhamOnly)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

function Say($msg, $color = "White") { Write-Host $msg -ForegroundColor $color }

# ------------------------------------------------- stop Windows freezing us
# Clicking inside a console window puts it into selection mode, and in that
# mode Windows SUSPENDS whatever is running the moment it writes anything.
# The build then sits there looking hung until you press Esc. Turning
# QuickEdit off for this one window makes that impossible. It changes nothing
# outside this window and nothing permanently.
try {
    if (-not ("MT.Con" -as [type])) {
        Add-Type -Namespace MT -Name Con -MemberDefinition @"
[DllImport("kernel32.dll", SetLastError=true)]
public static extern IntPtr GetStdHandle(int n);
[DllImport("kernel32.dll", SetLastError=true)]
public static extern bool GetConsoleMode(IntPtr h, out uint m);
[DllImport("kernel32.dll", SetLastError=true)]
public static extern bool SetConsoleMode(IntPtr h, uint m);
"@
    }
    $hIn = [MT.Con]::GetStdHandle(-10)      # STD_INPUT_HANDLE
    $mode = 0
    if ([MT.Con]::GetConsoleMode($hIn, [ref]$mode)) {
        # clear ENABLE_QUICK_EDIT_MODE (0x40), set ENABLE_EXTENDED_FLAGS (0x80)
        [void][MT.Con]::SetConsoleMode($hIn, ($mode -band (-bnot 0x40)) -bor 0x80)
    }
} catch {}

Say ""
Say "  Monkey Tool 1.2.2 - build" "Cyan"
Say "  ========================"  "Cyan"
Say ""

# TLS 1.2 for older PowerShell versions
try { [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 } catch {}

# A download that cannot hang forever: WebClient waits for as long as the
# other end feels like, which is how a build appears to freeze.
function Get-File($url, $dest, $timeoutSec = 180) {
    $req = [System.Net.HttpWebRequest]::Create($url)
    $req.Timeout = $timeoutSec * 1000
    $req.ReadWriteTimeout = $timeoutSec * 1000
    $req.UserAgent = "MonkeyTool-build"
    $resp = $req.GetResponse()
    $in = $resp.GetResponseStream()
    $out = [System.IO.File]::Create($dest)
    try {
        $buf = New-Object byte[] 262144
        while (($n = $in.Read($buf, 0, $buf.Length)) -gt 0) { $out.Write($buf, 0, $n) }
    } finally {
        $out.Close(); $in.Close(); $resp.Close()
    }
}

# ---------------------------------------------------------------- compiler
$gpp = $null

$onPath = Get-Command g++ -ErrorAction SilentlyContinue
if ($onPath) {
    $gpp = $onPath.Source
    Say "Found a compiler already on PATH:" "Green"
    Say "  $gpp"
}

if (-not $gpp) {
    $local = Join-Path $root "w64devkit\bin\g++.exe"
    if (Test-Path $local) {
        $gpp = $local
        Say "Using the portable compiler from a previous run." "Green"
    }
}

if (-not $gpp) {
    Say "No C++ compiler found - downloading a portable one (about 90 MB)." "Yellow"
    Say "This happens only once; it is unpacked into this folder and nothing"
    Say "is installed on your system."
    Say ""

    $url = "https://github.com/skeeto/w64devkit/releases/download/v2.0.0/w64devkit-2.0.0.zip"
    $zip = Join-Path $root "w64devkit.zip"

    try {
        Say "Downloading..." "Yellow"
        $wc = New-Object System.Net.WebClient
        $wc.DownloadFile($url, $zip)
    } catch {
        Say ""
        Say "The download failed: $($_.Exception.Message)" "Red"
        Say ""
        Say "Download it manually instead:" "Yellow"
        Say "  $url"
        Say "and unzip it here so that this path exists:" "Yellow"
        Say "  $root\w64devkit\bin\g++.exe"
        Say "then run this script again."
        Say ""
        Read-Host "Press Enter to close"
        exit 1
    }

    Say "Unpacking..." "Yellow"
    try {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [System.IO.Compression.ZipFile]::ExtractToDirectory($zip, $root)
    } catch {
        Expand-Archive -Path $zip -DestinationPath $root -Force
    }
    Remove-Item $zip -ErrorAction SilentlyContinue

    $gpp = Join-Path $root "w64devkit\bin\g++.exe"
    if (-not (Test-Path $gpp)) {
        Say "Unpacked, but g++.exe is not where expected." "Red"
        Read-Host "Press Enter to close"
        exit 1
    }
    Say "Compiler ready." "Green"
}

$binDir  = Split-Path -Parent $gpp
$windres = Join-Path $binDir "windres.exe"

# ---------------------------------------------------------------- LZHAM
# The game compresses its .m3g scene models with LZHAM. Nobody ships a Windows
# DLL of it, so the source is fetched and built with the compiler this script
# already has. It is a 2013 codebase written for Visual C++, so two source
# trees and two sets of defines are tried in turn and the first that compiles
# wins.
#
# This is the slow part of the build - a couple of dozen files, a minute or
# three - so it runs last, after the tool itself is finished and usable, and
# it prints every file as it goes. Nothing here can stop you getting an exe.
function Build-Lzham($gpp, $root) {
    $lzhamDll = Join-Path $root "dist\lzham_x64.dll"
    if (Test-Path $lzhamDll) {
        Say "lzham_x64.dll is already built." "Green"
        return
    }
    # A compiler message on stderr must not be treated as a fatal script
    # error here - a file that will not build is exactly what the fallbacks
    # below are for.
    $savedEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
    New-Item -ItemType Directory -Force -Path (Join-Path $root "dist") | Out-Null

    $repos = @(
        @{ name = "lzham_codec";       url = "https://github.com/richgel999/lzham_codec/archive/refs/heads/master.zip" },
        @{ name = "lzham_codec_devel"; url = "https://github.com/richgel999/lzham_codec_devel/archive/refs/heads/master.zip" }
    )
    # plain first (-DNDEBUG is on anyway, so it stands for "nothing extra");
    # the ANSI build drops the MSVC intrinsics and the task pool, which is all
    # the decoder needs
    $defineSets = @("-DNDEBUG", "-DLZHAM_ANSI_CPLUSPLUS")

    foreach ($repo in $repos) {
        if (Test-Path $lzhamDll) { break }
        $ltmp = Join-Path $root "lzham_tmp"
        $lzip = Join-Path $root "lzham.zip"
        try {
            if (Test-Path $ltmp) { Remove-Item $ltmp -Recurse -Force }
            Say "  downloading $($repo.name) ..." "DarkGray"
            Get-File $repo.url $lzip
            Say "  unpacking ..." "DarkGray"
            try {
                Add-Type -AssemblyName System.IO.Compression.FileSystem
                [System.IO.Compression.ZipFile]::ExtractToDirectory($lzip, $ltmp)
            } catch {
                Expand-Archive -Path $lzip -DestinationPath $ltmp -Force
            }
            $lroot = Get-ChildItem -Path $ltmp -Directory | Select-Object -First 1
            if ($lroot) {
                # Work from inside the source tree, so every path handed to the
                # compiler is a short relative one. An absolute path through a
                # folder with a space in it - "D:\No Limits\..." - is exactly
                # what broke this before.
                Push-Location $lroot.FullName
                try {

                $lsrc = @()
                foreach ($d in @("lzhamdecomp", "lzhamcomp", "lzhamlib")) {
                    if (Test-Path $d) {
                        $lsrc += (Get-ChildItem -Path $d -Filter *.cpp |
                                  Where-Object { $_.Name -notlike "*pthreads*" } |
                                  ForEach-Object { Join-Path $d $_.Name })
                    }
                }

                # Do not guess where the headers live - find them. lzham.h sits
                # in include\ in one layout and at the top in another, and a
                # wrong guess is a fatal "lzham.h: No such file or directory"
                # on the very first file.
                $incDirs = @(".")
                Get-ChildItem -Recurse -Filter *.h | ForEach-Object {
                    try { $incDirs += (Resolve-Path -Relative $_.DirectoryName) } catch {}
                }
                $incDirs = $incDirs | Sort-Object -Unique
                $inc = @($incDirs | ForEach-Object { "-I$_" })
                Say ("  headers in: " + ($incDirs -join ", ")) "DarkGray"

                # everything below is relative to the source tree as well
                $objRel = "obj"
                $dllRel = "lzham_x64.dll"

                foreach ($defs in $defineSets) {
                    if (Test-Path $lzhamDll) { break }
                    if ($lsrc.Count -eq 0) { break }
                    if (Test-Path $objRel) { Remove-Item $objRel -Recurse -Force }
                    New-Item -ItemType Directory -Force -Path $objRel | Out-Null

                    # -O1: this decoder runs on a few hundred kilobytes at a
                    # time, so compiling fast matters more than running fast
                    $ok = $true
                    $objs = @()
                    $n = 0
                    $started = Get-Date
                    foreach ($f in $lsrc) {
                        $n++
                        $obj = Join-Path $objRel ([System.IO.Path]::GetFileNameWithoutExtension($f) + ".o")
                        Say ("  [{0,2}/{1}] {2}" -f $n, $lsrc.Count, [System.IO.Path]::GetFileName($f)) "DarkGray"
                        # Output is kept quiet on purpose - a failure is
                        # reported once, in full, in the log rather than as a
                        # wall of red.
                        #
                        # Two fixes for building LZHAM with MinGW rather than
                        # the MSVC / Linux toolchains it was written for. Both
                        # were reproduced and checked against g++ before being
                        # put here; without them the build stops on the first
                        # or second file, whichever repo is being tried.
                        #
                        #   -include cstdint
                        #     lzham_huffman_codes.cpp uses UINT16_MAX without
                        #     including <cstdint>, so the limit macros are not
                        #     in scope. Prepending the header fixes it for
                        #     every file at once.
                        #
                        #   -Dmalloc_usable_size=_msize
                        #     lzham_mem.cpp does "#define _msize
                        #     malloc_usable_size", which is right for glibc and
                        #     wrong for MinGW - MinGW has _msize and no
                        #     malloc_usable_size. Pointing the name back at
                        #     _msize makes the alias resolve to the real
                        #     function: _msize -> malloc_usable_size -> _msize,
                        #     which the preprocessor stops expanding there, so
                        #     the call lands on MinGW's own _msize.
                        $out = & $gpp (@("-std=c++11", "-O1", "-DNDEBUG", "-w", "-fpermissive",
                                         "-include", "cstdint", "-include", "malloc.h",
                                         "-Dmalloc_usable_size=_msize", "-c") +
                                       $inc + @($defs, $f, "-o", $obj)) 2>&1
                        if ($LASTEXITCODE -ne 0) {
                            $ok = $false
                            try {
                                $log = Join-Path $root "dist\lzham_build.log"
                                "=== $($repo.name) $defs : $f ===" | Out-File $log -Append -Encoding utf8
                                $out | Out-File $log -Append -Encoding utf8
                            } catch {}
                            ($out | Select-Object -First 4) | ForEach-Object { Say "    $_" "DarkGray" }
                            break
                        }
                        $objs += $obj
                    }
                    if ($ok -and $objs.Count -gt 0) {
                        Say "  linking lzham_x64.dll ..." "DarkGray"
                        Remove-Item $dllRel -Force -ErrorAction SilentlyContinue
                        $out = & $gpp (@("-shared") + $objs +
                                       @("-o", $dllRel, "-Wl,--export-all-symbols",
                                         "-static-libgcc", "-static-libstdc++")) 2>&1
                        if ($LASTEXITCODE -eq 0 -and (Test-Path $dllRel)) {
                            Copy-Item $dllRel $lzhamDll -Force
                        } else {
                            try {
                                $log = Join-Path $root "dist\lzham_build.log"
                                "=== $($repo.name) $defs : link ===" | Out-File $log -Append -Encoding utf8
                                $out | Out-File $log -Append -Encoding utf8
                            } catch {}
                            ($out | Select-Object -First 4) | ForEach-Object { Say "    $_" "DarkGray" }
                        }
                    }
                    if (Test-Path $lzhamDll) {
                        $secs = [int]((Get-Date) - $started).TotalSeconds
                        Say "  built in $secs seconds." "DarkGray"
                    } else {
                        Say "  that combination did not compile - trying the next one." "DarkGray"
                    }
                }

                } finally { Pop-Location }
            }
        } catch {
            Say "  $($repo.name): $($_.Exception.Message)" "DarkGray"
        }
        Remove-Item $lzip -Force -ErrorAction SilentlyContinue
        Remove-Item $ltmp -Recurse -Force -ErrorAction SilentlyContinue

    }

    Say ""
    if (Test-Path $lzhamDll) {
        Say "lzham_x64.dll ready - .m3g scene models will convert." "Green"
    } else {
        Say "LZHAM did not build here - .m3g models stay compressed." "Yellow"
        Say "Everything else works. The Actions build on GitHub makes this DLL" "Yellow"
        Say "too, if you would rather download it - see the README." "Yellow"
        if (Test-Path (Join-Path $root "dist\lzham_build.log")) {
            Say ""
            Say "The whole compiler output is in dist\lzham_build.log - send that" "Yellow"
            Say "file and the reason can be found without guessing." "Yellow"
        }
    }
    } finally {
        $ErrorActionPreference = $savedEap
    }
}

if ($LzhamOnly) {
    Build-Lzham $gpp $root
    Say ""
    Read-Host "Press Enter to close"
    exit 0
}

# ---------------------------------------------------------------- compile
New-Item -ItemType Directory -Force -Path (Join-Path $root "dist") | Out-Null

$core = @(
    "src\nfsnl_core.cpp"
    "src\nfsnl_image.cpp"
    "src\nfsnl_model.cpp"
    "src\nfsnl_m3g.cpp"
    "src\nfsnl_render.cpp"
    "src\nfsnl_audio.cpp"
    "src\nfsnl_text.cpp"
    "src\nfsnl_library.cpp"
    "src\nfsnl_profiles.cpp"
    "src\nfsnl_rr3.cpp"
    "src\nfsnl_import.cpp"
    "src\nfsnl_data.cpp"
    "src\nfsnl_save.cpp"
    "src\nfsnl_sbin.cpp"
    "src\nfsnl_decode.cpp"
    "src\nfsnl_encode.cpp"
    "src\nfsnl_im2.cpp"
    "src\nfsnl_scene.cpp"
    "src\nfsnl_astc.cpp"
    "src\nfsnl_brotli.cpp"
    "src\nfsnl_jsr.cpp"
    "src\nfsnl_lzham.cpp"
)

# the LZHAM decompressor (public domain), compiled in: its own flags, since
# it was written for older compilers
Say ""
Say "Compiling the LZHAM decoder..." "Yellow"
$lzObjDir = Join-Path $root "dist\lzham_obj"
New-Item -ItemType Directory -Force -Path $lzObjDir | Out-Null
foreach ($f in (Get-ChildItem -Path (Join-Path $root "src\lzham") -Filter *.cpp)) {
    $o = Join-Path $lzObjDir ($f.BaseName + ".o")
    & $gpp @("-std=c++11", "-O2", "-DNDEBUG", "-w", "-fpermissive", "-include", "cstdint",
             "-include", "malloc.h", "-c", $f.FullName, "-o", $o)
    if ($LASTEXITCODE -ne 0) {
        Say "LZHAM file $($f.Name) did not compile - see above." "Red"
        Read-Host "Press Enter to close"
        exit 1
    }
    $core += $o
}

# icon + version info + visual styles manifest
$resObj = ""
if (Test-Path $windres) {
    Say ""
    Say "Compiling resources (icon)..." "Yellow"
    $resObj = "dist\monkeytool_res.o"
    & $windres -I src -i src\monkeytool.rc -o $resObj
    if ($LASTEXITCODE -ne 0) {
        Say "Resource compile failed - continuing without the icon." "Yellow"
        $resObj = ""
    }
}

Say ""
Say "Building MonkeyTool.exe ..." "Yellow"
$guiArgs = @("-std=c++17", "-O2", "-Isrc", "-municode", "-mwindows") +
           $core + @("src\main_win32.cpp", "src\win32_save_editor.cpp")
if ($resObj) { $guiArgs += $resObj }
$guiArgs += @("-o", "dist\MonkeyTool.exe",
              "-lcomctl32", "-lcomdlg32", "-lshell32", "-lole32", "-luxtheme", "-lwinmm", "-lopengl32", "-lgdi32",
              "-static", "-static-libgcc", "-static-libstdc++")
& $gpp $guiArgs
if ($LASTEXITCODE -ne 0) {
    Say ""
    Say "The GUI build failed - the messages above say why." "Red"
    Read-Host "Press Enter to close"
    exit 1
}

Say "Building monkeytool_cli.exe ..." "Yellow"
$cliArgs = @("-std=c++17", "-O2", "-Isrc") + $core + @("src\main_cli.cpp",
            "-o", "dist\monkeytool_cli.exe", "-lole32",
            "-static", "-static-libgcc", "-static-libstdc++")
& $gpp $cliArgs
if ($LASTEXITCODE -ne 0) { Say "CLI build failed (the GUI still built)." "Yellow" }

# the exe falls back to this file if the icon resource could not be compiled
Copy-Item (Join-Path $root "src\monkeytool.ico") (Join-Path $root "dist\monkeytool.ico") -Force -ErrorAction SilentlyContinue

# vgmstream (the bundled Windows build, with its DLLs) goes beside the program:
# it decodes Wwise Vorbis, MP3, Ogg, AAC and the other formats the tool does
# not decode itself. MonkeyTool.exe finds it in dist\vgmstream on its own.
$vgmSrc = Join-Path $root "tools\vgmstream"
if (Test-Path (Join-Path $vgmSrc "vgmstream-cli.exe")) {
    $vgmDst = Join-Path $root "dist\vgmstream"
    New-Item -ItemType Directory -Force -Path $vgmDst | Out-Null
    Copy-Item (Join-Path $vgmSrc "*") $vgmDst -Force -ErrorAction SilentlyContinue
    Say "vgmstream copied to dist\vgmstream (sounds, music)" "Green"
}

# 0.7.3 shipped the Save Editor as a separate exe; it is built in now
Remove-Item (Join-Path $root "dist\FiremonkeysSaveEditor.exe") -Force -ErrorAction SilentlyContinue

# ---------------------------------------------------------------- libzstd
$dll = Join-Path $root "dist\libzstd.dll"
if (-not (Test-Path $dll)) {
    Say ""
    Say "Fetching libzstd.dll (needed for 3D models)..." "Yellow"
    try {
        $zurl = "https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-v1.5.6-win64.zip"
        $zzip = Join-Path $root "zstd.zip"
        Get-File $zurl $zzip
        $ztmp = Join-Path $root "zstd_tmp"
        if (Test-Path $ztmp) { Remove-Item $ztmp -Recurse -Force }
        try {
            Add-Type -AssemblyName System.IO.Compression.FileSystem
            [System.IO.Compression.ZipFile]::ExtractToDirectory($zzip, $ztmp)
        } catch {
            Expand-Archive -Path $zzip -DestinationPath $ztmp -Force
        }
        $found = Get-ChildItem -Recurse -Path $ztmp -Include "libzstd.dll","zstd.dll" |
                 Select-Object -First 1
        if ($found) {
            Copy-Item $found.FullName $dll -Force
            Say "libzstd.dll ready." "Green"
        } else {
            Say "Could not find the DLL inside the archive." "Yellow"
        }
        Remove-Item $zzip -Force -ErrorAction SilentlyContinue
        Remove-Item $ztmp -Recurse -Force -ErrorAction SilentlyContinue
    } catch {
        Say "Could not download libzstd.dll: $($_.Exception.Message)" "Yellow"
        Say "Models will be skipped until you place libzstd.dll in dist\." "Yellow"
    }
}

# ---------------------------------------------------------------- done
Say ""
Say "  ===========================================================" "Green"
Say "   The tool is built and ready to use." "Green"
Say "  ===========================================================" "Green"
Say ""
Say "   dist\MonkeyTool.exe        <- double-click this"
Say "   dist\monkeytool_cli.exe    command line version"
if (Test-Path $dll) { Say "   dist\libzstd.dll           needed for 3D models" }
if (Test-Path (Join-Path $root "dist\vgmstream\vgmstream-cli.exe")) { Say "   dist\vgmstream\             sound and music decoder" }

Say ""

# LZHAM (the .m3g models' codec) is compiled into MonkeyTool.exe since 1.2,
# so there is no optional DLL to build any more.

$open = Read-Host "Open the dist folder now? (y/n)"
if ($open -eq "y") { Start-Process explorer.exe (Join-Path $root "dist") }
