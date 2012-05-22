//===--- Type.cpp - Swift Language Type ASTs ------------------------------===//
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
//  This file implements the Type class and subclasses.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Types.h"
#include "swift/AST/Decl.h"
#include "swift/AST/AST.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/Support/raw_ostream.h"
using namespace swift;

// Only allow allocation of Stmts using the allocator in ASTContext.
void *TypeBase::operator new(size_t Bytes, ASTContext &C,
                         unsigned Alignment) {
  return C.Allocate(Bytes, Alignment);
}

bool CanType::isActuallyCanonicalOrNull() const {
  return getPointer() == 0 || 
         getPointer() == llvm::DenseMapInfo<TypeBase*>::getTombstoneKey() ||
         getPointer()->isCanonical();
}



//===----------------------------------------------------------------------===//
// Various Type Methods.
//===----------------------------------------------------------------------===//

/// isEqual - Return true if these two types are equal, ignoring sugar.
bool TypeBase::isEqual(Type Other) {
  return getCanonicalType() == Other.getPointer()->getCanonicalType();
}

/// isMaterializable - Is this type 'materializable' according to the
/// rules of the language?  Basically, does it not contain any l-value
/// types?
bool TypeBase::isMaterializable() {
  // Tuples are materializable if all their elements are.
  if (TupleType *tuple = getAs<TupleType>()) {
    for (auto &field : tuple->getFields())
      if (!field.getType()->isMaterializable())
        return false;
    return true;
  }

  // Some l-values may be materializable someday.
  if (LValueType *lvalue = getAs<LValueType>())
    return lvalue->isMaterializable();

  // Everything else is materializable.
  return true;
}

/// hasReferenceSemantics - Does this type have reference semantics?
bool TypeBase::hasReferenceSemantics() {
  // At the moment, Builtin.ObjectPointer, class types, and function types.
  return is<BuiltinObjectPointerType>() || is<ClassType>() || is<FunctionType>();
}

