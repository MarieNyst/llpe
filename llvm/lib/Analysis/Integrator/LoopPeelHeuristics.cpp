//===- LoopPeelHeuristics.cpp - Find loops that we might want to try peeling --------===//
//
// The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This pass uses some heuristics to figure out loops that might be worth peeling.
// Basically this is simplistic SCCP plus some use of MemDep to find out how many instructions
// from the loop body would likely get evaluated if we peeled an iterations.
// We also consider the possibility of concurrently peeling a group of nested loops.
// The hope is that the information provided is both more informative and quicker to obtain than just speculatively
// peeling and throwing a round of -std-compile-opt at the result.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "looppeelheuristics"
#include "llvm/Pass.h"
#include "llvm/Instructions.h"
#include "llvm/BasicBlock.h"
#include "llvm/Module.h"
#include "llvm/IntrinsicInst.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/Analysis/HypotheticalConstantFolder.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/Analysis/MemoryDependenceAnalysis.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Support/CFG.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/IRBuilder.h"

#include <string>
#include <algorithm>

#include <fcntl.h> // For O_RDONLY et al
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

using namespace llvm;

bool instructionCounts(Instruction* I);

char IntegrationHeuristicsPass::ID = 0;

ModulePass *llvm::createIntegrationHeuristicsPass() {
  return new IntegrationHeuristicsPass();
}

INITIALIZE_PASS(IntegrationHeuristicsPass, "intheuristics", "Score functions for pervasive integration benefit", false, false);

// This whole thing is basically a constant propagation simulation -- rather than modifying the code in place like the real constant prop,
// we maintain shadow structures indicating which instructions have been folded and which basic blocks eliminated.
// It might turn out to be a better idea to find out whether peeling is useful by just doing it and optimising! I'll see...

IntegrationAttempt::~IntegrationAttempt() {
  for(DenseMap<CallInst*, InlineAttempt*>::iterator II = inlineChildren.begin(), IE = inlineChildren.end(); II != IE; II++) {
    delete (II->second);
  } 
  for(DenseMap<const Loop*, PeelAttempt*>::iterator PI = peelChildren.begin(), PE = peelChildren.end(); PI != PE; PI++) {
    delete (PI->second);
  }
}

InlineAttempt::InlineAttempt(IntegrationHeuristicsPass* Pass, IntegrationAttempt* P, Function& F, 
			     DenseMap<Function*, LoopInfo*>& LI, TargetData* TD, AliasAnalysis* AA, CallInst* _CI, 
			     DenseMap<Instruction*, const Loop*>& _invariantInsts, 
			     DenseMap<std::pair<BasicBlock*, BasicBlock*>, const Loop*>& _invariantEdges, 
			     DenseMap<BasicBlock*, const Loop*>& _invariantBlocks, int depth) : 
  IntegrationAttempt(Pass, P, F, LI, TD, AA, _invariantInsts, _invariantEdges, _invariantBlocks, depth),
  CI(_CI)
  { 
    UniqueReturnBlock = Pass->getUniqueReturnBlock(&F);
  }

PeelAttempt::PeelAttempt(IntegrationHeuristicsPass* Pass, IntegrationAttempt* P, Function& _F, DenseMap<Function*, LoopInfo*>& _LI, TargetData* _TD, AliasAnalysis* _AA, 
			 DenseMap<Instruction*, const Loop*>& _invariantInsts, DenseMap<std::pair<BasicBlock*, BasicBlock*>, const Loop*>& _invariantEdges, 
			 DenseMap<BasicBlock*, const Loop*>& _invariantBlocks, const Loop* _L, int depth) 
  : pass(Pass), parent(P), F(_F), LI(_LI), TD(_TD), AA(_AA), L(_L), invariantInsts(_invariantInsts), invariantEdges(_invariantEdges), invariantBlocks(_invariantBlocks), nesting_depth(depth)
{
  
  L->getExitEdges(ExitEdges);
  getOrCreateIteration(0);

}

PeelAttempt::~PeelAttempt() {
  for(std::vector<PeelIteration*>::iterator it = Iterations.begin(), it2 = Iterations.end(); it != it2; it++) {
    delete *it;
  }
}

// Does this instruction count for accounting / performance measurement? Essentially: can this possibly be improved?
bool instructionCounts(Instruction* I) {

  if (isa<DbgInfoIntrinsic>(I))
    return false;
  if(BranchInst* BI = dyn_cast<BranchInst>(I))
    if(BI->isUnconditional()) // Don't count unconditional branches as they're already as specified as they're getting
      return false;
  return true;

}

AliasAnalysis* IntegrationAttempt::getAA() {

  return this->AA;

}

ValCtx IntegrationAttempt::getLocalReplacement(Value* V) {

  DenseMap<Value*, ValCtx >::iterator it = improvedValues.find(V);
  if(it == improvedValues.end())
    return make_vc(V, this);
  else
    return it->second;  

}

// Implement HCFParentCallbacks, except for tryForwardLoad which comes later

ValCtx IntegrationAttempt::getReplacement(Value* V) {

  // V is visible directly from within this loop. Therefore, due to LCSSA form, it's either a variant (in this loop)
  // or an invariant belonging to one of my parent loops, or the root function.
  // One exception: it's a variant, but we're being asked in the context of trying to load-forward through an unpeeled loop.
  // In that case it's never valid to resolve a variant so I just return the unresolved answer. The same applies to getDefaultVC.
  // The case for reading an exit PHI is taken care of by the PHI resolution code.

  if(Constant* C = dyn_cast<Constant>(V))
    return const_vc(C);

  const Loop* evalScope = getValueScope(V);
  const Loop* L = getLoopContext();

  if(L != evalScope && ((!L) || L->contains(evalScope))) {
    // The load-forwarding case mentioned above.
    return make_vc(V, this);
  }
  else {
    return getReplacementUsingScope(V, evalScope);
  }

}

ValCtx IntegrationAttempt::getReplacementUsingScope(Value* V, const Loop* LScope) {

  if(LScope == getLoopContext())
    return getLocalReplacement(V);
  else
    return parent->getReplacementUsingScope(V, LScope);

}

ValCtx IntegrationAttempt::getDefaultVC(Value* V) {

  if(Constant* C = dyn_cast<Constant>(V))
    return const_vc(C);
  
  const Loop* evalScope = getValueScope(V);
  const Loop* L = getLoopContext();

  if(L != evalScope && ((!L) || L->contains(evalScope))) {
    return make_vc(V, this);
  }
  else {
    return getDefaultVCWithScope(V, evalScope);
  }

}

ValCtx IntegrationAttempt::getDefaultVCWithScope(Value* V, const Loop* LScope) {

  if(LScope == getLoopContext())
    return make_vc(V, this);
  else
    return parent->getDefaultVCWithScope(V, LScope);

}

Constant* llvm::getConstReplacement(Value* V, HCFParentCallbacks* Ctx) {

  if(Constant* C = dyn_cast<Constant>(V))
    return C;
  ValCtx Replacement = Ctx->getReplacement(V);
  if(Constant* C = dyn_cast<Constant>(Replacement.first))
    return C;
  return 0;

}

Constant* IntegrationAttempt::getConstReplacement(Value* V) {

  return llvm::getConstReplacement(V, this);

}

// Only ever called on things that belong in this scope, thanks to shouldIgnoreBlock et al.
void IntegrationAttempt::setReplacement(Value* V, ValCtx R) {

  improvedValues[V] = R;

}

void IntegrationAttempt::eraseReplacement(Value* V) {

  improvedValues.erase(V);

}

// Get the loop scope at which a given instruction should be resolved.
const Loop* IntegrationAttempt::getValueScope(Value* V) {

  if(Instruction* I = dyn_cast<Instruction>(V)) {
    DenseMap<Instruction*, const Loop*>::iterator it = invariantInsts.find(I);
    if(it != invariantInsts.end())
      return it->second;
    else
      return LI[&F]->getLoopFor(I->getParent());
  }
  else
    return getLoopContext();

}

bool IntegrationAttempt::isUnresolved(Value* V) {

  return (!shouldForwardValue(getDefaultVC(V))) && (getDefaultVC(V) == getReplacement(V));

}

bool IntegrationAttempt::edgeIsDead(BasicBlock* B1, BasicBlock* B2) {

  const Loop* MyScope = getLoopContext();
  const Loop* EdgeScope = getEdgeScope(B1, B2);

  if(deadEdges.count(std::make_pair(B1, B2)))
    return true;

  if((MyScope != EdgeScope) && ((!MyScope) || MyScope->contains(EdgeScope))) {

    if(PeelAttempt* LPA = getPeelAttempt(immediateChildLoop(MyScope, EdgeScope))) {
      PeelIteration* FinalIter = LPA->Iterations[LPA->Iterations.size() - 1];
      if(FinalIter->iterStatus == IterationStatusFinal) {
	return FinalIter->edgeIsDeadWithScope(B1, B2, EdgeScope);
      }
    }
    
    return false;

  }

  return edgeIsDeadWithScope(B1, B2, EdgeScope);

}

bool IntegrationAttempt::edgeIsDeadWithScope(BasicBlock* B1, BasicBlock* B2, const Loop* ScopeL) {

  if(deadEdges.count(std::make_pair(B1, B2)))
    return true;
  
  const Loop* MyScope = getLoopContext();

  if(ScopeL == MyScope)
    return false;

  return parent->edgeIsDeadWithScope(B1, B2, ScopeL);

}

void IntegrationAttempt::setEdgeDead(BasicBlock* B1, BasicBlock* B2) {

  std::pair<BasicBlock*, BasicBlock*> Edge = std::make_pair(B1, B2);
  deadEdges.insert(Edge);

}

const Loop* IntegrationAttempt::getEdgeScope(BasicBlock* B1, BasicBlock* B2) {

  DenseMap<std::pair<BasicBlock*, BasicBlock*>, const Loop*>::iterator it = invariantEdges.find(std::make_pair(B1, B2));
  if(it != invariantEdges.end())
    return it->second;
  else
    return LI[&F]->getLoopFor(B1);

}

bool IntegrationAttempt::blockIsDeadWithScope(BasicBlock* BB, const Loop* ScopeL) {

  if(deadBlocks.count(BB))
    return true;
  if(ScopeL == getLoopContext())
    return false;
  else
    return parent->blockIsDeadWithScope(BB, ScopeL);

}

bool IntegrationAttempt::blockIsDead(BasicBlock* BB) {

  DenseMap<BasicBlock*, const Loop*>::iterator it = invariantBlocks.find(BB);
  if(it == invariantBlocks.end())
    return deadBlocks.count(BB);
  else
    return blockIsDeadWithScope(BB, it->second);

}

void IntegrationAttempt::setBlockDead(BasicBlock* BB) {

  deadBlocks.insert(BB);

}

