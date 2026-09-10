# REQUIRES: system-linux

# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-linux -dwarf-version=5 -defsym=CASE=1 %s -o %t1.o
# RUN: ld.lld -q -e main %t1.o -o %t1.exe
# RUN: llvm-dwarfdump --verify %t1.exe
# RUN: llvm-bolt %t1.exe -o %t1.bolt --update-debug-sections 2>&1 | FileCheck %s --check-prefix=VALID
# RUN: llvm-readelf -x .debug_loclists %t1.bolt | FileCheck %s --check-prefix=SKIP
# RUN: llvm-dwarfdump --verify %t1.bolt

# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-linux -dwarf-version=5 -defsym=CASE=2 %s -o %t2.o
# RUN: ld.lld -q -e main %t2.o -o %t2.exe
# RUN: llvm-dwarfdump --verify %t2.exe
# RUN: llvm-bolt %t2.exe -o %t2.bolt --update-debug-sections 2>&1 | FileCheck %s --check-prefix=VALID
# RUN: llvm-readelf -x .debug_loclists %t2.bolt | FileCheck %s --check-prefix=BRA
# RUN: llvm-dwarfdump --verify %t2.bolt

# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-linux -dwarf-version=5 -defsym=CASE=3 %s -o %t3.o
# RUN: ld.lld -q -e main %t3.o -o %t3.exe
# RUN: llvm-dwarfdump --verify %t3.exe
# RUN: llvm-bolt %t3.exe -o %t3.bolt --update-debug-sections 2>&1 | FileCheck %s --check-prefix=VALID
# RUN: llvm-readelf -x .debug_loclists %t3.bolt | FileCheck %s --check-prefix=NESTED
# RUN: llvm-dwarfdump --verify %t3.bolt

# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-linux -dwarf-version=5 -defsym=CASE=4 %s -o %t4.o
# RUN: ld.lld -q -e main %t4.o -o %t4.exe
# RUN: llvm-bolt %t4.exe -o %t4.bolt --update-debug-sections 2>&1 | FileCheck %s --check-prefix=UNKNOWN
# RUN: llvm-readelf -x .debug_loclists %t4.bolt | FileCheck %s --check-prefix=UNCHANGED

# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-linux -dwarf-version=5 -defsym=CASE=5 %s -o %t5.o
# RUN: ld.lld -q -e main %t5.o -o %t5.exe
# RUN: llvm-bolt %t5.exe -o %t5.bolt --update-debug-sections 2>&1 | FileCheck %s --check-prefix=INVALID
# RUN: llvm-readelf -x .debug_loclists %t5.bolt | FileCheck %s --check-prefix=INVALID-BYTES
# RUN: not llvm-dwarfdump --verify %t5.bolt 2>&1 | FileCheck %s --check-prefix=INVALID-REF

# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-linux -dwarf-version=5 -defsym=CASE=6 %s -o %t6.o
# RUN: ld.lld -q -e main %t6.o -o %t6.exe
# RUN: llvm-dwarfdump --verify %t6.exe
# RUN: llvm-bolt %t6.exe -o %t6.bolt --update-debug-sections 2>&1 | FileCheck %s --check-prefix=VALID
# RUN: llvm-readelf -x .debug_loclists %t6.bolt | FileCheck %s --check-prefix=GENERIC
# RUN: llvm-dwarfdump --verify %t6.bolt

# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-linux -dwarf-version=5 -defsym=CASE=7 %s -o %t7.o
# RUN: ld.lld -q -e main %t7.o -o %t7.exe
# RUN: llvm-dwarfdump --verify %t7.exe
# RUN: llvm-bolt %t7.exe -o %t7.bolt --update-debug-sections 2>&1 | FileCheck %s --check-prefix=VALID
# RUN: llvm-readelf -x .debug_loclists %t7.bolt | FileCheck %s --check-prefix=GROW
# RUN: llvm-dwarfdump --verify %t7.bolt

# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-linux -dwarf-version=5 -defsym=CASE=4 -defsym=INLINE=1 %s -o %tinline.o
# RUN: ld.lld -q -e main %tinline.o -o %tinline.exe
# RUN: llvm-bolt %tinline.exe -o %tinline.bolt --update-debug-sections 2>&1 | FileCheck %s --check-prefix=INLINE-WARN
# RUN: llvm-dwarfdump --debug-info %tinline.bolt | FileCheck %s --check-prefix=INLINE-UNKNOWN
# RUN: llvm-readelf -x .debug_info %tinline.bolt | FileCheck %s --check-prefix=INLINE-BYTES

# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-linux -dwarf-version=5 -defsym=CASE=8 -defsym=INLINE=1 %s -o %tmixed.o
# RUN: ld.lld -q -e main %tmixed.o -o %tmixed.exe
# RUN: llvm-dwarfdump --verify %tmixed.exe
# RUN: llvm-bolt %tmixed.exe -o %tmixed.bolt --update-debug-sections 2>&1 | FileCheck %s --check-prefix=VALID
# RUN: llvm-dwarfdump --debug-info %tmixed.bolt | FileCheck %s --check-prefix=MIXED
# RUN: llvm-readelf -x .debug_info %tmixed.bolt | FileCheck %s --check-prefix=MIXED-BYTES
# RUN: llvm-dwarfdump --verify %tmixed.bolt

# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-linux -dwarf-version=5 -defsym=CASE=9 %s -o %t9.o
# RUN: ld.lld -q -e main %t9.o -o %t9.exe
# RUN: llvm-bolt %t9.exe -o %t9.bolt --update-debug-sections 2>&1 | FileCheck %s --check-prefix=UNKNOWN
# RUN: llvm-readelf -x .debug_loclists %t9.bolt | FileCheck %s --check-prefix=MALFORMED

# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-linux -dwarf-version=5 -defsym=CASE=10 %s -o %t10.o
# RUN: ld.lld -q -e main %t10.o -o %t10.exe
# RUN: llvm-dwarfdump --verify %t10.exe
# RUN: llvm-bolt %t10.exe -o %t10.bolt --update-debug-sections 2>&1 | FileCheck %s --check-prefix=UNKNOWN
# RUN: llvm-readelf -x .debug_loclists %t10.bolt | FileCheck %s --check-prefix=OVERFLOW
# RUN: llvm-dwarfdump --verify %t10.bolt

# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-linux -dwarf-version=5 -defsym=CASE=11 %s -o %t11.o
# RUN: ld.lld -q -e main %t11.o -o %t11.exe
# RUN: llvm-dwarfdump --verify %t11.exe
# RUN: llvm-bolt %t11.exe -o %t11.bolt --update-debug-sections 2>&1 | FileCheck %s --check-prefix=VALID
# RUN: llvm-readelf -x .debug_loclists %t11.bolt | FileCheck %s --check-prefix=NO-REF
# RUN: llvm-dwarfdump --verify %t11.bolt

# RUN: llvm-mc -filetype=obj -triple x86_64-unknown-linux -dwarf-version=5 -defsym=CASE=12 %s -o %t12.o
# RUN: ld.lld -q -e main %t12.o -o %t12.exe
# RUN: llvm-bolt %t12.exe -o %t12.bolt --update-debug-sections 2>&1 | FileCheck %s --check-prefix=UNKNOWN
# RUN: llvm-readelf -x .debug_loclists %t12.bolt | FileCheck %s --check-prefix=TRUNCATED

## A CU whose base type remains at 0x37 isolates changes to expression layout
## from DIE relocation. Expanding its reference from one byte to a five-byte
## slot must also update branches and nested expression lengths. Check the
## bytes: the DWARF verifier does not diagnose all of these corruptions.
## Each list starts with startx_length(index=0, length=3), then its expression.

# VALID-NOT: BOLT-WARNING
# VALID: BOLT-INFO:
# VALID-NOT: BOLT-WARNING

## skip +10 jumps to the backward skip; skip +3 reaches stack_value;
## skip -13 jumps back to lit0. All three targets are opcode boundaries.
# SKIP:      0x00000010 03000311 2f0a0030 a8b78080 80002f03
# SKIP-NEXT: 0x00000020 002ff3ff 9f00

## Both bra operands grow when they cross the converted reference:
## bra +10 reaches lit1; bra -14 reaches lit0. The skip reaches stack_value.
# BRA:      0x00000010 03000313 31280a00 30a8b780 8080002f
# BRA-NEXT: 0x00000020 04003128 f2ff9f00

