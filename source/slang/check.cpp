#include "syntax-visitors.h"

#include "lookup.h"
#include "compiler.h"
#include "visitor.h"

#include <assert.h>

namespace Slang
{
    bool IsNumeric(BaseType t)
    {
        return t == BaseType::Int || t == BaseType::Float || t == BaseType::UInt;
    }

    String TranslateHLSLTypeNames(String name)
    {
        if (name == "float2" || name == "half2")
            return "vec2";
        else if (name == "float3" || name == "half3")
            return "vec3";
        else if (name == "float4" || name == "half4")
            return "vec4";
        else if (name == "half")
            return "float";
        else if (name == "int2")
            return "ivec2";
        else if (name == "int3")
            return "ivec3";
        else if (name == "int4")
            return "ivec4";
        else if (name == "uint2")
            return "uvec2";
        else if (name == "uint3")
            return "uvec3";
        else if (name == "uint4")
            return "uvec4";
        else if (name == "float3x3" || name == "half3x3")
            return "mat3";
        else if (name == "float4x4" || name == "half4x4")
            return "mat4";
        else
            return name;
    }

    struct SemanticsVisitor
        : ExprVisitor<SemanticsVisitor, RefPtr<Expr>>
        , StmtVisitor<SemanticsVisitor>
        , DeclVisitor<SemanticsVisitor>
    {
        DiagnosticSink* sink = nullptr;
        DiagnosticSink* getSink()
        {
            return sink;
        }

//        ModuleDecl * program = nullptr;
        FuncDecl * function = nullptr;

        CompileRequest* request = nullptr;
        TranslationUnitRequest* translationUnit = nullptr;

        SourceLanguage getSourceLanguage()
        {
            return translationUnit->sourceLanguage;
        }

        // lexical outer statements
        List<Stmt*> outerStmts;

        // We need to track what has been `import`ed,
        // to avoid importing the same thing more than once
        //
        // TODO: a smarter approach might be to filter
        // out duplicate references during lookup.
        HashSet<ModuleDecl*> importedModules;

    public:
        SemanticsVisitor(
            DiagnosticSink*         sink,
            CompileRequest*         request,
            TranslationUnitRequest* translationUnit)
            : sink(sink)
            , request(request)
            , translationUnit(translationUnit)
        {
        }

        CompileRequest* getCompileRequest() { return request; }
        TranslationUnitRequest* getTranslationUnit() { return translationUnit; }
        Session* getSession()
        {
            return getCompileRequest()->mSession;
        }

    public:
        // Translate Types
        RefPtr<Type> typeResult;
        RefPtr<Expr> TranslateTypeNodeImpl(const RefPtr<Expr> & node)
        {
            if (!node) return nullptr;

            auto expr = CheckTerm(node);
            expr = ExpectATypeRepr(expr);
            return expr;
        }
        RefPtr<Type> ExtractTypeFromTypeRepr(const RefPtr<Expr>& typeRepr)
        {
            if (!typeRepr) return nullptr;
            if (auto typeType = typeRepr->type->As<TypeType>())
            {
                return typeType->type;
            }
            return getSession()->getErrorType();
        }
        RefPtr<Type> TranslateTypeNode(const RefPtr<Expr> & node)
        {
            if (!node) return nullptr;
            auto typeRepr = TranslateTypeNodeImpl(node);
            return ExtractTypeFromTypeRepr(typeRepr);
        }
        TypeExp TranslateTypeNode(TypeExp const& typeExp)
        {
            // HACK(tfoley): It seems that in some cases we end up re-checking
            // syntax that we've already checked. We need to root-cause that
            // issue, but for now a quick fix in this case is to early
            // exist if we've already got a type associated here:
            if (typeExp.type)
            {
                return typeExp;
            }


            auto typeRepr = TranslateTypeNodeImpl(typeExp.exp);

            TypeExp result;
            result.exp = typeRepr;
            result.type = ExtractTypeFromTypeRepr(typeRepr);
            return result;
        }

        RefPtr<Expr> ConstructDeclRefExpr(
            DeclRef<Decl>   declRef,
            RefPtr<Expr>    baseExpr,
            SourceLoc       loc)
        {
            if (baseExpr)
            {
                if (baseExpr->type->As<TypeType>())
                {
                    auto expr = new StaticMemberExpr();
                    expr->loc = loc;
                    expr->BaseExpression = baseExpr;
                    expr->name = declRef.GetName();
                    expr->type = GetTypeForDeclRef(declRef);
                    expr->declRef = declRef;
                    return expr;
                }
                else
                {
                    auto expr = new MemberExpr();
                    expr->loc = loc;
                    expr->BaseExpression = baseExpr;
                    expr->name = declRef.GetName();
                    expr->type = GetTypeForDeclRef(declRef);
                    expr->declRef = declRef;
                    return expr;
                }
            }
            else
            {
                auto expr = new VarExpr();
                expr->loc = loc;
                expr->name = declRef.GetName();
                expr->type = GetTypeForDeclRef(declRef);
                expr->declRef = declRef;
                return expr;
            }
        }

        RefPtr<Expr> ConstructDerefExpr(
            RefPtr<Expr>    base,
            SourceLoc       loc)
        {
            auto ptrLikeType = base->type->As<PointerLikeType>();
            SLANG_ASSERT(ptrLikeType);

            auto derefExpr = new DerefExpr();
            derefExpr->loc = loc;
            derefExpr->base = base;
            derefExpr->type = QualType(ptrLikeType->elementType);

            // TODO(tfoley): handle l-value status here

            return derefExpr;
        }

        RefPtr<Expr> ConstructLookupResultExpr(
            LookupResultItem const& item,
            RefPtr<Expr>            baseExpr,
            SourceLoc               loc)
        {
            // If we collected any breadcrumbs, then these represent
            // additional segments of the lookup path that we need
            // to expand here.
            auto bb = baseExpr;
            for (auto breadcrumb = item.breadcrumbs; breadcrumb; breadcrumb = breadcrumb->next)
            {
                switch (breadcrumb->kind)
                {
                case LookupResultItem::Breadcrumb::Kind::Member:
                    bb = ConstructDeclRefExpr(breadcrumb->declRef, bb, loc);
                    break;
                case LookupResultItem::Breadcrumb::Kind::Deref:
                    bb = ConstructDerefExpr(bb, loc);
                    break;
                default:
                    SLANG_UNREACHABLE("all cases handle");
                }
            }

            return ConstructDeclRefExpr(item.declRef, bb, loc);
        }

        RefPtr<Expr> createLookupResultExpr(
            LookupResult const&     lookupResult,
            RefPtr<Expr>            baseExpr,
            SourceLoc               loc)
        {
            if (lookupResult.isOverloaded())
            {
                auto overloadedExpr = new OverloadedExpr();
                overloadedExpr->loc = loc;
                overloadedExpr->type = QualType(
                    getSession()->getOverloadedType());
                overloadedExpr->base = baseExpr;
                overloadedExpr->lookupResult2 = lookupResult;
                return overloadedExpr;
            }
            else
            {
                return ConstructLookupResultExpr(lookupResult.item, baseExpr, loc);
            }
        }

        RefPtr<Expr> ResolveOverloadedExpr(RefPtr<OverloadedExpr> overloadedExpr, LookupMask mask)
        {
            auto lookupResult = overloadedExpr->lookupResult2;
            SLANG_RELEASE_ASSERT(lookupResult.isValid() && lookupResult.isOverloaded());

            // Take the lookup result we had, and refine it based on what is expected in context.
            lookupResult = refineLookup(lookupResult, mask);

            if (!lookupResult.isValid())
            {
                // If we didn't find any symbols after filtering, then just
                // use the original and report errors that way
                return overloadedExpr;
            }

            if (lookupResult.isOverloaded())
            {
                // We had an ambiguity anyway, so report it.
                if (!isRewriteMode())
                {
                    getSink()->diagnose(overloadedExpr, Diagnostics::ambiguousReference, lookupResult.items[0].declRef.GetName());

                    for(auto item : lookupResult.items)
                    {
                        String declString = getDeclSignatureString(item);
                        getSink()->diagnose(item.declRef, Diagnostics::overloadCandidate, declString);
                    }
                }

                // TODO(tfoley): should we construct a new ErrorExpr here?
                return CreateErrorExpr(overloadedExpr);
            }

            // otherwise, we had a single decl and it was valid, hooray!
            return ConstructLookupResultExpr(lookupResult.item, overloadedExpr->base, overloadedExpr->loc);
        }

        RefPtr<Expr> ExpectATypeRepr(RefPtr<Expr> expr)
        {
            if (auto overloadedExpr = expr.As<OverloadedExpr>())
            {
                expr = ResolveOverloadedExpr(overloadedExpr, LookupMask::type);
            }

            if (auto typeType = expr->type.type->As<TypeType>())
            {
                return expr;
            }
            else if (auto errorType = expr->type.type->As<ErrorType>())
            {
                return expr;
            }

            if (!isRewriteMode())
            {
                getSink()->diagnose(expr, Diagnostics::unimplemented, "expected a type");
            }
            return CreateErrorExpr(expr);
        }

        RefPtr<Type> ExpectAType(RefPtr<Expr> expr)
        {
            auto typeRepr = ExpectATypeRepr(expr);
            if (auto typeType = typeRepr->type->As<TypeType>())
            {
                return typeType->type;
            }
            return getSession()->getErrorType();
        }

        RefPtr<Type> ExtractGenericArgType(RefPtr<Expr> exp)
        {
            return ExpectAType(exp);
        }

        RefPtr<IntVal> ExtractGenericArgInteger(RefPtr<Expr> exp)
        {
            return CheckIntegerConstantExpression(exp.Ptr());
        }

        RefPtr<Val> ExtractGenericArgVal(RefPtr<Expr> exp)
        {
            if (auto overloadedExpr = exp.As<OverloadedExpr>())
            {
                // assume that if it is overloaded, we want a type
                exp = ResolveOverloadedExpr(overloadedExpr, LookupMask::type);
            }

            if (auto typeType = exp->type->As<TypeType>())
            {
                return typeType->type;
            }
            else if (auto errorType = exp->type->As<ErrorType>())
            {
                return exp->type.type;
            }
            else
            {
                return ExtractGenericArgInteger(exp);
            }
        }

        // Construct a type reprsenting the instantiation of
        // the given generic declaration for the given arguments.
        // The arguments should already be checked against
        // the declaration.
        RefPtr<Type> InstantiateGenericType(
            DeclRef<GenericDecl>						genericDeclRef,
            List<RefPtr<Expr>> const&	args)
        {
            RefPtr<Substitutions> subst = new Substitutions();
            subst->genericDecl = genericDeclRef.getDecl();
            subst->outer = genericDeclRef.substitutions;

            for (auto argExpr : args)
            {
                subst->args.Add(ExtractGenericArgVal(argExpr));
            }

            DeclRef<Decl> innerDeclRef;
            innerDeclRef.decl = GetInner(genericDeclRef);
            innerDeclRef.substitutions = subst;

            return DeclRefType::Create(
                getSession(),
                innerDeclRef);
        }

        // Make sure a declaration has been checked, so we can refer to it.
        // Note that this may lead to us recursively invoking checking,
        // so this may not be the best way to handle things.
        void EnsureDecl(RefPtr<Decl> decl, DeclCheckState state = DeclCheckState::CheckedHeader)
        {
            if (decl->IsChecked(state)) return;
            if (decl->checkState == DeclCheckState::CheckingHeader)
            {
                // We tried to reference the same declaration while checking it!
                throw "circularity";
            }

            if (DeclCheckState::CheckingHeader > decl->checkState)
            {
                decl->SetCheckState(DeclCheckState::CheckingHeader);
            }

            // Use visitor pattern to dispatch to correct case
            DeclVisitor::dispatch(decl);

            decl->SetCheckState(DeclCheckState::Checked);
        }

        void EnusreAllDeclsRec(RefPtr<Decl> decl)
        {
            EnsureDecl(decl, DeclCheckState::Checked);
            if (auto containerDecl = decl.As<ContainerDecl>())
            {
                for (auto m : containerDecl->Members)
                {
                    EnusreAllDeclsRec(m);
                }
            }
        }

        // A "proper" type is one that can be used as the type of an expression.
        // Put simply, it can be a concrete type like `int`, or a generic
        // type that is applied to arguments, like `Texture2D<float4>`.
        // The type `void` is also a proper type, since we can have expressions
        // that return a `void` result (e.g., many function calls).
        //
        // A "non-proper" type is any type that can't actually have values.
        // A simple example of this in C++ is `std::vector` - you can't have
        // a value of this type.
        //
        // Part of what this function does is give errors if somebody tries
        // to use a non-proper type as the type of a variable (or anything
        // else that needs a proper type).
        //
        // The other thing it handles is the fact that HLSL lets you use
        // the name of a non-proper type, and then have the compiler fill
        // in the default values for its type arguments (e.g., a variable
        // given type `Texture2D` will actually have type `Texture2D<float4>`).
        bool CoerceToProperTypeImpl(TypeExp const& typeExp, RefPtr<Type>* outProperType)
        {
            Type* type = typeExp.type.Ptr();
            if (auto genericDeclRefType = type->As<GenericDeclRefType>())
            {
                // We are using a reference to a generic declaration as a concrete
                // type. This means we should substitute in any default parameter values
                // if they are available.
                //
                // TODO(tfoley): A more expressive type system would substitute in
                // "fresh" variables and then solve for their values...
                //

                auto genericDeclRef = genericDeclRefType->GetDeclRef();
                EnsureDecl(genericDeclRef.decl);
                List<RefPtr<Expr>> args;
                for (RefPtr<Decl> member : genericDeclRef.getDecl()->Members)
                {
                    if (auto typeParam = member.As<GenericTypeParamDecl>())
                    {
                        if (!typeParam->initType.exp)
                        {
                            if (outProperType)
                            {
                                if (!isRewriteMode())
                                {
                                    getSink()->diagnose(typeExp.exp.Ptr(), Diagnostics::unimplemented, "can't fill in default for generic type parameter");
                                }
                                *outProperType = getSession()->getErrorType();
                            }
                            return false;
                        }

                        // TODO: this is one place where syntax should get cloned!
                        if(outProperType)
                            args.Add(typeParam->initType.exp);
                    }
                    else if (auto valParam = member.As<GenericValueParamDecl>())
                    {
                        if (!valParam->initExpr)
                        {
                            if (outProperType)
                            {
                                if (!isRewriteMode())
                                {
                                    getSink()->diagnose(typeExp.exp.Ptr(), Diagnostics::unimplemented, "can't fill in default for generic type parameter");
                                }
                                *outProperType = getSession()->getErrorType();
                            }
                            return false;
                        }

                        // TODO: this is one place where syntax should get cloned!
                        if(outProperType)
                            args.Add(valParam->initExpr);
                    }
                    else
                    {
                        // ignore non-parameter members
                    }
                }

                if (outProperType)
                {
                    *outProperType = InstantiateGenericType(genericDeclRef, args);
                }
                return true;
            }
            else
            {
                // default case: we expect this to already be a proper type
                if (outProperType)
                {
                    *outProperType = type;
                }
                return true;
            }
        }



        TypeExp CoerceToProperType(TypeExp const& typeExp)
        {
            TypeExp result = typeExp;
            CoerceToProperTypeImpl(typeExp, &result.type);
            return result;
        }

        bool CanCoerceToProperType(TypeExp const& typeExp)
        {
            return CoerceToProperTypeImpl(typeExp, nullptr);
        }

        // Check a type, and coerce it to be proper
        TypeExp CheckProperType(TypeExp typeExp)
        {
            return CoerceToProperType(TranslateTypeNode(typeExp));
        }

        // For our purposes, a "usable" type is one that can be
        // used to declare a function parameter, variable, etc.
        // These turn out to be all the proper types except
        // `void`.
        //
        // TODO(tfoley): consider just allowing `void` as a
        // simple example of a "unit" type, and get rid of
        // this check.
        TypeExp CoerceToUsableType(TypeExp const& typeExp)
        {
            TypeExp result = CoerceToProperType(typeExp);
            Type* type = result.type.Ptr();
            if (auto basicType = type->As<BasicExpressionType>())
            {
                // TODO: `void` shouldn't be a basic type, to make this easier to avoid
                if (basicType->baseType == BaseType::Void)
                {
                    // TODO(tfoley): pick the right diagnostic message
                    if (!isRewriteMode())
                    {
                        getSink()->diagnose(result.exp.Ptr(), Diagnostics::invalidTypeVoid);
                    }
                    result.type = getSession()->getErrorType();
                    return result;
                }
            }
            return result;
        }

        // Check a type, and coerce it to be usable
        TypeExp CheckUsableType(TypeExp typeExp)
        {
            return CoerceToUsableType(TranslateTypeNode(typeExp));
        }

        RefPtr<Expr> CheckTerm(RefPtr<Expr> term)
        {
            if (!term) return nullptr;
            return ExprVisitor::dispatch(term);
        }

        RefPtr<Expr> CreateErrorExpr(Expr* expr)
        {
            expr->type = QualType(getSession()->getErrorType());
            return expr;
        }

        bool IsErrorExpr(RefPtr<Expr> expr)
        {
            // TODO: we may want other cases here...

            if (auto errorType = expr->type->As<ErrorType>())
                return true;

            return false;
        }

        // Capture the "base" expression in case this is a member reference
        RefPtr<Expr> GetBaseExpr(RefPtr<Expr> expr)
        {
            if (auto memberExpr = expr.As<MemberExpr>())
            {
                return memberExpr->BaseExpression;
            }
            else if(auto overloadedExpr = expr.As<OverloadedExpr>())
            {
                return overloadedExpr->base;
            }
            return nullptr;
        }

    public:

        bool ValuesAreEqual(
            RefPtr<IntVal> left,
            RefPtr<IntVal> right)
        {
            if(left == right) return true;

            if(auto leftConst = left.As<ConstantIntVal>())
            {
                if(auto rightConst = right.As<ConstantIntVal>())
                {
                    return leftConst->value == rightConst->value;
                }
            }

            if(auto leftVar = left.As<GenericParamIntVal>())
            {
                if(auto rightVar = right.As<GenericParamIntVal>())
                {
                    return leftVar->declRef.Equals(rightVar->declRef);
                }
            }

            return false;
        }

        // Compute the cost of using a particular declaration to
        // perform implicit type conversion.
        ConversionCost getImplicitConversionCost(
            Decl* decl)
        {
            if(auto modifier = decl->FindModifier<ImplicitConversionModifier>())
            {
                return modifier->cost;
            }

            return kConversionCost_Explicit;
        }

        // Central engine for implementing implicit coercion logic
        bool TryCoerceImpl(
            RefPtr<Type>			toType,		// the target type for conversion
            RefPtr<Expr>*	outToExpr,	// (optional) a place to stuff the target expression
            RefPtr<Type>			fromType,	// the source type for the conversion
            RefPtr<Expr>	fromExpr,	// the source expression
            ConversionCost*					outCost)	// (optional) a place to stuff the conversion cost
        {
            // Easy case: the types are equal
            if (toType->Equals(fromType))
            {
                if (outToExpr)
                    *outToExpr = fromExpr;
                if (outCost)
                    *outCost = kConversionCost_None;
                return true;
            }

            // If either type is an error, then let things pass.
            if (toType->As<ErrorType>() || fromType->As<ErrorType>())
            {
                if (outToExpr)
                    *outToExpr = CreateImplicitCastExpr(toType, fromExpr);
                if (outCost)
                    *outCost = kConversionCost_None;
                return true;
            }

            // Coercion from an initializer list is allowed for many types
            if( auto fromInitializerListExpr = fromExpr.As<InitializerListExpr>())
            {
                auto argCount = fromInitializerListExpr->args.Count();

                // In the case where we need to build a reuslt expression,
                // we will collect the new arguments here
                List<RefPtr<Expr>> coercedArgs;

                if(auto toDeclRefType = toType->As<DeclRefType>())
                {
                    auto toTypeDeclRef = toDeclRefType->declRef;
                    if(auto toStructDeclRef = toTypeDeclRef.As<StructDecl>())
                    {
                        // Trying to initialize a `struct` type given an initializer list.
                        // We will go through the fields in order and try to match them
                        // up with initializer arguments.


                        UInt argIndex = 0;
                        for(auto fieldDeclRef : getMembersOfType<StructField>(toStructDeclRef))
                        {
                            if(argIndex >= argCount)
                            {
                                // We've consumed all the arguments, so we should stop
                                break;
                            }

                            auto arg = fromInitializerListExpr->args[argIndex++];

                            // 
                            RefPtr<Expr> coercedArg;
                            ConversionCost argCost;

                            bool argResult = TryCoerceImpl(
                                GetType(fieldDeclRef),
                                outToExpr ? &coercedArg : nullptr,
                                arg->type,
                                arg,
                                outCost ? &argCost : nullptr);

                            // No point in trying further if any argument fails
                            if(!argResult)
                                return false;

                            // TODO(tfoley): what to do with cost?
                            // This only matters if/when we allow an initializer list as an argument to
                            // an overloaded call.

                            if( outToExpr )
                            {
                                coercedArgs.Add(coercedArg);
                            }
                        }
                    }
                }
                else if(auto toArrayType = toType->As<ArrayExpressionType>())
                {
                    // TODO(tfoley): If we can compute the size of the array statically,
                    // then we want to check that there aren't too many initializers present

                    auto toElementType = toArrayType->baseType;

                    for(auto& arg : fromInitializerListExpr->args)
                    {
                        RefPtr<Expr> coercedArg;
                        ConversionCost argCost;

                        bool argResult = TryCoerceImpl(
                                toElementType,
                                outToExpr ? &coercedArg : nullptr,
                                arg->type,
                                arg,
                                outCost ? &argCost : nullptr);

                        // No point in trying further if any argument fails
                        if(!argResult)
                            return false;

                        if( outToExpr )
                        {
                            coercedArgs.Add(coercedArg);
                        }
                    }
                }
                else
                {
                    // By default, we don't allow a type to be initialized using
                    // an initializer list.
                    return false;
                }

                // For now, coercion from an initializer list has no cost
                if(outCost)
                {
                    *outCost = kConversionCost_None;
                }

                // We were able to coerce all the arguments given, and so
                // we need to construct a suitable expression to remember the result
                if(outToExpr)
                {
                    auto toInitializerListExpr = new InitializerListExpr();
                    toInitializerListExpr->loc = fromInitializerListExpr->loc;
                    toInitializerListExpr->type = QualType(toType);
                    toInitializerListExpr->args = coercedArgs;


                    *outToExpr = toInitializerListExpr;
                }

                return true;
            }

            //


            if (auto toDeclRefType = toType->As<DeclRefType>())
            {
                auto toTypeDeclRef = toDeclRefType->declRef;
                if (auto interfaceDeclRef = toTypeDeclRef.As<InterfaceDecl>())
                {
                    // Trying to convert to an interface type.
                    //
                    // We will allow this if the type conforms to the interface.
                    if (DoesTypeConformToInterface(fromType, interfaceDeclRef))
                    {
                        if (outToExpr)
                            *outToExpr = CreateImplicitCastExpr(toType, fromExpr);
                        if (outCost)
                            *outCost = kConversionCost_CastToInterface;
                        return true;
                    }
                }
            }

            // Look for an initializer/constructor declaration in the target type,
            // which is marked as usable for implicit conversion, and which takes
            // the source type as an argument.

            OverloadResolveContext overloadContext;

            List<RefPtr<Expr>> args;
            args.Add(fromExpr);

            overloadContext.disallowNestedConversions = true;
            overloadContext.argCount = 1;
            overloadContext.argTypes = &fromType;

            overloadContext.originalExpr = nullptr;
            if(fromExpr)
            {
                overloadContext.loc = fromExpr->loc;
                overloadContext.funcLoc = fromExpr->loc;
                overloadContext.args = &fromExpr;
            }

            overloadContext.baseExpr = nullptr;
            overloadContext.mode = OverloadResolveContext::Mode::JustTrying;
            
            AddTypeOverloadCandidates(toType, overloadContext);

            if(overloadContext.bestCandidates.Count() != 0)
            {
                // There were multiple candidates that were equally good.

                // First, we will check if these candidates are even applicable.
                // If they aren't, then they can't be used for conversion.
                if(overloadContext.bestCandidates[0].status != OverloadCandidate::Status::Appicable)
                    return false;

                // If we reach this point, then we have multiple candidates which are
                // all equally applicable, which means we have an ambiguity.
                // If the user is just querying whether a conversion is possible, we
                // will tell them it is, because ambiguity should trigger an ambiguity
                // error, and not a "no conversion possible" error.

                // We will compute a nominal conversion cost as the minimum over
                // all the conversions available.
                ConversionCost cost = kConversionCost_GeneralConversion;
                for(auto candidate : overloadContext.bestCandidates)
                {
                    ConversionCost candidateCost = getImplicitConversionCost(
                        candidate.item.declRef.getDecl());

                    if(candidateCost < cost)
                        cost = candidateCost;
                }

                if(outCost)
                    *outCost = cost;

                if(outToExpr)
                {
                    // The user is asking for us to actually perform the conversion,
                    // so we need to generate an appropriate expression here.
                    
                    throw "foo bar baz";
                }

                return true;
            }
            else if(overloadContext.bestCandidate)
            {
                // There is a single best candidate for conversion.

                // It might not actually be usable, so let's check that first.
                if(overloadContext.bestCandidate->status != OverloadCandidate::Status::Appicable)
                    return false;

                // Okay, it is applicable, and we just need to let the user
                // know about it, and optionally construct a call.

                // We need to extract the conversion cost from the candidate we found.
                ConversionCost cost = getImplicitConversionCost(
                        overloadContext.bestCandidate->item.declRef.getDecl());;

                if(outCost)
                    *outCost = cost;

                if(outToExpr)
                {
                    *outToExpr = CompleteOverloadCandidate(overloadContext, *overloadContext.bestCandidate);
                }

                return true;
            }

            return false;
        }

        // Check whether a type coercion is possible
        bool CanCoerce(
            RefPtr<Type>			toType,			// the target type for conversion
            RefPtr<Type>			fromType,		// the source type for the conversion
            ConversionCost*					outCost = 0)	// (optional) a place to stuff the conversion cost
        {
            return TryCoerceImpl(
                toType,
                nullptr,
                fromType,
                nullptr,
                outCost);
        }

        RefPtr<Expr> CreateImplicitCastExpr(
            RefPtr<Type>	toType,
            RefPtr<Expr>	fromExpr)
        {
            // In "rewrite" mode, we will generate a different syntax node
            // to indicate that this type-cast was implicitly generated
            // by the compiler, and shouldn't appear in the output code.
            RefPtr<TypeCastExpr> castExpr;
            if (isRewriteMode())
            {
                castExpr = new HiddenImplicitCastExpr();
            }
            else
            {
                castExpr = new ImplicitCastExpr();
            }

            auto typeType = new TypeType();
            typeType->type = toType;

            auto typeExpr = new SharedTypeExpr();
            typeExpr->type.type = typeType;
            typeExpr->base.type = toType;

            castExpr->loc = fromExpr->loc;
            castExpr->FunctionExpr = typeExpr;
            castExpr->type = QualType(toType);
            castExpr->Arguments.Add(fromExpr);
            return castExpr;
        }

        bool isRewriteMode()
        {
            return (getTranslationUnit()->compileFlags & SLANG_COMPILE_FLAG_NO_CHECKING) != 0;
        }

        // Perform type coercion, and emit errors if it isn't possible
        RefPtr<Expr> Coerce(
            RefPtr<Type>			toType,
            RefPtr<Expr>	fromExpr)
        {
            // If semantic checking is being suppressed, then we might see
            // expressions without a type, and we need to ignore them.
            if( !fromExpr->type.type )
            {
                if(isRewriteMode())
                    return fromExpr;
            }

            RefPtr<Expr> expr;
            if (!TryCoerceImpl(
                toType,
                &expr,
                fromExpr->type.Ptr(),
                fromExpr.Ptr(),
                nullptr))
            {
                if(!isRewriteMode())
                {
                    getSink()->diagnose(fromExpr->loc, Diagnostics::typeMismatch, toType, fromExpr->type);
                }

                // Note(tfoley): We don't call `CreateErrorExpr` here, because that would
                // clobber the type on `fromExpr`, and an invariant here is that coercion
                // really shouldn't *change* the expression that is passed in, but should
                // introduce new AST nodes to coerce its value to a different type...
                return CreateImplicitCastExpr(
                    getSession()->getErrorType(),
                    fromExpr);
            }
            return expr;
        }

        void CheckVarDeclCommon(RefPtr<VarDeclBase> varDecl)
        {
            // Check the type, if one was given
            TypeExp type = CheckUsableType(varDecl->type);

            // TODO: Additional validation rules on types should go here,
            // but we need to deal with the fact that some cases might be
            // allowed in one context (e.g., an unsized array parameter)
            // but not in othters (e.g., an unsized array field in a struct).

            // Check the initializers, if one was given
            RefPtr<Expr> initExpr = CheckTerm(varDecl->initExpr);

            // If a type was given, ...
            if (type.Ptr())
            {
                // then coerce any initializer to the type
                if (initExpr)
                {
                    initExpr = Coerce(type.Ptr(), initExpr);
                }
            }
            else
            {
                // TODO: infer a type from the initializers

                if (!initExpr)
                {
                    if (!isRewriteMode())
                    {
                        getSink()->diagnose(varDecl, Diagnostics::unimplemented, "variable declaration with no type must have initializer");
                    }
                }
                else
                {
                    if (!isRewriteMode())
                    {
                        getSink()->diagnose(varDecl, Diagnostics::unimplemented, "type inference for variable declaration");
                    }
                }
            }

            varDecl->type = type;
            varDecl->initExpr = initExpr;
        }

        void CheckGenericConstraintDecl(GenericTypeConstraintDecl* decl)
        {
            // TODO: are there any other validations we can do at this point?
            //
            // There probably needs to be a kind of "occurs check" to make
            // sure that the constraint actually applies to at least one
            // of the parameters of the generic.

            decl->sub = TranslateTypeNode(decl->sub);
            decl->sup = TranslateTypeNode(decl->sup);
        }

        void checkDecl(Decl* decl)
        {
            EnsureDecl(decl, DeclCheckState::Checked);
        }

        void visitGenericDecl(GenericDecl* genericDecl)
        {
            // check the parameters
            for (auto m : genericDecl->Members)
            {
                if (auto typeParam = m.As<GenericTypeParamDecl>())
                {
                    typeParam->initType = CheckProperType(typeParam->initType);
                }
                else if (auto valParam = m.As<GenericValueParamDecl>())
                {
                    // TODO: some real checking here...
                    CheckVarDeclCommon(valParam);
                }
                else if(auto constraint = m.As<GenericTypeConstraintDecl>())
                {
                    CheckGenericConstraintDecl(constraint.Ptr());
                }
            }

            // check the nested declaration
            // TODO: this needs to be done in an appropriate environment...
            checkDecl(genericDecl->inner);
        }

        void visitInterfaceDecl(InterfaceDecl* /*decl*/)
        {
            // TODO: do some actual checking of members here
        }

        void visitInheritanceDecl(InheritanceDecl* inheritanceDecl)
        {
            // check the type being inherited from
            auto base = inheritanceDecl->base;
            base = TranslateTypeNode(base);
            inheritanceDecl->base = base;

            // For now we only allow inheritance from interfaces, so
            // we will validate that the type expression names an interface

            if(auto declRefType = base.type->As<DeclRefType>())
            {
                if(auto interfaceDeclRef = declRefType->declRef.As<InterfaceDecl>())
                {
                    return;
                }
            }

            // If type expression didn't name an interface, we'll emit an error here
            // TODO: deal with the case of an error in the type expression (don't cascade)
            if (!isRewriteMode())
            {
                getSink()->diagnose( base.exp, Diagnostics::expectedAnInterfaceGot, base.type);
            }
        }

        RefPtr<ConstantIntVal> checkConstantIntVal(
            RefPtr<Expr>    expr)
        {
            // First type-check the expression as normal
            expr = CheckExpr(expr);

            auto intVal = CheckIntegerConstantExpression(expr.Ptr());
            if(!intVal)
                return nullptr;

            auto constIntVal = intVal.As<ConstantIntVal>();
            if(!constIntVal)
            {
                if (!isRewriteMode())
                {
                    getSink()->diagnose(expr->loc, Diagnostics::expectedIntegerConstantNotLiteral);
                }
                return nullptr;
            }
            return constIntVal;
        }

        void visitSyntaxDecl(SyntaxDecl*)
        {
            // These are only used in the stdlib, so no checking is needed
        }

        void visitGenericTypeParamDecl(GenericTypeParamDecl*)
        {
            // These are only used in the stdlib, so no checking is needed for now
        }

        void visitGenericValueParamDecl(GenericValueParamDecl*)
        {
            // These are only used in the stdlib, so no checking is needed for now
        }

        void visitGenericTypeConstraintDecl(GenericTypeConstraintDecl*)
        {
            // These are only used in the stdlib, so no checking is needed for now
        }

        void visitModifier(Modifier*)
        {
            // Do nothing with modifiers for now
        }

        RefPtr<Modifier> checkModifier(
            RefPtr<Modifier>    m,
            Decl*               /*decl*/)
        {
            if(auto hlslUncheckedAttribute = m.As<HLSLUncheckedAttribute>())
            {
                // We have an HLSL `[name(arg,...)]` attribute, and we'd like
                // to check that it is provides all the expected arguments
                //
                // For now we will do this in a completely ad hoc fashion,
                // but it would be nice to have some generic routine to
                // do the needed type checking/coercion.
                if(getText(hlslUncheckedAttribute->getName()) == "numthreads")
                {
                    if(hlslUncheckedAttribute->args.Count() != 3)
                        return m;

                    auto xVal = checkConstantIntVal(hlslUncheckedAttribute->args[0]);
                    auto yVal = checkConstantIntVal(hlslUncheckedAttribute->args[1]);
                    auto zVal = checkConstantIntVal(hlslUncheckedAttribute->args[2]);

                    if(!xVal) return m;
                    if(!yVal) return m;
                    if(!zVal) return m;

                    auto hlslNumThreadsAttribute = new HLSLNumThreadsAttribute();

                    hlslNumThreadsAttribute->loc   = hlslUncheckedAttribute->loc;
                    hlslNumThreadsAttribute->name       = hlslUncheckedAttribute->getName();
                    hlslNumThreadsAttribute->args       = hlslUncheckedAttribute->args;
                    hlslNumThreadsAttribute->x          = (int32_t) xVal->value;
                    hlslNumThreadsAttribute->y          = (int32_t) yVal->value;
                    hlslNumThreadsAttribute->z          = (int32_t) zVal->value;

                    return hlslNumThreadsAttribute;
                }
            }

            // Default behavior is to leave things as they are,
            // and assume that modifiers are mostly already checked.
            //
            // TODO: This would be a good place to validate that
            // a modifier is actually valid for the thing it is
            // being applied to, and potentially to check that
            // it isn't in conflict with any other modifiers
            // on the same declaration.

            return m;
        }


        void checkModifiers(Decl* decl)
        {
            // TODO(tfoley): need to make sure this only
            // performs semantic checks on a `SharedModifier` once...

            // The process of checking a modifier may produce a new modifier in its place,
            // so we will build up a new linked list of modifiers that will replace
            // the old list.
            RefPtr<Modifier> resultModifiers;
            RefPtr<Modifier>* resultModifierLink = &resultModifiers;

            RefPtr<Modifier> modifier = decl->modifiers.first;
            while(modifier)
            {
                // Because we are rewriting the list in place, we need to extract
                // the next modifier here (not at the end of the loop).
                auto next = modifier->next;

                // We also go ahead and clobber the `next` field on the modifier
                // itself, so that the default behavior of `checkModifier()` can
                // be to return a single unlinked modifier.
                modifier->next = nullptr;

                auto checkedModifier = checkModifier(modifier, decl);
                if(checkedModifier)
                {
                    // If checking gave us a modifier to add, then we
                    // had better add it.

                    // Just in case `checkModifier` ever returns multiple
                    // modifiers, lets advance to the end of the list we
                    // are building.
                    while(*resultModifierLink)
                        resultModifierLink = &(*resultModifierLink)->next;

                    // attach the new modifier at the end of the list,
                    // and now set the "link" to point to its `next` field
                    *resultModifierLink = checkedModifier;
                    resultModifierLink = &checkedModifier->next;
                }

                // Move along to the next modifier
                modifier = next;
            }

            // Whether we actually re-wrote anything or note, lets
            // install the new list of modifiers on the declaration
            decl->modifiers.first = resultModifiers;
        }

        void visitModuleDecl(ModuleDecl* programNode)
        {
            // Try to register all the builtin decls
            for (auto decl : programNode->Members)
            {
                auto inner = decl;
                if (auto genericDecl = decl.As<GenericDecl>())
                {
                    inner = genericDecl->inner;
                }

                if (auto builtinMod = inner->FindModifier<BuiltinTypeModifier>())
                {
                    registerBuiltinDecl(getSession(), decl, builtinMod);
                }
                if (auto magicMod = inner->FindModifier<MagicTypeModifier>())
                {
                    registerMagicDecl(getSession(), decl, magicMod);
                }
            }

            // We need/want to visit any `import` declarations before
            // anything else, to make sure that scoping works.
            for(auto& importDecl : programNode->getMembersOfType<ImportDecl>())
            {
                EnsureDecl(importDecl);
            }

            //

            for (auto & s : programNode->getMembersOfType<TypeDefDecl>())
                checkDecl(s.Ptr());
            for (auto & s : programNode->getMembersOfType<StructDecl>())
            {
                checkDecl(s.Ptr());
            }
			for (auto & s : programNode->getMembersOfType<ClassDecl>())
			{
				checkDecl(s.Ptr());
			}
            // HACK(tfoley): Visiting all generic declarations here,
            // because otherwise they won't get visited.
            for (auto & g : programNode->getMembersOfType<GenericDecl>())
            {
                checkDecl(g.Ptr());
            }

            for (auto & func : programNode->getMembersOfType<FuncDecl>())
            {
                if (!func->IsChecked(DeclCheckState::Checked))
                {
                    VisitFunctionDeclaration(func.Ptr());
                }
            }
            for (auto & func : programNode->getMembersOfType<FuncDecl>())
            {
                EnsureDecl(func);
            }
        
            if (sink->GetErrorCount() != 0)
                return;
               
            // Force everything to be fully checked, just in case
            // Note that we don't just call this on the program,
            // because we'd end up recursing into this very code path...
            for (auto d : programNode->Members)
            {
                EnusreAllDeclsRec(d);
            }

            // Do any semantic checking required on modifiers?
            for (auto d : programNode->Members)
            {
                checkModifiers(d.Ptr());
            }
        }

		void visitClassDecl(ClassDecl * classNode)
		{
			if (classNode->IsChecked(DeclCheckState::Checked))
				return;
			classNode->SetCheckState(DeclCheckState::Checked);

			for (auto field : classNode->GetFields())
			{
				field->type = CheckUsableType(field->type);
				field->SetCheckState(DeclCheckState::Checked);
			}
		}

        void visitStructField(StructField* field)
        {
            // TODO: bottleneck through general-case variable checking

            field->type = CheckUsableType(field->type);
            field->SetCheckState(DeclCheckState::Checked);
        }

        void visitStructDecl(StructDecl * structNode)
        {
            if (structNode->IsChecked(DeclCheckState::Checked))
                return;
            structNode->SetCheckState(DeclCheckState::Checked);

            for (auto field : structNode->GetFields())
            {
                checkDecl(field);
            }
        }

        void visitDeclGroup(DeclGroup* declGroup)
        {
            for (auto decl : declGroup->decls)
            {
                checkDecl(decl);
            }
        }

        void visitTypeDefDecl(TypeDefDecl* decl)
        {
            if (decl->IsChecked(DeclCheckState::Checked)) return;

            decl->SetCheckState(DeclCheckState::CheckingHeader);
            decl->type = CheckProperType(decl->type);
            decl->SetCheckState(DeclCheckState::Checked);
        }

        void checkStmt(Stmt* stmt)
        {
            if (!stmt) return;
            StmtVisitor::dispatch(stmt);
        }

        void visitFuncDecl(FuncDecl *functionNode)
        {
            if (functionNode->IsChecked(DeclCheckState::Checked))
                return;

            VisitFunctionDeclaration(functionNode);
            // TODO: This should really onlye set "checked header"
            functionNode->SetCheckState(DeclCheckState::Checked);

            // TODO: should put the checking of the body onto a "work list"
            // to avoid recursion here.
            if (functionNode->Body)
            {
                this->function = functionNode;
                checkStmt(functionNode->Body);
                this->function = nullptr;
            }
        }

        // Check if two functions have the same signature for the purposes
        // of overload resolution.
        bool DoFunctionSignaturesMatch(
            FuncDecl* fst,
            FuncDecl* snd)
        {
            // TODO(tfoley): This function won't do anything sensible for generics,
            // so we need to figure out a plan for that...

            // TODO(tfoley): This copies the parameter array, which is bad for performance.
            auto fstParams = fst->GetParameters().ToArray();
            auto sndParams = snd->GetParameters().ToArray();

            // If the functions have different numbers of parameters, then
            // their signatures trivially don't match.
            auto fstParamCount = fstParams.Count();
            auto sndParamCount = sndParams.Count();
            if (fstParamCount != sndParamCount)
                return false;

            for (UInt ii = 0; ii < fstParamCount; ++ii)
            {
                auto fstParam = fstParams[ii];
                auto sndParam = sndParams[ii];

                // If a given parameter type doesn't match, then signatures don't match
                if (!fstParam->type.Equals(sndParam->type))
                    return false;

                // If one parameter is `out` and the other isn't, then they don't match
                //
                // Note(tfoley): we don't consider `out` and `inout` as distinct here,
                // because there is no way for overload resolution to pick between them.
                if (fstParam->HasModifier<OutModifier>() != sndParam->HasModifier<OutModifier>())
                    return false;
            }

            // Note(tfoley): return type doesn't enter into it, because we can't take
            // calling context into account during overload resolution.

            return true;
        }

        void ValidateFunctionRedeclaration(FuncDecl* funcDecl)
        {
            auto parentDecl = funcDecl->ParentDecl;
            SLANG_RELEASE_ASSERT(parentDecl);
            if (!parentDecl) return;

            // Look at previously-declared functions with the same name,
            // in the same container
            buildMemberDictionary(parentDecl);

            for (auto prevDecl = funcDecl->nextInContainerWithSameName; prevDecl; prevDecl = prevDecl->nextInContainerWithSameName)
            {
                // Look through generics to the declaration underneath
                auto prevGenericDecl = dynamic_cast<GenericDecl*>(prevDecl);
                if (prevGenericDecl)
                    prevDecl = prevGenericDecl->inner.Ptr();

                // We only care about previously-declared functions
                // Note(tfoley): although we should really error out if the
                // name is already in use for something else, like a variable...
                auto prevFuncDecl = dynamic_cast<FuncDecl*>(prevDecl);
                if (!prevFuncDecl)
                    continue;

                // If the parameter signatures don't match, then don't worry
                if (!DoFunctionSignaturesMatch(funcDecl, prevFuncDecl))
                    continue;

                // If we get this far, then we've got two declarations in the same
                // scope, with the same name and signature.
                //
                // They might just be redeclarations, which we would want to allow.

                // First, check if the return types match.
                // TODO(tfolye): this code won't work for generics
                if (!funcDecl->ReturnType.Equals(prevFuncDecl->ReturnType))
                {
                    // Bad dedeclaration
                    if (!isRewriteMode())
                    {
                        getSink()->diagnose(funcDecl, Diagnostics::unimplemented, "redeclaration has a different return type");
                    }

                    // Don't bother emitting other errors at this point
                    break;
                }

                // TODO(tfoley): track the fact that there is redeclaration going on,
                // so that we can detect it and react accordingly during overload resolution
                // (e.g., by only considering one declaration as the canonical one...)

                // If both have a body, then there is trouble
                if (funcDecl->Body && prevFuncDecl->Body)
                {
                    // Redefinition
                    if (!isRewriteMode())
                    {
                        getSink()->diagnose(funcDecl, Diagnostics::unimplemented, "function redefinition");
                    }

                    // Don't bother emitting other errors
                    break;
                }

                // TODO(tfoley): If both specific default argument expressions
                // for the same value, then that is an error too...
            }
        }

        void visitScopeDecl(ScopeDecl*)
        {
            // Nothing to do
        }

        void visitParamDecl(ParamDecl* para)
        {
            // TODO: This needs to bottleneck through the common variable checks

            para->type = CheckUsableType(para->type);
            if (para->type.Equals(getSession()->getVoidType()))
            {
                if (!isRewriteMode())
                {
                    getSink()->diagnose(para, Diagnostics::parameterCannotBeVoid);
                }
            }
        }

        void VisitFunctionDeclaration(FuncDecl *functionNode)
        {
            if (functionNode->IsChecked(DeclCheckState::CheckedHeader)) return;
            functionNode->SetCheckState(DeclCheckState::CheckingHeader);

            this->function = functionNode;
            auto returnType = CheckProperType(functionNode->ReturnType);
            functionNode->ReturnType = returnType;
            HashSet<Name*> paraNames;
            for (auto & para : functionNode->GetParameters())
            {
                checkDecl(para);

                if (paraNames.Contains(para->getName()))
                {
                    if (!isRewriteMode())
                    {
                        getSink()->diagnose(para, Diagnostics::parameterAlreadyDefined, para->getName());
                    }
                }
                else
                    paraNames.Add(para->getName());
            }
            this->function = NULL;
            functionNode->SetCheckState(DeclCheckState::CheckedHeader);

            // One last bit of validation: check if we are redeclaring an existing function
            ValidateFunctionRedeclaration(functionNode);
        }

        void visitDeclStmt(DeclStmt* stmt)
        {
            // We directly dispatch here instead of using `EnsureDecl()` for two
            // reasons:
            //
            // 1. We expect that a local declaration won't have been referenced
            // before it is declared, so that we can just check things in-order
            //
            // 2. `EnsureDecl()` is specialized for `Decl*` instead of `DeclBase*`
            // and trying to special case `DeclGroup*` here feels silly.
            //
            DeclVisitor::dispatch(stmt->decl);
        }

        void visitBlockStmt(BlockStmt* stmt)
        {
            checkStmt(stmt->body);
        }

        void visitSeqStmt(SeqStmt* stmt)
        {
            for(auto ss : stmt->stmts)
            {
                checkStmt(ss);
            }
        }

        template<typename T>
        T* FindOuterStmt()
        {
            UInt outerStmtCount = outerStmts.Count();
            for (UInt ii = outerStmtCount; ii > 0; --ii)
            {
                auto outerStmt = outerStmts[ii-1];
                auto found = dynamic_cast<T*>(outerStmt);
                if (found)
                    return found;
            }
            return nullptr;
        }

        void visitBreakStmt(BreakStmt *stmt)
        {
            auto outer = FindOuterStmt<BreakableStmt>();
            if (!outer)
            {
                if (!isRewriteMode())
                {
                    getSink()->diagnose(stmt, Diagnostics::breakOutsideLoop);
                }
            }
            stmt->parentStmt = outer;
        }
        void visitContinueStmt(ContinueStmt *stmt)
        {
            auto outer = FindOuterStmt<LoopStmt>();
            if (!outer)
            {
                if (!isRewriteMode())
                {
                    getSink()->diagnose(stmt, Diagnostics::continueOutsideLoop);
                }
            }
            stmt->parentStmt = outer;
        }

        void PushOuterStmt(Stmt* stmt)
        {
            outerStmts.Add(stmt);
        }

        void PopOuterStmt(Stmt* /*stmt*/)
        {
            outerStmts.RemoveAt(outerStmts.Count() - 1);
        }

        RefPtr<Expr> checkPredicateExpr(Expr* expr)
        {
            RefPtr<Expr> e = expr;
            e = CheckTerm(e);
            e = Coerce(getSession()->getBoolType(), e);
            return e;
        }

        void visitDoWhileStmt(DoWhileStmt *stmt)
        {
            PushOuterStmt(stmt);
            stmt->Predicate = checkPredicateExpr(stmt->Predicate);
            checkStmt(stmt->Statement);

            PopOuterStmt(stmt);
        }
        void visitForStmt(ForStmt *stmt)
        {
            PushOuterStmt(stmt);
            checkStmt(stmt->InitialStatement);
            if (stmt->PredicateExpression)
            {
                stmt->PredicateExpression = checkPredicateExpr(stmt->PredicateExpression);
            }
            if (stmt->SideEffectExpression)
            {
                stmt->SideEffectExpression = CheckExpr(stmt->SideEffectExpression);
            }
            checkStmt(stmt->Statement);

            PopOuterStmt(stmt);
        }

        RefPtr<Expr> checkExpressionAndExpectIntegerConstant(RefPtr<Expr> expr, RefPtr<IntVal>* outIntVal)
        {
            expr = CheckExpr(expr);
            auto intVal = CheckIntegerConstantExpression(expr);
            if (outIntVal)
                *outIntVal = intVal;
            return expr;
        }

        void visitCompileTimeForStmt(CompileTimeForStmt* stmt)
        {
            PushOuterStmt(stmt);

            stmt->varDecl->type.type = getSession()->getIntType();
            addModifier(stmt->varDecl, new ConstModifier());

            RefPtr<IntVal> rangeBeginVal;
            RefPtr<IntVal> rangeEndVal;

            if (stmt->rangeBeginExpr)
            {
                stmt->rangeBeginExpr = checkExpressionAndExpectIntegerConstant(stmt->rangeBeginExpr, &rangeBeginVal);
            }
            else
            {
                RefPtr<ConstantIntVal> rangeBeginConst = new ConstantIntVal();
                rangeBeginConst->value = 0;
                rangeBeginVal = rangeBeginConst;
            }

            stmt->rangeEndExpr = checkExpressionAndExpectIntegerConstant(stmt->rangeEndExpr, &rangeEndVal);

            stmt->rangeBeginVal = rangeBeginVal;
            stmt->rangeEndVal = rangeEndVal;

            checkStmt(stmt->body);


            PopOuterStmt(stmt);
        }

        void visitSwitchStmt(SwitchStmt* stmt)
        {
            PushOuterStmt(stmt);
            // TODO(tfoley): need to coerce condition to an integral type...
            stmt->condition = CheckExpr(stmt->condition);
            checkStmt(stmt->body);

            // TODO(tfoley): need to check that all case tags are unique

            // TODO(tfoley): check that there is at most one `default` clause

            PopOuterStmt(stmt);
        }
        void visitCaseStmt(CaseStmt* stmt)
        {
            // TODO(tfoley): Need to coerce to type being switch on,
            // and ensure that value is a compile-time constant
            auto expr = CheckExpr(stmt->expr);
            auto switchStmt = FindOuterStmt<SwitchStmt>();

            if (!switchStmt)
            {
                if (!isRewriteMode())
                {
                    getSink()->diagnose(stmt, Diagnostics::caseOutsideSwitch);
                }
            }
            else
            {
                // TODO: need to do some basic matching to ensure the type
                // for the `case` is consistent with the type for the `switch`...
            }

            stmt->expr = expr;
            stmt->parentStmt = switchStmt;
        }
        void visitDefaultStmt(DefaultStmt* stmt)
        {
            auto switchStmt = FindOuterStmt<SwitchStmt>();
            if (!switchStmt)
            {
                if (!isRewriteMode())
                {
                    getSink()->diagnose(stmt, Diagnostics::defaultOutsideSwitch);
                }
            }
            stmt->parentStmt = switchStmt;
        }
        void visitIfStmt(IfStmt *stmt)
        {
            stmt->Predicate = checkPredicateExpr(stmt->Predicate);
            checkStmt(stmt->PositiveStatement);
            checkStmt(stmt->NegativeStatement);
        }

        void visitUnparsedStmt(UnparsedStmt*)
        {
            // Nothing to do
        }

        void visitEmptyStmt(EmptyStmt*)
        {
            // Nothing to do
        }

        void visitDiscardStmt(DiscardStmt*)
        {
            // Nothing to do
        }

        void visitReturnStmt(ReturnStmt *stmt)
        {
            if (!stmt->Expression)
            {
                if (function && !function->ReturnType.Equals(getSession()->getVoidType()))
                {
                    if (!isRewriteMode())
                    {
                        getSink()->diagnose(stmt, Diagnostics::returnNeedsExpression);
                    }
                }
            }
            else
            {
                stmt->Expression = CheckTerm(stmt->Expression);
                if (!stmt->Expression->type->Equals(getSession()->getErrorType()))
                {
                    if (function)
                    {
                        stmt->Expression = Coerce(function->ReturnType.Ptr(), stmt->Expression);
                    }
                    else
                    {
                        // TODO(tfoley): this case currently gets triggered for member functions,
                        // which aren't being checked consistently (because of the whole symbol
                        // table idea getting in the way).

//							getSink()->diagnose(stmt, Diagnostics::unimplemented, "case for return stmt");
                    }
                }
            }
        }

        IntegerLiteralValue GetMinBound(RefPtr<IntVal> val)
        {
            if (auto constantVal = val.As<ConstantIntVal>())
                return constantVal->value;

            // TODO(tfoley): Need to track intervals so that this isn't just a lie...
            return 1;
        }

        void maybeInferArraySizeForVariable(Variable* varDecl)
        {
            // Not an array?
            auto arrayType = varDecl->type->AsArrayType();
            if (!arrayType) return;

            // Explicit element count given?
            auto elementCount = arrayType->ArrayLength;
            if (elementCount) return;

            // No initializer?
            auto initExpr = varDecl->initExpr;
            if(!initExpr) return;

            // Is the initializer an initializer list?
            if(auto initializerListExpr = initExpr.As<InitializerListExpr>())
            {
                auto argCount = initializerListExpr->args.Count();
                elementCount = new ConstantIntVal(argCount);
            }
            // Is the type of the initializer an array type?
            else if(auto arrayInitType = initExpr->type->As<ArrayExpressionType>())
            {
                elementCount = arrayInitType->ArrayLength;
            }
            else
            {
                // Nothing to do: we couldn't infer a size
                return;
            }

            // Create a new array type based on the size we found,
            // and install it into our type.
            varDecl->type.type = getArrayType(
                arrayType->baseType,
                elementCount);
        }

        void ValidateArraySizeForVariable(Variable* varDecl)
        {
            auto arrayType = varDecl->type->AsArrayType();
            if (!arrayType) return;

            auto elementCount = arrayType->ArrayLength;
            if (!elementCount)
            {
                // Note(tfoley): For now we allow arrays of unspecified size
                // everywhere, because some source languages (e.g., GLSL)
                // allow them in specific cases.
#if 0
                getSink()->diagnose(varDecl, Diagnostics::invalidArraySize);
#endif
                return;
            }

            // TODO(tfoley): How to handle the case where bound isn't known?
            if (GetMinBound(elementCount) <= 0)
            {
                if (!isRewriteMode())
                {
                    getSink()->diagnose(varDecl, Diagnostics::invalidArraySize);
                }
                return;
            }
        }

        void visitVariable(Variable* varDecl)
        {
            TypeExp typeExp = CheckUsableType(varDecl->type);
#if 0
            if (typeExp.type->GetBindableResourceType() != BindableResourceType::NonBindable)
            {
                // We don't want to allow bindable resource types as local variables (at least for now).
                auto parentDecl = varDecl->ParentDecl;
                if (auto parentScopeDecl = dynamic_cast<ScopeDecl*>(parentDecl))
                {
                    getSink()->diagnose(varDecl->type, Diagnostics::invalidTypeForLocalVariable);
                }
            }
#endif
            varDecl->type = typeExp;
            if (varDecl->type.Equals(getSession()->getVoidType()))
            {
                if (!isRewriteMode())
                {
                    getSink()->diagnose(varDecl, Diagnostics::invalidTypeVoid);
                }
            }

            if(auto initExpr = varDecl->initExpr)
            {
                initExpr = CheckTerm(initExpr);
                varDecl->initExpr = initExpr;
            }

            // If this is an array variable, then we first want to give
            // it a chance to infer an array size from its initializer
            //
            // TODO(tfoley): May need to extend this to handle the
            // multi-dimensional case...
            maybeInferArraySizeForVariable(varDecl);
            //
            // Next we want to make sure that the declared (or inferred)
            // size for the array meets whatever language-specific
            // constraints we want to enforce (e.g., disallow empty
            // arrays in specific cases)
            ValidateArraySizeForVariable(varDecl);


            if(auto initExpr = varDecl->initExpr)
            {
                // TODO(tfoley): should coercion of initializer lists be special-cased
                // here, or handled as a general case for coercion?

                initExpr = Coerce(varDecl->type.Ptr(), initExpr);
                varDecl->initExpr = initExpr;
            }

            varDecl->SetCheckState(DeclCheckState::Checked);
        }

        void visitWhileStmt(WhileStmt *stmt)
        {
            PushOuterStmt(stmt);
            stmt->Predicate = checkPredicateExpr(stmt->Predicate);
            checkStmt(stmt->Statement);
            PopOuterStmt(stmt);
        }
        void visitExpressionStmt(ExpressionStmt *stmt)
        {
            stmt->Expression = CheckExpr(stmt->Expression);
        }

        RefPtr<Expr> visitConstantExpr(ConstantExpr *expr)
        {
            // The expression might already have a type, determined by its suffix
            if(expr->type.type)
                return expr;

            switch (expr->ConstType)
            {
            case ConstantExpr::ConstantType::Int:
                expr->type = getSession()->getIntType();
                break;
            case ConstantExpr::ConstantType::Bool:
                expr->type = getSession()->getBoolType();
                break;
            case ConstantExpr::ConstantType::Float:
                expr->type = getSession()->getFloatType();
                break;
            default:
                expr->type = QualType(getSession()->getErrorType());
                throw "Invalid constant type.";
                break;
            }
            return expr;
        }

        IntVal* GetIntVal(ConstantExpr* expr)
        {
            // TODO(tfoley): don't keep allocating here!
            return new ConstantIntVal(expr->integerValue);
        }

        Name* getName(String const& text)
        {
            return getCompileRequest()->getNamePool()->getName(text);
        }

        RefPtr<IntVal> TryConstantFoldExpr(
            InvokeExpr* invokeExpr)
        {
            // We need all the operands to the expression

            // Check if the callee is an operation that is amenable to constant-folding.
            //
            // For right now we will look for calls to intrinsic functions, and then inspect
            // their names (this is bad and slow).
            auto funcDeclRefExpr = invokeExpr->FunctionExpr.As<DeclRefExpr>();
            if (!funcDeclRefExpr) return nullptr;

            auto funcDeclRef = funcDeclRefExpr->declRef;
            auto intrinsicMod = funcDeclRef.getDecl()->FindModifier<IntrinsicOpModifier>();
            if (!intrinsicMod) return nullptr;

            // Let's not constant-fold operations with more than a certain number of arguments, for simplicity
            static const int kMaxArgs = 8;
            if (invokeExpr->Arguments.Count() > kMaxArgs)
                return nullptr;

            // Before checking the operation name, let's look at the arguments
            RefPtr<IntVal> argVals[kMaxArgs];
            IntegerLiteralValue constArgVals[kMaxArgs];
            int argCount = 0;
            bool allConst = true;
            for (auto argExpr : invokeExpr->Arguments)
            {
                auto argVal = TryCheckIntegerConstantExpression(argExpr.Ptr());
                if (!argVal)
                    return nullptr;

                argVals[argCount] = argVal;

                if (auto constArgVal = argVal.As<ConstantIntVal>())
                {
                    constArgVals[argCount] = constArgVal->value;
                }
                else
                {
                    allConst = false;
                }
                argCount++;
            }

            if (!allConst)
            {
                // TODO(tfoley): We probably want to support a very limited number of operations
                // on "constants" that aren't actually known, to be able to handle a generic
                // that takes an integer `N` but then constructs a vector of size `N+1`.
                //
                // The hard part there is implementing the rules for value unification in the
                // presence of more complicated `IntVal` subclasses, like `SumIntVal`. You'd
                // need inference to be smart enough to know that `2 + N` and `N + 2` are the
                // same value, as are `N + M + 1 + 1` and `M + 2 + N`.
                //
                // For now we can just bail in this case.
                return nullptr;
            }

            // At this point, all the operands had simple integer values, so we are golden.
            IntegerLiteralValue resultValue = 0;
            auto opName = funcDeclRef.GetName();

            // handle binary operators
            if (opName == getName("-"))
            {
                if (argCount == 1)
                {
                    resultValue = -constArgVals[0];
                }
                else if (argCount == 2)
                {
                    resultValue = constArgVals[0] - constArgVals[1];
                }
            }

            // simple binary operators
#define CASE(OP)                                                    \
            else if(opName == getName(#OP)) do {                    \
                if(argCount != 2) return nullptr;                   \
                resultValue = constArgVals[0] OP constArgVals[1];   \
            } while(0)

            CASE(+); // TODO: this can also be unary...
            CASE(*);
#undef CASE

            // binary operators with chance of divide-by-zero
            // TODO: issue a suitable error in that case
#define CASE(OP)                                                    \
            else if(opName == getName(#OP)) do {                    \
                if(argCount != 2) return nullptr;                   \
                if(!constArgVals[1]) return nullptr;                \
                resultValue = constArgVals[0] OP constArgVals[1];   \
            } while(0)

            CASE(/);
            CASE(%);
#undef CASE

            // TODO(tfoley): more cases
            else
            {
                return nullptr;
            }

            RefPtr<IntVal> result = new ConstantIntVal(resultValue);
            return result;
        }

        RefPtr<IntVal> TryConstantFoldExpr(
            Expr* expr)
        {
            // Unwrap any "identity" expressions
            while (auto parenExpr = dynamic_cast<ParenExpr*>(expr))
            {
                expr = parenExpr->base;
            }

            // TODO(tfoley): more serious constant folding here
            if (auto constExp = dynamic_cast<ConstantExpr*>(expr))
            {
                return GetIntVal(constExp);
            }

            // it is possible that we are referring to a generic value param
            if (auto declRefExpr = dynamic_cast<DeclRefExpr*>(expr))
            {
                auto declRef = declRefExpr->declRef;

                if (auto genericValParamRef = declRef.As<GenericValueParamDecl>())
                {
                    // TODO(tfoley): handle the case of non-`int` value parameters...
                    return new GenericParamIntVal(genericValParamRef);
                }

                // We may also need to check for references to variables that
                // are defined in a way that can be used as a constant expression:
                if(auto varRef = declRef.As<VarDeclBase>())
                {
                    auto varDecl = varRef.getDecl();

                    switch(getSourceLanguage())
                    {
                    case SourceLanguage::Slang:
                    case SourceLanguage::HLSL:
                        // HLSL: `static const` is used to mark compile-time constant expressions
                        if(auto staticAttr = varDecl->FindModifier<HLSLStaticModifier>())
                        {
                            if(auto constAttr = varDecl->FindModifier<ConstModifier>())
                            {
                                // HLSL `static const` can be used as a constant expression
                                if(auto initExpr = getInitExpr(varRef))
                                {
                                    return TryConstantFoldExpr(initExpr.Ptr());
                                }
                            }
                        }
                        break;

                    case SourceLanguage::GLSL:
                        // GLSL: `const` indicates compile-time constant expression
                        //
                        // TODO(tfoley): The current logic here isn't robust against
                        // GLSL "specialization constants" - we will extract the
                        // initializer for a `const` variable and use it to extract
                        // a value, when we really should be using an opaque
                        // reference to the variable.
                        if(auto constAttr = varDecl->FindModifier<ConstModifier>())
                        {
                            // We need to handle a "specialization constant" (with a `constant_id` layout modifier)
                            // differently from an ordinary compile-time constant. The latter can/should be reduced
                            // to a value, while the former should be kept as a symbolic reference

                            if(auto constantIDModifier = varDecl->FindModifier<GLSLConstantIDLayoutModifier>())
                            {
                                // Retain the specialization constant as a symbolic reference
                                //
                                // TODO(tfoley): handle the case of non-`int` value parameters...
                                //
                                // TODO(tfoley): this is cloned from the case above that handles generic value parameters
                                return new GenericParamIntVal(varRef);
                            }
                            else if(auto initExpr = getInitExpr(varRef))
                            {
                                // This is an ordinary constant, and not a specialization constant, so we
                                // can try to fold its value right now.
                                return TryConstantFoldExpr(initExpr.Ptr());
                            }
                        }
                        break;
                    }

                }
            }

            if (auto invokeExpr = dynamic_cast<InvokeExpr*>(expr))
            {
                auto val = TryConstantFoldExpr(invokeExpr);
                if (val)
                    return val;
            }
            else if(auto castExpr = dynamic_cast<TypeCastExpr*>(expr))
            {
                auto val = TryConstantFoldExpr(castExpr->Arguments[0].Ptr());
                if(val)
                    return val;
            }

            return nullptr;
        }

        // Try to check an integer constant expression, either returning the value,
        // or NULL if the expression isn't recognized as a constant.
        RefPtr<IntVal> TryCheckIntegerConstantExpression(Expr* exp)
        {
            if (!exp->type.type->Equals(getSession()->getIntType()))
            {
                return nullptr;
            }



            // Otherwise, we need to consider operations that we might be able to constant-fold...
            return TryConstantFoldExpr(exp);
        }

        // Enforce that an expression resolves to an integer constant, and get its value
        RefPtr<IntVal> CheckIntegerConstantExpression(Expr* inExpr)
        {
            // First coerce the expression to the expected type
            auto expr = Coerce(getSession()->getIntType(),inExpr);
            auto result = TryCheckIntegerConstantExpression(expr.Ptr());
            if (!result)
            {
                if (!isRewriteMode())
                {
                    getSink()->diagnose(expr, Diagnostics::expectedIntegerConstantNotConstant);
                }
            }
            return result;
        }



        RefPtr<Expr> CheckSimpleSubscriptExpr(
            RefPtr<IndexExpr>   subscriptExpr,
            RefPtr<Type>              elementType)
        {
            auto baseExpr = subscriptExpr->BaseExpression;
            auto indexExpr = subscriptExpr->IndexExpression;

            if (!indexExpr->type->Equals(getSession()->getIntType()) &&
                !indexExpr->type->Equals(getSession()->getUIntType()))
            {
                if (!isRewriteMode())
                {
                    getSink()->diagnose(indexExpr, Diagnostics::subscriptIndexNonInteger);
                }
                return CreateErrorExpr(subscriptExpr.Ptr());
            }

            subscriptExpr->type = QualType(elementType);

            // TODO(tfoley): need to be more careful about this stuff
            subscriptExpr->type.IsLeftValue = baseExpr->type.IsLeftValue;

            return subscriptExpr;
        }

        // The way that we have designed out type system, pretyt much *every*
        // type is a reference to some declaration in the standard library.
        // That means that when we construct a new type on the fly, we need
        // to make sure that it is wired up to reference the appropriate
        // declaration, or else it won't compare as equal to other types
        // that *do* reference the declaration.
        //
        // This function is used to construct a `vector<T,N>` type
        // programmatically, so that it will work just like a type of
        // that form constructed by the user.
        RefPtr<VectorExpressionType> createVectorType(
            RefPtr<Type>  elementType,
            RefPtr<IntVal>          elementCount)
        {
            auto session = getSession();
            auto vectorGenericDecl = findMagicDecl(
                session, "Vector").As<GenericDecl>();
            auto vectorTypeDecl = vectorGenericDecl->inner;
               
            auto substitutions = new Substitutions();
            substitutions->genericDecl = vectorGenericDecl.Ptr();
            substitutions->args.Add(elementType);
            substitutions->args.Add(elementCount);

            auto declRef = DeclRef<Decl>(vectorTypeDecl.Ptr(), substitutions);

            return DeclRefType::Create(
                session,
                declRef)->As<VectorExpressionType>();
        }

        RefPtr<Expr> visitIndexExpr(IndexExpr* subscriptExpr)
        {
            auto baseExpr = subscriptExpr->BaseExpression;
            baseExpr = CheckExpr(baseExpr);

            RefPtr<Expr> indexExpr = subscriptExpr->IndexExpression;
            if (indexExpr)
            {
                indexExpr = CheckExpr(indexExpr);
            }

            subscriptExpr->BaseExpression = baseExpr;
            subscriptExpr->IndexExpression = indexExpr;

            // If anything went wrong in the base expression,
            // then just move along...
            if (IsErrorExpr(baseExpr))
                return CreateErrorExpr(subscriptExpr);

            // Otherwise, we need to look at the type of the base expression,
            // to figure out how subscripting should work.
            auto baseType = baseExpr->type.Ptr();
            if (auto baseTypeType = baseType->As<TypeType>())
            {
                // We are trying to "index" into a type, so we have an expression like `float[2]`
                // which should be interpreted as resolving to an array type.

                RefPtr<IntVal> elementCount = nullptr;
                if (indexExpr)
                {
                    elementCount = CheckIntegerConstantExpression(indexExpr.Ptr());
                }

                auto elementType = CoerceToUsableType(TypeExp(baseExpr, baseTypeType->type));
                auto arrayType = getArrayType(
                    elementType,
                    elementCount);

                typeResult = arrayType;
                subscriptExpr->type = QualType(getTypeType(arrayType));
                return subscriptExpr;
            }
            else if (auto baseArrayType = baseType->As<ArrayExpressionType>())
            {
                return CheckSimpleSubscriptExpr(
                    subscriptExpr,
                    baseArrayType->baseType);
            }
            else if (auto vecType = baseType->As<VectorExpressionType>())
            {
                return CheckSimpleSubscriptExpr(
                    subscriptExpr,
                    vecType->elementType);
            }
            else if (auto matType = baseType->As<MatrixExpressionType>())
            {
                // TODO(tfoley): We shouldn't go and recompute
                // row types over and over like this... :(
                auto rowType = createVectorType(
                    matType->getElementType(),
                    matType->getColumnCount());

                return CheckSimpleSubscriptExpr(
                    subscriptExpr,
                    rowType);
            }

            // Default behavior is to look at all available `__subscript`
            // declarations on the type and try to call one of them.

            if (auto declRefType = baseType->AsDeclRefType())
            {
                if (auto aggTypeDeclRef = declRefType->declRef.As<AggTypeDecl>())
                {
                    // Checking of the type must be complete before we can reference its members safely
                    EnsureDecl(aggTypeDeclRef.getDecl(), DeclCheckState::Checked);

                    // Note(tfoley): The name used for lookup here is a bit magical, since
                    // it must match what the parser installed in subscript declarations.
                    LookupResult lookupResult = LookUpLocal(
                        getSession(),
                        this, getName("operator[]"), aggTypeDeclRef);
                    if (!lookupResult.isValid())
                    {
                        goto fail;
                    }

                    RefPtr<Expr> subscriptFuncExpr = createLookupResultExpr(
                        lookupResult, subscriptExpr->BaseExpression, subscriptExpr->loc);

                    // Now that we know there is at least one subscript member,
                    // we will construct a reference to it and try to call it

                    RefPtr<InvokeExpr> subscriptCallExpr = new InvokeExpr();
                    subscriptCallExpr->loc = subscriptExpr->loc;
                    subscriptCallExpr->FunctionExpr = subscriptFuncExpr;

                    // TODO(tfoley): This path can support multiple arguments easily
                    subscriptCallExpr->Arguments.Add(subscriptExpr->IndexExpression);

                    return CheckInvokeExprWithCheckedOperands(subscriptCallExpr.Ptr());
                }
            }

        fail:
            {
                if (!isRewriteMode())
                {
                    getSink()->diagnose(subscriptExpr, Diagnostics::subscriptNonArray, baseType);
                }
                return CreateErrorExpr(subscriptExpr);
            }
        }

        bool MatchArguments(FuncDecl * functionNode, List <RefPtr<Expr>> &args)
        {
            if (functionNode->GetParameters().Count() != args.Count())
                return false;
            int i = 0;
            for (auto param : functionNode->GetParameters())
            {
                if (!param->type.Equals(args[i]->type.Ptr()))
                    return false;
                i++;
            }
            return true;
        }

        // Coerce an expression to a specific  type that it is expected to have in context
        RefPtr<Expr> CoerceExprToType(
            RefPtr<Expr>	expr,
            RefPtr<Type>			type)
        {
            // TODO(tfoley): clean this up so there is only one version...
            return Coerce(type, expr);
        }

        RefPtr<Expr> visitParenExpr(ParenExpr* expr)
        {
            auto base = expr->base;
            base = CheckTerm(base);

            expr->base = base;
            expr->type = base->type;
            return expr;
        }

        //

        RefPtr<Expr> visitAssignExpr(AssignExpr* expr)
        {
            expr->left = CheckExpr(expr->left);

            auto type = expr->left->type;

            expr->right = Coerce(type, CheckTerm(expr->right));

            if (!type.IsLeftValue)
            {
                if (type->As<ErrorType>())
                {
                    // Don't report an l-value issue on an errorneous expression
                }
                else if (!isRewriteMode())
                {
                    getSink()->diagnose(expr, Diagnostics::assignNonLValue);
                }
            }
            expr->type = type;
            return expr;
        }


        //

        void visitExtensionDecl(ExtensionDecl* decl)
        {
            if (decl->IsChecked(DeclCheckState::Checked)) return;

            decl->SetCheckState(DeclCheckState::CheckingHeader);
            decl->targetType = CheckProperType(decl->targetType);

            // TODO: need to check that the target type names a declaration...

            if (auto targetDeclRefType = decl->targetType->As<DeclRefType>())
            {
                // Attach our extension to that type as a candidate...
                if (auto aggTypeDeclRef = targetDeclRefType->declRef.As<AggTypeDecl>())
                {
                    auto aggTypeDecl = aggTypeDeclRef.getDecl();
                    decl->nextCandidateExtension = aggTypeDecl->candidateExtensions;
                    aggTypeDecl->candidateExtensions = decl;
                }
                else
                {
                    if (!isRewriteMode())
                    {
                        getSink()->diagnose(decl->targetType.exp, Diagnostics::unimplemented, "expected a nominal type here");
                    }
                }
            }
            else if (decl->targetType->Equals(getSession()->getErrorType()))
            {
                // there was an error, so ignore
            }
            else
            {
                if (!isRewriteMode())
                {
                    getSink()->diagnose(decl->targetType.exp, Diagnostics::unimplemented, "expected a nominal type here");
                }
            }

            decl->SetCheckState(DeclCheckState::CheckedHeader);

            // now check the members of the extension
            for (auto m : decl->Members)
            {
                EnsureDecl(m);
            }

            decl->SetCheckState(DeclCheckState::Checked);
        }

        void visitConstructorDecl(ConstructorDecl* decl)
        {
            if (decl->IsChecked(DeclCheckState::Checked)) return;
            decl->SetCheckState(DeclCheckState::CheckingHeader);

            for (auto& paramDecl : decl->GetParameters())
            {
                paramDecl->type = CheckUsableType(paramDecl->type);
            }
            decl->SetCheckState(DeclCheckState::CheckedHeader);

            // TODO(tfoley): check body
            decl->SetCheckState(DeclCheckState::Checked);
        }


        void visitSubscriptDecl(SubscriptDecl* decl)
        {
            if (decl->IsChecked(DeclCheckState::Checked)) return;
            decl->SetCheckState(DeclCheckState::CheckingHeader);

            for (auto& paramDecl : decl->GetParameters())
            {
                paramDecl->type = CheckUsableType(paramDecl->type);
            }

            decl->ReturnType = CheckUsableType(decl->ReturnType);

            decl->SetCheckState(DeclCheckState::CheckedHeader);

            decl->SetCheckState(DeclCheckState::Checked);
        }

        void visitAccessorDecl(AccessorDecl* decl)
        {
            // TODO: check the body!

            decl->SetCheckState(DeclCheckState::Checked);
        }


        //

        struct Constraint
        {
            Decl*		decl; // the declaration of the thing being constraints
            RefPtr<Val>	val; // the value to which we are constraining it
            bool satisfied = false; // Has this constraint been met?
        };

        // A collection of constraints that will need to be satisified (solved)
        // in order for checking to suceed.
        struct ConstraintSystem
        {
            List<Constraint> constraints;
        };

        RefPtr<Type> TryJoinVectorAndScalarType(
            RefPtr<VectorExpressionType> vectorType,
            RefPtr<BasicExpressionType>  scalarType)
        {
            // Join( vector<T,N>, S ) -> vetor<Join(T,S), N>
            //
            // That is, the join of a vector and a scalar type is
            // a vector type with a joined element type.
            auto joinElementType = TryJoinTypes(
                vectorType->elementType,
                scalarType);
            if(!joinElementType)
                return nullptr;

            return createVectorType(
                joinElementType,
                vectorType->elementCount);
        }

        bool DoesTypeConformToInterface(
            RefPtr<Type>  type,
            DeclRef<InterfaceDecl>        interfaceDeclRef)
        {
            // for now look up a conformance member...
            if(auto declRefType = type->As<DeclRefType>())
            {
                if( auto aggTypeDeclRef = declRefType->declRef.As<AggTypeDecl>() )
                {
                    for( auto inheritanceDeclRef : getMembersOfType<InheritanceDecl>(aggTypeDeclRef))
                    {
                        EnsureDecl(inheritanceDeclRef.getDecl());

                        auto inheritedDeclRefType = getBaseType(inheritanceDeclRef)->As<DeclRefType>();
                        if (!inheritedDeclRefType)
                            continue;

                        if(interfaceDeclRef.Equals(inheritedDeclRefType->declRef))
                            return true;
                    }
                }
            }

            // default is failure
            return false;
        }

        RefPtr<Type> TryJoinTypeWithInterface(
            RefPtr<Type>  type,
            DeclRef<InterfaceDecl>        interfaceDeclRef)
        {
            // The most basic test here should be: does the type declare conformance to the trait.
            if(DoesTypeConformToInterface(type, interfaceDeclRef))
                return type;

            // There is a more nuanced case if `type` is a builtin type, and we need to make it
            // conform to a trait that some but not all builtin types support (the main problem
            // here is when an operation wants an integer type, but one of our operands is a `float`.
            // The HLSL rules will allow that, with implicit conversion, but our default join rules
            // will end up picking `float` and we don't want that...).

            // For now we don't handle the hard case and just bail
            return nullptr;
        }

        // Try to compute the "join" between two types
        RefPtr<Type> TryJoinTypes(
            RefPtr<Type>  left,
            RefPtr<Type>  right)
        {
            // Easy case: they are the same type!
            if (left->Equals(right))
                return left;

            // We can join two basic types by picking the "better" of the two
            if (auto leftBasic = left->As<BasicExpressionType>())
            {
                if (auto rightBasic = right->As<BasicExpressionType>())
                {
                    auto leftFlavor = leftBasic->baseType;
                    auto rightFlavor = rightBasic->baseType;

                    // TODO(tfoley): Need a special-case rule here that if
                    // either operand is of type `half`, then we promote
                    // to at least `float`

                    // Return the one that had higher rank...
                    if (leftFlavor > rightFlavor)
                        return left;
                    else
                    {
                        SLANG_ASSERT(rightFlavor > leftFlavor);
                        return right;
                    }
                }

                // We can also join a vector and a scalar
                if(auto rightVector = right->As<VectorExpressionType>())
                {
                    return TryJoinVectorAndScalarType(rightVector, leftBasic);
                }
            }

            // We can join two vector types by joining their element types
            // (and also their sizes...)
            if( auto leftVector = left->As<VectorExpressionType>())
            {
                if(auto rightVector = right->As<VectorExpressionType>())
                {
                    // Check if the vector sizes match
                    if(!leftVector->elementCount->EqualsVal(rightVector->elementCount.Ptr()))
                        return nullptr;

                    // Try to join the element types
                    auto joinElementType = TryJoinTypes(
                        leftVector->elementType,
                        rightVector->elementType);
                    if(!joinElementType)
                        return nullptr;

                    return createVectorType(
                        joinElementType,
                        leftVector->elementCount);
                }

                // We can also join a vector and a scalar
                if(auto rightBasic = right->As<BasicExpressionType>())
                {
                    return TryJoinVectorAndScalarType(leftVector, rightBasic);
                }
            }

            // HACK: trying to work trait types in here...
            if(auto leftDeclRefType = left->As<DeclRefType>())
            {
                if( auto leftInterfaceRef = leftDeclRefType->declRef.As<InterfaceDecl>() )
                {
                    // 
                    return TryJoinTypeWithInterface(right, leftInterfaceRef);
                }
            }
            if(auto rightDeclRefType = right->As<DeclRefType>())
            {
                if( auto rightInterfaceRef = rightDeclRefType->declRef.As<InterfaceDecl>() )
                {
                    // 
                    return TryJoinTypeWithInterface(left, rightInterfaceRef);
                }
            }

            // TODO: all the cases for vectors apply to matrices too!

            // Default case is that we just fail.
            return nullptr;
        }

        // Try to solve a system of generic constraints.
        // The `system` argument provides the constraints.
        // The `varSubst` argument provides the list of constraint
        // variables that were created for the system.
        //
        // Returns a new substitution representing the values that
        // we solved for along the way.
        RefPtr<Substitutions> TrySolveConstraintSystem(
            ConstraintSystem*		system,
            DeclRef<GenericDecl>          genericDeclRef)
        {
            // For now the "solver" is going to be ridiculously simplistic.

            // The generic itself will have some constraints, so we need to try and solve those too
            for( auto constraintDeclRef : getMembersOfType<GenericTypeConstraintDecl>(genericDeclRef) )
            {
                if(!TryUnifyTypes(*system, GetSub(constraintDeclRef), GetSup(constraintDeclRef)))
                    return nullptr;
            }

            // We will loop over the generic parameters, and for
            // each we will try to find a way to satisfy all
            // the constraints for that parameter
            List<RefPtr<Val>> args;
            for (auto m : getMembers(genericDeclRef))
            {
                if (auto typeParam = m.As<GenericTypeParamDecl>())
                {
                    RefPtr<Type> type = nullptr;
                    for (auto& c : system->constraints)
                    {
                        if (c.decl != typeParam.getDecl())
                            continue;

                        auto cType = c.val.As<Type>();
                        SLANG_RELEASE_ASSERT(cType.Ptr());

                        if (!type)
                        {
                            type = cType;
                        }
                        else
                        {
                            auto joinType = TryJoinTypes(type, cType);
                            if (!joinType)
                            {
                                // failure!
                                return nullptr;
                            }
                            type = joinType;
                        }

                        c.satisfied = true;
                    }

                    if (!type)
                    {
                        // failure!
                        return nullptr;
                    }
                    args.Add(type);
                }
                else if (auto valParam = m.As<GenericValueParamDecl>())
                {
                    // TODO(tfoley): maybe support more than integers some day?
                    // TODO(tfoley): figure out how this needs to interact with
                    // compile-time integers that aren't just constants...
                    RefPtr<IntVal> val = nullptr;
                    for (auto& c : system->constraints)
                    {
                        if (c.decl != valParam.getDecl())
                            continue;

                        auto cVal = c.val.As<IntVal>();
                        SLANG_RELEASE_ASSERT(cVal.Ptr());

                        if (!val)
                        {
                            val = cVal;
                        }
                        else
                        {
                            if(!val->EqualsVal(cVal.Ptr()))
                            {
                                // failure!
                                return nullptr;
                            }
                        }

                        c.satisfied = true;
                    }

                    if (!val)
                    {
                        // failure!
                        return nullptr;
                    }
                    args.Add(val);
                }
                else
                {
                    // ignore anything that isn't a generic parameter
                }
            }

            // Make sure we haven't constructed any spurious constraints
            // that we aren't able to satisfy:
            for (auto c : system->constraints)
            {
                if (!c.satisfied)
                {
                    return nullptr;
                }
            }

            // Consruct a reference to the extension with our constraint variables
            // as the 
            RefPtr<Substitutions> solvedSubst = new Substitutions();
            solvedSubst->genericDecl = genericDeclRef.getDecl();
            solvedSubst->outer = genericDeclRef.substitutions;
            solvedSubst->args = args;

            return solvedSubst;
        }

        //

        struct OverloadCandidate
        {
            enum class Flavor
            {
                Func,
                Generic,
                UnspecializedGeneric,
            };
            Flavor flavor;

            enum class Status
            {
                GenericArgumentInferenceFailed,
                Unchecked,
                ArityChecked,
                FixityChecked,
                TypeChecked,
                Appicable,
            };
            Status status = Status::Unchecked;

            // Reference to the declaration being applied
            LookupResultItem item;

            // The type of the result expression if this candidate is selected
            RefPtr<Type>	resultType;

            // A system for tracking constraints introduced on generic parameters
            ConstraintSystem constraintSystem;

            // How much conversion cost should be considered for this overload,
            // when ranking candidates.
            ConversionCost conversionCostSum = kConversionCost_None;
        };



        // State related to overload resolution for a call
        // to an overloaded symbol
        struct OverloadResolveContext
        {
            enum class Mode
            {
                // We are just checking if a candidate works or not
                JustTrying,

                // We want to actually update the AST for a chosen candidate
                ForReal,
            };

            // Location to use when reporting overload-resolution errors.
            SourceLoc loc;

            // The original expression (if any) that triggered things
            RefPtr<Expr> originalExpr;

            // Source location of the "function" part of the expression, if any
            SourceLoc       funcLoc;

            // The original arguments to the call
            UInt argCount = 0;
            RefPtr<Expr>* args = nullptr;
            RefPtr<Type>* argTypes = nullptr;

            UInt getArgCount() { return argCount; }
            RefPtr<Expr>& getArg(UInt index) { return args[index]; }
            RefPtr<Type>& getArgType(UInt index)
            {
                if(argTypes)
                    return argTypes[index];
                else
                    return getArg(index)->type.type;
            }

            bool disallowNestedConversions = false;

            RefPtr<Expr> baseExpr;

            // Are we still trying out candidates, or are we
            // checking the chosen one for real?
            Mode mode = Mode::JustTrying;

            // We store one candidate directly, so that we don't
            // need to do dynamic allocation on the list every time
            OverloadCandidate bestCandidateStorage;
            OverloadCandidate*	bestCandidate = nullptr;

            // Full list of all candidates being considered, in the ambiguous case
            List<OverloadCandidate> bestCandidates;
        };

        struct ParamCounts
        {
            UInt required;
            UInt allowed;
        };

        // count the number of parameters required/allowed for a callable
        ParamCounts CountParameters(FilteredMemberRefList<ParamDecl> params)
        {
            ParamCounts counts = { 0, 0 };
            for (auto param : params)
            {
                counts.allowed++;

                // No initializer means no default value
                //
                // TODO(tfoley): The logic here is currently broken in two ways:
                //
                // 1. We are assuming that once one parameter has a default, then all do.
                //    This can/should be validated earlier, so that we can assume it here.
                //
                // 2. We are not handling the possibility of multiple declarations for
                //    a single function, where we'd need to merge default parameters across
                //    all the declarations.
                if (!param.getDecl()->initExpr)
                {
                    counts.required++;
                }
            }
            return counts;
        }

        // count the number of parameters required/allowed for a generic
        ParamCounts CountParameters(DeclRef<GenericDecl> genericRef)
        {
            ParamCounts counts = { 0, 0 };
            for (auto m : genericRef.getDecl()->Members)
            {
                if (auto typeParam = m.As<GenericTypeParamDecl>())
                {
                    counts.allowed++;
                    if (!typeParam->initType.Ptr())
                    {
                        counts.required++;
                    }
                }
                else if (auto valParam = m.As<GenericValueParamDecl>())
                {
                    counts.allowed++;
                    if (!valParam->initExpr)
                    {
                        counts.required++;
                    }
                }
            }
            return counts;
        }

        bool TryCheckOverloadCandidateArity(
            OverloadResolveContext&		context,
            OverloadCandidate const&	candidate)
        {
            UInt argCount = context.getArgCount();
            ParamCounts paramCounts = { 0, 0 };
            switch (candidate.flavor)
            {
            case OverloadCandidate::Flavor::Func:
                paramCounts = CountParameters(GetParameters(candidate.item.declRef.As<CallableDecl>()));
                break;

            case OverloadCandidate::Flavor::Generic:
                paramCounts = CountParameters(candidate.item.declRef.As<GenericDecl>());
                break;

            default:
                SLANG_UNEXPECTED("unknown flavor of overload candidate");
                break;
            }

            if (argCount >= paramCounts.required && argCount <= paramCounts.allowed)
                return true;

            // Emit an error message if we are checking this call for real
            if (context.mode != OverloadResolveContext::Mode::JustTrying)
            {
                if (argCount < paramCounts.required)
                {
                    if (!isRewriteMode())
                    {
                        getSink()->diagnose(context.loc, Diagnostics::notEnoughArguments, argCount, paramCounts.required);
                    }
                }
                else
                {
                    SLANG_ASSERT(argCount > paramCounts.allowed);
                    if (!isRewriteMode())
                    {
                        getSink()->diagnose(context.loc, Diagnostics::tooManyArguments, argCount, paramCounts.allowed);
                    }
                }
            }

            return false;
        }

        bool TryCheckOverloadCandidateFixity(
            OverloadResolveContext&		context,
            OverloadCandidate const&	candidate)
        {
            auto expr = context.originalExpr;

            auto decl = candidate.item.declRef.decl;

            if(auto prefixExpr = expr.As<PrefixExpr>())
            {
                if(decl->HasModifier<PrefixModifier>())
                    return true;

                if (context.mode != OverloadResolveContext::Mode::JustTrying)
                {
                    if (!isRewriteMode())
                    {
                        getSink()->diagnose(context.loc, Diagnostics::expectedPrefixOperator);
                        getSink()->diagnose(decl, Diagnostics::seeDefinitionOf, decl->getName());
                    }
                }

                return false;
            }
            else if(auto postfixExpr = expr.As<PostfixExpr>())
            {
                if(decl->HasModifier<PostfixModifier>())
                    return true;

                if (context.mode != OverloadResolveContext::Mode::JustTrying)
                {
                    if (!isRewriteMode())
                    {
                        getSink()->diagnose(context.loc, Diagnostics::expectedPostfixOperator);
                        getSink()->diagnose(decl, Diagnostics::seeDefinitionOf, decl->getName());
                    }
                }

                return false;
            }
            else
            {
                return true;
            }

            return false;
        }

        bool TryCheckGenericOverloadCandidateTypes(
            OverloadResolveContext&	context,
            OverloadCandidate&		candidate)
        {
            auto genericDeclRef = candidate.item.declRef.As<GenericDecl>();

            int aa = 0;
            for (auto memberRef : getMembers(genericDeclRef))
            {
                if (auto typeParamRef = memberRef.As<GenericTypeParamDecl>())
                {
                    auto arg = context.getArg(aa++);

                    if (context.mode == OverloadResolveContext::Mode::JustTrying)
                    {
                        if (!CanCoerceToProperType(TypeExp(arg)))
                        {
                            return false;
                        }
                    }
                    else
                    {
                        TypeExp typeExp = CoerceToProperType(TypeExp(arg));
                    }
                }
                else if (auto valParamRef = memberRef.As<GenericValueParamDecl>())
                {
                    auto arg = context.getArg(aa++);

                    if (context.mode == OverloadResolveContext::Mode::JustTrying)
                    {
                        ConversionCost cost = kConversionCost_None;
                        if (!CanCoerce(GetType(valParamRef), arg->type, &cost))
                        {
                            return false;
                        }
                        candidate.conversionCostSum += cost;
                    }
                    else
                    {
                        arg = Coerce(GetType(valParamRef), arg);
                        auto val = ExtractGenericArgInteger(arg);
                    }
                }
                else
                {
                    continue;
                }
            }

            return true;
        }

        bool TryCheckOverloadCandidateTypes(
            OverloadResolveContext&	context,
            OverloadCandidate&		candidate)
        {
            UInt argCount = context.getArgCount();

            List<DeclRef<ParamDecl>> params;
            switch (candidate.flavor)
            {
            case OverloadCandidate::Flavor::Func:
                params = GetParameters(candidate.item.declRef.As<CallableDecl>()).ToArray();
                break;

            case OverloadCandidate::Flavor::Generic:
                return TryCheckGenericOverloadCandidateTypes(context, candidate);

            default:
                SLANG_UNEXPECTED("unknown flavor of overload candidate");
                break;
            }

            // Note(tfoley): We might have fewer arguments than parameters in the
            // case where one or more parameters had defaults.
            SLANG_RELEASE_ASSERT(argCount <= params.Count());

            for (UInt ii = 0; ii < argCount; ++ii)
            {
                auto& arg = context.getArg(ii);
                auto argType = context.getArgType(ii);
                auto param = params[ii];

                if (context.mode == OverloadResolveContext::Mode::JustTrying)
                {
                    ConversionCost cost = kConversionCost_None;
                    if( context.disallowNestedConversions )
                    {
                        // We need an exact match in this case.
                        if(!GetType(param)->Equals(argType))
                            return false;
                    }
                    else if (!CanCoerce(GetType(param), argType, &cost))
                    {
                        return false;
                    }
                    candidate.conversionCostSum += cost;
                }
                else
                {
                    arg = Coerce(GetType(param), arg);
                }
            }
            return true;
        }

        bool TryCheckOverloadCandidateDirections(
            OverloadResolveContext&		/*context*/,
            OverloadCandidate const&	/*candidate*/)
        {
            // TODO(tfoley): check `in` and `out` markers, as needed.
            return true;
        }

        // Try to check an overload candidate, but bail out
        // if any step fails
        void TryCheckOverloadCandidate(
            OverloadResolveContext&		context,
            OverloadCandidate&			candidate)
        {
            if (!TryCheckOverloadCandidateArity(context, candidate))
                return;

            candidate.status = OverloadCandidate::Status::ArityChecked;
            if (!TryCheckOverloadCandidateFixity(context, candidate))
                return;

            candidate.status = OverloadCandidate::Status::FixityChecked;
            if (!TryCheckOverloadCandidateTypes(context, candidate))
                return;

            candidate.status = OverloadCandidate::Status::TypeChecked;
            if (!TryCheckOverloadCandidateDirections(context, candidate))
                return;

            candidate.status = OverloadCandidate::Status::Appicable;
        }

        // Create the representation of a given generic applied to some arguments
        RefPtr<Expr> CreateGenericDeclRef(
            RefPtr<Expr>        baseExpr,
            RefPtr<Expr>        originalExpr,
            UInt                argCount,
            RefPtr<Expr> const* args)
        {
            auto baseDeclRefExpr = baseExpr.As<DeclRefExpr>();
            if (!baseDeclRefExpr)
            {
                SLANG_DIAGNOSE_UNEXPECTED(getSink(), baseExpr, "expected a reference to a generic declaration");
                return CreateErrorExpr(originalExpr);
            }
            auto baseGenericRef = baseDeclRefExpr->declRef.As<GenericDecl>();
            if (!baseGenericRef)
            {
                SLANG_DIAGNOSE_UNEXPECTED(getSink(), baseExpr, "expected a reference to a generic declaration");
                return CreateErrorExpr(originalExpr);
            }

            RefPtr<Substitutions> subst = new Substitutions();
            subst->genericDecl = baseGenericRef.getDecl();
            subst->outer = baseGenericRef.substitutions;

            for(UInt aa = 0; aa < argCount; ++aa)
            {
                auto arg = args[aa];
                subst->args.Add(ExtractGenericArgVal(arg));
            }

            DeclRef<Decl> innerDeclRef(GetInner(baseGenericRef), subst);

            return ConstructDeclRefExpr(
                innerDeclRef,
                nullptr,
                originalExpr->loc);
        }

        // Take an overload candidate that previously got through
        // `TryCheckOverloadCandidate` above, and try to finish
        // up the work and turn it into a real expression.
        //
        // If the candidate isn't actually applicable, this is
        // where we'd start reporting the issue(s).
        RefPtr<Expr> CompleteOverloadCandidate(
            OverloadResolveContext&		context,
            OverloadCandidate&			candidate)
        {
            // special case for generic argument inference failure
            if (candidate.status == OverloadCandidate::Status::GenericArgumentInferenceFailed)
            {
                String callString = getCallSignatureString(context);
                if (!isRewriteMode())
                {
                    getSink()->diagnose(
                        context.loc,
                        Diagnostics::genericArgumentInferenceFailed,
                        callString);

                    String declString = getDeclSignatureString(candidate.item);
                    getSink()->diagnose(candidate.item.declRef, Diagnostics::genericSignatureTried, declString);
                }
                goto error;
            }

            context.mode = OverloadResolveContext::Mode::ForReal;

            if (!TryCheckOverloadCandidateArity(context, candidate))
                goto error;

            if (!TryCheckOverloadCandidateFixity(context, candidate))
                goto error;

            if (!TryCheckOverloadCandidateTypes(context, candidate))
                goto error;

            if (!TryCheckOverloadCandidateDirections(context, candidate))
                goto error;

            {
                auto baseExpr = ConstructLookupResultExpr(
                    candidate.item, context.baseExpr, context.funcLoc);

                switch(candidate.flavor)
                {
                case OverloadCandidate::Flavor::Func:
                    {
                        RefPtr<AppExprBase> callExpr = context.originalExpr.As<InvokeExpr>();
                        if(!callExpr)
                        {
                            callExpr = new InvokeExpr();
                            callExpr->loc = context.loc;

                            for(UInt aa = 0; aa < context.argCount; ++aa)
                                callExpr->Arguments.Add(context.getArg(aa));
                        }


                        callExpr->FunctionExpr = baseExpr;
                        callExpr->type = QualType(candidate.resultType);

                        // A call may yield an l-value, and we should take a look at the candidate to be sure
                        if(auto subscriptDeclRef = candidate.item.declRef.As<SubscriptDecl>())
                        {
                            for(auto setter : subscriptDeclRef.getDecl()->getMembersOfType<SetterDecl>())
                            {
                                callExpr->type.IsLeftValue = true;
                            }
                        }

                        // TODO: there may be other cases that confer l-value-ness

                        return callExpr;
                    }

                    break;

                case OverloadCandidate::Flavor::Generic:
                    return CreateGenericDeclRef(baseExpr, context.originalExpr,
                        context.argCount,
                        context.args);
                    break;

                default:
                    SLANG_DIAGNOSE_UNEXPECTED(getSink(), context.loc, "unknown overload candidate flavor");
                    break;
                }
            }


        error:

            if(context.originalExpr)
            {
                return CreateErrorExpr(context.originalExpr.Ptr());
            }
            else
            {
                SLANG_DIAGNOSE_UNEXPECTED(getSink(), context.loc, "no original expression for overload result");
                return nullptr;
            }
        }

        // Implement a comparison operation between overload candidates,
        // so that the better candidate compares as less-than the other
        int CompareOverloadCandidates(
            OverloadCandidate*	left,
            OverloadCandidate*	right)
        {
            // If one candidate got further along in validation, pick it
            if (left->status != right->status)
                return int(right->status) - int(left->status);

            // If both candidates are applicable, then we need to compare
            // the costs of their type conversion sequences
            if(left->status == OverloadCandidate::Status::Appicable)
            {
                if (left->conversionCostSum != right->conversionCostSum)
                    return left->conversionCostSum - right->conversionCostSum;
            }

            return 0;
        }

        void AddOverloadCandidateInner(
            OverloadResolveContext& context,
            OverloadCandidate&		candidate)
        {
            // Filter our existing candidates, to remove any that are worse than our new one

            bool keepThisCandidate = true; // should this candidate be kept?

            if (context.bestCandidates.Count() != 0)
            {
                // We have multiple candidates right now, so filter them.
                bool anyFiltered = false;
                // Note that we are querying the list length on every iteration,
                // because we might remove things.
                for (UInt cc = 0; cc < context.bestCandidates.Count(); ++cc)
                {
                    int cmp = CompareOverloadCandidates(&candidate, &context.bestCandidates[cc]);
                    if (cmp < 0)
                    {
                        // our new candidate is better!

                        // remove it from the list (by swapping in a later one)
                        context.bestCandidates.FastRemoveAt(cc);
                        // and then reduce our index so that we re-visit the same index
                        --cc;

                        anyFiltered = true;
                    }
                    else if(cmp > 0)
                    {
                        // our candidate is worse!
                        keepThisCandidate = false;
                    }
                }
                // It should not be possible that we removed some existing candidate *and*
                // chose not to keep this candidate (otherwise the better-ness relation
                // isn't transitive). Therefore we confirm that we either chose to keep
                // this candidate (in which case filtering is okay), or we didn't filter
                // anything.
                SLANG_ASSERT(keepThisCandidate || !anyFiltered);
            }
            else if(context.bestCandidate)
            {
                // There's only one candidate so far
                int cmp = CompareOverloadCandidates(&candidate, context.bestCandidate);
                if(cmp < 0)
                {
                    // our new candidate is better!
                    context.bestCandidate = nullptr;
                }
                else if (cmp > 0)
                {
                    // our candidate is worse!
                    keepThisCandidate = false;
                }
            }

            // If our candidate isn't good enough, then drop it
            if (!keepThisCandidate)
                return;

            // Otherwise we want to keep the candidate
            if (context.bestCandidates.Count() > 0)
            {
                // There were already multiple candidates, and we are adding one more
                context.bestCandidates.Add(candidate);
            }
            else if (context.bestCandidate)
            {
                // There was a unique best candidate, but now we are ambiguous
                context.bestCandidates.Add(*context.bestCandidate);
                context.bestCandidates.Add(candidate);
                context.bestCandidate = nullptr;
            }
            else
            {
                // This is the only candidate worthe keeping track of right now
                context.bestCandidateStorage = candidate;
                context.bestCandidate = &context.bestCandidateStorage;
            }
        }

        void AddOverloadCandidate(
            OverloadResolveContext& context,
            OverloadCandidate&		candidate)
        {
            // Try the candidate out, to see if it is applicable at all.
            TryCheckOverloadCandidate(context, candidate);

            // Now (potentially) add it to the set of candidate overloads to consider.
            AddOverloadCandidateInner(context, candidate);
        }

        void AddFuncOverloadCandidate(
            LookupResultItem			item,
            DeclRef<CallableDecl>             funcDeclRef,
            OverloadResolveContext&		context)
        {
            EnsureDecl(funcDeclRef.getDecl());

            OverloadCandidate candidate;
            candidate.flavor = OverloadCandidate::Flavor::Func;
            candidate.item = item;
            candidate.resultType = GetResultType(funcDeclRef);

            AddOverloadCandidate(context, candidate);
        }

        void AddFuncOverloadCandidate(
            RefPtr<FuncType>		/*funcType*/,
            OverloadResolveContext&	/*context*/)
        {
#if 0
            if (funcType->decl)
            {
                AddFuncOverloadCandidate(funcType->decl, context);
            }
            else if (funcType->Func)
            {
                AddFuncOverloadCandidate(funcType->Func->SyntaxNode, context);
            }
            else if (funcType->Component)
            {
                AddComponentFuncOverloadCandidate(funcType->Component, context);
            }
#else
            throw "unimplemented";
#endif
        }

        void AddCtorOverloadCandidate(
            LookupResultItem		typeItem,
            RefPtr<Type>	type,
            DeclRef<ConstructorDecl>		ctorDeclRef,
            OverloadResolveContext&	context)
        {
            EnsureDecl(ctorDeclRef.getDecl());

            // `typeItem` refers to the type being constructed (the thing
            // that was applied as a function) so we need to construct
            // a `LookupResultItem` that refers to the constructor instead

            LookupResultItem ctorItem;
            ctorItem.declRef = ctorDeclRef;
            ctorItem.breadcrumbs = new LookupResultItem::Breadcrumb(LookupResultItem::Breadcrumb::Kind::Member, typeItem.declRef, typeItem.breadcrumbs);

            OverloadCandidate candidate;
            candidate.flavor = OverloadCandidate::Flavor::Func;
            candidate.item = ctorItem;
            candidate.resultType = type;

            AddOverloadCandidate(context, candidate);
        }

        // If the given declaration has generic parameters, then
        // return the corresponding `GenericDecl` that holds the
        // parameters, etc.
        GenericDecl* GetOuterGeneric(Decl* decl)
        {
            auto parentDecl = decl->ParentDecl;
            if (!parentDecl) return nullptr;
            auto parentGeneric = dynamic_cast<GenericDecl*>(parentDecl);
            return parentGeneric;
        }

        // Try to find a unification for two values
        bool TryUnifyVals(
            ConstraintSystem&	constraints,
            RefPtr<Val>			fst,
            RefPtr<Val>			snd)
        {
            // if both values are types, then unify types
            if (auto fstType = fst.As<Type>())
            {
                if (auto sndType = snd.As<Type>())
                {
                    return TryUnifyTypes(constraints, fstType, sndType);
                }
            }

            // if both values are constant integers, then compare them
            if (auto fstIntVal = fst.As<ConstantIntVal>())
            {
                if (auto sndIntVal = snd.As<ConstantIntVal>())
                {
                    return fstIntVal->value == sndIntVal->value;
                }
            }

            // Check if both are integer values in general
            if (auto fstInt = fst.As<IntVal>())
            {
                if (auto sndInt = snd.As<IntVal>())
                {
                    auto fstParam = fstInt.As<GenericParamIntVal>();
                    auto sndParam = sndInt.As<GenericParamIntVal>();

                    if (fstParam)
                        TryUnifyIntParam(constraints, fstParam->declRef, sndInt);
                    if (sndParam)
                        TryUnifyIntParam(constraints, sndParam->declRef, fstInt);

                    if (fstParam || sndParam)
                        return true;
                }
            }

            throw "unimplemented";

            // default: fail
            return false;
        }

        bool TryUnifySubstitutions(
            ConstraintSystem&		constraints,
            RefPtr<Substitutions>	fst,
            RefPtr<Substitutions>	snd)
        {
            // They must both be NULL or non-NULL
            if (!fst || !snd)
                return fst == snd;

            // They must be specializing the same generic
            if (fst->genericDecl != snd->genericDecl)
                return false;

            // Their arguments must unify
            SLANG_RELEASE_ASSERT(fst->args.Count() == snd->args.Count());
            UInt argCount = fst->args.Count();
            for (UInt aa = 0; aa < argCount; ++aa)
            {
                if (!TryUnifyVals(constraints, fst->args[aa], snd->args[aa]))
                    return false;
            }

            // Their "base" specializations must unify
            if (!TryUnifySubstitutions(constraints, fst->outer, snd->outer))
                return false;

            return true;
        }

        bool TryUnifyTypeParam(
            ConstraintSystem&				constraints,
            RefPtr<GenericTypeParamDecl>	typeParamDecl,
            RefPtr<Type>			type)
        {
            // We want to constrain the given type parameter
            // to equal the given type.
            Constraint constraint;
            constraint.decl = typeParamDecl.Ptr();
            constraint.val = type;

            constraints.constraints.Add(constraint);

            return true;
        }

        bool TryUnifyIntParam(
            ConstraintSystem&               constraints,
            RefPtr<GenericValueParamDecl>	paramDecl,
            RefPtr<IntVal>                  val)
        {
            // We want to constrain the given parameter to equal the given value.
            Constraint constraint;
            constraint.decl = paramDecl.Ptr();
            constraint.val = val;

            constraints.constraints.Add(constraint);

            return true;
        }

        bool TryUnifyIntParam(
            ConstraintSystem&       constraints,
            DeclRef<VarDeclBase> const&   varRef,
            RefPtr<IntVal>          val)
        {
            if(auto genericValueParamRef = varRef.As<GenericValueParamDecl>())
            {
                return TryUnifyIntParam(constraints, genericValueParamRef.getDecl(), val);
            }
            else
            {
                return false;
            }
        }

        bool TryUnifyTypesByStructuralMatch(
            ConstraintSystem&       constraints,
            RefPtr<Type>  fst,
            RefPtr<Type>  snd)
        {
            if (auto fstDeclRefType = fst->As<DeclRefType>())
            {
                auto fstDeclRef = fstDeclRefType->declRef;

                if (auto typeParamDecl = dynamic_cast<GenericTypeParamDecl*>(fstDeclRef.getDecl()))
                    return TryUnifyTypeParam(constraints, typeParamDecl, snd);

                if (auto sndDeclRefType = snd->As<DeclRefType>())
                {
                    auto sndDeclRef = sndDeclRefType->declRef;

                    if (auto typeParamDecl = dynamic_cast<GenericTypeParamDecl*>(sndDeclRef.getDecl()))
                        return TryUnifyTypeParam(constraints, typeParamDecl, fst);

                    // can't be unified if they refer to differnt declarations.
                    if (fstDeclRef.getDecl() != sndDeclRef.getDecl()) return false;

                    // next we need to unify the substitutions applied
                    // to each decalration reference.
                    if (!TryUnifySubstitutions(
                        constraints,
                        fstDeclRef.substitutions,
                        sndDeclRef.substitutions))
                    {
                        return false;
                    }

                    return true;
                }
            }

            return false;
        }

        bool TryUnifyTypes(
            ConstraintSystem&       constraints,
            RefPtr<Type>  fst,
            RefPtr<Type>  snd)
        {
            if (fst->Equals(snd)) return true;

            // An error type can unify with anything, just so we avoid cascading errors.

            if (auto fstErrorType = fst->As<ErrorType>())
                return true;

            if (auto sndErrorType = snd->As<ErrorType>())
                return true;

            // A generic parameter type can unify with anything.
            // TODO: there actually needs to be some kind of "occurs check" sort
            // of thing here...

            if (auto fstDeclRefType = fst->As<DeclRefType>())
            {
                auto fstDeclRef = fstDeclRefType->declRef;

                if (auto typeParamDecl = dynamic_cast<GenericTypeParamDecl*>(fstDeclRef.getDecl()))
                    return TryUnifyTypeParam(constraints, typeParamDecl, snd);
            }

            if (auto sndDeclRefType = snd->As<DeclRefType>())
            {
                auto sndDeclRef = sndDeclRefType->declRef;

                if (auto typeParamDecl = dynamic_cast<GenericTypeParamDecl*>(sndDeclRef.getDecl()))
                    return TryUnifyTypeParam(constraints, typeParamDecl, fst);
            }

            // If we can unify the types structurally, then we are golden
            if(TryUnifyTypesByStructuralMatch(constraints, fst, snd))
                return true;

            // Now we need to consider cases where coercion might
            // need to be applied. For now we can try to do this
            // in a completely ad hoc fashion, but eventually we'd
            // want to do it more formally.

            if(auto fstVectorType = fst->As<VectorExpressionType>())
            {
                if(auto sndScalarType = snd->As<BasicExpressionType>())
                {
                    return TryUnifyTypes(
                        constraints,
                        fstVectorType->elementType,
                        sndScalarType);
                }
            }

            if(auto fstScalarType = fst->As<BasicExpressionType>())
            {
                if(auto sndVectorType = snd->As<VectorExpressionType>())
                {
                    return TryUnifyTypes(
                        constraints,
                        fstScalarType,
                        sndVectorType->elementType);
                }
            }

            // TODO: the same thing for vectors...

            return false;
        }

        // Is the candidate extension declaration actually applicable to the given type
        DeclRef<ExtensionDecl> ApplyExtensionToType(
            ExtensionDecl*			extDecl,
            RefPtr<Type>	type)
        {
            if (auto extGenericDecl = GetOuterGeneric(extDecl))
            {
                ConstraintSystem constraints;

                if (!TryUnifyTypes(constraints, extDecl->targetType.Ptr(), type))
                    return DeclRef<Decl>().As<ExtensionDecl>();

                auto constraintSubst = TrySolveConstraintSystem(&constraints, DeclRef<Decl>(extGenericDecl, nullptr).As<GenericDecl>());
                if (!constraintSubst)
                {
                    return DeclRef<Decl>().As<ExtensionDecl>();
                }

                // Consruct a reference to the extension with our constraint variables
                // set as they were found by solving the constraint system.
                DeclRef<ExtensionDecl> extDeclRef = DeclRef<Decl>(extDecl, constraintSubst).As<ExtensionDecl>();

                // We expect/require that the result of unification is such that
                // the target types are now equal
                SLANG_ASSERT(GetTargetType(extDeclRef)->Equals(type));

                return extDeclRef;
            }
            else
            {
                // The easy case is when the extension isn't generic:
                // either it applies to the type or not.
                if (!type->Equals(extDecl->targetType))
                    return DeclRef<Decl>().As<ExtensionDecl>();
                return DeclRef<Decl>(extDecl, nullptr).As<ExtensionDecl>();
            }
        }

#if 0
        bool TryUnifyArgAndParamTypes(
            ConstraintSystem&				system,
            RefPtr<Expr>	argExpr,
            DeclRef<ParamDecl>					paramDeclRef)
        {
            // TODO(tfoley): potentially need a bit more
            // nuance in case where argument might be
            // an overload group...
            return TryUnifyTypes(system, argExpr->type, GetType(paramDeclRef));
        }
#endif

        // Take a generic declaration and try to specialize its parameters
        // so that the resulting inner declaration can be applicable in
        // a particular context...
        DeclRef<Decl> SpecializeGenericForOverload(
            DeclRef<GenericDecl>			genericDeclRef,
            OverloadResolveContext&	context)
        {
            ConstraintSystem constraints;

            // Construct a reference to the inner declaration that has any generic
            // parameter substitutions in place already, but *not* any substutions
            // for the generic declaration we are currently trying to infer.
            auto innerDecl = GetInner(genericDeclRef);
            DeclRef<Decl> unspecializedInnerRef = DeclRef<Decl>(innerDecl, genericDeclRef.substitutions);

            // Check what type of declaration we are dealing with, and then try
            // to match it up with the arguments accordingly...
            if (auto funcDeclRef = unspecializedInnerRef.As<CallableDecl>())
            {
                auto params = GetParameters(funcDeclRef).ToArray();

                UInt argCount = context.getArgCount();
                UInt paramCount = params.Count();

                // Bail out on mismatch.
                // TODO(tfoley): need more nuance here
                if (argCount != paramCount)
                {
                    return DeclRef<Decl>(nullptr, nullptr);
                }

                for (UInt aa = 0; aa < argCount; ++aa)
                {
#if 0
                    if (!TryUnifyArgAndParamTypes(constraints, args[aa], params[aa]))
                        return DeclRef<Decl>(nullptr, nullptr);
#else
                    // The question here is whether failure to "unify" an argument
                    // and parameter should lead to immediate failure.
                    //
                    // The case that is interesting is if we want to unify, say:
                    // `vector<float,N>` and `vector<int,3>`
                    //
                    // It is clear that we should solve with `N = 3`, and then
                    // a later step may find that the resulting types aren't
                    // actually a match.
                    //
                    // A more refined approach to "unification" could of course
                    // see that `int` can convert to `float` and use that fact.
                    // (and indeed we already use something like this to unify
                    // `float` and `vector<T,3>`)
                    //
                    // So the question is then whether a mismatch during the
                    // unification step should be taken as an immediate failure...

                    TryUnifyTypes(constraints, context.getArgType(aa), GetType(params[aa]));
#endif
                }
            }
            else
            {
                // TODO(tfoley): any other cases needed here?
                return DeclRef<Decl>(nullptr, nullptr);
            }

            auto constraintSubst = TrySolveConstraintSystem(&constraints, genericDeclRef);
            if (!constraintSubst)
            {
                // constraint solving failed
                return DeclRef<Decl>(nullptr, nullptr);
            }

            // We can now construct a reference to the inner declaration using
            // the solution to our constraints.
            return DeclRef<Decl>(innerDecl, constraintSubst);
        }

        void AddAggTypeOverloadCandidates(
            LookupResultItem		typeItem,
            RefPtr<Type>	type,
            DeclRef<AggTypeDecl>			aggTypeDeclRef,
            OverloadResolveContext&	context)
        {
            for (auto ctorDeclRef : getMembersOfType<ConstructorDecl>(aggTypeDeclRef))
            {
                // now work through this candidate...
                AddCtorOverloadCandidate(typeItem, type, ctorDeclRef, context);
            }

            // Now walk through any extensions we can find for this types
            for (auto ext = GetCandidateExtensions(aggTypeDeclRef); ext; ext = ext->nextCandidateExtension)
            {
                auto extDeclRef = ApplyExtensionToType(ext, type);
                if (!extDeclRef)
                    continue;

                for (auto ctorDeclRef : getMembersOfType<ConstructorDecl>(extDeclRef))
                {
                    // TODO(tfoley): `typeItem` here should really reference the extension...

                    // now work through this candidate...
                    AddCtorOverloadCandidate(typeItem, type, ctorDeclRef, context);
                }

                // Also check for generic constructors
                for (auto genericDeclRef : getMembersOfType<GenericDecl>(extDeclRef))
                {
                    if (auto ctorDecl = genericDeclRef.getDecl()->inner.As<ConstructorDecl>())
                    {
                        DeclRef<Decl> innerRef = SpecializeGenericForOverload(genericDeclRef, context);
                        if (!innerRef)
                            continue;

                        DeclRef<ConstructorDecl> innerCtorRef = innerRef.As<ConstructorDecl>();

                        AddCtorOverloadCandidate(typeItem, type, innerCtorRef, context);

                        // TODO(tfoley): need a way to do the solving step for the constraint system
                    }
                }
            }
        }

        void AddTypeOverloadCandidates(
            RefPtr<Type>	type,
            OverloadResolveContext&	context)
        {
            if (auto declRefType = type->As<DeclRefType>())
            {
                if (auto aggTypeDeclRef = declRefType->declRef.As<AggTypeDecl>())
                {
                    AddAggTypeOverloadCandidates(LookupResultItem(aggTypeDeclRef), type, aggTypeDeclRef, context);
                }
            }
        }

        void AddDeclRefOverloadCandidates(
            LookupResultItem		item,
            OverloadResolveContext&	context)
        {
            auto declRef = item.declRef;

            if (auto funcDeclRef = item.declRef.As<CallableDecl>())
            {
                AddFuncOverloadCandidate(item, funcDeclRef, context);
            }
            else if (auto aggTypeDeclRef = item.declRef.As<AggTypeDecl>())
            {
                auto type = DeclRefType::Create(
                    getSession(),
                    aggTypeDeclRef);
                AddAggTypeOverloadCandidates(item, type, aggTypeDeclRef, context);
            }
            else if (auto genericDeclRef = item.declRef.As<GenericDecl>())
            {
                // Try to infer generic arguments, based on the context
                DeclRef<Decl> innerRef = SpecializeGenericForOverload(genericDeclRef, context);

                if (innerRef)
                {
                    // If inference works, then we've now got a
                    // specialized declaration reference we can apply.

                    LookupResultItem innerItem;
                    innerItem.breadcrumbs = item.breadcrumbs;
                    innerItem.declRef = innerRef;

                    AddDeclRefOverloadCandidates(innerItem, context);
                }
                else
                {
                    // If inference failed, then we need to create
                    // a candidate that can be used to reflect that fact
                    // (so we can report a good error)
                    OverloadCandidate candidate;
                    candidate.item = item;
                    candidate.flavor = OverloadCandidate::Flavor::UnspecializedGeneric;
                    candidate.status = OverloadCandidate::Status::GenericArgumentInferenceFailed;

                    AddOverloadCandidateInner(context, candidate);
                }
            }
            else if( auto typeDefDeclRef = item.declRef.As<TypeDefDecl>() )
            {
                AddTypeOverloadCandidates(GetType(typeDefDeclRef), context);
            }
            else
            {
                // TODO(tfoley): any other cases needed here?
            }
        }

        void AddOverloadCandidates(
            RefPtr<Expr>	funcExpr,
            OverloadResolveContext&			context)
        {
            auto funcExprType = funcExpr->type;

            if (auto funcDeclRefExpr = funcExpr.As<DeclRefExpr>())
            {
                // The expression referenced a function declaration
                AddDeclRefOverloadCandidates(LookupResultItem(funcDeclRefExpr->declRef), context);
            }
            else if (auto funcType = funcExprType->As<FuncType>())
            {
                // TODO(tfoley): deprecate this path...
                AddFuncOverloadCandidate(funcType, context);
            }
            else if (auto overloadedExpr = funcExpr.As<OverloadedExpr>())
            {
                auto lookupResult = overloadedExpr->lookupResult2;
                SLANG_RELEASE_ASSERT(lookupResult.isOverloaded());
                for(auto item : lookupResult.items)
                {
                    AddDeclRefOverloadCandidates(item, context);
                }
            }
            else if (auto typeType = funcExprType->As<TypeType>())
            {
                // If none of the above cases matched, but we are
                // looking at a type, then I suppose we have
                // a constructor call on our hands.
                //
                // TODO(tfoley): are there any meaningful types left
                // that aren't declaration references?
                AddTypeOverloadCandidates(typeType->type, context);
                return;
            }
        }

        void formatType(StringBuilder& sb, RefPtr<Type> type)
        {
            sb << type->ToString();
        }

        void formatVal(StringBuilder& sb, RefPtr<Val> val)
        {
            sb << val->ToString();
        }

        void formatDeclPath(StringBuilder& sb, DeclRef<Decl> declRef)
        {
            // Find the parent declaration
            auto parentDeclRef = declRef.GetParent();

            // If the immediate parent is a generic, then we probably
            // want the declaration above that...
            auto parentGenericDeclRef = parentDeclRef.As<GenericDecl>();
            if(parentGenericDeclRef)
            {
                parentDeclRef = parentGenericDeclRef.GetParent();
            }

            // Depending on what the parent is, we may want to format things specially
            if(auto aggTypeDeclRef = parentDeclRef.As<AggTypeDecl>())
            {
                formatDeclPath(sb, aggTypeDeclRef);
                sb << ".";
            }

            sb << getText(declRef.GetName());

            // If the parent declaration is a generic, then we need to print out its
            // signature
            if( parentGenericDeclRef )
            {
                SLANG_RELEASE_ASSERT(declRef.substitutions);
                SLANG_RELEASE_ASSERT(declRef.substitutions->genericDecl == parentGenericDeclRef.getDecl());

                sb << "<";
                bool first = true;
                for(auto arg : declRef.substitutions->args)
                {
                    if(!first) sb << ", ";
                    formatVal(sb, arg);
                    first = false;
                }
                sb << ">";
            }
        }

        void formatDeclParams(StringBuilder& sb, DeclRef<Decl> declRef)
        {
            if (auto funcDeclRef = declRef.As<CallableDecl>())
            {

                // This is something callable, so we need to also print parameter types for overloading
                sb << "(";

                bool first = true;
                for (auto paramDeclRef : GetParameters(funcDeclRef))
                {
                    if (!first) sb << ", ";

                    formatType(sb, GetType(paramDeclRef));

                    first = false;

                }

                sb << ")";
            }
            else if(auto genericDeclRef = declRef.As<GenericDecl>())
            {
                sb << "<";
                bool first = true;
                for (auto paramDeclRef : getMembers(genericDeclRef))
                {
                    if(auto genericTypeParam = paramDeclRef.As<GenericTypeParamDecl>())
                    {
                        if (!first) sb << ", ";
                        first = false;

                        sb << getText(genericTypeParam.GetName());
                    }
                    else if(auto genericValParam = paramDeclRef.As<GenericValueParamDecl>())
                    {
                        if (!first) sb << ", ";
                        first = false;

                        formatType(sb, GetType(genericValParam));
                        sb << " ";
                        sb << getText(genericValParam.GetName());
                    }
                    else
                    {}
                }
                sb << ">";

                formatDeclParams(sb, DeclRef<Decl>(GetInner(genericDeclRef), genericDeclRef.substitutions));
            }
            else
            {
            }
        }

        void formatDeclSignature(StringBuilder& sb, DeclRef<Decl> declRef)
        {
            formatDeclPath(sb, declRef);
            formatDeclParams(sb, declRef);
        }

        String getDeclSignatureString(DeclRef<Decl> declRef)
        {
            StringBuilder sb;
            formatDeclSignature(sb, declRef);
            return sb.ProduceString();
        }

        String getDeclSignatureString(LookupResultItem item)
        {
            return getDeclSignatureString(item.declRef);
        }

        String getCallSignatureString(
            OverloadResolveContext&     context)
        {
            StringBuilder argsListBuilder;
            argsListBuilder << "(";

            UInt argCount = context.getArgCount();
            for( UInt aa = 0; aa < argCount; ++aa )
            {
                if(aa != 0) argsListBuilder << ", ";
                argsListBuilder << context.getArgType(aa)->ToString();
            }
            argsListBuilder << ")";
            return argsListBuilder.ProduceString();
        }

#if 0
        String GetCallSignatureString(RefPtr<AppExprBase> expr)
        {
            return getCallSignatureString(expr->Arguments);
        }
#endif

        RefPtr<Expr> ResolveInvoke(InvokeExpr * expr)
        {
            // Look at the base expression for the call, and figure out how to invoke it.
            auto funcExpr = expr->FunctionExpr;
            auto funcExprType = funcExpr->type;

            // If we are trying to apply an erroroneous expression, then just bail out now.
            if(IsErrorExpr(funcExpr))
            {
                return CreateErrorExpr(expr);
            }
            // If any of the arguments is an error, then we should bail out, to avoid
            // cascading errors where we successfully pick an overload, but not the one
            // the user meant.
            for (auto arg : expr->Arguments)
            {
                if (IsErrorExpr(arg))
                    return CreateErrorExpr(expr);
            }

            OverloadResolveContext context;

            context.originalExpr = expr;
            context.funcLoc = funcExpr->loc;

            context.argCount = expr->Arguments.Count();
            context.args = expr->Arguments.Buffer();
            context.loc = expr->loc;

            if (auto funcMemberExpr = funcExpr.As<MemberExpr>())
            {
                context.baseExpr = funcMemberExpr->BaseExpression;
            }
            else if (auto funcOverloadExpr = funcExpr.As<OverloadedExpr>())
            {
                context.baseExpr = funcOverloadExpr->base;
            }
            AddOverloadCandidates(funcExpr, context);

            if (context.bestCandidates.Count() > 0)
            {
                // Things were ambiguous.

                // It might be that things were only ambiguous because
                // one of the argument expressions had an error, and
                // so a bunch of candidates could match at that position.
                //
                // If any argument was an error, we skip out on printing
                // another message, to avoid cascading errors.
                for (auto arg : expr->Arguments)
                {
                    if (IsErrorExpr(arg))
                    {
                        return CreateErrorExpr(expr);
                    }
                }

                Name* funcName = nullptr;
                if (auto baseVar = funcExpr.As<VarExpr>())
                    funcName = baseVar->name;
                else if(auto baseMemberRef = funcExpr.As<MemberExpr>())
                    funcName = baseMemberRef->name;

                String argsList = getCallSignatureString(context);

                if (context.bestCandidates[0].status != OverloadCandidate::Status::Appicable)
                {
                    // There were multple equally-good candidates, but none actually usable.
                    // We will construct a diagnostic message to help out.
                    if (funcName)
                    {
                        if (!isRewriteMode())
                        {
                            getSink()->diagnose(expr, Diagnostics::noApplicableOverloadForNameWithArgs, funcName, argsList);
                        }
                    }
                    else
                    {
                        if (!isRewriteMode())
                        {
                            getSink()->diagnose(expr, Diagnostics::noApplicableWithArgs, argsList);
                        }
                    }
                }
                else
                {
                    // There were multiple applicable candidates, so we need to report them.

                    if (funcName)
                    {
                        if (!isRewriteMode())
                        {
                            getSink()->diagnose(expr, Diagnostics::ambiguousOverloadForNameWithArgs, funcName, argsList);
                        }
                    }
                    else
                    {
                        if (!isRewriteMode())
                        {
                            getSink()->diagnose(expr, Diagnostics::ambiguousOverloadWithArgs, argsList);
                        }
                    }
                }

                if (!isRewriteMode())
                {
                    UInt candidateCount = context.bestCandidates.Count();
                    UInt maxCandidatesToPrint = 10; // don't show too many candidates at once...
                    UInt candidateIndex = 0;
                    for (auto candidate : context.bestCandidates)
                    {
                        String declString = getDeclSignatureString(candidate.item);

                        declString = declString + "[" + String(candidate.conversionCostSum) + "]";

                        getSink()->diagnose(candidate.item.declRef, Diagnostics::overloadCandidate, declString);

                        candidateIndex++;
                        if (candidateIndex == maxCandidatesToPrint)
                            break;
                    }
                    if (candidateIndex != candidateCount)
                    {
                        getSink()->diagnose(expr, Diagnostics::moreOverloadCandidates, candidateCount - candidateIndex);
                    }
                }

                return CreateErrorExpr(expr);
            }
            else if (context.bestCandidate)
            {
                // There was one best candidate, even if it might not have been
                // applicable in the end.
                // We will report errors for this one candidate, then, to give
                // the user the most help we can.
                return CompleteOverloadCandidate(context, *context.bestCandidate);
            }
            else
            {
                // Nothing at all was found that we could even consider invoking
                if (!isRewriteMode())
                {
                    getSink()->diagnose(expr->FunctionExpr, Diagnostics::expectedFunction);
                }
                expr->type = QualType(getSession()->getErrorType());
                return expr;
            }
        }

        void AddGenericOverloadCandidate(
            LookupResultItem		baseItem,
            OverloadResolveContext&	context)
        {
            if (auto genericDeclRef = baseItem.declRef.As<GenericDecl>())
            {
                EnsureDecl(genericDeclRef.getDecl());

                OverloadCandidate candidate;
                candidate.flavor = OverloadCandidate::Flavor::Generic;
                candidate.item = baseItem;
                candidate.resultType = nullptr;

                AddOverloadCandidate(context, candidate);
            }
        }

        void AddGenericOverloadCandidates(
            RefPtr<Expr>	baseExpr,
            OverloadResolveContext&			context)
        {
            if(auto baseDeclRefExpr = baseExpr.As<DeclRefExpr>())
            {
                auto declRef = baseDeclRefExpr->declRef;
                AddGenericOverloadCandidate(LookupResultItem(declRef), context);
            }
            else if (auto overloadedExpr = baseExpr.As<OverloadedExpr>())
            {
                // We are referring to a bunch of declarations, each of which might be generic
                LookupResult result;
                for (auto item : overloadedExpr->lookupResult2.items)
                {
                    AddGenericOverloadCandidate(item, context);
                }
            }
            else
            {
                // any other cases?
            }
        }

        RefPtr<Expr> visitGenericAppExpr(GenericAppExpr * genericAppExpr)
        {
            // We are applying a generic to arguments, but there might be multiple generic
            // declarations with the same name, so this becomes a specialized case of
            // overload resolution.


            // Start by checking the base expression and arguments.
            auto& baseExpr = genericAppExpr->FunctionExpr;
            baseExpr = CheckTerm(baseExpr);
            auto& args = genericAppExpr->Arguments;
            for (auto& arg : args)
            {
                arg = CheckTerm(arg);
            }

            // If there was an error in the base expression,  or in any of
            // the arguments, then just bail.
            if (IsErrorExpr(baseExpr))
            {
                return CreateErrorExpr(genericAppExpr);
            }
            for (auto argExpr : args)
            {
                if (IsErrorExpr(argExpr))
                {
                    return CreateErrorExpr(genericAppExpr);
                }
            }

            // Otherwise, let's start looking at how to find an overload...

            OverloadResolveContext context;
            context.originalExpr = genericAppExpr;
            context.funcLoc = baseExpr->loc;
            context.argCount = args.Count();
            context.args = args.Buffer();
            context.loc = genericAppExpr->loc;

            context.baseExpr = GetBaseExpr(baseExpr);

            AddGenericOverloadCandidates(baseExpr, context);

            if (context.bestCandidates.Count() > 0)
            {
                // Things were ambiguous.
                if (context.bestCandidates[0].status != OverloadCandidate::Status::Appicable)
                {
                    // There were multple equally-good candidates, but none actually usable.
                    // We will construct a diagnostic message to help out.

                    // TODO(tfoley): print a reasonable message here...

                    if (!isRewriteMode())
                    {
                        getSink()->diagnose(genericAppExpr, Diagnostics::unimplemented, "no applicable generic");
                    }

                    return CreateErrorExpr(genericAppExpr);
                }
                else
                {
                    // There were multiple viable candidates, but that isn't an error: we just need
                    // to complete all of them and create an overloaded expression as a result.

                    LookupResult result;
                    for (auto candidate : context.bestCandidates)
                    {
                        auto candidateExpr = CompleteOverloadCandidate(context, candidate);
                    }

                    throw "what now?";
//                        auto overloadedExpr = new OverloadedExpr();
//                        return overloadedExpr;
                }
            }
            else if (context.bestCandidate)
            {
                // There was one best candidate, even if it might not have been
                // applicable in the end.
                // We will report errors for this one candidate, then, to give
                // the user the most help we can.
                return CompleteOverloadCandidate(context, *context.bestCandidate);
            }
            else
            {
                // Nothing at all was found that we could even consider invoking
                if (!isRewriteMode())
                {
                    getSink()->diagnose(genericAppExpr, Diagnostics::unimplemented, "expected a generic");
                }
                return CreateErrorExpr(genericAppExpr);
            }
        }

        RefPtr<Expr> visitSharedTypeExpr(SharedTypeExpr* expr)
        {
            if (!expr->type.Ptr())
            {
                expr->base = CheckProperType(expr->base);
                expr->type = expr->base.exp->type;
            }
            return expr;
        }




        RefPtr<Expr> CheckExpr(RefPtr<Expr> expr)
        {
            auto term = CheckTerm(expr);

            // TODO(tfoley): Need a step here to ensure that the term actually
            // resolves to a (single) expression with a real type.

            return term;
        }

        RefPtr<Expr> CheckInvokeExprWithCheckedOperands(InvokeExpr *expr)
        {

            auto rs = ResolveInvoke(expr);
            if (auto invoke = dynamic_cast<InvokeExpr*>(rs.Ptr()))
            {
                // if this is still an invoke expression, test arguments passed to inout/out parameter are LValues
                if(auto funcType = invoke->FunctionExpr->type->As<FuncType>())
                {
                    UInt paramCount = funcType->getParamCount();
                    for (UInt pp = 0; pp < paramCount; ++pp)
                    {
                        auto paramType = funcType->getParamType(pp);
                        if (auto outParamType = paramType->As<OutTypeBase>())
                        {
                            if (pp < expr->Arguments.Count()
                                && !expr->Arguments[pp]->type.IsLeftValue)
                            {
                                if (!isRewriteMode())
                                {
                                    getSink()->diagnose(
                                        expr->Arguments[pp],
                                        Diagnostics::argumentExpectedLValue,
                                        pp);
                                }
                            }
                        }
                    }
                }
            }
            return rs;
        }

        RefPtr<Expr> visitInvokeExpr(InvokeExpr *expr)
        {
            // check the base expression first
            expr->FunctionExpr = CheckExpr(expr->FunctionExpr);

            // Next check the argument expressions
            for (auto & arg : expr->Arguments)
            {
                arg = CheckExpr(arg);
            }

            return CheckInvokeExprWithCheckedOperands(expr);
        }


        RefPtr<Expr> visitVarExpr(VarExpr *expr)
        {
            // If we've already resolved this expression, don't try again.
            if (expr->declRef)
                return expr;

            expr->type = QualType(getSession()->getErrorType());

            auto lookupResult = LookUp(
                getSession(),
                this, expr->name, expr->scope);
            if (lookupResult.isValid())
            {
                return createLookupResultExpr(
                    lookupResult,
                    nullptr,
                    expr->loc);
            }

            if (!isRewriteMode())
            {
                getSink()->diagnose(expr, Diagnostics::undefinedIdentifier2, expr->name);
            }

            return expr;
        }

        RefPtr<Expr> visitTypeCastExpr(TypeCastExpr * expr)
        {
            // Check the term we are applying first
            auto funcExpr = expr->FunctionExpr;
            funcExpr = CheckTerm(funcExpr);

            // Now ensure that the term represnets a (proper) type.
            TypeExp typeExp;
            typeExp.exp = funcExpr;
            typeExp = CheckProperType(typeExp);

            expr->FunctionExpr = typeExp.exp;
            expr->type.type = typeExp.type;

            // Next check the argument expression (there should be only one)
            for (auto & arg : expr->Arguments)
            {
                arg = CheckExpr(arg);
            }

            // Now process this like any other explicit call (so casts
            // and constructor calls are semantically equivalent).
            return CheckInvokeExprWithCheckedOperands(expr);

#if 0
            expr->Expression = CheckTerm(expr->Expression);
            auto targetType = CheckProperType(expr->TargetType);
            expr->TargetType = targetType;

            // The way to perform casting depends on the types involved
            if (expr->Expression->type->Equals(getSession()->getErrorType()))
            {
                // If the expression being casted has an error type, then just silently succeed
                expr->type = targetType.Ptr();
                return expr;
            }
            else if (auto targetArithType = targetType->AsArithmeticType())
            {
                if (auto exprArithType = expr->Expression->type->AsArithmeticType())
                {
                    // Both source and destination types are arithmetic, so we might
                    // have a valid cast
                    auto targetScalarType = targetArithType->GetScalarType();
                    auto exprScalarType = exprArithType->GetScalarType();

                    if (!IsNumeric(exprScalarType->baseType)) goto fail;
                    if (!IsNumeric(targetScalarType->baseType)) goto fail;

                    // TODO(tfoley): this checking is incomplete here, and could
                    // lead to downstream compilation failures
                    expr->type = targetType.Ptr();
                    return expr;
                }
            }
            // TODO: other cases? Should we allow a cast to succeeed whenever
            // a single-argument constructor for the target type would work?

        fail:
            // Default: in no other case succeds, then the cast failed and we emit a diagnostic.
            if (!isRewriteMode())
            {
                getSink()->diagnose(expr, Diagnostics::invalidTypeCast, expr->Expression->type, targetType->ToString());
            }
            expr->type = QualType(getSession()->getErrorType());
            return expr;
#endif
        }

        // Get the type to use when referencing a declaration
        QualType GetTypeForDeclRef(DeclRef<Decl> declRef)
        {
            return getTypeForDeclRef(
                getSession(),
                this,
                getSink(),
                declRef,
                &typeResult);
        }

        //
        // Some syntax nodes should not occur in the concrete input syntax,
        // and will only appear *after* checking is complete. We need to
        // deal with this cases here, even if they are no-ops.
        //

        RefPtr<Expr> visitDerefExpr(DerefExpr* expr)
        {
            SLANG_DIAGNOSE_UNEXPECTED(getSink(), expr, "should not appear in input syntax");
            return expr;
        }

        RefPtr<Expr> visitSwizzleExpr(SwizzleExpr* expr)
        {
            SLANG_DIAGNOSE_UNEXPECTED(getSink(), expr, "should not appear in input syntax");
            return expr;
        }

        RefPtr<Expr> visitOverloadedExpr(OverloadedExpr* expr)
        {
            SLANG_DIAGNOSE_UNEXPECTED(getSink(), expr, "should not appear in input syntax");
            return expr;
        }

        RefPtr<Expr> visitAggTypeCtorExpr(AggTypeCtorExpr* expr)
        {
            SLANG_DIAGNOSE_UNEXPECTED(getSink(), expr, "should not appear in input syntax");
            return expr;
        }

        //
        //
        //

        RefPtr<Expr> MaybeDereference(RefPtr<Expr> inExpr)
        {
            RefPtr<Expr> expr = inExpr;
            for (;;)
            {
                auto& type = expr->type;
                if (auto pointerLikeType = type->As<PointerLikeType>())
                {
                    type = QualType(pointerLikeType->elementType);

                    auto derefExpr = new DerefExpr();
                    derefExpr->base = expr;
                    derefExpr->type = QualType(pointerLikeType->elementType);

                    // TODO(tfoley): deal with l-value-ness here

                    expr = derefExpr;
                    continue;
                }

                // Default case: just use the expression as-is
                return expr;
            }
        }

        RefPtr<Expr> CheckSwizzleExpr(
            MemberExpr* memberRefExpr,
            RefPtr<Type>      baseElementType,
            IntegerLiteralValue         baseElementCount)
        {
            RefPtr<SwizzleExpr> swizExpr = new SwizzleExpr();
            swizExpr->loc = memberRefExpr->loc;
            swizExpr->base = memberRefExpr->BaseExpression;

            IntegerLiteralValue limitElement = baseElementCount;

            int elementIndices[4];
            int elementCount = 0;

            bool elementUsed[4] = { false, false, false, false };
            bool anyDuplicates = false;
            bool anyError = false;

            auto swizzleText = getText(memberRefExpr->name);

            for (UInt i = 0; i < swizzleText.Length(); i++)
            {
                auto ch = swizzleText[i];
                int elementIndex = -1;
                switch (ch)
                {
                case 'x': case 'r': elementIndex = 0; break;
                case 'y': case 'g': elementIndex = 1; break;
                case 'z': case 'b': elementIndex = 2; break;
                case 'w': case 'a': elementIndex = 3; break;
                default:
                    // An invalid character in the swizzle is an error
                    if (!isRewriteMode())
                    {
                        getSink()->diagnose(swizExpr, Diagnostics::unimplemented, "invalid component name for swizzle");
                    }
                    anyError = true;
                    continue;
                }

                // TODO(tfoley): GLSL requires that all component names
                // come from the same "family"...

                // Make sure the index is in range for the source type
                if (elementIndex >= limitElement)
                {
                    if (!isRewriteMode())
                    {
                        getSink()->diagnose(swizExpr, Diagnostics::unimplemented, "swizzle component out of range for type");
                    }
                    anyError = true;
                    continue;
                }

                // Check if we've seen this index before
                for (int ee = 0; ee < elementCount; ee++)
                {
                    if (elementIndices[ee] == elementIndex)
                        anyDuplicates = true;
                }

                // add to our list...
                elementIndices[elementCount++] = elementIndex;
            }

            for (int ee = 0; ee < elementCount; ++ee)
            {
                swizExpr->elementIndices[ee] = elementIndices[ee];
            }
            swizExpr->elementCount = elementCount;

            if (anyError)
            {
                return CreateErrorExpr(memberRefExpr);
            }
            else if (elementCount == 1)
            {
                // single-component swizzle produces a scalar
                //
                // Note(tfoley): the official HLSL rules seem to be that it produces
                // a one-component vector, which is then implicitly convertible to
                // a scalar, but that seems like it just adds complexity.
                swizExpr->type = QualType(baseElementType);
            }
            else
            {
                // TODO(tfoley): would be nice to "re-sugar" type
                // here if the input type had a sugared name...
                swizExpr->type = QualType(createVectorType(
                    baseElementType,
                    new ConstantIntVal(elementCount)));
            }

            // A swizzle can be used as an l-value as long as there
            // were no duplicates in the list of components
            swizExpr->type.IsLeftValue = !anyDuplicates;

            return swizExpr;
        }

        RefPtr<Expr> CheckSwizzleExpr(
            MemberExpr*	memberRefExpr,
            RefPtr<Type>		baseElementType,
            RefPtr<IntVal>				baseElementCount)
        {
            if (auto constantElementCount = baseElementCount.As<ConstantIntVal>())
            {
                return CheckSwizzleExpr(memberRefExpr, baseElementType, constantElementCount->value);
            }
            else
            {
                if (!isRewriteMode())
                {
                    getSink()->diagnose(memberRefExpr, Diagnostics::unimplemented, "swizzle on vector of unknown size");
                }
                return CreateErrorExpr(memberRefExpr);
            }
        }

        RefPtr<Expr> visitStaticMemberExpr(StaticMemberExpr* expr)
        {
            SLANG_UNEXPECTED("should not occur in unchecked AST");
            return expr;
        }

        RefPtr<Expr> lookupResultFailure(
            MemberExpr*     expr,
            QualType const& baseType)
        {
            if (!isRewriteMode())
            {
                getSink()->diagnose(expr, Diagnostics::noMemberOfNameInType, expr->name, baseType);
            }
            expr->type = QualType(getSession()->getErrorType());
            return expr;

        }

        RefPtr<Expr> visitMemberExpr(MemberExpr * expr)
        {
            expr->BaseExpression = CheckExpr(expr->BaseExpression);

            expr->BaseExpression = MaybeDereference(expr->BaseExpression);

            auto & baseType = expr->BaseExpression->type;

            // Note: Checking for vector types before declaration-reference types,
            // because vectors are also declaration reference types...
            if (auto baseVecType = baseType->AsVectorType())
            {
                return CheckSwizzleExpr(
                    expr,
                    baseVecType->elementType,
                    baseVecType->elementCount);
            }
            else if(auto baseScalarType = baseType->AsBasicType())
            {
                // Treat scalar like a 1-element vector when swizzling
                return CheckSwizzleExpr(
                    expr,
                    baseScalarType,
                    1);
            }
            else if(auto typeType = baseType->As<TypeType>())
            {
                // We are looking up a member inside a type.
                // We want to be careful here because we should only find members
                // that are implicitly or explicitly `static`.
                //
                // TODO: this duplicates a *lot* of logic with the case below.
                // We need to fix that.
                auto type = typeType->type;
                if(auto declRefType = type->AsDeclRefType())
                {
                    if (auto aggTypeDeclRef = declRefType->declRef.As<AggTypeDecl>())
                    {
                        // Checking of the type must be complete before we can reference its members safely
                        EnsureDecl(aggTypeDeclRef.getDecl(), DeclCheckState::Checked);

                        LookupResult lookupResult = LookUpLocal(
                            getSession(),
                            this, expr->name, aggTypeDeclRef);
                        if (!lookupResult.isValid())
                        {
                            return lookupResultFailure(expr, baseType);
                        }

                        // TODO: need to filter for declarations that are valid to refer
                        // to in this context...

                        return createLookupResultExpr(
                            lookupResult,
                            expr->BaseExpression,
                            expr->loc);
                    }
                }
            }
            else if (auto declRefType = baseType->AsDeclRefType())
            {
                if (auto aggTypeDeclRef = declRefType->declRef.As<AggTypeDecl>())
                {
                    // Checking of the type must be complete before we can reference its members safely
                    EnsureDecl(aggTypeDeclRef.getDecl(), DeclCheckState::Checked);

                    LookupResult lookupResult = LookUpLocal(
                        getSession(),
                        this, expr->name, aggTypeDeclRef);
                    if (!lookupResult.isValid())
                    {
                        return lookupResultFailure(expr, baseType);
                    }

                    return createLookupResultExpr(
                        lookupResult,
                        expr->BaseExpression,
                        expr->loc);
                }

                // catch-all
                return lookupResultFailure(expr, baseType);
            }
            // All remaining cases assume we have a `BasicType`
            else if (!baseType->AsBasicType())
                expr->type = QualType(getSession()->getErrorType());
            else
                expr->type = QualType(getSession()->getErrorType());
            if (!baseType->Equals(getSession()->getErrorType()) &&
                expr->type->Equals(getSession()->getErrorType()))
            {
                if (!isRewriteMode())
                {
                    getSink()->diagnose(expr, Diagnostics::typeHasNoPublicMemberOfName, baseType, expr->name);
                }
            }
            return expr;
        }
        SemanticsVisitor & operator = (const SemanticsVisitor &) = delete;


        //

        RefPtr<Expr> visitInitializerListExpr(InitializerListExpr* expr)
        {
            // When faced with an initializer list, we first just check the sub-expressions blindly.
            // Actually making them conform to a desired type will wait for when we know the desired
            // type based on context.

            for( auto& arg : expr->args )
            {
                arg = CheckTerm(arg);
            }

            expr->type = getSession()->getInitializerListType();

            return expr;
        }

        void importModuleIntoScope(Scope* scope, ModuleDecl* moduleDecl)
        {
            // If we've imported this one already, then
            // skip the step where we modify the current scope.
            if (importedModules.Contains(moduleDecl))
            {
                return;
            }
            importedModules.Add(moduleDecl);


            // Create a new sub-scope to wire the module
            // into our lookup chain.
            auto subScope = new Scope();
            subScope->containerDecl = moduleDecl;

            subScope->nextSibling = scope->nextSibling;
            scope->nextSibling = subScope;

            // Also import any modules from nested `import` declarations
            // with the `__exported` modifier
            for (auto importDecl : moduleDecl->getMembersOfType<ImportDecl>())
            {
                if (!importDecl->HasModifier<ExportedModifier>())
                    continue;

                importModuleIntoScope(scope, importDecl->importedModuleDecl.Ptr());
            }
        }

        void visitEmptyDecl(EmptyDecl* /*decl*/)
        {
            // nothing to do
        }

        void visitImportDecl(ImportDecl* decl)
        {
            if(decl->IsChecked(DeclCheckState::Checked))
                return;

            // We need to look for a module with the specified name
            // (whether it has already been loaded, or needs to
            // be loaded), and then put its declarations into
            // the current scope.

            auto name = decl->moduleNameAndLoc.name;
            auto scope = decl->scope;

            // Try to load a module matching the name
            auto importedModuleDecl = findOrImportModule(request, name, decl->moduleNameAndLoc.loc);

            // If we didn't find a matching module, then bail out
            if (!importedModuleDecl)
                return;

            // Record the module that was imported, so that we can use
            // it later during code generation.
            decl->importedModuleDecl = importedModuleDecl;

            importModuleIntoScope(scope.Ptr(), importedModuleDecl.Ptr());

            decl->SetCheckState(DeclCheckState::Checked);
        }
    };

    void checkTranslationUnit(
        TranslationUnitRequest* translationUnit)
    {
        SemanticsVisitor visitor(
            &translationUnit->compileRequest->mSink,
            translationUnit->compileRequest,
            translationUnit);

        visitor.checkDecl(translationUnit->SyntaxNode);
    }

    //

    // Get the type to use when referencing a declaration
    QualType getTypeForDeclRef(
        Session*                session,
        SemanticsVisitor*       sema,
        DiagnosticSink*         sink,
        DeclRef<Decl>           declRef,
        RefPtr<Type>* outTypeResult)
    {
        if( sema )
        {
            sema->EnsureDecl(declRef.getDecl());
        }

        // We need to insert an appropriate type for the expression, based on
        // what we found.
        if (auto varDeclRef = declRef.As<VarDeclBase>())
        {
            QualType qualType;
            qualType.type = GetType(varDeclRef);
            qualType.IsLeftValue = true; // TODO(tfoley): allow explicit `const` or `let` variables
            return qualType;
        }
        else if (auto typeAliasDeclRef = declRef.As<TypeDefDecl>())
        {
            auto type = getNamedType(session, typeAliasDeclRef);
            *outTypeResult = type;
            return QualType(getTypeType(type));
        }
        else if (auto aggTypeDeclRef = declRef.As<AggTypeDecl>())
        {
            auto type = DeclRefType::Create(session, aggTypeDeclRef);
            *outTypeResult = type;
            return QualType(getTypeType(type));
        }
        else if (auto simpleTypeDeclRef = declRef.As<SimpleTypeDecl>())
        {
            auto type = DeclRefType::Create(session, simpleTypeDeclRef);
            *outTypeResult = type;
            return QualType(getTypeType(type));
        }
        else if (auto genericDeclRef = declRef.As<GenericDecl>())
        {
            auto type = getGenericDeclRefType(session, genericDeclRef);
            *outTypeResult = type;
            return QualType(getTypeType(type));
        }
        else if (auto funcDeclRef = declRef.As<CallableDecl>())
        {
            auto type = getFuncType(session, funcDeclRef);
            return QualType(type);
        }

        if( sink )
        {
            sink->diagnose(declRef, Diagnostics::unimplemented, "cannot form reference to this kind of declaration");
        }
        return QualType(session->getErrorType());
    }

    QualType getTypeForDeclRef(
        Session*        session,
        DeclRef<Decl>   declRef)
    {
        RefPtr<Type> typeResult;
        return getTypeForDeclRef(session, nullptr, nullptr, declRef, &typeResult);
    }

    DeclRef<ExtensionDecl> ApplyExtensionToType(
        SemanticsVisitor*       semantics,
        ExtensionDecl*          extDecl,
        RefPtr<Type>  type)
    {
        if(!semantics)
            return DeclRef<ExtensionDecl>();

        return semantics->ApplyExtensionToType(extDecl, type);
    }

}
