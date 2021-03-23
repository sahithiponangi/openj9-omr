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

#include "codegen/ARM64Instruction.hpp"
#include "codegen/ARM64ShiftCode.hpp"
#include "codegen/CodeGenerator.hpp"
#include "codegen/ConstantDataSnippet.hpp"
#include "codegen/GenerateInstructions.hpp"
#include "codegen/Linkage.hpp"
#include "codegen/Linkage_inlines.hpp"
#include "codegen/MemoryReference.hpp"
#include "codegen/RegisterDependency.hpp"
#include "codegen/Relocation.hpp"
#include "codegen/TreeEvaluator.hpp"
#include "il/AutomaticSymbol.hpp"
#include "il/LabelSymbol.hpp"
#include "il/Node.hpp"
#include "il/Node_inlines.hpp"
#include "il/ParameterSymbol.hpp"
#include "il/StaticSymbol.hpp"

TR::Instruction *loadAddressConstantInSnippet(TR::CodeGenerator *cg, TR::Node *node, intptr_t address, TR::Register *targetRegister, TR_ExternalRelocationTargetKind reloKind, TR::Instruction *cursor)
   {
   // We use LDR literal to load a value from the snippet. Offset to PC will be patched by LabelRelative24BitRelocation
   auto snippet = cg->findOrCreate8ByteConstant(node, address);
   auto labelSym = snippet->getSnippetLabel();
   snippet->setReloType(reloKind);
   return generateTrg1ImmSymInstruction(cg, TR::InstOpCode::ldrx, node, targetRegister, 0, labelSym, cursor);
   }

TR::Instruction *loadConstant32(TR::CodeGenerator *cg, TR::Node *node, int32_t value, TR::Register *trgReg, TR::Instruction *cursor)
   {
   TR::Instruction *insertingInstructions = cursor;
   if (cursor == NULL)
      cursor = cg->getAppendInstruction();

   TR::InstOpCode::Mnemonic op = TR::InstOpCode::bad;
   uint32_t imm;

   if (value >= 0 && value <= 65535)
      {
      op = TR::InstOpCode::movzw;
      imm = value & 0xFFFF;
      }
   else if (value >= -65535 && value < 0)
      {
      op = TR::InstOpCode::movnw;
      imm = ~value & 0xFFFF;
      }
   else if ((value & 0xFFFF) == 0)
      {
      op = TR::InstOpCode::movzw;
      imm = ((value >> 16) & 0xFFFF) | TR::MOV_LSL16;
      }
   else if ((value & 0xFFFF) == 0xFFFF)
      {
      op = TR::InstOpCode::movnw;
      imm = ((~value >> 16) & 0xFFFF) | TR::MOV_LSL16;
      }

   if (op != TR::InstOpCode::bad)
      {
      cursor = generateTrg1ImmInstruction(cg, op, node, trgReg, imm, cursor);
      }
   else
      {
      // need two instructions
      cursor = generateTrg1ImmInstruction(cg, TR::InstOpCode::movzw, node, trgReg,
                                          (value & 0xFFFF), cursor);
      cursor = generateTrg1ImmInstruction(cg, TR::InstOpCode::movkw, node, trgReg,
                                          (((value >> 16) & 0xFFFF) | TR::MOV_LSL16), cursor);
      }

   if (!insertingInstructions)
      cg->setAppendInstruction(cursor);

   return cursor;
   }

TR::Instruction *loadConstant64(TR::CodeGenerator *cg, TR::Node *node, int64_t value, TR::Register *trgReg, TR::Instruction *cursor)
   {
   TR::Instruction *insertingInstructions = cursor;
   if (cursor == NULL)
      cursor = cg->getAppendInstruction();

   if (value == 0LL)
      {
      // 0
      cursor = generateTrg1ImmInstruction(cg, TR::InstOpCode::movzx, node, trgReg, 0, cursor);
      }
   else if (~value == 0LL)
      {
      // -1
      cursor = generateTrg1ImmInstruction(cg, TR::InstOpCode::movnx, node, trgReg, 0, cursor);
      }
   else
      {
      uint16_t h[4];
      int32_t count0000 = 0, countFFFF = 0;
      int32_t use_movz;
      int32_t i;

      for (i = 0; i < 4; i++)
         {
         h[i] = (value >> (i * 16)) & 0xFFFF;
         if (h[i] == 0)
            {
            count0000++;
            }
         else if (h[i] == 0xFFFF)
            {
            countFFFF++;
            }
         }
      use_movz = (count0000 >= countFFFF);

      TR::Instruction *start = cursor;

      for (i = 0; i < 4; i++)
         {
         uint32_t shift = TR::MOV_LSL16 * i;
         TR::InstOpCode::Mnemonic op = TR::InstOpCode::bad;
         uint32_t imm;

         if (use_movz && (h[i] != 0))
            {
            imm = h[i] | shift;
            if (cursor != start)
               {
               op = TR::InstOpCode::movkx;
               }
            else
               {
               op = TR::InstOpCode::movzx;
               }
            }
         else if (!use_movz && (h[i] != 0xFFFF))
            {
            if (cursor != start)
               {
               op = TR::InstOpCode::movkx;
               imm = h[i] | shift;
               }
            else
               {
               op = TR::InstOpCode::movnx;
               imm = (~h[i] & 0xFFFF) | shift;
               }
            }

         if (op != TR::InstOpCode::bad)
            {
            cursor = generateTrg1ImmInstruction(cg, op, node, trgReg, imm, cursor);
            }
         else
            {
            // generate no instruction here
            }
         }
      }

   if (!insertingInstructions)
      cg->setAppendInstruction(cursor);

   return cursor;
   }

