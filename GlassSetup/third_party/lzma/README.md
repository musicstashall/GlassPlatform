# LZMA decoder (7-Zip SDK)

Source: https://github.com/ip7z/7zip/tree/d3296e39e1f1f92c7b41c0c3848e65640428d42d/C

Unmodified files: LzmaDec.c, LzmaDec.h, 7zTypes.h, Precomp.h, Compiler.h.
Each source file carries Igor Pavlov's public-domain notice. No encoder, external
executable or subprocess is included. LzmaDec.c is compiled as C with no PCH.

Consumer: GlassInnoExtract.h; input, dictionary, output, record count and aggregate
chunk cache limits are imposed by that wrapper. Update the pinned revision and
run inno_extract_tests.py when replacing the decoder.
