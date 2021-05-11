/*******************************************************************************
 * Copyright (c) 2018, 2021 IBM Corp. and others
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at http://eclipse.org/legal/epl-2.0
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
 * [2] http://openjdk.java.net/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/

#include "codegen/ARM64ConditionCode.hpp"
#include "codegen/ARM64HelperCallSnippet.hpp"
#include "codegen/ARM64Instruction.hpp"
#include "codegen/CodeGenerator.hpp"
#include "codegen/CodeGeneratorUtils.hpp"
#include "codegen/GenerateInstructions.hpp"
#include "codegen/Linkage.hpp"
#include "codegen/Linkage_inlines.hpp"
#include "codegen/RegisterDependency.hpp"
#include "codegen/TreeEvaluator.hpp"
#include "il/Node.hpp"
#include "il/Node_inlines.hpp"

static bool virtualGuardHelper(TR::Node *node, TR::CodeGenerator *cg);

TR::Register *
genericReturnEvaluator(TR::Node *node, TR::RealRegister::RegNum rnum, TR_RegisterKinds rk, TR_ReturnInfo i,  TR::CodeGenerator *cg)
   {
   TR::Node *firstChild = node->getFirstChild();
   TR::Register *returnRegister = cg->evaluate(firstChild);

   TR::RegisterDependencyConditions *deps = new (cg->trHeapMemory()) TR::RegisterDependencyConditions(1, 0, cg->trMemory());
   deps->addPreCondition(returnRegister, rnum);
   generateAdminInstruction(cg, TR::InstOpCode::retn, node, deps);

   cg->comp()->setReturnInfo(i);
   cg->decReferenceCount(firstChild);

   return NULL;
   }

TR::Register *
OMR::ARM64::TreeEvaluator::ireturnEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   return genericReturnEvaluator(node, cg->getProperties().getIntegerReturnRegister(), TR_GPR, TR_IntReturn, cg);
   }

TR::Register *
OMR::ARM64::TreeEvaluator::lreturnEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   return genericReturnEvaluator(node, cg->getProperties().getLongReturnRegister(), TR_GPR, TR_LongReturn, cg);
   }

TR::Register *
OMR::ARM64::TreeEvaluator::areturnEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   return genericReturnEvaluator(node, cg->getProperties().getLongReturnRegister(), TR_GPR, TR_ObjectReturn, cg);
   }

// void return
TR::Register *
OMR::ARM64::TreeEvaluator::returnEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   generateAdminInstruction(cg, TR::InstOpCode::retn, node);
   cg->comp()->setReturnInfo(TR_VoidReturn);
   return NULL;
   }

TR::Register *OMR::ARM64::TreeEvaluator::gotoEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   TR::LabelSymbol *gotoLabel = node->getBranchDestination()->getNode()->getLabel();
   if (node->getNumChildren() > 0)
      {
      TR::Node *child = node->getFirstChild();
      cg->evaluate(child);
      generateLabelInstruction(cg, TR::InstOpCode::b, node, gotoLabel,
            generateRegisterDependencyConditions(cg, child, 0));
      cg->decReferenceCount(child);
      }
   else
      {
      generateLabelInstruction(cg, TR::InstOpCode::b, node, gotoLabel);
      }
   return NULL;
   }

static TR::Instruction *ificmpHelper(TR::Node *node, TR::ARM64ConditionCode cc, bool is64bit, TR::CodeGenerator *cg)
   {
   if (virtualGuardHelper(node, cg))
      return NULL;

   TR::Compilation *comp = cg->comp();
   TR::Node *firstChild = node->getFirstChild();
   TR::Node *secondChild = node->getSecondChild();
   TR::Node *thirdChild = NULL;
   TR::Register *src1Reg = cg->evaluate(firstChild);
   bool useRegCompare = true;
   TR::LabelSymbol *dstLabel;
   TR::Instruction *result;
   TR::RegisterDependencyConditions *deps;

#ifdef J9_PROJECT_SPECIFIC
if (cg->profiledPointersRequireRelocation() && secondChild->getOpCodeValue() == TR::aconst &&
   (secondChild->isClassPointerConstant() || secondChild->isMethodPointerConstant()))
   {
   if (node->isProfiledGuard())
      {
      TR_VirtualGuard *virtualGuard = comp->findVirtualGuardInfo(node);
      TR_AOTGuardSite *site = comp->addAOTNOPSite();
      site->setType(TR_ProfiledGuard);
      site->setGuard(virtualGuard);
      site->setNode(node);
      site->setAconstNode(secondChild);
      }
   else
      {
      TR_ASSERT(!(node->isNopableInlineGuard()),"Should not evaluate class or method pointer constants underneath NOPable guards as they are runtime assumptions handled by virtualGuardHelper");
      cg->evaluate(secondChild);
      }
   }
#endif

   if (secondChild->getOpCode().isLoadConst())
      {
      int64_t secondChildValue = is64bit ? secondChild->getLongInt() : secondChild->getInt();
      if ((cc == TR::CC_EQ || cc == TR::CC_NE)
            && (secondChildValue == 0)
            /* If the node has the third child (TR::GlRegDeps)
             * and if the number of children of it equals to the number of allocatable integer registers,
             * we cannot assign a register for a cbz/cbnz instruction because all registers are used up.
             * We need to use a b.cond instruction instead for that case.
             */
            && ((node->getNumChildren() != 3) ||
             (node->getChild(2)->getNumChildren() != cg->getLinkage()->getProperties().getNumAllocatableIntegerRegisters())))
         {
         TR::InstOpCode::Mnemonic op;
         if (cc == TR::CC_EQ )
            op = is64bit ? TR::InstOpCode::cbzx : TR::InstOpCode::cbzw;
         else
            op = is64bit ? TR::InstOpCode::cbnzx : TR::InstOpCode::cbnzw;

         dstLabel = node->getBranchDestination()->getNode()->getLabel();
         if (node->getNumChildren() == 3)
            {
            thirdChild = node->getChild(2);
            TR_ASSERT(thirdChild->getOpCodeValue() == TR::GlRegDeps, "The third child of a compare must be a TR::GlRegDeps");
            cg->evaluate(thirdChild);

            deps = generateRegisterDependencyConditions(cg, thirdChild, 0);
            result = generateCompareBranchInstruction(cg, op, node, src1Reg, dstLabel, deps);
            }
         else
            {
            result = generateCompareBranchInstruction(cg, op, node, src1Reg, dstLabel);
            }

         cg->decReferenceCount(firstChild);
         cg->decReferenceCount(secondChild);
         if (thirdChild)
            {
            cg->decReferenceCount(thirdChild);
            }

         return result;
         }
      }

   if (secondChild->getOpCode().isLoadConst() && secondChild->getRegister() == NULL)
      {
      int64_t value = is64bit ? secondChild->getLongInt() : secondChild->getInt();
      if (constantIsUnsignedImm12(value))
         {
         generateCompareImmInstruction(cg, node, src1Reg, value, is64bit);
         useRegCompare = false;
         }
      else if (constantIsUnsignedImm12(-value))
         {
         generateCompareImmInstruction(cg, node, src1Reg, value, is64bit);
         useRegCompare = false;
         }
      }

   if (useRegCompare)
      {
      TR::Register *src2Reg = cg->evaluate(secondChild);
      generateCompareInstruction(cg, node, src1Reg, src2Reg, is64bit);
      }

   dstLabel = node->getBranchDestination()->getNode()->getLabel();
   if (node->getNumChildren() == 3)
      {
      thirdChild = node->getChild(2);
      TR_ASSERT(thirdChild->getOpCodeValue() == TR::GlRegDeps, "The third child of a compare must be a TR::GlRegDeps");

      cg->evaluate(thirdChild);

      deps = generateRegisterDependencyConditions(cg, thirdChild, 0);
      result = generateConditionalBranchInstruction(cg, TR::InstOpCode::b_cond, node, dstLabel, cc, deps);
      }
   else
      {
      result = generateConditionalBranchInstruction(cg, TR::InstOpCode::b_cond, node, dstLabel, cc);
      }

   cg->decReferenceCount(firstChild);
   cg->decReferenceCount(secondChild);
   if (thirdChild)
      {
      cg->decReferenceCount(thirdChild);
      }
   return result;
   }