const Loop* IntegrationAttempt::getBlockScope(BasicBlock* BB) {

  DenseMap<BasicBlock*, const Loop*>::iterator it = invariantBlocks.find(BB);
  if(it == invariantBlocks.end())
    return LI[&F]->getLoopFor(BB);
  else
    return it->second;
  
}

bool IntegrationAttempt::blockIsCertain(BasicBlock* BB) {

  const Loop* BlockL = getBlockScope(BB);
  const Loop* MyL = getLoopContext();

  if(((!MyL) && BlockL) || (MyL != BlockL && MyL->contains(BlockL))) {

    if(PeelAttempt* LPA = getPeelAttempt(BlockL)) {

      PeelIteration* FinalIter = LPA->Iterations[LPA->Iterations.size() - 1];
      if(FinalIter->iterStatus == IterationStatusFinal) {

	return FinalIter->certainBlocks.count(BB);

      }
      else {

	return false;

      }

    }

  }

  return certainBlocks.count(BB);

}

InlineAttempt* IntegrationAttempt::getInlineAttempt(CallInst* CI) {

  DenseMap<CallInst*, InlineAttempt*>::iterator it = inlineChildren.find(const_cast<CallInst*>(CI));
  if(it != inlineChildren.end())
    return it->second;

  return 0;

}

InlineAttempt* IntegrationAttempt::getOrCreateInlineAttempt(CallInst* CI) {

  if(InlineAttempt* IA = getInlineAttempt(CI))
    return IA;

  if(Function* FCalled = CI->getCalledFunction()) {

    if((!FCalled->isDeclaration()) && (!FCalled->isVarArg())) {

      if(certainBlocks.count(CI->getParent())) {

	InlineAttempt* IA = new InlineAttempt(pass, this, *FCalled, this->LI, this->TD, this->AA, CI, pass->getInstScopes(FCalled), pass->getEdgeScopes(FCalled), pass->getBlockScopes(FCalled), this->nesting_depth + 1);
	inlineChildren[CI] = IA;

	LPDEBUG("Inlining " << FCalled->getName() << " at " << *CI << "\n");

	pass->queueCheckBlock(IA, &(FCalled->getEntryBlock()));
	// Check every argument, for natural constants or for variables that have already been established.
      
	for(Function::arg_iterator AI = FCalled->arg_begin(), AE = FCalled->arg_end(); AI != AE; AI++) {
	  
	  pass->queueTryEvaluate(IA, &*AI /* Iterator to pointer */);

	}

	IA->queueInitialWork();

	// Recheck any loads that were clobbered by this call
	queueWorkBlockedOn(CI);

	return IA;

      }
      else {
	LPDEBUG("Ignored " << *CI << " because it is not yet certain to execute\n");
      }

    }
    else {
      LPDEBUG("Ignored " << *CI << " because we don't know the function body, or it's vararg\n");
    }

  }
  else {
    LPDEBUG("Ignored " << *CI << " because it's an uncertain indirect call\n");
  }

  return 0;

}

void PeelIteration::queueCheckExitBlock(BasicBlock* BB) {

  // Only called if the exit edge is a local variant
  pass->queueCheckBlock(parent, BB);

  for(BasicBlock::iterator BI = BB->begin(), BE = BB->end(); BI != BE && isa<PHINode>(BI); ++BI) {

    pass->queueTryEvaluate(parent, BI);
	
  }

}

void PeelIteration::checkFinalIteration() {

  // Check whether we now have evidence the loop terminates this time around
  // If it does, queue consideration of each exit PHI; by LCSSA these must belong to our parent.

  if(edgeIsDead(L->getLoopLatch(), L->getHeader())) {

    for(SmallVector<std::pair<BasicBlock*, BasicBlock*>, 4>::iterator EI = parentPA->ExitEdges.begin(), EE = parentPA->ExitEdges.end(); EI != EE; ++EI) {

      if(getEdgeScope(EI->first, EI->second) == L) {
	queueCheckExitBlock(EI->second);
      }
      else {
	LPDEBUG("Ignoring exit edge " << EI->first->getName() << " -> " << EI->second->getName() << " at this scope (invariant)\n");
      }

    }
    
    iterStatus = IterationStatusFinal;

    // Loads might now be able to be raised through this loop. They will be blocked at parent scope.
    parent->queueCFGBlockedLoads();

  }

}

PeelIteration* PeelAttempt::getIteration(unsigned iter) {

  if(Iterations.size() > iter)
    return Iterations[iter];

  return 0;

}

PeelIteration* PeelAttempt::getOrCreateIteration(unsigned iter) {

  if(PeelIteration* PI = getIteration(iter))
    return PI;
  
  LPDEBUG("Peeling iteration " << iter << " of loop " << L->getHeader()->getName() << "\n");

  assert(iter == Iterations.size());

  PeelIteration* NewIter = new PeelIteration(pass, parent, this, F, LI, TD, AA, L, invariantInsts, invariantEdges, invariantBlocks, iter, nesting_depth);
  Iterations.push_back(NewIter);
    
  BasicBlock* Header = L->getHeader();
   
  pass->queueCheckBlock(NewIter, L->getHeader());
 
  for(BasicBlock::iterator BI = Header->begin(), BE = Header->end(); BI != BE && isa<PHINode>(BI); ++BI) {
	
    pass->queueTryEvaluate(NewIter, BI);

  }
  
  NewIter->queueInitialWork();

  return NewIter;

}

PeelIteration* PeelIteration::getNextIteration() {

  return parentPA->getIteration(this->iterationCount + 1);

}

PeelIteration* PeelIteration::getOrCreateNextIteration() {

  if(PeelIteration* Existing = getNextIteration())
    return Existing;

  if(iterStatus == IterationStatusFinal) {
    LPDEBUG("Loop known to exit: will not create next iteration\n");
    return 0;
  }

  bool willIterate = true;

  for(SmallVector<std::pair<BasicBlock*, BasicBlock*>, 4>::iterator EI = parentPA->ExitEdges.begin(), EE = parentPA->ExitEdges.end(); EI != EE; ++EI) {

    if(!edgeIsDead(EI->first, EI->second)) {
      willIterate = false;
    }

  }
  
  if(!willIterate) {

    LPDEBUG("Won't peel loop " << L->getHeader()->getName() << " yet because at least one exit edge is still alive\n");
    return 0;
      
  }

  iterStatus = IterationStatusNonFinal;
  LPDEBUG("Loop known to iterate: creating next iteration\n");
  return parentPA->getOrCreateIteration(this->iterationCount + 1);

}

PeelAttempt* IntegrationAttempt::getPeelAttempt(const Loop* L) {

  DenseMap<const Loop*, PeelAttempt*>::const_iterator it = peelChildren.find(L);
  if(it != peelChildren.end())
    return it->second;

  return 0;

}

PeelAttempt* IntegrationAttempt::getOrCreatePeelAttempt(const Loop* NewL) {

  if(PeelAttempt* PA = getPeelAttempt(NewL))
    return PA;
  
  // Preheaders only have one successor (the header), so this is enough.
  if(!certainBlocks.count(NewL->getLoopPreheader())) {
   
    LPDEBUG("Will not expand loop " << NewL->getHeader()->getName() << " at this time because the preheader is not certain to execute\n");
    return 0;

  }

  if(NewL->getLoopPreheader() && NewL->getLoopLatch() && (NewL->getNumBackEdges() == 1)) {

    LPDEBUG("Inlining loop with header " << NewL->getHeader()->getName() << "\n");
    PeelAttempt* LPA = new PeelAttempt(pass, this, F, LI, TD, AA, invariantInsts, invariantEdges, invariantBlocks, NewL, nesting_depth + 1);
    peelChildren[NewL] = LPA;

    queueCFGBlockedLoads();

    return LPA;

  }
  else {

    LPDEBUG("Won't explore loop with header " << NewL->getHeader()->getName() << " because it lacks a preheader, a latch, or both, or has multiple backedges\n");
    return 0;

  }

}


const Loop* InlineAttempt::getLoopContext() {

  return 0;

}

const Loop* PeelIteration::getLoopContext() {

  return L;

}

ValCtx InlineAttempt::tryGetReturnValue() {

  // Let's have a go at supplying a return value to our caller. Simple measure:
  // we know the value if all the 'ret' instructions except one are dead,
  // and we know that instruction's operand.

  ValCtx returnVal = VCNull;
  bool foundReturnInst = false;

  for(Function::iterator FI = F.begin(), FE = F.end(); FI != FE; FI++) {
    if(blockIsDead(FI))
      continue;
    for(BasicBlock::iterator BI = FI->begin(), BE = FI->end(); BI != BE; BI++) {
      if(ReturnInst* RI = dyn_cast<ReturnInst>(BI)) {
	if(foundReturnInst) {
	  LPDEBUG("Can't determine return value: more than one 'ret' is live\n");
	  returnVal = VCNull;
	  break;
	}
	else
	  foundReturnInst = true;
	Value* ThisRet = RI->getReturnValue();
	returnVal = getReplacement(ThisRet);
	if(!returnVal.first) {
	  LPDEBUG("Can't determine return value: live instruction " << *RI << " has non-forwardable value " << *(RI->getReturnValue()) << "\n");
	  break;
	}
      }
    }
  }
  
  if(returnVal.first) {
    LPDEBUG("Found return value: " << returnVal << "\n");
  }
  
  return returnVal;

}

// Store->Load forwarding helpers:

BasicBlock* InlineAttempt::getEntryBlock() {

  return &F.getEntryBlock();

}

BasicBlock* PeelIteration::getEntryBlock() {
  
  return L->getHeader();

}

Instruction* InlineAttempt::getEntryInstruction() {

  return CI;

}

Instruction* PeelIteration::getEntryInstruction() {

  return L->getLoopPreheader()->getTerminator();

}

ValCtx InlineAttempt::getImprovedCallArgument(Argument* A) {

  return parent->getReplacement(CI->getArgOperand(A->getArgNo()));

}

// Given a MemDep Def, get the value loaded or stored.
ValCtx IntegrationAttempt::getDefn(const MemDepResult& Res) {

  IntegrationAttempt* QueryCtx = Res.getCookie() ? ((IntegrationAttempt*)Res.getCookie()) : this;
  ValCtx improved = VCNull;
  if(StoreInst* SI = dyn_cast<StoreInst>(Res.getInst())) {
    improved = QueryCtx->getReplacement(SI->getOperand(0));
  }
  else if(LoadInst* DefLI= dyn_cast<LoadInst>(Res.getInst())) {
    improved = QueryCtx->getReplacement(DefLI);
  }
  else {
    LPDEBUG("Defined by " << *(Res.getInst()) << " which is not a simple load or store\n");
    return VCNull;
  }

  if(improved.first != Res.getInst() || improved.second != QueryCtx) {
    LPDEBUG("Definition improved to " << improved << "\n");
    return improved;
  }
  else {
    LPDEBUG("Definition not improved\n");
    return VCNull;
  }

}

