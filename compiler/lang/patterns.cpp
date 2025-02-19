#include "seq/seq.h"

using namespace seq;
using namespace llvm;

Pattern::Pattern(types::Type *type) : SrcObject(), type(type), tc(nullptr) {}

void Pattern::setTryCatch(TryCatch *tc) { this->tc = tc; }

TryCatch *Pattern::getTryCatch() { return tc; }

void Pattern::resolveTypes(types::Type *type) {
  if (!types::is(type, this->type))
    throw exc::SeqException("pattern type mismatch: expected " +
                                this->type->getName() + " but got " +
                                type->getName(),
                            getSrcInfo());
}

bool Pattern::isCatchAll() { return false; }

Pattern *Pattern::clone(Generic *ref) { return this; }

Wildcard::Wildcard() : Pattern(types::Any), var(new Var()) {}

void Wildcard::resolveTypes(types::Type *type) { var->setType(type); }

bool Wildcard::isCatchAll() { return true; }

Wildcard *Wildcard::clone(Generic *ref) {
  if (ref->seenClone(this))
    return (Wildcard *)ref->getClone(this);

  auto *x = new Wildcard();
  ref->addClone(this, x);
  delete x->var;
  x->var = var->clone(ref);
  SEQ_RETURN_CLONE(x);
}

Var *Wildcard::getVar() { return var; }

Value *Wildcard::codegen(BaseFunc *base, types::Type *type, Value *val,
                         BasicBlock *&block) {
  LLVMContext &context = block->getContext();
  var->store(base, val, block);
  return ConstantInt::get(IntegerType::getInt1Ty(context), 1);
}

BoundPattern::BoundPattern(Pattern *pattern)
    : Pattern(types::Any), var(new Var()), pattern(pattern) {}

void BoundPattern::resolveTypes(types::Type *type) {
  var->setType(type);
  pattern->resolveTypes(type);
}

bool BoundPattern::isCatchAll() { return pattern->isCatchAll(); }

BoundPattern *BoundPattern::clone(Generic *ref) {
  if (ref->seenClone(this))
    return (BoundPattern *)ref->getClone(this);

  auto *x = new BoundPattern(pattern->clone(ref));
  ref->addClone(this, x);
  delete x->var;
  x->var = var->clone(ref);
  SEQ_RETURN_CLONE(x);
}

Var *BoundPattern::getVar() { return var; }

Value *BoundPattern::codegen(BaseFunc *base, types::Type *type, Value *val,
                             BasicBlock *&block) {
  var->store(base, val, block);
  return pattern->codegen(base, type, val, block);
}

StarPattern::StarPattern() : Pattern(types::Any) {}

void StarPattern::resolveTypes(types::Type *type) {}

Value *StarPattern::codegen(BaseFunc *base, types::Type *type, Value *val,
                            BasicBlock *&block) {
  assert(0);
  return nullptr;
}

IntPattern::IntPattern(seq_int_t val) : Pattern(types::Int), val(val) {}

BoolPattern::BoolPattern(bool val) : Pattern(types::Bool), val(val) {}

StrPattern::StrPattern(std::string val)
    : Pattern(types::Str), val(std::move(val)) {}

Value *IntPattern::codegen(BaseFunc *base, types::Type *type, Value *val,
                           BasicBlock *&block) {
  LLVMContext &context = block->getContext();
  IRBuilder<> builder(block);
  Value *pat = ConstantInt::get(types::Int->getLLVMType(context),
                                (uint64_t)this->val, true);
  return builder.CreateICmpEQ(val, pat);
}

Value *BoolPattern::codegen(BaseFunc *base, types::Type *type, Value *val,
                            BasicBlock *&block) {
  LLVMContext &context = block->getContext();
  IRBuilder<> builder(block);
  Value *pat =
      ConstantInt::get(types::Bool->getLLVMType(context), (uint64_t)this->val);
  return builder.CreateFCmpOEQ(val, pat);
}

