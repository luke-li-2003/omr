/*******************************************************************************
 * Copyright IBM Corp. and others 2000
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at https://www.eclipse.org/legal/epl-2.0/
 * or the Apache License, Version 2.0 which accompanies this distribution
 * and is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following Secondary
 * Licenses when the conditions for such availability set forth in the
 * Eclipse Public License, v. 2.0 are satisfied: GNU General Public License,
 * version 2 with the GNU Classpath Exception [1] and GNU General Public
 * License, version 2 with the OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] https://openjdk.org/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0-only WITH Classpath-exception-2.0 OR GPL-2.0-only WITH OpenJDK-assembly-exception-1.0
 *******************************************************************************/

#include "x/codegen/CompareAnalyser.hpp"

#include <stddef.h>
#include <stdint.h>
#include "codegen/Analyser.hpp"
#include "codegen/CodeGenerator.hpp"
#include "env/FrontEnd.hpp"
#include "codegen/LiveRegister.hpp"
#include "codegen/Machine.hpp"
#include "codegen/MemoryReference.hpp"
#include "codegen/RealRegister.hpp"
#include "codegen/Register.hpp"
#include "codegen/RegisterConstants.hpp"
#include "codegen/RegisterDependency.hpp"
#include "codegen/TreeEvaluator.hpp"
#include "compile/Compilation.hpp"
#include "control/Options.hpp"
#include "control/Options_inlines.hpp"
#include "env/TRMemory.hpp"
#include "il/ILOpCodes.hpp"
#include "il/ILOps.hpp"
#include "il/LabelSymbol.hpp"
#include "il/Node.hpp"
#include "il/Node_inlines.hpp"
#include "il/TreeTop.hpp"
#include "il/TreeTop_inlines.hpp"
#include "infra/Assert.hpp"
#include "infra/List.hpp"
#include "ras/Debug.hpp"
#include "runtime/Runtime.hpp"
#include "codegen/X86Instruction.hpp"
#include "codegen/InstOpCode.hpp"

static bool addressIsTemporarilyInt(TR::Node *node)
   {
   return (node->getOpCodeValue() == TR::a2i || node->getOpCodeValue() == TR::a2l) && (node->getReferenceCount() == 1);
   }


void TR_X86CompareAnalyser::integerCompareAnalyser(
   TR::Node       *root,
   TR::Node       *firstChild,
   TR::Node       *secondChild,
   bool           determineEvaluationOrder,
   TR::InstOpCode::Mnemonic  regRegOpCode,
   TR::InstOpCode::Mnemonic  regMemOpCode,
   TR::InstOpCode::Mnemonic  memRegOpCode)
   {
   TR::Node *realFirstChild = NULL;
   TR::Node *realSecondChild = NULL;

   if (addressIsTemporarilyInt(firstChild))
      {
      realFirstChild = firstChild;
      firstChild = firstChild->getFirstChild();
      }

   if (addressIsTemporarilyInt(secondChild))
      {
      realSecondChild = secondChild;
      secondChild = secondChild->getFirstChild();
      }

   TR::Register *firstRegister  = firstChild->getRegister();
   TR::Register *secondRegister = secondChild->getRegister();

   setInputs(firstChild, firstRegister, secondChild, secondRegister, true);

   // Do not allow direct memory forms for spine checks.  This ensures the children
   // will be available in registers or as constants.
   //
   if (root->getOpCode().isSpineCheck())
      {
      resetMem1();
      resetMem2();
      }

   printf("WCTEa %p\n", root);
   if ((determineEvaluationOrder && (cg()->whichChildToEvaluate(root) == 0)) || !determineEvaluationOrder)
      {
      if (getEvalChild1())
         {
         firstRegister = cg()->evaluate(firstChild);
         }
      if (getEvalChild2())
         {
         secondRegister = cg()->evaluate(secondChild);
         }
      }
   else
      {
      if (getEvalChild2())
         {
         secondRegister = cg()->evaluate(secondChild);
         }
      if (getEvalChild1())
         {
         firstRegister = cg()->evaluate(firstChild);
         }
      }

   if (getCmpReg1Reg2())
      {
      generateRegRegInstruction(regRegOpCode, root, firstRegister, secondRegister, cg());
      }
   else if (getCmpReg1Mem2())
      {
      TR::MemoryReference *tempMR = generateX86MemoryReference(secondChild, cg());

      TR::X86RegMemInstruction *inst = generateRegMemInstruction(regMemOpCode, root, firstRegister, tempMR, cg());

      if(!cg()->getImplicitExceptionPoint())
         cg()->setImplicitExceptionPoint(inst);
      tempMR->decNodeReferenceCounts(cg());
      }
   else // Must be CmpMem1Reg2
      {
      TR::MemoryReference *tempMR = generateX86MemoryReference(firstChild, cg());

      TR::X86MemRegInstruction *inst=generateMemRegInstruction(memRegOpCode, root, tempMR, secondRegister, cg());
      if(!cg()->getImplicitExceptionPoint())
         cg()->setImplicitExceptionPoint(inst);
      tempMR->decNodeReferenceCounts(cg());
      }

   if (realFirstChild)
      {
      if (!getCmpMem1Reg2())
         {
         cg()->recursivelyDecReferenceCount(realFirstChild);
         }
      else
         {
         cg()->decReferenceCount(realFirstChild);
         }
      }
   else
      {
      cg()->decReferenceCount(firstChild);
      }

   if (realSecondChild)
      {
      if (!getCmpReg1Mem2())
         {
         cg()->recursivelyDecReferenceCount(realSecondChild);
         }
      else
         {
         cg()->decReferenceCount(realSecondChild);
         }
      }
   else
      {
      cg()->decReferenceCount(secondChild);
      }
   }

void TR_X86CompareAnalyser::integerCompareAnalyser(
   TR::Node       *root,
   TR::InstOpCode::Mnemonic  regRegOpCode,
   TR::InstOpCode::Mnemonic  regMemOpCode,
   TR::InstOpCode::Mnemonic  memRegOpCode)
   {
   integerCompareAnalyser(
      root,
      root->getFirstChild(),
      root->getSecondChild(),
      true,
      regRegOpCode, regMemOpCode, memRegOpCode);
   }


// Evaluate su2l, c2l and bu2l nodes (that have not been evaluated and have
// reference counts of 1) as su2i, c2i and bu2i, respectively; this saves us
// from allocating a useless high register.
//
static TR::Register *optimizeIU2L(TR::Node *child, TR::ILOpCodes origOp, TR::CodeGenerator *cg)
   {
   TR::Register  *reg = NULL;
   TR::ILOpCodes  op;
   switch (origOp)
      {
      case TR::su2l: op = TR::su2i; break;
      case TR::bu2l: op = TR::bu2i; break;
      default:      op = TR::BadILOp;
      }
   if (op != TR::BadILOp)
      {
      TR::Node::recreate(child, op);
      reg = cg->evaluate(child);
      TR::Node::recreate(child, origOp);
      }
   else
      {
      reg = cg->evaluate(child);
      }
   return reg;
   }

