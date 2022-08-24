/*
 * Copyright 2020 Google LLC.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/sksl/SkSLRehydrator.h"

#include "include/private/SkSLModifiers.h"
#include "include/private/SkSLProgramElement.h"
#include "include/private/SkSLProgramKind.h"
#include "include/private/SkSLStatement.h"
#include "include/private/SkSLSymbol.h"
#include "include/private/SkTArray.h"
#include "include/sksl/DSLCore.h"
#include "include/sksl/SkSLOperator.h"
#include "include/sksl/SkSLPosition.h"
#include "include/sksl/SkSLVersion.h"
#include "src/sksl/SkSLAnalysis.h"
#include "src/sksl/SkSLCompiler.h"
#include "src/sksl/SkSLModifiersPool.h"
#include "src/sksl/SkSLParsedModule.h"
#include "src/sksl/SkSLPool.h"
#include "src/sksl/SkSLProgramSettings.h"
#include "src/sksl/SkSLThreadContext.h"
#include "src/sksl/ir/SkSLBinaryExpression.h"
#include "src/sksl/ir/SkSLBlock.h"
#include "src/sksl/ir/SkSLBreakStatement.h"
#include "src/sksl/ir/SkSLConstructorArray.h"
#include "src/sksl/ir/SkSLConstructorArrayCast.h"
#include "src/sksl/ir/SkSLConstructorCompound.h"
#include "src/sksl/ir/SkSLConstructorCompoundCast.h"
#include "src/sksl/ir/SkSLConstructorDiagonalMatrix.h"
#include "src/sksl/ir/SkSLConstructorMatrixResize.h"
#include "src/sksl/ir/SkSLConstructorScalarCast.h"
#include "src/sksl/ir/SkSLConstructorSplat.h"
#include "src/sksl/ir/SkSLConstructorStruct.h"
#include "src/sksl/ir/SkSLContinueStatement.h"
#include "src/sksl/ir/SkSLDiscardStatement.h"
#include "src/sksl/ir/SkSLDoStatement.h"
#include "src/sksl/ir/SkSLExpression.h"
#include "src/sksl/ir/SkSLExpressionStatement.h"
#include "src/sksl/ir/SkSLField.h"
#include "src/sksl/ir/SkSLFieldAccess.h"
#include "src/sksl/ir/SkSLForStatement.h"
#include "src/sksl/ir/SkSLFunctionCall.h"
#include "src/sksl/ir/SkSLFunctionDeclaration.h"
#include "src/sksl/ir/SkSLFunctionDefinition.h"
#include "src/sksl/ir/SkSLFunctionPrototype.h"
#include "src/sksl/ir/SkSLIfStatement.h"
#include "src/sksl/ir/SkSLIndexExpression.h"
#include "src/sksl/ir/SkSLInterfaceBlock.h"
#include "src/sksl/ir/SkSLLiteral.h"
#include "src/sksl/ir/SkSLNop.h"
#include "src/sksl/ir/SkSLPostfixExpression.h"
#include "src/sksl/ir/SkSLPrefixExpression.h"
#include "src/sksl/ir/SkSLProgram.h"
#include "src/sksl/ir/SkSLReturnStatement.h"
#include "src/sksl/ir/SkSLSetting.h"
#include "src/sksl/ir/SkSLStructDefinition.h"
#include "src/sksl/ir/SkSLSwitchCase.h"
#include "src/sksl/ir/SkSLSwitchStatement.h"
#include "src/sksl/ir/SkSLSwizzle.h"
#include "src/sksl/ir/SkSLSymbolTable.h"
#include "src/sksl/ir/SkSLTernaryExpression.h"
#include "src/sksl/ir/SkSLType.h"
#include "src/sksl/ir/SkSLVarDeclarations.h"
#include "src/sksl/ir/SkSLVariable.h"
#include "src/sksl/ir/SkSLVariableReference.h"

#include <stdio.h>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#define SYMBOL_DEBUGF(...) //SkDebugf(__VA_ARGS__)

namespace SkSL {

class AutoRehydratorSymbolTable {
public:
    AutoRehydratorSymbolTable(Rehydrator* rehydrator)
        : fRehydrator(rehydrator)
        , fOldSymbols(fRehydrator->fSymbolTable) {
        std::shared_ptr<SymbolTable> symbols = fRehydrator->symbolTable();
        if (symbols) {
            fRehydrator->fSymbolTable = std::move(symbols);
        }
    }

    ~AutoRehydratorSymbolTable() {
        fRehydrator->fSymbolTable = std::move(fOldSymbols);
    }

private:
    Rehydrator* fRehydrator;
    std::shared_ptr<SymbolTable> fOldSymbols;
};

Rehydrator::Rehydrator(Compiler& compiler, const uint8_t* src, size_t length)
        : Rehydrator(compiler, src, length, compiler.makeRootSymbolTableWithPublicTypes()) {}

Rehydrator::Rehydrator(Compiler& compiler, const uint8_t* src, size_t length,
                       std::shared_ptr<SymbolTable> symbols)
    : fCompiler(compiler)
    , fSymbolTable(std::move(symbols))
    SkDEBUGCODE(, fEnd(src + length)) {
    SkASSERT(fSymbolTable);
    SkASSERT(fSymbolTable->isBuiltin());
    fIP = src;
    [[maybe_unused]] uint16_t version = this->readU16();
    SkASSERTF(version == kVersion, "Dehydrated file is an unsupported version (current version is "
            "%d, found version %d)", kVersion, version);
    fStringStart = fIP;
    // skip over string data
    fIP += this->readU16();
}

#ifdef SK_DEBUG
Rehydrator::~Rehydrator() {
    // ensure that we have read the expected number of bytes
    SkASSERT(fIP == fEnd);
}
#endif

Context& Rehydrator::context() const {
    return fCompiler.context();
}

Layout Rehydrator::layout() {
    switch (this->readU8()) {
        case kBuiltinLayout_Command: {
            Layout result;
            result.fBuiltin = this->readS16();
            return result;
        }
        case kDefaultLayout_Command:
            return Layout();
        case kLayout_Command: {
            int flags = this->readU32();
            int location = this->readS8();
            int offset = this->readS16();
            int binding = this->readS16();
            int index = this->readS8();
            int set = this->readS8();
            int builtin = this->readS16();
            int inputAttachmentIndex = this->readS8();
            return Layout(
                    flags, location, offset, binding, index, set, builtin, inputAttachmentIndex);
        }
        default:
            SkASSERT(false);
            return Layout();
    }
}

Modifiers Rehydrator::modifiers() {
    switch (this->readU8()) {
        case kDefaultModifiers_Command:
            return Modifiers();
        case kModifiers8Bit_Command: {
            Layout l = this->layout();
            int flags = this->readU8();
            return Modifiers(l, flags);
        }
        case kModifiers_Command: {
            Layout l = this->layout();
            int flags = this->readS32();
            return Modifiers(l, flags);
        }
        default:
            SkASSERT(false);
            return Modifiers();
    }
}

Symbol* Rehydrator::symbol() {
    int kind = this->readU8();
    switch (kind) {
        case kArrayType_Command: {
            uint16_t id = this->readU16();
            const Type* componentType = this->type();
            int8_t count = this->readS8();
            const std::string* arrayName =
                    fSymbolTable->takeOwnershipOfString(componentType->getArrayName(count));
            Type* result = fSymbolTable->takeOwnershipOfSymbol(
                    Type::MakeArrayType(*arrayName, *componentType, count));
            this->addSymbol(id, result);
            return result;
        }
        case kFunctionDeclaration_Command: {
            uint16_t id = this->readU16();
            Modifiers modifiers = this->modifiers();
            std::string_view name = this->readString();
            int parameterCount = this->readU8();
            std::vector<const Variable*> parameters;
            parameters.reserve(parameterCount);
            for (int i = 0; i < parameterCount; ++i) {
                parameters.push_back(&this->symbol()->as<Variable>());
            }
            const Type* returnType = this->type();
            auto decl = std::make_unique<FunctionDeclaration>(Position(),
                                                              this->modifiersPool().add(modifiers),
                                                              name,
                                                              std::move(parameters),
                                                              returnType,
                                                              fSymbolTable->isBuiltin());
            FunctionDeclaration* sym = fSymbolTable->takeOwnershipOfSymbol(std::move(decl));
            this->addSymbol(id, sym);
            return sym;
        }
        case kField_Command: {
            const Variable* owner = this->symbolRef<Variable>();
            uint8_t index = this->readU8();
            Field* result = fSymbolTable->takeOwnershipOfSymbol(
                    std::make_unique<Field>(Position(), owner, index));
            return result;
        }
        case kStructType_Command: {
            uint16_t id = this->readU16();
            std::string name(this->readString());
            uint8_t fieldCount = this->readU8();
            std::vector<Type::Field> fields;
            fields.reserve(fieldCount);
            for (int i = 0; i < fieldCount; ++i) {
                Modifiers m = this->modifiers();
                std::string_view fieldName = this->readString();
                const Type* type = this->type();
                fields.emplace_back(Position(), m, fieldName, type);
            }
            bool interfaceBlock = this->readU8();
            std::string_view nameChars(*fSymbolTable->takeOwnershipOfString(std::move(name)));
            Type* result = fSymbolTable->takeOwnershipOfSymbol(
                    Type::MakeStructType(Position(), nameChars, std::move(fields), interfaceBlock));
            this->addSymbol(id, result);
            return result;
        }
        case kSymbolRef_Command: {
            return (Symbol*)this->possiblyBuiltinSymbolRef();
        }
        case kVariable_Command: {
            uint16_t id = this->readU16();
            const Modifiers* m = this->modifiersPool().add(this->modifiers());
            std::string_view name = this->readString();
            const Type* type = this->type();
            Variable::Storage storage = (Variable::Storage) this->readU8();
            Variable* result = fSymbolTable->takeOwnershipOfSymbol(std::make_unique<Variable>(
                    /*pos=*/Position(), /*modifiersPosition=*/Position(), m, name, type,
                    fSymbolTable->isBuiltin(), storage));
            this->addSymbol(id, result);
            return result;
        }
        default:
            printf("unsupported symbol %d\n", kind);
            SkASSERT(false);
            return nullptr;
    }
}

