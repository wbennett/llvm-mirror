//===-- CPPBackend.cpp - Library for converting LLVM code to C++ code -----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the writing of the LLVM IR as a set of C++ calls to the
// LLVM IR interface. The input module is assumed to be verified.
//
//===----------------------------------------------------------------------===//

#include "CPPTargetMachine.h"
#include "llvm/CallingConv.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/InlineAsm.h"
#include "llvm/Instruction.h"
#include "llvm/Instructions.h"
#include "llvm/Module.h"
#include "llvm/Pass.h"
#include "llvm/PassManager.h"
#include "llvm/TypeSymbolTable.h"
#include "llvm/Target/TargetMachineRegistry.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Streams.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Config/config.h"
#include <algorithm>
#include <set>

using namespace llvm;

static cl::opt<std::string>
FuncName("cppfname", cl::desc("Specify the name of the generated function"),
         cl::value_desc("function name"));

enum WhatToGenerate {
  GenProgram,
  GenModule,
  GenContents,
  GenFunction,
  GenFunctions,
  GenInline,
  GenVariable,
  GenType
};

static cl::opt<WhatToGenerate> GenerationType("cppgen", cl::Optional,
  cl::desc("Choose what kind of output to generate"),
  cl::init(GenProgram),
  cl::values(
    clEnumValN(GenProgram,  "program",   "Generate a complete program"),
    clEnumValN(GenModule,   "module",    "Generate a module definition"),
    clEnumValN(GenContents, "contents",  "Generate contents of a module"),
    clEnumValN(GenFunction, "function",  "Generate a function definition"),
    clEnumValN(GenFunctions,"functions", "Generate all function definitions"),
    clEnumValN(GenInline,   "inline",    "Generate an inline function"),
    clEnumValN(GenVariable, "variable",  "Generate a variable definition"),
    clEnumValN(GenType,     "type",      "Generate a type definition"),
    clEnumValEnd
  )
);

static cl::opt<std::string> NameToGenerate("cppfor", cl::Optional,
  cl::desc("Specify the name of the thing to generate"),
  cl::init("!bad!"));

/// CppBackendTargetMachineModule - Note that this is used on hosts
/// that cannot link in a library unless there are references into the
/// library.  In particular, it seems that it is not possible to get
/// things to work on Win32 without this.  Though it is unused, do not
/// remove it.
extern "C" int CppBackendTargetMachineModule;
int CppBackendTargetMachineModule = 0;

// Register the target.
static RegisterTarget<CPPTargetMachine> X("cpp", "C++ backend");

namespace {
  typedef std::vector<const Type*> TypeList;
  typedef std::map<const Type*,std::string> TypeMap;
  typedef std::map<const Value*,std::string> ValueMap;
  typedef std::set<std::string> NameSet;
  typedef std::set<const Type*> TypeSet;
  typedef std::set<const Value*> ValueSet;
  typedef std::map<const Value*,std::string> ForwardRefMap;

  /// CppWriter - This class is the main chunk of code that converts an LLVM
  /// module to a C++ translation unit.
  class CppWriter : public ModulePass {
    const char* progname;
    raw_ostream &Out;
    const Module *TheModule;
    uint64_t uniqueNum;
    TypeMap TypeNames;
    ValueMap ValueNames;
    TypeMap UnresolvedTypes;
    TypeList TypeStack;
    NameSet UsedNames;
    TypeSet DefinedTypes;
    ValueSet DefinedValues;
    ForwardRefMap ForwardRefs;
    bool is_inline;

  public:
    static char ID;
    explicit CppWriter(raw_ostream &o) :
      ModulePass(&ID), Out(o), uniqueNum(0), is_inline(false) {}

    virtual const char *getPassName() const { return "C++ backend"; }

    bool runOnModule(Module &M);

    void printProgram(const std::string& fname, const std::string& modName );
    void printModule(const std::string& fname, const std::string& modName );
    void printContents(const std::string& fname, const std::string& modName );
    void printFunction(const std::string& fname, const std::string& funcName );
    void printFunctions();
    void printInline(const std::string& fname, const std::string& funcName );
    void printVariable(const std::string& fname, const std::string& varName );
    void printType(const std::string& fname, const std::string& typeName );

    void error(const std::string& msg);

  private:
    void printLinkageType(GlobalValue::LinkageTypes LT);
    void printVisibilityType(GlobalValue::VisibilityTypes VisTypes);
    void printCallingConv(unsigned cc);
    void printEscapedString(const std::string& str);
    void printCFP(const ConstantFP* CFP);

    std::string getCppName(const Type* val);
    inline void printCppName(const Type* val);

    std::string getCppName(const Value* val);
    inline void printCppName(const Value* val);

    void printAttributes(const AttrListPtr &PAL, const std::string &name);
    bool printTypeInternal(const Type* Ty);
    inline void printType(const Type* Ty);
    void printTypes(const Module* M);

    void printConstant(const Constant *CPV);
    void printConstants(const Module* M);

    void printVariableUses(const GlobalVariable *GV);
    void printVariableHead(const GlobalVariable *GV);
    void printVariableBody(const GlobalVariable *GV);

    void printFunctionUses(const Function *F);
    void printFunctionHead(const Function *F);
    void printFunctionBody(const Function *F);
    void printInstruction(const Instruction *I, const std::string& bbname);
    std::string getOpName(Value*);

    void printModuleBody();
  };

  static unsigned indent_level = 0;
  inline raw_ostream& nl(raw_ostream& Out, int delta = 0) {
    Out << "\n";
    if (delta >= 0 || indent_level >= unsigned(-delta))
      indent_level += delta;
    for (unsigned i = 0; i < indent_level; ++i)
      Out << "  ";
    return Out;
  }

  inline void in() { indent_level++; }
  inline void out() { if (indent_level >0) indent_level--; }

  inline void
  sanitize(std::string& str) {
    for (size_t i = 0; i < str.length(); ++i)
      if (!isalnum(str[i]) && str[i] != '_')
        str[i] = '_';
  }

  inline std::string
  getTypePrefix(const Type* Ty ) {
    switch (Ty->getTypeID()) {
    case Type::VoidTyID:     return "void_";
    case Type::IntegerTyID:
      return std::string("int") + utostr(cast<IntegerType>(Ty)->getBitWidth()) +
        "_";
    case Type::FloatTyID:    return "float_";
    case Type::DoubleTyID:   return "double_";
    case Type::LabelTyID:    return "label_";
    case Type::FunctionTyID: return "func_";
    case Type::StructTyID:   return "struct_";
    case Type::ArrayTyID:    return "array_";
    case Type::PointerTyID:  return "ptr_";
    case Type::VectorTyID:   return "packed_";
    case Type::OpaqueTyID:   return "opaque_";
    default:                 return "other_";
    }
    return "unknown_";
  }

  // Looks up the type in the symbol table and returns a pointer to its name or
  // a null pointer if it wasn't found. Note that this isn't the same as the
  // Mode::getTypeName function which will return an empty string, not a null
  // pointer if the name is not found.
  inline const std::string*
  findTypeName(const TypeSymbolTable& ST, const Type* Ty) {
    TypeSymbolTable::const_iterator TI = ST.begin();
    TypeSymbolTable::const_iterator TE = ST.end();
    for (;TI != TE; ++TI)
      if (TI->second == Ty)
        return &(TI->first);
    return 0;
  }

  void CppWriter::error(const std::string& msg) {
    cerr << progname << ": " << msg << "\n";
    exit(2);
  }

  // printCFP - Print a floating point constant .. very carefully :)
  // This makes sure that conversion to/from floating yields the same binary
  // result so that we don't lose precision.
  void CppWriter::printCFP(const ConstantFP *CFP) {
    bool ignored;
    APFloat APF = APFloat(CFP->getValueAPF());  // copy
    if (CFP->getType() == Type::FloatTy)
      APF.convert(APFloat::IEEEdouble, APFloat::rmNearestTiesToEven, &ignored);
    Out << "ConstantFP::get(";
    Out << "APFloat(";
#if HAVE_PRINTF_A
    char Buffer[100];
    sprintf(Buffer, "%A", APF.convertToDouble());
    if ((!strncmp(Buffer, "0x", 2) ||
         !strncmp(Buffer, "-0x", 3) ||
         !strncmp(Buffer, "+0x", 3)) &&
        APF.bitwiseIsEqual(APFloat(atof(Buffer)))) {
      if (CFP->getType() == Type::DoubleTy)
        Out << "BitsToDouble(" << Buffer << ")";
      else
        Out << "BitsToFloat((float)" << Buffer << ")";
      Out << ")";
    } else {
#endif
      std::string StrVal = ftostr(CFP->getValueAPF());

      while (StrVal[0] == ' ')
        StrVal.erase(StrVal.begin());

      // Check to make sure that the stringized number is not some string like
      // "Inf" or NaN.  Check that the string matches the "[-+]?[0-9]" regex.
      if (((StrVal[0] >= '0' && StrVal[0] <= '9') ||
           ((StrVal[0] == '-' || StrVal[0] == '+') &&
            (StrVal[1] >= '0' && StrVal[1] <= '9'))) &&
          (CFP->isExactlyValue(atof(StrVal.c_str())))) {
        if (CFP->getType() == Type::DoubleTy)
          Out <<  StrVal;
        else
          Out << StrVal << "f";
      } else if (CFP->getType() == Type::DoubleTy)
        Out << "BitsToDouble(0x"
            << utohexstr(CFP->getValueAPF().bitcastToAPInt().getZExtValue())
            << "ULL) /* " << StrVal << " */";
      else
        Out << "BitsToFloat(0x"
            << utohexstr((uint32_t)CFP->getValueAPF().
                                        bitcastToAPInt().getZExtValue())
            << "U) /* " << StrVal << " */";
      Out << ")";
#if HAVE_PRINTF_A
    }
#endif
    Out << ")";
  }

