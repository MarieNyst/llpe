
#include "llvm/Function.h"
#include "llvm/BasicBlock.h"
#include "llvm/Instructions.h"
#include "llvm/IntrinsicInst.h"
#include "llvm/GlobalVariable.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/MemoryBuiltins.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

//// Type definitions: the LF walker, and its 2 derivatives for normal and PB LF.

namespace llvm {

struct LFPathContext {

  bool cacheAllowed;
  ShadowBB* readOnlyUntilBB;

  LFPathContext() : cacheAllowed(0), readOnlyUntilBB(0) { }
  LFPathContext(const struct LFPathContext& other) : cacheAllowed(other.cacheAllowed), readOnlyUntilBB(other.readOnlyUntilBB) { }

};

struct NormalLoadForwardWalker : public BackwardIAWalker {

  ShadowValue LoadedPtr;
  uint64_t LoadSize;

  ShadowValue LoadPtrBase;
  int64_t LoadPtrOffset;

  const Type* originalType;
  bool OptimisticMode;
  
  BasicBlock* cacheThresholdBB;
  IntegrationAttempt* cacheThresholdIA;

  PartialVal& inputPV;
  PointerBase Result;
  std::vector<std::string> OverdefReasons;
  std::vector<ShadowInstruction*> UsedInstructions;
  PointerBase* activeCacheEntry;
  IntegrationAttempt* usedCacheEntryIA;
  LFCacheKey usedCacheEntryKey;
  bool inLoopAnalyser;

  bool walkVerbose;

  NormalLoadForwardWalker(ShadowInstruction* Start, ShadowValue Ptr, uint64_t Size, const Type* OT, bool OM, BasicBlock* optBB, IntegrationAttempt* optIA, struct LFPathContext* firstCtx, PartialVal& iPV, bool iLA, bool WV) : BackwardIAWalker(Start->invar->idx, Start->parent, true, firstCtx), LoadedPtr(Ptr), LoadSize(Size), originalType(OT), OptimisticMode(OM), cacheThresholdBB(optBB), cacheThresholdIA(optIA), inputPV(iPV), activeCacheEntry(0), usedCacheEntryIA(0), inLoopAnalyser(iLA), walkVerbose(WV) { 

    LoadPtrOffset = 0;
    if(!getBaseAndConstantOffset(LoadedPtr, LoadPtrBase, LoadPtrOffset, /* ignoreNull = */ true)) {
      LoadPtrBase = ShadowValue();
      LoadPtrOffset = 0;
    }

  }

  NormalLoadForwardWalker(ShadowInstruction* Start, ShadowValue PtrBase, int64_t PtrOffset, uint64_t Size, const Type* OT, bool OM, BasicBlock* optBB, IntegrationAttempt* optIA, struct LFPathContext* firstCtx, PartialVal& iPV, bool iLA) : BackwardIAWalker(Start->invar->idx, Start->parent, true, firstCtx), LoadedPtr(), LoadSize(Size), LoadPtrBase(PtrBase), LoadPtrOffset(PtrOffset), originalType(OT), OptimisticMode(OM), cacheThresholdBB(optBB), cacheThresholdIA(optIA), inputPV(iPV), activeCacheEntry(0), usedCacheEntryIA(0), inLoopAnalyser(iLA), walkVerbose(false) { 

  }

  virtual WalkInstructionResult walkInstruction(ShadowInstruction*, void*);
  virtual bool shouldEnterCall(ShadowInstruction*, void*);

  WalkInstructionResult handleAlias(ShadowInstruction* SI, SVAAResult R, ShadowValue Ptr, uint64_t PtrSize, void* Ctx);
  virtual WalkInstructionResult reachedTop();
  virtual WalkInstructionResult mayAscendFromContext(IntegrationAttempt* IA, void*);
  bool addPartialVal(PartialVal& PV, PointerBase& PB, std::string& error, ShadowInstruction* I, uint64_t FirstDef, uint64_t FirstNotDef, bool cacheAllowed, bool maySubquery);
  bool getMIOrReadValue(ShadowInstruction* SI, uint64_t FirstDef, uint64_t FirstNotDef, int64_t ReadOffset, uint64_t LoadSize, PartialVal& NewPV, PointerBase&, std::string& error);
  virtual bool blockedByUnexpandedCall(ShadowInstruction*, void*);
  bool* getValidBuf();
  virtual WalkInstructionResult walkFromBlock(ShadowBB* BB, void* Ctx);
  PointerBase PVToPB(PartialVal& PV, raw_string_ostream& RSO);
  ShadowValue PVToSV(PartialVal& PV, raw_string_ostream& RSO);

  void addPBDefn(PointerBase& NewPB, bool cacheAllowed);
  void _addPBDefn(PointerBase& MergeWith, PointerBase& NewPB);
  void setPBOverdef(std::string reason, bool cacheAllowed);

  void cancelCache();

  virtual void freeContext(void* ctx) {
    delete ((struct LFPathContext*)ctx);
  }

  virtual void* copyContext(void* ctx) {
    struct LFPathContext* newctx = new LFPathContext(*((struct LFPathContext*)ctx));
    return (void*)newctx;
  }

};

}

bool IntAAProxy::isNoAliasPBs(ShadowValue Ptr1Base, int64_t Ptr1Offset, uint64_t Ptr1Size, ShadowValue Ptr2, uint64_t Ptr2Size) {

  return (tryResolvePointerBases(Ptr1Base, Ptr1Offset, Ptr1Size, Ptr2, Ptr2Size, true) == SVNoAlias);

}

//// Implement generic LF

bool NormalLoadForwardWalker::shouldEnterCall(ShadowInstruction* SI, void* context) {

  AliasAnalysis::ModRefResult Res;
  struct LFPathContext* LCtx = (struct LFPathContext*)context;
  CallInst* CI = cast_inst<CallInst>(SI);

  if(LCtx->readOnlyUntilBB)
    return !!SI->parent->IA->getInlineAttempt(CI);

  if(LoadPtrBase.isInval()) {

    Res = GlobalAA->getCSModRefInfo(SI, LoadedPtr, LoadSize, true);

  }
  else {

    IntAAProxy AACB;
    Res = GlobalAA->getCSModRefInfoWithOffset(SI, LoadPtrBase, LoadPtrOffset, LoadSize, AACB);

  }

  switch(Res) {
    
  case AliasAnalysis::NoModRef:
  case AliasAnalysis::Ref: 
    {
      // Consider entering the call just to look for cache entries we can use:
      // Is there no point because caching is forbidden for this query?
      if(!cacheThresholdIA) 
	return false;
      // Is the call unavailable?
      InlineAttempt* IA = SI->parent->IA->getInlineAttempt(CI);
      if(!IA)
	return false;
      // OK, set a threshold when we'll start walking "properly" again.
      LCtx->readOnlyUntilBB = IA->BBs[0];
      return true;
    }
  default:
    return true;
  }

}