TR::Instruction *addConstant64(TR::CodeGenerator *cg, TR::Node *node, TR::Register *trgReg, TR::Register *srcReg, int64_t value)
   {
   TR::Instruction *cursor;

   if (constantIsUnsignedImm12(value))
      {
      cursor = generateTrg1Src1ImmInstruction(cg, TR::InstOpCode::addimmx, node, trgReg, srcReg, value);
      }
   else
      {
      TR::Register *tempReg = cg->allocateRegister();
      loadConstant64(cg, node, value, tempReg);
      cursor = generateTrg1Src2Instruction(cg, TR::InstOpCode::addx, node, trgReg, srcReg, tempReg);
      cg->stopUsingRegister(tempReg);
      }

   return cursor;
   }

TR::Instruction *addConstant32(TR::CodeGenerator *cg, TR::Node *node, TR::Register *trgReg, TR::Register *srcReg, int32_t value)
   {
   TR::Instruction *cursor;

   if (constantIsUnsignedImm12(value))
      {
      cursor = generateTrg1Src1ImmInstruction(cg, TR::InstOpCode::addimmw, node, trgReg, srcReg, value);
      }
   else
      {
      TR::Register *tempReg = cg->allocateRegister();
      loadConstant32(cg, node, value, tempReg);
      cursor = generateTrg1Src2Instruction(cg, TR::InstOpCode::addw, node, trgReg, srcReg, tempReg);
      cg->stopUsingRegister(tempReg);
      }

   return cursor;
   }

/**
 * Add meta data to instruction loading address constant
 * @param[in] cg : CodeGenerator
 * @param[in] node : node
 * @param[in] firstInstruction : instruction cursor
 * @param[in] typeAddress : type of address
 * @param[in] value : address value
 */
static void
addMetaDataForLoadAddressConstantFixed(TR::CodeGenerator *cg, TR::Node *node, TR::Instruction *firstInstruction, int16_t typeAddress, intptr_t value)
   {
   if (value == 0x0)
      return;

   if (typeAddress == -1)
      typeAddress = TR_FixedSequenceAddress2;

   TR::Compilation *comp = cg->comp();

   TR::Relocation *relo = NULL;

   switch (typeAddress)
      {
      case TR_DataAddress:
         {
         relo = new (cg->trHeapMemory()) TR::BeforeBinaryEncodingExternalRelocation(
            firstInstruction,
            (uint8_t *)node->getSymbolReference(),
            (uint8_t *)node->getInlinedSiteIndex(),
            TR_DataAddress, cg);
         break;
         }

      case TR_DebugCounter:
         {
         TR::DebugCounterBase *counter = comp->getCounterFromStaticAddress(node->getSymbolReference());
         if (counter == NULL)
            comp->failCompilation<TR::CompilationException>("Could not generate relocation for debug counter in addMetaDataForLoadAddressConstantFixed\n");

         TR::DebugCounter::generateRelocation(comp, firstInstruction, node, counter);
         return;
         }

      case TR_ClassAddress:
         {
         if (comp->getOption(TR_UseSymbolValidationManager))
            {
            TR::SymbolReference *symRef = (TR::SymbolReference *)value;

            relo = new (cg->trHeapMemory()) TR::BeforeBinaryEncodingExternalRelocation(
               firstInstruction,
               (uint8_t *)symRef->getSymbol()->getStaticSymbol()->getStaticAddress(),
               (uint8_t *)TR::SymbolType::typeClass,
               TR_DiscontiguousSymbolFromManager, cg);
            }
         else
            {
            TR::SymbolReference *symRef = (TR::SymbolReference *)value;

            relo = new (cg->trHeapMemory()) TR::BeforeBinaryEncodingExternalRelocation(
               firstInstruction,
               (uint8_t *)symRef,
               (uint8_t *)(node == NULL ? -1 : node->getInlinedSiteIndex()),
               TR_ClassAddress, cg);
            }
         break;
         }

      case TR_RamMethodSequence:
         {
         if (comp->getOption(TR_UseSymbolValidationManager))
            {
            relo = new (cg->trHeapMemory()) TR::BeforeBinaryEncodingExternalRelocation(
               firstInstruction,
               (uint8_t *)comp->getJittedMethodSymbol()->getResolvedMethod()->resolvedMethodAddress(),
               (uint8_t *)TR::SymbolType::typeMethod,
               TR_DiscontiguousSymbolFromManager,
               cg);
            }
         break;
         }
      }

   if (!relo)
      {
      relo = new (cg->trHeapMemory()) TR::BeforeBinaryEncodingExternalRelocation(
         firstInstruction,
         (uint8_t *)value,
         (TR_ExternalRelocationTargetKind)typeAddress,
         cg);
      }

   cg->addExternalRelocation(
      relo,
      __FILE__,
      __LINE__,
      node);
   }

/**
 * Generates relocatable instructions for loading 64-bit integer value to a register
 * @param[in] cg : CodeGenerator
 * @param[in] node : node
 * @param[in] value : integer value
 * @param[in] trgReg : target register
 * @param[in] cursor : instruction cursor
 * @param[in] typeAddress : type of address
 */ 