void TR_X86CompareAnalyser::longOrderedCompareAndBranchAnalyser(TR::Node       *root,
                                                                 TR::InstOpCode::Mnemonic lowBranchOpCode,
                                                                 TR::InstOpCode::Mnemonic highBranchOpCode,
                                                                 TR::InstOpCode::Mnemonic highReversedBranchOpCode)
   {
   TR::Node     *firstChild     = root->getFirstChild();
   TR::Node     *secondChild    = root->getSecondChild();
   TR::Register *firstRegister  = firstChild->getRegister();
   TR::Register *secondRegister = secondChild->getRegister();
   TR::ILOpCodes firstOp        = firstChild->getOpCodeValue();
   TR::ILOpCodes secondOp       = secondChild->getOpCodeValue();
   TR::Compilation* comp = cg()->comp();

   bool         firstIU2L      = false;
   bool         secondIU2L     = false;
   bool         useFirstHighOrder  = false;
   bool         useSecondHighOrder = false;
   bool         firstHighZero      = false;
   bool         secondHighZero     = false;

   // can generate better code for compares when one or more children are TR::iu2l
   // but only when we don't need the result of the iu2l for another parent.
   if (firstChild->isHighWordZero())
      {
      firstHighZero = true;
      if (firstChild->getReferenceCount() == 1 && firstRegister == 0)
         {
         if (firstOp == TR::su2l || firstOp == TR::bu2l)
            {
            firstIU2L = true;
            }
         else if (firstOp == TR::iu2l ||
             (firstOp == TR::lushr &&
              firstChild->getSecondChild()->getOpCodeValue() == TR::iconst &&
              (firstChild->getSecondChild()->getInt() & TR::TreeEvaluator::shiftMask(true)) == 32))
            {
            firstChild    = firstChild->getFirstChild();
            firstRegister = firstChild->getRegister();
            firstIU2L     = true;
            if (firstOp == TR::lushr)
               {
               if (firstRegister == NULL)
                  {
            	   firstRegister = cg()->evaluate(firstChild);
                  }
               /*
                The assumption below seems to be invalid.
                Apparently, it is possible for the first register (and, probably, the second one, also) to be NULL. For java code below
                if (end != 0
                    && logEnd >>> 32)
                    < end >>> 32))
                    {
                    logEnd = end;
				    }
			   we put end in a register pair and logEnd stays in memory.
               */
               //TR_ASSERT(firstRegister != NULL,"Can the first register be NULL in this case?");
               useFirstHighOrder = true;
               }
            }
         }
      }

   if (secondChild->isHighWordZero())
      {
      secondHighZero = true;
      if (secondChild->getReferenceCount() == 1 && secondRegister == 0)
         {
         if (secondOp == TR::su2l || secondOp == TR::bu2l)
            {
            secondIU2L = true;
            }
         else if (secondOp == TR::iu2l ||
             (secondOp == TR::lushr &&
              secondChild->getSecondChild()->getOpCodeValue() == TR::iconst &&
              (secondChild->getSecondChild()->getInt() & TR::TreeEvaluator::shiftMask(true)) == 32))
            {
            secondChild    = secondChild->getFirstChild();
            secondRegister = secondChild->getRegister();
            secondIU2L     = true;
            if (secondOp == TR::lushr)
               {
               if (secondRegister == NULL)
                  {
            	   secondRegister = cg()->evaluate(secondChild);
                  }
               //TR_ASSERT(secondRegister != NULL,"Can the second register be NULL in this case?");
               useSecondHighOrder = true;
               }
            }
         }
      }

   setInputs(firstChild, firstRegister, secondChild, secondRegister, true);

   TR::MemoryReference  *lowFirstMR    = NULL;
   TR::MemoryReference  *highFirstMR   = NULL;
   TR::MemoryReference  *lowSecondMR   = NULL;
   TR::MemoryReference  *highSecondMR  = NULL;
   TR::Register            *delayedFirst  = NULL;
   TR::Register            *delayedSecond = NULL;

   printf("WCTEb %p\n", root);
   if (cg()->whichChildToEvaluate(root) == 0)
      {
      if (getEvalChild1())
         {
         if (firstChild->getReferenceCount() == 1 &&
             firstChild->getRegister() == NULL    &&
             (firstChild->getOpCodeValue() == TR::lload ||
              (firstChild->getOpCodeValue() == TR::iload &&
               firstIU2L)))
            {
            lowFirstMR   = generateX86MemoryReference(firstChild, cg());
            delayedFirst = cg()->allocateRegister();
            if (!firstIU2L)
               {
               highFirstMR = generateX86MemoryReference(*lowFirstMR, 4, cg());
               generateRegMemInstruction(TR::InstOpCode::L4RegMem, firstChild, delayedFirst, highFirstMR, cg());
               }
            }
         else if (firstIU2L)
            {
            firstRegister = optimizeIU2L(firstChild, firstOp, cg());
            }
         else
            {
            firstRegister = cg()->evaluate(firstChild);
            }
         }
      if (getEvalChild2())
         {
         if (secondChild->getReferenceCount() == 1 &&
             secondChild->getRegister() == NULL    &&
             (secondChild->getOpCodeValue() == TR::lload ||
              (secondChild->getOpCodeValue() == TR::iload &&
               secondIU2L)))
            {
            lowSecondMR   = generateX86MemoryReference(secondChild, cg());
            delayedSecond = cg()->allocateRegister();
            if (!secondIU2L)
               {
               highSecondMR = generateX86MemoryReference(*lowSecondMR, 4, cg());
               generateRegMemInstruction(TR::InstOpCode::L4RegMem, secondChild, delayedSecond, highSecondMR, cg());
               }
            }
         else if (secondIU2L)
            {
            secondRegister = optimizeIU2L(secondChild, secondOp, cg());
            }
         else
            {
            secondRegister = cg()->evaluate(secondChild);
            }
         }
      }
   else
      {
      if (getEvalChild2())
         {
         if (secondChild->getReferenceCount() == 1 &&
             secondChild->getRegister() == NULL    &&
             (secondChild->getOpCodeValue() == TR::lload ||
              (secondChild->getOpCodeValue() == TR::iload &&
               secondIU2L)))
            {
            lowSecondMR   = generateX86MemoryReference(secondChild, cg());
            delayedSecond = cg()->allocateRegister();
            if (!secondIU2L)
               {
               highSecondMR = generateX86MemoryReference(*lowSecondMR, 4, cg());
               generateRegMemInstruction(TR::InstOpCode::L4RegMem, secondChild, delayedSecond, highSecondMR, cg());
               }
            }
         else
            {
            secondRegister = cg()->evaluate(secondChild);
            }
         }
      if (getEvalChild1())
         {
         if (firstChild->getReferenceCount() == 1 &&
             firstChild->getRegister() == NULL    &&
             (firstChild->getOpCodeValue() == TR::lload ||
              (firstChild->getOpCodeValue() == TR::iload &&
               firstIU2L)))
            {
            lowFirstMR   = generateX86MemoryReference(firstChild, cg());
            delayedFirst = cg()->allocateRegister();
            if (!firstIU2L)
               {
               highFirstMR = generateX86MemoryReference(*lowFirstMR, 4, cg());
               generateRegMemInstruction(TR::InstOpCode::L4RegMem, firstChild, delayedFirst, highFirstMR, cg());
               }
            }
         else
            {
            firstRegister = cg()->evaluate(firstChild);
            }
         }
      }

   if (firstHighZero && firstRegister && firstRegister->getRegisterPair())
      {
      if (!useFirstHighOrder)
         {
         firstRegister = firstRegister->getLowOrder();
         }
      else
         {
         firstRegister = firstRegister->getHighOrder();
         }
      }

   if (secondHighZero && secondRegister && secondRegister->getRegisterPair())
      {
      if (!useSecondHighOrder)
         {
         secondRegister = secondRegister->getLowOrder();
         }
      else
         {
         secondRegister = secondRegister->getHighOrder();
         }
      }

   TR::LabelSymbol                   *startLabel       = TR::LabelSymbol::create(cg()->trHeapMemory(),cg());
   TR::LabelSymbol                   *doneLabel        = TR::LabelSymbol::create(cg()->trHeapMemory(),cg());
   TR::LabelSymbol                   *destinationLabel = root->getBranchDestination()->getNode()->getLabel();
   TR::MemoryReference               *lowMR            = NULL;
   TR::MemoryReference               *highMR           = NULL;
   TR::RegisterDependencyConditions  *deps             = NULL;
   uint32_t                           numAdditionalRegDeps = 5;

   if (getCmpReg1Mem2())
      {
      lowMR  = generateX86MemoryReference(secondChild, cg());
      if (secondIU2L == false)
         highMR = generateX86MemoryReference(*lowMR, 4, cg());
      }
   else if (getCmpMem1Reg2())
      {
      lowMR  = generateX86MemoryReference(firstChild, cg());
      if (firstIU2L == false)
         highMR = generateX86MemoryReference(*lowMR, 4, cg());
      }

   bool hasGlobalDeps = (root->getNumChildren() == 3) ? true : false;

   if (hasGlobalDeps)
      {
      // This child must be evaluated last to ensure virtual regs
      // survive unclobbered up to the branch; all other evaluations (including
      // generateX86MemoryReference on a node) must preceed this.
      //
      TR::Node *third = root->getChild(2);
      cg()->evaluate(third);
      deps = generateRegisterDependencyConditions(third, cg(), numAdditionalRegDeps);
      }
   else
      {
      deps = generateRegisterDependencyConditions((uint8_t)0, (uint8_t)numAdditionalRegDeps, cg());
      }

   startLabel->setStartInternalControlFlow();
   doneLabel->setEndInternalControlFlow();
   generateLabelInstruction(TR::InstOpCode::label, root, startLabel, cg());

   if (getCmpReg1Reg2())
      {
      TR_ASSERT(delayedFirst == NULL && delayedSecond == NULL, "Bad combination in long ordered compare analyser\n");
      TR::Register *firstLow;
      if (firstHighZero == false)
         {
         deps->addPostCondition(firstRegister->getHighOrder(), TR::RealRegister::NoReg, cg());
         firstLow = firstRegister->getLowOrder();
         }
      else
         {
         firstLow = firstRegister;
         }
      deps->addPostCondition(firstLow, TR::RealRegister::NoReg, cg());
      TR::Register *secondLow;
      if (secondHighZero == false)
         {
         deps->addPostCondition(secondRegister->getHighOrder(), TR::RealRegister::NoReg, cg());
         secondLow = secondRegister->getLowOrder();
         }
      else
         {
         secondLow = secondRegister;
         }

      deps->addPostCondition(secondLow, TR::RealRegister::NoReg, cg());
      deps->stopAddingConditions();

      TR::InstOpCode::Mnemonic highBranchOp = highBranchOpCode;
      if (firstHighZero)
         {
         if (secondHighZero == false)   // if both are ui2l, then we just need an unsigned low order compare
            {
            generateRegImmInstruction(TR::InstOpCode::CMP4RegImms, root, secondRegister->getHighOrder(), 0, cg());
            highBranchOp = highReversedBranchOpCode;
            }
         }
      else if (secondHighZero)
         {
         generateRegImmInstruction(TR::InstOpCode::CMP4RegImms, root, firstRegister->getHighOrder(), 0, cg());
         }
      else
         {
         generateRegRegInstruction(TR::InstOpCode::CMP4RegReg, root, firstRegister->getHighOrder(), secondRegister->getHighOrder(), cg());
         }

      if (firstHighZero == false || secondHighZero == false)
         {
         if (hasGlobalDeps == false)
            {
            generateLabelInstruction(highBranchOp, root, destinationLabel, cg());
            generateLabelInstruction(TR::InstOpCode::JNE4, root, doneLabel, cg());
            }
         else
            {
            generateLabelInstruction(highBranchOp, root, destinationLabel, deps, cg());
            generateLabelInstruction(TR::InstOpCode::JNE4, root, doneLabel, deps, cg());
            }
         }
      generateRegRegInstruction(TR::InstOpCode::CMP4RegReg, root, firstLow, secondLow, cg());
      }
   else if (getCmpReg1Mem2())
      {
      TR::Register *firstLow;
      if (delayedFirst)
         {
         deps->addPostCondition(delayedFirst, TR::RealRegister::NoReg, cg());
         firstLow = NULL;
         }
      else if (firstHighZero == false)
         {
         deps->addPostCondition(firstRegister->getHighOrder(), TR::RealRegister::NoReg, cg());
         firstLow = firstRegister->getLowOrder();
         }
      else
         {
         firstLow = firstRegister;
         }
      if (firstLow)
         {
         deps->addPostCondition(firstLow, TR::RealRegister::NoReg, cg());
         }

      // Add register dependencies from the memory reference
      //
      TR::Register *depReg = NULL;
      while ((depReg = lowMR->getNextRegister(depReg)) != NULL)
         {
         // Don't assign NoReg dependencies to real registers.
         //
         if (!depReg->getRealRegister())
            deps->addPostCondition(depReg, TR::RealRegister::NoReg, cg());
         }
      deps->stopAddingConditions();

      TR::InstOpCode::Mnemonic highBranchOp = highBranchOpCode;
      if (firstHighZero)
         {
         if (secondIU2L == false)   // if both are ui2l, then we just need an unsigned low order compare
            {
            generateMemImmInstruction(TR::InstOpCode::CMP4MemImms, root, highMR, 0, cg());
            highBranchOp = highReversedBranchOpCode;
            }
         }
      else if (secondIU2L)
         {
         TR::Register *temp;
         if (delayedFirst)
            {
            temp = delayedFirst;
            }
         else
            {
            temp = firstRegister->getHighOrder();
            }
         generateRegImmInstruction(TR::InstOpCode::CMP4RegImms, root, temp, 0, cg());
         }
      else
         {
         TR::Register *temp;
         if (delayedFirst)
            {
            temp = delayedFirst;
            }
         else
            {
            temp = firstRegister->getHighOrder();
            }
         generateRegMemInstruction(TR::InstOpCode::CMP4RegMem, root, temp, highMR, cg());
         }

      if (firstHighZero == false || secondHighZero == false)
         {
         if (hasGlobalDeps == false)
            {
            generateLabelInstruction(highBranchOp, root, destinationLabel, cg());
            generateLabelInstruction(TR::InstOpCode::JNE4, root, doneLabel, cg());
            }
         else
            {
            generateLabelInstruction(highBranchOp, root, destinationLabel, deps, cg());
            generateLabelInstruction(TR::InstOpCode::JNE4, root, doneLabel, deps, cg());
            }
         }

      if (delayedFirst)
         {
         generateRegMemInstruction(TR::InstOpCode::L4RegMem, firstChild, delayedFirst, lowFirstMR, cg());
         firstLow = delayedFirst;
         }
      generateRegMemInstruction(TR::InstOpCode::CMP4RegMem, root, firstLow, lowMR, cg());
      }
   else // Must be CmpMem1Reg2
      {
      TR::Register *secondLow;
      if (delayedSecond)
         {
         deps->addPostCondition(delayedSecond, TR::RealRegister::NoReg, cg());
         secondLow = NULL;
         }
      else if (secondHighZero == false)
         {
         deps->addPostCondition(secondRegister->getHighOrder(), TR::RealRegister::NoReg, cg());
         secondLow = secondRegister->getLowOrder();
         }
      else
         {
         secondLow = secondRegister;
         }
      if (secondLow)
         {
         deps->addPostCondition(secondLow, TR::RealRegister::NoReg, cg());
         }

      // Add register dependencies from the memory reference
      //
      TR::Register *depReg = NULL;
      while ((depReg = lowMR->getNextRegister(depReg)) != NULL)
         {
         // Don't assign NoReg dependencies to real registers.
         //
         if (!depReg->getRealRegister())
            deps->addPostCondition(depReg, TR::RealRegister::NoReg, cg());
         }
      deps->stopAddingConditions();

      TR::InstOpCode::Mnemonic highBranchOp = highBranchOpCode;
      if (firstIU2L)
         {
         if (secondHighZero == false)   // if both are ui2l, then we just need an unsigned low order compare
            {
            generateRegImmInstruction(TR::InstOpCode::CMP4RegImms, root, secondRegister->getHighOrder(), 0, cg());
            highBranchOp = highReversedBranchOpCode;
            }
         }
      else if (secondHighZero)
         {
         generateMemImmInstruction(TR::InstOpCode::CMP4MemImms, root, highMR, 0, cg());
         }
      else
         {
         TR::Register *temp;
         if (delayedSecond)
            {
            temp = delayedSecond;
            }
         else
            {
            temp = secondRegister->getHighOrder();
            }
         generateMemRegInstruction(TR::InstOpCode::CMP4MemReg, root, highMR, temp, cg());
         }

      if (firstHighZero == false || secondHighZero == false)
         {
         if (hasGlobalDeps == false)
            {
            generateLabelInstruction(highBranchOp, root, destinationLabel, cg());
            generateLabelInstruction(TR::InstOpCode::JNE4, root, doneLabel, cg());
            }
         else
            {
            generateLabelInstruction(highBranchOp, root, destinationLabel, deps, cg());
            generateLabelInstruction(TR::InstOpCode::JNE4, root, doneLabel, deps, cg());
            }
         }

      if (delayedSecond)
         {
         generateRegMemInstruction(TR::InstOpCode::L4RegMem, secondChild, delayedSecond, lowSecondMR, cg());
         secondLow = delayedSecond;
         }
      generateMemRegInstruction(TR::InstOpCode::CMP4MemReg, root, lowMR, secondLow, cg());
      }

   generateLabelInstruction(lowBranchOpCode, root, destinationLabel, deps, cg());

   if (lowMR != NULL)
      lowMR->decNodeReferenceCounts(cg());

   deps->stopAddingConditions();
   generateLabelInstruction(TR::InstOpCode::label, root, doneLabel, deps, cg());

   if (delayedFirst)
      {
      cg()->stopUsingRegister(delayedFirst);
      }

   if (delayedSecond)
      {
      cg()->stopUsingRegister(delayedSecond);
      }

   cg()->decReferenceCount(firstChild);
   cg()->decReferenceCount(secondChild);
   }