const Type* Rehydrator::type() {
    const Symbol* result = this->symbol();
    SkASSERT(result->kind() == Symbol::Kind::kType);
    return (const Type*) result;
}

std::unique_ptr<Program> Rehydrator::program() {
    [[maybe_unused]] uint8_t command = this->readU8();
    SkASSERT(command == kProgram_Command);

    // Initialize the temporary config used to generate the complete program. We explicitly avoid
    // enforcing ES2 restrictions when rehydrating a program, which we assume to be already
    // well-formed when dehydrated.
    auto config = std::make_unique<ProgramConfig>();
    config->fKind = (ProgramKind)this->readU8();
    config->fRequiredSkSLVersion = (SkSL::Version)this->readU8();
    config->fSettings.fMaxVersionAllowed = SkSL::Version::k300;

    Context& context = this->context();
    ProgramConfig* oldConfig = context.fConfig;
    ModifiersPool* oldModifiersPool = context.fModifiersPool;
    context.fConfig = config.get();
    fSymbolTable = fCompiler.moduleForProgramKind(config->fKind).fSymbols;
    dsl::Start(&fCompiler, config->fKind, config->fSettings);
    auto modifiers = std::make_unique<ModifiersPool>();
    context.fModifiersPool = modifiers.get();
    this->symbolTable();
    std::vector<std::unique_ptr<ProgramElement>> elements = this->elements();
    context.fConfig = oldConfig;
    context.fModifiersPool = oldModifiersPool;
    Program::Inputs inputs;
    inputs.fUseFlipRTUniform = this->readU8();
    std::unique_ptr<Pool> pool = std::move(ThreadContext::MemoryPool());
    pool->detachFromThread();
    std::unique_ptr<Program> result = std::make_unique<Program>(nullptr, std::move(config),
            fCompiler.fContext, std::move(elements),
            /*sharedElements=*/std::vector<const ProgramElement*>(), std::move(modifiers),
            fSymbolTable, std::move(pool), inputs);
    fSymbolTable = fSymbolTable->fParent;
    dsl::End();
    return result;
}