WalkInstructionResult NormalLoadForwardWalker::walkInstruction(ShadowInstruction* I, void* Ctx) {

  struct LFPathContext* LCtx = (struct LFPathContext*)Ctx;
  // We're inside a readonly or readnone function -- we only bother walking at all
  // to find cache entries; there can't be any stores or the like.
  if(LCtx->readOnlyUntilBB)
    return WIRContinue;

  ShadowValue Ptr;
  uint64_t PtrSize;

  if (inst_is<StoreInst>(I)) {

    Ptr = I->getOperand(1);
    PtrSize = GlobalAA->getTypeStoreSize(I->invar->I->getOperand(0)->getType());
    // Fall through to alias analysis

  }
  else if (inst_is<AllocaInst>(I) || (inst_is<CallInst>(I) && extractMallocCall(I->invar->I))) {
    
    if(LoadPtrBase == I) {
      return handleAlias(I, SVMustAlias, ShadowValue(I), LoadSize, Ctx);
    }
    else
      return WIRContinue;

  }
  else if(inst_is<MemIntrinsic>(I)) {

    Ptr = I->getCallArgOperand(0);
    ConstantInt* CI = dyn_cast_or_null<ConstantInt>(getConstReplacement(I->getCallArgOperand(2)));
    PtrSize = CI ? CI->getLimitedValue() : AliasAnalysis::UnknownSize;
    // Fall through to alias analysis

  }
  else if(CallInst* CI = dyn_cast_inst<CallInst>(I)) {

    if(ReadFile* RF = I->parent->IA->tryGetReadFile(CI)) {
      
      Ptr = I->getCallArgOperand(1);
      PtrSize = RF->readSize;
      // Fall through to alias analysis

    }
    else if(Function* CalledF = getCalledFunction(I)) {

      if(CalledF->getName() == "llvm.va_start" || CalledF->getName() == "llvm.va_copy") {

	Ptr = I->getCallArgOperand(0);
	PtrSize = 24;

      }
      else if(CalledF->getName() == "realloc") {

	Ptr = I;
	PtrSize = AliasAnalysis::UnknownSize;

      }
      else {

	return WIRContinue;

      }

    }
    else {

      return WIRContinue;

    }

  }
  else {

    return WIRContinue;

  }

  SVAAResult R;
  if(LoadedPtr.isInval())
    R = tryResolvePointerBases(LoadPtrBase, LoadPtrOffset, LoadSize, Ptr, PtrSize, true);
  else
    R = aliasSVs(LoadedPtr, LoadSize, Ptr, PtrSize, true);
  if(R == SVNoAlias)
    return WIRContinue;

  return handleAlias(I, R, Ptr, PtrSize, Ctx);

}

//// Implement guts of PartialVal:

void PartialVal::initByteArray(uint64_t nbytes) {

  type = PVByteArray;

  uint64_t nqwords = (nbytes + 7) / 8;
  partialBuf = new uint64_t[nqwords];

  if(!partialValidBuf) {

    partialValidBuf = new bool[nbytes];
    for(uint64_t i = 0; i < nbytes; ++i)
      partialValidBuf[i] = false;

  }

  partialBufBytes = nbytes;
  loadFinished = false;

}

PartialVal::PartialVal(uint64_t nbytes) : TotalIV(), C(0), ReadOffset(0), partialValidBuf(0)  {

  initByteArray(nbytes);

}

PartialVal& PartialVal::operator=(const PartialVal& Other) {

  if(partialBuf) {
    delete[] partialBuf;
    partialBuf = 0;
  }
  if(partialValidBuf) {
    delete[] partialValidBuf;
    partialValidBuf = 0;
  }

  type = Other.type;
  TotalIV = Other.TotalIV;
  TotalIVType = Other.TotalIVType;
  C = Other.C;
  ReadOffset = Other.ReadOffset;

  if(Other.partialBuf) {

    partialBuf = new uint64_t[(Other.partialBufBytes + 7) / 8];
    memcpy(partialBuf, Other.partialBuf, Other.partialBufBytes);

  }

  if(Other.partialValidBuf) {

    partialValidBuf = new bool[Other.partialBufBytes];
    memcpy(partialValidBuf, Other.partialValidBuf, Other.partialBufBytes);
    
  }

  partialBufBytes = Other.partialBufBytes;
  loadFinished = Other.loadFinished;

  return *this;

}

PartialVal::PartialVal(const PartialVal& Other) {

  partialBuf = 0;
  partialValidBuf = 0;
  (*this) = Other;

}

PartialVal::~PartialVal() {

  if(partialBuf) {
    delete[] partialBuf;
  }
  if(partialValidBuf) {
    delete[] partialValidBuf;
  }

}

bool* PartialVal::getValidArray(uint64_t nbytes) {

  if(!partialValidBuf) {
    partialValidBuf = new bool[nbytes];
    partialBufBytes = nbytes;
  }

  return partialValidBuf;

}

static uint64_t markPaddingBytes(bool* pvb, const Type* Ty, TargetData* TD) {

  uint64_t marked = 0;

  if(const StructType* STy = dyn_cast<StructType>(Ty)) {
    
    const StructLayout* SL = TD->getStructLayout(STy);
    if(!SL) {
      DEBUG(dbgs() << "Couldn't get struct layout for type " << *STy << "\n");
      return 0;
    }

    uint64_t EIdx = 0;
    for(StructType::element_iterator EI = STy->element_begin(), EE = STy->element_end(); EI != EE; ++EI, ++EIdx) {

      marked += markPaddingBytes(&(pvb[SL->getElementOffset(EIdx)]), *EI, TD);
      uint64_t ThisEStart = SL->getElementOffset(EIdx);
      uint64_t ESize = (TD->getTypeSizeInBits(*EI) + 7) / 8;
      uint64_t NextEStart = (EIdx + 1 == STy->getNumElements()) ? SL->getSizeInBytes() : SL->getElementOffset(EIdx + 1);
      for(uint64_t i = ThisEStart + ESize; i < NextEStart; ++i, ++marked) {
	
	pvb[i] = true;

      }

    }

  }
  else if(const ArrayType* ATy = dyn_cast<ArrayType>(Ty)) {

    uint64_t ECount = ATy->getNumElements();
    const Type* EType = ATy->getElementType();
    uint64_t ESize = (TD->getTypeSizeInBits(EType) + 7) / 8;

    uint64_t Offset = 0;
    for(uint64_t i = 0; i < ECount; ++i, Offset += ESize) {

      marked += markPaddingBytes(&(pvb[Offset]), EType, TD);

    }

  }

  return marked;

}

bool PartialVal::isComplete() {

  return isTotal() || isPartial() || loadFinished;

}

bool PartialVal::convertToBytes(uint64_t size, TargetData* TD, std::string& error) {

  if(isByteArray())
    return true;

  PartialVal conv(size);
  if(!conv.combineWith(*this, 0, size, size, TD, error))
    return false;

  (*this) = conv;

  return true;

}

