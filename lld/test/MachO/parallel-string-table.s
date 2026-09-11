# REQUIRES: aarch64

# RUN: rm -rf %t && mkdir -p %t/default %t/t1 %t/t2 %t/t8 %t/no-dedup-t1 %t/no-dedup-t8
# RUN: %python -c "exact = '_' + 'a' * 1048572; giant = '_giant_' + 'b' * 1048576; open(r'%t/input.s', 'w').write('.subsections_via_symbols\\n.text\\n.globl {0}\\n{0}:\\n  ret\\n.globl _boundary\\n_boundary:\\n  ret\\n.globl {1}\\n{1}:\\n  ret\\n.globl _after_giant\\n_after_giant:\\n  ret\\n'.format(exact, giant))"
# RUN: llvm-mc -filetype=obj -triple=arm64-apple-darwin %t/input.s -o %t/input.o
# RUN: %no-lsystem-lld -arch arm64 -dylib -undefined dynamic_lookup -install_name @rpath/string-table.dylib -add_ast_path _after_giant %t/input.o -o %t/default/string-table.dylib
# RUN: %no-lsystem-lld -arch arm64 -dylib -undefined dynamic_lookup -install_name @rpath/string-table.dylib -add_ast_path _after_giant --threads=1 %t/input.o -o %t/t1/string-table.dylib
# RUN: cmp %t/default/string-table.dylib %t/t1/string-table.dylib
# RUN: %no-lsystem-lld -arch arm64 -dylib -undefined dynamic_lookup -install_name @rpath/string-table.dylib -add_ast_path _after_giant --threads=2 %t/input.o -o %t/t2/string-table.dylib
# RUN: cmp %t/t1/string-table.dylib %t/t2/string-table.dylib
# RUN: %no-lsystem-lld -arch arm64 -dylib -undefined dynamic_lookup -install_name @rpath/string-table.dylib -add_ast_path _after_giant --threads=8 %t/input.o -o %t/t8/string-table.dylib
# RUN: cmp %t/t1/string-table.dylib %t/t8/string-table.dylib
# RUN: %no-lsystem-lld -arch arm64 -dylib -undefined dynamic_lookup -install_name @rpath/string-table.dylib -no-deduplicate-symbol-strings -add_ast_path _after_giant --threads=1 %t/input.o -o %t/no-dedup-t1/string-table.dylib
# RUN: %no-lsystem-lld -arch arm64 -dylib -undefined dynamic_lookup -install_name @rpath/string-table.dylib -no-deduplicate-symbol-strings -add_ast_path _after_giant --threads=8 %t/input.o -o %t/no-dedup-t8/string-table.dylib
# RUN: cmp %t/no-dedup-t1/string-table.dylib %t/no-dedup-t8/string-table.dylib

## The first symbol leaves the table exactly at 1 MiB including the leading
## space/NUL pair. The boundary symbol starts a new chunk; the giant symbol
## crosses another boundary without being split, and the AST name is a later
## duplicate append. Existing stabs-symbol-string-offsets.s covers the same
## t1/threaded and no-dedup behavior for STABS/source-file strings.
