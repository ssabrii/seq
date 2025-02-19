#include "seq/seq.h"

using namespace seq;
using namespace llvm;

types::RecordType::RecordType(std::vector<Type *> types,
                              std::vector<std::string> names, std::string name)
    : Type(name, BaseType::get(), false, !name.empty()),
      types(std::move(types)), names(std::move(names)) {
  assert(this->names.empty() || this->names.size() == this->types.size());
}

void types::RecordType::setContents(std::vector<Type *> types,
                                    std::vector<std::string> names) {
  this->types = std::move(types);
  this->names = std::move(names);
  assert(this->names.empty() || this->names.size() == this->types.size());
}

bool types::RecordType::empty() const { return types.empty(); }

std::vector<types::Type *> types::RecordType::getTypes() { return types; }

std::string types::RecordType::getName() const {
  if (!name.empty())
    return name;

  std::string name = "tuple[";

  for (unsigned i = 0; i < types.size(); i++) {
    name += types[i]->getName();
    if (i < types.size() - 1)
      name += ",";
  }

  name += "]";
  return name;
}

Value *types::RecordType::defaultValue(BasicBlock *block) {
  LLVMContext &context = block->getContext();
  Value *self = UndefValue::get(getLLVMType(context));

  for (unsigned i = 0; i < types.size(); i++) {
    Value *elem = types[i]->defaultValue(block);
    IRBuilder<> builder(block);
    self = builder.CreateInsertValue(self, elem, i);
  }

  return self;
}

bool types::RecordType::isAtomic() const {
  for (auto *type : types) {
    if (!type->isAtomic())
      return false;
  }
  return true;
}

bool types::RecordType::is(types::Type *type) const {
  unsigned b = numBaseTypes();

  if (!isGeneric(type) || b != type->numBaseTypes())
    return false;

  if (!name.empty()) {
    auto *rec = dynamic_cast<types::RecordType *>(type);
    assert(rec);
    if (name != rec->name)
      return false;
  }

  for (unsigned i = 0; i < b; i++) {
    if (!types::is(getBaseType(i), type->getBaseType(i)))
      return false;
  }

  return true;
}

Function *types::RecordType::getContainsFunc(Module *module) {
  // only support __contains__ for homogeneous tuples:
  assert(types.size() > 0);
  for (auto *type : types) {
    assert(types::is(type, types[0]));
  }

  const std::string name = "seq." + getName() + ".__contains__";
  LLVMContext &context = module->getContext();
  Function *func = module->getFunction(name);

  if (!func) {
    func = cast<Function>(module->getOrInsertFunction(
        name, types::Bool->getLLVMType(context), getLLVMType(context),
        types[0]->getLLVMType(context)));
    func->setLinkage(GlobalValue::PrivateLinkage);
    func->addFnAttr(Attribute::AlwaysInline);

    auto iter = func->arg_begin();
    Value *rec = iter++;
    Value *val = iter;

    BasicBlock *block = BasicBlock::Create(context, "entry", func);
    BasicBlock *retFalse = BasicBlock::Create(context, "exit_false", func);
    BasicBlock *retTrue = BasicBlock::Create(context, "exit_true", func);
    IRBuilder<> builder(block);

    for (unsigned i = 0; i < types.size(); i++) {
      Value *m = memb(rec, std::to_string(i + 1), block);
      types::Type *eqType = types[i]->magicOut("__eq__", {types[i]});
      Value *eq =
          types[i]->callMagic("__eq__", {types[i]}, m, {val}, block, nullptr);
      eq = eqType->boolValue(eq, block, nullptr);
      builder.SetInsertPoint(block);
      eq = builder.CreateICmpNE(eq, builder.getInt8(0));

      BasicBlock *next = BasicBlock::Create(context, "cmp", func);
      builder.CreateCondBr(eq, retTrue, next);
      block = next;
    }

    builder.SetInsertPoint(block);
    builder.CreateBr(retFalse);

    builder.SetInsertPoint(retFalse);
    builder.CreateRet(builder.getInt8(0));

    builder.SetInsertPoint(retTrue);
    builder.CreateRet(builder.getInt8(1));
  }

  return func;
}