bool PartialVal::combineWith(PartialVal& Other, uint64_t FirstDef, uint64_t FirstNotDef, uint64_t LoadSize, TargetData* TD, std::string& error) {

  if(isEmpty()) {

    if(FirstDef == 0 && (FirstNotDef - FirstDef == LoadSize)) {

      *this = Other;
      return true;

    }
    else {

      // Transition to bytewise load forwarding: this value can't satisfy
      // the entire requirement. Turn into a PVByteArray and fall through.
      initByteArray(LoadSize);

    }

  }

  assert(isByteArray());

  if(Other.isTotal()) {

    Constant* TotalC = dyn_cast_or_null<Constant>(Other.TotalIV.V.getVal());
    if(!TotalC) {
      //LPDEBUG("Unable to use total definition " << itcache(PV.TotalVC) << " because it is not constant but we need to perform byte operations on it\n");
      error = "PP2";
      return false;
    }
    Other.C = TotalC;
    Other.ReadOffset = 0;
    Other.type = PVPartial;

  }

  DEBUG(dbgs() << "This store can satisfy bytes (" << FirstDef << "-" << FirstNotDef << "] of the source load\n");

  // Store defined some of the bytes we need! Grab those, then perhaps complete the load.

  unsigned char* tempBuf;

  if(Other.isPartial()) {

    tempBuf = (unsigned char*)alloca(FirstNotDef - FirstDef);
    // ReadDataFromGlobal assumes a zero-initialised buffer!
    memset(tempBuf, 0, FirstNotDef - FirstDef);

    if(!ReadDataFromGlobal(Other.C, Other.ReadOffset, tempBuf, FirstNotDef - FirstDef, *TD)) {
      DEBUG(dbgs() << "ReadDataFromGlobal failed; perhaps the source " << *(Other.C) << " can't be bitcast?\n");
      error = "RDFG";
      return false;
    }

  }
  else {

    tempBuf = (unsigned char*)Other.partialBuf;

  }

  assert(FirstDef < partialBufBytes);
  assert(FirstNotDef <= partialBufBytes);

  // Avoid rewriting bytes which have already been defined
  for(uint64_t i = 0; i < (FirstNotDef - FirstDef); ++i) {
    if(partialValidBuf[FirstDef + i]) {
      continue;
    }
    else {
      ((unsigned char*)partialBuf)[FirstDef + i] = tempBuf[i]; 
    }
  }

  loadFinished = true;
  // Meaning of the predicate: stop at the boundary, or bail out if there's no more setting to do
  // and there's no hope we've finished.
  for(uint64_t i = 0; i < LoadSize && (loadFinished || i < FirstNotDef); ++i) {

    if(i >= FirstDef && i < FirstNotDef) {
      partialValidBuf[i] = true;
    }
    else {
      if(!partialValidBuf[i]) {
	loadFinished = false;
      }
    }

  }

  return true;

}

//// Implement Normal LF:

static bool containsPointerTypes(const Type* Ty) {

  if(Ty->isPointerTy())
    return true;

  for(Type::subtype_iterator it = Ty->subtype_begin(), it2 = Ty->subtype_end(); it != it2; ++it) {

    if(containsPointerTypes(*it))
      return true;

  }

  return false;

}

bool NormalLoadForwardWalker::addPartialVal(PartialVal& PV, PointerBase& PB, std::string& error, ShadowInstruction* I, uint64_t FirstDef, uint64_t FirstNotDef, bool cacheAllowed, bool maySubquery) {

  if(PB.Overdef) {
    addPBDefn(PB, cacheAllowed);
    return false;
  }

  // For now, forbid using pursuing several different subqueries because a partial defn had multiple values.
  if(PB.Values.size() >= 1) {

    if(FirstDef == 0 && FirstNotDef == LoadSize && inputPV.isEmpty() && (PB.Type == ValSetTypeFD || PB.Type == ValSetTypeVarArg || PB.Type == ValSetTypePB || PB.Values[0].V.getType() == originalType)) {

      addPBDefn(PB, cacheAllowed);
      if(PB.Overdef)
	error = "OD";
      return !PB.Overdef;

    }
    else if(PB.Values.size() == 1 && PB.Type == ValSetTypeScalar) {

      PV = PartialVal::getPartial(cast<Constant>(PB.Values[0].V.getVal()), 0);
      // Fall through to standard PV case
      
    }
    else {

      errs() << "Defn is ";
      I->parent->IA->printPB(errs(), PB);
      errs() << "\n";
      error = "PMV";
      return false;

    }
    
  }

  PartialVal valSoFar(inputPV);
  if(!valSoFar.combineWith(PV, FirstDef, FirstNotDef, LoadSize, GlobalTD, error))
    return false;

  PointerBase NewPB;

  if(!valSoFar.isComplete()) {

    // Disallow complex queries when solving for loop invariants:
    if(maySubquery && !inLoopAnalyser) {

      NewPB = tryForwardLoadSubquery(I, LoadedPtr, LoadPtrBase, LoadPtrOffset, LoadSize, originalType, valSoFar, error);

    }
    else {

      if(maySubquery)
	error = "RQL";
      else
	error = "RT";
      return false;

    }

  }
  else {

    std::string synthError;
    ShadowValue NewSV;
    {
      raw_string_ostream RSO(synthError);
      NewPB = PVToPB(valSoFar, RSO);
    }
    if(!NewPB.isInitialised()) {
      error = synthError;
      return false;
    }

  }

  addPBDefn(NewPB, cacheAllowed);
  return !Result.Overdef;

}

bool NormalLoadForwardWalker::getMIOrReadValue(ShadowInstruction* I, uint64_t FirstDef, uint64_t FirstNotDef, int64_t ReadOffset, uint64_t LoadSize, PartialVal& NewPV, PointerBase& NewPB, std::string& error) {

  bool ret;

  if (inst_is<MemIntrinsic>(I)) {

    if(inst_is<MemSetInst>(I))
      ret = getMemsetPV(I, FirstNotDef - FirstDef, NewPV, error);
    else {
      bool* validBytes = inputPV.isByteArray() ? inputPV.partialValidBuf : 0;
      ret = getMemcpyPB(I, FirstDef, FirstNotDef, ReadOffset, LoadSize, originalType, validBytes, NewPV, NewPB, error, cacheThresholdBB, cacheThresholdIA);
    }

  }
  else {

    Function* F = getCalledFunction(I);
    if(F->getName() == "read") {
      
      ret = getReadPV(I, FirstNotDef - FirstDef, ReadOffset, NewPV, error);

    }
    else if(F->getName() == "llvm.va_start") {

      ret = getVaStartPV(I, ReadOffset, NewPV, error);

    }
    else if(F->getName() == "realloc") {

      bool* validBytes = inputPV.isByteArray() ? inputPV.partialValidBuf : 0;
      ret = getReallocPB(I, FirstDef, FirstNotDef, ReadOffset, LoadSize, originalType, validBytes, NewPB, error, cacheThresholdBB, cacheThresholdIA);

    }
    else {

      assert(F->getName() == "llvm.va_copy");
      bool* validBytes = inputPV.isByteArray() ? inputPV.partialValidBuf : 0;
      ret = getVaCopyPB(I, FirstDef, FirstNotDef, ReadOffset, LoadSize, originalType, validBytes, NewPB, error, cacheThresholdBB, cacheThresholdIA);

    }

  }

  return ret;

}



#define NLFWFail(Code) do { std::string failureText; { raw_string_ostream RSO(failureText); RSO << Code << " " << I->parent->IA->itcache(I); }  setPBOverdef(failureText, cacheAllowed); if(!cacheAllowed) { cancelCache(); } return WIRStopWholeWalk; } while(0);