// Find the unique definer or clobberer for a given Load.
MemDepResult IntegrationAttempt::getUniqueDependency(LFAQueryable& LFA) {

  MemoryDependenceAnalyser MD;
  MD.init(AA, this, &(LFA.getLFA()));

  LoadInst* QueryInst = LFA.getQueryInst();
  LoadInst* OriginalInst = LFA.getOriginalInst();

  MemDepResult Seen = MD.getDependency(QueryInst);

  if(Seen.isNonLocal()) {

    Seen = MemDepResult();
    Value* LPointer = QueryInst->getOperand(0);

    SmallVector<NonLocalDepResult, 4> NLResults;

    MD.getNonLocalPointerDependency(LPointer, true, QueryInst->getParent(), NLResults);

    if(NLResults.size() == 0) {

      // Probably we're in a block which is dead, but has yet to be diagnosed as such.
      return MemDepResult();

    }

    for(unsigned int i = 0; i < NLResults.size(); i++) {
		
      const MemDepResult& Res = NLResults[i].getResult();

      if(Res.isNonLocal())
	continue;
      else if(Res == Seen)
	continue;
      else if(Seen == MemDepResult()) { // Nothing seen yet
	Seen = Res;
      }
      else {
	LPDEBUG(*OriginalInst << " is overdefined: depends on at least " << Seen << " and " << Res << "\n");
	return MemDepResult();
      }

    }

    LPDEBUG(*OriginalInst << " nonlocally defined by " << Seen << "\n");

  }
  else {
    LPDEBUG(*OriginalInst << " locally defined by " << Seen << "\n");
  }

  return Seen;

}

ValCtx IntegrationAttempt::getUltimateUnderlyingObject(Value* V) {

  ValCtx Ultimate = getDefaultVC(V);
  while(!isIdentifiedObject(Ultimate.first)) {

    ValCtx New = Ultimate.second->getReplacement(Ultimate.first);
    New = make_vc(New.first->getUnderlyingObject(), New.second);
  
    if(New == Ultimate)
      break;

    Ultimate = New;

  }

  return Ultimate;

}

// Main load forwarding entry point:
// Try to forward the load locally (within this loop or function), or otherwise build a symbolic expression
// and ask our parent to continue resolving the load.
ValCtx IntegrationAttempt::tryForwardLoad(LoadInst* LoadI) {

  LPDEBUG("Trying to forward load: " << *LoadI << "\n");

  if(Constant* C = getConstReplacement(LoadI->getPointerOperand())) {

    // Try ordinary constant folding first! Might not work because globals constitute constant expressions.
    // For them we should do the ordinary alias analysis task.
    Constant* ret = ConstantFoldLoadFromConstPtr(C, this->TD);
    LPDEBUG("Resolved load as a constant expression\n");
    if(ret)
      return const_vc(ret);

  }

  // Check whether pursuing alises is pointless -- this is true if we're certain that the ultimate underlying object is a constant.
  // If it is, our attempt above was likely foiled only by uncertainty about the specific bit of the constant (e.g. index within a const string)
  // and the only way the situation will improve is if those offsets become clear.

  ValCtx Ultimate = getUltimateUnderlyingObject(LoadI->getPointerOperand());

  if(GlobalVariable* GV = dyn_cast<GlobalVariable>(Ultimate.first)) {

    if(GV->isConstant()) {
      LPDEBUG("Load cannot presently be resolved, but is rooted on a constant global. Abandoning search\n");
      return VCNull;
    }

  }

  MemDepResult Res = tryResolveLoad(LoadI);
  IntegrationAttempt* ResAttempt = (Res.getCookie() ? (IntegrationAttempt*)Res.getCookie() : this);
  ValCtx Result = VCNull;

  if(Res.isClobber()) {
    // See if we can do better for clobbers by large stores, memcpy, read calls, etc.
    Result = tryResolveClobber(LoadI, make_vc(Res.getInst(), ResAttempt));
  }
  else if(Res.isDef()) {

    Result = getDefn(Res);

    if(Result.first && Result.first->getType() != LoadI->getType()) {

      Constant* ResultC;
      if(LoadI->getType()->isIntegerTy() && (ResultC = dyn_cast<Constant>(Result.first))) {

	Result = const_vc(CoerceConstExprToLoadType(ResultC, LoadI->getType()));
	if(Result.first) {

	  LPDEBUG("Successfully coerced value to " << Result << " to match load type\n");

	}

      }
      else {

	LPDEBUG("Unable to use the definition because its type doesn't match the load and the def isn't an integer constant\n");
	Result = VCNull;

      }

    }

  }

  if(Result == VCNull || !shouldForwardValue(Result)) {

    if(Result == VCNull) {
      if(Res.isDef())
	LPDEBUG("Load resolved successfully, but we couldn't retrieve a value from the defining instruction\n");
    }
    else {
      LPDEBUG("Load resolved successfully, but " << Result << " is not a forwardable value\n");
    }

    if(Res.getInst() && Res.getInst()->mayWriteToMemory())
      ResAttempt->addBlockedLoad(Res.getInst(), this, LoadI);
    // Otherwise we're stuck due to a PHI translation failure. That'll only improve when the load pointer is improved.
    return VCNull;

  }
  else {
    return Result;
  }

}

MemDepResult IntegrationAttempt::tryResolveLoad(LoadInst* LoadI) {

  MemDepResult Result;
  LoadForwardAttempt Attempt(LoadI, this);

  if(forwardLoadIsNonLocal(Attempt, Result)) {

    if(!parent)
      return MemDepResult();

    if(!Attempt.canBuildSymExpr())
      return MemDepResult();

    LPDEBUG("Will resolve ");
    DEBUG(Attempt.describeSymExpr(dbgs()));
    DEBUG(dbgs() << "\n");

    return tryForwardExprFromParent(Attempt);

  }
  else {
    if(Result != MemDepResult()) {
      LPDEBUG("Forwarded " << *LoadI << " locally: got " << Result << "\n");
    }
    return Result;
  }

}

// Pursue a load further. Current context is a function body; ask our caller to pursue further.
MemDepResult InlineAttempt::tryForwardExprFromParent(LoadForwardAttempt& LFA) {

  if(!parent) {
    LPDEBUG("Unable to pursue further; this function is the root\n");
    return MemDepResult();
  }
  else {
    LPDEBUG("Resolving load at call site\n");
    return parent->tryResolveLoadAtChildSite(this, LFA);
  }

}

bool PeelAttempt::tryForwardExprFromIter(LoadForwardAttempt& LFA, int originIter, MemDepResult& Result) {

  // First of all, try winding backwards through our sibling iterations. We can use a single realisation
  // of the LFA for all of these checks, since the instructions are always the same.

  LFARealization LFAR(LFA, Iterations[0], L->getLoopLatch()->getTerminator());
  
  LPDEBUG("Trying to resolve by walking backwards through loop " << L->getHeader()->getName() << "\n");

  for(int iter = originIter - 1; iter >= 0; iter--) {

    LPDEBUG("Trying to resolve in iteration " << iter << "\n");

    if(!(Iterations[iter]->tryResolveExprUsing(LFAR, Result))) {
      // Shouldn't pursue further -- the result is either defined or conclusively clobbered here.
      if(Result.isDef()) {
	LPDEBUG("Resolved to " << Result << "\n");
      }
      else {
	LPDEBUG("Resolution failed\n");
      }
      return false;
    }
    else {
      // Go round the loop and try the next iteration.
    }

    if(LFA.getBaseContext() == Iterations[iter]) {
      LPDEBUG("Abandoning resolution: " << LFA.getBaseVC() << " is out of scope\n");
      Result = MemDepResult();
      return false;
    }

  }

  return true;

}

// Pursue a load further. Current context is a loop body -- try resolving it in previous iterations,
// then ask our enclosing loop or function body to look further.
MemDepResult PeelAttempt::tryForwardExprFromParent(LoadForwardAttempt& LFA, int originIter) {

  MemDepResult Result;
  if(!tryForwardExprFromIter(LFA, originIter, Result)) {
    return Result;
  }
  else {
    LPDEBUG("Resolving out the preheader edge; deferring to parent\n");
    return parent->tryResolveLoadAtChildSite(Iterations[0], LFA);
  }

}

// Helper: loop iterations defer the resolution process to the abstract loop.
MemDepResult PeelIteration::tryForwardExprFromParent(LoadForwardAttempt& LFA) {

  return parentPA->tryForwardExprFromParent(LFA, this->iterationCount);

}

// Try forwarding a load locally; return true if it is nonlocal or false if not, in which case
// Result is set to the resolution result.
bool IntegrationAttempt::forwardLoadIsNonLocal(LFAQueryable& LFAQ, MemDepResult& Result) {

  Result = getUniqueDependency(LFAQ);

  if(Result == MemDepResult()) {
    // The definition or clobber was not unique. Edges need to be killed before this can be resolved.
    CFGBlockedLoads.push_back(std::make_pair(LFAQ.getOriginalCtx(), LFAQ.getOriginalInst()));
  }
  else if(Result.isClobber()) {
    if(Result.getInst()->getParent() == getEntryBlock()) {
      BasicBlock::iterator TestII(Result.getInst());
      if(TestII == getEntryBlock()->begin()) {
	return true;
      }
    }
  }

  if(Result != MemDepResult() && (!Result.getCookie())) {
    // This result is generated by MD, not one of our callbacks for handling child contexts
    // Tag it as originating here
    Result.setCookie(this);
  }

  return false;

}

bool IntegrationAttempt::tryResolveExprUsing(LFARealization& LFAR, MemDepResult& Result) {

  LFARMapping LFARM(LFAR, this);

  return forwardLoadIsNonLocal(LFAR, Result);

}

bool IntegrationAttempt::tryResolveExprFrom(LoadForwardAttempt& LFA, Instruction* Where, MemDepResult& Result) {

  LFARealization LFAR(LFA, this, Where);
  
  return tryResolveExprUsing(LFAR, Result);

}

// Entry point for a child loop or function that wishes us to continue pursuing a load.
// Find the instruction before the child begins (so loop preheader or call site), realise the given symbolic
// expression, and try ordinary load forwarding from there.
MemDepResult IntegrationAttempt::tryResolveLoadAtChildSite(IntegrationAttempt* IA, LoadForwardAttempt& LFA) {

  MemDepResult Result;

  LPDEBUG("Continuing resolution from entry point " << *(IA->getEntryInstruction()) << "\n");

  if(tryResolveExprFrom(LFA, IA->getEntryInstruction(), Result)) {
    LPDEBUG("Still nonlocal, passing to our parent scope\n");
    return tryForwardExprFromParent(LFA);
  }
  else {
    LPDEBUG("Resolved at this scope: " << Result << "\n");
    return Result;
  }

}