enum { EQ, NE, LT, LE, GT, GE };
Function *types::RecordType::getCmpFunc(Module *module, int kind) {
  std::string magic;
  switch (kind) {
  case EQ:
    magic = "__eq__";
    break;
  case NE:
    magic = "__ne__";
    break;
  case LT:
    magic = "__lt__";
    break;
  case LE:
    magic = "__le__";
    break;
  case GT:
    magic = "__gt__";
    break;
  case GE:
    magic = "__ge__";
    break;
  default:
    assert(0);
  }

  const std::string name = "seq." + getName() + "." + magic;
  LLVMContext &context = module->getContext();
  Function *func = module->getFunction(name);

  if (!func) {
    func = cast<Function>(module->getOrInsertFunction(
        name, types::Bool->getLLVMType(context), getLLVMType(context),
        getLLVMType(context)));
    func->setLinkage(GlobalValue::PrivateLinkage);
    func->addFnAttr(Attribute::AlwaysInline);

    auto iter = func->arg_begin();
    Value *r1 = iter++;
    Value *r2 = iter;

    BasicBlock *block = BasicBlock::Create(context, "entry", func);
    BasicBlock *retFalse = BasicBlock::Create(context, "exit_false", func);
    BasicBlock *retTrue = BasicBlock::Create(context, "exit_true", func);
    IRBuilder<> builder(block);

    for (unsigned i = 0; i < types.size(); i++) {
      Value *val1 = memb(r1, std::to_string(i + 1), block);
      Value *val2 = memb(r2, std::to_string(i + 1), block);

      types::Type *cmpType = types[i]->magicOut(magic, {types[i]});
      Value *cmp =
          types[i]->callMagic(magic, {types[i]}, val1, {val2}, block, nullptr);
      cmp = cmpType->boolValue(cmp, block, nullptr);
      builder.SetInsertPoint(block);
      cmp = builder.CreateICmpNE(cmp, builder.getInt8(0));
      BasicBlock *next = BasicBlock::Create(context, "cmp", func);

      if (kind == EQ) {
        builder.CreateCondBr(cmp, next, retFalse);
      } else if (kind == NE) {
        builder.CreateCondBr(cmp, retTrue, next);
      } else {
        BasicBlock *eqBlock = BasicBlock::Create(context, "eq", func);
        types::Type *eqType = types[i]->magicOut("__eq__", {types[i]});
        Value *eq = types[i]->callMagic("__eq__", {types[i]}, val1, {val2},
                                        eqBlock, nullptr);
        eq = eqType->boolValue(eq, eqBlock, nullptr);
        builder.SetInsertPoint(eqBlock);
        eq = builder.CreateICmpNE(eq, builder.getInt8(0));
        builder.CreateCondBr(eq, next,
                             (kind == LT || kind == GT) ? retFalse : retTrue);

        builder.SetInsertPoint(block);
        if (kind == LT || kind == GT) {
          builder.CreateCondBr(cmp, retTrue, eqBlock);
        } else {
          builder.CreateCondBr(cmp, eqBlock, retFalse);
        }
      }

      block = next;
    }

    builder.SetInsertPoint(block);
    builder.CreateBr((kind == EQ || kind == LE || kind == GE) ? retTrue
                                                              : retFalse);

    builder.SetInsertPoint(retFalse);
    builder.CreateRet(builder.getInt8(0));

    builder.SetInsertPoint(retTrue);
    builder.CreateRet(builder.getInt8(1));
  }

  return func;
}