WalkInstructionResult NormalLoadForwardWalker::handleAlias(ShadowInstruction* I, SVAAResult R, ShadowValue Ptr, uint64_t PtrSize, void* Ctx) { 

  PartialVal NewPV;
  PointerBase NewPB;
  uint64_t FirstDef, FirstNotDef, ReadOffset;

  // If we're in the optimistic phase, ignore anything but the following:
  // * Defining stores with an associated PB
  // * Defining alloca instructions
  // Unexpanded calls are also significant but these are caught by blockedByUnexpandedCall.
  // Don't behave optimistically if we're outside the loop subject to consideration.

  if(R == SVMustAlias)
    UsedInstructions.push_back(I);

  bool cacheAllowed = ((struct LFPathContext*)Ctx)->cacheAllowed;

  if(OptimisticMode && !cacheAllowed) {

    bool ignore = true;

    if(R == SVMustAlias) {
      if(inst_is<AllocaInst>(I))
	ignore = false;
      else if(inst_is<StoreInst>(I)) {
	PointerBase ResPB;
	ShadowValue StoredVal = I->getOperand(0);
	if(StoredVal.isVal() || getPointerBase(StoredVal, ResPB))
	  ignore = false;
	else
	  return WIRStopThisPath;
      }
    }
      
    if(ignore)
      return WIRContinue;

  }

  if(walkVerbose) {

    errs() << "alias " << I->parent->IA->itcache(I) << "\n";
    
  }
  
  if(R == SVMustAlias) {

    FirstDef = 0; FirstNotDef = std::min(LoadSize, PtrSize); ReadOffset = 0;

    if(inst_is<StoreInst>(I)) {

      if(!getPointerBase(I->getOperand(0), NewPB)) {
	// Defined by store with no value
	NLFWFail("DNS");
      }
      else if(NewPB.Overdef) {
	NLFWFail("DO");
      }

    }
    else if(inst_is<AllocaInst>(I) || (inst_is<CallInst>(I) && extractMallocCall(I->invar->I))) {

      const Type* defType;
      if(AllocaInst* AI = dyn_cast_inst<AllocaInst>(I)) 
	defType = AI->getAllocatedType();
      else
	defType = Type::getIntNTy(I->invar->I->getContext(), 8 * LoadSize);
      
      NewPV = PartialVal::getTotal(defType->isPointerTy() ? ValSetTypePB : ValSetTypeScalar, ImprovedVal(ShadowValue(Constant::getNullValue(defType))));

    }
    else {

      std::string error;
      if(!getMIOrReadValue(I, 0, std::min(LoadSize, PtrSize), 0, LoadSize, NewPV, NewPB, error)) {

	// Memcpy, memset or read failed
	NLFWFail(error);

      }
      // Else fall through

    }

  }
  else {
    
    // MayAlias

    int64_t WriteOffset = 0;
    ShadowValue WriteBase;
    if(getBaseAndConstantOffset(Ptr, WriteBase, WriteOffset)) {

      if(GetDefinedRange(LoadPtrBase, LoadPtrOffset, LoadSize,
			 WriteBase, WriteOffset, PtrSize,
			 FirstDef, FirstNotDef, ReadOffset)) {

	if(inst_is<StoreInst>(I)) {

	  Constant* StoreC = getConstReplacement(I->getOperand(0));
	  if(!StoreC) {

	    // Partial defn by store of non-const
	    NLFWFail("NCS");

	  }
	  else {

	    NewPV = PartialVal::getPartial(StoreC, ReadOffset);

	  }

	}
	else {

	  std::string error;
	  if(!getMIOrReadValue(I, FirstDef, FirstNotDef, ReadOffset, LoadSize, NewPV, NewPB, error)) {
	
	    // Memset, memcpy or read failed
	    NLFWFail(error);

	  }
	  // Else fall through
	
	}

      }
      else {
	
	NLFWFail("C");

      }

    }
    else {

      // We don't know enough about one or other pointer, must assume this write
      // trashes the entire value.
      NLFWFail("C");

    }

  }

  if(walkVerbose) {

    errs() << "Merge ";
    I->parent->IA->printPB(errs(), Result);
    errs() << " with ";
    I->parent->IA->printPB(errs(), NewPB);
    errs() << "\n";

  }

  std::string error;
  if(!addPartialVal(NewPV, NewPB, error, I, FirstDef, FirstNotDef, cacheAllowed, true)) {
    // Couldn't perform some implicit cast, or encountered a conflict
    NLFWFail(error);
  }

  return WIRStopThisPath;

}

WalkInstructionResult NormalLoadForwardWalker::reachedTop() {

  if(GlobalVariable* GV = dyn_cast_or_null<GlobalVariable>(LoadPtrBase.getVal())) {
	    
    if(GV->hasDefinitiveInitializer()) {

      DEBUG(dbgs() << "Load using global initialiser " << (*(GV->getInitializer())) << "\n");

      Constant* GVC = GV->getInitializer();
      uint64_t GVCSize = (GlobalTD->getTypeSizeInBits(GVC->getType()) + 7) / 8;
      uint64_t FirstNotDef = std::min(GVCSize - LoadPtrOffset, LoadSize);
      DEBUG(dbgs() << "Read offset is " << LoadPtrOffset << "\n");

      PartialVal GPV = PartialVal::getPartial(GVC, LoadPtrOffset);
      PartialVal valSoFar = inputPV;
      std::string error;
      
      PointerBase NoPB;
      if(!addPartialVal(GPV, NoPB, error, 0, 0, FirstNotDef, true, false))
	return WIRStopWholeWalk;
      else
	return WIRStopThisPath;

    }

  }
  
  if(!OptimisticMode) {
    setPBOverdef("Reached top", true);
    return WIRStopWholeWalk;
  }
  else {
    return WIRStopThisPath;
  }

}

bool* NormalLoadForwardWalker::getValidBuf() {

  return inputPV.getValidArray(LoadSize);

}

void NormalLoadForwardWalker::_addPBDefn(PointerBase& MergeWith, PointerBase& NewPB) {

  bool WasOverdef = MergeWith.Overdef;
  MergeWith.merge(NewPB);
  if(MergeWith.Overdef && (!WasOverdef) && (!NewPB.Overdef))
    OverdefReasons.push_back("Fan-in");

}

void NormalLoadForwardWalker::addPBDefn(PointerBase& NewPB, bool cacheAllowed) {

  _addPBDefn(Result, NewPB);
  if(activeCacheEntry && cacheAllowed)
    _addPBDefn(*activeCacheEntry, NewPB);

}

void NormalLoadForwardWalker::setPBOverdef(std::string reason, bool cacheAllowed) {
  OverdefReasons.push_back(reason);
  Result = PointerBase::getOverdef();
  if(activeCacheEntry && cacheAllowed)
    *activeCacheEntry = PointerBase::getOverdef();
}

void NormalLoadForwardWalker::cancelCache() {

  if(activeCacheEntry) {

    LFCacheKey Key = LFCK(cacheThresholdBB, LoadPtrBase, LoadPtrOffset, LoadSize);
    cacheThresholdIA->deleteLFPBCacheEntry(Key);
    activeCacheEntry = 0;

  }

}