static TR::Instruction *
loadAddressConstantRelocatable(TR::CodeGenerator *cg, TR::Node *node, intptr_t value, TR::Register *trgReg, TR::Instruction *cursor=NULL, int16_t typeAddress = -1)
   {
   TR::Compilation *comp = cg->comp();
   // load a 64-bit constant into a register with a fixed 4 instruction sequence
   TR::Instruction *temp = cursor;
   TR::Instruction *firstInstruction;

   if (cursor == NULL)
      cursor = cg->getAppendInstruction();

   cursor = firstInstruction = generateTrg1ImmInstruction(cg, TR::InstOpCode::movzx, node, trgReg, value & 0x0000ffff, cursor);
   cursor = generateTrg1ImmInstruction(cg, TR::InstOpCode::movkx, node, trgReg, ((value >> 16) & 0x0000ffff) | TR::MOV_LSL16, cursor);
   cursor = generateTrg1ImmInstruction(cg, TR::InstOpCode::movkx, node, trgReg, ((value >> 32) & 0x0000ffff) | (TR::MOV_LSL16 * 2) , cursor);
   cursor = generateTrg1ImmInstruction(cg, TR::InstOpCode::movkx, node, trgReg, (value >> 48) | (TR::MOV_LSL16 * 3) , cursor);

   addMetaDataForLoadAddressConstantFixed(cg, node, firstInstruction, typeAddress, value);

   if (temp == NULL)
      cg->setAppendInstruction(cursor);

   return cursor;
   }

TR::Instruction *
loadAddressConstant(TR::CodeGenerator *cg, TR::Node *node, intptr_t value, TR::Register *trgReg, TR::Instruction *cursor, bool isPicSite, int16_t typeAddress)
   {
   if (cg->comp()->compileRelocatableCode())
      {
      return loadAddressConstantRelocatable(cg, node, value, trgReg, cursor, typeAddress);
      }

   return loadConstant64(cg, node, value, trgReg, cursor);
   }

TR::Register *
OMR::ARM64::TreeEvaluator::unImpOpEvaluator(TR::Node *node, TR::CodeGenerator *cg)
	{
	TR_ASSERT_FATAL(false, "Opcode %s is not implemented\n", node->getOpCode().getName());
	return NULL;
	}

TR::Register *
OMR::ARM64::TreeEvaluator::badILOpEvaluator(TR::Node *node, TR::CodeGenerator *cg)
	{
	// TODO:ARM64: Enable TR::TreeEvaluator::badILOpEvaluator in compiler/aarch64/codegen/TreeEvaluatorTable.hpp when Implemented.
	return OMR::ARM64::TreeEvaluator::unImpOpEvaluator(node, cg);
	}

TR::Register *commonLoadEvaluator(TR::Node *node, TR::InstOpCode::Mnemonic op, TR::CodeGenerator *cg)
   {
   TR::Register *tempReg;
   bool needSync = (node->getSymbolReference()->getSymbol()->isSyncVolatile() && cg->comp()->target().isSMP());

   if (op == TR::InstOpCode::vldrimms)
      {
      tempReg = cg->allocateSinglePrecisionRegister();
      }
   else if (op == TR::InstOpCode::vldrimmd)
      {
      tempReg = cg->allocateRegister(TR_FPR);
      }
   else
      {
      tempReg = cg->allocateRegister();
      }
   node->setRegister(tempReg);
   TR::MemoryReference *tempMR = new (cg->trHeapMemory()) TR::MemoryReference(node, cg);
   generateTrg1MemInstruction(cg, op, node, tempReg, tempMR);

   if (needSync)
      {
      generateSynchronizationInstruction(cg, TR::InstOpCode::dmb, node, 0xF); // dmb SY
      }

   tempMR->decNodeReferenceCounts(cg);

   return tempReg;
   }

// also handles iloadi
TR::Register *
OMR::ARM64::TreeEvaluator::iloadEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   return commonLoadEvaluator(node, TR::InstOpCode::ldrimmw, cg);
   }

// also handles aloadi
TR::Register *
OMR::ARM64::TreeEvaluator::aloadEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   TR::Compilation *comp = cg->comp();
   TR::Register *tempReg;

   if (!node->getSymbolReference()->getSymbol()->isInternalPointer())
      {
      if (node->getSymbolReference()->getSymbol()->isNotCollected())
         tempReg = cg->allocateRegister();
      else
         tempReg = cg->allocateCollectedReferenceRegister();
      }
   else
      {
      tempReg = cg->allocateRegister();
      tempReg->setPinningArrayPointer(node->getSymbolReference()->getSymbol()->castToInternalPointerAutoSymbol()->getPinningArrayPointer());
      tempReg->setContainsInternalPointer();
      }

   node->setRegister(tempReg);

   TR::InstOpCode::Mnemonic op;

   if (TR::Compiler->om.generateCompressedObjectHeaders() &&
       (node->getSymbol()->isClassObject() ||
        (node->getSymbolReference() == comp->getSymRefTab()->findVftSymbolRef())))
      {
      op = TR::InstOpCode::ldrimmw;
      }
   else
      {
      op = TR::InstOpCode::ldrimmx;
      }
   TR::MemoryReference *tempMR = new (cg->trHeapMemory()) TR::MemoryReference(node, cg);
   generateTrg1MemInstruction(cg, op, node, tempReg, tempMR);

   if (node->getSymbolReference() == comp->getSymRefTab()->findVftSymbolRef())
      {
      TR::TreeEvaluator::generateVFTMaskInstruction(cg, node, tempReg);
      }

   bool needSync = (node->getSymbolReference()->getSymbol()->isSyncVolatile() && cg->comp()->target().isSMP());
   if (needSync)
      {
      generateSynchronizationInstruction(cg, TR::InstOpCode::dmb, node, 0xF); // dmb SY
      }

   tempMR->decNodeReferenceCounts(cg);

   return tempReg;
   }

// also handles lloadi
TR::Register *
OMR::ARM64::TreeEvaluator::lloadEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   return commonLoadEvaluator(node, TR::InstOpCode::ldrimmx, cg);
   }