TR::Register *
OMR::ARM64::TreeEvaluator::ificmpeqEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   ificmpHelper(node, TR::CC_EQ, false, cg);
   return NULL;
   }

TR::Register *
OMR::ARM64::TreeEvaluator::ificmpneEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   ificmpHelper(node, TR::CC_NE, false, cg);
   return NULL;
   }

TR::Register *
OMR::ARM64::TreeEvaluator::ificmpltEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   ificmpHelper(node, TR::CC_LT, false, cg);
   return NULL;
   }

TR::Register *
OMR::ARM64::TreeEvaluator::ificmpgeEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   ificmpHelper(node, TR::CC_GE, false, cg);
   return NULL;
   }

TR::Register *
OMR::ARM64::TreeEvaluator::ificmpgtEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   ificmpHelper(node, TR::CC_GT, false, cg);
   return NULL;
   }

TR::Register *
OMR::ARM64::TreeEvaluator::ificmpleEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   ificmpHelper(node, TR::CC_LE, false, cg);
   return NULL;
   }

TR::Register *
OMR::ARM64::TreeEvaluator::ifiucmpltEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   ificmpHelper(node, TR::CC_CC, false, cg);
   return NULL;
   }

TR::Register *
OMR::ARM64::TreeEvaluator::ifiucmpgeEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   ificmpHelper(node, TR::CC_CS, false, cg);
   return NULL;
   }