Type TypeBase::getUnlabeledType(ASTContext &Context) {
  switch (getKind()) {
  case TypeKind::Error: 
  case TypeKind::BuiltinRawPointer:
  case TypeKind::BuiltinObjectPointer:
  case TypeKind::BuiltinObjCPointer:
  case TypeKind::BuiltinInteger:
  case TypeKind::BuiltinFloat:
  case TypeKind::UnstructuredDependent:
  case TypeKind::OneOf:
  case TypeKind::Struct:
  case TypeKind::Class:
  case TypeKind::MetaType:
  case TypeKind::Module:
  case TypeKind::Protocol:
  case TypeKind::Archetype:
    return this;

  case TypeKind::NameAlias:
    if (TypeAliasDecl *D = cast<NameAliasType>(this)->getDecl())
      if (D->hasUnderlyingType())
        return D->getUnderlyingType()->getUnlabeledType(Context);
    
    return this;
      
  case TypeKind::Identifier: {
    IdentifierType *IdentTy = cast<IdentifierType>(this);
    if (IdentTy->isMapped())
      return IdentTy->getMappedType()->getUnlabeledType(Context);
    return this;
  }
  
  case TypeKind::Paren: {
    ParenType *ParenTy = cast<ParenType>(this);
    Type UnderlyingTy = ParenTy->getUnderlyingType()->getUnlabeledType(Context);
    if (UnderlyingTy.getPointer() != ParenTy->getUnderlyingType().getPointer())
      return ParenType::get(Context, UnderlyingTy);
    return this;
  }
      
  case TypeKind::Tuple: {
    TupleType *TupleTy = cast<TupleType>(this);
    llvm::SmallVector<TupleTypeElt, 4> Elements;
    bool Rebuild = false;
    unsigned Idx = 0;
    for (const TupleTypeElt &Elt : TupleTy->getFields()) {
      Type EltTy = Elt.getType()->getUnlabeledType(Context);
      if (EltTy.getPointer() != Elt.getType().getPointer() ||
          Elt.getInit() || !Elt.getName().empty() || Rebuild) {
        if (!Rebuild) {
          Elements.reserve(TupleTy->getFields().size());
          for (unsigned I = 0; I != Idx; ++I) {
            const TupleTypeElt &Elt = TupleTy->getFields()[I];
            Elements.push_back(TupleTypeElt(Elt.getType(), Identifier(),
                                            nullptr, Elt.getVarargBaseTy()));
          }
          Rebuild = true;
        }

        Type VarargBaseType;
        if (Elt.isVararg())
          VarargBaseType = Elt.getVarargBaseTy()->getUnlabeledType(Context);
        Elements.push_back(TupleTypeElt(EltTy, Identifier(),
                                        nullptr, VarargBaseType));
      }
      ++Idx;
    }
    
    if (!Rebuild)
      return this;
    
    // An unlabeled 1-element tuple type is represented as a parenthesized
    // type.
    if (Elements.size() == 1 && !Elements[0].isVararg())
      return ParenType::get(Context, Elements[0].getType());
    
    return TupleType::get(Elements, Context);
  }
      
  case TypeKind::Function: {
    FunctionType *FunctionTy = cast<FunctionType>(this);
    Type InputTy = FunctionTy->getInput()->getUnlabeledType(Context);
    Type ResultTy = FunctionTy->getResult()->getUnlabeledType(Context);
    if (InputTy.getPointer() != FunctionTy->getInput().getPointer() ||
        ResultTy.getPointer() != FunctionTy->getResult().getPointer())
      return FunctionType::get(InputTy, ResultTy, FunctionTy->isAutoClosure(),
                               Context);
    
    return this;
  }
      
  case TypeKind::Array: {
    ArrayType *ArrayTy = cast<ArrayType>(this);
    Type BaseTy = ArrayTy->getBaseType()->getUnlabeledType(Context);
    if (BaseTy.getPointer() != ArrayTy->getBaseType().getPointer())
      return ArrayType::get(BaseTy, ArrayTy->getSize(), Context);
    
    return this;
  }

  case TypeKind::ArraySlice: {
    ArraySliceType *sliceTy = cast<ArraySliceType>(this);
    Type baseTy = sliceTy->getBaseType()->getUnlabeledType(Context);
    if (baseTy.getPointer() != sliceTy->getBaseType().getPointer()) {
      ArraySliceType *newSliceTy 
        = ArraySliceType::get(baseTy, sliceTy->getFirstRefLoc(), Context);
      if (!newSliceTy->hasImplementationType())
        newSliceTy->setImplementationType(sliceTy->getImplementationType());
      return newSliceTy;
    }
    
    return this;
  }
      
  case TypeKind::LValue: {
    LValueType *LValueTy = cast<LValueType>(this);
    Type ObjectTy = LValueTy->getObjectType()->getUnlabeledType(Context);
    if (ObjectTy.getPointer() != LValueTy->getObjectType().getPointer())
      return LValueType::get(ObjectTy, LValueTy->getQualifiers(), Context);
    return this;
  }
  }
}