bool InlineAttempt::tryForwardLoadFromExit(LoadForwardAttempt& LFA, MemDepResult& Result) {

  BasicBlock* RetBB = pass->getUniqueReturnBlock(&F);

  if(!RetBB) {

    LPDEBUG("Can't investigate because this function has no unique return block! Run -mergereturn\n");
    return false;

  }

  if(tryResolveExprFrom(LFA, RetBB->getTerminator(), Result)) {
    Result = MemDepResult::getNonLocal();
    return true;
  }
  else {
    return Result.isDef();
  }

}

bool IntegrationAttempt::tryForwardLoadThroughCall(LoadForwardAttempt& LFA, CallInst* CI, MemDepResult& Result) {

  InlineAttempt* IA = getInlineAttempt(CI);

  if(!IA) {
    LPDEBUG("Unable to pursue load through call " << *CI << " as it has not yet been explored\n");
    return false;
  }

  LPDEBUG("Trying to forward load " << *(LFA.getOriginalInst()) << " through call " << *CI << ":\n");
  
  bool ret;

  if(!LFA.canBuildSymExpr())
    return false;

  ret = IA->tryForwardLoadFromExit(LFA, Result);

  if(!ret) {
    LPDEBUG("Call " << *CI << " clobbers " << *(LFA.getOriginalInst()) << "\n");
  }
  else if(Result.isNonLocal()) {
    LPDEBUG("Call " << *CI << " doesn't affect " << *(LFA.getOriginalInst()) << "\n");
  }
  else {
    LPDEBUG("Call " << *CI << " defines " << *(LFA.getOriginalInst()) << "\n");
  }

  return ret;

}

bool PeelAttempt::tryForwardLoadThroughLoopFromBB(BasicBlock* BB, LoadForwardAttempt& LFA, BasicBlock*& PreheaderOut, SmallVectorImpl<NonLocalDepResult> &Result) {

  // MDA has just traversed an exit edge. Pursue the load from the exiting block to the header,
  // then from latch to preheader like the forward-from-parent case. Cache in the LFA object
  // which exit block -> header paths and which Loops' main bodies have already been investigated.

  PreheaderOut = 0;

  if(Iterations.back()->iterStatus != IterationStatusFinal) {
    
    LPDEBUG("Raising " << *(LFA.getOriginalInst()) << " through loop " << (L->getHeader()->getName()) << " without per-iteration knowledge as it is not yet known to terminate\n");
    return false;

  }
  
  std::pair<DenseMap<std::pair<BasicBlock*, const Loop*>, MemDepResult>::iterator, bool> LastIterEntry = LFA.getLastIterCache(BB, L);
  MemDepResult& LastIterResult = LastIterEntry.first->second;

  if(!LastIterEntry.second) {
    LPDEBUG("Raising " << *(LFA.getOriginalInst()) << " from exit block " << BB->getName() << " to header of " << L->getHeader()->getName() << " (cached: " << LastIterResult << ")\n");
    if(!LastIterResult.isNonLocal()) {
      Result.push_back(NonLocalDepResult(BB, LastIterResult, 0)); // Hack -- NLDRs should contain the true BB where a relationship was discovered and the PHI translated address.
      return true;
    }
  }
  else {
    LPDEBUG("Raising " << *(LFA.getOriginalInst()) << " from exit block " << BB->getName() << " to header of " << L->getHeader()->getName() << "\n");

    if(Iterations.back()->tryResolveExprFrom(LFA, BB->getTerminator(), LastIterResult)) {
      LastIterResult = MemDepResult::getNonLocal();
    }
    else {
      if(LastIterResult.isClobber()) {
	LPDEBUG(*(LFA.getOriginalInst()) << " clobbered in last iteration of " << L->getHeader()->getName() << "\n");
      }
      else {
	LPDEBUG(*(LFA.getOriginalInst()) << " defined in last iteration of " << L->getHeader()->getName() << "\n");
      }
      Result.push_back(NonLocalDepResult(BB, LastIterResult, 0));
      return true;
    }
  }

  // OK, try raising the load through the iterations before the last.
  std::pair<DenseMap<const Loop*, MemDepResult>::iterator, bool> OtherItersEntry = LFA.getOtherItersCache(L);
  MemDepResult& OtherItersResult = OtherItersEntry.first->second;

  if(!OtherItersEntry.second) { 
    LPDEBUG("Raising " << *(LFA.getOriginalInst()) << " through main body of " << L->getHeader()->getName() << " (cached: " << OtherItersResult << ")\n");
    if(!OtherItersResult.isNonLocal()) {
      Result.push_back(NonLocalDepResult(BB, OtherItersResult, 0));
      return true;
    }
  }
  else {
    LPDEBUG("Raising " << *(LFA.getOriginalInst()) << " through main body of " << L->getHeader()->getName() << "\n");
    if(tryForwardExprFromIter(LFA, Iterations.size() - 1, OtherItersResult)) {
      OtherItersResult = MemDepResult::getNonLocal();
    }
    else {
      if(OtherItersResult.isClobber()) {
	LPDEBUG(*(LFA.getOriginalInst()) << " clobbered in non-final iteration of " << L->getHeader()->getName() << "\n");
      }
      else {
	LPDEBUG(*(LFA.getOriginalInst()) << " defined in non-final iteration of " << L->getHeader()->getName() << "\n");
      }
      Result.push_back(NonLocalDepResult(BB, OtherItersResult, 0));
      return true;
    }
  }

  // Made it here: the instruction propagates through the entire loop.
  PreheaderOut = L->getLoopPreheader();
  return true;

}

bool IntegrationAttempt::tryForwardLoadThroughLoopFromBB(BasicBlock* BB, LoadForwardAttempt& LFA, BasicBlock*& PreheaderOut, SmallVectorImpl<NonLocalDepResult> &Result) {

  const Loop* BBL = LI[&F]->getLoopFor(BB);

  if(BBL != getLoopContext() && ((!getLoopContext()) || getLoopContext()->contains(BBL))) {

    PeelAttempt* LPA = getPeelAttempt(BBL);
    if(!LPA) {
      LPDEBUG("Raising " << *(LFA.getOriginalInst()) << " through loop " << (BBL->getHeader()->getName()) << " without per-iteration knowledge as it has not yet been explored\n");
      return false;
    }

    if(!LFA.canBuildSymExpr()) {
      LPDEBUG("Raising " << *(LFA.getOriginalInst()) << " through loop " << (BBL->getHeader()->getName()) << " without per-iteration knowledge because the pointer cannot be represented simply\n");
      return false;
    }

    return LPA->tryForwardLoadThroughLoopFromBB(BB, LFA, PreheaderOut, Result);

  }
  else {
    return false;
  }

}

void IntegrationAttempt::addBlockedLoad(Instruction* BlockedOn, IntegrationAttempt* RetryCtx, LoadInst* RetryLI) {

  InstBlockedLoads[BlockedOn].push_back(std::make_pair(RetryCtx, RetryLI));

}

void IntegrationAttempt::tryPromoteOpenCall(CallInst* CI) {
  
  if(!certainBlocks.count(CI->getParent())) {
    LPDEBUG("Won't promote open call " << *CI << " yet: not certain to execute\n");
    return;
  }

  if(forwardableOpenCalls.count(CI)) {
    LPDEBUG("Open call " << *CI << ": already promoted\n");
    return;
  }
  
  if(Function *SysOpen = F.getParent()->getFunction("open")) {
    const FunctionType *FT = SysOpen->getFunctionType();
    if (FT->getNumParams() == 2 && FT->getReturnType()->isIntegerTy(32) &&
        FT->getParamType(0)->isPointerTy() &&
        FT->getParamType(1)->isIntegerTy(32) &&
	FT->isVarArg()) {

      ValCtx VCalled = getReplacement(CI->getCalledValue());
      if(Function* FCalled = dyn_cast<Function>(VCalled.first)) {

	if(FCalled == SysOpen) {

	  ValCtx ModeArg = getReplacement(CI->getArgOperand(1));
	  if(ConstantInt* ModeValue = dyn_cast<ConstantInt>(ModeArg.first)) {
	    int RawMode = (int)ModeValue->getLimitedValue();
	    if(RawMode & O_RDWR || RawMode & O_WRONLY) {
	      LPDEBUG("Can't promote open call " << *CI << " because it is not O_RDONLY\n");
	      return;
	    }
	  }
	  else {
	    LPDEBUG("Can't promote open call " << *CI << " because its mode argument can't be resolved\n");
	    return;
	  }
	  
	  ValCtx NameArg = getReplacement(CI->getArgOperand(0));
	  std::string Filename;
	  if (!GetConstantStringInfo(NameArg.first, Filename)) {
	    LPDEBUG("Can't promote open call " << *CI << " because its filename argument is unresolved\n");
	    return;
	  }

	  bool FDEscapes = false;
	  for(Value::use_iterator UI = CI->use_begin(), UE = CI->use_end(); (!FDEscapes) && (UI != UE); ++UI) {

	    if(Instruction* I = dyn_cast<Instruction>(*UI)) {

	      if(I->mayWriteToMemory()) {
		
		LPDEBUG("Marking open call " << *CI << " escaped due to user " << *I << "\n");
		FDEscapes = true;

	      }

	    }

	  }

	  LPDEBUG("Successfully promoted open of file " << Filename << ": queueing initial forward attempt\n");
	  forwardableOpenCalls[CI] = OpenStatus(make_vc(CI, this), Filename, FDEscapes);

	  pass->queueOpenPush(make_vc(CI, this), make_vc(CI, this));

	  // Also investigate users, since we now know it'll emit a non-negative FD.
	  investigateUsers(CI);
      
	}
	else {
	  
	  LPDEBUG("Unable to identify " << *CI << " as an open call because it calls something else\n");

	}

      }
      else {
	
	LPDEBUG("Unable to identify " << *CI << " as an open call because its target is unknown\n");

      }

    }
    else {

      LPDEBUG("Unable to identify " << *CI << " as an open call because the symbol 'open' resolves to something with inappropriate type!\n");

    }

  }
  else {

    LPDEBUG("Unable to identify " << *CI << " as an open call because no symbol 'open' is in scope\n");

  }

}

void IntegrationAttempt::tryPushOpen(CallInst* OpenI, ValCtx OpenProgress) {

  OpenStatus& OS = forwardableOpenCalls[OpenI];

  if(OS.LatestResolvedUser != OpenProgress) {

    LPDEBUG("Skipping as call has been pushed in the meantime\n");
    return;

  }

  // Try to follow the trail from LastResolvedUser forwards.

  LPDEBUG("Trying to extend VFS op chain for " << *OpenI << " from " << OpenProgress << "\n");

  ValCtx NextStart = OpenProgress;
  bool skipFirst = true;

  while(NextStart.second->tryPushOpenFrom(NextStart, make_vc(OpenI, this), OpenProgress, OS, skipFirst)) {
    LPDEBUG("Continuing from " << NextStart << "\n");
    skipFirst = false;
  }

}