// also handles bloadi
TR::Register *
OMR::ARM64::TreeEvaluator::bloadEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   return commonLoadEvaluator(node, TR::InstOpCode::ldrsbimmx, cg);
   }

// also handles sloadi
TR::Register *
OMR::ARM64::TreeEvaluator::sloadEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   return commonLoadEvaluator(node, TR::InstOpCode::ldrshimmx, cg);
   }

TR::Register *
OMR::ARM64::TreeEvaluator::cloadEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   return commonLoadEvaluator(node, TR::InstOpCode::ldrhimm, cg);
   }

TR::Register *
OMR::ARM64::TreeEvaluator::awrtbarEvaluator(TR::Node *node, TR::CodeGenerator *cg)
	{
	// TODO:ARM64: Enable TR::TreeEvaluator::awrtbarEvaluator in compiler/aarch64/codegen/TreeEvaluatorTable.hpp when Implemented.
	return OMR::ARM64::TreeEvaluator::unImpOpEvaluator(node, cg);
	}

TR::Register *commonStoreEvaluator(TR::Node *node, TR::InstOpCode::Mnemonic op, TR::CodeGenerator *cg)
   {
   TR::MemoryReference *tempMR = new (cg->trHeapMemory()) TR::MemoryReference(node, cg);
   bool needSync = (node->getSymbolReference()->getSymbol()->isSyncVolatile() && cg->comp()->target().isSMP());
   bool lazyVolatile = false;
   if (node->getSymbolReference()->getSymbol()->isShadow() &&
       node->getSymbolReference()->getSymbol()->isOrdered() && cg->comp()->target().isSMP())
      {
      needSync = true;
      lazyVolatile = true;
      }

   TR::Node *valueChild;

   if (node->getOpCode().isIndirect())
      {
      valueChild = node->getSecondChild();
      }
   else
      {
      valueChild = node->getFirstChild();
      }

   if (needSync)
      {
      generateSynchronizationInstruction(cg, TR::InstOpCode::dmb, node, 0xE); // dmb ST
      }
   generateMemSrc1Instruction(cg, op, node, tempMR, cg->evaluate(valueChild));
   if (needSync)
      {
      // ordered and lazySet operations will not generate a post-write sync
      if (!lazyVolatile)
         {
         generateSynchronizationInstruction(cg, TR::InstOpCode::dmb, node, 0xF); // dmb SY
         }
      }

   cg->decReferenceCount(valueChild);
   tempMR->decNodeReferenceCounts(cg);

   return NULL;
   }

// also handles lstorei
TR::Register *
OMR::ARM64::TreeEvaluator::lstoreEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   return commonStoreEvaluator(node, TR::InstOpCode::strimmx, cg);
   }

// also handles bstorei
TR::Register *
OMR::ARM64::TreeEvaluator::bstoreEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   return commonStoreEvaluator(node, TR::InstOpCode::strbimm, cg);
   }

// also handles sstorei
TR::Register *
OMR::ARM64::TreeEvaluator::sstoreEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   return commonStoreEvaluator(node, TR::InstOpCode::strhimm, cg);
   }

// also handles istorei
TR::Register *
OMR::ARM64::TreeEvaluator::istoreEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   TR::Compilation *comp = cg->comp();

   commonStoreEvaluator(node, TR::InstOpCode::strimmw, cg);

   if (comp->useCompressedPointers() && node->getOpCode().isIndirect())
      node->setStoreAlreadyEvaluated(true);

   return NULL;
   }

// also handles astore, astorei
TR::Register *
OMR::ARM64::TreeEvaluator::astoreEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   TR::Compilation *comp = cg->comp();
   bool isCompressedClassPointerOfObjectHeader = TR::Compiler->om.generateCompressedObjectHeaders() &&
         (node->getSymbol()->isClassObject() ||
         (node->getSymbolReference() == comp->getSymRefTab()->findVftSymbolRef()));
   TR::InstOpCode::Mnemonic op = isCompressedClassPointerOfObjectHeader ? TR::InstOpCode::strimmw : TR::InstOpCode::strimmx;

   return commonStoreEvaluator(node, op, cg);
   }

TR::Register *
OMR::ARM64::TreeEvaluator::monentEvaluator(TR::Node *node, TR::CodeGenerator *cg)
	{
	// TODO:ARM64: Enable TR::TreeEvaluator::monentEvaluator in compiler/aarch64/codegen/TreeEvaluatorTable.hpp when Implemented.
	return OMR::ARM64::TreeEvaluator::unImpOpEvaluator(node, cg);
	}

TR::Register *
OMR::ARM64::TreeEvaluator::monexitEvaluator(TR::Node *node, TR::CodeGenerator *cg)
	{
	// TODO:ARM64: Enable TR::TreeEvaluator::monexitEvaluator in compiler/aarch64/codegen/TreeEvaluatorTable.hpp when Implemented.
	return OMR::ARM64::TreeEvaluator::unImpOpEvaluator(node, cg);
	}

TR::Register *
OMR::ARM64::TreeEvaluator::arraytranslateAndTestEvaluator(TR::Node *node, TR::CodeGenerator *cg)
	{
	// TODO:ARM64: Enable TR::TreeEvaluator::arraytranslateAndTestEvaluator in compiler/aarch64/codegen/TreeEvaluatorTable.hpp when Implemented.
	return OMR::ARM64::TreeEvaluator::unImpOpEvaluator(node, cg);
	}

TR::Register *
OMR::ARM64::TreeEvaluator::arraytranslateEvaluator(TR::Node *node, TR::CodeGenerator *cg)
	{
	// TODO:ARM64: Enable TR::TreeEvaluator::arraytranslateEvaluator in compiler/aarch64/codegen/TreeEvaluatorTable.hpp when Implemented.
	return OMR::ARM64::TreeEvaluator::unImpOpEvaluator(node, cg);
	}