TR::Register *
OMR::ARM64::TreeEvaluator::ifiucmpgtEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   ificmpHelper(node, TR::CC_HI, false, cg);
   return NULL;
   }

TR::Register *
OMR::ARM64::TreeEvaluator::ifiucmpleEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   ificmpHelper(node, TR::CC_LS, false, cg);
   return NULL;
   }

// also handles ifacmpeq
TR::Register *
OMR::ARM64::TreeEvaluator::iflcmpeqEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   ificmpHelper(node, TR::CC_EQ, true, cg);
   return NULL;
   }

// also handles ifacmpne
TR::Register *
OMR::ARM64::TreeEvaluator::iflcmpneEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   ificmpHelper(node, TR::CC_NE, true, cg);
   return NULL;
   }

TR::Register *
OMR::ARM64::TreeEvaluator::iflcmpltEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   ificmpHelper(node, TR::CC_LT, true, cg);
   return NULL;
   }

TR::Register *
OMR::ARM64::TreeEvaluator::iflcmpgeEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   ificmpHelper(node, TR::CC_GE, true, cg);
   return NULL;
   }

// also handles ifacmplt
TR::Register *
OMR::ARM64::TreeEvaluator::iflucmpltEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   ificmpHelper(node, TR::CC_CC, true, cg);
   return NULL;
   }

// also handles ifacmpge
TR::Register *
OMR::ARM64::TreeEvaluator::iflucmpgeEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   ificmpHelper(node, TR::CC_CS, true, cg);
   return NULL;
   }

TR::Register *
OMR::ARM64::TreeEvaluator::iflcmpgtEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   ificmpHelper(node, TR::CC_GT, true, cg);
   return NULL;
   }

TR::Register *
OMR::ARM64::TreeEvaluator::iflcmpleEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   ificmpHelper(node, TR::CC_LE, true, cg);
   return NULL;
   }

// also handles ifacmpgt
TR::Register *
OMR::ARM64::TreeEvaluator::iflucmpgtEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   ificmpHelper(node, TR::CC_HI, true, cg);
   return NULL;
   }

// also handles ifacmple
TR::Register *
OMR::ARM64::TreeEvaluator::iflucmpleEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   ificmpHelper(node, TR::CC_LS, true, cg);
   return NULL;
   }

static TR::Register *icmpHelper(TR::Node *node, TR::ARM64ConditionCode cc, bool is64bit, TR::CodeGenerator *cg)
   {
   TR::Register *trgReg = cg->allocateRegister();
   TR::Node *firstChild = node->getFirstChild();
   TR::Node *secondChild = node->getSecondChild();
   TR::Register *src1Reg = cg->evaluate(firstChild);
   bool useRegCompare = true;

   if (secondChild->getOpCode().isLoadConst() && secondChild->getRegister() == NULL)
      {
      int64_t value = is64bit ? secondChild->getLongInt() : secondChild->getInt();
      if (constantIsUnsignedImm12(value))
         {
         generateCompareImmInstruction(cg, node, src1Reg, value, is64bit);
         useRegCompare = false;
         }
      else if (constantIsUnsignedImm12(-value))
         {
         generateCompareImmInstruction(cg, node, src1Reg, value, is64bit);
         useRegCompare = false;
         }
      }

   if (useRegCompare)
      {
      TR::Register *src2Reg = cg->evaluate(secondChild);
      generateCompareInstruction(cg, node, src1Reg, src2Reg, is64bit);
      }

   generateCSetInstruction(cg, node, trgReg, cc);

   node->setRegister(trgReg);
   cg->decReferenceCount(firstChild);
   cg->decReferenceCount(secondChild);
   return trgReg;
   }

// also handles bcmpeq, bucmpeq, scmpeq, sucmpeq
TR::Register *
OMR::ARM64::TreeEvaluator::icmpeqEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   return icmpHelper(node, TR::CC_EQ, false, cg);
   }

// also handles bcmpne, bucmpne, scmpne, sucmpne
TR::Register *
OMR::ARM64::TreeEvaluator::icmpneEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   return icmpHelper(node, TR::CC_NE, false, cg);
   }

// also handles bcmplt, scmplt
TR::Register *
OMR::ARM64::TreeEvaluator::icmpltEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   return icmpHelper(node, TR::CC_LT, false, cg);
   }

// also handles bcmple, scmple
TR::Register *
OMR::ARM64::TreeEvaluator::icmpleEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   return icmpHelper(node, TR::CC_LE, false, cg);
   }