  void CppWriter::printCallingConv(unsigned cc){
    // Print the calling convention.
    switch (cc) {
    case CallingConv::C:     Out << "CallingConv::C"; break;
    case CallingConv::Fast:  Out << "CallingConv::Fast"; break;
    case CallingConv::Cold:  Out << "CallingConv::Cold"; break;
    case CallingConv::FirstTargetCC: Out << "CallingConv::FirstTargetCC"; break;
    default:                 Out << cc; break;
    }
  }

  void CppWriter::printLinkageType(GlobalValue::LinkageTypes LT) {
    switch (LT) {
    case GlobalValue::InternalLinkage:
      Out << "GlobalValue::InternalLinkage"; break;
    case GlobalValue::PrivateLinkage:
      Out << "GlobalValue::PrivateLinkage"; break;
    case GlobalValue::LinkOnceAnyLinkage:
      Out << "GlobalValue::LinkOnceAnyLinkage "; break;
    case GlobalValue::LinkOnceODRLinkage:
      Out << "GlobalValue::LinkOnceODRLinkage "; break;
    case GlobalValue::WeakAnyLinkage:
      Out << "GlobalValue::WeakAnyLinkage"; break;
    case GlobalValue::WeakODRLinkage:
      Out << "GlobalValue::WeakODRLinkage"; break;
    case GlobalValue::AppendingLinkage:
      Out << "GlobalValue::AppendingLinkage"; break;
    case GlobalValue::ExternalLinkage:
      Out << "GlobalValue::ExternalLinkage"; break;
    case GlobalValue::DLLImportLinkage:
      Out << "GlobalValue::DLLImportLinkage"; break;
    case GlobalValue::DLLExportLinkage:
      Out << "GlobalValue::DLLExportLinkage"; break;
    case GlobalValue::ExternalWeakLinkage:
      Out << "GlobalValue::ExternalWeakLinkage"; break;
    case GlobalValue::GhostLinkage:
      Out << "GlobalValue::GhostLinkage"; break;
    case GlobalValue::CommonAnyLinkage:
      Out << "GlobalValue::CommonAnyLinkage"; break;
    case GlobalValue::CommonODRLinkage:
      Out << "GlobalValue::CommonODRLinkage"; break;
    }
  }

  void CppWriter::printVisibilityType(GlobalValue::VisibilityTypes VisType) {
    switch (VisType) {
    default: assert(0 && "Unknown GVar visibility");
    case GlobalValue::DefaultVisibility:
      Out << "GlobalValue::DefaultVisibility";
      break;
    case GlobalValue::HiddenVisibility:
      Out << "GlobalValue::HiddenVisibility";
      break;
    case GlobalValue::ProtectedVisibility:
      Out << "GlobalValue::ProtectedVisibility";
      break;
    }
  }

  // printEscapedString - Print each character of the specified string, escaping
  // it if it is not printable or if it is an escape char.
  void CppWriter::printEscapedString(const std::string &Str) {
    for (unsigned i = 0, e = Str.size(); i != e; ++i) {
      unsigned char C = Str[i];
      if (isprint(C) && C != '"' && C != '\\') {
        Out << C;
      } else {
        Out << "\\x"
            << (char) ((C/16  < 10) ? ( C/16 +'0') : ( C/16 -10+'A'))
            << (char)(((C&15) < 10) ? ((C&15)+'0') : ((C&15)-10+'A'));
      }
    }
  }

  std::string CppWriter::getCppName(const Type* Ty) {
    // First, handle the primitive types .. easy
    if (Ty->isPrimitiveType() || Ty->isInteger()) {
      switch (Ty->getTypeID()) {
      case Type::VoidTyID:   return "Type::VoidTy";
      case Type::IntegerTyID: {
        unsigned BitWidth = cast<IntegerType>(Ty)->getBitWidth();
        return "IntegerType::get(" + utostr(BitWidth) + ")";
      }
      case Type::FloatTyID:  return "Type::FloatTy";
      case Type::DoubleTyID: return "Type::DoubleTy";
      case Type::LabelTyID:  return "Type::LabelTy";
      default:
        error("Invalid primitive type");
        break;
      }
      return "Type::VoidTy"; // shouldn't be returned, but make it sensible
    }

    // Now, see if we've seen the type before and return that
    TypeMap::iterator I = TypeNames.find(Ty);
    if (I != TypeNames.end())
      return I->second;

    // Okay, let's build a new name for this type. Start with a prefix
    const char* prefix = 0;
    switch (Ty->getTypeID()) {
    case Type::FunctionTyID:    prefix = "FuncTy_"; break;
    case Type::StructTyID:      prefix = "StructTy_"; break;
    case Type::ArrayTyID:       prefix = "ArrayTy_"; break;
    case Type::PointerTyID:     prefix = "PointerTy_"; break;
    case Type::OpaqueTyID:      prefix = "OpaqueTy_"; break;
    case Type::VectorTyID:      prefix = "VectorTy_"; break;
    default:                    prefix = "OtherTy_"; break; // prevent breakage
    }

    // See if the type has a name in the symboltable and build accordingly
    const std::string* tName = findTypeName(TheModule->getTypeSymbolTable(), Ty);
    std::string name;
    if (tName)
      name = std::string(prefix) + *tName;
    else
      name = std::string(prefix) + utostr(uniqueNum++);
    sanitize(name);

    // Save the name
    return TypeNames[Ty] = name;
  }

  void CppWriter::printCppName(const Type* Ty) {
    printEscapedString(getCppName(Ty));
  }

  std::string CppWriter::getCppName(const Value* val) {
    std::string name;
    ValueMap::iterator I = ValueNames.find(val);
    if (I != ValueNames.end() && I->first == val)
      return  I->second;

    if (const GlobalVariable* GV = dyn_cast<GlobalVariable>(val)) {
      name = std::string("gvar_") +
        getTypePrefix(GV->getType()->getElementType());
    } else if (isa<Function>(val)) {
      name = std::string("func_");
    } else if (const Constant* C = dyn_cast<Constant>(val)) {
      name = std::string("const_") + getTypePrefix(C->getType());
    } else if (const Argument* Arg = dyn_cast<Argument>(val)) {
      if (is_inline) {
        unsigned argNum = std::distance(Arg->getParent()->arg_begin(),
                                        Function::const_arg_iterator(Arg)) + 1;
        name = std::string("arg_") + utostr(argNum);
        NameSet::iterator NI = UsedNames.find(name);
        if (NI != UsedNames.end())
          name += std::string("_") + utostr(uniqueNum++);
        UsedNames.insert(name);
        return ValueNames[val] = name;
      } else {
        name = getTypePrefix(val->getType());
      }
    } else {
      name = getTypePrefix(val->getType());
    }
    name += (val->hasName() ? val->getName() : utostr(uniqueNum++));
    sanitize(name);
    NameSet::iterator NI = UsedNames.find(name);
    if (NI != UsedNames.end())
      name += std::string("_") + utostr(uniqueNum++);
    UsedNames.insert(name);
    return ValueNames[val] = name;
  }

  void CppWriter::printCppName(const Value* val) {
    printEscapedString(getCppName(val));
  }

  void CppWriter::printAttributes(const AttrListPtr &PAL,
                                  const std::string &name) {
    Out << "AttrListPtr " << name << "_PAL;";
    nl(Out);
    if (!PAL.isEmpty()) {
      Out << '{'; in(); nl(Out);
      Out << "SmallVector<AttributeWithIndex, 4> Attrs;"; nl(Out);
      Out << "AttributeWithIndex PAWI;"; nl(Out);
      for (unsigned i = 0; i < PAL.getNumSlots(); ++i) {
        unsigned index = PAL.getSlot(i).Index;
        Attributes attrs = PAL.getSlot(i).Attrs;
        Out << "PAWI.Index = " << index << "U; PAWI.Attrs = 0 ";
#define HANDLE_ATTR(X)                 \
        if (attrs & Attribute::X)      \
          Out << " | Attribute::" #X;  \
        attrs &= ~Attribute::X;
        
        HANDLE_ATTR(SExt);
        HANDLE_ATTR(ZExt);
        HANDLE_ATTR(StructRet);
        HANDLE_ATTR(InReg);
        HANDLE_ATTR(NoReturn);
        HANDLE_ATTR(NoUnwind);
        HANDLE_ATTR(ByVal);
        HANDLE_ATTR(NoAlias);
        HANDLE_ATTR(Nest);
        HANDLE_ATTR(ReadNone);
        HANDLE_ATTR(ReadOnly);
        HANDLE_ATTR(NoCapture);
#undef HANDLE_ATTR
        assert(attrs == 0 && "Unhandled attribute!");
        Out << ";";
        nl(Out);
        Out << "Attrs.push_back(PAWI);";
        nl(Out);
      }
      Out << name << "_PAL = AttrListPtr::get(Attrs.begin(), Attrs.end());";
      nl(Out);
      out(); nl(Out);
      Out << '}'; nl(Out);
    }
  }

