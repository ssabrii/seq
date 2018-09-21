#include "seq/seq.h"

using namespace seq;
using namespace llvm;

types::OptionalType::OptionalType(seq::types::Type *baseType) :
    Type(baseType->getName() + "?", BaseType::get()), baseType(baseType)
{
}

/*
 * Reference types are special-cased since their empty value can just be the null pointer.
 */
bool types::OptionalType::isRefOpt() const
{
	return (dynamic_cast<types::RefType *>(baseType) != nullptr);
}

Value *types::OptionalType::defaultValue(BasicBlock *block)
{
	return make(nullptr, block);
}

void types::OptionalType::initFields()
{
	if (isRefOpt() || !vtable.fields.empty())
		return;

	vtable.fields = {
		{"has", {0, Void}},
		{"val", {1, Void}}
	};
}

bool types::OptionalType::isAtomic() const
{
	return baseType->isAtomic();
}

bool types::OptionalType::is(types::Type *type) const
{
	return isGeneric(type) && types::is(getBaseType(0), type->getBaseType(0));
}

unsigned types::OptionalType::numBaseTypes() const
{
	return 1;
}

types::Type *types::OptionalType::getBaseType(unsigned idx) const
{
	return baseType;
}

Type *types::OptionalType::getLLVMType(LLVMContext& context) const
{
	if (isRefOpt())
		return baseType->getLLVMType(context);
	else
		return StructType::get(IntegerType::getInt1Ty(context), baseType->getLLVMType(context));
}

seq_int_t types::OptionalType::size(Module *module) const
{
	return module->getDataLayout().getTypeAllocSize(getLLVMType(module->getContext()));
}

Value *types::OptionalType::make(Value *val, BasicBlock *block)
{
	LLVMContext& context = block->getContext();

	if (isRefOpt())
		return val ? val : ConstantPointerNull::get(cast<PointerType>(getLLVMType(context)));
	else {
		IRBuilder<> builder(block);
		Value *self = UndefValue::get(getLLVMType(context));
		self = setMemb(self, "has", ConstantInt::get(IntegerType::getInt1Ty(context), val ? 1 : 0), block);
		if (val) self = setMemb(self, "val", val, block);
		return self;
	}
}

Value *types::OptionalType::has(Value *self, BasicBlock *block)
{
	if (isRefOpt()) {
		LLVMContext& context = block->getContext();
		IRBuilder<> builder(block);
		return builder.CreateICmpNE(self, ConstantPointerNull::get(cast<PointerType>(getLLVMType(context))));
	} else {
		return memb(self, "has", block);
	}
}

Value *types::OptionalType::val(Value *self, BasicBlock *block)
{
	return isRefOpt() ? self : memb(self, "val", block);
}

types::OptionalType *types::OptionalType::get(types::Type *baseType) noexcept
{
	return new OptionalType(baseType);
}

types::OptionalType *types::OptionalType::get()
{
	return get(types::BaseType::get());
}

types::OptionalType *types::OptionalType::clone(Generic *ref)
{
	return get(baseType->clone(ref));
}