std::vector<std::unique_ptr<ProgramElement>> Rehydrator::elements() {
    SkDEBUGCODE(uint8_t command = )this->readU8();
    SkASSERT(command == kElements_Command);
    std::vector<std::unique_ptr<ProgramElement>> result;
    while (std::unique_ptr<ProgramElement> elem = this->element()) {
        result.push_back(std::move(elem));
    }
    return result;
}

std::unique_ptr<ProgramElement> Rehydrator::element() {
    int kind = this->readU8();
    switch (kind) {
        case Rehydrator::kFunctionDefinition_Command: {
            const FunctionDeclaration* decl = this->symbolRef<FunctionDeclaration>();
            std::unique_ptr<Statement> body = this->statement();
            auto result = FunctionDefinition::Convert(this->context(), Position(), *decl,
                                                      std::move(body), fSymbolTable->isBuiltin());
            decl->setDefinition(result.get());
            return std::move(result);
        }
        case Rehydrator::kFunctionPrototype_Command: {
            const FunctionDeclaration* decl = this->symbolRef<FunctionDeclaration>();
            // since we skip over builtin prototypes when dehydrating, we know that this
            // builtin=false
            return std::make_unique<FunctionPrototype>(Position(), decl, /*builtin=*/false);
        }
        case Rehydrator::kGlobalVar_Command: {
            std::unique_ptr<Statement> decl = this->statement();
            return std::make_unique<GlobalVarDeclaration>(std::move(decl));
        }
        case Rehydrator::kInterfaceBlock_Command: {
            const Symbol* var = this->symbol();
            SkASSERT(var && var->is<Variable>());
            std::string_view typeName = this->readString();
            std::string_view instanceName = this->readString();
            int arraySize = this->readU8();
            return std::make_unique<InterfaceBlock>(Position(), var->as<Variable>(), typeName,
                                                    instanceName, arraySize, nullptr);
        }
        case Rehydrator::kStructDefinition_Command: {
            const Symbol* type = this->symbol();
            SkASSERT(type && type->is<Type>());
            return std::make_unique<StructDefinition>(Position(), type->as<Type>());
        }
        case Rehydrator::kSharedFunction_Command: {
            int count = this->readU8();
            for (int i = 0; i < count; ++i) {
                [[maybe_unused]] const Symbol* param = this->symbol();
                SkASSERT(param->is<Variable>());
            }
            [[maybe_unused]] const Symbol* decl = this->symbol();
            SkASSERT(decl->is<FunctionDeclaration>());
            std::unique_ptr<ProgramElement> result = this->element();
            SkASSERT(result->is<FunctionDefinition>());
            return result;
        }
        case Rehydrator::kElementsComplete_Command:
            return nullptr;
        default:
            SkDEBUGFAILF("unsupported element %d\n", kind);
            return nullptr;
    }
}