  bool CppWriter::printTypeInternal(const Type* Ty) {
    // We don't print definitions for primitive types
    if (Ty->isPrimitiveType() || Ty->isInteger())
      return false;

    // If we already defined this type, we don't need to define it again.
    if (DefinedTypes.find(Ty) != DefinedTypes.end())
      return false;

    // Everything below needs the name for the type so get it now.
    std::string typeName(getCppName(Ty));

    // Search the type stack for recursion. If we find it, then generate this
    // as an OpaqueType, but make sure not to do this multiple times because
    // the type could appear in multiple places on the stack. Once the opaque
    // definition is issued, it must not be re-issued. Consequently we have to
    // check the UnresolvedTypes list as well.
    TypeList::const_iterator TI = std::find(TypeStack.begin(), TypeStack.end(),
                                            Ty);
    if (TI != TypeStack.end()) {
      TypeMap::const_iterator I = UnresolvedTypes.find(Ty);
      if (I == UnresolvedTypes.end()) {
        Out << "PATypeHolder " << typeName << "_fwd = OpaqueType::get();";
        nl(Out);
        UnresolvedTypes[Ty] = typeName;
      }
      return true;
    }

    // We're going to print a derived type which, by definition, contains other
    // types. So, push this one we're printing onto the type stack to assist with
    // recursive definitions.
    TypeStack.push_back(Ty);

    // Print the type definition
    switch (Ty->getTypeID()) {
    case Type::FunctionTyID:  {
      const FunctionType* FT = cast<FunctionType>(Ty);
      Out << "std::vector<const Type*>" << typeName << "_args;";
      nl(Out);
      FunctionType::param_iterator PI = FT->param_begin();
      FunctionType::param_iterator PE = FT->param_end();
      for (; PI != PE; ++PI) {
        const Type* argTy = static_cast<const Type*>(*PI);
        bool isForward = printTypeInternal(argTy);
        std::string argName(getCppName(argTy));
        Out << typeName << "_args.push_back(" << argName;
        if (isForward)
          Out << "_fwd";
        Out << ");";
        nl(Out);
      }
      bool isForward = printTypeInternal(FT->getReturnType());
      std::string retTypeName(getCppName(FT->getReturnType()));
      Out << "FunctionType* " << typeName << " = FunctionType::get(";
      in(); nl(Out) << "/*Result=*/" << retTypeName;
      if (isForward)
        Out << "_fwd";
      Out << ",";
      nl(Out) << "/*Params=*/" << typeName << "_args,";
      nl(Out) << "/*isVarArg=*/" << (FT->isVarArg() ? "true" : "false") << ");";
      out();
      nl(Out);
      break;
    }
    case Type::StructTyID: {
      const StructType* ST = cast<StructType>(Ty);
      Out << "std::vector<const Type*>" << typeName << "_fields;";
      nl(Out);
      StructType::element_iterator EI = ST->element_begin();
      StructType::element_iterator EE = ST->element_end();
      for (; EI != EE; ++EI) {
        const Type* fieldTy = static_cast<const Type*>(*EI);
        bool isForward = printTypeInternal(fieldTy);
        std::string fieldName(getCppName(fieldTy));
        Out << typeName << "_fields.push_back(" << fieldName;
        if (isForward)
          Out << "_fwd";
        Out << ");";
        nl(Out);
      }
      Out << "StructType* " << typeName << " = StructType::get("
          << typeName << "_fields, /*isPacked=*/"
          << (ST->isPacked() ? "true" : "false") << ");";
      nl(Out);
      break;
    }
    case Type::ArrayTyID: {
      const ArrayType* AT = cast<ArrayType>(Ty);
      const Type* ET = AT->getElementType();
      bool isForward = printTypeInternal(ET);
      std::string elemName(getCppName(ET));
      Out << "ArrayType* " << typeName << " = ArrayType::get("
          << elemName << (isForward ? "_fwd" : "")
          << ", " << utostr(AT->getNumElements()) << ");";
      nl(Out);
      break;
    }
    case Type::PointerTyID: {
      const PointerType* PT = cast<PointerType>(Ty);
      const Type* ET = PT->getElementType();
      bool isForward = printTypeInternal(ET);
      std::string elemName(getCppName(ET));
      Out << "PointerType* " << typeName << " = PointerType::get("
          << elemName << (isForward ? "_fwd" : "")
          << ", " << utostr(PT->getAddressSpace()) << ");";
      nl(Out);
      break;
    }
    case Type::VectorTyID: {
      const VectorType* PT = cast<VectorType>(Ty);
      const Type* ET = PT->getElementType();
      bool isForward = printTypeInternal(ET);
      std::string elemName(getCppName(ET));
      Out << "VectorType* " << typeName << " = VectorType::get("
          << elemName << (isForward ? "_fwd" : "")
          << ", " << utostr(PT->getNumElements()) << ");";
      nl(Out);
      break;
    }
    case Type::OpaqueTyID: {
      Out << "OpaqueType* " << typeName << " = OpaqueType::get();";
      nl(Out);
      break;
    }
    default:
      error("Invalid TypeID");
    }

    // If the type had a name, make sure we recreate it.
    const std::string* progTypeName =
      findTypeName(TheModule->getTypeSymbolTable(),Ty);
    if (progTypeName) {
      Out << "mod->addTypeName(\"" << *progTypeName << "\", "
          << typeName << ");";
      nl(Out);
    }

    // Pop us off the type stack
    TypeStack.pop_back();

    // Indicate that this type is now defined.
    DefinedTypes.insert(Ty);

    // Early resolve as many unresolved types as possible. Search the unresolved
    // types map for the type we just printed. Now that its definition is complete
    // we can resolve any previous references to it. This prevents a cascade of
    // unresolved types.
    TypeMap::iterator I = UnresolvedTypes.find(Ty);
    if (I != UnresolvedTypes.end()) {
      Out << "cast<OpaqueType>(" << I->second
          << "_fwd.get())->refineAbstractTypeTo(" << I->second << ");";
      nl(Out);
      Out << I->second << " = cast<";
      switch (Ty->getTypeID()) {
      case Type::FunctionTyID: Out << "FunctionType"; break;
      case Type::ArrayTyID:    Out << "ArrayType"; break;
      case Type::StructTyID:   Out << "StructType"; break;
      case Type::VectorTyID:   Out << "VectorType"; break;
      case Type::PointerTyID:  Out << "PointerType"; break;
      case Type::OpaqueTyID:   Out << "OpaqueType"; break;
      default:                 Out << "NoSuchDerivedType"; break;
      }
      Out << ">(" << I->second << "_fwd.get());";
      nl(Out); nl(Out);
      UnresolvedTypes.erase(I);
    }

    // Finally, separate the type definition from other with a newline.
    nl(Out);

    // We weren't a recursive type
    return false;
  }

  // Prints a type definition. Returns true if it could not resolve all the
  // types in the definition but had to use a forward reference.
  void CppWriter::printType(const Type* Ty) {
    assert(TypeStack.empty());
    TypeStack.clear();
    printTypeInternal(Ty);
    assert(TypeStack.empty());
  }

  void CppWriter::printTypes(const Module* M) {
    // Walk the symbol table and print out all its types
    const TypeSymbolTable& symtab = M->getTypeSymbolTable();
    for (TypeSymbolTable::const_iterator TI = symtab.begin(), TE = symtab.end();
         TI != TE; ++TI) {

      // For primitive types and types already defined, just add a name
      TypeMap::const_iterator TNI = TypeNames.find(TI->second);
      if (TI->second->isInteger() || TI->second->isPrimitiveType() ||
          TNI != TypeNames.end()) {
        Out << "mod->addTypeName(\"";
        printEscapedString(TI->first);
        Out << "\", " << getCppName(TI->second) << ");";
        nl(Out);
        // For everything else, define the type
      } else {
        printType(TI->second);
      }
    }

    // Add all of the global variables to the value table...
    for (Module::const_global_iterator I = TheModule->global_begin(),
           E = TheModule->global_end(); I != E; ++I) {
      if (I->hasInitializer())
        printType(I->getInitializer()->getType());
      printType(I->getType());
    }

    // Add all the functions to the table
    for (Module::const_iterator FI = TheModule->begin(), FE = TheModule->end();
         FI != FE; ++FI) {
      printType(FI->getReturnType());
      printType(FI->getFunctionType());
      // Add all the function arguments
      for (Function::const_arg_iterator AI = FI->arg_begin(),
             AE = FI->arg_end(); AI != AE; ++AI) {
        printType(AI->getType());
      }

      // Add all of the basic blocks and instructions
      for (Function::const_iterator BB = FI->begin(),
             E = FI->end(); BB != E; ++BB) {
        printType(BB->getType());
        for (BasicBlock::const_iterator I = BB->begin(), E = BB->end(); I!=E;
             ++I) {
          printType(I->getType());
          for (unsigned i = 0; i < I->getNumOperands(); ++i)
            printType(I->getOperand(i)->getType());
        }
      }
    }
  }