Value *StrPattern::codegen(BaseFunc *base, types::Type *type, Value *val,
                           BasicBlock *&block) {
  LLVMContext &context = block->getContext();
  Value *pat = StrExpr(this->val).codegen(base, block);
  Value *b =
      types::Str->callMagic("__eq__", {type}, pat, {val}, block, getTryCatch());
  IRBuilder<> builder(block);
  return builder.CreateTrunc(b, IntegerType::getInt1Ty(context));
}

RecordPattern::RecordPattern(std::vector<Pattern *> patterns)
    : Pattern(types::Any), patterns(std::move(patterns)) {}

void RecordPattern::resolveTypes(types::Type *type) {
  types::RecordType *rec = type->asRec();

  if (!rec)
    throw exc::SeqException("cannot match record pattern with non-record value",
                            getSrcInfo());

  std::vector<types::Type *> types = rec->getTypes();

  if (types.size() != patterns.size())
    throw exc::SeqException("record element count mismatch in pattern",
                            getSrcInfo());

  for (unsigned i = 0; i < types.size(); i++)
    patterns[i]->resolveTypes(types[i]);
}

Value *RecordPattern::codegen(BaseFunc *base, types::Type *type, Value *val,
                              BasicBlock *&block) {
  LLVMContext &context = block->getContext();
  Value *result = ConstantInt::get(IntegerType::getInt1Ty(context), 1);

  for (unsigned i = 0; i < patterns.size(); i++) {
    std::string m = std::to_string(i + 1);
    Value *sub = type->memb(val, m, block);
    Value *subRes = patterns[i]->codegen(base, type->membType(m), sub, block);
    IRBuilder<> builder(block);
    result = builder.CreateAnd(result, subRes);
  }

  return result;
}

bool RecordPattern::isCatchAll() {
  for (auto *pattern : patterns) {
    if (!pattern->isCatchAll())
      return false;
  }
  return true;
}

RecordPattern *RecordPattern::clone(Generic *ref) {
  std::vector<Pattern *> patternsCloned;
  for (auto *elem : patterns)
    patternsCloned.push_back(elem->clone(ref));
  SEQ_RETURN_CLONE(new RecordPattern(patternsCloned));
}

ArrayPattern::ArrayPattern(std::vector<Pattern *> patterns)
    : Pattern(types::Any), patterns(std::move(patterns)) {}

void ArrayPattern::resolveTypes(types::Type *type) {
  if (!type->isGeneric(types::Array))
    throw exc::SeqException("cannot match array pattern with non-array value",
                            getSrcInfo());

  types::Type *baseType = type->getBaseType(0);
  for (auto *pattern : patterns)
    pattern->resolveTypes(baseType);
}