std::unique_ptr<Statement> Rehydrator::statement() {
    int kind = this->readU8();
    switch (kind) {
        case Rehydrator::kBlock_Command: {
            AutoRehydratorSymbolTable symbols(this);
            int count = this->readU8();
            StatementArray statements;
            statements.reserve_back(count);
            for (int i = 0; i < count; ++i) {
                statements.push_back(this->statement());
            }
            Block::Kind blockKind = (Block::Kind)this->readU8();
            return Block::Make(Position(), std::move(statements), blockKind, fSymbolTable);
        }
        case Rehydrator::kBreak_Command:
            return BreakStatement::Make(Position());
        case Rehydrator::kContinue_Command:
            return ContinueStatement::Make(Position());
        case Rehydrator::kDiscard_Command:
            return DiscardStatement::Make(Position());
        case Rehydrator::kDo_Command: {
            std::unique_ptr<Statement> stmt = this->statement();
            std::unique_ptr<Expression> expr = this->expression();
            return DoStatement::Make(this->context(), Position(), std::move(stmt), std::move(expr));
        }
        case Rehydrator::kExpressionStatement_Command: {
            std::unique_ptr<Expression> expr = this->expression();
            return ExpressionStatement::Make(this->context(), std::move(expr));
        }
        case Rehydrator::kFor_Command: {
            AutoRehydratorSymbolTable symbols(this);
            std::unique_ptr<Statement> initializer = this->statement();
            std::unique_ptr<Expression> test = this->expression();
            std::unique_ptr<Expression> next = this->expression();
            std::unique_ptr<Statement> body = this->statement();
            std::unique_ptr<LoopUnrollInfo> unrollInfo =
                    Analysis::GetLoopUnrollInfo(Position(), ForLoopPositions{},
                        initializer.get(), test.get(), next.get(), body.get(), /*errors=*/nullptr);
            return ForStatement::Make(this->context(), Position(), ForLoopPositions{},
                                      std::move(initializer), std::move(test), std::move(next),
                                      std::move(body), std::move(unrollInfo), fSymbolTable);
        }
        case Rehydrator::kIf_Command: {
            bool isStatic = this->readU8();
            std::unique_ptr<Expression> test = this->expression();
            std::unique_ptr<Statement> ifTrue = this->statement();
            std::unique_ptr<Statement> ifFalse = this->statement();
            return IfStatement::Make(this->context(), Position(), isStatic, std::move(test),
                                     std::move(ifTrue), std::move(ifFalse));
        }
        case Rehydrator::kNop_Command:
            return std::make_unique<SkSL::Nop>();
        case Rehydrator::kReturn_Command: {
            std::unique_ptr<Expression> expr = this->expression();
            return ReturnStatement::Make(Position(), std::move(expr));
        }
        case Rehydrator::kSwitch_Command: {
            bool isStatic = this->readU8();
            AutoRehydratorSymbolTable symbols(this);
            std::unique_ptr<Expression> expr = this->expression();
            int caseCount = this->readU8();
            StatementArray cases;
            cases.reserve_back(caseCount);
            for (int i = 0; i < caseCount; ++i) {
                bool isDefault = this->readU8();
                if (isDefault) {
                    std::unique_ptr<Statement> statement = this->statement();
                    cases.push_back(SwitchCase::MakeDefault(Position(), std::move(statement)));
                } else {
                    SKSL_INT value = this->readS32();
                    std::unique_ptr<Statement> statement = this->statement();
                    cases.push_back(SwitchCase::Make(Position(), std::move(value),
                            std::move(statement)));
                }
            }
            return SwitchStatement::Make(this->context(), Position(), isStatic, std::move(expr),
                                         std::move(cases), fSymbolTable);
        }
        case Rehydrator::kVarDeclaration_Command: {
            Variable* var = this->symbolRef<Variable>();
            const Type* baseType = this->type();
            int arraySize = this->readU8();
            std::unique_ptr<Expression> value = this->expression();
            return VarDeclaration::Make(this->context(), var, baseType, arraySize,
                    std::move(value));
        }
        case Rehydrator::kVoid_Command:
            return nullptr;
        default:
            printf("unsupported statement %d\n", kind);
            SkASSERT(false);
            return nullptr;
    }
}

