//===- bolt/Core/DIEBuilder.cpp -------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "bolt/Core/DIEBuilder.h"
#include "bolt/Core/BinaryContext.h"
#include "bolt/Core/DebugData.h"
#include "bolt/Core/ParallelUtilities.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/CodeGen/DIE.h"
#include "llvm/DebugInfo/DWARF/DWARFAbbreviationDeclaration.h"
#include "llvm/DebugInfo/DWARF/DWARFDie.h"
#include "llvm/DebugInfo/DWARF/DWARFFormValue.h"
#include "llvm/DebugInfo/DWARF/DWARFTypeUnit.h"
#include "llvm/DebugInfo/DWARF/DWARFUnit.h"
#include "llvm/DebugInfo/DWARF/DWARFUnitIndex.h"
#include "llvm/DebugInfo/DWARF/LowLevel/DWARFExpression.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#undef DEBUG_TYPE
#define DEBUG_TYPE "bolt"
namespace opts {
extern cl::opt<unsigned> Verbosity;
}
namespace llvm {
namespace bolt {

/// Adds a \p Str to .debug_str section.
/// Uses \p AttrInfoVal to either update entry in a DIE for legacy DWARF using
/// \p DebugInfoPatcher, or for DWARF5 update an index in .debug_str_offsets
/// for this contribution of \p Unit.
static void addStringHelper(DebugStrOffsetsWriter &StrOffstsWriter,
                            DebugStrWriter &StrWriter, DIEBuilder &DIEBldr,
                            DIE &Die, const DWARFUnit &Unit,
                            DIEValue &DIEAttrInfo, StringRef Str) {
  uint32_t NewOffset = StrWriter.addString(Str);
  if (Unit.getVersion() >= 5) {
    StrOffstsWriter.updateAddressMap(DIEAttrInfo.getDIEInteger().getValue(),
                                     NewOffset, Unit);
    return;
  }
  DIEBldr.replaceValue(&Die, DIEAttrInfo.getAttribute(), DIEAttrInfo.getForm(),
                       DIEInteger(NewOffset));
}

std::string DIEBuilder::updateDWONameCompDir(
    DebugStrOffsetsWriter &StrOffstsWriter, DebugStrWriter &StrWriter,
    DWARFUnit &SkeletonCU, StringRef DwarfOutputPath,
    const StringRef DWONameToUse) {
  DIE &UnitDIE = *getUnitDIEbyUnit(SkeletonCU);
  DIEValue DWONameAttrInfo = UnitDIE.findAttribute(dwarf::DW_AT_dwo_name);
  if (!DWONameAttrInfo)
    DWONameAttrInfo = UnitDIE.findAttribute(dwarf::DW_AT_GNU_dwo_name);
  if (!DWONameAttrInfo)
    return "";
  std::string ObjectName(DWONameToUse);
  addStringHelper(StrOffstsWriter, StrWriter, *this, UnitDIE, SkeletonCU,
                  DWONameAttrInfo, ObjectName);

  DIEValue CompDirAttrInfo = UnitDIE.findAttribute(dwarf::DW_AT_comp_dir);
  assert(CompDirAttrInfo && "DW_AT_comp_dir is not in Skeleton CU.");

  if (!DwarfOutputPath.empty()) {
    addStringHelper(StrOffstsWriter, StrWriter, *this, UnitDIE, SkeletonCU,
                    CompDirAttrInfo, DwarfOutputPath);
  }
  return ObjectName;
}

void DIEBuilder::updateDWONameCompDirForTypes(
    DebugStrOffsetsWriter &StrOffstsWriter, DebugStrWriter &StrWriter,
    DWARFUnit &Unit, StringRef DwarfOutputPath, const StringRef DWOName) {
  for (DWARFUnit *DU : getState().DWARF5TUVector)
    updateDWONameCompDir(StrOffstsWriter, StrWriter, *DU, DwarfOutputPath,
                         DWOName);
  if (StrOffstsWriter.isStrOffsetsSectionModified())
    StrOffstsWriter.finalizeSection(Unit, *this);
}

void DIEBuilder::updateReferences() {
  for (auto &[SrcDIEInfo, ReferenceInfo] : getState().AddrReferences) {
    DIEInfo *DstDIEInfo = ReferenceInfo.Dst;
    DWARFUnitInfo &DstUnitInfo = getUnitInfo(DstDIEInfo->UnitId);
    dwarf::Attribute Attr = ReferenceInfo.AttrSpec.Attr;
    dwarf::Form Form = ReferenceInfo.AttrSpec.Form;

    const uint64_t NewAddr =
        DstDIEInfo->Die->getOffset() + DstUnitInfo.UnitOffset;
    SrcDIEInfo->Die->replaceValue(getState().DIEAlloc, Attr, Form,
                                  DIEInteger(NewAddr));
  }
  getState().AddrReferences.clear();
}

void DIEBuilder::updateLocationReferences() {
  // Handling references in location expressions.
  for (LocWithReference &LocExpr : getState().LocWithReferencesToProcess) {
    // Address indices may have been rewritten since preparation. Patch the
    // current attribute, whose padded type operands still hold input offsets.
    DIEValue Current = LocExpr.Die.findAttribute(LocExpr.Attr);
    if (!Current)
      continue;
    const DIEValueList &Values =
        LocExpr.Form == dwarf::DW_FORM_exprloc
            ? static_cast<const DIEValueList &>(Current.getDIELoc())
            : static_cast<const DIEValueList &>(Current.getDIEBlock());
    SmallVector<uint8_t, 32> Input;
    for (const DIEValue &Value : Values.values())
      Input.push_back(Value.getDIEInteger().getValue());
    SmallVector<uint8_t, 32> Buffer;
    Expected<bool> HasReference = rewriteExpressionReferences(
        Input, LocExpr.U, Buffer, ExpressionStage::Patch);
    if (!HasReference) {
      BC.errs()
          << "BOLT-WARNING: [internal-dwarf-error]: cannot patch location "
             "expression: "
          << toString(HasReference.takeError()) << '\n';
      continue;
    }
    if (Buffer.size() != Input.size()) {
      BC.errs() << "BOLT-WARNING: [internal-dwarf-error]: location expression "
                   "size changed while patching references\n";
      continue;
    }

    DIEValueList *AttrVal;
    if (LocExpr.Form == dwarf::DW_FORM_exprloc) {
      DIELoc *DL = new (getState().DIEAlloc) DIELoc;
      DL->setSize(Buffer.size());
      AttrVal = static_cast<DIEValueList *>(DL);
    } else {
      DIEBlock *DBL = new (getState().DIEAlloc) DIEBlock;
      DBL->setSize(Buffer.size());
      AttrVal = static_cast<DIEValueList *>(DBL);
    }
    for (auto Byte : Buffer)
      AttrVal->addValue(getState().DIEAlloc, static_cast<dwarf::Attribute>(0),
                        dwarf::DW_FORM_data1, DIEInteger(Byte));

    DIEValue Value;
    if (LocExpr.Form == dwarf::DW_FORM_exprloc)
      Value =
          DIEValue(dwarf::Attribute(LocExpr.Attr), dwarf::Form(LocExpr.Form),
                   static_cast<DIELoc *>(AttrVal));
    else
      Value =
          DIEValue(dwarf::Attribute(LocExpr.Attr), dwarf::Form(LocExpr.Form),
                   static_cast<DIEBlock *>(AttrVal));

    LocExpr.Die.replaceValue(getState().DIEAlloc, LocExpr.Attr, LocExpr.Form,
                             Value);
  }
  getState().LocWithReferencesToProcess.clear();
}

uint32_t DIEBuilder::allocDIE(const DWARFUnit &DU, const DWARFDie &DDie,
                              BumpPtrAllocator &Alloc, const uint32_t UId) {
  DWARFUnitInfo &DWARFUnitInfo = getUnitInfo(UId);
  const uint64_t DDieOffset = DDie.getOffset();
  if (DWARFUnitInfo.DIEIDMap.count(DDieOffset))
    return DWARFUnitInfo.DIEIDMap[DDieOffset];

  DIE *Die = DIE::get(Alloc, dwarf::Tag(DDie.getTag()));
  // This handles the case where there is a DIE ref which points to
  // invalid DIE. This prevents assert when IR is written out.
  // Also it makes debugging easier.
  // DIE dump is not very useful.
  // It's nice to know original offset from which this DIE was constructed.
  Die->setOffset(DDie.getOffset());
  if (opts::Verbosity >= 1)
    getState().DWARFDieAddressesParsed.insert(DDie.getOffset());
  const uint32_t DId = DWARFUnitInfo.DieInfoVector.size();
  DWARFUnitInfo.DIEIDMap[DDieOffset] = DId;
  DWARFUnitInfo.DieInfoVector.emplace_back(
      std::make_unique<DIEInfo>(DIEInfo{Die, DId, UId}));
  return DId;
}

void DIEBuilder::constructFromUnit(DWARFUnit &DU) {
  std::optional<uint32_t> UnitId = getUnitId(DU);
  if (!UnitId) {
    BC.errs() << "BOLT-WARNING: [internal-dwarf-error]: "
              << "Skip Unit at " << Twine::utohexstr(DU.getOffset()) << "\n";
    return;
  }

  const uint32_t UnitHeaderSize = DU.getHeaderSize();
  uint64_t DIEOffset = DU.getOffset() + UnitHeaderSize;
  uint64_t NextCUOffset = DU.getNextUnitOffset();
  DWARFDataExtractor DebugInfoData = DU.getDebugInfoExtractor();
  DWARFDebugInfoEntry DIEEntry;
  std::vector<DIE *> CurParentDIEStack;
  std::vector<uint32_t> Parents;
  uint32_t TUTypeOffset = 0;

  if (DWARFTypeUnit *TU = dyn_cast_or_null<DWARFTypeUnit>(&DU))
    TUTypeOffset = TU->getTypeOffset();

  assert(DebugInfoData.isValidOffset(NextCUOffset - 1));
  Parents.push_back(UINT32_MAX);
  do {
    const bool IsTypeDIE = (TUTypeOffset == DIEOffset - DU.getOffset());
    if (!DIEEntry.extractFast(DU, &DIEOffset, DebugInfoData, NextCUOffset,
                              Parents.back()))
      break;

    if (const DWARFAbbreviationDeclaration *AbbrDecl =
            DIEEntry.getAbbreviationDeclarationPtr()) {
      DWARFDie DDie(&DU, &DIEEntry);

      DIE *CurDIE = constructDIEFast(DDie, DU, *UnitId);
      DWARFUnitInfo &UI = getUnitInfo(*UnitId);
      // Can't rely on first element in DieVector due to cross CU forward
      // references.
      if (!UI.UnitDie)
        UI.UnitDie = CurDIE;
      if (IsTypeDIE)
        getState().TypeDIEMap[&DU] = CurDIE;

      if (!CurParentDIEStack.empty())
        CurParentDIEStack.back()->addChild(CurDIE);

      if (AbbrDecl->hasChildren())
        CurParentDIEStack.push_back(CurDIE);
    } else {
      // NULL DIE: finishes current children scope.
      CurParentDIEStack.pop_back();
    }
  } while (CurParentDIEStack.size() > 0);

  getState().CloneUnitCtxMap[*UnitId].IsConstructed = true;
}

DIEBuilder::DIEBuilder(BinaryContext &BC, DWARFContext *DwarfContext,
                       DWARF5AcceleratorTable &DebugNamesTable,
                       DWARFUnit *SkeletonCU)
    : BC(BC), DwarfContext(DwarfContext), SkeletonCU(SkeletonCU),
      DebugNamesTable(DebugNamesTable) {}

static unsigned int getCUNum(DWARFContext *DwarfContext, bool IsDWO) {
  unsigned int CUNum = IsDWO ? DwarfContext->getNumDWOCompileUnits()
                             : DwarfContext->getNumCompileUnits();
  CUNum += IsDWO ? DwarfContext->getNumDWOTypeUnits()
                 : DwarfContext->getNumTypeUnits();
  return CUNum;
}

void DIEBuilder::buildTypeUnits(DebugStrOffsetsWriter *StrOffsetWriter,
                                const bool Init) {
  if (Init)
    BuilderState.reset(new State());

  const DWARFUnitIndex &TUIndex = DwarfContext->getTUIndex();
  if (!TUIndex.getRows().empty()) {
    for (auto &Row : TUIndex.getRows()) {
      uint64_t Signature = Row.getSignature();
      // manually populate TypeUnit to UnitVector
      DwarfContext->getTypeUnitForHash(Signature, true);
    }
  }
  const unsigned int CUNum = getCUNum(DwarfContext, isDWO());
  getState().CloneUnitCtxMap.resize(CUNum);
  DWARFContext::unit_iterator_range CU4TURanges =
      isDWO() ? DwarfContext->dwo_types_section_units()
              : DwarfContext->types_section_units();

  getState().Type = ProcessingType::DWARF4TUs;
  for (std::unique_ptr<DWARFUnit> &DU : CU4TURanges)
    registerUnit(*DU, false);

  for (std::unique_ptr<DWARFUnit> &DU : CU4TURanges)
    constructFromUnit(*DU);

  DWARFContext::unit_iterator_range CURanges =
      isDWO() ? DwarfContext->dwo_info_section_units()
              : DwarfContext->info_section_units();

  // This handles DWARF4 CUs and DWARF5 CU/TUs.
  // Creating a vector so that for reference handling only DWARF5 CU/TUs are
  // used, and not DWARF4 TUs.
  getState().Type = ProcessingType::DWARF5TUs;
  for (std::unique_ptr<DWARFUnit> &DU : CURanges) {
    if (!DU->isTypeUnit())
      continue;
    registerUnit(*DU, false);
  }

  for (DWARFUnit *DU : getState().DWARF5TUVector) {
    constructFromUnit(*DU);
    if (StrOffsetWriter)
      StrOffsetWriter->finalizeSection(*DU, *this);
  }
}

/// Collects the signatures of all type units referenced (via DW_FORM_ref_sig8)
/// by any DIE of \p U. The DIEs are streamed with forEachDIEInUnit so the
/// unit's full DIE vector is never materialized.
///
/// Note: De-duplication of the collected signatures is handled at the outer
/// level by registerUnit.
static void collectReferencedTypeSignatures(DWARFUnit &U,
                                            DenseSet<uint64_t> &ProcessedTU,
                                            SmallVectorImpl<uint64_t> &TUlist) {
  forEachDIEInUnit(U, [&](const DWARFDie &Die) {
    for (const DWARFAttribute &Attr : Die.attributes()) {
      if (Attr.Value.getForm() != dwarf::DW_FORM_ref_sig8)
        continue;
      if (const std::optional<uint64_t> Signature =
              Attr.Value.getAsSignatureReference())
        if (ProcessedTU.insert(*Signature).second)
          TUlist.push_back(*Signature);
    }
  });
}

void DIEBuilder::buildDWPTypeUnitsForUnit(DWARFUnit &U) {
  std::unique_lock<std::mutex> LockGuard(BC.getUnitsMutex());
  // Avoid processing the same type unit multiple times.
  DenseSet<uint64_t> ProcessedTU;
  SmallVector<uint64_t, 8> TUlist;
  // Collecting signatures of type units referenced by this unit.
  collectReferencedTypeSignatures(U, ProcessedTU, TUlist);

  getState().Type = U.getVersion() < 5 ? ProcessingType::DWARF4TUs
                                       : ProcessingType::DWARF5TUs;
  // addressing type units referenced.
  for (unsigned I = 0; I != TUlist.size(); ++I) {
    const uint64_t Signature = TUlist[I];
    DWARFTypeUnit *TU = DwarfContext->getTypeUnitForHash(Signature, true);
    if (!TU)
      continue;
    if (!registerUnit(*TU, false))
      continue;

    const std::optional<uint32_t> UnitId = getUnitId(*TU);
    if (!UnitId || getState().CloneUnitCtxMap[*UnitId].IsConstructed)
      continue;

    collectReferencedTypeSignatures(*TU, ProcessedTU, TUlist);
  }

  // Ensure original order of processing type units
  auto SortByOffset = [](const DWARFUnit *A, const DWARFUnit *B) {
    return A->getOffset() < B->getOffset();
  };

  // For Split DWARF, we have either DWARF4 or DWARF5, they cannot be mixed.
  std::vector<DWARFUnit *> &TUVec = getState().Type == ProcessingType::DWARF4TUs
                                        ? getState().DWARF4TUVector
                                        : getState().DWARF5TUVector;
  llvm::sort(TUVec, SortByOffset);

  for (DWARFUnit *TU : TUVec)
    constructFromUnit(*TU);
}

void DIEBuilder::buildCompileUnits(const bool Init) {
  if (Init)
    BuilderState.reset(new State());

  unsigned int CUNum = getCUNum(DwarfContext, isDWO());
  getState().CloneUnitCtxMap.resize(CUNum);
  DWARFContext::unit_iterator_range CURanges =
      isDWO() ? DwarfContext->dwo_info_section_units()
              : DwarfContext->info_section_units();

  // This handles DWARF4 CUs and DWARF5 CU/TUs.
  // Creating a vector so that for reference handling only DWARF5 CU/TUs are
  // used, and not DWARF4 TUs.getState().DUList
  getState().Type = ProcessingType::CUs;
  for (std::unique_ptr<DWARFUnit> &DU : CURanges) {
    if (DU->isTypeUnit())
      continue;
    registerUnit(*DU, false);
  }

  // Using DULIst since it can be modified by cross CU reference resolution.
  for (DWARFUnit *DU : getState().DUList) {
    if (DU->isTypeUnit())
      continue;
    constructFromUnit(*DU);
  }
}
void DIEBuilder::buildCompileUnits(const SmallVector<DWARFUnit *> &CUs) {
  BuilderState.reset(new State());
  // Allocating enough for current batch being processed.
  // In real use cases we either processing a batch of CUs with no cross
  // references, or if they do have them it is due to LTO. With clang they will
  // share the same abbrev table. In either case this vector will not grow.
  getState().CloneUnitCtxMap.resize(CUs.size());
  getState().Type = ProcessingType::CUs;
  for (DWARFUnit *CU : CUs)
    registerUnit(*CU, false);

  for (DWARFUnit *DU : getState().DUList)
    constructFromUnit(*DU);
}

void DIEBuilder::buildDWOUnit(DWARFUnit &U) {
  BuilderState.release();
  BuilderState = std::make_unique<State>();
  if (DwarfContext->isDWP()) {
    buildDWPTypeUnitsForUnit(U);
  } else {
    buildTypeUnits(nullptr, false);
  }
  getState().Type = ProcessingType::CUs;
  registerUnit(U, false);
  constructFromUnit(U);
}

DIE *DIEBuilder::constructDIEFast(DWARFDie &DDie, DWARFUnit &U,
                                  uint32_t UnitId) {

  std::optional<uint32_t> Idx = getAllocDIEId(U, DDie);
  if (Idx) {
    DWARFUnitInfo &DWARFUnitInfo = getUnitInfo(UnitId);
    DIEInfo &DieInfo = getDIEInfo(UnitId, *Idx);
    if (DWARFUnitInfo.IsConstructed && DieInfo.Die)
      return DieInfo.Die;
  } else {
    Idx = allocDIE(U, DDie, getState().DIEAlloc, UnitId);
  }

  DIEInfo &DieInfo = getDIEInfo(UnitId, *Idx);

  uint64_t Offset = DDie.getOffset();
  uint64_t NextOffset = Offset;
  DWARFDataExtractor Data = U.getDebugInfoExtractor();
  DWARFDebugInfoEntry DDIEntry;

  if (DDIEntry.extractFast(U, &NextOffset, Data, U.getNextUnitOffset(), 0))
    assert(NextOffset - U.getOffset() <= Data.getData().size() &&
           "NextOffset OOB");

  SmallString<40> DIECopy(Data.getData().substr(Offset, NextOffset - Offset));
  Data =
      DWARFDataExtractor(DIECopy, Data.isLittleEndian(), Data.getAddressSize());

  const DWARFAbbreviationDeclaration *Abbrev =
      DDie.getAbbreviationDeclarationPtr();
  uint64_t AttrOffset = getULEB128Size(Abbrev->getCode());

  using AttrSpec = DWARFAbbreviationDeclaration::AttributeSpec;
  for (const AttrSpec &AttrSpec : Abbrev->attributes()) {
    DWARFFormValue Val = AttrSpec.getFormValue();
    Val.extractValue(Data, &AttrOffset, U.getFormParams(), &U);
    cloneAttribute(*DieInfo.Die, DDie, U, Val, AttrSpec);
  }
  return DieInfo.Die;
}

static DWARFUnit *
getUnitForOffset(DIEBuilder &Builder, DWARFContext &DWCtx,
                 const uint64_t Offset,
                 const DWARFAbbreviationDeclaration::AttributeSpec AttrSpec) {
  auto findUnit = [&](std::vector<DWARFUnit *> &Units) -> DWARFUnit * {
    auto CUIter = llvm::upper_bound(Units, Offset,
                                    [](uint64_t LHS, const DWARFUnit *RHS) {
                                      return LHS < RHS->getNextUnitOffset();
                                    });
    static std::vector<DWARFUnit *> CUOffsets;
    static std::once_flag InitVectorFlag;
    auto initCUVector = [&]() {
      CUOffsets.reserve(DWCtx.getNumCompileUnits());
      for (const std::unique_ptr<DWARFUnit> &CU : DWCtx.compile_units())
        CUOffsets.emplace_back(CU.get());
    };
    DWARFUnit *CU = CUIter != Units.end() ? *CUIter : nullptr;
    // Above algorithm breaks when there is only one CU, and reference is
    // outside of it. Fall through slower path, that searches all the CUs.
    // For example when src and destination of cross CU references have
    // different abbrev section.
    if (!CU ||
        (CU && AttrSpec.Form == dwarf::DW_FORM_ref_addr &&
         !(CU->getOffset() < Offset && CU->getNextUnitOffset() > Offset))) {
      // This is a work around for XCode clang. There is a build error when we
      // pass DWCtx.compile_units() to llvm::upper_bound
      std::call_once(InitVectorFlag, initCUVector);
      auto CUIter = llvm::upper_bound(CUOffsets, Offset,
                                      [](uint64_t LHS, const DWARFUnit *RHS) {
                                        return LHS < RHS->getNextUnitOffset();
                                      });
      CU = CUIter != CUOffsets.end() ? (*CUIter) : nullptr;
    }
    return CU;
  };

  switch (Builder.getCurrentProcessingState()) {
  case DIEBuilder::ProcessingType::DWARF4TUs:
    return findUnit(Builder.getDWARF4TUVector());
  case DIEBuilder::ProcessingType::DWARF5TUs:
    return findUnit(Builder.getDWARF5TUVector());
  case DIEBuilder::ProcessingType::CUs:
    return findUnit(Builder.getDWARFCUVector());
  };

  return nullptr;
}

uint32_t DIEBuilder::finalizeDIEs(DWARFUnit &CU, DIE &Die,
                                  uint32_t &CurOffset) {
  getState().DWARFDieAddressesParsed.erase(Die.getOffset());
  uint32_t CurSize = 0;
  Die.setOffset(CurOffset);
  // It is possible that an indexed debugging information entry has a parent
  // that is not indexed (for example, if its parent does not have a name
  // attribute). In such a case, a parent attribute may point to a nameless
  // index entry (that is, one that cannot be reached from any entry in the name
  // table), or it may point to the nearest ancestor that does have an index
  // entry.
  // Skipping entry is not very useful for LLDB. This follows clang where
  // children of forward declaration won't have DW_IDX_parent.
  // https://github.com/llvm/llvm-project/pull/91808

  // If Parent is nullopt and NumberParentsInChain is not zero, then forward
  // declaration was encountered in this DF traversal. Propagating nullopt for
  // Parent to children.
  for (DIEValue &Val : Die.values())
    CurSize += Val.sizeOf(CU.getFormParams());
  CurSize += getULEB128Size(Die.getAbbrevNumber());
  CurOffset += CurSize;

  for (DIE &Child : Die.children()) {
    uint32_t ChildSize = finalizeDIEs(CU, Child, CurOffset);
    CurSize += ChildSize;
  }
  // for children end mark.
  if (Die.hasChildren()) {
    CurSize += sizeof(uint8_t);
    CurOffset += sizeof(uint8_t);
  }

  Die.setSize(CurSize);
  return CurSize;
}

void DIEBuilder::finish() {
  auto finalizeCU = [&](DWARFUnit &CU, uint64_t &UnitStartOffset) -> void {
    DIE *UnitDIE = getUnitDIEbyUnit(CU);
    uint32_t HeaderSize = CU.getHeaderSize();
    uint32_t CurOffset = HeaderSize;
    std::vector<std::optional<BOLTDWARF5AccelTableData *>> Parents;
    Parents.push_back(std::nullopt);
    finalizeDIEs(CU, *UnitDIE, CurOffset);

    DWARFUnitInfo &CurUnitInfo = getUnitInfoByDwarfUnit(CU);
    CurUnitInfo.UnitOffset = UnitStartOffset;
    CurUnitInfo.UnitLength = HeaderSize + UnitDIE->getSize();
    UnitStartOffset += CurUnitInfo.UnitLength;
  };
  // Computing offsets for .debug_types section.
  // It's processed first when CU is registered so will be at the beginning of
  // the vector.
  uint64_t TypeUnitStartOffset = 0;
  for (DWARFUnit *CU : getState().DUList) {
    // We process DWARF$ types first.
    if (!(CU->getVersion() < 5 && CU->isTypeUnit()))
      break;
    finalizeCU(*CU, TypeUnitStartOffset);
  }

  for (DWARFUnit *CU : getState().DUList) {
    // Skipping DWARF4 types.
    if (CU->getVersion() < 5 && CU->isTypeUnit())
      continue;
    finalizeCU(*CU, UnitSize);
  }
  if (opts::Verbosity >= 1) {
    if (!getState().DWARFDieAddressesParsed.empty())
      dbgs() << "Referenced DIE offsets not in .debug_info\n";
    for (const uint64_t Address : getState().DWARFDieAddressesParsed) {
      dbgs() << Twine::utohexstr(Address) << "\n";
    }
  }
  updateLocationReferences();
}

void DIEBuilder::populateDebugNamesTable(
    DWARFUnit &CU, const DIE &Die,
    std::optional<BOLTDWARF5AccelTableData *> Parent,
    uint32_t NumberParentsInChain) {
  std::optional<BOLTDWARF5AccelTableData *> NameEntry =
      DebugNamesTable.addAccelTableEntry(
          CU, Die, SkeletonCU ? SkeletonCU->getDWOId() : std::nullopt,
          NumberParentsInChain, Parent);
  if (!Parent && NumberParentsInChain)
    NameEntry = std::nullopt;
  if (NameEntry)
    ++NumberParentsInChain;

  for (const DIE &Child : Die.children())
    populateDebugNamesTable(CU, Child, NameEntry, NumberParentsInChain);
}

void DIEBuilder::updateDebugNamesTable() {
  auto finalizeDebugNamesTableForCU = [&](DWARFUnit &CU,
                                          uint64_t &UnitStartOffset) -> void {
    DIE *UnitDIE = getUnitDIEbyUnit(CU);
    DebugNamesTable.setCurrentUnit(CU, UnitStartOffset);
    populateDebugNamesTable(CU, *UnitDIE, std::nullopt, 0);

    DWARFUnitInfo &CurUnitInfo = getUnitInfoByDwarfUnit(CU);
    UnitStartOffset += CurUnitInfo.UnitLength;
  };

  uint64_t TypeUnitStartOffset = 0;
  for (DWARFUnit *CU : getState().DUList) {
    if (!(CU->getVersion() < 5 && CU->isTypeUnit()))
      break;
    finalizeDebugNamesTableForCU(*CU, TypeUnitStartOffset);
  }

  for (DWARFUnit *CU : getState().DUList) {
    if (CU->getVersion() < 5 && CU->isTypeUnit())
      continue;
    finalizeDebugNamesTableForCU(*CU, DebugNamesUnitSize);
  }
  // Debug names resolve cross-CU references using their original offsets.
  updateReferences();
}

DWARFDie DIEBuilder::resolveDIEReference(
    const DWARFAbbreviationDeclaration::AttributeSpec AttrSpec,
    const uint64_t RefOffset, DWARFUnit *&RefCU,
    DWARFDebugInfoEntry &DwarfDebugInfoEntry) {
  uint64_t TmpRefOffset = RefOffset;
  if ((RefCU =
           getUnitForOffset(*this, *DwarfContext, TmpRefOffset, AttrSpec))) {
    /// Trying to add to current working set in case it's cross CU reference.
    if (!registerUnit(*RefCU, true))
      return DWARFDie();
    DWARFDataExtractor DebugInfoData = RefCU->getDebugInfoExtractor();
    if (DwarfDebugInfoEntry.extractFast(*RefCU, &TmpRefOffset, DebugInfoData,
                                        RefCU->getNextUnitOffset(), 0)) {
      // In a file with broken references, an attribute might point to a NULL
      // DIE.
      DWARFDie RefDie = DWARFDie(RefCU, &DwarfDebugInfoEntry);
      if (!RefDie.isNULL()) {
        std::optional<uint32_t> UnitId = getUnitId(*RefCU);

        // forward reference
        if (UnitId && !getState().CloneUnitCtxMap[*UnitId].IsConstructed &&
            !getAllocDIEId(*RefCU, RefDie))
          allocDIE(*RefCU, RefDie, getState().DIEAlloc, *UnitId);
        return RefDie;
      }
      BC.errs()
          << "BOLT-WARNING: [internal-dwarf-error]: invalid referenced DIE "
             "at offset: "
          << Twine::utohexstr(RefOffset) << ".\n";

    } else {
      BC.errs() << "BOLT-WARNING: [internal-dwarf-error]: could not parse "
                   "referenced DIE at offset: "
                << Twine::utohexstr(RefOffset) << ".\n";
    }
  } else {
    BC.errs()
        << "BOLT-WARNING: [internal-dwarf-error]: could not find referenced "
           "CU. Referenced DIE offset: "
        << Twine::utohexstr(RefOffset) << ".\n";
  }
  return DWARFDie();
}

void DIEBuilder::cloneDieOffsetReferenceAttribute(
    DIE &Die, DWARFUnit &U, const DWARFDie &InputDIE,
    const DWARFAbbreviationDeclaration::AttributeSpec AttrSpec, uint64_t Ref) {
  DIE *NewRefDie = nullptr;
  DWARFUnit *RefUnit = nullptr;

  DWARFDebugInfoEntry DDIEntry;
  const DWARFDie RefDie = resolveDIEReference(AttrSpec, Ref, RefUnit, DDIEntry);

  if (!RefDie)
    return;

  const std::optional<uint32_t> UnitId = getUnitId(*RefUnit);
  const std::optional<uint32_t> IsAllocId = getAllocDIEId(*RefUnit, RefDie);
  assert(IsAllocId.has_value() && "Encountered unexpected unallocated DIE.");
  const uint32_t DIEId = *IsAllocId;
  DIEInfo &DieInfo = getDIEInfo(*UnitId, DIEId);

  if (!DieInfo.Die) {
    assert(Ref > InputDIE.getOffset());
    (void)Ref;
    BC.errs() << "BOLT-WARNING: [internal-dwarf-error]: encounter unexpected "
                 "unallocated DIE. Should be alloc!\n";
    // We haven't cloned this DIE yet. Just create an empty one and
    // store it. It'll get really cloned when we process it.
    DieInfo.Die = DIE::get(getState().DIEAlloc, dwarf::Tag(RefDie.getTag()));
  }
  NewRefDie = DieInfo.Die;

  if (AttrSpec.Form == dwarf::DW_FORM_ref_addr) {
    // Adding referenced DIE to DebugNames to be used when entries are created
    // that contain cross cu references.
    if (DebugNamesTable.canGenerateEntryWithCrossCUReference(U, Die, AttrSpec))
      DebugNamesTable.addCrossCUDie(&U, DieInfo.Die);
    // no matter forward reference or backward reference, we are supposed
    // to calculate them in `finish` due to the possible modification of
    // the DIE.
    DWARFDie CurDie = const_cast<DWARFDie &>(InputDIE);
    DIEInfo *CurDieInfo = &getDIEInfoByDwarfDie(CurDie);
    getState().AddrReferences.push_back(
        std::make_pair(CurDieInfo, AddrReferenceInfo(&DieInfo, AttrSpec)));

    Die.addValue(getState().DIEAlloc, AttrSpec.Attr, dwarf::DW_FORM_ref_addr,
                 DIEInteger(DieInfo.Die->getOffset()));
    return;
  }

  Die.addValue(getState().DIEAlloc, AttrSpec.Attr, AttrSpec.Form,
               DIEEntry(*NewRefDie));
}

void DIEBuilder::cloneStringAttribute(
    DIE &Die, const DWARFUnit &U,
    const DWARFAbbreviationDeclaration::AttributeSpec AttrSpec,
    const DWARFFormValue &Val) {
  if (AttrSpec.Form == dwarf::DW_FORM_string) {
    Expected<const char *> StrAddr = Val.getAsCString();
    if (!StrAddr) {
      consumeError(StrAddr.takeError());
      return;
    }
    Die.addValue(getState().DIEAlloc, AttrSpec.Attr, dwarf::DW_FORM_string,
                 new (getState().DIEAlloc)
                     DIEInlineString(StrAddr.get(), getState().DIEAlloc));
  } else {
    std::optional<uint64_t> OffsetIndex = Val.getRawUValue();
    Die.addValue(getState().DIEAlloc, AttrSpec.Attr, AttrSpec.Form,
                 DIEInteger(*OffsetIndex));
  }
}

Expected<bool>
DIEBuilder::rewriteExpressionReferences(ArrayRef<uint8_t> Input, DWARFUnit &U,
                                        SmallVectorImpl<uint8_t> &Output,
                                        ExpressionStage Stage) {
  using Encoding = DWARFExpression::Operation::Encoding;
  struct Branch {
    uint64_t Target;
    uint64_t Offset;
  };
  const unsigned OffsetSize = dwarf::getDwarfOffsetByteSize(U.getFormat());
  const unsigned ReferenceSize = (OffsetSize * 8 + 6) / 7;
  bool BranchOverflow = false;
  auto appendULEB = [](SmallVectorImpl<uint8_t> &Buffer, uint64_t Value,
                       unsigned PadTo = 0) {
    uint8_t Bytes[10];
    const unsigned Size = encodeULEB128(Value, Bytes, PadTo);
    Buffer.append(Bytes, Bytes + Size);
  };

  // Each entry_value block has its own instruction boundaries and branch
  // destinations. Serialize children first, including their length fields,
  // before translating branches in the enclosing expression.
  auto rewrite = [&](auto &&Rewrite, ArrayRef<uint8_t> Bytes,
                     SmallVectorImpl<uint8_t> &Buffer,
                     unsigned Depth) -> Expected<bool> {
    if (Depth == 64)
      return createStringError("location expression nesting is too deep");
    DataExtractor Data(Bytes, U.isLittleEndian());
    DWARFExpression Expression(Data, U.getAddressByteSize(), U.getFormat());
    SmallDenseMap<uint64_t, uint64_t, 16> Offsets;
    SmallVector<Branch, 2> Branches;
    bool HasReference = false;
    for (auto It = Expression.begin(), End = Expression.end(); It != End;) {
      const DWARFExpression::Operation &Op = *It;
      const uint64_t OpOffset = It.getOffset();
      const uint64_t OpEnd = Op.getEndOffset();
      if (Op.isError())
        return createStringError("cannot decode DW_OP at offset 0x%llx",
                                 static_cast<unsigned long long>(OpOffset));
      if (OpEnd <= OpOffset || OpEnd > Bytes.size())
        return createStringError("invalid location expression operand");
      // The expression decoder does not propagate every DataExtractor error.
      // Check that each operand was consumed, and opaque blocks stay in bounds.
      uint64_t OperandStart = OpOffset + 1;
      const auto &Encodings = Op.getDescription().Op;
      for (unsigned I = 0; I < Encodings.size(); ++I) {
        const uint64_t OperandEnd = Op.getOperandEndOffset(I);
        if (OperandEnd < OperandStart || OperandEnd > Bytes.size() ||
            (OperandEnd == OperandStart && Encodings[I] != Encoding::SizeBlock))
          return createStringError("invalid location expression operand");
        OperandStart = OperandEnd;
      }

      Offsets[OpOffset] = Buffer.size();
      if (Op.getCode() == dwarf::DW_OP_entry_value ||
          Op.getCode() == dwarf::DW_OP_GNU_entry_value) {
        const uint64_t Size = Op.getRawOperand(0);
        if (Size > Bytes.size() - OpEnd)
          return createStringError("entry_value block exceeds its expression");
        SmallVector<uint8_t, 32> Nested;
        Expected<bool> HasNestedReference =
            Rewrite(Rewrite, Bytes.slice(OpEnd, Size), Nested, Depth + 1);
        if (!HasNestedReference)
          return HasNestedReference.takeError();
        HasReference |= *HasNestedReference;
        Buffer.push_back(Op.getCode());
        appendULEB(Buffer, Nested.size());
        Buffer.append(Nested);
        It = It.skipBytes(Size);
        continue;
      }

      if (Op.getCode() == dwarf::DW_OP_skip ||
          Op.getCode() == dwarf::DW_OP_bra) {
        const int16_t Displacement = Op.getRawOperand(0);
        if ((Displacement >= 0 &&
             uint64_t(Displacement) > Bytes.size() - OpEnd) ||
            (Displacement < 0 && uint64_t(-int64_t(Displacement)) > OpEnd))
          return createStringError("branch target is outside its expression");
        Branches.push_back({OpEnd + Displacement, Buffer.size()});
      }

      Buffer.push_back(Op.getCode());
      OperandStart = OpOffset + 1;
      for (unsigned I = 0; I < Encodings.size(); ++I) {
        const uint64_t OperandEnd = Op.getOperandEndOffset(I);
        const uint64_t Value = Op.getRawOperand(I);
        if (Encodings[I] == Encoding::BaseTypeRef &&
            (Value != 0 || Op.getCode() != dwarf::DW_OP_convert)) {
          if (OffsetSize == 4 && Value > UINT32_MAX)
            return createStringError("base type reference exceeds DWARF32");
          HasReference = true;
          uint64_t Offset = Value;
          if (Stage == ExpressionStage::Patch) {
            const std::optional<uint32_t> UnitID = getUnitId(U);
            const std::optional<uint32_t> DieID =
                Value < U.getNextUnitOffset() - U.getOffset()
                    ? getAllocDIEId(U, U.getOffset() + Value)
                    : std::nullopt;
            DIE *Target =
                UnitID && DieID ? getDIEInfo(*UnitID, *DieID).Die : nullptr;
            if (Target && Target->getTag() == dwarf::DW_TAG_base_type)
              Offset = Target->getOffset();
            else
              BC.errs()
                  << "BOLT-WARNING: [internal-dwarf-error]: cannot resolve "
                     "base type reference 0x"
                  << Twine::utohexstr(Value) << " in CU at 0x"
                  << Twine::utohexstr(U.getOffset())
                  << "; preserving original offset\n";
          }
          appendULEB(Buffer, Offset, ReferenceSize);
        } else if (I == 0 && (Op.getCode() == dwarf::DW_OP_addrx ||
                              Op.getCode() == dwarf::DW_OP_GNU_addr_index)) {
          // The inline address-index rewrite runs between these two stages.
          // Reserve its full index width too, so it can preserve the layout of
          // expressions containing both address indices and DIE references.
          if (Value > UINT32_MAX)
            return createStringError("address index exceeds 32 bits");
          appendULEB(Buffer, Value, 5);
        } else {
          const ArrayRef<uint8_t> Operand =
              Bytes.slice(OperandStart, OperandEnd - OperandStart);
          Buffer.append(Operand.begin(), Operand.end());
        }
        OperandStart = OperandEnd;
      }
      ++It;
    }
    Offsets[Bytes.size()] = Buffer.size();
    for (const Branch &B : Branches) {
      auto Target = Offsets.find(B.Target);
      if (Target == Offsets.end())
        return createStringError(
            "branch target is not an instruction boundary");
      const int64_t Displacement =
          int64_t(Target->second) - int64_t(B.Offset + 3);
      if (!isInt<16>(Displacement)) {
        // Expressions without type references retain their original bytes.
        // Only reject this candidate layout if it will actually be used.
        BranchOverflow = true;
        continue;
      }
      support::endian::write16(Buffer.data() + B.Offset + 1,
                               static_cast<uint16_t>(Displacement),
                               U.isLittleEndian() ? llvm::endianness::little
                                                  : llvm::endianness::big);
    }
    return HasReference;
  };

  SmallVector<uint8_t, 32> Buffer;
  Expected<bool> HasReference = rewrite(rewrite, Input, Buffer, 0);
  if (!HasReference)
    return HasReference.takeError();
  if (*HasReference && BranchOverflow)
    return createStringError("rewritten branch displacement exceeds 16 bits");
  if (*HasReference)
    Output.append(Buffer);
  else
    Output.append(Input.begin(), Input.end());
  return *HasReference;
}

void DIEBuilder::cloneBlockAttribute(
    DIE &Die, DWARFUnit &U,
    const DWARFAbbreviationDeclaration::AttributeSpec AttrSpec,
    const DWARFFormValue &Val) {
  DIEValueList *Attr;
  DIEValue Value;
  DIELoc *Loc = nullptr;
  DIEBlock *Block = nullptr;

  if (AttrSpec.Form == dwarf::DW_FORM_exprloc) {
    Loc = new (getState().DIEAlloc) DIELoc;
  } else if (doesFormBelongToClass(AttrSpec.Form, DWARFFormValue::FC_Block,
                                   U.getVersion())) {
    Block = new (getState().DIEAlloc) DIEBlock;
  } else {
    BC.errs()
        << "BOLT-WARNING: [internal-dwarf-error]: Unexpected Form value in "
           "cloneBlockAttribute\n";
    return;
  }
  Attr = Loc ? static_cast<DIEValueList *>(Loc)
             : static_cast<DIEValueList *>(Block);

  SmallVector<uint8_t, 32> Buffer;
  ArrayRef<uint8_t> Bytes = *Val.getAsBlock();
  if (DWARFAttribute::mayHaveLocationExpr(AttrSpec.Attr) &&
      (Val.isFormClass(DWARFFormValue::FC_Block) ||
       Val.isFormClass(DWARFFormValue::FC_Exprloc))) {
    Expected<bool> HasReference =
        rewriteExpressionReferences(Bytes, U, Buffer, ExpressionStage::Prepare);
    if (!HasReference) {
      BC.errs()
          << "BOLT-WARNING: [internal-dwarf-error]: cannot rewrite location "
             "expression: "
          << toString(HasReference.takeError())
          << "; preserving original expression\n";
    } else {
      if (*HasReference)
        getState().LocWithReferencesToProcess.emplace_back(
            U, Die, AttrSpec.Form, AttrSpec.Attr);
      Bytes = Buffer;
    }
  }
  for (auto Byte : Bytes)
    Attr->addValue(getState().DIEAlloc, static_cast<dwarf::Attribute>(0),
                   dwarf::DW_FORM_data1, DIEInteger(Byte));

  if (Loc)
    Loc->setSize(Bytes.size());
  else
    Block->setSize(Bytes.size());

  if (Loc)
    Value = DIEValue(dwarf::Attribute(AttrSpec.Attr),
                     dwarf::Form(AttrSpec.Form), Loc);
  else
    Value = DIEValue(dwarf::Attribute(AttrSpec.Attr),
                     dwarf::Form(AttrSpec.Form), Block);
  Die.addValue(getState().DIEAlloc, Value);
}

void DIEBuilder::cloneAddressAttribute(
    DIE &Die, const DWARFUnit &U,
    const DWARFAbbreviationDeclaration::AttributeSpec AttrSpec,
    const DWARFFormValue &Val) {
  Die.addValue(getState().DIEAlloc, AttrSpec.Attr, AttrSpec.Form,
               DIEInteger(Val.getRawUValue()));
}

void DIEBuilder::cloneRefsigAttribute(
    DIE &Die, DWARFAbbreviationDeclaration::AttributeSpec AttrSpec,
    const DWARFFormValue &Val) {
  const std::optional<uint64_t> SigVal = Val.getAsSignatureReference();
  Die.addValue(getState().DIEAlloc, AttrSpec.Attr, dwarf::DW_FORM_ref_sig8,
               DIEInteger(*SigVal));
}

void DIEBuilder::cloneScalarAttribute(
    DIE &Die, const DWARFDie &InputDIE,
    const DWARFAbbreviationDeclaration::AttributeSpec AttrSpec,
    const DWARFFormValue &Val) {
  uint64_t Value;

  if (auto OptionalValue = Val.getAsUnsignedConstant())
    Value = *OptionalValue;
  else if (auto OptionalValue = Val.getAsSignedConstant())
    Value = *OptionalValue;
  else if (auto OptionalValue = Val.getAsSectionOffset())
    Value = *OptionalValue;
  else {
    BC.errs() << "BOLT-WARNING: [internal-dwarf-error]: Unsupported scalar "
                 "attribute form. Dropping "
                 "attribute.\n";
    return;
  }

  Die.addValue(getState().DIEAlloc, AttrSpec.Attr, AttrSpec.Form,
               DIEInteger(Value));
}

void DIEBuilder::cloneLoclistAttrubute(
    DIE &Die, const DWARFDie &InputDIE,
    const DWARFAbbreviationDeclaration::AttributeSpec AttrSpec,
    const DWARFFormValue &Val) {
  std::optional<uint64_t> Value = std::nullopt;

  if (auto OptionalValue = Val.getAsUnsignedConstant())
    Value = OptionalValue;
  else if (auto OptionalValue = Val.getAsSignedConstant())
    Value = OptionalValue;
  else if (auto OptionalValue = Val.getAsSectionOffset())
    Value = OptionalValue;
  else
    BC.errs() << "BOLT-WARNING: [internal-dwarf-error]: Unsupported scalar "
                 "attribute form. Dropping "
                 "attribute.\n";

  if (!Value.has_value())
    return;

  Die.addValue(getState().DIEAlloc, AttrSpec.Attr, AttrSpec.Form,
               DIELocList(*Value));
}

void DIEBuilder::cloneAttribute(
    DIE &Die, const DWARFDie &InputDIE, DWARFUnit &U, const DWARFFormValue &Val,
    const DWARFAbbreviationDeclaration::AttributeSpec AttrSpec) {
  switch (AttrSpec.Form) {
  case dwarf::DW_FORM_strp:
  case dwarf::DW_FORM_string:
  case dwarf::DW_FORM_strx:
  case dwarf::DW_FORM_strx1:
  case dwarf::DW_FORM_strx2:
  case dwarf::DW_FORM_strx3:
  case dwarf::DW_FORM_strx4:
  case dwarf::DW_FORM_GNU_str_index:
  case dwarf::DW_FORM_line_strp:
    cloneStringAttribute(Die, U, AttrSpec, Val);
    break;
  case dwarf::DW_FORM_ref_addr:
    cloneDieOffsetReferenceAttribute(Die, U, InputDIE, AttrSpec,
                                     *Val.getAsDebugInfoReference());
    break;
  case dwarf::DW_FORM_ref1:
  case dwarf::DW_FORM_ref2:
  case dwarf::DW_FORM_ref4:
  case dwarf::DW_FORM_ref8:
  case dwarf::DW_FORM_ref_udata:
    cloneDieOffsetReferenceAttribute(Die, U, InputDIE, AttrSpec,
                                     Val.getUnit()->getOffset() +
                                         *Val.getAsRelativeReference());
    break;
  case dwarf::DW_FORM_block:
  case dwarf::DW_FORM_block1:
  case dwarf::DW_FORM_block2:
  case dwarf::DW_FORM_block4:
  case dwarf::DW_FORM_exprloc:
    cloneBlockAttribute(Die, U, AttrSpec, Val);
    break;
  case dwarf::DW_FORM_addr:
  case dwarf::DW_FORM_addrx:
  case dwarf::DW_FORM_GNU_addr_index:
    cloneAddressAttribute(Die, U, AttrSpec, Val);
    break;
  case dwarf::DW_FORM_data1:
  case dwarf::DW_FORM_data2:
  case dwarf::DW_FORM_data4:
  case dwarf::DW_FORM_data8:
  case dwarf::DW_FORM_udata:
  case dwarf::DW_FORM_sdata:
  case dwarf::DW_FORM_sec_offset:
  case dwarf::DW_FORM_rnglistx:
  case dwarf::DW_FORM_flag:
  case dwarf::DW_FORM_flag_present:
  case dwarf::DW_FORM_implicit_const:
    cloneScalarAttribute(Die, InputDIE, AttrSpec, Val);
    break;
  case dwarf::DW_FORM_loclistx:
    cloneLoclistAttrubute(Die, InputDIE, AttrSpec, Val);
    break;
  case dwarf::DW_FORM_ref_sig8:
    cloneRefsigAttribute(Die, AttrSpec, Val);
    break;
  default:
    BC.errs() << "BOLT-WARNING: [internal-dwarf-error]: Unsupported attribute "
                 "form " +
                     dwarf::FormEncodingString(AttrSpec.Form).str() +
                     " in cloneAttribute. Dropping.";
  }
}
void DIEBuilder::assignAbbrev(DIEAbbrev &Abbrev) {
  // Check the set for priors.
  FoldingSetNodeID ID;
  Abbrev.Profile(ID);
  FoldingSetInsertToken Token;
  DIEAbbrev *InSet = AbbreviationsSet.lookup(ID, Token);

  // If it's newly added.
  if (InSet) {
    // Assign existing abbreviation number.
    Abbrev.setNumber(InSet->getNumber());
  } else {
    // Add to abbreviation list.
    Abbreviations.push_back(
        std::make_unique<DIEAbbrev>(Abbrev.getTag(), Abbrev.hasChildren()));
    for (const auto &Attr : Abbrev.getData())
      Abbreviations.back()->AddAttribute(Attr);
    AbbreviationsSet.insert(Abbreviations.back().get(), Token);
    // Assign the unique abbreviation number.
    Abbrev.setNumber(Abbreviations.size());
    Abbreviations.back()->setNumber(Abbreviations.size());
  }
}

void DIEBuilder::generateAbbrevs() {
  if (isEmpty())
    return;

  for (DWARFUnit *DU : getState().DUList) {
    DIE *UnitDIE = getUnitDIEbyUnit(*DU);
    generateUnitAbbrevs(UnitDIE);
  }
}

void DIEBuilder::generateUnitAbbrevs(DIE *Die) {
  DIEAbbrev NewAbbrev = Die->generateAbbrev();

  if (Die->hasChildren())
    NewAbbrev.setChildrenFlag(dwarf::DW_CHILDREN_yes);
  assignAbbrev(NewAbbrev);
  Die->setAbbrevNumber(NewAbbrev.getNumber());

  for (auto &Child : Die->children()) {
    generateUnitAbbrevs(&Child);
  }
}

static uint64_t getHash(const DWARFUnit &DU) {
  // Before DWARF5 TU units are in their own section, so at least one offset,
  // first one, will be the same as CUs in .debug_info.dwo section
  if (DU.getVersion() < 5 && DU.isTypeUnit()) {
    const uint64_t TypeUnitHash =
        cast_or_null<DWARFTypeUnit>(&DU)->getTypeHash();
    const uint64_t Offset = DU.getOffset();
    return llvm::hash_combine(llvm::hash_value(TypeUnitHash),
                              llvm::hash_value(Offset));
  }
  return DU.getOffset();
}

bool DIEBuilder::registerUnit(DWARFUnit &DU, bool NeedSort) {
  if (!BC.isValidDwarfUnit(DU))
    return false;
  auto IterGlobal = AllProcessed.insert(getHash(DU));
  // If DU is already in a current working set or was already processed we can
  // skip it.
  if (!IterGlobal.second)
    return true;
  if (getState().Type == ProcessingType::DWARF4TUs) {
    getState().DWARF4TUVector.push_back(&DU);
  } else if (getState().Type == ProcessingType::DWARF5TUs) {
    getState().DWARF5TUVector.push_back(&DU);
  } else {
    getState().DWARFCUVector.push_back(&DU);
    /// Sorting for cross CU reference resolution.
    if (NeedSort)
      std::sort(getState().DWARFCUVector.begin(),
                getState().DWARFCUVector.end(),
                [](const DWARFUnit *A, const DWARFUnit *B) {
                  return A->getOffset() < B->getOffset();
                });
  }
  getState().UnitIDMap[getHash(DU)] = getState().DUList.size();
  // This handles the case where we do have cross cu references, but CUs do not
  // share the same abbrev table.
  if (getState().DUList.size() == getState().CloneUnitCtxMap.size())
    getState().CloneUnitCtxMap.emplace_back();
  getState().DUList.push_back(&DU);
  return true;
}

std::optional<uint32_t> DIEBuilder::getUnitId(const DWARFUnit &DU) {
  auto Iter = getState().UnitIDMap.find(getHash(DU));
  if (Iter != getState().UnitIDMap.end())
    return Iter->second;
  return std::nullopt;
}

} // namespace bolt
} // namespace llvm