  // printConstant - Print out a constant pool entry...
  void CppWriter::printConstant(const Constant *CV) {
    // First, if the constant is actually a GlobalValue (variable or function)
    // or its already in the constant list then we've printed it already and we
    // can just return.
    if (isa<GlobalValue>(CV) || ValueNames.find(CV) != ValueNames.end())
      return;

    std::string constName(getCppName(CV));
    std::string typeName(getCppName(CV->getType()));

    if (isa<GlobalValue>(CV)) {
      // Skip variables and functions, we emit them elsewhere
      return;
    }

    if (const ConstantInt *CI = dyn_cast<ConstantInt>(CV)) {
      std::string constValue = CI->getValue().toString(10, true);
      Out << "ConstantInt* " << constName << " = ConstantInt::get(APInt("
          << cast<IntegerType>(CI->getType())->getBitWidth() << ",  \""
          <<  constValue << "\", " << constValue.length() << ", 10));";
    } else if (isa<ConstantAggregateZero>(CV)) {
      Out << "ConstantAggregateZero* " << constName
          << " = ConstantAggregateZero::get(" << typeName << ");";
    } else if (isa<ConstantPointerNull>(CV)) {
      Out << "ConstantPointerNull* " << constName
          << " = ConstantPointerNull::get(" << typeName << ");";
    } else if (const ConstantFP *CFP = dyn_cast<ConstantFP>(CV)) {
      Out << "ConstantFP* " << constName << " = ";
      printCFP(CFP);
      Out << ";";
    } else if (const ConstantArray *CA = dyn_cast<ConstantArray>(CV)) {
      if (CA->isString() && CA->getType()->getElementType() == Type::Int8Ty) {
        Out << "Constant* " << constName << " = ConstantArray::get(\"";
        std::string tmp = CA->getAsString();
        bool nullTerminate = false;
        if (tmp[tmp.length()-1] == 0) {
          tmp.erase(tmp.length()-1);
          nullTerminate = true;
        }
        printEscapedString(tmp);
        // Determine if we want null termination or not.
        if (nullTerminate)
          Out << "\", true"; // Indicate that the null terminator should be
                             // added.
        else
          Out << "\", false";// No null terminator
        Out << ");";
      } else {
        Out << "std::vector<Constant*> " << constName << "_elems;";
        nl(Out);
        unsigned N = CA->getNumOperands();
        for (unsigned i = 0; i < N; ++i) {
          printConstant(CA->getOperand(i)); // recurse to print operands
          Out << constName << "_elems.push_back("
              << getCppName(CA->getOperand(i)) << ");";
          nl(Out);
        }
        Out << "Constant* " << constName << " = ConstantArray::get("
            << typeName << ", " << constName << "_elems);";
      }
    } else if (const ConstantStruct *CS = dyn_cast<ConstantStruct>(CV)) {
      Out << "std::vector<Constant*> " << constName << "_fields;";
      nl(Out);
      unsigned N = CS->getNumOperands();
      for (unsigned i = 0; i < N; i++) {
        printConstant(CS->getOperand(i));
        Out << constName << "_fields.push_back("
            << getCppName(CS->getOperand(i)) << ");";
        nl(Out);
      }
      Out << "Constant* " << constName << " = ConstantStruct::get("
          << typeName << ", " << constName << "_fields);";
    } else if (const ConstantVector *CP = dyn_cast<ConstantVector>(CV)) {
      Out << "std::vector<Constant*> " << constName << "_elems;";
      nl(Out);
      unsigned N = CP->getNumOperands();
      for (unsigned i = 0; i < N; ++i) {
        printConstant(CP->getOperand(i));
        Out << constName << "_elems.push_back("
            << getCppName(CP->getOperand(i)) << ");";
        nl(Out);
      }
      Out << "Constant* " << constName << " = ConstantVector::get("
          << typeName << ", " << constName << "_elems);";
    } else if (isa<UndefValue>(CV)) {
      Out << "UndefValue* " << constName << " = UndefValue::get("
          << typeName << ");";
    } else if (const ConstantExpr *CE = dyn_cast<ConstantExpr>(CV)) {
      if (CE->getOpcode() == Instruction::GetElementPtr) {
        Out << "std::vector<Constant*> " << constName << "_indices;";
        nl(Out);
        printConstant(CE->getOperand(0));
        for (unsigned i = 1; i < CE->getNumOperands(); ++i ) {
          printConstant(CE->getOperand(i));
          Out << constName << "_indices.push_back("
              << getCppName(CE->getOperand(i)) << ");";
          nl(Out);
        }
        Out << "Constant* " << constName
            << " = ConstantExpr::getGetElementPtr("
            << getCppName(CE->getOperand(0)) << ", "
            << "&" << constName << "_indices[0], "
            << constName << "_indices.size()"
            << " );";
      } else if (CE->isCast()) {
        printConstant(CE->getOperand(0));
        Out << "Constant* " << constName << " = ConstantExpr::getCast(";
        switch (CE->getOpcode()) {
        default: assert(0 && "Invalid cast opcode");
        case Instruction::Trunc: Out << "Instruction::Trunc"; break;
        case Instruction::ZExt:  Out << "Instruction::ZExt"; break;
        case Instruction::SExt:  Out << "Instruction::SExt"; break;
        case Instruction::FPTrunc:  Out << "Instruction::FPTrunc"; break;
        case Instruction::FPExt:  Out << "Instruction::FPExt"; break;
        case Instruction::FPToUI:  Out << "Instruction::FPToUI"; break;
        case Instruction::FPToSI:  Out << "Instruction::FPToSI"; break;
        case Instruction::UIToFP:  Out << "Instruction::UIToFP"; break;
        case Instruction::SIToFP:  Out << "Instruction::SIToFP"; break;
        case Instruction::PtrToInt:  Out << "Instruction::PtrToInt"; break;
        case Instruction::IntToPtr:  Out << "Instruction::IntToPtr"; break;
        case Instruction::BitCast:  Out << "Instruction::BitCast"; break;
        }
        Out << ", " << getCppName(CE->getOperand(0)) << ", "
            << getCppName(CE->getType()) << ");";
      } else {
        unsigned N = CE->getNumOperands();
        for (unsigned i = 0; i < N; ++i ) {
          printConstant(CE->getOperand(i));
        }
        Out << "Constant* " << constName << " = ConstantExpr::";
        switch (CE->getOpcode()) {
        case Instruction::Add:    Out << "getAdd(";  break;
        case Instruction::Sub:    Out << "getSub("; break;
        case Instruction::Mul:    Out << "getMul("; break;
        case Instruction::UDiv:   Out << "getUDiv("; break;
        case Instruction::SDiv:   Out << "getSDiv("; break;
        case Instruction::FDiv:   Out << "getFDiv("; break;
        case Instruction::URem:   Out << "getURem("; break;
        case Instruction::SRem:   Out << "getSRem("; break;
        case Instruction::FRem:   Out << "getFRem("; break;
        case Instruction::And:    Out << "getAnd("; break;
        case Instruction::Or:     Out << "getOr("; break;
        case Instruction::Xor:    Out << "getXor("; break;
        case Instruction::ICmp:
          Out << "getICmp(ICmpInst::ICMP_";
          switch (CE->getPredicate()) {
          case ICmpInst::ICMP_EQ:  Out << "EQ"; break;
          case ICmpInst::ICMP_NE:  Out << "NE"; break;
          case ICmpInst::ICMP_SLT: Out << "SLT"; break;
          case ICmpInst::ICMP_ULT: Out << "ULT"; break;
          case ICmpInst::ICMP_SGT: Out << "SGT"; break;
          case ICmpInst::ICMP_UGT: Out << "UGT"; break;
          case ICmpInst::ICMP_SLE: Out << "SLE"; break;
          case ICmpInst::ICMP_ULE: Out << "ULE"; break;
          case ICmpInst::ICMP_SGE: Out << "SGE"; break;
          case ICmpInst::ICMP_UGE: Out << "UGE"; break;
          default: error("Invalid ICmp Predicate");
          }
          break;
        case Instruction::FCmp:
          Out << "getFCmp(FCmpInst::FCMP_";
          switch (CE->getPredicate()) {
          case FCmpInst::FCMP_FALSE: Out << "FALSE"; break;
          case FCmpInst::FCMP_ORD:   Out << "ORD"; break;
          case FCmpInst::FCMP_UNO:   Out << "UNO"; break;
          case FCmpInst::FCMP_OEQ:   Out << "OEQ"; break;
          case FCmpInst::FCMP_UEQ:   Out << "UEQ"; break;
          case FCmpInst::FCMP_ONE:   Out << "ONE"; break;
          case FCmpInst::FCMP_UNE:   Out << "UNE"; break;
          case FCmpInst::FCMP_OLT:   Out << "OLT"; break;
          case FCmpInst::FCMP_ULT:   Out << "ULT"; break;
          case FCmpInst::FCMP_OGT:   Out << "OGT"; break;
          case FCmpInst::FCMP_UGT:   Out << "UGT"; break;
          case FCmpInst::FCMP_OLE:   Out << "OLE"; break;
          case FCmpInst::FCMP_ULE:   Out << "ULE"; break;
          case FCmpInst::FCMP_OGE:   Out << "OGE"; break;
          case FCmpInst::FCMP_UGE:   Out << "UGE"; break;
          case FCmpInst::FCMP_TRUE:  Out << "TRUE"; break;
          default: error("Invalid FCmp Predicate");
          }
          break;
        case Instruction::Shl:     Out << "getShl("; break;
        case Instruction::LShr:    Out << "getLShr("; break;
        case Instruction::AShr:    Out << "getAShr("; break;
        case Instruction::Select:  Out << "getSelect("; break;
        case Instruction::ExtractElement: Out << "getExtractElement("; break;
        case Instruction::InsertElement:  Out << "getInsertElement("; break;
        case Instruction::ShuffleVector:  Out << "getShuffleVector("; break;
        default:
          error("Invalid constant expression");
          break;
        }
        Out << getCppName(CE->getOperand(0));
        for (unsigned i = 1; i < CE->getNumOperands(); ++i)
          Out << ", " << getCppName(CE->getOperand(i));
        Out << ");";
      }
    } else {
      error("Bad Constant");
      Out << "Constant* " << constName << " = 0; ";
    }
    nl(Out);
  }

  void CppWriter::printConstants(const Module* M) {
    // Traverse all the global variables looking for constant initializers
    for (Module::const_global_iterator I = TheModule->global_begin(),
           E = TheModule->global_end(); I != E; ++I)
      if (I->hasInitializer())
        printConstant(I->getInitializer());

    // Traverse the LLVM functions looking for constants
    for (Module::const_iterator FI = TheModule->begin(), FE = TheModule->end();
         FI != FE; ++FI) {
      // Add all of the basic blocks and instructions
      for (Function::const_iterator BB = FI->begin(),
             E = FI->end(); BB != E; ++BB) {
        for (BasicBlock::const_iterator I = BB->begin(), E = BB->end(); I!=E;
             ++I) {
          for (unsigned i = 0; i < I->getNumOperands(); ++i) {
            if (Constant* C = dyn_cast<Constant>(I->getOperand(i))) {
              printConstant(C);
            }
          }
        }
      }
    }
  }

  void CppWriter::printVariableUses(const GlobalVariable *GV) {
    nl(Out) << "// Type Definitions";
    nl(Out);
    printType(GV->getType());
    if (GV->hasInitializer()) {
      Constant* Init = GV->getInitializer();
      printType(Init->getType());
      if (Function* F = dyn_cast<Function>(Init)) {
        nl(Out)<< "/ Function Declarations"; nl(Out);
        printFunctionHead(F);
      } else if (GlobalVariable* gv = dyn_cast<GlobalVariable>(Init)) {
        nl(Out) << "// Global Variable Declarations"; nl(Out);
        printVariableHead(gv);
      } else  {
        nl(Out) << "// Constant Definitions"; nl(Out);
        printConstant(gv);
      }
      if (GlobalVariable* gv = dyn_cast<GlobalVariable>(Init)) {
        nl(Out) << "// Global Variable Definitions"; nl(Out);
        printVariableBody(gv);
      }
    }
  }