bool NormalLoadForwardWalker::blockedByUnexpandedCall(ShadowInstruction* I, void* Ctx) {

  bool cacheAllowed = ((struct LFPathContext*)Ctx)->cacheAllowed;

  // Memory intrinsics are dealt with in the direct effects path.
  if(inst_is<MemIntrinsic>(I))
    return false;

  if(OptimisticMode && !cacheAllowed) {

    bool ignore = true;

    Function* CF = getCalledFunction(I);
    if(!CF)
      ignore = false;
    else {
      if((!functionIsBlacklisted(CF)) && !functionBlacklistedWithinLoops(CF))
	ignore = false;
    }

    // Grim hack: usually a function maybe-modifying a pointer is a sign that the function's
    // arguments need to be better resolved -- e.g. read(2) modifying some buffer, and we should
    // look past it. In other cases the function is known to clobber for certain and always will,
    // but modrefinfo isn't expressive enough to tell us that.
    // A particular case is errno: it's easily recognised, and frequently clobbered. Don't ignore
    // for that case, but TODO make modrefinfo capable of describe *WILL* clobber vs. *MAY* clobber.
    
    if(!LoadPtrBase.isInval()) {

      if(GlobalVariable* GV = dyn_cast_val<GlobalVariable>(LoadPtrBase)) {

	if(GV->getName() == "errno")
	  ignore = false;

      }

    }

    if(ignore)
      return false;

  }

  std::string RStr;
  raw_string_ostream RSO(RStr);
  RSO << "UEC " << I->parent->IA->itcache(I, true);
  RSO.flush();
  setPBOverdef(RStr, cacheAllowed);

  if(!cacheAllowed)
    cancelCache();

  return true;

}

WalkInstructionResult NormalLoadForwardWalker::mayAscendFromContext(IntegrationAttempt* IA, void* Ctx) {

  bool cacheAllowed = ((struct LFPathContext*)Ctx)->cacheAllowed;

  if(ShadowInstruction* SI = LoadPtrBase.getInst()) {

    if(IA == SI->parent->IA) {
    
      if(!OptimisticMode) {
	setPBOverdef("Scope", cacheAllowed);
	if(!cacheAllowed)
	  cancelCache();
	return WIRStopWholeWalk;
      }
      else {
	return WIRStopThisPath;
      }

    }
    
  }

  return WIRContinue;

}

PointerBase* IntegrationAttempt::getLFPBCacheEntry(LFCacheKey& Key) {

  DenseMap<LFCacheKey, PointerBase>::iterator it = LFPBCache.find(Key);
  if(it != LFPBCache.end())
    return &(it->second);
  else
    return 0;

}

void IntegrationAttempt::deleteLFPBCacheEntry(LFCacheKey& Key) {

  release_assert(LFPBCache.erase(Key));

}

PointerBase* IntegrationAttempt::createLFPBCacheEntry(LFCacheKey& Key) {

  return &(LFPBCache[Key]);

}

WalkInstructionResult NormalLoadForwardWalker::walkFromBlock(ShadowBB* BB, void* Ctx) {

  struct LFPathContext* LCtx = (struct LFPathContext*)Ctx;

  if(BB == LCtx->readOnlyUntilBB)
    LCtx->readOnlyUntilBB = 0;

  if(!LCtx->cacheAllowed) {

    // Don't cache if we're not certain what location we're addressing.
    if(LoadPtrBase.isInval())
      return WIRContinue;

    // See if we're walking from the first block that is cache-eligible
    if(BB->invar->BB == cacheThresholdBB && BB->IA == cacheThresholdIA) {

      LPDEBUG("Left loop at " << BB->invar->BB->getName() << "\n");
      LCtx->cacheAllowed = 1;

    }
    else {

      return WIRContinue;

    }

  }

  // No point either looking for cache entries or making them if the block isn't a certainty
  // or the cache threshold (an uncertain threshold indicates a temporary cachepoint)
  if(BB->status != BBSTATUS_CERTAIN && BB->invar->BB != cacheThresholdBB)
    return WIRContinue;

  // See if this block has a cache entry for us:
  LFCacheKey Key = LFCK(BB->invar->BB, LoadPtrBase, LoadPtrOffset, LoadSize);
  if(PointerBase* CachedPB = BB->IA->getLFPBCacheEntry(Key)) {
      
    if(walkVerbose) {

      errs() << "Use cache entry at " << BB->invar->BB->getName() << "\n";
      errs() << "Entry = ";
      BB->IA->printPB(errs(), *CachedPB);
      errs() << "\n";

    }
    LPDEBUG("Use cache entry at " << BB->invar->BB->getName() << "\n");
    if(CachedPB->Overdef) {
      std::string cacheODReason;
      {
	raw_string_ostream RSO(cacheODReason);
	RSO << "COD " << BB->invar->BB->getName() << "/" << BB->IA->F.getName();
      }
      OverdefReasons.push_back(cacheODReason);
    }
    addPBDefn(*CachedPB, true);
    
    usedCacheEntryIA = BB->IA;
    usedCacheEntryKey = Key;

    return WIRStopThisPath;

    // Don't delete this potentially redundant cache entry just yet!
    // We might yet abort this walk and want to keep it.
    // Instead clean it up in TFLPB below if necessary.

  }
  else if(!activeCacheEntry) {

    // This is necessarily the cache threshold:
    LPDEBUG("Create cache entry at " << BB->getName() << "\n");
    // Make a cache entry here:
    activeCacheEntry = BB->IA->createLFPBCacheEntry(Key);
    return WIRContinue;
      
  }
  else {
      
    // Keep building existing entry
    return WIRContinue;

  }

}
 
PointerBase NormalLoadForwardWalker::PVToPB(PartialVal& PV, raw_string_ostream& RSO) {

  // Try to use an entire value:
  if(PV.isTotal()) {

    release_assert(PV.TotalIVType == ValSetTypeScalar || PV.TotalIVType == ValSetTypeVarArg);
    if(PV.TotalIVType == ValSetTypeVarArg)
      return PointerBase::get(PV.TotalIV, PV.TotalIVType);

    const Type* sourceType = PV.TotalIV.V.getType();

    if(allowTotalDefnImplicitCast(sourceType, originalType) || allowTotalDefnImplicitPtrToInt(sourceType, originalType, GlobalTD))
      return PointerBase::get(PV.TotalIV, PV.TotalIVType);

  }

  ShadowValue NewSV = PVToSV(PV, RSO);
  if(NewSV.isInval())
    return PointerBase();

  PointerBase NewPB;
  if(!getPointerBase(NewSV, NewPB)) {
    RSO << "PVToPB";
    return PointerBase();
  }

  return NewPB;

}

ShadowValue NormalLoadForwardWalker::PVToSV(PartialVal& PV, raw_string_ostream& RSO) {

  // Otherwise try to use a sub-value:
  if(PV.isTotal() || PV.isPartial()) {

    // Try to salvage a total definition from a partial if this is a load clobbered by a store
    // of a larger aggregate type. This is to permit pointers and other non-constant forwardable values
    // to be moved about. In future our value representation needs to get richer to become a recursive type like
    // ConstantStruct et al.

    // Note that because you can't write an LLVM struct literal featuring a non-constant,
    // the only kinds of pointers this permits to be moved around are globals, since they are constant pointers.
    Constant* SalvageC = PV.isTotal() ? dyn_cast_or_null<Constant>(PV.TotalIV.V.getVal()) : PV.C;

    if(SalvageC) {

      uint64_t Offset = PV.isTotal() ? 0 : PV.ReadOffset;
      Constant* extr = extractAggregateMemberAt(SalvageC, Offset, originalType, LoadSize, GlobalTD);
      if(extr)
	return ShadowValue(extr);

    }
    else {

      RSO << "NonConstBOps";
      return ShadowValue();

    }

  }

  // Finally build it from bytes.
  std::string error;
  if(!PV.convertToBytes(LoadSize, GlobalTD, error)) {
    RSO << error;
    return ShadowValue();
  }

  assert(PV.isByteArray());

  if(containsPointerTypes(originalType)) {

    // If we're trying to synthesise a pointer from raw bytes, only a null pointer is allowed.
    unsigned char* checkBuf = (unsigned char*)PV.partialBuf;
    for(unsigned i = 0; i < PV.partialBufBytes; ++i) {

      if(checkBuf[i]) {
	RSO << "Non-null Ptr Byteops";
	return ShadowValue();
      }

    }

  }

  return ShadowValue(constFromBytes((unsigned char*)PV.partialBuf, originalType, GlobalTD));

}

