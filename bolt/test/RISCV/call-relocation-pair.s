// Test that R_RISCV_CALL and R_RISCV_CALL_PLT cover and decode the complete
// AUIPC/JALR instruction pair.

// RUN: llvm-mc -triple riscv64 -mattr=-relax -filetype=obj -o %t.o %s
// RUN: ld.lld --no-relax --emit-relocs -o %t %t.o
// RUN: llvm-readelf --relocations %t | FileCheck --check-prefix=RELOCS %s
// RUN: llvm-bolt --print-fix-riscv-calls --print-only=_start -o %t.bolt %t \
// RUN:     | FileCheck --check-prefix=BOLT %s
// RUN: llvm-objdump -d %t.bolt | FileCheck --check-prefix=OBJDUMP %s

// RELOCS: R_RISCV_CALL {{.*}} target_call
// RELOCS: R_RISCV_CALL_PLT {{.*}} target_call_plt

// BOLT-LABEL: Binary Function "_start" after fix-riscv-calls {
// BOLT:       nop
// BOLT-NEXT:  call target_call
// BOLT-NEXT:  nop
// BOLT-NEXT:  call target_call_plt

// OBJDUMP-LABEL: <_start>:
// OBJDUMP:       nop
// OBJDUMP-NEXT:  auipc ra,
// OBJDUMP-NEXT:  jalr {{.*}}(ra)
// OBJDUMP-NEXT:  nop
// OBJDUMP-NEXT:  auipc ra,
// OBJDUMP-NEXT:  jalr {{.*}}(ra)
// OBJDUMP-LABEL: <target_call>:
// OBJDUMP-LABEL: <target_call_plt>:

  .text
  .option norvc
  .option norelax

  .globl _start
  .type _start,@function
_start:
  .reloc ., R_RISCV_CALL, target_call
  auipc ra, 0
  jalr ra
  .reloc ., R_RISCV_CALL_PLT, target_call_plt
  auipc ra, 0
  jalr ra
  ret
  .size _start, .-_start

  .skip (1 << 21) + 0x7c

  .globl target_call
  .type target_call,@function
target_call:
  ret
  .size target_call, .-target_call

  .skip 0x84

  .globl target_call_plt
  .type target_call_plt,@function
target_call_plt:
  ret
  .size target_call_plt, .-target_call_plt