  void CppWriter::printVariableHead(const GlobalVariable *GV) {
    nl(Out) << "GlobalVariable* " << getCppName(GV);
    if (is_inline) {
      Out << " = mod->getGlobalVariable(";
      printEscapedString(GV->getName());
      Out << ", " << getCppName(GV->getType()->getElementType()) << ",true)";
      nl(Out) << "if (!" << getCppName(GV) << ") {";
      in(); nl(Out) << getCppName(GV);
    }
    Out << " = new GlobalVariable(";
    nl(Out) << "/*Type=*/";
    printCppName(GV->getType()->getElementType());
    Out << ",";
    nl(Out) << "/*isConstant=*/" << (GV->isConstant()?"true":"false");
    Out << ",";
    nl(Out) << "/*Linkage=*/";
    printLinkageType(GV->getLinkage());
    Out << ",";
    nl(Out) << "/*Initializer=*/0, ";
    if (GV->hasInitializer()) {
      Out << "// has initializer, specified below";
    }
    nl(Out) << "/*Name=*/\"";
    printEscapedString(GV->getName());
    Out << "\",";
    nl(Out) << "mod);";
    nl(Out);

    if (GV->hasSection()) {
      printCppName(GV);
      Out << "->setSection(\"";
      printEscapedString(GV->getSection());
      Out << "\");";
      nl(Out);
    }
    if (GV->getAlignment()) {
      printCppName(GV);
      Out << "->setAlignment(" << utostr(GV->getAlignment()) << ");";
      nl(Out);
    }
    if (GV->getVisibility() != GlobalValue::DefaultVisibility) {
      printCppName(GV);
      Out << "->setVisibility(";
      printVisibilityType(GV->getVisibility());
      Out << ");";
      nl(Out);
    }
    if (is_inline) {
      out(); Out << "}"; nl(Out);
    }
  }

  void CppWriter::printVariableBody(const GlobalVariable *GV) {
    if (GV->hasInitializer()) {
      printCppName(GV);
      Out << "->setInitializer(";
      Out << getCppName(GV->getInitializer()) << ");";
      nl(Out);
    }
  }

  std::string CppWriter::getOpName(Value* V) {
    if (!isa<Instruction>(V) || DefinedValues.find(V) != DefinedValues.end())
      return getCppName(V);

    // See if its alread in the map of forward references, if so just return the
    // name we already set up for it
    ForwardRefMap::const_iterator I = ForwardRefs.find(V);
    if (I != ForwardRefs.end())
      return I->second;

    // This is a new forward reference. Generate a unique name for it
    std::string result(std::string("fwdref_") + utostr(uniqueNum++));

    // Yes, this is a hack. An Argument is the smallest instantiable value that
    // we can make as a placeholder for the real value. We'll replace these
    // Argument instances later.
    Out << "Argument* " << result << " = new Argument("
        << getCppName(V->getType()) << ");";
    nl(Out);
    ForwardRefs[V] = result;
    return result;
  }

