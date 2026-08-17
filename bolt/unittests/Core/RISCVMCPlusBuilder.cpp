//===- bolt/unittest/Core/RISCVMCPlusBuilder.cpp --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifdef RISCV_AVAILABLE

#include "MCTargetDesc/RISCVMCTargetDesc.h"
#include "bolt/Core/BinaryContext.h"
#include "bolt/Rewrite/RewriteInstance.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/MC/MCInstBuilder.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/TargetParser/SubtargetFeature.h"
#include "gtest/gtest.h"

using namespace llvm;
using namespace llvm::bolt;
using namespace llvm::object;

namespace {

class RISCVMCPlusBuilderTest : public testing::Test {
protected:
  void SetUp() override {
    LLVMInitializeRISCVTargetInfo();
    LLVMInitializeRISCVTargetMC();
    LLVMInitializeRISCVDisassembler();

    memcpy(ElfBuf, "\177ELF", 4);
    auto *EHdr = reinterpret_cast<ELF64LE::Ehdr *>(ElfBuf);
    EHdr->e_ident[ELF::EI_CLASS] = ELF::ELFCLASS64;
    EHdr->e_ident[ELF::EI_DATA] = ELF::ELFDATA2LSB;
    EHdr->e_machine = ELF::EM_RISCV;
    MemoryBufferRef Source(StringRef(ElfBuf, sizeof(ElfBuf)), "ELF");
    ObjFile = cantFail(ObjectFile::createObjectFile(Source));

    Relocation::Arch = Triple::riscv64;
    SubtargetFeatures Features("+m,+a,+f,+d,+c");
    BC = cantFail(BinaryContext::createBinaryContext(
        ObjFile->makeTriple(), std::make_shared<orc::SymbolStringPool>(),
        ObjFile->getFileName(), &Features, true, DWARFContext::create(*ObjFile),
        {llvm::outs(), llvm::errs()}));
    BC->initializeTarget(std::unique_ptr<MCPlusBuilder>(
        createMCPlusBuilder(Triple::riscv64, BC->MIA.get(), BC->MII.get(),
                            BC->MRI.get(), BC->STI.get())));
  }

  static MCInst makeAUIPC(MCPhysReg Destination, int64_t Immediate) {
    return MCInstBuilder(RISCV::AUIPC).addReg(Destination).addImm(Immediate);
  }

  static MCInst makeJALR(MCPhysReg Link, MCPhysReg Base, int64_t Immediate) {
    return MCInstBuilder(RISCV::JALR)
        .addReg(Link)
        .addReg(Base)
        .addImm(Immediate);
  }

  char ElfBuf[sizeof(ELF64LE::Ehdr)] = {};
  std::unique_ptr<ObjectFile> ObjFile;
  std::unique_ptr<BinaryContext> BC;
};

TEST_F(RISCVMCPlusBuilderTest,
       UnsymbolizedCall_StandardCallAndTailCall_AreRecognized) {
  const MCInst CallAUIPC = makeAUIPC(RISCV::X1, 1);
  const MCInst CallJALR = makeJALR(RISCV::X1, RISCV::X1, -2048);
  const MCInst OddCallJALR = makeJALR(RISCV::X1, RISCV::X1, -2047);
  EXPECT_TRUE(BC->MIB->isUnsymbolizedRISCVCall(CallAUIPC, CallJALR));
  EXPECT_EQ(2048, BC->MIB->getUnsymbolizedRISCVCallOffset(CallAUIPC, CallJALR));
  EXPECT_EQ(2048,
            BC->MIB->getUnsymbolizedRISCVCallOffset(CallAUIPC, OddCallJALR));

  const MCInst TailAUIPC = makeAUIPC(RISCV::X6, 0xfffff);
  const MCInst TailJALR = makeJALR(RISCV::X0, RISCV::X6, -4);
  EXPECT_TRUE(BC->MIB->isUnsymbolizedRISCVCall(TailAUIPC, TailJALR));
  EXPECT_EQ(-4100,
            BC->MIB->getUnsymbolizedRISCVCallOffset(TailAUIPC, TailJALR));
}

TEST_F(RISCVMCPlusBuilderTest, UnsymbolizedCall_NearMatches_AreRejected) {
  const MCInst AUIPC = makeAUIPC(RISCV::X1, 1);
  const MCInst JALR = makeJALR(RISCV::X1, RISCV::X1, 0);

  EXPECT_FALSE(BC->MIB->isUnsymbolizedRISCVCall(
      MCInstBuilder(RISCV::LUI).addReg(RISCV::X1).addImm(1), JALR));
  EXPECT_FALSE(BC->MIB->isUnsymbolizedRISCVCall(
      AUIPC, MCInstBuilder(RISCV::JAL).addReg(RISCV::X1).addImm(0)));
  EXPECT_FALSE(BC->MIB->isUnsymbolizedRISCVCall(
      AUIPC, makeJALR(RISCV::X1, RISCV::X6, 0)));
  EXPECT_FALSE(BC->MIB->isUnsymbolizedRISCVCall(
      makeAUIPC(RISCV::X0, 1), makeJALR(RISCV::X1, RISCV::X0, 0)));
  EXPECT_FALSE(BC->MIB->isUnsymbolizedRISCVCall(
      makeAUIPC(RISCV::X5, 1), makeJALR(RISCV::X5, RISCV::X5, 0)));

  MCSymbol *Target = BC->Ctx->createTempSymbol();
  const MCInst SymbolizedAUIPC =
      MCInstBuilder(RISCV::AUIPC)
          .addReg(RISCV::X1)
          .addExpr(MCSymbolRefExpr::create(Target, *BC->Ctx));
  EXPECT_FALSE(BC->MIB->isUnsymbolizedRISCVCall(SymbolizedAUIPC, JALR));
}

TEST(RISCVRelocationTest, CallRelocations_InstructionPair_IsDecoded) {
  Relocation::Arch = Triple::riscv64;
  const auto EncodePair = [](uint32_t Hi20, int32_t Lo12) {
    const uint32_t AUIPC = (Hi20 << 12) | 0x17;
    const uint32_t JALR = ((static_cast<uint32_t>(Lo12) & 0xfff) << 20) | 0x67;
    return static_cast<uint64_t>(AUIPC) | (static_cast<uint64_t>(JALR) << 32);
  };

  EXPECT_EQ(8U, Relocation::getSizeForType(ELF::R_RISCV_CALL));
  EXPECT_EQ(8U, Relocation::getSizeForType(ELF::R_RISCV_CALL_PLT));

  const uint64_t Forward = EncodePair(1, -2048);
  EXPECT_EQ(2048U, Relocation::extractValue(ELF::R_RISCV_CALL, Forward, 0));

  const uint64_t Backward = EncodePair(0xfffff, -4);
  EXPECT_EQ(static_cast<uint64_t>(-4100),
            Relocation::extractValue(ELF::R_RISCV_CALL_PLT, Backward, 0));
}

} // namespace

#endif // RISCV_AVAILABLE
