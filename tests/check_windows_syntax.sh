#!/bin/bash
# Syntax-check the Windows sources on a non-Windows machine.
#
# tests/winstub/ holds stub headers declaring only the Win32 API surface this
# project uses. They are NOT the real SDK - they exist so that typos, missing
# declarations, wrong argument types and ordinary C++ mistakes get caught
# without needing Windows. Nothing here is compiled into the program.
set -u
cd "$(dirname "$0")/.."

CXXINC=$(echo | g++ -xc++ -E -v - 2>&1 | sed -n '/#include <...>/,/End of search/p' \
         | grep '^ /' | sed 's/^ /-I/' | tr '\n' ' ')

fail=0
for f in src/main_win32.cpp src/win32_save_editor.cpp src/main_cli.cpp src/nfsnl_core.cpp \
         src/nfsnl_image.cpp src/nfsnl_model.cpp src/nfsnl_m3g.cpp src/nfsnl_render.cpp src/nfsnl_audio.cpp src/nfsnl_text.cpp src/nfsnl_library.cpp src/nfsnl_profiles.cpp src/nfsnl_rr3.cpp src/nfsnl_import.cpp src/nfsnl_data.cpp src/nfsnl_save.cpp src/nfsnl_sbin.cpp src/nfsnl_decode.cpp src/nfsnl_encode.cpp src/nfsnl_im2.cpp src/nfsnl_scene.cpp src/nfsnl_astc.cpp src/nfsnl_brotli.cpp src/nfsnl_jsr.cpp; do
    printf "%-24s " "$(basename "$f")"
    if out=$(g++ -std=c++17 -fsyntax-only -D_WIN32 -DUNICODE -D_UNICODE \
                 -Isrc -Itests/winstub -nostdinc++ $CXXINC "$f" 2>&1); then
        echo "ok"
    else
        echo "FAILED"
        echo "$out" | head -20
        fail=1
    fi
done
exit $fail