  // printInstruction - This member is called for each Instruction in a function.
  void CppWriter::printInstruction(const Instruction *I,
                                   const std::string& bbname) {
    std::string iName(getCppName(I));

    // Before we emit this instruction, we need to take care of generating any
    // forward references. So, we get the names of all the operands in advance
    std::string* opNames = new std::string[I->getNumOperands()];
    for (unsigned i = 0; i < I->getNumOperands(); i++) {
      opNames[i] = getOpName(I->getOperand(i));
    }

    switch (I->getOpcode()) {
    default:
      error("Invalid instruction");
      break;

    case Instruction::Ret: {
      const ReturnInst* ret =  cast<ReturnInst>(I);
      Out << "ReturnInst::Create("
          << (ret->getReturnValue() ? opNames[0] + ", " : "") << bbname << ");";
      break;
    }
    case Instruction::Br: {
      const BranchInst* br = cast<BranchInst>(I);
      Out << "BranchInst::Create(" ;
      if (br->getNumOperands() == 3 ) {
        Out << opNames[0] << ", "
            << opNames[1] << ", "
            << opNames[2] << ", ";

      } else if (br->getNumOperands() == 1) {
        Out << opNames[0] << ", ";
      } else {
        error("Branch with 2 operands?");
      }
      Out << bbname << ");";
      break;
    }
    case Instruction::Switch: {
      const SwitchInst* sw = cast<SwitchInst>(I);
      Out << "SwitchInst* " << iName << " = SwitchInst::Create("
          << opNames[0] << ", "
          << opNames[1] << ", "
          << sw->getNumCases() << ", " << bbname << ");";
      nl(Out);
      for (unsigned i = 2; i < sw->getNumOperands(); i += 2 ) {
        Out << iName << "->addCase("
            << opNames[i] << ", "
            << opNames[i+1] << ");";
        nl(Out);
      }
      break;
    }
    case Instruction::Invoke: {
      const InvokeInst* inv = cast<InvokeInst>(I);
      Out << "std::vector<Value*> " << iName << "_params;";
      nl(Out);
      for (unsigned i = 3; i < inv->getNumOperands(); ++i) {
        Out << iName << "_params.push_back("
            << opNames[i] << ");";
        nl(Out);
      }
      Out << "InvokeInst *" << iName << " = InvokeInst::Create("
          << opNames[0] << ", "
          << opNames[1] << ", "
          << opNames[2] << ", "
          << iName << "_params.begin(), " << iName << "_params.end(), \"";
      printEscapedString(inv->getName());
      Out << "\", " << bbname << ");";
      nl(Out) << iName << "->setCallingConv(";
      printCallingConv(inv->getCallingConv());
      Out << ");";
      printAttributes(inv->getAttributes(), iName);
      Out << iName << "->setAttributes(" << iName << "_PAL);";
      nl(Out);
      break;
    }
    case Instruction::Unwind: {
      Out << "new UnwindInst("
          << bbname << ");";
      break;
    }
    case Instruction::Unreachable:{
      Out << "new UnreachableInst("
          << bbname << ");";
      break;
    }
    case Instruction::Add:
    case Instruction::Sub:
    case Instruction::Mul:
    case Instruction::UDiv:
    case Instruction::SDiv:
    case Instruction::FDiv:
    case Instruction::URem:
    case Instruction::SRem:
    case Instruction::FRem:
    case Instruction::And:
    case Instruction::Or:
    case Instruction::Xor:
    case Instruction::Shl:
    case Instruction::LShr:
    case Instruction::AShr:{
      Out << "BinaryOperator* " << iName << " = BinaryOperator::Create(";
      switch (I->getOpcode()) {
      case Instruction::Add: Out << "Instruction::Add"; break;
      case Instruction::Sub: Out << "Instruction::Sub"; break;
      case Instruction::Mul: Out << "Instruction::Mul"; break;
      case Instruction::UDiv:Out << "Instruction::UDiv"; break;
      case Instruction::SDiv:Out << "Instruction::SDiv"; break;
      case Instruction::FDiv:Out << "Instruction::FDiv"; break;
      case Instruction::URem:Out << "Instruction::URem"; break;
      case Instruction::SRem:Out << "Instruction::SRem"; break;
      case Instruction::FRem:Out << "Instruction::FRem"; break;
      case Instruction::And: Out << "Instruction::And"; break;
      case Instruction::Or:  Out << "Instruction::Or";  break;
      case Instruction::Xor: Out << "Instruction::Xor"; break;
      case Instruction::Shl: Out << "Instruction::Shl"; break;
      case Instruction::LShr:Out << "Instruction::LShr"; break;
      case Instruction::AShr:Out << "Instruction::AShr"; break;
      default: Out << "Instruction::BadOpCode"; break;
      }
      Out << ", " << opNames[0] << ", " << opNames[1] << ", \"";
      printEscapedString(I->getName());
      Out << "\", " << bbname << ");";
      break;
    }
    case Instruction::FCmp: {
      Out << "FCmpInst* " << iName << " = new FCmpInst(";
      switch (cast<FCmpInst>(I)->getPredicate()) {
      case FCmpInst::FCMP_FALSE: Out << "FCmpInst::FCMP_FALSE"; break;
      case FCmpInst::FCMP_OEQ  : Out << "FCmpInst::FCMP_OEQ"; break;
      case FCmpInst::FCMP_OGT  : Out << "FCmpInst::FCMP_OGT"; break;
      case FCmpInst::FCMP_OGE  : Out << "FCmpInst::FCMP_OGE"; break;
      case FCmpInst::FCMP_OLT  : Out << "FCmpInst::FCMP_OLT"; break;
      case FCmpInst::FCMP_OLE  : Out << "FCmpInst::FCMP_OLE"; break;
      case FCmpInst::FCMP_ONE  : Out << "FCmpInst::FCMP_ONE"; break;
      case FCmpInst::FCMP_ORD  : Out << "FCmpInst::FCMP_ORD"; break;
      case FCmpInst::FCMP_UNO  : Out << "FCmpInst::FCMP_UNO"; break;
      case FCmpInst::FCMP_UEQ  : Out << "FCmpInst::FCMP_UEQ"; break;
      case FCmpInst::FCMP_UGT  : Out << "FCmpInst::FCMP_UGT"; break;
      case FCmpInst::FCMP_UGE  : Out << "FCmpInst::FCMP_UGE"; break;
      case FCmpInst::FCMP_ULT  : Out << "FCmpInst::FCMP_ULT"; break;
      case FCmpInst::FCMP_ULE  : Out << "FCmpInst::FCMP_ULE"; break;
      case FCmpInst::FCMP_UNE  : Out << "FCmpInst::FCMP_UNE"; break;
      case FCmpInst::FCMP_TRUE : Out << "FCmpInst::FCMP_TRUE"; break;
      default: Out << "FCmpInst::BAD_ICMP_PREDICATE"; break;
      }
      Out << ", " << opNames[0] << ", " << opNames[1] << ", \"";
      printEscapedString(I->getName());
      Out << "\", " << bbname << ");";
      break;
    }
    case Instruction::ICmp: {
      Out << "ICmpInst* " << iName << " = new ICmpInst(";
      switch (cast<ICmpInst>(I)->getPredicate()) {
      case ICmpInst::ICMP_EQ:  Out << "ICmpInst::ICMP_EQ";  break;
      case ICmpInst::ICMP_NE:  Out << "ICmpInst::ICMP_NE";  break;
      case ICmpInst::ICMP_ULE: Out << "ICmpInst::ICMP_ULE"; break;
      case ICmpInst::ICMP_SLE: Out << "ICmpInst::ICMP_SLE"; break;
      case ICmpInst::ICMP_UGE: Out << "ICmpInst::ICMP_UGE"; break;
      case ICmpInst::ICMP_SGE: Out << "ICmpInst::ICMP_SGE"; break;
      case ICmpInst::ICMP_ULT: Out << "ICmpInst::ICMP_ULT"; break;
      case ICmpInst::ICMP_SLT: Out << "ICmpInst::ICMP_SLT"; break;
      case ICmpInst::ICMP_UGT: Out << "ICmpInst::ICMP_UGT"; break;
      case ICmpInst::ICMP_SGT: Out << "ICmpInst::ICMP_SGT"; break;
      default: Out << "ICmpInst::BAD_ICMP_PREDICATE"; break;
      }
      Out << ", " << opNames[0] << ", " << opNames[1] << ", \"";
      printEscapedString(I->getName());
      Out << "\", " << bbname << ");";
      break;
    }
    case Instruction::Malloc: {
      const MallocInst* mallocI = cast<MallocInst>(I);
      Out << "MallocInst* " << iName << " = new MallocInst("
          << getCppName(mallocI->getAllocatedType()) << ", ";
      if (mallocI->isArrayAllocation())
        Out << opNames[0] << ", " ;
      Out << "\"";
      printEscapedString(mallocI->getName());
      Out << "\", " << bbname << ");";
      if (mallocI->getAlignment())
        nl(Out) << iName << "->setAlignment("
            << mallocI->getAlignment() << ");";
      break;
    }
    case Instruction::Free: {
      Out << "FreeInst* " << iName << " = new FreeInst("
          << getCppName(I->getOperand(0)) << ", " << bbname << ");";
      break;
    }
    case Instruction::Alloca: {
      const AllocaInst* allocaI = cast<AllocaInst>(I);
      Out << "AllocaInst* " << iName << " = new AllocaInst("
          << getCppName(allocaI->getAllocatedType()) << ", ";
      if (allocaI->isArrayAllocation())
        Out << opNames[0] << ", ";
      Out << "\"";
      printEscapedString(allocaI->getName());
      Out << "\", " << bbname << ");";
      if (allocaI->getAlignment())
        nl(Out) << iName << "->setAlignment("
            << allocaI->getAlignment() << ");";
      break;
    }
    case Instruction::Load:{
      const LoadInst* load = cast<LoadInst>(I);
      Out << "LoadInst* " << iName << " = new LoadInst("
          << opNames[0] << ", \"";
      printEscapedString(load->getName());
      Out << "\", " << (load->isVolatile() ? "true" : "false" )
          << ", " << bbname << ");";
      break;
    }
    case Instruction::Store: {
      const StoreInst* store = cast<StoreInst>(I);
      Out << " new StoreInst("
          << opNames[0] << ", "
          << opNames[1] << ", "
          << (store->isVolatile() ? "true" : "false")
          << ", " << bbname << ");";
      break;
    }
    case Instruction::GetElementPtr: {
      const GetElementPtrInst* gep = cast<GetElementPtrInst>(I);
      if (gep->getNumOperands() <= 2) {
        Out << "GetElementPtrInst* " << iName << " = GetElementPtrInst::Create("
            << opNames[0];
        if (gep->getNumOperands() == 2)
          Out << ", " << opNames[1];
      } else {
        Out << "std::vector<Value*> " << iName << "_indices;";
        nl(Out);
        for (unsigned i = 1; i < gep->getNumOperands(); ++i ) {
          Out << iName << "_indices.push_back("
              << opNames[i] << ");";
          nl(Out);
        }
        Out << "Instruction* " << iName << " = GetElementPtrInst::Create("
            << opNames[0] << ", " << iName << "_indices.begin(), "
            << iName << "_indices.end()";
      }
      Out << ", \"";
      printEscapedString(gep->getName());
      Out << "\", " << bbname << ");";
      break;
    }
    case Instruction::PHI: {
      const PHINode* phi = cast<PHINode>(I);

      Out << "PHINode* " << iName << " = PHINode::Create("
          << getCppName(phi->getType()) << ", \"";
      printEscapedString(phi->getName());
      Out << "\", " << bbname << ");";
      nl(Out) << iName << "->reserveOperandSpace("
        << phi->getNumIncomingValues()
          << ");";
      nl(Out);
      for (unsigned i = 0; i < phi->getNumOperands(); i+=2) {
        Out << iName << "->addIncoming("
            << opNames[i] << ", " << opNames[i+1] << ");";
        nl(Out);
      }
      break;
    }
    case Instruction::Trunc:
    case Instruction::ZExt:
    case Instruction::SExt:
    case Instruction::FPTrunc:
    case Instruction::FPExt:
    case Instruction::FPToUI:
    case Instruction::FPToSI:
    case Instruction::UIToFP:
    case Instruction::SIToFP:
    case Instruction::PtrToInt:
    case Instruction::IntToPtr:
    case Instruction::BitCast: {
      const CastInst* cst = cast<CastInst>(I);
      Out << "CastInst* " << iName << " = new ";
      switch (I->getOpcode()) {
      case Instruction::Trunc:    Out << "TruncInst"; break;
      case Instruction::ZExt:     Out << "ZExtInst"; break;
      case Instruction::SExt:     Out << "SExtInst"; break;
      case Instruction::FPTrunc:  Out << "FPTruncInst"; break;
      case Instruction::FPExt:    Out << "FPExtInst"; break;
      case Instruction::FPToUI:   Out << "FPToUIInst"; break;
      case Instruction::FPToSI:   Out << "FPToSIInst"; break;
      case Instruction::UIToFP:   Out << "UIToFPInst"; break;
      case Instruction::SIToFP:   Out << "SIToFPInst"; break;
      case Instruction::PtrToInt: Out << "PtrToIntInst"; break;
      case Instruction::IntToPtr: Out << "IntToPtrInst"; break;
      case Instruction::BitCast:  Out << "BitCastInst"; break;
      default: assert(!"Unreachable"); break;
      }
      Out << "(" << opNames[0] << ", "
          << getCppName(cst->getType()) << ", \"";
      printEscapedString(cst->getName());
      Out << "\", " << bbname << ");";
      break;
    }
    case Instruction::Call:{
      const CallInst* call = cast<CallInst>(I);
      if (InlineAsm* ila = dyn_cast<InlineAsm>(call->getOperand(0))) {
        Out << "InlineAsm* " << getCppName(ila) << " = InlineAsm::get("
            << getCppName(ila->getFunctionType()) << ", \""
            << ila->getAsmString() << "\", \""
            << ila->getConstraintString() << "\","
            << (ila->hasSideEffects() ? "true" : "false") << ");";
        nl(Out);
      }
      if (call->getNumOperands() > 2) {
        Out << "std::vector<Value*> " << iName << "_params;";
        nl(Out);
        for (unsigned i = 1; i < call->getNumOperands(); ++i) {
          Out << iName << "_params.push_back(" << opNames[i] << ");";
          nl(Out);
        }
        Out << "CallInst* " << iName << " = CallInst::Create("
            << opNames[0] << ", " << iName << "_params.begin(), "
            << iName << "_params.end(), \"";
      } else if (call->getNumOperands() == 2) {
        Out << "CallInst* " << iName << " = CallInst::Create("
            << opNames[0] << ", " << opNames[1] << ", \"";
      } else {
        Out << "CallInst* " << iName << " = CallInst::Create(" << opNames[0]
            << ", \"";
      }
      printEscapedString(call->getName());
      Out << "\", " << bbname << ");";
      nl(Out) << iName << "->setCallingConv(";
      printCallingConv(call->getCallingConv());
      Out << ");";
      nl(Out) << iName << "->setTailCall("
          << (call->isTailCall() ? "true":"false");
      Out << ");";
      printAttributes(call->getAttributes(), iName);
      Out << iName << "->setAttributes(" << iName << "_PAL);";
      nl(Out);
      break;
    }
    case Instruction::Select: {
      const SelectInst* sel = cast<SelectInst>(I);
      Out << "SelectInst* " << getCppName(sel) << " = SelectInst::Create(";
      Out << opNames[0] << ", " << opNames[1] << ", " << opNames[2] << ", \"";
      printEscapedString(sel->getName());
      Out << "\", " << bbname << ");";
      break;
    }
    case Instruction::UserOp1:
      /// FALL THROUGH
    case Instruction::UserOp2: {
      /// FIXME: What should be done here?
      break;
    }
    case Instruction::VAArg: {
      const VAArgInst* va = cast<VAArgInst>(I);
      Out << "VAArgInst* " << getCppName(va) << " = new VAArgInst("
          << opNames[0] << ", " << getCppName(va->getType()) << ", \"";
      printEscapedString(va->getName());
      Out << "\", " << bbname << ");";
      break;
    }
    case Instruction::ExtractElement: {
      const ExtractElementInst* eei = cast<ExtractElementInst>(I);
      Out << "ExtractElementInst* " << getCppName(eei)
          << " = new ExtractElementInst(" << opNames[0]
          << ", " << opNames[1] << ", \"";
      printEscapedString(eei->getName());
      Out << "\", " << bbname << ");";
      break;
    }
    case Instruction::InsertElement: {
      const InsertElementInst* iei = cast<InsertElementInst>(I);
      Out << "InsertElementInst* " << getCppName(iei)
          << " = InsertElementInst::Create(" << opNames[0]
          << ", " << opNames[1] << ", " << opNames[2] << ", \"";
      printEscapedString(iei->getName());
      Out << "\", " << bbname << ");";
      break;
    }
    case Instruction::ShuffleVector: {
      const ShuffleVectorInst* svi = cast<ShuffleVectorInst>(I);
      Out << "ShuffleVectorInst* " << getCppName(svi)
          << " = new ShuffleVectorInst(" << opNames[0]
          << ", " << opNames[1] << ", " << opNames[2] << ", \"";
      printEscapedString(svi->getName());
      Out << "\", " << bbname << ");";
      break;
    }
    case Instruction::ExtractValue: {
      const ExtractValueInst *evi = cast<ExtractValueInst>(I);
      Out << "std::vector<unsigned> " << iName << "_indices;";
      nl(Out);
      for (unsigned i = 0; i < evi->getNumIndices(); ++i) {
        Out << iName << "_indices.push_back("
            << evi->idx_begin()[i] << ");";
        nl(Out);
      }
      Out << "ExtractValueInst* " << getCppName(evi)
          << " = ExtractValueInst::Create(" << opNames[0]
          << ", "
          << iName << "_indices.begin(), " << iName << "_indices.end(), \"";
      printEscapedString(evi->getName());
      Out << "\", " << bbname << ");";
      break;
    }
    case Instruction::InsertValue: {
      const InsertValueInst *ivi = cast<InsertValueInst>(I);
      Out << "std::vector<unsigned> " << iName << "_indices;";
      nl(Out);
      for (unsigned i = 0; i < ivi->getNumIndices(); ++i) {
        Out << iName << "_indices.push_back("
            << ivi->idx_begin()[i] << ");";
        nl(Out);
      }
      Out << "InsertValueInst* " << getCppName(ivi)
          << " = InsertValueInst::Create(" << opNames[0]
          << ", " << opNames[1] << ", "
          << iName << "_indices.begin(), " << iName << "_indices.end(), \"";
      printEscapedString(ivi->getName());
      Out << "\", " << bbname << ");";
      break;
    }
  }
  DefinedValues.insert(I);
  nl(Out);
  delete [] opNames;
}