/// getCanonicalType - Return the canonical version of this type, which has
/// sugar from all levels stripped off.
CanType TypeBase::getCanonicalType() {
  assert(this != 0 &&
         "Cannot call getCanonicalType before name binding is complete");

  // If the type is itself canonical, return it.
  if (CanonicalType.is<ASTContext*>())
    return CanType(this);
  // If the canonical type was already computed, just return what we have.
  if (TypeBase *CT = CanonicalType.get<TypeBase*>())
    return CanType(CT);
  
  // Otherwise, compute and cache it.
  TypeBase *Result = 0;
  switch (getKind()) {
#define ALWAYS_CANONICAL_TYPE(id, parent) case TypeKind::id:
#define UNCHECKED_TYPE(id, parent) case TypeKind::id:
#define TYPE(id, parent)
#include "swift/AST/TypeNodes.def"
    llvm_unreachable("these types are always canonical");

#define SUGARED_TYPE(id, parent) \
  case TypeKind::id: \
    Result = cast<id##Type>(this)-> \
             getDesugaredType()->getCanonicalType().getPointer(); \
    break;
#define TYPE(id, parent)
#include "swift/AST/TypeNodes.def"

  case TypeKind::Tuple: {
    TupleType *TT = cast<TupleType>(this);
    assert(!TT->getFields().empty() && "Empty tuples are always canonical");

    SmallVector<TupleTypeElt, 8> CanElts;
    CanElts.reserve(TT->getFields().size());
    for (const TupleTypeElt &field : TT->getFields()) {
      assert(!field.getType().isNull() &&
             "Cannot get canonical type of un-typechecked TupleType!");
      Type canVarargBaseTy;
      if (field.isVararg())
        canVarargBaseTy = field.getVarargBaseTy()->getCanonicalType();
      CanElts.push_back(TupleTypeElt(field.getType()->getCanonicalType(),
                                     field.getName(),
                                     field.getInit(),
                                     canVarargBaseTy));
    }
    
    Result = TupleType::get(CanElts, CanElts[0].getType()->getASTContext());
    break;
  }
    
  case TypeKind::LValue: {
    LValueType *lvalue = cast<LValueType>(this);
    Type objectType = lvalue->getObjectType();
    objectType = objectType->getCanonicalType();
    Result = LValueType::get(objectType, lvalue->getQualifiers(),
                             objectType->getASTContext());
    break;
  }
  case TypeKind::Function: {
    FunctionType *FT = cast<FunctionType>(this);
    Type In = FT->getInput()->getCanonicalType();
    Type Out = FT->getResult()->getCanonicalType();
    Result = FunctionType::get(In, Out, FT->isAutoClosure(),
                               In->getASTContext());
    break;
  }
  case TypeKind::Array:
    ArrayType *AT = cast<ArrayType>(this);
    Type EltTy = AT->getBaseType()->getCanonicalType();
    Result = ArrayType::get(EltTy, AT->getSize(), EltTy->getASTContext());
    break;
  }
    
  
  // Cache the canonical type for future queries.
  assert(Result && "Case not implemented!");
  CanonicalType = Result;
  return CanType(Result);
}


TypeBase *TypeBase::getDesugaredType() {
  switch (getKind()) {
#define ALWAYS_CANONICAL_TYPE(id, parent) case TypeKind::id:
#define UNCHECKED_TYPE(id, parent) case TypeKind::id:
#define TYPE(id, parent)
#include "swift/AST/TypeNodes.def"
  case TypeKind::Tuple:
  case TypeKind::Function:
  case TypeKind::Array:
  case TypeKind::LValue:
    // None of these types have sugar at the outer level.
    return this;
  case TypeKind::Paren:
    return cast<ParenType>(this)->getDesugaredType();
  case TypeKind::Identifier:
    return cast<IdentifierType>(this)->getDesugaredType();
  case TypeKind::NameAlias:
    return cast<NameAliasType>(this)->getDesugaredType();
  case TypeKind::ArraySlice:
    return cast<ArraySliceType>(this)->getDesugaredType();
  }

  llvm_unreachable("Unknown type kind");
}

TypeBase *ParenType::getDesugaredType() {
  return getUnderlyingType()->getDesugaredType();
}

TypeBase *NameAliasType::getDesugaredType() {
  return getDecl()->getUnderlyingType()->getDesugaredType();
}

TypeBase *IdentifierType::getDesugaredType() {
  return getMappedType()->getDesugaredType();
}

TypeBase *ArraySliceType::getDesugaredType() {
  return getImplementationType()->getDesugaredType();
}

const llvm::fltSemantics &BuiltinFloatType::getAPFloatSemantics() const {
  switch (getFPKind()) {
  case BuiltinFloatType::IEEE16:  return APFloat::IEEEhalf;
  case BuiltinFloatType::IEEE32:  return APFloat::IEEEsingle;
  case BuiltinFloatType::IEEE64:  return APFloat::IEEEdouble;
  case BuiltinFloatType::IEEE80:  return APFloat::x87DoubleExtended;
  case BuiltinFloatType::IEEE128: return APFloat::IEEEquad;
  case BuiltinFloatType::PPC128:  return APFloat::PPCDoubleDouble;
  }
  assert(0 && "Unknown FP semantics");
  return APFloat::IEEEhalf;
}

bool IdentifierType::isMapped() const {
  return !Components.back().Value.isNull();
}

Type IdentifierType::getMappedType() {
  assert(!Components.back().Value.isNull() &&
         "Name binding haven't resolved this to a type yet");
  return Components.back().Value.get<Type>();
}