void types::RecordType::initOps() {
  if (!vtable.magic.empty())
    return;

  static RecordType *pyObjType =
      RecordType::get({PtrType::get(Byte)}, {"p"}, "PyObject");

  vtable.magic = {
      {"__init__", types, this,
       [this](Value *self, std::vector<Value *> args, IRBuilder<> &b) {
         Value *val = defaultValue(b.GetInsertBlock());
         for (unsigned i = 0; i < args.size(); i++)
           val =
               setMemb(val, std::to_string(i + 1), args[i], b.GetInsertBlock());
         return val;
       },
       false},

      {"__str__",
       {},
       Str,
       [this](Value *self, std::vector<Value *> args, IRBuilder<> &b) {
         LLVMContext &context = b.getContext();
         BasicBlock *block = b.GetInsertBlock();
         Module *module = block->getModule();
         const std::string strName = "seq." + getName() + ".__str__";
         Function *str = module->getFunction(strName);

         if (!str) {
           str = cast<Function>(module->getOrInsertFunction(
               strName, Str->getLLVMType(context), getLLVMType(context)));
           str->setLinkage(GlobalValue::PrivateLinkage);
           str->setPersonalityFn(makePersonalityFunc(module));

           Value *arg = str->arg_begin();
           BasicBlock *entry = BasicBlock::Create(context, "entry", str);
           b.SetInsertPoint(entry);
           Value *len = ConstantInt::get(seqIntLLVM(context), types.size());
           Value *strs = b.CreateAlloca(Str->getLLVMType(context), len);

           for (unsigned i = 0; i < types.size(); i++) {
             Value *v = memb(arg, std::to_string(i + 1), entry);
             // won't create new block since no try-catch:
             Value *s = types[i]->strValue(v, entry, nullptr);
             Value *dest = b.CreateGEP(strs, b.getInt32(i));
             b.CreateStore(s, dest);
           }

           auto *strReal = cast<Function>(module->getOrInsertFunction(
               "seq_str_tuple", Str->getLLVMType(context),
               Str->getLLVMType(context)->getPointerTo(), seqIntLLVM(context)));
           strReal->setDoesNotThrow();
           Value *res = b.CreateCall(strReal, {strs, len});
           b.CreateRet(res);
         }

         b.SetInsertPoint(block);
         return b.CreateCall(str, self);
       },
       false},

      {"__iter__",
       {},
       GenType::get(types.empty() ? Void : types[0]),
       [this](Value *self, std::vector<Value *> args, IRBuilder<> &b) {
         if (types.empty())
           throw exc::SeqException("cannot iterate over empty tuple");

         for (auto *type : types) {
           if (!types::is(type, types[0]))
             throw exc::SeqException("cannot iterate over heterogeneous tuple");
         }

         BasicBlock *block = b.GetInsertBlock();
         Module *module = block->getModule();
         const std::string iterName = "seq." + getName() + ".__iter__";
         Function *iter = module->getFunction(iterName);

         if (!iter) {
           Func iterFunc;
           iterFunc.setName(iterName);
           iterFunc.setIns({this});
           iterFunc.setOut(types[0]);
           iterFunc.setArgNames({"self"});

           VarExpr arg(iterFunc.getArgVar("self"));
           Block *body = iterFunc.getBlock();
           for (unsigned i = 0; i < types.size(); i++) {
             auto *yield = new Yield(new GetElemExpr(&arg, i + 1));
             yield->setBase(&iterFunc);
             body->add(yield);
             if (i == 0)
               iterFunc.sawYield(yield);
           }

           iterFunc.resolveTypes();
           iterFunc.codegen(module);
           iter = iterFunc.getFunc();
         }

         return b.CreateCall(iter, self);
       },
       false},

      {"__len__",
       {},
       Int,
       [this](Value *self, std::vector<Value *> args, IRBuilder<> &b) {
         return ConstantInt::get(seqIntLLVM(b.getContext()), types.size(),
                                 true);
       },
       false},

      {"__eq__",
       {this},
       Bool,
       [this](Value *self, std::vector<Value *> args, IRBuilder<> &b) {
         Function *cmp = getCmpFunc(b.GetInsertBlock()->getModule(), EQ);
         return b.CreateCall(cmp, {self, args[0]});
       },
       false},

      {"__ne__",
       {this},
       Bool,
       [this](Value *self, std::vector<Value *> args, IRBuilder<> &b) {
         Function *cmp = getCmpFunc(b.GetInsertBlock()->getModule(), NE);
         return b.CreateCall(cmp, {self, args[0]});
       },
       false},

      {"__lt__",
       {this},
       Bool,
       [this](Value *self, std::vector<Value *> args, IRBuilder<> &b) {
         Function *cmp = getCmpFunc(b.GetInsertBlock()->getModule(), LT);
         return b.CreateCall(cmp, {self, args[0]});
       },
       false},

      {"__gt__",
       {this},
       Bool,
       [this](Value *self, std::vector<Value *> args, IRBuilder<> &b) {
         Function *cmp = getCmpFunc(b.GetInsertBlock()->getModule(), GT);
         return b.CreateCall(cmp, {self, args[0]});
       },
       false},

      {"__le__",
       {this},
       Bool,
       [this](Value *self, std::vector<Value *> args, IRBuilder<> &b) {
         Function *cmp = getCmpFunc(b.GetInsertBlock()->getModule(), LE);
         return b.CreateCall(cmp, {self, args[0]});
       },
       false},

      {"__ge__",
       {this},
       Bool,
       [this](Value *self, std::vector<Value *> args, IRBuilder<> &b) {
         Function *cmp = getCmpFunc(b.GetInsertBlock()->getModule(), GE);
         return b.CreateCall(cmp, {self, args[0]});
       },
       false},

      {"__hash__",
       {},
       Int,
       [this](Value *self, std::vector<Value *> args, IRBuilder<> &b) {
         // hash_combine combine algorithm used in boost
         LLVMContext &context = b.getContext();
         BasicBlock *block = b.GetInsertBlock();
         Value *seed = zeroLLVM(context);
         Value *phi = ConstantInt::get(seqIntLLVM(context), 0x9e3779b9);
         for (unsigned i = 0; i < types.size(); i++) {
           Value *val = memb(self, std::to_string(i + 1), block);
           if (!types[i]->magicOut("__hash__", {})->is(Int))
             throw exc::SeqException("__hash__ for type '" +
                                     types[i]->getName() +
                                     "' does return an 'int'");
           Value *hash =
               types[i]->callMagic("__hash__", {}, val, {}, block, nullptr);
           Value *p1 = b.CreateShl(seed, 6);
           Value *p2 = b.CreateLShr(seed, 2);
           hash = b.CreateAdd(hash, phi);
           hash = b.CreateAdd(hash, p1);
           hash = b.CreateAdd(hash, p2);
           seed = b.CreateXor(seed, hash);
         }
         return seed;
       },
       false},

      {"__contains__",
       {types.empty() ? Base : types[0]},
       Bool,
       [this](Value *self, std::vector<Value *> args, IRBuilder<> &b) {
         if (types.empty())
           throw exc::SeqException("cannot use 'in' on empty tuple");

         for (auto *type : types) {
           if (!types::is(type, types[0]))
             throw exc::SeqException("cannot use 'in' on heterogeneous tuple");
         }

         Function *contains = getContainsFunc(b.GetInsertBlock()->getModule());
         return b.CreateCall(contains, {self, args[0]});
       },
       false},

      {"__to_py__",
       {},
       pyObjType,
       [this](Value *self, std::vector<Value *> args, IRBuilder<> &b) {
         LLVMContext &context = b.getContext();
         BasicBlock *block = b.GetInsertBlock();
         Module *module = block->getModule();

         auto *pyTupNew = cast<Function>(module->getOrInsertFunction(
             "seq_py_tuple_new", PtrType::get(Byte)->getLLVMType(context),
             seqIntLLVM(context)));

         auto *pyTupSet = cast<Function>(module->getOrInsertFunction(
             "seq_py_tuple_setitem", llvm::Type::getVoidTy(context),
             PtrType::get(Byte)->getLLVMType(context), seqIntLLVM(context),
             PtrType::get(Byte)->getLLVMType(context)));

         pyTupNew->setDoesNotThrow();
         pyTupSet->setDoesNotThrow();

         Value *pyTup = b.CreateCall(
             pyTupNew, ConstantInt::get(seqIntLLVM(context), types.size()));
         for (unsigned i = 0; i < types.size(); i++) {
           Value *val = memb(self, std::to_string(i + 1), block);
           if (!types[i]->magicOut("__to_py__", {})->is(pyObjType))
             throw exc::SeqException("__to_py__ for type '" +
                                     types[i]->getName() +
                                     "' does return a 'PyObject'");
           Value *pyVal =
               types[i]->callMagic("__to_py__", {}, val, {}, block, nullptr);
           Value *ptr = pyObjType->memb(pyVal, "p", block);
           b.CreateCall(pyTupSet,
                        {pyTup, ConstantInt::get(seqIntLLVM(context), i), ptr});
         }

         Value *result = pyObjType->defaultValue(block);
         result = pyObjType->setMemb(result, "p", pyTup, block);
         return result;
       },
       false},

      {"__from_py__",
       {pyObjType},
       this,
       [this](Value *self, std::vector<Value *> args, IRBuilder<> &b) {
         LLVMContext &context = b.getContext();
         BasicBlock *block = b.GetInsertBlock();
         Module *module = block->getModule();

         auto *pyTupGet = cast<Function>(module->getOrInsertFunction(
             "seq_py_tuple_getitem", PtrType::get(Byte)->getLLVMType(context),
             PtrType::get(Byte)->getLLVMType(context), seqIntLLVM(context)));
         pyTupGet->setDoesNotThrow();

         Value *pyTup = pyObjType->memb(args[0], "p", block);
         Value *result = defaultValue(block);
         for (unsigned i = 0; i < types.size(); i++) {
           // last arg type being null means static magic:
           if (!types::is(
                   types[i]->magicOut("__from_py__", {pyObjType, nullptr}),
                   types[i]))
             throw exc::SeqException("__from_py__ for type '" +
                                     types[i]->getName() +
                                     "' returns a different type");
           Value *valPtr = b.CreateCall(
               pyTupGet, {pyTup, ConstantInt::get(seqIntLLVM(context), i)});
           Value *val = pyObjType->defaultValue(block);
           val = pyObjType->setMemb(val, "p", valPtr, block);
           val = types[i]->callMagic("__from_py__", {pyObjType}, nullptr, {val},
                                     block, nullptr);
           result = setMemb(result, std::to_string(i + 1), val, block);
         }
         return result;
       },
       true},

      {"__pickle__",
       {PtrType::get(Byte)},
       Void,
       [this](Value *self, std::vector<Value *> args, IRBuilder<> &b) {
         BasicBlock *block = b.GetInsertBlock();
         for (unsigned i = 0; i < types.size(); i++) {
           Value *val = memb(self, std::to_string(i + 1), block);
           types[i]->callMagic("__pickle__", {PtrType::get(Byte)}, val,
                               {args[0]}, block, nullptr);
         }
         return (Value *)nullptr;
       },
       false},

      {"__unpickle__",
       {PtrType::get(Byte)},
       this,
       [this](Value *self, std::vector<Value *> args, IRBuilder<> &b) {
         BasicBlock *block = b.GetInsertBlock();
         Value *result = defaultValue(block);
         for (unsigned i = 0; i < types.size(); i++) {
           Value *val =
               types[i]->callMagic("__unpickle__", {PtrType::get(Byte)},
                                   nullptr, {args[0]}, block, nullptr);
           result = setMemb(result, std::to_string(i + 1), val, block);
         }
         return result;
       },
       true},
  };
}