// also handles bcmpge, scmpge
TR::Register *
OMR::ARM64::TreeEvaluator::icmpgeEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   return icmpHelper(node, TR::CC_GE, false, cg);
   }

// also handles bcmpgt, scmpgt
TR::Register *
OMR::ARM64::TreeEvaluator::icmpgtEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   return icmpHelper(node, TR::CC_GT, false, cg);
   }

// also handles bucmplt, sucmplt
TR::Register *
OMR::ARM64::TreeEvaluator::iucmpltEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   return icmpHelper(node, TR::CC_CC, false, cg);
   }

// also handles bucmple, sucmple
TR::Register *
OMR::ARM64::TreeEvaluator::iucmpleEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   return icmpHelper(node, TR::CC_LS, false, cg);
   }

// also handles bucmpge, sucmpge
TR::Register *
OMR::ARM64::TreeEvaluator::iucmpgeEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   return icmpHelper(node, TR::CC_CS, false, cg);
   }

// also handles bucmpgt, sucmpgt
TR::Register *
OMR::ARM64::TreeEvaluator::iucmpgtEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   return icmpHelper(node, TR::CC_HI, false, cg);
   }

// also handles  acmpeq
TR::Register *
OMR::ARM64::TreeEvaluator::lcmpeqEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   return icmpHelper(node, TR::CC_EQ, true, cg);
   }

// also handles acmpne
TR::Register *
OMR::ARM64::TreeEvaluator::lcmpneEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   return icmpHelper(node, TR::CC_NE, true, cg);
   }

TR::Register *
OMR::ARM64::TreeEvaluator::lcmpltEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   return icmpHelper(node, TR::CC_LT, true, cg);
   }

TR::Register *
OMR::ARM64::TreeEvaluator::lcmpgeEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   return icmpHelper(node, TR::CC_GE, true, cg);
   }

TR::Register *
OMR::ARM64::TreeEvaluator::lcmpgtEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   return icmpHelper(node, TR::CC_GT, true, cg);
   }

TR::Register *
OMR::ARM64::TreeEvaluator::lcmpleEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   return icmpHelper(node, TR::CC_LE, true, cg);
   }

// also handles acmplt
TR::Register *
OMR::ARM64::TreeEvaluator::lucmpltEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   return icmpHelper(node, TR::CC_CC, true, cg);
   }

// also handles acmpge
TR::Register *
OMR::ARM64::TreeEvaluator::lucmpgeEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   return icmpHelper(node, TR::CC_CS, true, cg);
   }

// also handles acmpgt
TR::Register *
OMR::ARM64::TreeEvaluator::lucmpgtEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   return icmpHelper(node, TR::CC_HI, true, cg);
   }

// also handles acmple
TR::Register *
OMR::ARM64::TreeEvaluator::lucmpleEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   return icmpHelper(node, TR::CC_LS, true, cg);
   }

TR::Register *
OMR::ARM64::TreeEvaluator::lcmpEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   TR::Node *firstChild = node->getFirstChild();
   TR::Node *secondChild = node->getSecondChild();
   TR::Register *src1Reg = cg->evaluate(firstChild);
   TR::Register *src2Reg = cg->evaluate(secondChild);
   TR::Register *trgReg = cg->allocateRegister();
   TR::Register *tmpReg = cg->allocateRegister();

   generateCompareInstruction(cg, node, src1Reg, src2Reg, true);
   generateCSetInstruction(cg, node, trgReg, TR::CC_GE);
   generateCSetInstruction(cg, node, tmpReg, TR::CC_LE);
   generateTrg1Src2Instruction(cg, TR::InstOpCode::subw, node, trgReg, trgReg, tmpReg);

   cg->stopUsingRegister(tmpReg);

   node->setRegister(trgReg);
   cg->decReferenceCount(firstChild);
   cg->decReferenceCount(secondChild);
   return trgReg;
   }

TR::Register *
OMR::ARM64::TreeEvaluator::acmpeqEvaluator(TR::Node *node, TR::CodeGenerator *cg)
	{
	// TODO:ARM64: Enable TR::TreeEvaluator::acmpeqEvaluator in compiler/aarch64/codegen/TreeEvaluatorTable.hpp when Implemented.
	return OMR::ARM64::TreeEvaluator::unImpOpEvaluator(node, cg);
	}