TR::Register *
OMR::ARM64::TreeEvaluator::arraysetEvaluator(TR::Node *node, TR::CodeGenerator *cg)
	{
	// TODO:ARM64: Enable TR::TreeEvaluator::arraysetEvaluator in compiler/aarch64/codegen/TreeEvaluatorTable.hpp when Implemented.
	return OMR::ARM64::TreeEvaluator::unImpOpEvaluator(node, cg);
	}

TR::Register *
OMR::ARM64::TreeEvaluator::arraycmpEvaluator(TR::Node *node, TR::CodeGenerator *cg)
	{
	// TODO:ARM64: Enable TR::TreeEvaluator::arraycmpEvaluator in compiler/aarch64/codegen/TreeEvaluatorTable.hpp when Implemented.
	return OMR::ARM64::TreeEvaluator::unImpOpEvaluator(node, cg);
	}

static void inlineArrayCopy(TR::Node *node, int64_t byteLen, TR::Register *src, TR::Register *dst, TR::CodeGenerator *cg)
   {
   if (byteLen == 0)
      return;

   TR::Register *regs[4] = {NULL, NULL, NULL, NULL};
   TR::Register *fpRegs[4] = {NULL, NULL, NULL, NULL};
   int32_t groups, residual, regIx=0, ix=0, fpRegIx=0;
   uint8_t numDeps = 11;
   int32_t memRefSize;
   TR::Compilation* comp = cg->comp();

   memRefSize = TR::Compiler->om.sizeofReferenceAddress();

   TR::RealRegister::RegNum tempDep, srcDep, dstDep, cndDep;
   tempDep = TR::RealRegister::NoReg;
   srcDep = TR::RealRegister::NoReg;
   dstDep = TR::RealRegister::NoReg;
   cndDep = TR::RealRegister::NoReg;


   TR::RegisterDependencyConditions *conditions = new (cg->trHeapMemory()) TR::RegisterDependencyConditions(numDeps, numDeps, cg->trMemory());

   TR::addDependency(conditions, src, srcDep, TR_GPR, cg);
   TR::addDependency(conditions, dst, dstDep, TR_GPR, cg);

   TR::LabelSymbol *doneLabel = generateLabelSymbol(cg);


   //  byteLen reg
   regs[0] = cg->allocateRegister(TR_GPR);
   TR::addDependency(conditions, regs[0], TR::RealRegister::NoReg, TR_GPR, cg);

   //  First VSX vector reg
   //  regs[1] = cg->allocateRegister(TR_VSX_VECTOR);
   //  TR::addDependency(conditions, regs[1], TR::RealRegister::NoReg, TR_VSX_VECTOR, cg);

   int32_t iteration64 = byteLen >> 6, residue64 = byteLen & 0x3F, standingOffset = 0;

   if (iteration64 > 0)
      {
      TR::LabelSymbol *loopStart;
      TR::Register    *cndReg;

      if (iteration64 > 1)
         {
         generateTrg1ImmInstruction(cg, TR::InstOpCode::ldrimmw, node, regs[0], iteration64);
         generateSrc1Instruction(cg, TR::InstOpCode::ldrimmw, node, regs[0]);

         cndReg = cg->allocateRegister(TR_GPR);
         TR::addDependency(conditions, cndReg, TR::RealRegister::NoReg, TR_GPR, cg);

         loopStart = generateLabelSymbol(cg);
         generateLabelInstruction(cg, TR::InstOpCode::label, node, loopStart);
         }

/*       generateTrg1MemInstruction(cg, TR::InstOpCode::lxv, node, regs[1], TR::MemoryReference::createWithDisplacement(cg, src, 0, 16));
         generateMemSrc1Instruction(cg, TR::InstOpCode::stxv, node, TR::MemoryReference::createWithDisplacement(cg, dst, 0, 16), regs[1]);
         generateTrg1MemInstruction(cg, TR::InstOpCode::lxv, node, regs[1], TR::MemoryReference::createWithDisplacement(cg, src, 16, 16));
         generateMemSrc1Instruction(cg, TR::InstOpCode::stxv, node, TR::MemoryReference::createWithDisplacement(cg, dst, 16, 16), regs[1]);
         generateTrg1MemInstruction(cg, TR::InstOpCode::lxv, node, regs[1], TR::MemoryReference::createWithDisplacement(cg, src, 32, 16));
         generateMemSrc1Instruction(cg, TR::InstOpCode::stxv, node, TR::MemoryReference::createWithDisplacement(cg, dst, 32, 16), regs[1]);
         generateTrg1MemInstruction(cg, TR::InstOpCode::lxv, node, regs[1], TR::MemoryReference::createWithDisplacement(cg, src, 48, 16));
         generateMemSrc1Instruction(cg, TR::InstOpCode::stxv, node, TR::MemoryReference::createWithDisplacement(cg, dst, 48, 16), regs[1]); */

         if (iteration64 > 1)
            {
            generateTrg1Src1ImmInstruction(cg, TR::InstOpCode::addimmx, node, src, src, 64);
            generateTrg1Src1ImmInstruction(cg, TR::InstOpCode::addimmx, node, dst, dst, 64);
            generateConditionalBranchInstruction(cg, TR::InstOpCode::cbnzx, node, loopStart, TR::CC_NE);
            }
         else
            standingOffset = 64;
         }

/*       for (int32_t i = 0; i < (residue64>>4); i++)
         {
         generateTrg1MemInstruction(cg, TR::InstOpCode::lxv, node, regs[1], TR::MemoryReference::createWithDisplacement(cg, src, standingOffset+i*16, 16));
         generateMemSrc1Instruction(cg, TR::InstOpCode::stxv, node, TR::MemoryReference::createWithDisplacement(cg, dst, standingOffset+i*16, 16), regs[1]);
         } */

      if ((residue64 & 0xF) != 0)
         {
         standingOffset += residue64 & 0x30;
         switch (residue64 & 0xF)
            {
            case 1:
               generateTrg1MemInstruction(cg, TR::InstOpCode::ldpoffx, node, regs[0], new (cg->trHeapMemory()) TR::MemoryReference(src, standingOffset, cg));
               generateMemSrc1Instruction(cg, TR::InstOpCode::stpoffx, node, new (cg->trHeapMemory()) TR::MemoryReference(dst, standingOffset, cg), regs[0]);
               break;
            case 2:
               generateTrg1MemInstruction(cg, TR::InstOpCode::ldpoffx, node, regs[0], new (cg->trHeapMemory()) TR::MemoryReference(src, standingOffset, cg));
               generateMemSrc1Instruction(cg, TR::InstOpCode::stpoffx, node, new (cg->trHeapMemory()) TR::MemoryReference(dst, standingOffset, cg), regs[0]);
               break;
            case 4:
               generateTrg1MemInstruction(cg, TR::InstOpCode::ldpoffx, node, regs[0], new (cg->trHeapMemory()) TR::MemoryReference(src, standingOffset, cg));
               generateMemSrc1Instruction(cg, TR::InstOpCode::stpoffx, node, new (cg->trHeapMemory()) TR::MemoryReference(dst, standingOffset, cg), regs[0]);
               break;
            case 8:
               generateTrg1MemInstruction(cg, TR::InstOpCode::ldpoffx, node, regs[0], new (cg->trHeapMemory()) TR::MemoryReference(src, standingOffset, cg));
               generateMemSrc1Instruction(cg, TR::InstOpCode::stpoffx, node, new (cg->trHeapMemory()) TR::MemoryReference(dst, standingOffset, cg), regs[0]);
               break;
            default:
               generateTrg1ImmInstruction(cg, TR::InstOpCode::ldrimmw, node, regs[0], residue64 & 0xF);
               generateTrg1ImmInstruction(cg, TR::InstOpCode::movkw, node, regs[0],
                                               (((residue64 >> 16) & 0xFFFF) | TR::MOV_LSL16));
               if (standingOffset != 0)
                  {
                  generateTrg1Src1ImmInstruction(cg, TR::InstOpCode::addimmx, node, src, src, standingOffset);
                  generateTrg1Src1ImmInstruction(cg, TR::InstOpCode::addimmx, node, dst, dst, standingOffset);
                  }
            /* generateTrg1Src2Instruction(cg, TR::InstOpCode::lxvl, node, regs[1], src, regs[0]);
               generateSrc3Instruction(cg, TR::InstOpCode::stxvl, node, regs[1], dst, regs[0]); */
               break;
            }
         }


   generateLabelInstruction(cg, TR::InstOpCode::label, node, doneLabel, conditions);

   conditions->stopUsingDepRegs(cg);
   return;
   }