// Called in the context of Start.second. OpenInst is the open instruction we're pursuing, and the context where OS is stored.
// ReadInst is the entry in the chain of VFS operations that starts at OpenInst.
bool IntegrationAttempt::tryPushOpenFrom(ValCtx& Start, ValCtx OpenInst, ValCtx ReadInst, OpenStatus& OS, bool skipFirst) {

  Instruction* StartI = cast<Instruction>(Start.first);
  BasicBlock* BB = StartI->getParent();
  BasicBlock::iterator BI(StartI);

  while(1) {

    if(!skipFirst) {

      if(CallInst* CI = dyn_cast<CallInst>(BI)) {

	bool isVFSCall, shouldRequeue;
	if(vfsCallBlocksOpen(CI, OpenInst, ReadInst, OS, isVFSCall, shouldRequeue)) {
	  if(shouldRequeue) {
	    // Queue to retry when we know more about the call.
	    InstBlockedOpens[CI].push_back(std::make_pair(OpenInst, ReadInst));
	  }
	  return false;
	}

	if(!isVFSCall) {

	  // This call cannot affect the FD we're pursuing unless (a) it uses the FD, or (b) the FD escapes (is stored) and the function is non-pure.
	  bool callMayUseFD = false;

	  if(OS.FDEscapes && !CI->getCalledFunction()->doesNotAccessMemory())
	    callMayUseFD = true;

	  if(!callMayUseFD) {

	    for(unsigned i = 0; i < CI->getNumArgOperands() && !callMayUseFD; ++i) {

	      ValCtx ArgVC = getReplacement(CI->getArgOperand(i));
	      if(ArgVC == OpenInst)
		callMayUseFD = true;
	      if(isUnresolved(CI->getArgOperand(i))) {
		LPDEBUG("Assuming " << *CI << " may use " << OpenInst << " due to unresolved argument " << ArgVC << "\n");
		callMayUseFD = true;
	      }

	    }
	    
	  }

	  if(callMayUseFD) {

	    if(InlineAttempt* IA = getInlineAttempt(CI)) {

	      Start = make_vc(IA->getEntryBlock()->begin(), IA);
	      return true;

	    }
	    else {

	      LPDEBUG("Unexpanded call " << *CI << " may affect FD from " << OpenInst << "\n");
	      InstBlockedOpens[CI].push_back(std::make_pair(OpenInst, ReadInst));
	      return false;

	    }

	  }

	}

      }

    }

    skipFirst = false;

    ++BI;
    if(BI == BB->end()) {
      
      BasicBlock* UniqueSuccessor = 0;
      for(succ_iterator SI = succ_begin(BB), SE = succ_end(BB); SI != SE; ++SI) {

	if(edgeIsDead(BB, *SI))
	  continue;
	else if(UniqueSuccessor) {
	  UniqueSuccessor = 0;
	  break;
	}
	else {
	  UniqueSuccessor = *SI;
	}

      }

      if(UniqueSuccessor) {

	if(checkLoopIterationOrExit(BB, UniqueSuccessor, Start)) {
	  if(Start == VCNull) {
	    addBlockedOpen(OpenInst, ReadInst);
	    return false;
	  }
	  else
	    return true;
	}

	Loop* SuccLoop = LI[&F]->getLoopFor(UniqueSuccessor);
	if(SuccLoop != getLoopContext()) {

	  if((!getLoopContext()) || getLoopContext()->contains(SuccLoop)) {

	    if(PeelAttempt* LPA = getPeelAttempt(SuccLoop)) {

	      assert(SuccLoop->getHeader() == UniqueSuccessor);
	      Start = make_vc(UniqueSuccessor->begin(), LPA->Iterations[0]);
	      return true;

	    }
	    else {
	      
	      LPDEBUG("Open forwarding blocked by unexpanded loop " << SuccLoop->getHeader()->getName() << "\n");
	      addBlockedOpen(OpenInst, ReadInst);
	      return false;

	    }

	  }
	  else {

	    Start = make_vc(UniqueSuccessor->begin(), parent);
	    return true;

	  }

	}
	else {
	  
	  if(!certainBlocks.count(UniqueSuccessor)) {

	    LPDEBUG("Open forwarding blocked because block " << UniqueSuccessor->getName() << " not yet marked certain\n");
	    addBlockedOpen(OpenInst, ReadInst);
	    return false;

	  }
	  else {

	    BB = UniqueSuccessor;
	    BI = BB->begin();

	  }

	}

      }
      else {

	if(isa<ReturnInst>(BB->getTerminator())) {

	  if(!parent) {

	    LPDEBUG("VFS instruction chain reaches end of main!\n");
	    return false;

	  }
	  BasicBlock::iterator CallIt(getEntryInstruction());
	  ++CallIt;
	  Start = make_vc(CallIt, parent);
	  return true;

	}

	LPDEBUG("Open forwarding blocked because block " << BB->getName() << " has no unique successor\n");
	addBlockedOpen(OpenInst, ReadInst);
	return false;

      }

    }

  }

}

bool InlineAttempt::checkLoopIterationOrExit(BasicBlock* PresentBlock, BasicBlock* NextBlock, ValCtx& Start) {

  return false;

}

bool PeelIteration::checkLoopIterationOrExit(BasicBlock* PresentBlock, BasicBlock* NextBlock, ValCtx& Start) {

  if(PresentBlock == L->getLoopLatch() && NextBlock == L->getHeader()) {

    PeelIteration* nextIter = getNextIteration();
    if(!nextIter) {

      LPDEBUG("Can't continue to pursue open call because loop " << L->getHeader()->getName() << " does not yet have iteration " << iterationCount+1 << "\n");
      Start = VCNull;
      return true;

    }
    else {

      Start = make_vc(L->getHeader()->begin(), nextIter);
      return true;

    }

  }
  else if(!L->contains(NextBlock)) {

    // LCSSA, so this must be our parent
    Start = make_vc(NextBlock->begin(), parent);
    return true;

  }

  return false;

}

int64_t IntegrationAttempt::tryGetIncomingOffset(Value* V) {

  CallInst* CI = cast<CallInst>(V);

  DenseMap<CallInst*, ReadFile>::iterator it = resolvedReadCalls.find(CI);
  if(it != resolvedReadCalls.end())
    return it->second.incomingOffset + it->second.readSize;
  else {
    DenseMap<CallInst*, SeekFile>::iterator it = resolvedSeekCalls.find(CI);
    if(it != resolvedSeekCalls.end())
      return it->second.newOffset;
  }
  
  return -1;

}

ReadFile* IntegrationAttempt::tryGetReadFile(CallInst* CI) {

  DenseMap<CallInst*, ReadFile>::iterator it = resolvedReadCalls.find(CI);
  if(it != resolvedReadCalls.end())
    return &it->second;
  else
    return 0;

}

bool IntegrationAttempt::vfsCallBlocksOpen(CallInst* VFSCall, ValCtx OpenInst, ValCtx LastReadInst, OpenStatus& OS, bool& isVfsCall, bool& shouldRequeue) {

  // Call to read() or close()?

  isVfsCall = false;
  shouldRequeue = false;

  Function* Callee = VFSCall->getCalledFunction();
  if (!Callee->isDeclaration() || !(Callee->hasExternalLinkage() || Callee->hasDLLImportLinkage())) {
    // Call to an internal function. Our caller should handle this.
    return false;
  }
  StringRef CalleeName = Callee->getName();
  if(CalleeName == "read") {

    const FunctionType *FT = Callee->getFunctionType();
    if (FT->getNumParams() != 3 || !FT->getParamType(0)->isIntegerTy(32) ||
	!FT->getParamType(1)->isPointerTy() || !FT->getParamType(2)->isIntegerTy() ||
	!FT->getReturnType()->isIntegerTy()) {
      LPDEBUG("Assuming call to " << *Callee << " is not 'read' due to its weird signature\n");
      return false;
    }

    isVfsCall = true;

    Value* readFD = VFSCall->getArgOperand(0);
    if(isUnresolved(readFD)) {

      LPDEBUG("Can't forward open because FD argument of " << *VFSCall << " is unresolved\n");
      shouldRequeue = true;
      return true;

    }
    else if(getReplacement(readFD) != OpenInst) {

      LPDEBUG("Ignoring " << *VFSCall << " which references a different file\n");
      return false;

    }

    Value* readBytes = VFSCall->getArgOperand(2);

    ConstantInt* intBytes = dyn_cast<ConstantInt>(getConstReplacement(readBytes));
    if(!intBytes) {
      LPDEBUG("Can't push " << OpenInst << " further: read amount uncertain\n");
      shouldRequeue = true;
      return true;
    }

    // OK, we know what this read operation does. Record that and queue another exploration from this point.
    int64_t incomingOffset;
    if(LastReadInst == OpenInst)
      incomingOffset = 0;
    else {
      incomingOffset = LastReadInst.second->tryGetIncomingOffset(LastReadInst.first);
    }

    int cBytes = (int)intBytes->getLimitedValue();

    struct stat file_stat;
    if(::stat(OS.Name.c_str(), &file_stat) == -1) {
      
      LPDEBUG("Failed to stat " << OS.Name << "\n");
      return true;

    }
    
    int bytesAvail = file_stat.st_size - incomingOffset;
    if(cBytes > bytesAvail) {
      LPDEBUG("Desired read of " << cBytes << " truncated to " << bytesAvail << " (EOF)\n");
      cBytes = bytesAvail;
    }

    // OK, we know what this read operation does. Record that and queue another exploration from this point.

    LPDEBUG("Successfully forwarded to " << *VFSCall << " which reads " << cBytes << " bytes\n");

    resolveReadCall(VFSCall, ReadFile(&OS, incomingOffset, cBytes));
    ValCtx thisReader = make_vc(VFSCall, this);
    OS.LatestResolvedUser = thisReader;
    pass->queueOpenPush(OpenInst, thisReader);

    // Investigate anyone that refs the buffer
    investigateUsers(VFSCall->getArgOperand(1));

    // The number of bytes read is also the return value of read.
    setReplacement(VFSCall, const_vc(ConstantInt::get(Type::getInt64Ty(VFSCall->getContext()), cBytes)));
    investigateUsers(VFSCall);
    
    return true;

  }
  else if(CalleeName == "close") {
    const FunctionType *FT = Callee->getFunctionType();
    if(FT->getNumParams() != 1 || !FT->getParamType(0)->isIntegerTy(32)) {
      LPDEBUG("Assuming call to " << *Callee << " is not really 'close' due to weird signature\n");
      return false;
    }

    isVfsCall = true;

    Value* closeFD = VFSCall->getArgOperand(0);
    if(isUnresolved(closeFD)) {
      shouldRequeue = true;
      return true;
    }
    else if(getReplacement(closeFD) != OpenInst) {
      return false;
    }

    LPDEBUG("Successfully forwarded to " << *VFSCall << " which closes the file\n");

    OS.LatestResolvedUser = make_vc(VFSCall, this);
    return true;

  }
  else if(CalleeName == "llseek" || CalleeName == "lseek" || CalleeName == "llseek64") {

    const FunctionType* FT = Callee->getFunctionType();
    if(FT->getNumParams() != 3 || (!FT->getParamType(0)->isIntegerTy(32)) || (!FT->getParamType(1)->isIntegerTy()) || (!FT->getParamType(2)->isIntegerTy(32))) {
      LPDEBUG("Assuming call to " << *Callee << " is not really an [l]lseek due to weird signature\n");
      return false;
    }

    isVfsCall = true;

    Value* seekFD = VFSCall->getArgOperand(0);
    if(isUnresolved(seekFD)) {
      shouldRequeue = true;
      return true;
    }
    else if(getReplacement(seekFD) != OpenInst) {
      return false;
    }

    Constant* whence = getConstReplacement(VFSCall->getArgOperand(2));
    Constant* newOffset = getConstReplacement(VFSCall->getArgOperand(1));
    
    if((!newOffset) || (!whence)) {
      LPDEBUG("Unable to push " << OpenInst << " further due to uncertainty of " << *VFSCall << " seek offset or whence");
      shouldRequeue = true;
      return true;
    }

    uint64_t intOffset = cast<ConstantInt>(newOffset)->getLimitedValue();
    int32_t seekWhence = (int32_t)cast<ConstantInt>(whence)->getSExtValue();

    switch(seekWhence) {
    case SEEK_CUR:
      {
	int64_t incomingOffset;
	if(LastReadInst == OpenInst)
	  incomingOffset = 0;
	else {
	  incomingOffset = LastReadInst.second->tryGetIncomingOffset(LastReadInst.first);
	}      
	intOffset += incomingOffset;
      }
      break;
    case SEEK_END:
      {
	struct stat file_stat;
	if(::stat(OS.Name.c_str(), &file_stat) == -1) {
	  
	  LPDEBUG("Failed to stat " << OS.Name << "\n");
	  return true;
	  
	}
	intOffset += file_stat.st_size;
	break;
      }  
    case SEEK_SET:
      break;
    default:
      LPDEBUG("Seek whence parameter is unknown value " << seekWhence << "!");
      return true;
    }

    LPDEBUG("Successfully forwarded to " << *VFSCall << " which seeks to offset " << intOffset << "\n");

    // Seek's return value is the new offset.
    setReplacement(VFSCall, const_vc(ConstantInt::get(FT->getParamType(1), intOffset)));
    investigateUsers(VFSCall);

    resolveSeekCall(VFSCall, SeekFile(&OS, intOffset));

    ValCtx seekCall = make_vc(VFSCall, this);
    OS.LatestResolvedUser = seekCall;
    pass->queueOpenPush(OpenInst, seekCall);

    return true;

  }

  return false;

}