void TR_X86CompareAnalyser::longEqualityCompareAndBranchAnalyser(TR::Node        *root,
                                                                  TR::LabelSymbol *firstBranchLabel,
                                                                  TR::LabelSymbol *secondBranchLabel,
                                                                  TR::InstOpCode::Mnemonic  secondBranchOp)
   {
   TR::Node     *firstChild     = root->getFirstChild();
   TR::Node     *secondChild    = root->getSecondChild();
   TR::Register *firstRegister  = firstChild->getRegister();
   TR::Register *secondRegister = secondChild->getRegister();
   TR::Compilation* comp = cg()->comp();
   TR::LabelSymbol *startLabel  = TR::LabelSymbol::create(cg()->trHeapMemory(),cg());
   TR::LabelSymbol *endLabel    = TR::LabelSymbol::create(cg()->trHeapMemory(),cg());
   startLabel->setStartInternalControlFlow();
   endLabel->setEndInternalControlFlow();

   setInputs(firstChild, firstRegister, secondChild, secondRegister, true);

   printf("WCTEc %p\n", root);
   if (cg()->whichChildToEvaluate(root) == 0)
      {
      if (getEvalChild1())
         {
         firstRegister = cg()->evaluate(firstChild);
         }
      if (getEvalChild2())
         {
         secondRegister = cg()->evaluate(secondChild);
         }
      }
   else
      {
      if (getEvalChild2())
         {
         secondRegister = cg()->evaluate(secondChild);
         }
      if (getEvalChild1())
         {
         firstRegister = cg()->evaluate(firstChild);
         }
      }

   // If the first branch label is null, create one here and add it to the end of
   // the instruction sequence.
   //
   TR::RegisterDependencyConditions  *deps                 = NULL;
   TR::MemoryReference               *lowMR                = NULL;
   TR::MemoryReference               *highMR               = NULL;
   bool                               createdFirstLabel    = (firstBranchLabel == NULL ? true : false);
   uint32_t                           numAdditionalRegDeps = 5;

   if (getCmpReg1Mem2())
      {
      lowMR  = generateX86MemoryReference(secondChild, cg());
      highMR = generateX86MemoryReference(*lowMR, 4, cg());
      }
   else if (getCmpMem1Reg2())
      {
      lowMR  = generateX86MemoryReference(firstChild, cg());
      highMR = generateX86MemoryReference(*lowMR, 4, cg());
      }

   bool hasGlobalDeps = (root->getNumChildren() == 3) ? true : false;

   if (hasGlobalDeps)
      {
      // This child must be evaluated last to ensure virtual regs
      // survive unclobbered up to the branch; all other evaluations (including
      // generateX86MemoryReference on a node) must preceed this.
      //
      TR::Node *third = root->getChild(2);
      cg()->evaluate(third);
      if (firstBranchLabel == NULL)
         {
         firstBranchLabel = TR::LabelSymbol::create(cg()->trHeapMemory(),cg());
         deps = generateRegisterDependencyConditions(third, cg(), numAdditionalRegDeps);
         }
      else
         {
         deps = generateRegisterDependencyConditions(third, cg(), numAdditionalRegDeps);
         }
      }
   else
      {
      if (firstBranchLabel == NULL)
         {
         firstBranchLabel = TR::LabelSymbol::create(cg()->trHeapMemory(),cg());
         }

      deps = generateRegisterDependencyConditions((uint8_t)0, (uint8_t)numAdditionalRegDeps, cg());
      }

   generateLabelInstruction(TR::InstOpCode::label, root, startLabel, cg());

   if (getCmpReg1Reg2())
      {
      generateRegRegInstruction(TR::InstOpCode::CMP4RegReg, root, firstRegister->getLowOrder(), secondRegister->getLowOrder(), cg());
      if (deps != NULL)
         {
         deps->addPostCondition(firstRegister->getHighOrder(), TR::RealRegister::NoReg, cg());
         deps->addPostCondition(firstRegister->getLowOrder(), TR::RealRegister::NoReg, cg());
         deps->addPostCondition(secondRegister->getHighOrder(), TR::RealRegister::NoReg, cg());
         deps->addPostCondition(secondRegister->getLowOrder(), TR::RealRegister::NoReg, cg());
         deps->stopAddingConditions();
         }
      if (hasGlobalDeps == false)
         {
         generateLabelInstruction(TR::InstOpCode::JNE4, root, firstBranchLabel, cg());
         }
      else
         {
         generateLabelInstruction(TR::InstOpCode::JNE4, root, firstBranchLabel, deps, cg());
         }
      generateRegRegInstruction(TR::InstOpCode::CMP4RegReg, root, firstRegister->getHighOrder(), secondRegister->getHighOrder(), cg());
      }
   else if (getCmpReg1Mem2())
      {
      if (deps != NULL)
         {
         deps->addPostCondition(firstRegister->getHighOrder(), TR::RealRegister::NoReg, cg());
         deps->addPostCondition(firstRegister->getLowOrder(), TR::RealRegister::NoReg, cg());
         TR::Register *depReg = NULL;
         while ((depReg = lowMR->getNextRegister(depReg)) != NULL)
            {
            // Don't assign NoReg dependencies to real registers.
            //
            if (!depReg->getRealRegister())
               deps->addPostCondition(depReg, TR::RealRegister::NoReg, cg());
            }
         deps->stopAddingConditions();
         }

      generateRegMemInstruction(TR::InstOpCode::CMP4RegMem, root,
                                   firstRegister->getLowOrder(),
                                   lowMR, cg());
      if (hasGlobalDeps == false)
         {
         generateLabelInstruction(TR::InstOpCode::JNE4, root, firstBranchLabel, cg());
         }
      else
         {
         generateLabelInstruction(TR::InstOpCode::JNE4, root, firstBranchLabel, deps, cg());
         }
      generateRegMemInstruction(TR::InstOpCode::CMP4RegMem, root, firstRegister->getHighOrder(), highMR, cg());
      }
   else // Must be CmpMem1Reg2
      {
      if (deps != NULL)
         {
         deps->addPostCondition(secondRegister->getHighOrder(), TR::RealRegister::NoReg, cg());
         deps->addPostCondition(secondRegister->getLowOrder(), TR::RealRegister::NoReg, cg());
         TR::Register *depReg = NULL;
         while ((depReg = lowMR->getNextRegister(depReg)) != NULL)
            {
            // Don't assign NoReg dependencies to real registers.
            //
            if (!depReg->getRealRegister())
               deps->addPostCondition(depReg, TR::RealRegister::NoReg, cg());
            }
         deps->stopAddingConditions();
         }

      generateMemRegInstruction(TR::InstOpCode::CMP4MemReg, root,
                                   lowMR,
                                   secondRegister->getLowOrder(), cg());
      if (hasGlobalDeps == false)
         {
         generateLabelInstruction(TR::InstOpCode::JNE4, root, firstBranchLabel, cg());
         }
      else
         {
         generateLabelInstruction(TR::InstOpCode::JNE4, root, firstBranchLabel, deps, cg());
         }
      generateMemRegInstruction(TR::InstOpCode::CMP4MemReg, root, highMR, secondRegister->getHighOrder(), cg());
      }

   generateLabelInstruction(secondBranchOp, root, secondBranchLabel, deps, cg());

   // Set up the label for the local false branch target with the appropriate
   // register dependencies and emit the label
   //
   if (deps != NULL && createdFirstLabel != false)
      {
      generateLabelInstruction(TR::InstOpCode::label, root, firstBranchLabel, deps, cg());
      }

   generateLabelInstruction(TR::InstOpCode::label, root, endLabel, deps, cg());

   if (lowMR != NULL)
      lowMR->decNodeReferenceCounts(cg());

   cg()->decReferenceCount(firstChild);
   cg()->decReferenceCount(secondChild);
   }