TR::Register *
OMR::ARM64::TreeEvaluator::asynccheckEvaluator(TR::Node *node, TR::CodeGenerator *cg)
	{
	// TODO:ARM64: Enable TR::TreeEvaluator::asynccheckEvaluator in compiler/aarch64/codegen/TreeEvaluatorTable.hpp when Implemented.
	return OMR::ARM64::TreeEvaluator::unImpOpEvaluator(node, cg);
	}

TR::Register *
OMR::ARM64::TreeEvaluator::instanceofEvaluator(TR::Node *node, TR::CodeGenerator *cg)
	{
	// TODO:ARM64: Enable TR::TreeEvaluator::instanceofEvaluator in compiler/aarch64/codegen/TreeEvaluatorTable.hpp when Implemented.
	return OMR::ARM64::TreeEvaluator::unImpOpEvaluator(node, cg);
	}

TR::Register *
OMR::ARM64::TreeEvaluator::checkcastEvaluator(TR::Node *node, TR::CodeGenerator *cg)
	{
	// TODO:ARM64: Enable TR::TreeEvaluator::checkcastEvaluator in compiler/aarch64/codegen/TreeEvaluatorTable.hpp when Implemented.
	return OMR::ARM64::TreeEvaluator::unImpOpEvaluator(node, cg);
	}

TR::Register *
OMR::ARM64::TreeEvaluator::checkcastAndNULLCHKEvaluator(TR::Node *node, TR::CodeGenerator *cg)
	{
	// TODO:ARM64: Enable TR::TreeEvaluator::checkcastAndNULLCHKEvaluator in compiler/aarch64/codegen/TreeEvaluatorTable.hpp when Implemented.
	return OMR::ARM64::TreeEvaluator::unImpOpEvaluator(node, cg);
	}

// handles call, icall, lcall, fcall, dcall, acall
TR::Register *
OMR::ARM64::TreeEvaluator::directCallEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   TR::SymbolReference *symRef = node->getSymbolReference();
   TR::MethodSymbol *callee = symRef->getSymbol()->castToMethodSymbol();
   TR::Linkage *linkage = cg->getLinkage(callee->getLinkageConvention());

   return linkage->buildDirectDispatch(node);
   }

