//===- SILLowerAggregateInstrs.cpp - Aggregate insts to Scalar insts  -----===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Simplify aggregate instructions into scalar instructions.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-lower-aggregate-instrs"
#include "swift/SILPasses/Passes.h"
#include "swift/SILPasses/Transforms.h"
#include "swift/SILPasses/PassManager.h"
#include "swift/SILPasses/Utils/Local.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILModule.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Debug.h"
using namespace swift;
using namespace swift::Lowering;

STATISTIC(NumExpand, "Number of instructions expanded");

//===----------------------------------------------------------------------===//
//                      Higher Level Operation Expansion
//===----------------------------------------------------------------------===//

/// \brief Lower copy_addr into loads/stores/retain/release if we have a
/// non-address only type. We do this here so we can process the resulting
/// loads/stores.
///
/// This peephole implements the following optimizations:
///
/// copy_addr %0 to %1 : $*T
/// ->
///     %new = load %0 : $*T        // Load the new value from the source
///     %old = load %1 : $*T        // Load the old value from the destination
///     strong_retain %new : $T     // Retain the new value
///     strong_release %old : $T    // Release the old
///     store %new to %1 : $*T      // Store the new value to the destination
///
/// copy_addr [take] %0 to %1 : $*T
/// ->
///     %new = load %0 : $*T
///     %old = load %1 : $*T
///     // no retain of %new!
///     strong_release %old : $T
///     store %new to %1 : $*T
///
/// copy_addr %0 to [initialization] %1 : $*T
/// ->
///     %new = load %0 : $*T
///     strong_retain %new : $T
///     // no load/release of %old!
///     store %new to %1 : $*T
///
/// copy_addr [take] %0 to [initialization] %1 : $*T
/// ->
///     %new = load %0 : $*T
///     // no retain of %new!
///     // no load/release of %old!
///     store %new to %1 : $*T
static bool expandCopyAddr(CopyAddrInst *CA) {
  SILModule &M = CA->getModule();
  SILValue Source = CA->getSrc();

  // If we have an address only type don't do anything.
  SILType SrcType = Source.getType();
  if (SrcType.isAddressOnly(M))
    return false;

  SILBuilder Builder(CA);

  // %new = load %0 : $*T
  LoadInst *New = Builder.createLoad(CA->getLoc(), Source);

  SILValue Destination = CA->getDest();

  // If our object type is not trivial, we may need to release the old value and
  // retain the new one.

  auto &TL = M.getTypeLowering(SrcType);

  // If we have a non-trivial type...
  if (!TL.isTrivial()) {

    // If we are not initializing:
    // %old = load %1 : $*T
    IsInitialization_t IsInit = CA->isInitializationOfDest();
    LoadInst *Old = nullptr;
    if (IsInitialization_t::IsNotInitialization == IsInit) {
      Old = Builder.createLoad(CA->getLoc(), Destination);
    }

    // If we are not taking and have a reference type:
    //   strong_retain %new : $*T
    // or if we have a non-trivial non-reference type.
    //   copy_value %new : $*T
    IsTake_t IsTake = CA->isTakeOfSrc();
    if (IsTake_t::IsNotTake == IsTake) {
      TL.emitLoweredCopyValue(Builder, CA->getLoc(), New,
                              TypeLowering::LoweringStyle::DeepNoEnum);
    }

    // If we are not initializing:
    // strong_release %old : $*T
    //   *or*
    // destroy_value %new : $*T
    if (Old) {
      TL.emitLoweredDestroyValue(Builder, CA->getLoc(), Old,
                                 TypeLowering::LoweringStyle::DeepNoEnum);
    }
  }

  // Create the store.
  Builder.createStore(CA->getLoc(), New, Destination);

  ++NumExpand;
  return true;
}

static bool expandDestroyAddr(DestroyAddrInst *DA) {
  SILModule &Module = DA->getModule();
  SILBuilder Builder(DA);

  // Strength reduce destroy_addr inst into release/store if
  // we have a non-address only type.
  SILValue Addr = DA->getOperand();

  // If we have an address only type, do nothing.
  SILType Type = Addr.getType();
  if (Type.isAddressOnly(Module))
    return false;

  // If we have a non-trivial type...
  if (!Type.isTrivial(Module)) {
    // If we have a type with reference semantics, emit a load/strong release.
    LoadInst *LI = Builder.createLoad(DA->getLoc(), Addr);
    auto &TL = Module.getTypeLowering(Type);
    TL.emitLoweredDestroyValue(Builder, DA->getLoc(), LI,
                               TypeLowering::LoweringStyle::DeepNoEnum);
  }

  ++NumExpand;
  return true;
}