TR::Register *
OMR::ARM64::TreeEvaluator::lookupEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   int32_t numChildren = node->getNumChildren();
   TR::Node *selectorNode = node->getFirstChild();
   TR::Register *selectorReg = cg->evaluate(selectorNode);
   TR::Node *defaultChild = node->getSecondChild();
   TR::RegisterDependencyConditions *conditions;
   TR::Register *tmpRegister = NULL;

   if (!constantIsUnsignedImm12(node->getChild(2)->getCaseConstant())
       || !constantIsUnsignedImm12(node->getChild(numChildren-1)->getCaseConstant()))
      {
      conditions = new (cg->trHeapMemory()) TR::RegisterDependencyConditions(2, 2, cg->trMemory());
      tmpRegister = cg->allocateRegister();
      TR::addDependency(conditions, tmpRegister, TR::RealRegister::NoReg, TR_GPR, cg);
      }
   else
      {
      conditions = new (cg->trHeapMemory()) TR::RegisterDependencyConditions(1, 1, cg->trMemory());
      }
   TR::addDependency(conditions, selectorReg, TR::RealRegister::NoReg, TR_GPR, cg);

   for (int32_t i = 2; i < numChildren; i++)
      {
      TR::Node *child = node->getChild(i);
      int32_t caseValue = child->getCaseConstant();

      if (!constantIsUnsignedImm12(caseValue))
         {
         loadConstant32(cg, node, caseValue, tmpRegister);
         generateCompareInstruction(cg, node, selectorReg, tmpRegister);
         }
      else
         {
         generateCompareImmInstruction(cg, node, selectorReg, caseValue);
         }

      TR::RegisterDependencyConditions *cond = conditions;
      if (child->getNumChildren() > 0)
         {
         // GRA
         cg->evaluate(child->getFirstChild());
         cond = cond->clone(cg, generateRegisterDependencyConditions(cg, child->getFirstChild(), 0));
         }
      generateConditionalBranchInstruction(cg, TR::InstOpCode::b_cond, node, child->getBranchDestination()->getNode()->getLabel(), TR::CC_EQ, cond);
      }

   // Branch to default
   if (defaultChild->getNumChildren() > 0)
      {
      // GRA
      cg->evaluate(defaultChild->getFirstChild());
      conditions = conditions->clone(cg, generateRegisterDependencyConditions(cg, defaultChild->getFirstChild(), 0));
      }
   generateLabelInstruction(cg, TR::InstOpCode::b, node, defaultChild->getBranchDestination()->getNode()->getLabel(), conditions);

   if (tmpRegister)
      {
      cg->stopUsingRegister(tmpRegister);
      }

   cg->decReferenceCount(selectorNode);
   return NULL;
   }

TR::Register *
OMR::ARM64::TreeEvaluator::tableEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   int32_t numBranchTableEntries = node->getNumChildren() - 2;
   TR::Node *defaultChild = node->getSecondChild();
   TR::Register *selectorReg = cg->evaluate(node->getFirstChild());
   TR::Register *tmpRegister = NULL;
   TR::RegisterDependencyConditions *conditions;
   int32_t i;

   if (5 <= numBranchTableEntries)
      {
      conditions = new (cg->trHeapMemory()) TR::RegisterDependencyConditions(2, 2, cg->trMemory());
      tmpRegister = cg->allocateRegister();
      TR::addDependency(conditions, tmpRegister, TR::RealRegister::NoReg, TR_GPR, cg);
      }
   else
      {
      conditions = new (cg->trHeapMemory()) TR::RegisterDependencyConditions(1, 1, cg->trMemory());
      }

   TR::addDependency(conditions, selectorReg, TR::RealRegister::NoReg, TR_GPR, cg);

   if (0 < defaultChild->getNumChildren())
      {
      cg->evaluate(defaultChild->getFirstChild());
      conditions = conditions->clone(cg, generateRegisterDependencyConditions(cg, defaultChild->getFirstChild(), 0));
      }

   if (5 > numBranchTableEntries)
      {
      for (i = 0; i < numBranchTableEntries; i++)
         {
         generateCompareImmInstruction(cg, node, selectorReg, i);
         generateConditionalBranchInstruction(cg, TR::InstOpCode::b_cond, node, node->getChild(2+i)->getBranchDestination()->getNode()->getLabel(), TR::CC_EQ);
         }

      generateLabelInstruction(cg, TR::InstOpCode::b, node, defaultChild->getBranchDestination()->getNode()->getLabel(), conditions);
      }
   else
      {
      if (!constantIsUnsignedImm12(numBranchTableEntries))
         {
         loadConstant32(cg, node, numBranchTableEntries, tmpRegister);
         generateCompareInstruction(cg, node, selectorReg, tmpRegister);
         }
      else
         {
         generateCompareImmInstruction(cg, node, selectorReg, numBranchTableEntries);
         }

      generateConditionalBranchInstruction(cg, TR::InstOpCode::b_cond, node, defaultChild->getBranchDestination()->getNode()->getLabel(), TR::CC_CS);
      generateTrg1ImmInstruction(cg, TR::InstOpCode::adr, node, tmpRegister, 12); // distance between this instruction to the jump table
      generateTrg1Src2ShiftedInstruction(cg, TR::InstOpCode::addx, node, tmpRegister, tmpRegister, selectorReg, TR::SH_LSL, 2);
      generateRegBranchInstruction(cg, TR::InstOpCode::br, node, tmpRegister);

      for (i = 2; i < node->getNumChildren()-1; i++)
         {
         generateLabelInstruction(cg, TR::InstOpCode::b, node, node->getChild(i)->getBranchDestination()->getNode()->getLabel());
         }
      generateLabelInstruction(cg, TR::InstOpCode::b, node, node->getChild(i)->getBranchDestination()->getNode()->getLabel(), conditions);
      }

   if (NULL != tmpRegister)
      cg->stopUsingRegister(tmpRegister);

   cg->decReferenceCount(node->getFirstChild());
   return NULL;
   }