## entry_value contains ten bytes; GNU_entry_value contains eight. The
## two-byte register operand of regval_type (200) must remain intact.
# NESTED:      0x00000010 0300030d a30af308 a5c801b7 80808000
# NESTED-NEXT: 0x00000020 9f00

## The decoder does not support DW_OP_reinterpret. Preserve the entire
## expression, including both the recognized prefix and unsupported suffix.
# UNKNOWN: BOLT-WARNING: [internal-dwarf-error]: cannot rewrite location expression:
# UNCHANGED: 0x00000010 03000306 30a837a9 379f00

## A missing DIE must remain an invalid reference, without aborting BOLT or
## changing DW_OP_convert into conversion to the generic type (operand zero).
# INVALID: BOLT-WARNING: [internal-dwarf-error]: cannot resolve base type reference
# INVALID-BYTES: 0x00000010 03000308 30a8ff80 8080009f 00
# INVALID-REF: invalid base_type ref: 0x7f

## DW_OP_convert 0 does not reference a DIE and needs no padding.
# GENERIC: 0x00000010 03000304 30a8009f 00

## Growing a nested expression from 127 to 131 bytes also adds a byte to
## its ULEB length. The whole location expression is then 135 bytes long.
# GROW: 0x00000010 03000387 01a38301 a5c801b7 80808000
# GROW: 0x00000090 96969696 96969696 9696969f 00

## Both inline-expression consumers must preserve an unsupported suffix.
# INLINE-WARN: BOLT-WARNING: [internal-dwarf-error]:
# INLINE-UNKNOWN: DW_AT_location (DW_OP_lit0, DW_OP_convert (0x00000037) "int", <decoding error> a9 37 9f)
# INLINE-BYTES: 0x00000050 0630a837 a9379f00 00

## The nested addrx operand has no type reference of its own. It still needs
## padding because the parent expression has references. Address remapping
## shrinks index 128 to 0; the late type patch must retain that new index,
## the six-byte nested body, and the branch target after the skipped convert.
# MIXED: DW_AT_location (DW_OP_entry_value(DW_OP_addrx 0x0), DW_OP_skip +7, DW_OP_lit0, DW_OP_convert (0x00000037) "int", DW_OP_convert (0x00000037) "int", DW_OP_stack_value)
# MIXED-BYTES:      0x00000050 19a306a1 80808080 002f0700 30a8b780
# MIXED-BYTES-NEXT: 0x00000060 808000a8 b7808080 009f0000

## The entry_value body is shorter than its declared length. Preserve the
## recognized reference prefix as well as the malformed nested expression.
# MALFORMED: 0x00000010 03000306 30a837a3 025000

## The input branch displacement is INT16_MAX; reference expansion would
## overflow it. Keep the original expression and report the failed rewrite.
# OVERFLOW: 0x00000010 03000384 80022fff 7f30a837 96969696
# OVERFLOW: 0x00008010 96969696 96969696 319f00

## Without type references, addrx needs no padding. A speculative padded
## layout must not cause an overflow warning for this unchanged expression.
# NO-REF: 0x00000010 03000384 80022fff 7fa10096 96969696
# NO-REF: 0x00008010 96969696 96969696 319f00

## The reference operand itself is truncated. Preserve its continuation
## byte instead of inventing a zero operand or dropping the operation.
# TRUNCATED: 0x00000010 03000303 30a88000

  .ifndef INLINE
  .set INLINE, 0
  .endif

  .text
  .file 0 "." "loclist-layout.c"
  .globl main
  .type main,@function
main:
  .loc 0 1 0
  xorl %eax, %eax
  retq
.Lend:
  .size main, .Lend-main

  .section .debug_abbrev,"",@progbits