static bool expandDestroyValue(DestroyValueInst *DV) {
  SILModule &Module = DV->getModule();
  SILBuilder Builder(DV);

  // Strength reduce destroy_addr inst into release/store if
  // we have a non-address only type.
  SILValue Value = DV->getOperand();

  // If we have an address only type, do nothing.
  SILType Type = Value.getType();
  assert(Type.isLoadable(Module) &&
         "destroy_value should never be called on a non-loadable type.");

  auto &TL = Module.getTypeLowering(Type);
  TL.emitLoweredDestroyValue(Builder, DV->getLoc(), Value,
                             TypeLowering::LoweringStyle::DeepNoEnum);

  DEBUG(llvm::dbgs() << "    Expanding Destroy Value: " << *DV);

  ++NumExpand;
  return true;
}

static bool expandCopyValue(CopyValueInst *CV) {
  SILModule &Module = CV->getModule();
  SILBuilder Builder(CV);

  // Strength reduce destroy_addr inst into release/store if
  // we have a non-address only type.
  SILValue Value = CV->getOperand();

  // If we have an address only type, do nothing.
  SILType Type = Value.getType();
  assert(Type.isLoadable(Module) && "Copy Value can only be called on loadable "
         "types.");

  auto &TL = Module.getTypeLowering(Type);
  SILValue Result =
    TL.emitLoweredCopyValue(Builder, CV->getLoc(), Value,
                            TypeLowering::LoweringStyle::DeepNoEnum);
  SILValue(CV, 0).replaceAllUsesWith(Result);

  DEBUG(llvm::dbgs() << "    Expanding Copy Value: " << *CV);

  ++NumExpand;
  return true;
}

//===----------------------------------------------------------------------===//
//                              Top Level Driver
//===----------------------------------------------------------------------===//

static void processFunction(SILFunction &Fn) {
  for (auto BI = Fn.begin(), BE = Fn.end(); BI != BE; ++BI) {
    auto II = BI->begin(), IE = BI->end();
    while (II != IE) {
      SILInstruction *Inst = &*II;

      DEBUG(llvm::dbgs() << "Visiting: " << *Inst);

      if (auto *CA = dyn_cast<CopyAddrInst>(Inst))
        if (expandCopyAddr(CA)) {
          ++II;
          CA->eraseFromParent();
          continue;
        }

      if (auto *DA = dyn_cast<DestroyAddrInst>(Inst))
        if (expandDestroyAddr(DA)) {
          ++II;
          DA->eraseFromParent();
          continue;
        }

      if (auto *CV = dyn_cast<CopyValueInst>(Inst))
        if (expandCopyValue(CV)) {
          ++II;
          CV->eraseFromParent();
          continue;
        }

      if (auto *DV = dyn_cast<DestroyValueInst>(Inst))
        if (expandDestroyValue(DV)) {
          ++II;
          DV->eraseFromParent();
          continue;
        }

      ++II;
    }
  }
}

void swift::performSILLowerAggregateInstrs(SILModule *M) {

  DEBUG(llvm::dbgs() << "*** SIL LowerAggregateInstrs ***\n");

  // For each function Fn in M...
  for (auto &Fn : *M) {

    // If Fn has no basic blocks skip it.
    if (Fn.empty())
      continue;

    DEBUG(llvm::dbgs() << "***** Visiting " << Fn.getName() << " *****\n");

    // Otherwise perform LowerAggregateInstrs.
    processFunction(Fn);
  }
}

class SILLowerAggregate : public SILFunctionTrans {
  virtual ~SILLowerAggregate() {}

  /// The entry point to the transformation.
  virtual void runOnFunction(SILFunction &F, SILPassManager *PM) {
    DEBUG(llvm::dbgs() << "***** LowerAggregate on function: " <<
          F.getName() << " *****\n");
    processFunction(F);
    PM->invalidateAllAnalysis(&F, SILAnalysis::InvalidationKind::Instructions);
  }
};

SILTransform *swift::createLowerAggregate() {
  return new SILLowerAggregate();
}