TR::Register *
OMR::ARM64::TreeEvaluator::ZEROCHKEvaluator(TR::Node *node, TR::CodeGenerator *cg)
	{
	// TODO:ARM64: Enable TR::TreeEvaluator::ZEROCHKEvaluator in compiler/aarch64/codegen/TreeEvaluatorTable.hpp when Implemented.
	return OMR::ARM64::TreeEvaluator::unImpOpEvaluator(node, cg);
	}

TR::Register *
OMR::ARM64::TreeEvaluator::DIVCHKEvaluator(TR::Node *node, TR::CodeGenerator *cg)
	{
	// TODO:ARM64: Enable TR::TreeEvaluator::DIVCHKEvaluator in compiler/aarch64/codegen/TreeEvaluatorTable.hpp when Implemented.
	return OMR::ARM64::TreeEvaluator::unImpOpEvaluator(node, cg);
	}

TR::Register *
OMR::ARM64::TreeEvaluator::BNDCHKEvaluator(TR::Node *node, TR::CodeGenerator *cg)
	{
	// TODO:ARM64: Enable TR::TreeEvaluator::BNDCHKEvaluator in compiler/aarch64/codegen/TreeEvaluatorTable.hpp when Implemented.
	return OMR::ARM64::TreeEvaluator::unImpOpEvaluator(node, cg);
	}

static TR::Instruction *compareInts(TR::ARM64ConditionCode  branchType,
                                           TR::Node             *node,
                                           TR::CodeGenerator    *cg,
                                           TR::SymbolReference  *sr =  NULL)
   {
   TR::Node *secondChild = node->getSecondChild();
   TR::Node *firstChild = node->getFirstChild();
   TR::LabelSymbol *branchTarget = sr ? NULL : node->getBranchDestination()->getNode()->getLabel();
   TR::Register *src1Reg = cg->evaluate(firstChild);
   TR::Register *src2Reg = cg->evaluate(secondChild);
   TR::Instruction *result;
   bool foundConst = false;
   if (secondChild->getOpCode().isLoadConst())
      {
      int64_t value = secondChild->get64bitIntegralValue();
      int32_t cmpValue = secondChild->getInt();
      bool negated = cmpValue < 0 && cmpValue != 0x80000000;
      if (value >= (-(1 << 15)) && value <= ((1 << 15) - 1))
         {
         generateCompareImmInstruction(cg, node, src1Reg, value);
         }
      else
         {
         generateCompareInstruction(cg, node, src1Reg, src2Reg);
         }
      foundConst = true;
      }
   if(!foundConst)
      generateCompareInstruction(cg, node, src1Reg, src2Reg);

   TR::RegisterDependencyConditions *deps = NULL;
   if (node->getNumChildren() == 3)
      {
      TR::Node *thirdChild = node->getChild(2);

      TR_ASSERT(thirdChild->getOpCodeValue() == TR::GlRegDeps,
             "The third child of a compare must be a TR::GlRegDeps");

      cg->evaluate(thirdChild);

      // NB: must generate reg deps before the conditonal branch
      deps = generateRegisterDependencyConditions(cg, thirdChild, 0);
      thirdChild->decReferenceCount();  // IS it correct to decReferenceCount before the instruction generation ?!!!
      }

   if (!sr)
      {
      result = generateConditionalBranchInstruction(cg, TR::InstOpCode::b_cond, node, branchTarget, branchType, deps);
      }
   else
      {
      cg->addSnippet(new (cg->trHeapMemory()) TR::ARM64HelperCallSnippet(cg, node, branchTarget, sr));
      cg->machine()->setLinkRegisterKilled(true);
      }

   cg->decReferenceCount(firstChild);
   cg->decReferenceCount(secondChild);
   return NULL;
   }