bool IntegrationAttempt::tryResolveLoadFromConstant(ShadowInstruction* LoadI, PointerBase& Result, std::string& error, bool finalise) {

  // A special case: loading from a symbolic vararg:

  PointerBase PtrPB;
  if(!getPointerBase(LoadI->getOperand(0), PtrPB))
    return false;

  if(PtrPB.Type == ValSetTypeVarArg && PtrPB.Values.size() == 1) {
  
    ImprovedVal& IV = PtrPB.Values[0];
    if(IV.getVaArgType() != ImprovedVal::va_baseptr) {
    
      ShadowInstruction* PtrI = IV.V.getInst();
      PtrI->parent->IA->getVarArg(IV.Offset, Result);
      //LPDEBUG("va_arg " << itcache(IV.V) << " " << IV.Offset << " yielded " << printPB(Result) << "\n");
    
      return true;

    }

  }

  ShadowValue PtrBase;
  int64_t PtrOffset;

  if(getBaseAndConstantOffset(LoadI->getOperand(0), PtrBase, PtrOffset)) {

    if(GlobalVariable* GV = dyn_cast_or_null<GlobalVariable>(PtrBase.getVal())) {

      if(GV->isConstant()) {

	uint64_t LoadSize = (GlobalTD->getTypeSizeInBits(LoadI->getType()) + 7) / 8;
	const Type* FromType = GV->getInitializer()->getType();
	uint64_t FromSize = (GlobalTD->getTypeSizeInBits(FromType) + 7) / 8;

	if(PtrOffset < 0 || PtrOffset + LoadSize > FromSize) {
	  error = "Const out of range";
	  Result = PointerBase::getOverdef();
	  return true;
	}

	Constant* ExVal = extractAggregateMemberAt(GV->getInitializer(), PtrOffset, LoadI->getType(), LoadSize, GlobalTD);

	if(ExVal) {
      
	  getPointerBase(ShadowValue(ExVal), Result);
	  if(!((!Result.Overdef) && Result.Values.size() > 0)) {
	    error = "No PB for ExVal";
	    Result = PointerBase::getOverdef();
	  }

	  return true;

	}

	int64_t CSize = GlobalTD->getTypeAllocSize(GV->getInitializer()->getType());
	if(CSize < PtrOffset) {
	  
	  LPDEBUG("Can't forward from constant: read from global out of range\n");
	  error = "Const out of range 2";
	  Result = PointerBase::getOverdef();
	  return true;
	    
	}

	unsigned char* buf = (unsigned char*)alloca(LoadSize);
	memset(buf, 0, LoadSize);
	if(ReadDataFromGlobal(GV->getInitializer(), PtrOffset, buf, LoadSize, *GlobalTD)) {

	  getPointerBase(ShadowValue(constFromBytes(buf, LoadI->getType(), GlobalTD)), Result);
	  return true;
	    
	}
	else {

	  LPDEBUG("ReadDataFromGlobal failed\n");
	  error = "Const RDFG failed";
	  Result = PointerBase::getOverdef();
	  return true;

	}

      }

    }

  }
      
  // Check for loads which are pointless to pursue further because they're known to be rooted on
  // a constant global but we're uncertain what offset within that global we're looking for:

  if(ShadowInstruction* SI = LoadI->getOperand(0).getInst()) {

    if(SI->i.PB.Values.size() > 0 && SI->i.PB.Type == ValSetTypePB) {

      bool foundNonNull = false;
      bool foundNonConst = false;
      for(unsigned i = 0; i < SI->i.PB.Values.size(); ++i) {

	Value* BaseV = SI->i.PB.Values[i].V.getVal();

	if(BaseV && isa<ConstantPointerNull>(BaseV))
	  continue;

	foundNonNull = true;

	GlobalVariable* GV = dyn_cast_or_null<GlobalVariable>(BaseV);
	if((!GV) || !GV->isConstant())
	  foundNonConst = true;

      }

      if(!foundNonNull) {

	// Suppose that loading from a known null returns a null result.
	// TODO: convert this to undef, and thus rationalise the multi-load path.
	const Type* defType = LoadI->getType();
	Constant* nullVal = Constant::getNullValue(defType);
	std::pair<ValSetType, ImprovedVal> ResultIV = getValPB(nullVal);
	Result = PointerBase::get(ResultIV.second, ResultIV.first);
	return true;

      }
      else if(!foundNonConst) {

	LPDEBUG("Load cannot presently be resolved, but is rooted on a constant global. Abandoning search\n");
	error = "Const pointer vague";
	if(finalise)
	  Result = PointerBase::getOverdef();
	else
	  Result = PointerBase();
	return true;

      }

    }

  }

  return false;

}

PointerBase llvm::tryForwardLoadSubquery(ShadowInstruction* StartInst, ShadowValue LoadPtr, ShadowValue LoadPtrBase, int64_t LoadPtrOffset, uint64_t LoadSize, const Type* originalType, PartialVal& ResolvedSoFar, std::string& error) {

  struct LFPathContext* disableCaching = new LFPathContext();

  if(LoadPtr.isInval()) {
    NormalLoadForwardWalker Walker(StartInst, LoadPtrBase, LoadPtrOffset, LoadSize, originalType, false, 0, 0, disableCaching, ResolvedSoFar, false);
    Walker.walk();
    
    if(Walker.Result.Overdef) {
      
      error = "";
      raw_string_ostream RSO(error);
      RSO << "SQ3 (" << describePBWalker(Walker, StartInst->parent->IA) << ")";
      
    }

    return Walker.Result;
  }
  else {
    NormalLoadForwardWalker Walker(StartInst, LoadPtr, LoadSize, originalType, false, 0, 0, disableCaching, ResolvedSoFar, false, false);
    Walker.walk();
    
    if(Walker.Result.Overdef) {
      
      error = "";
      raw_string_ostream RSO(error);
      RSO << "SQ1 (" << describePBWalker(Walker, StartInst->parent->IA) << ")";
      
    }

    return Walker.Result;
  }

}

  // Like normal load forwarding, but using a base+offset instead of a pointer.
  // This is used when forwarding through a copy instruction. 
PointerBase llvm::tryForwardLoadArtificial(ShadowInstruction* StartInst, ShadowValue LoadBase, int64_t LoadOffset, uint64_t LoadSize, const Type* targetType, bool* alreadyValidBytes, std::string& error, BasicBlock* cacheThresholdBB, IntegrationAttempt* cacheThresholdIA, bool inLoopAnalyser, bool optimisticMode) {

  PartialVal emptyPV;
  struct LFPathContext* firstCtx = new LFPathContext();
  
  NormalLoadForwardWalker Walker(StartInst, LoadBase, LoadOffset, LoadSize, targetType, optimisticMode, cacheThresholdBB, cacheThresholdIA, firstCtx, emptyPV, inLoopAnalyser);

  if(alreadyValidBytes) {
    bool* validBytes = Walker.getValidBuf();
    memcpy(validBytes, alreadyValidBytes, sizeof(bool) * LoadSize);
  }

  Walker.walk();

  if(Walker.Result.Overdef) {

    error = "";
    raw_string_ostream RSO(error);
    RSO << "SQ2 (" << describePBWalker(Walker, StartInst->parent->IA) << ")";

  }

  return Walker.Result;

}