  // Print out the types, constants and declarations needed by one function
  void CppWriter::printFunctionUses(const Function* F) {
    nl(Out) << "// Type Definitions"; nl(Out);
    if (!is_inline) {
      // Print the function's return type
      printType(F->getReturnType());

      // Print the function's function type
      printType(F->getFunctionType());

      // Print the types of each of the function's arguments
      for (Function::const_arg_iterator AI = F->arg_begin(), AE = F->arg_end();
           AI != AE; ++AI) {
        printType(AI->getType());
      }
    }

    // Print type definitions for every type referenced by an instruction and
    // make a note of any global values or constants that are referenced
    SmallPtrSet<GlobalValue*,64> gvs;
    SmallPtrSet<Constant*,64> consts;
    for (Function::const_iterator BB = F->begin(), BE = F->end();
         BB != BE; ++BB){
      for (BasicBlock::const_iterator I = BB->begin(), E = BB->end();
           I != E; ++I) {
        // Print the type of the instruction itself
        printType(I->getType());

        // Print the type of each of the instruction's operands
        for (unsigned i = 0; i < I->getNumOperands(); ++i) {
          Value* operand = I->getOperand(i);
          printType(operand->getType());

          // If the operand references a GVal or Constant, make a note of it
          if (GlobalValue* GV = dyn_cast<GlobalValue>(operand)) {
            gvs.insert(GV);
            if (GlobalVariable *GVar = dyn_cast<GlobalVariable>(GV))
              if (GVar->hasInitializer())
                consts.insert(GVar->getInitializer());
          } else if (Constant* C = dyn_cast<Constant>(operand))
            consts.insert(C);
        }
      }
    }

    // Print the function declarations for any functions encountered
    nl(Out) << "// Function Declarations"; nl(Out);
    for (SmallPtrSet<GlobalValue*,64>::iterator I = gvs.begin(), E = gvs.end();
         I != E; ++I) {
      if (Function* Fun = dyn_cast<Function>(*I)) {
        if (!is_inline || Fun != F)
          printFunctionHead(Fun);
      }
    }

    // Print the global variable declarations for any variables encountered
    nl(Out) << "// Global Variable Declarations"; nl(Out);
    for (SmallPtrSet<GlobalValue*,64>::iterator I = gvs.begin(), E = gvs.end();
         I != E; ++I) {
      if (GlobalVariable* F = dyn_cast<GlobalVariable>(*I))
        printVariableHead(F);
    }

  // Print the constants found
    nl(Out) << "// Constant Definitions"; nl(Out);
    for (SmallPtrSet<Constant*,64>::iterator I = consts.begin(),
           E = consts.end(); I != E; ++I) {
      printConstant(*I);
    }

    // Process the global variables definitions now that all the constants have
    // been emitted. These definitions just couple the gvars with their constant
    // initializers.
    nl(Out) << "// Global Variable Definitions"; nl(Out);
    for (SmallPtrSet<GlobalValue*,64>::iterator I = gvs.begin(), E = gvs.end();
         I != E; ++I) {
      if (GlobalVariable* GV = dyn_cast<GlobalVariable>(*I))
        printVariableBody(GV);
    }
  }

  void CppWriter::printFunctionHead(const Function* F) {
    nl(Out) << "Function* " << getCppName(F);
    if (is_inline) {
      Out << " = mod->getFunction(\"";
      printEscapedString(F->getName());
      Out << "\", " << getCppName(F->getFunctionType()) << ");";
      nl(Out) << "if (!" << getCppName(F) << ") {";
      nl(Out) << getCppName(F);
    }
    Out<< " = Function::Create(";
    nl(Out,1) << "/*Type=*/" << getCppName(F->getFunctionType()) << ",";
    nl(Out) << "/*Linkage=*/";
    printLinkageType(F->getLinkage());
    Out << ",";
    nl(Out) << "/*Name=*/\"";
    printEscapedString(F->getName());
    Out << "\", mod); " << (F->isDeclaration()? "// (external, no body)" : "");
    nl(Out,-1);
    printCppName(F);
    Out << "->setCallingConv(";
    printCallingConv(F->getCallingConv());
    Out << ");";
    nl(Out);
    if (F->hasSection()) {
      printCppName(F);
      Out << "->setSection(\"" << F->getSection() << "\");";
      nl(Out);
    }
    if (F->getAlignment()) {
      printCppName(F);
      Out << "->setAlignment(" << F->getAlignment() << ");";
      nl(Out);
    }
    if (F->getVisibility() != GlobalValue::DefaultVisibility) {
      printCppName(F);
      Out << "->setVisibility(";
      printVisibilityType(F->getVisibility());
      Out << ");";
      nl(Out);
    }
    if (F->hasGC()) {
      printCppName(F);
      Out << "->setGC(\"" << F->getGC() << "\");";
      nl(Out);
    }
    if (is_inline) {
      Out << "}";
      nl(Out);
    }
    printAttributes(F->getAttributes(), getCppName(F));
    printCppName(F);
    Out << "->setAttributes(" << getCppName(F) << "_PAL);";
    nl(Out);
  }

  void CppWriter::printFunctionBody(const Function *F) {
    if (F->isDeclaration())
      return; // external functions have no bodies.

    // Clear the DefinedValues and ForwardRefs maps because we can't have
    // cross-function forward refs
    ForwardRefs.clear();
    DefinedValues.clear();

    // Create all the argument values
    if (!is_inline) {
      if (!F->arg_empty()) {
        Out << "Function::arg_iterator args = " << getCppName(F)
            << "->arg_begin();";
        nl(Out);
      }
      for (Function::const_arg_iterator AI = F->arg_begin(), AE = F->arg_end();
           AI != AE; ++AI) {
        Out << "Value* " << getCppName(AI) << " = args++;";
        nl(Out);
        if (AI->hasName()) {
          Out << getCppName(AI) << "->setName(\"" << AI->getName() << "\");";
          nl(Out);
        }
      }
    }

    // Create all the basic blocks
    nl(Out);
    for (Function::const_iterator BI = F->begin(), BE = F->end();
         BI != BE; ++BI) {
      std::string bbname(getCppName(BI));
      Out << "BasicBlock* " << bbname << " = BasicBlock::Create(\"";
      if (BI->hasName())
        printEscapedString(BI->getName());
      Out << "\"," << getCppName(BI->getParent()) << ",0);";
      nl(Out);
    }

    // Output all of its basic blocks... for the function
    for (Function::const_iterator BI = F->begin(), BE = F->end();
         BI != BE; ++BI) {
      std::string bbname(getCppName(BI));
      nl(Out) << "// Block " << BI->getName() << " (" << bbname << ")";
      nl(Out);

      // Output all of the instructions in the basic block...
      for (BasicBlock::const_iterator I = BI->begin(), E = BI->end();
           I != E; ++I) {
        printInstruction(I,bbname);
      }
    }

    // Loop over the ForwardRefs and resolve them now that all instructions
    // are generated.
    if (!ForwardRefs.empty()) {
      nl(Out) << "// Resolve Forward References";
      nl(Out);
    }

    while (!ForwardRefs.empty()) {
      ForwardRefMap::iterator I = ForwardRefs.begin();
      Out << I->second << "->replaceAllUsesWith("
          << getCppName(I->first) << "); delete " << I->second << ";";
      nl(Out);
      ForwardRefs.erase(I);
    }
  }