TR::Register *
OMR::ARM64::TreeEvaluator::ArrayCopyBNDCHKEvaluator(TR::Node *node, TR::CodeGenerator *cg)
	{
   // TODO - check this code
   // check that child[0] >= child[1], if not branch to check failure
   //
   // If the first child is a constant and the second isn't, swap the children.
   //

   TR::Node *firstChild = node->getFirstChild();
   TR::Node *secondChild = node->getSecondChild();
   TR::SymbolReference *BNDCHKException = node->getSymbolReference();
   TR::Instruction *instr;

   if (firstChild->getOpCode().isLoadConst())
      {
      if (secondChild->getOpCode().isLoadConst())
         {
         if (firstChild->getInt() < secondChild->getInt())
            {
            // Check will always fail, just jump to the exception handler
            instr = generateImmSymInstruction(cg, TR::InstOpCode::bl, node, (uintptr_t)BNDCHKException->getMethodAddress(), NULL, BNDCHKException, NULL);
            cg->machine()->setLinkRegisterKilled(true);
            }
         else
            {
            // Check will always succeed, no need for an instruction
            instr = NULL;
            }
         cg->decReferenceCount(firstChild);
         cg->decReferenceCount(secondChild);
         }
      else
         {
         node->swapChildren();
         instr = compareInts(TR::CC_GT, node, cg, BNDCHKException);
         node->swapChildren();
         }
      }
   else
      {
      instr = compareInts(TR::CC_LT, node, cg, BNDCHKException);
      }

   if (instr)
      {
      instr->ARM64NeedsGCMap(cg, 0xFFFFFFFF);
      }

   return NULL;
	}

TR::Register *
OMR::ARM64::TreeEvaluator::ArrayStoreCHKEvaluator(TR::Node *node, TR::CodeGenerator *cg)
	{
	// TODO:ARM64: Enable TR::TreeEvaluator::ArrayStoreCHKEvaluator in compiler/aarch64/codegen/TreeEvaluatorTable.hpp when Implemented.
	return OMR::ARM64::TreeEvaluator::unImpOpEvaluator(node, cg);
	}

TR::Register *
OMR::ARM64::TreeEvaluator::ArrayCHKEvaluator(TR::Node *node, TR::CodeGenerator *cg)
	{
	// TODO:ARM64: Enable TR::TreeEvaluator::ArrayCHKEvaluator in compiler/aarch64/codegen/TreeEvaluatorTable.hpp when Implemented.
	return OMR::ARM64::TreeEvaluator::unImpOpEvaluator(node, cg);
	}

static TR::Register *
commonMinMaxEvaluator(TR::Node *node, bool is64bit, TR::ARM64ConditionCode cc, TR::CodeGenerator *cg)
   {
   TR::Node *firstChild = node->getFirstChild();
   TR::Register *src1Reg = cg->evaluate(firstChild);
   TR::Register *trgReg;

   if (cg->canClobberNodesRegister(firstChild))
      {
      trgReg = src1Reg; // use the first child as the target
      }
   else
      {
      trgReg = cg->allocateRegister();
      }

   TR_ASSERT(node->getNumChildren() == 2, "The number of children for imax/imin/lmax/lmin must be 2.");

   TR::Node *secondChild = node->getSecondChild();
   TR::Register *src2Reg = cg->evaluate(secondChild);

   // ToDo:
   // Optimize the code by using generateCompareImmInstruction() when possible
   generateCompareInstruction(cg, node, src1Reg, src2Reg, is64bit);

   TR::InstOpCode::Mnemonic op = is64bit ? TR::InstOpCode::cselx : TR::InstOpCode::cselw;
   generateCondTrg1Src2Instruction(cg, op, node, trgReg, src1Reg, src2Reg, cc);

   node->setRegister(trgReg);
   cg->decReferenceCount(firstChild);
   cg->decReferenceCount(secondChild);

   return trgReg;
   }

TR::Register *
OMR::ARM64::TreeEvaluator::imaxEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   return commonMinMaxEvaluator(node, false, TR::CC_GT, cg);
   }

TR::Register *
OMR::ARM64::TreeEvaluator::lmaxEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   return commonMinMaxEvaluator(node, true, TR::CC_GT, cg);
   }

TR::Register *
OMR::ARM64::TreeEvaluator::iminEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   return commonMinMaxEvaluator(node, false, TR::CC_LT, cg);
   }

TR::Register *
OMR::ARM64::TreeEvaluator::lminEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   return commonMinMaxEvaluator(node, true, TR::CC_LT, cg);
   }