TR::Register *TR_X86CompareAnalyser::longEqualityBooleanAnalyser(TR::Node       *root,
                                                                 TR::InstOpCode::Mnemonic setOpCode,
                                                                 TR::InstOpCode::Mnemonic combineOpCode)
   {
   TR::Node     *firstChild     = root->getFirstChild();
   TR::Node     *secondChild    = root->getSecondChild();
   TR::Register *firstRegister  = firstChild->getRegister();
   TR::Register *secondRegister = secondChild->getRegister();

   setInputs(firstChild, firstRegister, secondChild, secondRegister, true);

   printf("WCTEd %p\n", root);
   if (cg()->whichChildToEvaluate(root) == 0)
      {
      if (getEvalChild1())
         {
         firstRegister = cg()->evaluate(firstChild);
         }
      if (getEvalChild2())
         {
         secondRegister = cg()->evaluate(secondChild);
         }
      }
   else
      {
      if (getEvalChild2())
         {
         secondRegister = cg()->evaluate(secondChild);
         }
      if (getEvalChild1())
         {
         firstRegister = cg()->evaluate(firstChild);
         }
      }

   TR::Register *lowRegister  = cg()->allocateRegister();
   TR::Register *highRegister = cg()->allocateRegister();

   if (cg()->enableRegisterInterferences())
      {
      cg()->getLiveRegisters(TR_GPR)->setByteRegisterAssociation(lowRegister);
      cg()->getLiveRegisters(TR_GPR)->setByteRegisterAssociation(highRegister);
      }

   if (getCmpReg1Reg2())
      {
      generateRegRegInstruction(TR::InstOpCode::CMP4RegReg, root, firstRegister->getLowOrder(), secondRegister->getLowOrder(), cg());
      generateRegInstruction(setOpCode, root, lowRegister, cg());
      generateRegRegInstruction(TR::InstOpCode::CMP4RegReg, root, firstRegister->getHighOrder(), secondRegister->getHighOrder(), cg());
      }
   else if (getCmpReg1Mem2())
      {
      TR::MemoryReference  *lowMR  = generateX86MemoryReference(secondChild, cg());
      TR::MemoryReference  *highMR = generateX86MemoryReference(*lowMR, 4, cg());
      generateRegMemInstruction(TR::InstOpCode::CMP4RegMem, root, firstRegister->getLowOrder(), lowMR, cg());
      generateRegInstruction(setOpCode, root, lowRegister, cg());
      generateRegMemInstruction(TR::InstOpCode::CMP4RegMem, root, firstRegister->getHighOrder(), highMR, cg());
      lowMR->decNodeReferenceCounts(cg());
      }
   else // Must be CmpMem1Reg2
      {
      TR::MemoryReference  *lowMR  = generateX86MemoryReference(firstChild, cg());
      TR::MemoryReference  *highMR = generateX86MemoryReference(*lowMR, 4, cg());
      generateMemRegInstruction(TR::InstOpCode::CMP4MemReg, root, lowMR, secondRegister->getLowOrder(), cg());
      generateRegInstruction(setOpCode, root, lowRegister, cg());
      generateMemRegInstruction(TR::InstOpCode::CMP4MemReg, root, highMR, secondRegister->getHighOrder(), cg());
      lowMR->decNodeReferenceCounts(cg());
      }

   generateRegInstruction(setOpCode, root, highRegister, cg());
   generateRegRegInstruction(combineOpCode, root, highRegister, lowRegister, cg());

   generateRegRegInstruction(TR::InstOpCode::MOVZXReg4Reg1, root, highRegister, highRegister, cg());

   cg()->stopUsingRegister(lowRegister);

   root->setRegister(highRegister);
   cg()->decReferenceCount(firstChild);
   cg()->decReferenceCount(secondChild);

   return highRegister;

   }