void IntegrationAttempt::resolveReadCall(CallInst* CI, struct ReadFile RF) {

  resolvedReadCalls[CI] = RF;

}

void IntegrationAttempt::resolveSeekCall(CallInst* CI, struct SeekFile SF) {

  resolvedSeekCalls[CI] = SF;

}

void IntegrationAttempt::addBlockedOpen(ValCtx OpenInst, ValCtx ReadInst) {

  CFGBlockedOpens.push_back(std::make_pair(OpenInst, ReadInst));

}

bool IntegrationAttempt::isResolvedVFSCall(const Instruction* I) {
  
  if(CallInst* CI = dyn_cast<CallInst>(const_cast<Instruction*>(I))) {

    return forwardableOpenCalls.count(CI) || resolvedReadCalls.count(CI) || resolvedSeekCalls.count(CI);

  }

  return false;

}

void PeelIteration::describe(raw_ostream& Stream) const {

  Stream << "(Loop " << L->getHeader()->getName() << "/" << iterationCount << ")";

}


void InlineAttempt::describe(raw_ostream& Stream) const {

  Stream << "(" << F.getName() << ")";

}

// GDB callable:
void IntegrationAttempt::dump() const {

  describe(outs());

}

void IntegrationAttempt::collectBlockStats(BasicBlock* BB) {

  for(BasicBlock::iterator BI = BB->begin(), BE = BB->end(); BI != BE; BI++) {
      
    if(instructionCounts(BI)) { 

      if(BB == getEntryBlock() && isa<PHINode>(BI))
	continue;

      improvableInstructions++;

      if(blockIsDead(BB))
	improvedInstructions++;
      else if(improvedValues.find(BI) != improvedValues.end())
	improvedInstructions++;
      else if(BranchInst* BrI = dyn_cast<BranchInst>(BI)) {
	if(BrI->isConditional() && (improvedValues.find(BrI->getCondition()) != improvedValues.end()))
	  improvedInstructions++;
      }

    }

    if(CallInst* CI = dyn_cast<CallInst>(BI)) {
      DenseMap<CallInst*, InlineAttempt*>::iterator it = inlineChildren.find(CI);
      if(it == inlineChildren.end())
	unexploredCalls.push_back(CI);
    }

  }

}

void IntegrationAttempt::collectLoopStats(const Loop* LoopI) {

  DenseMap<const Loop*, PeelAttempt*>::const_iterator it = peelChildren.find(LoopI);

  if(it == peelChildren.end()) {
    unexploredLoops.push_back(LoopI);
    for(Loop::block_iterator BI = LoopI->block_begin(), BE = LoopI->block_end(); BI != BE; ++BI)
      collectBlockStats(*BI);
  }

}

void InlineAttempt::collectAllBlockStats() {

  for(Function::iterator FI = F.begin(), FE = F.end(); FI != FE; ++FI)
    if(!LI[&F]->getLoopFor(FI))
      collectBlockStats(FI);

  for(LoopInfo::iterator LoopI = LI[&F]->begin(), LoopE = LI[&F]->end(); LoopI != LoopE; ++LoopI)
    collectLoopStats(*LoopI);

}

void PeelIteration::collectAllBlockStats() {

  for(Loop::block_iterator BI = L->block_begin(), BE = L->block_end(); BI != BE; ++BI) {
    if(LI[&F]->getLoopFor(*BI) == L)
      collectBlockStats(*BI);
  }

  for(Loop::iterator LoopI = L->begin(), LoopE = L->end(); LoopI != LoopE; ++LoopI)
    collectLoopStats(*LoopI);

}

void PeelAttempt::collectStats() {

  for(std::vector<PeelIteration*>::iterator it = Iterations.begin(), it2 = Iterations.end(); it != it2; ++it)
    (*it)->collectStats();

}

void IntegrationAttempt::collectStats() {

  collectAllBlockStats();

  for(DenseMap<CallInst*, InlineAttempt*>::const_iterator it = inlineChildren.begin(), it2 = inlineChildren.end(); it != it2; ++it)
    it->second->collectStats();

  for(DenseMap<const Loop*, PeelAttempt*>::const_iterator it = peelChildren.begin(), it2 = peelChildren.end(); it != it2; ++it)
    it->second->collectStats();

}

void InlineAttempt::printHeader(raw_ostream& OS) const {

  OS << (!CI ? "Root " : "") << "Function " << F.getName();
  if(CI)
    OS << " at " << *CI;

}

void PeelIteration::printHeader(raw_ostream& OS) const {

  OS << "Loop " << L->getHeader()->getName() << " iteration " << iterationCount;

}

void PeelAttempt::printHeader(raw_ostream& OS) const {

  OS << "Loop " << L->getHeader()->getName();

}

void PeelAttempt::print(raw_ostream& OS) const {

  OS << nestingIndent() << "Loop " << L->getHeader()->getName() << (Iterations.back()->iterStatus == IterationStatusFinal ? "(terminated)" : "(not terminated)") << "\n";

  for(std::vector<PeelIteration*>::const_iterator it = Iterations.begin(), it2 = Iterations.end(); it != it2; ++it) {

    (*it)->print(OS);

  }

}

void IntegrationAttempt::print(raw_ostream& OS) const {

  OS << nestingIndent();
  printHeader(OS);
  OS << ": improved " << improvedInstructions << "/" << improvableInstructions << "\n";
  for(DenseMap<Value*, ValCtx>::const_iterator it = improvedValues.begin(), it2 = improvedValues.end(); it != it2; ++it) {
    OS << nestingIndent() << *(it->first) << " -> " << it->second << "\n";
  }
  if(unexploredLoops.size()) {
    OS << nestingIndent() << "Unexplored loops:\n";
    for(SmallVector<const Loop*, 4>::const_iterator it = unexploredLoops.begin(), it2 = unexploredLoops.end(); it != it2; ++it) {
      OS << nestingIndent() << "  " << (*it)->getHeader()->getName() << "\n";
    }
  }
  if(unexploredCalls.size()) {
    OS << nestingIndent() << "Unexplored calls:\n";
    for(SmallVector<CallInst*, 4>::const_iterator it = unexploredCalls.begin(), it2 = unexploredCalls.end(); it != it2; ++it) {
      OS << nestingIndent() << **it << "\n";
    }
  }

  for(DenseMap<CallInst*, InlineAttempt*>::const_iterator it = inlineChildren.begin(), it2 = inlineChildren.end(); it != it2; ++it) {
    it->second->print(OS);
  }

  for(DenseMap<const Loop*, PeelAttempt*>::const_iterator it = peelChildren.begin(), it2 = peelChildren.end(); it != it2; ++it) {
    it->second->print(OS);
  }

}

std::string IntegrationAttempt::nestingIndent() const {

  return ind(nesting_depth * 2);

}

std::string PeelAttempt::nestingIndent() const {

  return ind(nesting_depth * 2);

}

// Implement LoadForwardAttempt

LoadForwardAttempt::LoadForwardAttempt(LoadInst* _LI, IntegrationAttempt* C) : LI(_LI), originalCtx(C), ExprValid(false) { }

void LoadForwardAttempt::describeSymExpr(raw_ostream& Str) {
  
  if(!tryBuildSymExpr())
    return;

  for(SmallVector<SymExpr*, 4>::iterator it = Expr.begin(), it2 = Expr.end(); it != it2; it++) {
    if(it != Expr.begin())
      Str << " of ";
    (*it)->describe(Str);
  }
  
}