ExpressionArray Rehydrator::expressionArray() {
    uint8_t count = this->readU8();
    ExpressionArray array;
    array.reserve_back(count);
    for (int i = 0; i < count; ++i) {
        array.push_back(this->expression());
    }
    return array;
}

std::unique_ptr<Expression> Rehydrator::expression() {
    Position pos;
    int kind = this->readU8();
    switch (kind) {
        case Rehydrator::kBinary_Command: {
            std::unique_ptr<Expression> left = this->expression();
            Operator::Kind op = (Operator::Kind)this->readU8();
            std::unique_ptr<Expression> right = this->expression();
            return BinaryExpression::Make(this->context(), pos, std::move(left), op,
                    std::move(right));
        }
        case Rehydrator::kBoolLiteral_Command: {
            bool value = this->readU8();
            return Literal::MakeBool(this->context(), pos, value);
        }
        case Rehydrator::kConstructorArray_Command: {
            const Type* type = this->type();
            return ConstructorArray::Make(this->context(), pos, *type, this->expressionArray());
        }
        case Rehydrator::kConstructorArrayCast_Command: {
            const Type* type = this->type();
            ExpressionArray args = this->expressionArray();
            SkASSERT(args.size() == 1);
            return ConstructorArrayCast::Make(this->context(), pos, *type, std::move(args[0]));
        }
        case Rehydrator::kConstructorCompound_Command: {
            const Type* type = this->type();
            return ConstructorCompound::Make(this->context(), pos, *type, this->expressionArray());
        }
        case Rehydrator::kConstructorDiagonalMatrix_Command: {
            const Type* type = this->type();
            ExpressionArray args = this->expressionArray();
            SkASSERT(args.size() == 1);
            return ConstructorDiagonalMatrix::Make(this->context(), pos, *type, std::move(args[0]));
        }
        case Rehydrator::kConstructorMatrixResize_Command: {
            const Type* type = this->type();
            ExpressionArray args = this->expressionArray();
            SkASSERT(args.size() == 1);
            return ConstructorMatrixResize::Make(this->context(), pos, *type, std::move(args[0]));
        }
        case Rehydrator::kConstructorScalarCast_Command: {
            const Type* type = this->type();
            ExpressionArray args = this->expressionArray();
            SkASSERT(args.size() == 1);
            return ConstructorScalarCast::Make(this->context(), pos, *type, std::move(args[0]));
        }
        case Rehydrator::kConstructorSplat_Command: {
            const Type* type = this->type();
            ExpressionArray args = this->expressionArray();
            SkASSERT(args.size() == 1);
            return ConstructorSplat::Make(this->context(), pos, *type, std::move(args[0]));
        }
        case Rehydrator::kConstructorStruct_Command: {
            const Type* type = this->type();
            return ConstructorStruct::Make(this->context(), pos, *type, this->expressionArray());
        }
        case Rehydrator::kConstructorCompoundCast_Command: {
            const Type* type = this->type();
            ExpressionArray args = this->expressionArray();
            SkASSERT(args.size() == 1);
            return ConstructorCompoundCast::Make(this->context(), pos, *type, std::move(args[0]));
        }
        case Rehydrator::kFieldAccess_Command: {
            std::unique_ptr<Expression> base = this->expression();
            int index = this->readU8();
            FieldAccess::OwnerKind ownerKind = (FieldAccess::OwnerKind) this->readU8();
            return FieldAccess::Make(this->context(), pos, std::move(base), index, ownerKind);
        }
        case Rehydrator::kFloatLiteral_Command: {
            const Type* type = this->type();
            int32_t floatBits = this->readS32();
            float value;
            memcpy(&value, &floatBits, sizeof(value));
            return Literal::MakeFloat(pos, value, type);
        }
        case Rehydrator::kFunctionCall_Command: {
            const Type* type = this->type();
            const Symbol* symbol = this->possiblyBuiltinSymbolRef();
            ExpressionArray args = this->expressionArray();
            const FunctionDeclaration* f = &symbol->as<FunctionDeclaration>();
            f = FunctionCall::FindBestFunctionForCall(this->context(), f, args);
            return FunctionCall::Make(this->context(), pos, type, *f, std::move(args));
        }
        case Rehydrator::kIndex_Command: {
            std::unique_ptr<Expression> base = this->expression();
            std::unique_ptr<Expression> index = this->expression();
            return IndexExpression::Make(this->context(), pos, std::move(base), std::move(index));
        }
        case Rehydrator::kIntLiteral_Command: {
            const Type* type = this->type();
            if (type->isUnsigned()) {
                unsigned int value = this->readU32();
                return Literal::MakeInt(pos, value, type);
            } else {
                int value = this->readS32();
                return Literal::MakeInt(pos, value, type);
            }
        }
        case Rehydrator::kPostfix_Command: {
            Operator::Kind op = (Operator::Kind)this->readU8();
            std::unique_ptr<Expression> operand = this->expression();
            return PostfixExpression::Make(this->context(), pos, std::move(operand), op);
        }
        case Rehydrator::kPrefix_Command: {
            Operator::Kind op = (Operator::Kind)this->readU8();
            std::unique_ptr<Expression> operand = this->expression();
            return PrefixExpression::Make(this->context(), pos, op, std::move(operand));
        }
        case Rehydrator::kSetting_Command: {
            std::string_view name(this->readString());
            return Setting::Convert(this->context(), pos, name);
        }
        case Rehydrator::kSwizzle_Command: {
            std::unique_ptr<Expression> base = this->expression();
            int count = this->readU8();
            ComponentArray components;
            for (int i = 0; i < count; ++i) {
                components.push_back(this->readU8());
            }
            return Swizzle::Make(this->context(), pos, std::move(base), components);
        }
        case Rehydrator::kTernary_Command: {
            std::unique_ptr<Expression> test = this->expression();
            std::unique_ptr<Expression> ifTrue = this->expression();
            std::unique_ptr<Expression> ifFalse = this->expression();
            return TernaryExpression::Make(this->context(), pos, std::move(test),
                                           std::move(ifTrue), std::move(ifFalse));
        }
        case Rehydrator::kVariableReference_Command: {
            const Variable* var = &this->possiblyBuiltinSymbolRef()->as<Variable>();
            VariableReference::RefKind refKind = (VariableReference::RefKind) this->readU8();
            return VariableReference::Make(pos, var, refKind);
        }
        case Rehydrator::kVoid_Command:
            return nullptr;
        default:
            printf("unsupported expression %d\n", kind);
            SkASSERT(false);
            return nullptr;
    }
}