  void CppWriter::printInline(const std::string& fname,
                              const std::string& func) {
    const Function* F = TheModule->getFunction(func);
    if (!F) {
      error(std::string("Function '") + func + "' not found in input module");
      return;
    }
    if (F->isDeclaration()) {
      error(std::string("Function '") + func + "' is external!");
      return;
    }
    nl(Out) << "BasicBlock* " << fname << "(Module* mod, Function *"
            << getCppName(F);
    unsigned arg_count = 1;
    for (Function::const_arg_iterator AI = F->arg_begin(), AE = F->arg_end();
         AI != AE; ++AI) {
      Out << ", Value* arg_" << arg_count;
    }
    Out << ") {";
    nl(Out);
    is_inline = true;
    printFunctionUses(F);
    printFunctionBody(F);
    is_inline = false;
    Out << "return " << getCppName(F->begin()) << ";";
    nl(Out) << "}";
    nl(Out);
  }

  void CppWriter::printModuleBody() {
    // Print out all the type definitions
    nl(Out) << "// Type Definitions"; nl(Out);
    printTypes(TheModule);

    // Functions can call each other and global variables can reference them so
    // define all the functions first before emitting their function bodies.
    nl(Out) << "// Function Declarations"; nl(Out);
    for (Module::const_iterator I = TheModule->begin(), E = TheModule->end();
         I != E; ++I)
      printFunctionHead(I);

    // Process the global variables declarations. We can't initialze them until
    // after the constants are printed so just print a header for each global
    nl(Out) << "// Global Variable Declarations\n"; nl(Out);
    for (Module::const_global_iterator I = TheModule->global_begin(),
           E = TheModule->global_end(); I != E; ++I) {
      printVariableHead(I);
    }

    // Print out all the constants definitions. Constants don't recurse except
    // through GlobalValues. All GlobalValues have been declared at this point
    // so we can proceed to generate the constants.
    nl(Out) << "// Constant Definitions"; nl(Out);
    printConstants(TheModule);

    // Process the global variables definitions now that all the constants have
    // been emitted. These definitions just couple the gvars with their constant
    // initializers.
    nl(Out) << "// Global Variable Definitions"; nl(Out);
    for (Module::const_global_iterator I = TheModule->global_begin(),
           E = TheModule->global_end(); I != E; ++I) {
      printVariableBody(I);
    }

    // Finally, we can safely put out all of the function bodies.
    nl(Out) << "// Function Definitions"; nl(Out);
    for (Module::const_iterator I = TheModule->begin(), E = TheModule->end();
         I != E; ++I) {
      if (!I->isDeclaration()) {
        nl(Out) << "// Function: " << I->getName() << " (" << getCppName(I)
                << ")";
        nl(Out) << "{";
        nl(Out,1);
        printFunctionBody(I);
        nl(Out,-1) << "}";
        nl(Out);
      }
    }
  }

  void CppWriter::printProgram(const std::string& fname,
                               const std::string& mName) {
    Out << "#include <llvm/Module.h>\n";
    Out << "#include <llvm/DerivedTypes.h>\n";
    Out << "#include <llvm/Constants.h>\n";
    Out << "#include <llvm/GlobalVariable.h>\n";
    Out << "#include <llvm/Function.h>\n";
    Out << "#include <llvm/CallingConv.h>\n";
    Out << "#include <llvm/BasicBlock.h>\n";
    Out << "#include <llvm/Instructions.h>\n";
    Out << "#include <llvm/InlineAsm.h>\n";
    Out << "#include <llvm/Support/MathExtras.h>\n";
    Out << "#include <llvm/Support/raw_ostream.h>\n";
    Out << "#include <llvm/Pass.h>\n";
    Out << "#include <llvm/PassManager.h>\n";
    Out << "#include <llvm/ADT/SmallVector.h>\n";
    Out << "#include <llvm/Analysis/Verifier.h>\n";
    Out << "#include <llvm/Assembly/PrintModulePass.h>\n";
    Out << "#include <algorithm>\n";
    Out << "using namespace llvm;\n\n";
    Out << "Module* " << fname << "();\n\n";
    Out << "int main(int argc, char**argv) {\n";
    Out << "  Module* Mod = " << fname << "();\n";
    Out << "  verifyModule(*Mod, PrintMessageAction);\n";
    Out << "  errs().flush();\n";
    Out << "  outs().flush();\n";
    Out << "  PassManager PM;\n";
    Out << "  PM.add(createPrintModulePass(&outs()));\n";
    Out << "  PM.run(*Mod);\n";
    Out << "  return 0;\n";
    Out << "}\n\n";
    printModule(fname,mName);
  }

  void CppWriter::printModule(const std::string& fname,
                              const std::string& mName) {
    nl(Out) << "Module* " << fname << "() {";
    nl(Out,1) << "// Module Construction";
    nl(Out) << "Module* mod = new Module(\"" << mName << "\");";
    if (!TheModule->getTargetTriple().empty()) {
      nl(Out) << "mod->setDataLayout(\"" << TheModule->getDataLayout() << "\");";
    }
    if (!TheModule->getTargetTriple().empty()) {
      nl(Out) << "mod->setTargetTriple(\"" << TheModule->getTargetTriple()
              << "\");";
    }

    if (!TheModule->getModuleInlineAsm().empty()) {
      nl(Out) << "mod->setModuleInlineAsm(\"";
      printEscapedString(TheModule->getModuleInlineAsm());
      Out << "\");";
    }
    nl(Out);

    // Loop over the dependent libraries and emit them.
    Module::lib_iterator LI = TheModule->lib_begin();
    Module::lib_iterator LE = TheModule->lib_end();
    while (LI != LE) {
      Out << "mod->addLibrary(\"" << *LI << "\");";
      nl(Out);
      ++LI;
    }
    printModuleBody();
    nl(Out) << "return mod;";
    nl(Out,-1) << "}";
    nl(Out);
  }

  void CppWriter::printContents(const std::string& fname,
                                const std::string& mName) {
    Out << "\nModule* " << fname << "(Module *mod) {\n";
    Out << "\nmod->setModuleIdentifier(\"" << mName << "\");\n";
    printModuleBody();
    Out << "\nreturn mod;\n";
    Out << "\n}\n";
  }

  void CppWriter::printFunction(const std::string& fname,
                                const std::string& funcName) {
    const Function* F = TheModule->getFunction(funcName);
    if (!F) {
      error(std::string("Function '") + funcName + "' not found in input module");
      return;
    }
    Out << "\nFunction* " << fname << "(Module *mod) {\n";
    printFunctionUses(F);
    printFunctionHead(F);
    printFunctionBody(F);
    Out << "return " << getCppName(F) << ";\n";
    Out << "}\n";
  }

  void CppWriter::printFunctions() {
    const Module::FunctionListType &funcs = TheModule->getFunctionList();
    Module::const_iterator I  = funcs.begin();
    Module::const_iterator IE = funcs.end();

    for (; I != IE; ++I) {
      const Function &func = *I;
      if (!func.isDeclaration()) {
        std::string name("define_");
        name += func.getName();
        printFunction(name, func.getName());
      }
    }
  }

  void CppWriter::printVariable(const std::string& fname,
                                const std::string& varName) {
    const GlobalVariable* GV = TheModule->getNamedGlobal(varName);

    if (!GV) {
      error(std::string("Variable '") + varName + "' not found in input module");
      return;
    }
    Out << "\nGlobalVariable* " << fname << "(Module *mod) {\n";
    printVariableUses(GV);
    printVariableHead(GV);
    printVariableBody(GV);
    Out << "return " << getCppName(GV) << ";\n";
    Out << "}\n";
  }

  void CppWriter::printType(const std::string& fname,
                            const std::string& typeName) {
    const Type* Ty = TheModule->getTypeByName(typeName);
    if (!Ty) {
      error(std::string("Type '") + typeName + "' not found in input module");
      return;
    }
    Out << "\nType* " << fname << "(Module *mod) {\n";
    printType(Ty);
    Out << "return " << getCppName(Ty) << ";\n";
    Out << "}\n";
  }

  bool CppWriter::runOnModule(Module &M) {
    TheModule = &M;

    // Emit a header
    Out << "// Generated by llvm2cpp - DO NOT MODIFY!\n\n";

    // Get the name of the function we're supposed to generate
    std::string fname = FuncName.getValue();

    // Get the name of the thing we are to generate
    std::string tgtname = NameToGenerate.getValue();
    if (GenerationType == GenModule ||
        GenerationType == GenContents ||
        GenerationType == GenProgram ||
        GenerationType == GenFunctions) {
      if (tgtname == "!bad!") {
        if (M.getModuleIdentifier() == "-")
          tgtname = "<stdin>";
        else
          tgtname = M.getModuleIdentifier();
      }
    } else if (tgtname == "!bad!")
      error("You must use the -for option with -gen-{function,variable,type}");

    switch (WhatToGenerate(GenerationType)) {
     case GenProgram:
      if (fname.empty())
        fname = "makeLLVMModule";
      printProgram(fname,tgtname);
      break;
     case GenModule:
      if (fname.empty())
        fname = "makeLLVMModule";
      printModule(fname,tgtname);
      break;
     case GenContents:
      if (fname.empty())
        fname = "makeLLVMModuleContents";
      printContents(fname,tgtname);
      break;
     case GenFunction:
      if (fname.empty())
        fname = "makeLLVMFunction";
      printFunction(fname,tgtname);
      break;
     case GenFunctions:
      printFunctions();
      break;
     case GenInline:
      if (fname.empty())
        fname = "makeLLVMInline";
      printInline(fname,tgtname);
      break;
     case GenVariable:
      if (fname.empty())
        fname = "makeLLVMVariable";
      printVariable(fname,tgtname);
      break;
     case GenType:
      if (fname.empty())
        fname = "makeLLVMType";
      printType(fname,tgtname);
      break;
     default:
      error("Invalid generation option");
    }

    return false;
  }
}

char CppWriter::ID = 0;

//===----------------------------------------------------------------------===//
//                       External Interface declaration
//===----------------------------------------------------------------------===//

bool CPPTargetMachine::addPassesToEmitWholeFile(PassManager &PM,
                                                raw_ostream &o,
                                                CodeGenFileType FileType,
                                                bool Fast) {
  if (FileType != TargetMachine::AssemblyFile) return true;
  PM.add(new CppWriter(o));
  return false;
}