Value *ArrayPattern::codegen(BaseFunc *base, types::Type *type, Value *val,
                             BasicBlock *&block) {
  LLVMContext &context = block->getContext();

  bool hasStar = false;
  unsigned star = 0;

  for (unsigned i = 0; i < patterns.size(); i++) {
    if (dynamic_cast<StarPattern *>(patterns[i])) {
      assert(!hasStar);
      star = i;
      hasStar = true;
    }
  }

  Value *len = type->memb(val, "len", block);
  Value *lenMatch = nullptr;
  BasicBlock *startBlock = block;
  IRBuilder<> builder(block);

  if (hasStar) {
    Value *minLen = ConstantInt::get(seqIntLLVM(context), patterns.size() - 1);
    lenMatch = builder.CreateICmpSGE(len, minLen);
  } else {
    Value *expectedLen = ConstantInt::get(seqIntLLVM(context), patterns.size());
    lenMatch = builder.CreateICmpEQ(len, expectedLen);
  }

  block = BasicBlock::Create(
      context, "",
      block->getParent()); // block for checking array contents
  BranchInst *branch = builder.CreateCondBr(lenMatch, block, block);

  builder.SetInsertPoint(block);
  Value *result = ConstantInt::get(IntegerType::getInt1Ty(context), 1);

  if (hasStar) {
    for (unsigned i = 0; i < star; i++) {
      Value *idx = ConstantInt::get(seqIntLLVM(context), i);
      Value *sub = type->callMagic("__getitem__", {types::Int}, val, {idx},
                                   block, getTryCatch());
      Value *subRes = patterns[i]->codegen(
          base, type->magicOut("__getitem__", {types::Int}), sub, block);
      builder.SetInsertPoint(
          block); // recall that pattern codegen can change the block
      result = builder.CreateAnd(result, subRes);
    }

    for (unsigned i = star + 1; i < patterns.size(); i++) {
      Value *idx = ConstantInt::get(seqIntLLVM(context), i);
      idx = builder.CreateAdd(idx, len);
      idx = builder.CreateSub(
          idx, ConstantInt::get(seqIntLLVM(context), patterns.size()));

      Value *sub = type->callMagic("__getitem__", {types::Int}, val, {idx},
                                   block, getTryCatch());
      Value *subRes = patterns[i]->codegen(
          base, type->magicOut("__getitem__", {types::Int}), sub, block);
      builder.SetInsertPoint(
          block); // recall that pattern codegen can change the block
      result = builder.CreateAnd(result, subRes);
    }
  } else {
    for (unsigned i = 0; i < patterns.size(); i++) {
      Value *idx = ConstantInt::get(seqIntLLVM(context), i);
      Value *sub = type->callMagic("__getitem__", {types::Int}, val, {idx},
                                   block, getTryCatch());
      Value *subRes = patterns[i]->codegen(
          base, type->magicOut("__getitem__", {types::Int}), sub, block);
      builder.SetInsertPoint(
          block); // recall that pattern codegen can change the block
      result = builder.CreateAnd(result, subRes);
    }
  }

  BasicBlock *checkBlock = block;

  block = BasicBlock::Create(context, "",
                             block->getParent()); // final result block
  builder.CreateBr(block);
  branch->setSuccessor(1, block);

  builder.SetInsertPoint(block);
  PHINode *resultFinal = builder.CreatePHI(IntegerType::getInt1Ty(context), 2);
  resultFinal->addIncoming(ConstantInt::get(IntegerType::getInt1Ty(context), 0),
                           startBlock); // length didn't match
  resultFinal->addIncoming(result,
                           checkBlock); // result of checking array elements

  return resultFinal;
}

ArrayPattern *ArrayPattern::clone(Generic *ref) {
  std::vector<Pattern *> patternsCloned;
  for (auto *elem : patterns)
    patternsCloned.push_back(elem->clone(ref));
  SEQ_RETURN_CLONE(new ArrayPattern(patternsCloned));
}

SeqPattern::SeqPattern(std::string pattern)
    : Pattern(types::Any), pattern(std::move(pattern)) {}

void SeqPattern::resolveTypes(types::Type *type) {
  if (!type->is(types::Seq) && !type->asKMer())
    throw exc::SeqException(
        "sequence pattern must be used with sequence or k-mer type",
        getSrcInfo());
}