//// PBLF Interface

std::string llvm::describePBWalker(NormalLoadForwardWalker& Walker, IntegrationAttempt* IA) {
  
  std::string out;
  {
    raw_string_ostream RSO(out);
  
    if(Walker.Result.Overdef) {
      for(unsigned i = 0; i < Walker.OverdefReasons.size(); ++i) {
	if(i != 0)
	  RSO << ", ";
	RSO << Walker.OverdefReasons[i];
      }
    }  
    else if(Walker.Result.Values.size() == 0) {
    
      RSO << "No defn";
    
    }
    else {
    
      IA->printPB(RSO, Walker.Result, true);
    
    }
  }
    
  return out;
    
}
  
static double time_diff(struct timespec& start, struct timespec& end) {

  timespec temp;
  if ((end.tv_nsec-start.tv_nsec)<0) {
    temp.tv_sec = end.tv_sec-start.tv_sec-1;
    temp.tv_nsec = 1000000000+end.tv_nsec-start.tv_nsec;
  } else {
    temp.tv_sec = end.tv_sec-start.tv_sec;
    temp.tv_nsec = end.tv_nsec-start.tv_nsec;
  }

  return (temp.tv_sec) + (((double)temp.tv_nsec) / 1000000000.0);

}

static bool multiLoadEnabled = true;

static bool shouldMultiload(PointerBase& PB) {

  if(PB.Overdef || PB.Values.size() == 0)
    return false;

  if(PB.Type != ValSetTypePB)
    return false;

  uint32_t numNonNulls = 0;

  for(uint32_t i = 0, ilim = PB.Values.size(); i != ilim; ++i) {

    if(Value* V = PB.Values[i].V.getVal()) {
      if(isa<ConstantPointerNull>(V))
	continue;      
    }

    if(PB.Values[i].Offset == LLONG_MAX)
      return false;

    ++numNonNulls;

  }

  return numNonNulls > 1;

}

static bool tryMultiload(ShadowInstruction* LI, bool finalise, PointerBase& NewPB, BasicBlock* CacheThresholdBB, IntegrationAttempt* CacheThresholdIA, LoopPBAnalyser* LPBA, std::string& report) {

  uint32_t LoadSize = GlobalAA->getTypeStoreSize(LI->getType());

  // We already know that LI's PB is made up entirely of nulls and definite pointers.
  NewPB = PointerBase();
  PointerBase LIPB;
  getPointerBase(LI->getOperand(0), LIPB);

  raw_string_ostream RSO(report);
  RSO << "ML! ";

  std::string thisError;

  for(uint32_t i = 0, ilim = LIPB.Values.size(); i != ilim && !NewPB.Overdef; ++i) {

    if(Value* V = LIPB.Values[i].V.getVal()) {
      if(isa<ConstantPointerNull>(V)) {

	const Type* defType = LI->getType();
	Constant* nullVal = Constant::getNullValue(defType);
	std::pair<ValSetType, ImprovedVal> ResultIV = getValPB(nullVal);
	PointerBase NullPB = PointerBase::get(ResultIV.second, ResultIV.first);
	NewPB.merge(NullPB);
	continue;

      }
    }

    PointerBase ThisPB = tryForwardLoadArtificial(LI, LIPB.Values[i].V, LIPB.Values[i].Offset, LoadSize, LI->getType(), /* alreadyValidBytes = */ 0, thisError, CacheThresholdBB, CacheThresholdIA, !!LPBA, !finalise);

    NewPB.merge(ThisPB);

    if(ThisPB.Overdef) {
	
      RSO << LI->parent->IA->itcache(LIPB.Values[i].V, true) << ": " << thisError;

    }
    else {
	
      RSO << LI->parent->IA->itcache(LIPB.Values[i].V, true) << ": ";
      LI->parent->IA->printPB(RSO, ThisPB, true);

    }

    RSO << ", ";

  }

  return NewPB.isInitialised();

}

// Do load forwarding, possibly in optimistic mode: this means that
// stores that def but which have no associated PB are optimistically assumed
// to be compatible with anything, the same as the mergepoint logic above
// when finalise is false. When finalise = true this is just like normal load
// forwarding operating in PB mode.
bool IntegrationAttempt::tryForwardLoadPB(ShadowInstruction* LI, bool finalise, PointerBase& NewPB, BasicBlock* CacheThresholdBB, IntegrationAttempt* CacheThresholdIA, LoopPBAnalyser* LPBA) {

  PointerBase ConstResult;
  std::string error;
  if(tryResolveLoadFromConstant(LI, ConstResult, error, finalise)) {
    NewPB = ConstResult;
    if(NewPB.Overdef) {
      if(!finalise)
	optimisticForwardStatus[LI->invar->I] = error;
      else
	pessimisticForwardStatus[LI->invar->I] = error;
    }
    return NewPB.isInitialised();
  }

  if(multiLoadEnabled) {

    PointerBase LoadPtrPB;
    getPointerBase(LI->getOperand(0), LoadPtrPB);
    if(shouldMultiload(LoadPtrPB)) {

      std::string report;

      bool ret = tryMultiload(LI, finalise, NewPB, CacheThresholdBB, CacheThresholdIA, LPBA, report);

      if(!finalise)
	optimisticForwardStatus[LI->invar->I] = report;
      else
	pessimisticForwardStatus[LI->invar->I] = report;

      return ret;

    }

  }

  bool walkVerbose = false;

  // Freed by the walker:
  struct LFPathContext* initialCtx = new LFPathContext();
  // Per-block context records whether we've passed the cache threshold.
  // When we're outside the cache threshold we also switch to pessimistic mode
  // since everything before that point is a fixed certainty.

  const Type* TargetType = LI->getType();

  PartialVal emptyPV;
  NormalLoadForwardWalker Walker(LI, LI->getOperand(0),
				 GlobalAA->getTypeStoreSize(TargetType), TargetType,
				 !finalise,
				 CacheThresholdBB, CacheThresholdIA, initialCtx,
				 emptyPV,
				 !!LPBA,
				 walkVerbose);

  if(LPBA && (!finalise) && Walker.LoadPtrBase.isInval()) {

    // Wait and see if we can better describe this pointer:
    return false;

  }

  if(TargetType->isStructTy() || TargetType->isArrayTy()) {
    bool* validBytes = Walker.getValidBuf();
    markPaddingBytes(validBytes, TargetType, GlobalTD);
  }

  bool verbose = false;

  if(verbose) {

    errs() << "=== START LFA for " << LI->parent->IA->itcache(LI) << "\n";

    IntegrationAttempt* PrintCtx = this;
    while(PrintCtx) {
      errs() << PrintCtx->getShortHeader() << ", ";
      PrintCtx = PrintCtx->parent;
    }
    errs() << "\n";

  }

  struct timespec start;
  clock_gettime(CLOCK_REALTIME, &start);
  
  Walker.walk();

  struct timespec end;
  clock_gettime(CLOCK_REALTIME, &end);

  if(time_diff(start, end) > 0.1) {

    errs() << "Consider " << LI->parent->IA->itcache(LI) << " took " << time_diff(start, end) << "\n";
    errs() << "Cache params: " << LI->parent->IA->itcache(Walker.LoadPtrBase) << ", " << Walker.LoadPtrOffset << ", " << Walker.LoadSize << ", " << (!!Walker.activeCacheEntry) << ", " << (Walker.usedCacheEntryIA ? Walker.usedCacheEntryIA->getShortHeader() : "(none)") << ", " << (Walker.usedCacheEntryIA ? Walker.usedCacheEntryKey.first.first.first->getName() : "(none)") << "\n";

  }

  if(Walker.activeCacheEntry && Walker.usedCacheEntryIA && ((!LPBA) || !LPBA->cachePointIsTemporary)) {

    LPDEBUG("Delete cache entry\n");
    // Our new cache entry subsumes this old one, since we walk the program in topological order.
    Walker.usedCacheEntryIA->deleteLFPBCacheEntry(Walker.usedCacheEntryKey);

  }

  if(!finalise) {

    for(std::vector<ShadowInstruction*>::iterator it = Walker.UsedInstructions.begin(), it2 = Walker.UsedInstructions.end(); it != it2; ++it) {

      if(!LPBA->isConsidered(ShadowValue(*it)))
	continue;

      // Register our dependency on various instructions:
      // This is only useful during loop invariant analysis.
      if(std::find((*it)->indirectUsers.begin(), (*it)->indirectUsers.end(), LI) == (*it)->indirectUsers.end())
	(*it)->indirectUsers.push_back(LI);

    }

  }

  if(verbose)
    errs() << "=== END LFA\n";

  if(!finalise)
    optimisticForwardStatus[LI->invar->I] = describePBWalker(Walker, LI->parent->IA);
  else
    pessimisticForwardStatus[LI->invar->I] = describePBWalker(Walker, LI->parent->IA);
    
  if(Walker.Result.Values.size() == 0 && !Walker.Result.Overdef)
    return false;

  if(Walker.Result.Type == ValSetTypeVarArg)
    contextTaintedByVarargs = true;

  NewPB = Walker.Result;
  return NewPB.isInitialised();

}