.Labbrev:
  .byte 1, 0x11, 1               # DW_TAG_compile_unit, children
  .byte 0x03, 0x08               # DW_AT_name, DW_FORM_string
  .byte 0x11, 0x01               # DW_AT_low_pc, DW_FORM_addr
  .byte 0x55, 0x23               # DW_AT_ranges, DW_FORM_rnglistx
  .byte 0x10, 0x17               # DW_AT_stmt_list, DW_FORM_sec_offset
  .uleb128 0x8c; .byte 0x17      # DW_AT_loclists_base, DW_FORM_sec_offset
  .byte 0x74, 0x17               # DW_AT_rnglists_base, DW_FORM_sec_offset
  .byte 0x73, 0x17               # DW_AT_addr_base, DW_FORM_sec_offset
  .byte 0, 0
  .byte 2, 0x24, 0               # DW_TAG_base_type, no children
  .byte 0x03, 0x08               # DW_AT_name, DW_FORM_string
  .byte 0x3e, 0x0b               # DW_AT_encoding, DW_FORM_data1
  .byte 0x0b, 0x0b               # DW_AT_byte_size, DW_FORM_data1
  .byte 0, 0
  .byte 3, 0x2e, 1               # DW_TAG_subprogram, children
  .byte 0x11, 0x01               # DW_AT_low_pc, DW_FORM_addr
  .byte 0x12, 0x06               # DW_AT_high_pc, DW_FORM_data4
  .byte 0, 0
  .byte 4, 0x34, 0               # DW_TAG_variable, no children
  .byte 0x49, 0x13               # DW_AT_type, DW_FORM_ref4
  .byte 0x02                     # DW_AT_location
  .if INLINE
  .byte 0x18                     # DW_FORM_exprloc
  .else
  .byte 0x22                     # DW_FORM_loclistx
  .endif
  .byte 0, 0, 0

  .section .debug_info,"",@progbits
.Lcu:
  .long .Lcu_end-.Lcu_header
.Lcu_header:
  .short 5
  .byte 1, 8
  .long .Labbrev
  .byte 1
  .asciz "loclist-layout.c"
  .quad 0
  .uleb128 0
  .long .Lline
  .long .Lloc_base
  .long .Lrng_base
  .long .Laddr_base
.Ltype:
  .byte 2
  .asciz "int"
  .byte 5, 4
  .byte 3
  .quad main
  .long .Lend-main
  .byte 4
  .long .Ltype-.Lcu
  .if INLINE
  .uleb128 .Lexpr_end-.Lexpr
  .else
  .uleb128 0
  .byte 0, 0
.Lcu_end:
  .endif

  .section .debug_loclists,"",@progbits
  .long .Lloc_end-.Lloc_header
.Lloc_header:
  .short 5
  .byte 8, 0
  .long 1
.Lloc_base:
  .long .Llist-.Lloc_base
.Llist:
  .if INLINE
  .byte 0                        # Unused empty location list
.Lloc_end:
  .section .debug_info,"",@progbits
  .else
  .byte 8                        # DW_LLE_start_length
  .quad main
  .uleb128 .Lend-main
  .uleb128 .Lexpr_end-.Lexpr
  .endif
.Lexpr:
.if CASE == 1
  .byte 0x2f                     # DW_OP_skip
  .short .Lback_skip-.Lafter_skip
.Lafter_skip:
  .byte 0x30, 0xa8               # DW_OP_lit0, DW_OP_convert
  .uleb128 .Ltype-.Lcu
  .byte 0x2f                     # DW_OP_skip
  .short .Lskip_end-.Lback_skip
.Lback_skip:
  .byte 0x2f                     # DW_OP_skip
  .short .Lafter_skip-.Lskip_end
.Lskip_end:
  .byte 0x9f                     # DW_OP_stack_value
.elseif CASE == 2
  .byte 0x31, 0x28               # DW_OP_lit1, DW_OP_bra
  .short .Lback_bra-.Lafter_bra
.Lafter_bra:
  .byte 0x30, 0xa8               # DW_OP_lit0, DW_OP_convert
  .uleb128 .Ltype-.Lcu
  .byte 0x2f                     # DW_OP_skip
  .short .Lbra_end-.Lback_bra
.Lback_bra:
  .byte 0x31, 0x28               # DW_OP_lit1, DW_OP_bra
  .short .Lafter_bra-.Lbra_end
.Lbra_end:
  .byte 0x9f                     # DW_OP_stack_value
.elseif CASE == 3
  .byte 0xa3                     # DW_OP_entry_value
  .uleb128 .Louter_end-.Louter
.Louter:
  .byte 0xf3                     # DW_OP_GNU_entry_value
  .uleb128 .Linner_end-.Linner
.Linner:
  .byte 0xa5                     # DW_OP_regval_type
  .uleb128 200
  .uleb128 .Ltype-.Lcu
.Linner_end:
.Louter_end:
  .byte 0x9f                     # DW_OP_stack_value