// handles calli, icalli, lcalli, fcalli, dcalli, acalli
TR::Register *
OMR::ARM64::TreeEvaluator::indirectCallEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   TR::SymbolReference *symRef = node->getSymbolReference();
   TR::MethodSymbol *callee = symRef->getSymbol()->castToMethodSymbol();
   TR::Linkage *linkage = cg->getLinkage(callee->getLinkageConvention());

   return linkage->buildIndirectDispatch(node);
   }

TR::Register *
OMR::ARM64::TreeEvaluator::treetopEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   TR::Register *tempReg = cg->evaluate(node->getFirstChild());
   cg->decReferenceCount(node->getFirstChild());
   return tempReg;
   }

TR::Register *
OMR::ARM64::TreeEvaluator::exceptionRangeFenceEvaluator(TR::Node *node, TR::CodeGenerator *cg)
	{
	// TODO:ARM64: Enable TR::TreeEvaluator::exceptionRangeFenceEvaluator in compiler/aarch64/codegen/TreeEvaluatorTable.hpp when Implemented.
	return OMR::ARM64::TreeEvaluator::unImpOpEvaluator(node, cg);
	}

TR::Register *
OMR::ARM64::TreeEvaluator::loadaddrEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   TR::Register *resultReg;
   TR::Symbol *sym = node->getSymbol();
   TR::Compilation *comp = cg->comp();
   TR::MemoryReference *mref = new (cg->trHeapMemory()) TR::MemoryReference(node, node->getSymbolReference(), cg);

   if (mref->getUnresolvedSnippet() != NULL)
      {
      resultReg = sym->isLocalObject() ? cg->allocateCollectedReferenceRegister() : cg->allocateRegister();
      if (mref->useIndexedForm())
         {
         TR_ASSERT(false, "Unresolved indexed snippet is not supported");
         }
      else
         {
         TR_UNIMPLEMENTED();
         }
      }
   else
      {
      if (mref->useIndexedForm())
         {
         resultReg = sym->isLocalObject() ? cg->allocateCollectedReferenceRegister() : cg->allocateRegister();
         generateTrg1Src2Instruction(cg, TR::InstOpCode::addx, node, resultReg, mref->getBaseRegister(), mref->getIndexRegister());
         }
      else
         {
         int32_t offset = mref->getOffset();
         if (mref->hasDelayedOffset() || offset != 0)
            {
            resultReg = sym->isLocalObject() ? cg->allocateCollectedReferenceRegister() : cg->allocateRegister();
            if (mref->hasDelayedOffset())
               {
               generateTrg1MemInstruction(cg, TR::InstOpCode::addimmx, node, resultReg, mref);
               }
            else
               {
               if (offset >= 0 && constantIsUnsignedImm12(offset))
                  {
                  generateTrg1Src1ImmInstruction(cg, TR::InstOpCode::addimmx, node, resultReg, mref->getBaseRegister(), offset);
                  }
               else
                  {
                  loadConstant64(cg, node, offset, resultReg);
                  generateTrg1Src2Instruction(cg, TR::InstOpCode::addx, node, resultReg, mref->getBaseRegister(), resultReg);
                  }
               }
            }
         else
            {
            resultReg = mref->getBaseRegister();
            if (resultReg == cg->getMethodMetaDataRegister())
               {
               resultReg = cg->allocateRegister();
               generateMovInstruction(cg, node, resultReg, mref->getBaseRegister());
               }
            }
         }
      }
   node->setRegister(resultReg);
   mref->decNodeReferenceCounts(cg);
   return resultReg;
   }

TR::Register *
OMR::ARM64::TreeEvaluator::aRegLoadEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   TR::Register *globalReg = node->getRegister();

   if (globalReg == NULL)
      {
      if (node->getRegLoadStoreSymbolReference()->getSymbol()->isNotCollected() ||
          node->getRegLoadStoreSymbolReference()->getSymbol()->isInternalPointer())
         {
         globalReg = cg->allocateRegister();
         if (node->getRegLoadStoreSymbolReference()->getSymbol()->isInternalPointer())
            {
            globalReg->setContainsInternalPointer();
            globalReg->setPinningArrayPointer(node->getRegLoadStoreSymbolReference()->getSymbol()->castToInternalPointerAutoSymbol()->getPinningArrayPointer());
            }
         }
      else
         {
         globalReg = cg->allocateCollectedReferenceRegister();
         }

      node->setRegister(globalReg);
      }
   return globalReg;
   }

// Also handles sRegLoad, bRegLoad, and lRegLoad
TR::Register *
OMR::ARM64::TreeEvaluator::iRegLoadEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   TR::Register *globalReg = node->getRegister();

   if (globalReg == NULL)
      {
      globalReg = cg->allocateRegister();
      node->setRegister(globalReg);
      }
   return(globalReg);
   }

// Also handles sRegStore, bRegStore, lRegStore, and aRegStore
TR::Register *
OMR::ARM64::TreeEvaluator::iRegStoreEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   TR::Node *child = node->getFirstChild();
   TR::Register *globalReg = cg->evaluate(child);
   cg->decReferenceCount(child);
   return globalReg;
   }

TR::Register *
OMR::ARM64::TreeEvaluator::GlRegDepsEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   int32_t i;

   for (i = 0; i < node->getNumChildren(); i++)
      {
      cg->evaluate(node->getChild(i));
      cg->decReferenceCount(node->getChild(i));
      }
   return NULL;
   }