std::shared_ptr<SymbolTable> Rehydrator::symbolTable() {
    int command = this->readU8();
    if (command == kVoid_Command) {
        return nullptr;
    }
    SkASSERT(command == kSymbolTable_Command);
    bool builtin = this->readU8();
    uint16_t ownedCount = this->readU16();
    fSymbolTable = std::make_shared<SymbolTable>(std::move(fSymbolTable), builtin);
    std::vector<Symbol*> ownedSymbols;
    ownedSymbols.reserve(ownedCount);

    // Write the owned symbols.
    SYMBOL_DEBUGF("\nOwned symbols in Rehydrator:\n\n\n");
    for (int i = 0; i < ownedCount; ++i) {
        ownedSymbols.push_back(this->symbol());
        SYMBOL_DEBUGF("%s\n", ownedSymbols.back()->description().c_str());
    }

    SYMBOL_DEBUGF("\nOrdered symbols in Rehydrator:\n\n\n");
    uint16_t symbolCount = this->readU16();
    for (int i = 0; i < symbolCount; ++i) {
        int index = this->readU16();
        if (index != kBuiltin_Symbol) {
            SYMBOL_DEBUGF("%s\n", ownedSymbols[index]->description().c_str());
            fSymbolTable->addWithoutOwnership(ownedSymbols[index]);
        } else {
            std::string_view name = this->readString();
            SymbolTable* root = fSymbolTable.get();
            while (root->fParent) {
                root = root->fParent.get();
            }
            const Symbol* s = (*root)[name];
            SkASSERT(s);
            SYMBOL_DEBUGF("(builtin symbol) %s\n", s->description().c_str());
            fSymbolTable->addWithoutOwnership(s);
        }
    }
    return fSymbolTable;
}

}  // namespace SkSL