// Make a symbolic expression for a given load instruction if it depends solely on one pointer
// with many constant offsets.
bool LoadForwardAttempt::buildSymExpr() {

  ValCtx Ptr = originalCtx->getDefaultVC(LI->getPointerOperand());

  LPDEBUG("Trying to describe " << Ptr << " as a simple symbolic expression\n");

  // Check that we're trying to fetch a cast-of-constGEP-of-cast-of... an identified object, and
  // build a symbolic expression representing the derived expression if so.
 
  bool success = true;

  while(1) {

    if(GEPOperator* GEP = dyn_cast<GEPOperator>(Ptr.first)) {
      SmallVector<Value*, 4> idxs;
      for (unsigned i = 1, e = GEP->getNumOperands(); i != e; ++i) {
	Value* idx = GEP->getOperand(i);
	Constant* Cidx = getConstReplacement(idx, Ptr.second);
	if(Cidx)
	  idxs.push_back(Cidx);
	else {
	  LPDEBUG("Can't describe pointer with non-const offset " << *idx << "\n");
	  success = false; 
	  break;
	}
      }
      Expr.push_back((new SymGEP(idxs)));
      Ptr = make_vc(GEP->getPointerOperand(), Ptr.second);
    }
    else if(BitCastInst* C = dyn_cast<BitCastInst>(Ptr.first)) {
      Expr.push_back((new SymCast(C->getType())));
      Ptr = make_vc(C->getOperand(0), Ptr.second);
    }
    else if (isa<Constant>(Ptr.first)) {
      Expr.push_back(new SymThunk(Ptr));
      break;
    }
    else {
      ValCtx Repl = Ptr.second->getReplacement(Ptr.first);
      if(isIdentifiedObject(Repl.first)) {
	Expr.push_back((new SymThunk(Repl)));
	break;
      }
      else if(Repl == Ptr) {
	LPDEBUG("Can't describe due to unresolved pointer " << Ptr << "\n");
	success = false; 
	break;
      }
      else {
	Ptr = Repl; // Must continue resolving!
      }
    }
    
  }

  return success;

}

bool LoadForwardAttempt::tryBuildSymExpr() {

  if(ExprValid)
    return (Expr.size() > 0);
  else {
    bool ret = buildSymExpr();
    ExprValid = true;
    return ret;
  }

}

bool LoadForwardAttempt::canBuildSymExpr() {

  // Perhaps we could do some quickier checks than just making the thing right away?
  return tryBuildSymExpr();

}

SmallVector<SymExpr*, 4>* LoadForwardAttempt::getSymExpr() {
  
  if(!tryBuildSymExpr())
    return 0;
  else
    return &Expr;
  
}

LoadForwardAttempt& LoadForwardAttempt::getLFA() {

  return *this;

}

IntegrationAttempt* LoadForwardAttempt::getOriginalCtx() {

  return originalCtx;

}

LoadInst* LoadForwardAttempt::getOriginalInst() {

  return LI;

}

LoadInst* LoadForwardAttempt::getQueryInst() {

  return LI;

}

LoadForwardAttempt::~LoadForwardAttempt() {

  for(SmallVector<SymExpr*, 4>::iterator it = Expr.begin(), it2 = Expr.end(); it != it2; it++) {
    delete (*it);
  }

}

// Precondition for both: checked Expr is a real thing already.

ValCtx LoadForwardAttempt::getBaseVC() { 
  return (cast<SymThunk>(Expr.back()))->RealVal; 
}

HCFParentCallbacks* LoadForwardAttempt::getBaseContext() { 
  return getBaseVC().second; 
}

std::pair<DenseMap<std::pair<BasicBlock*, const Loop*>, MemDepResult>::iterator, bool> LoadForwardAttempt::getLastIterCache(BasicBlock* FromBB, const Loop* L) {
  return lastIterCache.insert(std::make_pair(std::make_pair(FromBB, L), MemDepResult()));
}

std::pair<DenseMap<const Loop*, MemDepResult>::iterator, bool> LoadForwardAttempt::getOtherItersCache(const Loop* L) {
  return otherItersCache.insert(std::make_pair(L, MemDepResult()));
}

// Implement LFARealisation

// Realise a symbolic expression at a given location. 
// Temporary instructions are created and recorded for later deletion.
LFARealization::LFARealization(LoadForwardAttempt& _LFA, IntegrationAttempt* IA, Instruction* InsertPoint) : LFA(_LFA) {

  // Build it backwards: the in chain should end in a defined object, in or outside our scope.
  // Start with that, then wrap it incrementally in operators.
  // Precondition: LFA.canBuildSymExpr()
  
  SmallVector<SymExpr*, 4>& in = *(LFA.getSymExpr());
  SmallVector<SymExpr*, 4>::iterator SI = in.end(), SE = in.begin();
  
  Value* lastPtr;
  
  SI--;
  SymThunk* th = cast<SymThunk>(*SI);

  LLVMContext& ctx = InsertPoint->getContext();
  BasicBlock::iterator BI(InsertPoint);
  IRBuilder<> Builder(ctx);
  Builder.SetInsertPoint(InsertPoint->getParent(), *BI);

  // I make up a fake location that we're supposedly accessing. The structure is
  // %pointless = alloca()
  // %junk = load %pointless
  // %expr_0 = gep(%junk, ...)
  // %expr_1 = bitcast(%expr_0)
  // ...
  // %expr_n = gep(%expr_n_1, ...)
  // %accessor = load %expr_n
  // Then our caller should set his local improvedValues so that junk resolves to
  // the base pointer he wishes to query (i.e. the base pointer from the SymExpr),
  // and then issue a MemDep query against accessor.

  Instruction* FakeLoc = Builder.CreateAlloca(th->RealVal.first->getType());
  tempInstructions.push_back(FakeLoc);
  lastPtr = FakeBase = Builder.CreateLoad(FakeLoc);
  tempInstructions.push_back(FakeBase);

  while(SI != SE) {
    SI--;
    if(SymGEP* GEP = dyn_cast<SymGEP>(*SI)) {
      lastPtr = Builder.CreateGEP(lastPtr, GEP->Offsets.begin(), GEP->Offsets.end());
    }
    else if(SymCast* Cast = dyn_cast<SymCast>(*SI)) {
      lastPtr = Builder.CreateBitCast(lastPtr, Cast->ToType);
    }
    else {
      assert(0 && "Investigated expression should only contain GEPs and Casts except at the end\n");
    }
    //LPDEBUG("Created temporary instruction: " << *lastPtr << "\n");
    tempInstructions.push_back(cast<Instruction>(lastPtr));
  }

  // Make up a fake load, since MD wants an accessor.
  QueryInst = Builder.CreateLoad(lastPtr);
  tempInstructions.push_back(QueryInst);

  //  LPDEBUG("Temporarily augmented parent block:\n");
  //  DEBUG(dbgs() << *Where->getParent());

}

LFARealization::~LFARealization() {

  for(SmallVector<Instruction*, 4>::iterator II = tempInstructions.end(), IE = tempInstructions.begin(); II != IE; ) {
    Instruction* I = *(--II);
    I->eraseFromParent();
  }

}

LoadInst* LFARealization::getQueryInst() {

  return QueryInst;

}

LoadInst* LFARealization::getOriginalInst() {

  return LFA.getOriginalInst();

}

IntegrationAttempt* LFARealization::getOriginalCtx() {

  return LFA.getOriginalCtx();

}

LoadForwardAttempt& LFARealization::getLFA() {

  return LFA;

}

Instruction* LFARealization::getFakeBase() {

  return FakeBase;

}

// Implement LFARMapping

// Precondition: LFAR.getLFA().canBuildSymExpr()
LFARMapping::LFARMapping(LFARealization& _LFAR, IntegrationAttempt* _Ctx) : LFAR(_LFAR), Ctx(_Ctx) {

  SymThunk* Th = cast<SymThunk>(LFAR.getLFA().getSymExpr()->back());
  Ctx->setReplacement(LFAR.getFakeBase(), Th->RealVal);

}

LFARMapping::~LFARMapping() {

  Ctx->eraseReplacement(LFAR.getFakeBase());

}

void IntegratorWQItem::execute() { 
  switch(type) {
  case TryEval:
    ctx->tryEvaluate(u.V);
    break;
  case CheckBlock:
    ctx->checkBlock(u.BB);
    break;
  case CheckLoad:
    ctx->checkLoad(u.LI);
    break;
  case OpenPush:
    ctx->tryPushOpen(u.OpenArgs.OpenI, u.OpenArgs.OpenProgress);
    break;
  }
}

void IntegratorWQItem::describe(raw_ostream& s) {

  switch(type) {
  case TryEval:
    s << "Try-eval " << *(u.V);
    break;
  case CheckBlock:
    s << "Check-BB-status " << u.BB->getName();
    break;
  case CheckLoad:
    s << "Check-load " << make_vc(u.LI, ctx);
    break;
  case OpenPush:
    s << "Push-VFS-chain " << make_vc(u.OpenArgs.OpenI, ctx);
  }

}

BasicBlock* IntegrationHeuristicsPass::getUniqueReturnBlock(Function* F) {

  DenseMap<Function*, BasicBlock*>::iterator it = uniqueReturnBlocks.find(F);
  
  if(it != uniqueReturnBlocks.end())
    return it->second;

  BasicBlock* uniqueReturnBlock = 0;

  for(Function::iterator FI = F->begin(), FE = F->end(); FI != FE; ++FI) {

    BasicBlock* BB = FI;

    if(isa<ReturnInst>(BB->getTerminator())) {
      if(!uniqueReturnBlock)
	uniqueReturnBlock = BB;
      else {
	uniqueReturnBlock = 0;
	break;
      }
    }

  }

  uniqueReturnBlocks[F] = uniqueReturnBlock;

  return uniqueReturnBlock;

}