TR::Register *
OMR::ARM64::TreeEvaluator::BBStartEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   TR::Compilation *comp = cg->comp();
   TR::Block *block = node->getBlock();
   cg->setCurrentBlock(block);

   TR::RegisterDependencyConditions *deps = NULL;

   if (!block->isExtensionOfPreviousBlock())
      {
      TR::Machine *machine = cg->machine();
      // REG ASSOC
      machine->clearRegisterAssociations();
      machine->setRegisterWeightsFromAssociations();

      if (node->getNumChildren() > 0)
         {
         int32_t i;
         TR::Node *child = node->getFirstChild();

         cg->evaluate(child);

         deps = generateRegisterDependencyConditions(cg, child, 0);
         if (cg->getCurrentEvaluationTreeTop() == comp->getStartTree())
            {
            for (i=0; i<child->getNumChildren(); i++)
               {
               TR::ParameterSymbol *sym = child->getChild(i)->getSymbol()->getParmSymbol();
               if (sym != NULL)
                  {
                  sym->setAssignedGlobalRegisterIndex(cg->getGlobalRegister(child->getChild(i)->getGlobalRegisterNumber()));
                  }
               }
            }
         cg->decReferenceCount(child);
         }
      }

   TR::LabelSymbol *labelSym = node->getLabel();
   if (!labelSym)
      {
      labelSym = generateLabelSymbol(cg);
      node->setLabel(labelSym);
      }
   TR::Instruction *labelInst = generateLabelInstruction(cg, TR::InstOpCode::label, node, labelSym, deps);
   labelSym->setInstruction(labelInst);
   block->setFirstInstruction(labelInst);

   TR::Node *fenceNode = TR::Node::createRelative32BitFenceNode(node, &block->getInstructionBoundaries()._startPC);
   TR::Instruction *fence = generateAdminInstruction(cg, TR::InstOpCode::fence, node, fenceNode);

   if (block->isCatchBlock())
      {
      cg->generateCatchBlockBBStartPrologue(node, fence);
      }

   return NULL;
   }

TR::Register *
OMR::ARM64::TreeEvaluator::BBEndEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   TR::Block *block = node->getBlock();
   TR::Compilation *comp = cg->comp();
   TR::Node *fenceNode = TR::Node::createRelative32BitFenceNode(node, &node->getBlock()->getInstructionBoundaries()._endPC);

   if (NULL == block->getNextBlock())
      {
      TR::Instruction *lastInstruction = cg->getAppendInstruction();
      if (lastInstruction->getOpCodeValue() == TR::InstOpCode::bl
              && lastInstruction->getNode()->getSymbolReference()->getReferenceNumber() == TR_aThrow)
         {
         lastInstruction = generateInstruction(cg, TR::InstOpCode::bad, node, lastInstruction);
         }
      }

   TR::TreeTop *nextTT = cg->getCurrentEvaluationTreeTop()->getNextTreeTop();

   TR::RegisterDependencyConditions *deps = NULL;
   if (!nextTT || !nextTT->getNode()->getBlock()->isExtensionOfPreviousBlock())
      {
      if (cg->enableRegisterAssociations() &&
          cg->getAppendInstruction()->getOpCodeValue() != TR::InstOpCode::assocreg)
         {
         cg->machine()->createRegisterAssociationDirective(cg->getAppendInstruction());
         }

      if (node->getNumChildren() > 0)
         {
         TR::Node *child = node->getFirstChild();
         cg->evaluate(child);
         deps = generateRegisterDependencyConditions(cg, child, 0);
         cg->decReferenceCount(child);
         }
      }

   // put the dependencies (if any) on the fence
   generateAdminInstruction(cg, TR::InstOpCode::fence, node, deps, fenceNode);

   return NULL;
   }

// handles l2a, lu2a, a2l
TR::Register *
OMR::ARM64::TreeEvaluator::passThroughEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   TR::Node *child = node->getFirstChild();
   TR::Register *trgReg = cg->evaluate(child);
   node->setRegister(trgReg);
   cg->decReferenceCount(child);
   return trgReg;
   }

TR::Register *
OMR::ARM64::TreeEvaluator::PrefetchEvaluator(TR::Node *node, TR::CodeGenerator *cg)
   {
   TR_ASSERT(node->getNumChildren() == 4, "TR::Prefetch should contain 4 child nodes");

   TR::Compilation *comp = cg->comp();
   TR::Node *firstChild = node->getFirstChild();
   TR::Node *secondChild = node->getChild(1);
   TR::Node *sizeChild = node->getChild(2);
   TR::Node *typeChild = node->getChild(3);

   // Do nothing for now

   cg->recursivelyDecReferenceCount(firstChild);
   cg->recursivelyDecReferenceCount(secondChild);
   cg->recursivelyDecReferenceCount(sizeChild);
   cg->recursivelyDecReferenceCount(typeChild);
   return NULL;
   }

TR::Register *
OMR::ARM64::TreeEvaluator::performCall(TR::Node *node, bool isIndirect, TR::CodeGenerator *cg)
   {
   TR::SymbolReference *symRef = node->getSymbolReference();
   TR::MethodSymbol *callee = symRef->getSymbol()->castToMethodSymbol();
   TR::Linkage *linkage = cg->getLinkage(callee->getLinkageConvention());
   TR::Register *returnRegister;

   if (isIndirect)
      returnRegister = linkage->buildIndirectDispatch(node);
   else
      returnRegister = linkage->buildDirectDispatch(node);

   return returnRegister;
   }

TR::Instruction *
OMR::ARM64::TreeEvaluator::generateVFTMaskInstruction(TR::CodeGenerator *cg, TR::Node *node, TR::Register *dstReg, TR::Register *srcReg, TR::Instruction *preced)
   {
   // Do nothing in OMR
   return preced;
   }

TR::Instruction *
OMR::ARM64::TreeEvaluator::generateVFTMaskInstruction(TR::CodeGenerator *cg, TR::Node *node, TR::Register *reg, TR::Instruction *preced)
   {
   // Do nothing in OMR
   return preced;
   }
