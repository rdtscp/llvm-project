# REQUIRES: system-linux

# RUN: rm -rf %t && mkdir %t
# RUN: cd %t
# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-linux -dwarf-version=5 %s -split-dwarf-file=ref.dwo -o main.o
# RUN: %clang %cflags main.o -Wl,-q -Wl,-e,main -o main.exe
# RUN: llvm-dwarfdump --verify ref.dwo
# RUN: llvm-dwarfdump --show-form --verbose --debug-info ref.dwo > dump.txt
# RUN: llvm-bolt main.exe -o main.bolt --update-debug-sections --debug-thread-count=4 --cu-processing-batch-size=1
# RUN: llvm-dwarfdump --show-form --verbose --debug-info ref.dwo.dwo >> dump.txt
# RUN: FileCheck %s < dump.txt
# RUN: llvm-dwarfdump --verify main.bolt ref.dwo.dwo

## Rebuilding the split CU removes one byte from the padded DW_AT_low_pc
## operand and adds four bytes to the inline expression's type reference.
## Check that both references follow the type, which moves three bytes.
## Location-list references must use the split CU rather than its skeleton.
## The verifier alone does not diagnose stale references in DWO loclists.

# CHECK: ref.dwo:
# CHECK: DW_AT_location [DW_FORM_loclistx]
# CHECK-NEXT: DW_LLE_startx_length {{.*}}DW_OP_lit0, DW_OP_convert (0x[[#%.8x,TYPE:]]
# CHECK-SAME: -> 0x[[#TYPE]]) "int", DW_OP_stack_value
# CHECK: DW_AT_location [DW_FORM_exprloc] (DW_OP_lit0, DW_OP_convert (0x[[#TYPE]] -> 0x[[#TYPE]]) "int")
# CHECK: 0x[[#TYPE]]: DW_TAG_base_type
# CHECK: ref.dwo.dwo:
# CHECK: DW_AT_location [DW_FORM_loclistx]
# CHECK-NEXT: DW_LLE_startx_length {{.*}}DW_OP_lit0, DW_OP_convert (0x[[#TYPE + 3]] -> 0x[[#TYPE + 3]]) "int", DW_OP_stack_value
# CHECK: DW_AT_location [DW_FORM_exprloc] (DW_OP_lit0, DW_OP_convert (0x[[#TYPE + 3]] -> 0x[[#TYPE + 3]]) "int")
# CHECK: 0x[[#TYPE + 3]]: DW_TAG_base_type

  .text
  .globl main
  .type main,@function
main:
  .file 0 "." "loclist-ref.c"
  .loc 0 1 0
  xorl %eax, %eax
  retq
.Lend:
  .size main, .Lend-main

  .section .debug_abbrev,"",@progbits
  .byte 1, 0x4a, 0               # DW_TAG_skeleton_unit, no children
  .byte 0x76, 0x25               # DW_AT_dwo_name, DW_FORM_strx1
  .byte 0x1b, 0x25               # DW_AT_comp_dir, DW_FORM_strx1
  .byte 0x72, 0x17               # DW_AT_str_offsets_base, DW_FORM_sec_offset
  .byte 0x11, 0x01               # DW_AT_low_pc, DW_FORM_addr
  .byte 0x12, 0x06               # DW_AT_high_pc, DW_FORM_data4
  .byte 0x10, 0x17               # DW_AT_stmt_list, DW_FORM_sec_offset
  .byte 0x73, 0x17               # DW_AT_addr_base, DW_FORM_sec_offset
  .byte 0, 0, 0

  .section .debug_info,"",@progbits
  .long .Lskel_end-.Lskel_header
.Lskel_header:
  .short 5
  .byte 4, 8                     # DW_UT_skeleton, address size
  .long 0
  .quad 42                       # DWO ID
  .byte 1
  .byte 0, 1                     # DWO name, compilation directory
  .long .Lstr_base
  .quad main
  .long .Lend-main
  .long .Lline
  .long .Laddr_base