static Value *codegenSeqMatchForSeq(const std::vector<char> &patterns,
                                    BaseFunc *base, types::Type *type,
                                    Value *val, BasicBlock *&block) {
  unsigned star = 0;
  bool hasStar = false;
  for (unsigned i = 0; i < patterns.size(); i++) {
    if (patterns[i] == '\0') {
      star = i;
      hasStar = true;
    }
  }

  LLVMContext &context = block->getContext();
  Value *ptr = type->memb(val, "ptr", block);
  Value *len = type->memb(val, "len", block);
  Value *lenMatch = nullptr;
  BasicBlock *startBlock = block;
  IRBuilder<> builder(block);

  // check lengths:
  if (hasStar) {
    Value *minLen = ConstantInt::get(seqIntLLVM(context), patterns.size() - 1);
    lenMatch = builder.CreateICmpSGE(len, minLen);
  } else {
    Value *expectedLen = ConstantInt::get(seqIntLLVM(context), patterns.size());
    lenMatch = builder.CreateICmpEQ(len, expectedLen);
  }

  block = BasicBlock::Create(
      context, "",
      block->getParent()); // block for checking array contents
  BranchInst *branch = builder.CreateCondBr(lenMatch, block, block);

  builder.SetInsertPoint(block);
  Value *result = ConstantInt::get(IntegerType::getInt1Ty(context), 1);

  if (hasStar) {
    for (unsigned i = 0; i < star; i++) {
      if (patterns[i] == '_')
        continue;
      Value *idx = ConstantInt::get(seqIntLLVM(context), i);
      Value *sub = builder.CreateLoad(builder.CreateGEP(ptr, idx));
      Value *c = ConstantInt::get(IntegerType::getInt8Ty(context),
                                  (uint64_t)patterns[i]);
      Value *subRes = builder.CreateICmpEQ(sub, c);
      builder.SetInsertPoint(
          block); // recall that pattern codegen can change the block
      result = builder.CreateAnd(result, subRes);
    }

    for (unsigned i = star + 1; i < patterns.size(); i++) {
      if (patterns[i] == '_')
        continue;
      Value *idx = ConstantInt::get(seqIntLLVM(context), i);
      idx = builder.CreateAdd(idx, len);
      idx = builder.CreateSub(
          idx, ConstantInt::get(seqIntLLVM(context), patterns.size()));

      Value *sub = builder.CreateLoad(builder.CreateGEP(ptr, idx));
      Value *c = ConstantInt::get(IntegerType::getInt8Ty(context),
                                  (uint64_t)patterns[i]);
      Value *subRes = builder.CreateICmpEQ(sub, c);
      builder.SetInsertPoint(
          block); // recall that pattern codegen can change the block
      result = builder.CreateAnd(result, subRes);
    }
  } else {
    for (unsigned i = 0; i < patterns.size(); i++) {
      if (patterns[i] == '_')
        continue;
      Value *idx = ConstantInt::get(seqIntLLVM(context), i);
      Value *sub = builder.CreateLoad(builder.CreateGEP(ptr, idx));
      Value *c = ConstantInt::get(IntegerType::getInt8Ty(context),
                                  (uint64_t)patterns[i]);
      Value *subRes = builder.CreateICmpEQ(sub, c);
      builder.SetInsertPoint(
          block); // recall that pattern codegen can change the block
      result = builder.CreateAnd(result, subRes);
    }
  }

  BasicBlock *checkBlock = block;

  block = BasicBlock::Create(context, "",
                             block->getParent()); // final result block
  builder.CreateBr(block);
  branch->setSuccessor(1, block);

  builder.SetInsertPoint(block);
  PHINode *resultFinal = builder.CreatePHI(IntegerType::getInt1Ty(context), 2);
  resultFinal->addIncoming(ConstantInt::get(IntegerType::getInt1Ty(context), 0),
                           startBlock); // length didn't match
  resultFinal->addIncoming(result,
                           checkBlock); // result of checking array elements

  return resultFinal;
}