TupleType::TupleType(ArrayRef<TupleTypeElt> fields, ASTContext *CanCtx)
  : TypeBase(TypeKind::Tuple, CanCtx, /*Dependent=*/false), Fields(fields) 
{
  // Determine whether this tuple type is dependent.
  for (const auto &F : Fields) {
    if (!F.getType().isNull() && F.getType()->isDependentType()) {
      setDependent();
      break;
    }
  }
}

/// hasAnyDefaultValues - Return true if any of our elements has a default
/// value.
bool TupleType::hasAnyDefaultValues() const {
  for (const TupleTypeElt &Elt : Fields)
    if (Elt.hasInit())
      return true;
  return false;
}

/// getNamedElementId - If this tuple has a field with the specified name,
/// return the field index, otherwise return -1.
int TupleType::getNamedElementId(Identifier I) const {
  for (unsigned i = 0, e = Fields.size(); i != e; ++i) {
    if (Fields[i].getName() == I)
      return i;
  }

  // Otherwise, name not found.
  return -1;
}

/// getFieldForScalarInit - If a tuple of this type can be initialized with a
/// scalar, return the field number that the scalar is assigned to.  If not,
/// return -1.
int TupleType::getFieldForScalarInit() const {
  if (Fields.empty()) return -1;
  
  int FieldWithoutDefault = -1;
  for (unsigned i = 0, e = Fields.size(); i != e; ++i) {
    // Ignore fields with a default value.
    if (Fields[i].hasInit()) continue;
    
    // If we already saw a field missing a default value, then we cannot assign
    // a scalar to this tuple.
    if (FieldWithoutDefault != -1 && !Fields[i].isVararg())
      return -1;
    
    // Otherwise, remember this field number.
    FieldWithoutDefault = i;    
  }
  
  // If all the elements have default values, the scalar initializes the first
  // value in the tuple.
  return FieldWithoutDefault == -1 ? 0 : FieldWithoutDefault;
}


/// updateInitializedElementType - This methods updates the element type and
/// initializer for a non-canonical TupleType that has an initializer for the
/// specified element.  This should only be used by TypeChecker.
void TupleType::updateInitializedElementType(unsigned EltNo, Type NewTy,
                                             Expr *NewInit) {
  TupleTypeElt &Elt = const_cast<TupleTypeElt&>(Fields[EltNo]);
  assert(Elt.hasInit() && "Can only update elements with default values");
  Elt = TupleTypeElt(NewTy, Elt.getName(), NewInit);
}

ArchetypeType *ArchetypeType::getNew(StringRef DisplayName, ASTContext &Ctx) {
  void *Mem = Ctx.Allocate(sizeof(ArchetypeType) + DisplayName.size(),
                           llvm::alignOf<ArchetypeType>());
  char *StoredStringData = (char*)Mem + sizeof(ArchetypeType);
  memcpy(StoredStringData, DisplayName.data(), DisplayName.size());
  return ::new (Mem) ArchetypeType(StringRef(StoredStringData,
                                             DisplayName.size()),
                                   Ctx);
}

//===----------------------------------------------------------------------===//
//  Type Printing
//===----------------------------------------------------------------------===//

void Type::dump() const {
  print(llvm::errs());
  llvm::errs() << '\n';
}
void Type::print(raw_ostream &OS) const {
  if (isNull())
    OS << "<null>";
  else
    Ptr->print(OS);
}

/// getString - Return the name of the type as a string, for use in
/// diagnostics only.
std::string Type::getString() const {
  std::string Result;
  llvm::raw_string_ostream OS(Result);
  print(OS);
  return OS.str();
}


/// getString - Return the name of the type as a string, for use in
/// diagnostics only.
std::string TypeBase::getString() const {
  std::string Result;
  llvm::raw_string_ostream OS(Result);
  print(OS);
  return OS.str();
}


void TypeBase::dump() const {
  print(llvm::errs());
  if (isDependentType())
    llvm::errs() << " [dependent]";
  llvm::errs() << '\n';
}