TR::Register *TR_X86CompareAnalyser::longOrderedBooleanAnalyser(TR::Node       *root,
                                                                TR::InstOpCode::Mnemonic highSetOpCode,
                                                                TR::InstOpCode::Mnemonic lowSetOpCode)
   {
   TR::Node     *firstChild     = root->getFirstChild();
   TR::Node     *secondChild    = root->getSecondChild();
   TR::Register *firstRegister  = firstChild->getRegister();
   TR::Register *secondRegister = secondChild->getRegister();
   TR::Compilation* comp = cg()->comp();

   setInputs(firstChild, firstRegister, secondChild, secondRegister, true);

   printf("WCTEf %p\n", root);
   if (cg()->whichChildToEvaluate(root) == 0)
      {
      if (getEvalChild1())
         {
         firstRegister = cg()->evaluate(firstChild);
         }
      if (getEvalChild2())
         {
         secondRegister = cg()->evaluate(secondChild);
         }
      }
   else
      {
      if (getEvalChild2())
         {
         secondRegister = cg()->evaluate(secondChild);
         }
      if (getEvalChild1())
         {
         firstRegister = cg()->evaluate(firstChild);
         }
      }

   TR::Register *setRegister = cg()->allocateRegister();

   if (cg()->enableRegisterInterferences())
      cg()->getLiveRegisters(TR_GPR)->setByteRegisterAssociation(setRegister);

   TR::LabelSymbol *startLabel  = TR::LabelSymbol::create(cg()->trHeapMemory(),cg());
   TR::LabelSymbol *doneLabel   = TR::LabelSymbol::create(cg()->trHeapMemory(),cg());
   TR::RegisterDependencyConditions  *deps = generateRegisterDependencyConditions((uint8_t)0, 5, cg());
   TR::MemoryReference  *lowMR = NULL;
   TR::MemoryReference  *highMR;

   startLabel->setStartInternalControlFlow();
   doneLabel->setEndInternalControlFlow();
   generateLabelInstruction(TR::InstOpCode::label, root, startLabel, cg());

   if (getCmpReg1Reg2())
      {
      generateRegRegInstruction(TR::InstOpCode::CMP4RegReg, root, firstRegister->getHighOrder(), secondRegister->getHighOrder(), cg());
      generateRegInstruction(highSetOpCode, root, setRegister, cg());
      generateLabelInstruction(TR::InstOpCode::JNE4, root, doneLabel, cg());
      generateRegRegInstruction(TR::InstOpCode::CMP4RegReg, root, firstRegister->getLowOrder(), secondRegister->getLowOrder(), cg());
      deps->addPostCondition(firstRegister->getHighOrder(), TR::RealRegister::NoReg, cg());
      deps->addPostCondition(firstRegister->getLowOrder(), TR::RealRegister::NoReg, cg());
      deps->addPostCondition(secondRegister->getHighOrder(), TR::RealRegister::NoReg, cg());
      deps->addPostCondition(secondRegister->getLowOrder(), TR::RealRegister::NoReg, cg());
      }
   else if (getCmpReg1Mem2())
      {
      lowMR  = generateX86MemoryReference(secondChild, cg());
      highMR = generateX86MemoryReference(*lowMR, 4, cg());
      generateRegMemInstruction(TR::InstOpCode::CMP4RegMem, root,
                                   firstRegister->getHighOrder(),
                                   highMR, cg());
      generateRegInstruction(highSetOpCode, root, setRegister, cg());
      generateLabelInstruction(TR::InstOpCode::JNE4, root, doneLabel, cg());
      generateRegMemInstruction(TR::InstOpCode::CMP4RegMem, root, firstRegister->getLowOrder(), lowMR, cg());
      deps->addPostCondition(firstRegister->getHighOrder(), TR::RealRegister::NoReg, cg());
      deps->addPostCondition(firstRegister->getLowOrder(), TR::RealRegister::NoReg, cg());
      }
   else // Must be CmpMem1Reg2
      {
      lowMR  = generateX86MemoryReference(firstChild, cg());
      highMR = generateX86MemoryReference(*lowMR, 4, cg());
      generateMemRegInstruction(TR::InstOpCode::CMP4MemReg, root, highMR, secondRegister->getHighOrder(), cg());
      generateRegInstruction(highSetOpCode, root, setRegister, cg());
      generateLabelInstruction(TR::InstOpCode::JNE4, root, doneLabel, cg());
      generateMemRegInstruction(TR::InstOpCode::CMP4MemReg, root, lowMR, secondRegister->getLowOrder(), cg());
      deps->addPostCondition(secondRegister->getHighOrder(), TR::RealRegister::NoReg, cg());
      deps->addPostCondition(secondRegister->getLowOrder(), TR::RealRegister::NoReg, cg());
      }

   generateRegInstruction(lowSetOpCode, root, setRegister, cg());
   if (lowMR != NULL)
      {
      // Add register dependencies from the memory reference
      //
      TR::Register *depReg = NULL;
      while ((depReg = lowMR->getNextRegister(depReg)) != NULL)
         {
         // Don't assign NoReg dependencies to real registers.
         //
         if (!depReg->getRealRegister())
            deps->addPostCondition(depReg, TR::RealRegister::NoReg, cg());
         }
      lowMR->decNodeReferenceCounts(cg());
      }
   deps->stopAddingConditions();
   generateLabelInstruction(TR::InstOpCode::label, root, doneLabel, deps, cg());

   generateRegRegInstruction(TR::InstOpCode::MOVZXReg4Reg1, root, setRegister, setRegister, cg());

   root->setRegister(setRegister);
   cg()->decReferenceCount(firstChild);
   cg()->decReferenceCount(secondChild);

   return setRegister;

   }