static Value *codegenSeqMatchForKmer(const std::vector<char> &patterns,
                                     BaseFunc *base, types::KMer *type,
                                     Value *val, BasicBlock *&block) {
  LLVMContext &context = block->getContext();
  Value *fail = ConstantInt::get(IntegerType::getInt1Ty(context), 0);
  Value *succ = ConstantInt::get(IntegerType::getInt1Ty(context), 1);

  if (patterns.empty())
    return fail; // k-mer length must be at least 1

  const unsigned k = type->getK();
  std::vector<unsigned> wildcardsLeft;  // left of star
  std::vector<unsigned> wildcardsRight; // right of star
  std::vector<unsigned> *wildcards = &wildcardsLeft;
  unsigned star = 0;
  bool hasStar = false;
  for (unsigned i = 0; i < patterns.size(); i++) {
    if (patterns[i] == '\0') {
      star = i;
      hasStar = true;
      wildcards = &wildcardsRight;
    } else if (patterns[i] == '_') {
      wildcards->push_back(i);
    } else if (patterns[i] == 'N') {
      return fail; // k-mers can't contain N
    }
  }
  bool hasWildcard = !(wildcardsLeft.empty() && wildcardsRight.empty());

  // check lengths:
  if (hasStar) {
    const unsigned minLen = patterns.size() - 1;
    if (k < minLen)
      return fail;
  } else {
    const unsigned expectedLen = patterns.size();
    if (k != expectedLen)
      return fail;
  }

  Type *llvmType = type->getLLVMType(context);
  if (!hasStar) {
    SeqExpr s(std::string(patterns.begin(), patterns.end()));
    Value *expectedSeq = s.codegen(base, block);
    Value *expectedKmer = type->callMagic("__init__", {types::Seq}, nullptr,
                                          {expectedSeq}, block, nullptr);
    IRBuilder<> builder(block);

    if (!hasWildcard) {
      // just build the k-mer and compare
      return builder.CreateICmpEQ(val, expectedKmer);
    } else {
      // similar to above, but zero out wildcard bases with a mask
      Value *mask = ConstantInt::get(llvmType, 0);
      for (unsigned pos : wildcardsLeft) {
        Value *shift = ConstantInt::get(llvmType, 3);
        shift = builder.CreateShl(shift, 2 * (k - pos - 1));
        mask = builder.CreateOr(mask, shift);
      }
      mask = builder.CreateNot(mask);
      return builder.CreateICmpEQ(builder.CreateAnd(val, mask),
                                  builder.CreateAnd(expectedKmer, mask));
    }
  } else {
    // handle left and right separately
    std::string leftString(patterns.begin(), patterns.begin() + star);
    std::string rightString(patterns.begin() + star + 1, patterns.end());

    types::KMer *typeLeft = nullptr;
    types::KMer *typeRight = nullptr;

    Value *expectedKmerLeft = nullptr;
    Value *expectedKmerRight = nullptr;

    if (!leftString.empty()) {
      SeqExpr sLeft(leftString);
      Value *expectedSeqLeft = sLeft.codegen(base, block);
      typeLeft = types::KMer::get(leftString.size());
      expectedKmerLeft = typeLeft->callMagic("__init__", {types::Seq}, nullptr,
                                             {expectedSeqLeft}, block, nullptr);
    }

    if (!rightString.empty()) {
      SeqExpr sRight(rightString);
      Value *expectedSeqRight = sRight.codegen(base, block);
      typeRight = types::KMer::get(rightString.size());
      expectedKmerRight =
          typeRight->callMagic("__init__", {types::Seq}, nullptr,
                               {expectedSeqRight}, block, nullptr);
    }

    IRBuilder<> builder(block);

    if (expectedKmerLeft) {
      expectedKmerLeft = builder.CreateZExt(expectedKmerLeft, llvmType);
      expectedKmerLeft =
          builder.CreateShl(expectedKmerLeft, 2 * (k - typeLeft->getK()));
    }
    if (expectedKmerRight) {
      expectedKmerRight = builder.CreateZExt(expectedKmerRight, llvmType);
    }

    // left and right masks for comparing correct bases:
    Value *maskLeft = ConstantInt::get(llvmType, 1);
    if (typeLeft) {
      maskLeft = builder.CreateShl(maskLeft, 2 * typeLeft->getK());
      maskLeft = builder.CreateSub(maskLeft, ConstantInt::get(llvmType, 1));
      maskLeft = builder.CreateShl(maskLeft, 2 * (k - typeLeft->getK()));
    }

    Value *maskRight = ConstantInt::get(llvmType, 1);
    if (typeRight) {
      maskRight = builder.CreateShl(maskRight, 2 * typeRight->getK());
      maskRight = builder.CreateSub(maskRight, ConstantInt::get(llvmType, 1));
    }

    if (hasWildcard) {
      // zero out wildcard bases:
      for (unsigned pos : wildcardsLeft) {
        Value *shift = ConstantInt::get(llvmType, 3);
        shift = builder.CreateShl(shift, 2 * (typeLeft->getK() - pos - 1));
        shift = builder.CreateShl(shift, 2 * (k - typeLeft->getK()));
        shift = builder.CreateNot(shift);
        maskLeft = builder.CreateAnd(maskLeft, shift);
      }

      for (unsigned pos : wildcardsRight) {
        // fix pos to be relative to right side:
        pos = (pos - 1) - star;
        Value *shift = ConstantInt::get(llvmType, 3);
        shift = builder.CreateShl(shift, 2 * (typeRight->getK() - pos - 1));
        shift = builder.CreateNot(shift);
        maskRight = builder.CreateAnd(maskRight, shift);
      }
    }

    Value *lhsEq = expectedKmerLeft
                       ? builder.CreateICmpEQ(
                             builder.CreateAnd(val, maskLeft),
                             builder.CreateAnd(expectedKmerLeft, maskLeft))
                       : succ;
    Value *rhsEq = expectedKmerRight
                       ? builder.CreateICmpEQ(
                             builder.CreateAnd(val, maskRight),
                             builder.CreateAnd(expectedKmerRight, maskRight))
                       : succ;
    return builder.CreateAnd(lhsEq, rhsEq);
  }

  assert(0);
  return nullptr;
}