.elseif CASE == 4
  .byte 0x30, 0xa8               # DW_OP_lit0, DW_OP_convert
  .uleb128 .Ltype-.Lcu
  .byte 0xa9                     # DW_OP_reinterpret
  .uleb128 .Ltype-.Lcu
  .byte 0x9f                     # DW_OP_stack_value
.elseif CASE == 5
  .byte 0x30, 0xa8               # DW_OP_lit0, DW_OP_convert
  .uleb128 0x7f                  # No DIE at this offset
  .byte 0x9f                     # DW_OP_stack_value
.elseif CASE == 6
  .byte 0x30, 0xa8, 0, 0x9f      # Convert to the generic type
.elseif CASE == 7
  .byte 0xa3                     # DW_OP_entry_value
  .uleb128 .Llarge_end-.Llarge
.Llarge:
  .byte 0xa5                     # DW_OP_regval_type
  .uleb128 200
  .uleb128 .Ltype-.Lcu
  .rept 123
  .byte 0x96                     # DW_OP_nop
  .endr
.Llarge_end:
  .byte 0x9f                     # DW_OP_stack_value
.elseif CASE == 8
  .byte 0xa3                     # DW_OP_entry_value
  .uleb128 .Laddr_expr_end-.Laddr_expr
.Laddr_expr:
  .byte 0xa1                     # DW_OP_addrx
  .uleb128 128
.Laddr_expr_end:
  .byte 0x2f                     # DW_OP_skip
  .short .Laddr_target-.Laddr_after_skip
.Laddr_after_skip:
  .byte 0x30, 0xa8               # DW_OP_lit0, DW_OP_convert
  .uleb128 .Ltype-.Lcu
.Laddr_target:
  .byte 0xa8                     # DW_OP_convert
  .uleb128 .Ltype-.Lcu
  .byte 0x9f                     # DW_OP_stack_value
.elseif CASE == 9
  .byte 0x30, 0xa8               # DW_OP_lit0, DW_OP_convert
  .uleb128 .Ltype-.Lcu
  .byte 0xa3, 2, 0x50            # Truncated DW_OP_entry_value body
.elseif CASE == 10
  .byte 0x2f                     # DW_OP_skip
  .short .Loverflow_target-.Loverflow_after_skip
.Loverflow_after_skip:
  .byte 0x30, 0xa8               # DW_OP_lit0, DW_OP_convert
  .uleb128 .Ltype-.Lcu
  .rept 32764
  .byte 0x96                     # DW_OP_nop
  .endr
.Loverflow_target:
  .byte 0x31, 0x9f               # DW_OP_lit1, DW_OP_stack_value
.elseif CASE == 11
  .byte 0x2f                     # DW_OP_skip
  .short .Lno_ref_target-.Lno_ref_after_skip
.Lno_ref_after_skip:
  .byte 0xa1, 0                  # DW_OP_addrx 0, no type reference
  .rept 32765
  .byte 0x96                     # DW_OP_nop
  .endr
.Lno_ref_target:
  .byte 0x31, 0x9f               # DW_OP_lit1, DW_OP_stack_value
.elseif CASE == 12
  .byte 0x30, 0xa8, 0x80         # DW_OP_convert has an incomplete ULEB
.endif
.Lexpr_end:
  .if INLINE
  .byte 0, 0                     # End subprogram and CU children
.Lcu_end:
  .else
  .byte 0                        # DW_LLE_end_of_list
.Lloc_end:
  .endif

  .section .debug_rnglists,"",@progbits
  .long .Lrng_end-.Lrng_header
.Lrng_header:
  .short 5
  .byte 8, 0
  .long 1
.Lrng_base:
  .long .Lrng_list-.Lrng_base
.Lrng_list:
  .byte 7                        # DW_RLE_start_length
  .quad main
  .uleb128 .Lend-main
  .byte 0
.Lrng_end:

  .section .debug_addr,"",@progbits
  .long .Laddr_end-.Laddr_header
.Laddr_header:
  .short 5
  .byte 8, 0
.Laddr_base:
  .quad main
  .if CASE == 8
  .zero 127*8
  .quad main                     # Index 128, remapped to index 0
  .endif
.Laddr_end:

  .section .debug_line,"",@progbits
.Lline:
  .section .note.GNU-stack,"",@progbits