static ImprovedVal* getUniqueNonNullIV(PointerBase& PB) {

  ImprovedVal* uniqueVal = 0;
  
  for(uint32_t i = 0, ilim = PB.Values.size(); i != ilim; ++i) {

    if(Value* V = PB.Values[i].V.getVal()) {
      if(isa<ConstantPointerNull>(V))
	continue;
    }
    
    if(uniqueVal)
      return 0;
    else
      uniqueVal = &(PB.Values[i]);

  }

  return uniqueVal;

}

// Potentially dubious: report a must-alias relationship even if either of them may be null.
// The theory is that either a store-through or read-from a null pointer will kill the program,
// so we can safely assume they alias since either they do or the resulting code is not executed.
static bool PBsMustAliasIfStoredAndLoaded(PointerBase& PB1, PointerBase& PB2) {

  ImprovedVal* IV1;
  ImprovedVal* IV2;

  if((!(IV1 = getUniqueNonNullIV(PB1))) || (!(IV2 = getUniqueNonNullIV(PB2))))
    return false;
  
  return (IV1->Offset != LLONG_MAX && IV1->Offset == IV2->Offset && IV1->V == IV2->V);

}

SVAAResult llvm::tryResolvePointerBases(PointerBase& PB1, unsigned V1Size, PointerBase& PB2, unsigned V2Size, bool usePBKnowledge) {

  if(V1Size == V2Size && PBsMustAliasIfStoredAndLoaded(PB1, PB2))
    return SVMustAlias;

  for(unsigned i = 0; i < PB1.Values.size(); ++i) {

    for(unsigned j = 0; j < PB2.Values.size(); ++j) {

      if(!basesAlias(PB1.Values[i].V, PB2.Values[j].V))
	continue;

      if(PB1.Values[i].Offset == LLONG_MAX || PB2.Values[j].Offset == LLONG_MAX)
	return SVPartialAlias;
	   
      if(!((V2Size != AliasAnalysis::UnknownSize && 
	    PB1.Values[i].Offset >= (PB2.Values[j].Offset + V2Size)) || 
	   (V1Size != AliasAnalysis::UnknownSize && 
	    (PB1.Values[i].Offset + V1Size) <= PB2.Values[j].Offset)))
	return SVPartialAlias;

    }

  }
	
  return SVNoAlias;

}

SVAAResult llvm::tryResolvePointerBases(ShadowValue V1Base, int64_t V1Offset, unsigned V1Size, ShadowValue V2, unsigned V2Size, bool usePBKnowledge) {
      
  PointerBase PB1(ValSetTypePB);
  PB1.insert(ImprovedVal(V1Base, V1Offset));
  PointerBase PB2;
  if(!getPointerBase(V2, PB2))
    return SVMayAlias;
      
  if(PB2.Overdef || PB2.Values.size() == 0)
    return SVMayAlias;

  if(PB2.Type != ValSetTypePB)
    return SVMayAlias;

  return tryResolvePointerBases(PB1, V1Size, PB2, V2Size, usePBKnowledge);

}

SVAAResult llvm::tryResolvePointerBases(ShadowValue V1, unsigned V1Size, ShadowValue V2, unsigned V2Size, bool usePBKnowledge) {
      
  PointerBase PB1, PB2;
  if((!getPointerBase(V1, PB1)) || (!getPointerBase(V2, PB2)))
    return SVMayAlias;
      
  if(PB1.Overdef || PB1.Values.size() == 0 || PB2.Overdef || PB2.Values.size() == 0)
    return SVMayAlias;

  if(PB1.Type != ValSetTypePB || PB2.Type != ValSetTypePB)
    return SVMayAlias;

  return tryResolvePointerBases(PB1, V1Size, PB2, V2Size, usePBKnowledge);
       
}

SVAAResult llvm::aliasSVs(ShadowValue V1, unsigned V1Size,
			  ShadowValue V2, unsigned V2Size,
			  bool usePBKnowledge) {
  
  SVAAResult Alias = tryResolvePointerBases(V1, V1Size, V2, V2Size, usePBKnowledge);
  if(Alias != SVMayAlias)
    return Alias;

  switch(GlobalAA->aliasHypothetical(V1, V1Size, V2, V2Size, usePBKnowledge)) {
  case AliasAnalysis::NoAlias: return SVNoAlias;
  case AliasAnalysis::MustAlias: return SVMustAlias;
  case AliasAnalysis::MayAlias: return SVMayAlias;
  default: release_assert(0); return SVMayAlias;
  }

}

bool llvm::basesAlias(ShadowValue V1, ShadowValue V2) {

  if(V1.isVal()) {

    if(!V2.isVal())
      return false;
    else
      return V1.getVal() == V2.getVal();

  }
  else if(V1.isArg()) {

    if(!V2.isArg())
      return false;
    return V1.getArg() == V2.getArg();

  }
  else {

    if(!V2.isInst())
      return false;

    if(V1.getInst()->invar == V2.getInst()->invar) {

      return (V1.getCtx()->ctxContains(V2.getCtx()) || V2.getCtx()->ctxContains(V1.getCtx()));

    }
    else
      return false;

  }
   
}