void TypeBase::print(raw_ostream &OS) const {
  switch (getKind()) {
#define TYPE(id, parent) \
  case TypeKind::id:            return cast<id##Type>(this)->print(OS);
#include "swift/AST/TypeNodes.def"
  }
  llvm_unreachable("bad type kind!");
}

void BuiltinRawPointerType::print(raw_ostream &OS) const {
  OS << "Builtin.RawPointer";
}

void BuiltinObjectPointerType::print(raw_ostream &OS) const {
  OS << "Builtin.ObjectPointer";
}

void BuiltinObjCPointerType::print(raw_ostream &OS) const {
  OS << "Builtin.ObjCPointer";
}

void BuiltinIntegerType::print(raw_ostream &OS) const {
  OS << "Builtin.int" << cast<BuiltinIntegerType>(this)->getBitWidth();
}

void BuiltinFloatType::print(raw_ostream &OS) const {
  switch (getFPKind()) {
  case IEEE16:  OS << "Builtin.FP_IEEE16"; return;
  case IEEE32:  OS << "Builtin.FP_IEEE32"; return;
  case IEEE64:  OS << "Builtin.FP_IEEE64"; return;
  case IEEE80:  OS << "Builtin.FP_IEEE80"; return;
  case IEEE128: OS << "Builtin.FP_IEEE128"; return;
  case PPC128:  OS << "Builtin.FP_PPC128"; return;
  }
}

void ErrorType::print(raw_ostream &OS) const {
  OS << "<<error type>>";
}

void UnstructuredDependentType::print(raw_ostream &OS) const {
  OS << "<<dependent type>>";
}

void ParenType::print(raw_ostream &OS) const {
  OS << '(';
  UnderlyingType->print(OS);
  OS << ')';
}

void NameAliasType::print(raw_ostream &OS) const {
  OS << TheDecl->getName().get();
}

void IdentifierType::print(raw_ostream &OS) const {
  OS << Components[0].Id.get();

  for (const Component &C : Components.slice(1, Components.size()-1))
    OS << '.' << C.Id.get();
}

void OneOfType::print(raw_ostream &OS) const {
  OS << getDecl()->getName().get();
}

void MetaTypeType::print(raw_ostream &OS) const {
  OS << "metatype<" << TheType->getName() << '>';
}

void ModuleType::print(raw_ostream &OS) const {
  OS << "module<" << TheModule->Name << '>';
}


void TupleType::print(raw_ostream &OS) const {
  OS << "(";
  
  for (unsigned i = 0, e = Fields.size(); i != e; ++i) {
    if (i) OS << ", ";
    const TupleTypeElt &TD = Fields[i];
    
    if (TD.hasName())
      OS << TD.getName() << " : ";

    if (TD.isVararg())
      OS <<  TD.getVarargBaseTy() << "...";
    else
      OS << TD.getType();
  }
  OS << ')';
}

void FunctionType::print(raw_ostream &OS) const {
  if (isAutoClosure())
    OS << "[auto_closure]";
  OS << Input << " -> " << Result;
}

void ArraySliceType::print(raw_ostream &OS) const {
  OS << Base << "[]";
}

void ArrayType::print(raw_ostream &OS) const {
  OS << Base << '[' << Size << ']';
}

void ProtocolType::print(raw_ostream &OS) const {
  OS << getDecl()->getName().str();
}

void LValueType::print(raw_ostream &OS) const {
  OS << "[byref";

  Qual qs = getQualifiers();
  if (qs != Qual::DefaultForType) {
    bool hasQual = false;
#define APPEND_QUAL(cond, text) do { \
      if (cond) {                    \
        if (hasQual) OS << ", ";     \
        hasQual = true;              \
        OS << text;                  \
      }                              \
    } while(false)

    OS << '(';
    APPEND_QUAL(!(qs & Qual::NonHeap), "heap");
    OS << ')';

#undef APPEND_QUAL
  }
  OS << "] ";
  getObjectType()->print(OS);
}

void StructType::print(raw_ostream &OS) const {
  OS << getDecl()->getName().get();
}

void ClassType::print(raw_ostream &OS) const {
  OS << getDecl()->getName().get();
}

void ArchetypeType::print(raw_ostream &OS) const {
  OS << DisplayName;
}