void types::RecordType::initFields() {
  if (!getVTable().fields.empty())
    return;

  assert(names.empty() || names.size() == types.size());

  for (unsigned i = 0; i < types.size(); i++) {
    getVTable().fields.insert({std::to_string(i + 1), {i, types[i]}});

    if (!names.empty() && !names[i].empty())
      getVTable().fields.insert({names[i], {i, types[i]}});
  }
}

unsigned types::RecordType::numBaseTypes() const {
  return (unsigned)types.size();
}

types::Type *types::RecordType::getBaseType(unsigned idx) const {
  return types[idx];
}

Type *types::RecordType::getLLVMType(LLVMContext &context) const {
  std::vector<llvm::Type *> body;
  for (auto &type : types)
    body.push_back(type->getLLVMType(context));

  return StructType::get(context, body);
}

void types::RecordType::addLLVMTypesToStruct(StructType *structType) {
  std::vector<llvm::Type *> body;
  for (auto &type : types)
    body.push_back(type->getLLVMType(structType->getContext()));
  structType->setBody(body);
}

size_t types::RecordType::size(Module *module) const {
  return module->getDataLayout().getTypeAllocSize(
      getLLVMType(module->getContext()));
}

types::RecordType *types::RecordType::asRec() { return this; }

types::RecordType *types::RecordType::clone(Generic *ref) {
  if (ref->seenClone(this))
    return (types::RecordType *)ref->getClone(this);

  auto *x = types::RecordType::get({}, {}, name);
  ref->addClone(this, x);

  std::vector<Type *> typesCloned;
  for (auto *type : types)
    typesCloned.push_back(type->clone(ref));

  std::vector<MagicOverload> overloadsCloned;
  for (auto &magic : getVTable().overloads)
    overloadsCloned.push_back({magic.name, magic.func->clone(ref)});

  std::map<std::string, BaseFunc *> methodsCloned;
  for (auto &method : getVTable().methods)
    methodsCloned.insert({method.first, method.second->clone(ref)});

  x->types = typesCloned;
  x->names = names;
  x->getVTable().overloads = overloadsCloned;
  x->getVTable().methods = methodsCloned;
  return x;
}

types::RecordType *types::RecordType::get(std::vector<Type *> types,
                                          std::vector<std::string> names,
                                          std::string name) {
  return new RecordType(std::move(types), std::move(names), std::move(name));
}