Value *SeqPattern::codegen(BaseFunc *base, types::Type *type, Value *val,
                           BasicBlock *&block) {
  std::vector<char> patterns;
  bool hasStar = false;

  // parse pattern:
  unsigned i = 0;
  while (i < pattern.size()) {
    const char c = pattern[i];
    if (isspace(c))
      continue;

    switch (c) {
    case 'A':
    case 'C':
    case 'G':
    case 'T':
    case 'N':
    case '_':
      patterns.push_back(c);
      break;
    case '.':
      if (hasStar)
        throw exc::SeqException(
            "at most one '...' allowed in sequence pattern");
      if (!(i < pattern.size() - 2 && pattern[i + 1] == '.' &&
            pattern[i + 2] == '.'))
        throw exc::SeqException("invalid sequence pattern: '" + pattern + "'");
      i += 2;
      patterns.push_back('\0');
      hasStar = true;
      break;
    default:
      throw exc::SeqException("invalid character in sequence pattern: '" +
                              std::string(1, c) + "'");
    }
    ++i;
  }

  if (type->is(types::Seq)) {
    return codegenSeqMatchForSeq(patterns, base, type, val, block);
  } else if (types::KMer *kmerType = type->asKMer()) {
    return codegenSeqMatchForKmer(patterns, base, kmerType, val, block);
  } else {
    assert(0);
  }
}

OptPattern::OptPattern(Pattern *pattern)
    : Pattern(types::Any), pattern(pattern) {}

void OptPattern::resolveTypes(types::Type *type) {
  types::OptionalType *optType = type->asOpt();
  if (!optType)
    throw exc::SeqException(
        "cannot match optional pattern against non-optional value",
        getSrcInfo());

  if (pattern)
    pattern->resolveTypes(optType->getBaseType(0));
}

Value *OptPattern::codegen(BaseFunc *base, types::Type *type, Value *val,
                           BasicBlock *&block) {
  LLVMContext &context = block->getContext();

  types::OptionalType *optType = type->asOpt();
  assert(optType);

  if (!pattern) { // no pattern means we're matching the empty optional pattern
    Value *has = optType->has(val, block);
    IRBuilder<> builder(block);
    return builder.CreateNot(has);
  }

  Value *hasResult = optType->has(val, block);
  BasicBlock *startBlock = block;
  IRBuilder<> builder(block);
  block = BasicBlock::Create(context, "",
                             block->getParent()); // pattern eval block
  BranchInst *branch = builder.CreateCondBr(hasResult, block, block);

  Value *had = optType->val(val, block);
  Value *patternResult =
      pattern->codegen(base, optType->getBaseType(0), had, block);
  BasicBlock *checkBlock = block;
  builder.SetInsertPoint(block);

  block = BasicBlock::Create(context, "",
                             block->getParent()); // final result block
  builder.CreateBr(block);
  branch->setSuccessor(1, block);

  builder.SetInsertPoint(block);
  PHINode *resultFinal = builder.CreatePHI(IntegerType::getInt1Ty(context), 2);
  resultFinal->addIncoming(ConstantInt::get(IntegerType::getInt1Ty(context), 0),
                           startBlock); // no value
  resultFinal->addIncoming(patternResult,
                           checkBlock); // result of pattern match

  return resultFinal;
}