// also handles lselect, bselect, sselect, aselect
TR::Register *
OMR::ARM64::TreeEvaluator::iselectEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   TR::Node *condNode = node->getChild(0);
   TR::Node *trueNode = node->getChild(1);
   TR::Node *falseNode = node->getChild(2);

   TR::Register *condReg = cg->evaluate(condNode);
   TR::Register *trueReg = cg->evaluate(trueNode);
   TR::Register *falseReg = cg->evaluate(falseNode);
   TR::Register *resultReg = trueReg;

   // Internal pointers cannot be handled since we cannot set the pinning array
   // on the result register without knowing which side of the select will be
   // taken.
   TR_ASSERT_FATAL_WITH_NODE(
      node,
      !trueReg->containsInternalPointer() && !falseReg->containsInternalPointer(),
      "Select nodes cannot have children that are internal pointers"
   );
   if (falseReg->containsCollectedReference())
      {
      if (cg->comp()->getOption(TR_TraceCG))
         traceMsg(
            cg->comp(),
            "Setting containsCollectedReference on result of select node in register %s\n",
            cg->getDebug()->getName(resultReg));
      resultReg->setContainsCollectedReference();
      }

   if (!cg->canClobberNodesRegister(trueNode))
      {
      resultReg = (node->getOpCodeValue() == TR::aselect) ? cg->allocateCollectedReferenceRegister() : cg->allocateRegister();
      }

   generateCompareImmInstruction(cg, node, condReg, 0, true); // 64-bit compare
   generateCondTrg1Src2Instruction(cg, TR::InstOpCode::cselx, node, resultReg, trueReg, falseReg, TR::CC_NE);

   node->setRegister(resultReg);
   cg->decReferenceCount(condNode);
   cg->decReferenceCount(trueNode);
   cg->decReferenceCount(falseNode);

   return resultReg;
   }

static bool virtualGuardHelper(TR::Node *node, TR::CodeGenerator *cg)
   {
#ifdef J9_PROJECT_SPECIFIC
   if (!cg->willGenerateNOPForVirtualGuard(node))
      {
      return false;
      }

   TR::Compilation *comp = cg->comp();
   TR_VirtualGuard *virtualGuard = comp->findVirtualGuardInfo(node);

   TR_VirtualGuardSite *site = NULL;

   if (cg->comp()->compileRelocatableCode())
      {
      site = (TR_VirtualGuardSite *)comp->addAOTNOPSite();
      TR_AOTGuardSite *aotSite = (TR_AOTGuardSite *)site;
      aotSite->setType(virtualGuard->getKind());
      aotSite->setNode(node);

      switch (virtualGuard->getKind())
         {
         case TR_DirectMethodGuard:
         case TR_NonoverriddenGuard:
         case TR_InterfaceGuard:
         case TR_MethodEnterExitGuard:
         case TR_HCRGuard:
         case TR_AbstractGuard:
         case TR_BreakpointGuard:
            aotSite->setGuard(virtualGuard);
            break;

         case TR_ProfiledGuard:
            break;

         default:
            TR_ASSERT(0, "got AOT guard in node but virtual guard not one of known guards supported for AOT. Guard: %d", virtualGuard->getKind());
            break;
         }
      }
   else if (!node->isSideEffectGuard())
      {
      site = virtualGuard->addNOPSite();
      }
   else
      site = comp->addSideEffectNOPSite();

   TR::RegisterDependencyConditions *deps;
   if (node->getNumChildren() == 3)
      {
      TR::Node *third = node->getChild(2);
      cg->evaluate(third);
      deps = generateRegisterDependencyConditions(cg, third, 0);
      }
   else
      deps = new (cg->trHeapMemory()) TR::RegisterDependencyConditions(0, 0, cg->trMemory());

   if(virtualGuard->shouldGenerateChildrenCode())
      cg->evaluateChildrenWithMultipleRefCount(node);

   TR::LabelSymbol *label = node->getBranchDestination()->getNode()->getLabel();
   generateVirtualGuardNOPInstruction(cg, node, site, deps, label);
   cg->recursivelyDecReferenceCount(node->getFirstChild());
   cg->recursivelyDecReferenceCount(node->getSecondChild());

   return true;
#else
   return false;
#endif
   }

TR::Register *OMR::ARM64::TreeEvaluator::igotoEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   TR::Node *labelAddr = node->getFirstChild();
   TR::Register *addrReg = cg->evaluate(labelAddr);
   TR::RegisterDependencyConditions *deps = NULL;
   if (node->getNumChildren() > 1)
      {
      TR_ASSERT(node->getNumChildren() == 2 && node->getChild(1)->getOpCodeValue() == TR::GlRegDeps, "igoto has maximum of two children and second one must be global register dependency");
      TR::Node *glregdep = node->getChild(1);
      cg->evaluate(glregdep);
      deps = generateRegisterDependencyConditions(cg, glregdep, 0);
      cg->decReferenceCount(glregdep);
      }
   if (deps)
      generateRegBranchInstruction(cg, TR::InstOpCode::br, node, addrReg, deps);
   else
      generateRegBranchInstruction(cg, TR::InstOpCode::br, node, addrReg);
   cg->decReferenceCount(labelAddr);
   node->setRegister(NULL);
   return NULL;
   }