TR::Register *TR_X86CompareAnalyser::longCMPAnalyser(TR::Node *root)
   {
   TR::Node     *firstChild     = root->getFirstChild();
   TR::Node     *secondChild    = root->getSecondChild();
   TR::Register *firstRegister  = firstChild->getRegister();
   TR::Register *secondRegister = secondChild->getRegister();
   TR::Compilation* comp = cg()->comp();
   setInputs(firstChild, firstRegister, secondChild, secondRegister, true);

   printf("WCTEg %p\n", root);
   if (cg()->whichChildToEvaluate(root) == 0)
      {
      if (getEvalChild1())
         {
         firstRegister = cg()->evaluate(firstChild);
         }
      if (getEvalChild2())
         {
         secondRegister = cg()->evaluate(secondChild);
         }
      }
   else
      {
      if (getEvalChild2())
         {
         secondRegister = cg()->evaluate(secondChild);
         }
      if (getEvalChild1())
         {
         firstRegister = cg()->evaluate(firstChild);
         }
      }

   TR::Register *setRegister = cg()->allocateRegister();

   if (cg()->enableRegisterInterferences())
      cg()->getLiveRegisters(TR_GPR)->setByteRegisterAssociation(setRegister);

   TR::LabelSymbol *highDoneLabel = TR::LabelSymbol::create(cg()->trHeapMemory(),cg());
   TR::LabelSymbol *startLabel    = TR::LabelSymbol::create(cg()->trHeapMemory(),cg());
   TR::LabelSymbol *doneLabel     = TR::LabelSymbol::create(cg()->trHeapMemory(),cg());
   TR::RegisterDependencyConditions  *deps = generateRegisterDependencyConditions((uint8_t)0, 6, cg());
   TR::MemoryReference  *lowMR = NULL;
   TR::MemoryReference  *highMR;

   startLabel->setStartInternalControlFlow();
   doneLabel->setEndInternalControlFlow();
   generateLabelInstruction(TR::InstOpCode::label, root, startLabel, cg());

   if (getCmpReg1Reg2())
      {
      generateRegRegInstruction(TR::InstOpCode::CMP4RegReg, root, firstRegister->getHighOrder(), secondRegister->getHighOrder(), cg());
      generateRegInstruction(TR::InstOpCode::SETNE1Reg, root, setRegister, cg());
      generateLabelInstruction(TR::InstOpCode::JNE4, root, highDoneLabel, cg());
      generateRegRegInstruction(TR::InstOpCode::CMP4RegReg, root, firstRegister->getLowOrder(), secondRegister->getLowOrder(), cg());
      deps->addPostCondition(firstRegister->getHighOrder(), TR::RealRegister::NoReg, cg());
      deps->addPostCondition(firstRegister->getLowOrder(), TR::RealRegister::NoReg, cg());
      deps->addPostCondition(secondRegister->getHighOrder(), TR::RealRegister::NoReg, cg());
      deps->addPostCondition(secondRegister->getLowOrder(), TR::RealRegister::NoReg, cg());

      }
   else if (getCmpReg1Mem2())
      {
      lowMR  = generateX86MemoryReference(secondChild, cg());
      highMR = generateX86MemoryReference(*lowMR, 4, cg());
      generateRegMemInstruction(TR::InstOpCode::CMP4RegMem, root, firstRegister->getHighOrder(), highMR, cg());
      generateRegInstruction(TR::InstOpCode::SETNE1Reg, root, setRegister, cg());
      generateLabelInstruction(TR::InstOpCode::JNE4, root, highDoneLabel, cg());
      generateRegMemInstruction(TR::InstOpCode::CMP4RegMem, root, firstRegister->getLowOrder(), lowMR, cg());
      deps->addPostCondition(firstRegister->getHighOrder(), TR::RealRegister::NoReg, cg());
      deps->addPostCondition(firstRegister->getLowOrder(), TR::RealRegister::NoReg, cg());
      }
   else // Must be CmpMem1Reg2
      {
      lowMR  = generateX86MemoryReference(firstChild, cg());
      highMR = generateX86MemoryReference(*lowMR, 4, cg());
      generateMemRegInstruction(TR::InstOpCode::CMP4MemReg, root, highMR, secondRegister->getHighOrder(), cg());
      generateRegInstruction(TR::InstOpCode::SETNE1Reg, root, setRegister, cg());
      generateLabelInstruction(TR::InstOpCode::JNE4, root, highDoneLabel, cg());
      generateMemRegInstruction(TR::InstOpCode::CMP4MemReg, root, lowMR, secondRegister->getLowOrder(), cg());
      deps->addPostCondition(secondRegister->getHighOrder(), TR::RealRegister::NoReg, cg());
      deps->addPostCondition(secondRegister->getLowOrder(), TR::RealRegister::NoReg, cg());
      }

   generateRegInstruction(TR::InstOpCode::SETNE1Reg, root, setRegister, cg());
   generateLabelInstruction(TR::InstOpCode::JAE4, root, doneLabel, cg());
   generateRegInstruction(TR::InstOpCode::NEG1Reg, root, setRegister, cg());
   generateLabelInstruction(TR::InstOpCode::JMP4, root, doneLabel, cg());
   generateLabelInstruction(TR::InstOpCode::label, root, highDoneLabel, cg());
   generateLabelInstruction(TR::InstOpCode::JGE4, root, doneLabel, cg());
   generateRegInstruction(TR::InstOpCode::NEG1Reg, root, setRegister, cg());

   deps->addPostCondition(setRegister, TR::RealRegister::ByteReg, cg());
   if (lowMR != NULL)
      {
      // Add register dependencies from the memory reference
      //
      TR::Register *depReg = NULL;
      while ((depReg = lowMR->getNextRegister(depReg)) != NULL)
         {
         // Don't assign NoReg dependencies to real registers.
         //
         if (!depReg->getRealRegister())
            deps->addPostCondition(depReg, TR::RealRegister::NoReg, cg());
         }
      lowMR->decNodeReferenceCounts(cg());
      }
   deps->stopAddingConditions();
   generateLabelInstruction(TR::InstOpCode::label, root, doneLabel, deps, cg());

   generateRegRegInstruction(TR::InstOpCode::MOVSXReg4Reg1, root, setRegister, setRegister, cg());

   cg()->decReferenceCount(firstChild);
   cg()->decReferenceCount(secondChild);

   return setRegister;

   }