void IntegrationHeuristicsPass::createInvariantScopes(Function* F, DenseMap<Instruction*, const Loop*>*& pInsts, DenseMap<std::pair<BasicBlock*, BasicBlock*>, const Loop*>*& pEdges, DenseMap<BasicBlock*, const Loop*>*& pBlocks) {

  DenseMap<Instruction*, const Loop*>& Insts = invariantInstScopes[F];
  DenseMap<std::pair<BasicBlock*, BasicBlock*>, const Loop*>& Edges = invariantEdgeScopes[F];
  DenseMap<BasicBlock*, const Loop*>& Blocks = invariantBlockScopes[F];
  pInsts = &Insts;
  pEdges = &Edges;
  pBlocks = &Blocks;

  LoopInfo* LI = LIs[F];

  DEBUG(dbgs() << "Discovering loop invariants for function " << F->getName() << "\n");

  bool improvedThisTime;

  do {

    improvedThisTime = false;

    for(Function::iterator FI = F->begin(), FE = F->end(); FI != FE; ++FI) {

      BasicBlock* BB = FI;
      const Loop* instLoop = LI->getLoopFor(BB);
      for(BasicBlock::iterator BI = BB->begin(), BE = BB->end(); BI != BE; ++BI) {

	Instruction* I = BI;
	const Loop* innermostLoop = 0;

	// Skip instructions that can't be evaluated in any case, and loads because we'd need to do a bunch more analysis to establish that they're really invariant.
	if(I->mayReadFromMemory() || I->mayWriteToMemory())
	  continue;
	if(BranchInst* BI = dyn_cast<BranchInst>(I)) {
	  if(!BI->isConditional())
	    continue;
	}
	if(isa<CallInst>(I) || isa<InvokeInst>(I)) {
	  // Invariant calls are very silly! Surely this means it is really variant thanks to side-effects via globals or the like.
	  // Possible future improvement: spot whether a call really is invariant (i.e. looks invariant and is pure) whilst expanding it for the first time and promote it.
	  continue;
	}

	for (unsigned i = 0, e = I->getNumOperands(); i != e; ++i) {
	  Value* Op = I->getOperand(i);
	  if(Instruction* OpI = dyn_cast<Instruction>(Op)) {
	    // LCSSA form means this loop must be somewhere in instLoop's ancestors (including instLoop itself), not a sibling.
	    DenseMap<Instruction*, const Loop*>::iterator Improved = Insts.find(OpI);
	    const Loop* OpL;
	    if(Improved != Insts.end())
	      OpL = Improved->second;
	    else
	      OpL = LI->getLoopFor(OpI->getParent());
	    if(OpL == instLoop) {
	      // Common case: this is a common or garden variant. Nothing to see here.
	      innermostLoop = instLoop;
	      break;
	    }
	    else if((!innermostLoop) || innermostLoop->contains(OpL)) {
	      innermostLoop = OpL;
	    }
	  }
	}

	if(((!innermostLoop) && instLoop) || (innermostLoop && (innermostLoop != instLoop) && innermostLoop->contains(instLoop))) {
	  
	  DenseMap<Instruction*, const Loop*>::iterator Existing = Insts.find(I);
	  if(Existing != Insts.end() && Existing->second == innermostLoop)
	    continue;
	  improvedThisTime = true;
	  // An interesting invariant! But which kind?
	  if(Existing != Insts.end())
	    Existing->second = innermostLoop;
	  else
	    Insts[I] = innermostLoop;
	  DEBUG(dbgs() << "Instruction " << *I << " loop invariant: will evaluate in scope " << (innermostLoop ? innermostLoop->getHeader()->getName() : "'root'") << "\n");
	  if(TerminatorInst* TI = dyn_cast<TerminatorInst>(I)) {
	    unsigned NumSucc = TI->getNumSuccessors();
	    for (unsigned i = 0; i != NumSucc; ++i) {
	      DEBUG(dbgs() << "\tincluding edge " << BB->getName() << " -> " << TI->getSuccessor(i)->getName() << "\n");
	      Edges[std::make_pair(BB, TI->getSuccessor(i))] = innermostLoop;
	    }
	  }
	}

      }

    }

  } while(improvedThisTime);

  // Now figure out blocks which can be killed as an invariant, and consequently further edges, and so on.
  SmallVector<BasicBlock*, 4> WQ1;
  SmallVector<BasicBlock*, 4> WQ2;
  SmallVector<BasicBlock*, 4>* ConsumeQ = &WQ1;
  SmallVector<BasicBlock*, 4>* ProduceQ = &WQ2;

  for(DenseMap<std::pair<BasicBlock*, BasicBlock*>, const Loop*>::iterator it = Edges.begin(), it2 = Edges.end(); it != it2; ++it) {

    ConsumeQ->push_back(it->first.second);

  }

  while(ConsumeQ->size()) {

    for(SmallVector<BasicBlock*, 4>::iterator WI = ConsumeQ->begin(), WE = ConsumeQ->end(); WI != WE; ++WI) {

      BasicBlock* CheckBB = *WI;
      const Loop* innermostPred = 0;
      bool shouldSkip = false;
      const Loop* CheckBBL = LI->getLoopFor(CheckBB);
      
      for(pred_iterator PI = pred_begin(CheckBB), PE = pred_end(CheckBB); PI != PE; ++PI) {

	DenseMap<std::pair<BasicBlock*, BasicBlock*>, const Loop*>::iterator EdgeIt = Edges.find(std::make_pair(*PI, CheckBB));
	if(EdgeIt == Edges.end()) {
	  // The edge is a plain old variant, and so the block is too.
	  shouldSkip = true;
	  break;
	}
	else {
	  const Loop* edgeL = EdgeIt->second;
	  if(edgeL == CheckBBL) {
	    // Edge is a local variant; so is the block
	    shouldSkip = true;
	    break;
	  }
	  if((!innermostPred) || (innermostPred->contains(edgeL)))
	    innermostPred = edgeL;
	}

      }

      if(!shouldSkip) {
	DenseMap<BasicBlock*, const Loop*>::iterator BlockIt = Blocks.find(CheckBB);
	if(BlockIt == Blocks.end() || BlockIt->second != innermostPred) {
	  if(BlockIt == Blocks.end())
	    Blocks[CheckBB] = innermostPred;
	  else
	    BlockIt->second = innermostPred;
	  TerminatorInst* TI = CheckBB->getTerminator();
	  if(BranchInst* BI = dyn_cast<BranchInst>(TI)) {
	    if(!BI->isConditional()) {
	      BasicBlock* Succ = BI->getSuccessor(0);
	      Edges[std::make_pair(CheckBB, Succ)] = innermostPred;
	      ProduceQ->push_back(Succ);
	    }
	  }
	  else {
	    // For these conditional cases the edges will have been categorised as invariant by the terminator argument check above.
	    for(succ_iterator SI = succ_begin(CheckBB), SE = succ_end(CheckBB); SI != SE; ++SI) {
	      ProduceQ->push_back(*SI);
	    }
	  }
	}
      }

    }

    ConsumeQ->clear();
    std::swap(ConsumeQ, ProduceQ);

  }

  for(DenseMap<std::pair<BasicBlock*, BasicBlock*>, const Loop*>::iterator EdgeIt = Edges.begin(), EdgeItE = Edges.end(); EdgeIt != EdgeItE; ++EdgeIt) {

    DEBUG(dbgs() << "Edge " << EdgeIt->first.first->getName() << " -> " << EdgeIt->first.second->getName() << " is invariant; will evaluate at scope " << (EdgeIt->second ? EdgeIt->second->getHeader()->getName() : "root") << "\n");

  }

  for(DenseMap<BasicBlock*, const Loop*>::iterator BlockIt = Blocks.begin(), BlockItE = Blocks.end(); BlockIt != BlockItE; ++BlockIt) {

    DEBUG(dbgs() << "Block " << BlockIt->first->getName() << " is invariant; will evaluate at scope " << (BlockIt->second ? BlockIt->second->getHeader()->getName() : "root") << "\n");

  }

}

DenseMap<Instruction*, const Loop*>& IntegrationHeuristicsPass::getInstScopes(Function* F) {

  DenseMap<Function*, DenseMap<Instruction*, const Loop*> >::iterator it = invariantInstScopes.find(F);
  if(it != invariantInstScopes.end())
    return it->second;
  else {
    DenseMap<Instruction*, const Loop*>* instScopes;
    DenseMap<std::pair<BasicBlock*, BasicBlock*>, const Loop*>* edgeScopes;
    DenseMap<BasicBlock*, const Loop*>* blockScopes;
    createInvariantScopes(F, instScopes, edgeScopes, blockScopes);
    return *instScopes;
  }

}

DenseMap<std::pair<BasicBlock*, BasicBlock*>, const Loop*>& IntegrationHeuristicsPass::getEdgeScopes(Function* F) {

  DenseMap<Function*, DenseMap<std::pair<BasicBlock*, BasicBlock*>, const Loop*> >::iterator it = invariantEdgeScopes.find(F);
  if(it != invariantEdgeScopes.end())
    return it->second;
  else {
    DenseMap<Instruction*, const Loop*>* instScopes;
    DenseMap<std::pair<BasicBlock*, BasicBlock*>, const Loop*>* edgeScopes;
    DenseMap<BasicBlock*, const Loop*>* blockScopes;
    createInvariantScopes(F, instScopes, edgeScopes, blockScopes);
    return *edgeScopes;
  }

}

DenseMap<BasicBlock*, const Loop*>& IntegrationHeuristicsPass::getBlockScopes(Function* F) {

  DenseMap<Function*, DenseMap<BasicBlock*, const Loop*> >::iterator it = invariantBlockScopes.find(F);
  if(it != invariantBlockScopes.end())
    return it->second;
  else {
    DenseMap<Instruction*, const Loop*>* instScopes;
    DenseMap<std::pair<BasicBlock*, BasicBlock*>, const Loop*>* edgeScopes;
    DenseMap<BasicBlock*, const Loop*>* blockScopes;
    createInvariantScopes(F, instScopes, edgeScopes, blockScopes);
    return *blockScopes;
  }

}

bool IntegrationHeuristicsPass::runOnModule(Module& M) {

  TD = getAnalysisIfAvailable<TargetData>();
  AA = &getAnalysis<AliasAnalysis>();
  
  for(Module::iterator MI = M.begin(), ME = M.end(); MI != ME; MI++) {

    if(!MI->isDeclaration())
      LIs[MI] = &getAnalysis<LoopInfo>(*MI);

  }

  for(Module::iterator MI = M.begin(), ME = M.end(); MI != ME; MI++) {

    if(MI->isDeclaration())
      continue;

    Function& F = *MI;

    DEBUG(dbgs() << "Considering inlining starting at " << F.getName() << ":\n");

    InlineAttempt* IA = new InlineAttempt(this, 0, F, LIs, TD, AA, 0, getInstScopes(&F), getEdgeScopes(&F), getBlockScopes(&F), 0);

    rootAttempts.push_back(IA);

    SmallVector<IntegratorWQItem, 64>* consumeQueue = &workQueue1;
    produceQueue = &workQueue2;

    queueCheckBlock(IA, &(F.getEntryBlock()));
    IA->queueInitialWork();

    while(workQueue1.size() || workQueue2.size()) {

      for(SmallVector<IntegratorWQItem, 64>::iterator it = consumeQueue->begin(), itend = consumeQueue->end(); it != itend; ++it) {

	DEBUG(dbgs() << "Dequeue: ");
	DEBUG(it->describe(dbgs()));
	DEBUG(dbgs() << "\n");
	it->execute();

      }

      consumeQueue->clear();
      if(consumeQueue == &workQueue1) {
	consumeQueue = &workQueue2;
	produceQueue = &workQueue1;
      }
      else {
	consumeQueue = &workQueue1;
	produceQueue = &workQueue2;
      }

    }
    
    IA->collectStats();
    
  }

  return false;

}

void IntegrationHeuristicsPass::getAnalysisUsage(AnalysisUsage &AU) const {

  AU.addRequired<AliasAnalysis>();
  AU.addRequired<LoopInfo>();
  AU.setPreservesAll();
  
}