.Lskel_end:

  .section .debug_abbrev.dwo,"e",@progbits
  .byte 1, 0x11, 1               # DW_TAG_compile_unit, children
  .byte 0x03, 0x08               # DW_AT_name, DW_FORM_string
  .byte 0, 0
  .byte 2, 0x2e, 1               # DW_TAG_subprogram, children
  .byte 0x11, 0x1b               # DW_AT_low_pc, DW_FORM_addrx
  .byte 0x12, 0x06               # DW_AT_high_pc, DW_FORM_data4
  .byte 0, 0
  .byte 3, 0x34, 0               # DW_TAG_variable, no children
  .byte 0x02, 0x22               # DW_AT_location, DW_FORM_loclistx
  .byte 0x49, 0x13               # DW_AT_type, DW_FORM_ref4
  .byte 0, 0
  .byte 4, 0x24, 0               # DW_TAG_base_type, no children
  .byte 0x03, 0x08               # DW_AT_name, DW_FORM_string
  .byte 0x3e, 0x0b               # DW_AT_encoding, DW_FORM_data1
  .byte 0x0b, 0x0b               # DW_AT_byte_size, DW_FORM_data1
  .byte 0, 0
  .byte 5, 0x34, 0               # DW_TAG_variable, no children
  .byte 0x02, 0x18               # DW_AT_location, DW_FORM_exprloc
  .byte 0, 0, 0

  .section .debug_info.dwo,"e",@progbits
.Ldwo:
  .long .Ldwo_end-.Ldwo_header
.Ldwo_header:
  .short 5
  .byte 5, 8                     # DW_UT_split_compile, address size
  .long 0
  .quad 42                       # DWO ID
  .byte 1
  .asciz "loclist-ref.c"
  .byte 2
  .byte 0x80, 0x00               # DW_AT_low_pc: padded ULEB128(0)
  .long .Lend-main
  .byte 3
  .uleb128 0                     # DW_AT_location
  .long .Ltype-.Ldwo
  .byte 5
  .uleb128 .Linline_end-.Linline
.Linline:
  .byte 0x30, 0xa8               # DW_OP_lit0, DW_OP_convert
  .uleb128 .Ltype-.Ldwo
.Linline_end:
  .byte 0                        # End subprogram children
.Ltype:
  .byte 4
  .asciz "int"
  .byte 5, 4                     # DW_ATE_signed, byte size
  .byte 0                        # End CU children
.Ldwo_end:

  .section .debug_loclists.dwo,"e",@progbits
  .long .Lloc_end-.Lloc_header
.Lloc_header:
  .short 5
  .byte 8, 0
  .long 1
.Lloc_base:
  .long .Llist-.Lloc_base
.Llist:
  .byte 3                        # DW_LLE_startx_length
  .uleb128 0
  .uleb128 .Lend-main
  .uleb128 .Lexpr_end-.Lexpr
.Lexpr:
  .byte 0x30, 0xa8               # DW_OP_lit0, DW_OP_convert
  .uleb128 .Ltype-.Ldwo
  .byte 0x9f                     # DW_OP_stack_value
.Lexpr_end:
  .byte 0                        # DW_LLE_end_of_list
.Lloc_end:

  .section .debug_addr,"",@progbits
  .long .Laddr_end-.Laddr_header
.Laddr_header:
  .short 5
  .byte 8, 0
.Laddr_base:
  .quad main
.Laddr_end:

  .section .debug_str,"MS",@progbits,1
.Lstr_dwo:
  .asciz "ref.dwo"
.Lstr_dir:
  .asciz "."
  .section .debug_str_offsets,"",@progbits
  .long 12
  .short 5, 0
.Lstr_base:
  .long .Lstr_dwo, .Lstr_dir

  .section .debug_line,"",@progbits
.Lline:
  .section .note.GNU-stack,"",@progbits