const uint8_t TR_X86CompareAnalyser::_actionMap[NUM_ACTIONS] =
   {                  // Reg1 Mem1 Clob1 Reg2 Mem2 Clob2
   EvalChild1 |       //  0    0     0    0    0     0
   EvalChild2 |
   CmpReg1Reg2,

   EvalChild1 |       //  0    0     0    0    0     1
   EvalChild2 |
   CmpReg1Reg2,

   EvalChild1 |       //  0    0     0    0    1     0
   CmpReg1Mem2,

   EvalChild1 |       //  0    0     0    0    1     1
   CmpReg1Mem2,

   EvalChild1 |       //  0    0     0    1    0     0
   EvalChild2 |
   CmpReg1Reg2,

   EvalChild1 |       //  0    0     0    1    0     1
   EvalChild2 |
   CmpReg1Reg2,

   EvalChild1 |       //  0    0     0    1    1     0
   EvalChild2 |
   CmpReg1Reg2,

   EvalChild1 |       //  0    0     0    1    1     1
   EvalChild2 |
   CmpReg1Reg2,

   EvalChild1 |       //  0    0     1    0    0     0
   EvalChild2 |
   CmpReg1Reg2,

   EvalChild1 |       //  0    0     1    0    0     1
   EvalChild2 |
   CmpReg1Reg2,

   EvalChild1 |       //  0    0     1    0    1     0
   CmpReg1Mem2,

   EvalChild1 |       //  0    0     1    0    1     1
   CmpReg1Mem2,

   EvalChild1 |       //  0    0     1    1    0     0
   EvalChild2 |
   CmpReg1Reg2,

   EvalChild1 |       //  0    0     1    1    0     1
   EvalChild2 |
   CmpReg1Reg2,

   EvalChild1 |       //  0    0     1    1    1     0
   EvalChild2 |
   CmpReg1Reg2,

   EvalChild1 |       //  0    0     1    1    1     1
   EvalChild2 |
   CmpReg1Reg2,

   EvalChild2 |       //  0    1     0    0    0     0
   CmpMem1Reg2,

   EvalChild2 |       //  0    1     0    0    0     1
   CmpMem1Reg2,

   EvalChild1 |       //  0    1     0    0    1     0
   CmpReg1Mem2,

   EvalChild1 |       //  0    1     0    0    1     1
   CmpReg1Mem2,

   EvalChild2 |       //  0    1     0    1    0     0
   CmpMem1Reg2,

   EvalChild2 |       //  0    1     0    1    0     1
   CmpMem1Reg2,

   EvalChild2 |       //  0    1     0    1    1     0
   CmpMem1Reg2,

   EvalChild2 |       //  0    1     0    1    1     1
   CmpMem1Reg2,

   EvalChild2 |       //  0    1     1    0    0     0
   CmpMem1Reg2,

   EvalChild2 |       //  0    1     1    0    0     1
   CmpMem1Reg2,

   EvalChild1 |       //  0    1     1    0    1     0
   CmpReg1Mem2,

   EvalChild1 |       //  0    1     1    0    1     1
   CmpReg1Mem2,

   EvalChild2 |       //  0    1     1    1    0     0
   CmpMem1Reg2,

   EvalChild2 |       //  0    1     1    1    0     1
   CmpMem1Reg2,

   EvalChild2 |       //  0    1     1    1    1     0
   CmpMem1Reg2,

   EvalChild2 |       //  0    1     1    1    1     1
   CmpMem1Reg2,

   EvalChild1 |       //  1    0     0    0    0     0
   EvalChild2 |
   CmpReg1Reg2,

   EvalChild1 |       //  1    0     0    0    0     1
   EvalChild2 |
   CmpReg1Reg2,

   EvalChild1 |       //  1    0     0    0    1     0
   CmpReg1Mem2,

   EvalChild1 |       //  1    0     0    0    1     1
   CmpReg1Mem2,

   EvalChild1 |       //  1    0     0    1    0     0
   EvalChild2 |
   CmpReg1Reg2,

   EvalChild1 |       //  1    0     0    1    0     1
   EvalChild2 |
   CmpReg1Reg2,

   EvalChild1 |       //  1    0     0    1    1     0
   EvalChild2 |
   CmpReg1Reg2,

   EvalChild1 |       //  1    0     0    1    1     1
   EvalChild2 |
   CmpReg1Reg2,

   EvalChild1 |       //  1    0     1    0    0     0
   EvalChild2 |
   CmpReg1Reg2,

   EvalChild1 |       //  1    0     1    0    0     1
   EvalChild2 |
   CmpReg1Reg2,

   EvalChild1 |       //  1    0     1    0    1     0
   CmpReg1Mem2,

   EvalChild1 |       //  1    0     1    0    1     1
   CmpReg1Mem2,

   EvalChild1 |       //  1    0     1    1    0     0
   EvalChild2 |
   CmpReg1Reg2,

   EvalChild1 |       //  1    0     1    1    0     1
   EvalChild2 |
   CmpReg1Reg2,

   EvalChild1 |       //  1    0     1    1    1     0
   EvalChild2 |
   CmpReg1Reg2,

   EvalChild1 |       //  1    0     1    1    1     1
   EvalChild2 |
   CmpReg1Reg2,

   EvalChild1 |       //  1    1     0    0    0     0
   EvalChild2 |
   CmpReg1Reg2,

   EvalChild1 |       //  1    1     0    0    0     1
   EvalChild2 |
   CmpReg1Reg2,

   EvalChild1 |       //  1    1     0    0    1     0
   CmpReg1Mem2,

   EvalChild1 |       //  1    1     0    0    1     1
   CmpReg1Mem2,

   EvalChild1 |       //  1    1     0    1    0     0
   EvalChild2 |
   CmpReg1Reg2,

   EvalChild1 |       //  1    1     0    1    0     1
   EvalChild2 |
   CmpReg1Reg2,

   EvalChild1 |       //  1    1     0    1    1     0
   EvalChild2 |
   CmpReg1Reg2,

   EvalChild1 |       //  1    1     0    1    1     1
   EvalChild2 |
   CmpReg1Reg2,

   EvalChild1 |       //  1    1     1    0    0     0
   EvalChild2 |
   CmpReg1Reg2,

   EvalChild1 |       //  1    1     1    0    0     1
   EvalChild2 |
   CmpReg1Reg2,

   EvalChild1 |       //  1    1     1    0    1     0
   CmpReg1Mem2,

   EvalChild1 |       //  1    1     1    0    1     1
   CmpReg1Mem2,

   EvalChild1 |       //  1    1     1    1    0     0
   EvalChild2 |
   CmpReg1Reg2,

   EvalChild1 |       //  1    1     1    1    0     1
   EvalChild2 |
   CmpReg1Reg2,

   EvalChild1 |       //  1    1     1    1    1     0
   EvalChild2 |
   CmpReg1Reg2,

   EvalChild1 |       //  1    1     1    1    1     1
   EvalChild2 |
   CmpReg1Reg2
   };