OptPattern *OptPattern::clone(Generic *ref) {
  SEQ_RETURN_CLONE(new OptPattern(pattern ? pattern->clone(ref) : nullptr));
}

RangePattern::RangePattern(seq_int_t a, seq_int_t b)
    : Pattern(types::Int), a(a), b(b) {}

Value *RangePattern::codegen(BaseFunc *base, types::Type *type, Value *val,
                             BasicBlock *&block) {
  LLVMContext &context = block->getContext();

  Value *a = ConstantInt::get(seqIntLLVM(context), (uint64_t)this->a, true);
  Value *b = ConstantInt::get(seqIntLLVM(context), (uint64_t)this->b, true);

  IRBuilder<> builder(block);
  Value *c1 = builder.CreateICmpSLE(a, val);
  Value *c2 = builder.CreateICmpSLE(val, b);
  return builder.CreateAnd(c1, c2);
}

OrPattern::OrPattern(std::vector<Pattern *> patterns)
    : Pattern(types::Any), patterns(std::move(patterns)) {}

void OrPattern::resolveTypes(types::Type *type) {
  for (auto *pattern : patterns)
    pattern->resolveTypes(type);
}

Value *OrPattern::codegen(BaseFunc *base, types::Type *type, Value *val,
                          BasicBlock *&block) {
  LLVMContext &context = block->getContext();
  Value *result = ConstantInt::get(IntegerType::getInt1Ty(context), 0);

  for (auto *pattern : patterns) {
    Value *subRes = pattern->codegen(base, type, val, block);
    IRBuilder<> builder(block);
    result = builder.CreateOr(result, subRes);
  }

  return result;
}

bool OrPattern::isCatchAll() {
  for (auto *pattern : patterns) {
    if (pattern->isCatchAll())
      return true;
  }
  return false;
}

OrPattern *OrPattern::clone(Generic *ref) {
  std::vector<Pattern *> patternsCloned;
  for (auto *elem : patterns)
    patternsCloned.push_back(elem->clone(ref));
  SEQ_RETURN_CLONE(new OrPattern(patternsCloned));
}

GuardedPattern::GuardedPattern(Pattern *pattern, Expr *guard)
    : Pattern(types::Int), pattern(pattern), guard(guard) {}

void GuardedPattern::resolveTypes(types::Type *type) {
  pattern->resolveTypes(type);
}

Value *GuardedPattern::codegen(BaseFunc *base, types::Type *type, Value *val,
                               BasicBlock *&block) {
  LLVMContext &context = block->getContext();

  Value *patternResult = pattern->codegen(base, type, val, block);
  BasicBlock *startBlock = block;
  IRBuilder<> builder(block);
  block = BasicBlock::Create(context, "",
                             block->getParent()); // guard eval block
  BranchInst *branch = builder.CreateCondBr(patternResult, block, block);

  Value *guardResult = guard->codegen(base, block);
  guardResult = guard->getType()->boolValue(guardResult, block, getTryCatch());
  BasicBlock *checkBlock = block;
  builder.SetInsertPoint(block);
  guardResult =
      builder.CreateTrunc(guardResult, IntegerType::getInt1Ty(context));

  block = BasicBlock::Create(context, "",
                             block->getParent()); // final result block
  builder.CreateBr(block);
  branch->setSuccessor(1, block);

  builder.SetInsertPoint(block);
  PHINode *resultFinal = builder.CreatePHI(IntegerType::getInt1Ty(context), 2);
  resultFinal->addIncoming(ConstantInt::get(IntegerType::getInt1Ty(context), 0),
                           startBlock);              // pattern didn't match
  resultFinal->addIncoming(guardResult, checkBlock); // result of guard

  return resultFinal;
}

GuardedPattern *GuardedPattern::clone(Generic *ref) {
  SEQ_RETURN_CLONE(new GuardedPattern(pattern->clone(ref), guard->clone(ref)));
}
