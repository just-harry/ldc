//===-- tocall.cpp --------------------------------------------------------===//
//
//                         LDC – the LLVM D compiler
//
// This file is distributed under the BSD-style LDC license. See the LICENSE
// file for details.
//
//===----------------------------------------------------------------------===//

#include "declaration.h"
#include "id.h"
#include "mtype.h"
#include "target.h"
#include "pragma.h"
#include "gen/abi.h"
#include "gen/dvalue.h"
#include "gen/functions.h"
#include "gen/irstate.h"
#include "gen/llvm.h"
#include "gen/llvmhelpers.h"
#include "gen/logger.h"
#include "gen/nested.h"
#include "gen/tollvm.h"
#include "ir/irtype.h"

#if LDC_LLVM_VER == 302
namespace llvm
{
    typedef llvm::Attributes Attribute;
}
#endif

//////////////////////////////////////////////////////////////////////////////////////////

IrFuncTy &DtoIrTypeFunction(DValue* fnval)
{
    if (DFuncValue* dfnval = fnval->isFunc())
    {
        if (dfnval->func)
            return getIrFunc(dfnval->func)->irFty;
    }

    Type* type = stripModifiers(fnval->getType()->toBasetype());
    DtoType(type);
    assert(type->ctype);
    return type->ctype->getIrFuncTy();
}

TypeFunction* DtoTypeFunction(DValue* fnval)
{
    Type* type = fnval->getType()->toBasetype();
    if (type->ty == Tfunction)
    {
         return static_cast<TypeFunction*>(type);
    }
    else if (type->ty == Tdelegate)
    {
        // FIXME: There is really no reason why the function type should be
        // unmerged at this stage, but the frontend still seems to produce such
        // cases; for example for the uint(uint) next type of the return type of
        // (&zero)(), leading to a crash in DtoCallFunction:
        // ---
        // void test8198() {
        //   uint delegate(uint) zero() { return null; }
        //   auto a = (&zero)()(0);
        // }
        // ---
        // Calling merge() here works around the symptoms, but does not fix the
        // root cause.

        Type* next = type->nextOf()->merge();
        assert(next->ty == Tfunction);
        return static_cast<TypeFunction*>(next);
    }

    llvm_unreachable("Cannot get TypeFunction* from non lazy/function/delegate");
}

//////////////////////////////////////////////////////////////////////////////////////////

LLValue* DtoCallableValue(DValue* fn)
{
    Type* type = fn->getType()->toBasetype();
    if (type->ty == Tfunction)
    {
        return fn->getRVal();
    }
    else if (type->ty == Tdelegate)
    {
        if (fn->isLVal())
        {
            LLValue* dg = fn->getLVal();
            LLValue* funcptr = DtoGEPi(dg, 0, 1);
            return DtoLoad(funcptr, ".funcptr");
        }
        else
        {
            LLValue* dg = fn->getRVal();
            assert(isaStruct(dg));
            return gIR->ir->CreateExtractValue(dg, 1, ".funcptr");
        }
    }

    llvm_unreachable("Not a callable type.");
}

//////////////////////////////////////////////////////////////////////////////////////////

LLFunctionType* DtoExtractFunctionType(LLType* type)
{
    if (LLFunctionType* fty = isaFunction(type))
        return fty;
    else if (LLPointerType* pty = isaPointer(type))
    {
        if (LLFunctionType* fty = isaFunction(pty->getElementType()))
            return fty;
    }
    return NULL;
}

//////////////////////////////////////////////////////////////////////////////////////////

static LLValue *fixArgument(DValue *argval, IrFuncTy &irFty, LLType *callableArgType, size_t argIndex)
{
#if 0
    IF_LOG {
        Logger::cout() << "Argument before ABI: " << *argval->getRVal() << '\n';
        Logger::cout() << "Argument type before ABI: " << *DtoType(argval->getType()) << '\n';
    }
#endif

    // give the ABI a say
    LLValue* arg = irFty.putParam(argval->getType(), argIndex, argval);

#if 0
    IF_LOG {
        Logger::cout() << "Argument after ABI: " << *arg << '\n';
        Logger::cout() << "Argument type after ABI: " << *arg->getType() << '\n';
    }
#endif

    // Hack around LDC assuming structs and static arrays are in memory:
    // If the function wants a struct, and the argument value is a
    // pointer to a struct, load from it before passing it in.
    int ty = argval->getType()->toBasetype()->ty;
    if (isaPointer(arg) && !isaPointer(callableArgType) &&
        (ty == Tstruct || ty == Tsarray))
    {
        Logger::println("Loading struct type for function argument");
        arg = DtoLoad(arg);
    }

    // parameter type mismatch, this is hard to get rid of
    if (arg->getType() != callableArgType)
    {
    #if 1
        IF_LOG {
            Logger::cout() << "arg:     " << *arg << '\n';
            Logger::cout() << "of type: " << *arg->getType() << '\n';
            Logger::cout() << "expects: " << *callableArgType << '\n';
        }
    #endif
        if (isaStruct(arg))
            arg = DtoAggrPaint(arg, callableArgType);
        else
            arg = DtoBitCast(arg, callableArgType);
    }
    return arg;
}

static LLValue* fixOptionalArgument(DValue* argval,
#if LDC_LLVM_VER >= 303
    llvm::AttrBuilder& attrs
#else
    llvm::Attributes& attrs
#endif
    )
{
    Type* type = argval->getType();

#if 0
    IF_LOG{
        Logger::cout() << "Optional argument before ABI: " << *argval->getRVal() << '\n';
        Logger::cout() << "Optional argument type before ABI: " << *DtoType(type) << '\n';
    }
#endif

#if LDC_LLVM_VER >= 302
    llvm::AttrBuilder initialAttrs;
#else
    llvm::Attributes initialAttrs = llvm::Attribute::None;
#endif

    // byval
    if (gABI->passByVal(type))
    {
#if LDC_LLVM_VER >= 302
        initialAttrs.addAttribute(llvm::Attribute::ByVal);
#else
        initialAttrs |= llvm::Attribute::ByVal;
#endif
    }
    // sext/zext
    else
    {
#if LDC_LLVM_VER >= 303
        if (llvm::Attribute::AttrKind a = DtoShouldExtend(type))
            initialAttrs.addAttribute(a);
#elif LDC_LLVM_VER == 302
        if (llvm::Attributes::AttrVal a = DtoShouldExtend(type))
            initialAttrs.addAttribute(a);
#else
        initialAttrs |= DtoShouldExtend(type);
#endif
    }

    // give the ABI a say
#if LDC_LLVM_VER == 302
    IrFuncTyArg irArg(type, false, llvm::Attributes::get(gIR->context(), initialAttrs));
#else
    IrFuncTyArg irArg(type, false, initialAttrs);
#endif
    gABI->rewriteArgument(irArg);

    LLValue* arg = (irArg.rewrite
        ? irArg.rewrite->put(type, argval)
        : argval->getRVal());
    attrs = irArg.attrs;

#if 0
    IF_LOG{
        Logger::cout() << "Optional argument after ABI: " << *arg << '\n';
        Logger::cout() << "Optional argument type after ABI: " << *arg->getType() << '\n';
    }
#endif

    return arg;
}

//////////////////////////////////////////////////////////////////////////////////////////

#if LDC_LLVM_VER >= 303
static inline void addToAttributes(llvm::AttributeSet &Attrs,
                                   unsigned Idx, llvm::AttrBuilder B)
{
    llvm::AttrBuilder Builder(B);
    Attrs = Attrs.addAttributes(gIR->context(), Idx,
                                llvm::AttributeSet::get(gIR->context(), Idx, Builder));
}
#else
static inline void addToAttributes(std::vector<llvm::Attributes> &attrs,
                                   unsigned Idx, llvm::Attributes Attr)
{
    if (Idx >= attrs.size())
    {
#if LDC_LLVM_VER == 302
        attrs.resize(Idx + 1);
#elif LDC_LLVM_VER < 302
        attrs.resize(Idx + 1, llvm::Attribute::None);
#endif
    }

    attrs[Idx] = Attr;
}
#endif


//////////////////////////////////////////////////////////////////////////////////////////

static void addTypeinfoArrayArgumentForDVarArg(std::vector<LLValue*>& args,
#if LDC_LLVM_VER >= 303
                                               llvm::AttributeSet &attrs,
#else
                                               std::vector<llvm::Attributes> &attrs,
#endif
                                               TypeFunction* tf, IrFuncTy &irFty,
                                               Expressions* arguments, size_t argidx)
{
    IF_LOG Logger::println("doing d-style variadic arguments");
    LOG_SCOPE

    // number of non variadic args
    int begin = Parameter::dim(tf->parameters);
    IF_LOG Logger::println("num non vararg params = %d", begin);

    // get n args in arguments list
    size_t n_arguments = arguments ? arguments->dim : 0;

    const size_t numVariadicArgs = n_arguments - begin;

    // build type info array
    LLType* typeinfotype = DtoType(Type::dtypeinfo->type);
    LLArrayType* typeinfoarraytype = LLArrayType::get(typeinfotype, numVariadicArgs);

    llvm::GlobalVariable* typeinfomem =
        new llvm::GlobalVariable(*gIR->module, typeinfoarraytype, true, llvm::GlobalValue::InternalLinkage, NULL, "._arguments.storage");
    IF_LOG Logger::cout() << "_arguments storage: " << *typeinfomem << '\n';

    std::vector<LLConstant*> vtypeinfos;
    vtypeinfos.reserve(n_arguments);
    for (size_t i=begin; i<n_arguments; i++)
    {
        vtypeinfos.push_back(DtoTypeInfoOf((*arguments)[i]->type));
    }

    // apply initializer
    LLConstant* tiinits = LLConstantArray::get(typeinfoarraytype, vtypeinfos);
    typeinfomem->setInitializer(tiinits);

    // put data in d-array
    LLConstant* pinits[] = {
        DtoConstSize_t(numVariadicArgs),
        llvm::ConstantExpr::getBitCast(typeinfomem, getPtrToType(typeinfotype))
    };
    LLType* tiarrty = DtoType(Type::dtypeinfo->type->arrayOf());
    tiinits = LLConstantStruct::get(isaStruct(tiarrty), llvm::ArrayRef<LLConstant*>(pinits));
    LLValue* typeinfoarrayparam = new llvm::GlobalVariable(*gIR->module, tiarrty,
        true, llvm::GlobalValue::InternalLinkage, tiinits, "._arguments.array");

    // add argument
    args.push_back(DtoLoad(typeinfoarrayparam));
    if (HAS_ATTRIBUTES(irFty.arg_arguments->attrs)) {
        addToAttributes(attrs, argidx, irFty.arg_arguments->attrs);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////

// FIXME: this function is a mess !

DValue* DtoCallFunction(Loc& loc, Type* resulttype, DValue* fnval, Expressions* arguments, llvm::Value *retvar)
{
    IF_LOG Logger::println("DtoCallFunction()");
    LOG_SCOPE

    // the callee D type
    Type* calleeType = fnval->getType();

    // make sure the callee type has been processed
    DtoType(calleeType);

    // get func value if any
    DFuncValue* dfnval = fnval->isFunc();

    // handle intrinsics
    bool intrinsic = (dfnval && dfnval->func && dfnval->func->llvmInternal == LLVMintrinsic);

    // handle special vararg intrinsics
    bool va_intrinsic = (dfnval && dfnval->func && DtoIsVaIntrinsic(dfnval->func));

    // get function type info
    IrFuncTy &irFty = DtoIrTypeFunction(fnval);
    TypeFunction* tf = DtoTypeFunction(fnval);

    // misc
    bool retinptr = irFty.arg_sret;
    bool thiscall = irFty.arg_this;
    bool delegatecall = (calleeType->toBasetype()->ty == Tdelegate);
    bool nestedcall = irFty.arg_nest;
    bool dvarargs = (tf->linkage == LINKd && tf->varargs == 1);

    llvm::CallingConv::ID callconv = gABI->callingConv(tf->linkage);

    // get callee llvm value
    LLValue* callable = DtoCallableValue(fnval);
    LLFunctionType* callableTy = DtoExtractFunctionType(callable->getType());
    assert(callableTy);

//     IF_LOG Logger::cout() << "callable: " << *callable << '\n';

    // get n arguments
    size_t n_arguments = arguments ? arguments->dim : 0;

    // get llvm argument iterator, for types
    LLFunctionType::param_iterator argbegin = callableTy->param_begin();
    LLFunctionType::param_iterator argiter = argbegin;

    // parameter attributes
#if LDC_LLVM_VER >= 303
    llvm::AttributeSet attrs;
#else
    std::vector<llvm::Attributes> attrs;
    // return attributes + attributes for max 3 implicit args (sret, context, _arguments) & all regular args
    attrs.reserve(1 + 3 + n_arguments);
#endif

    // return attrs
    if (HAS_ATTRIBUTES(irFty.ret->attrs))
    {
        addToAttributes(attrs, 0, irFty.ret->attrs);
    }

    // handle implicit arguments
    std::vector<LLValue*> args;
    args.reserve(irFty.args.size());

    // return in hidden ptr is first
    if (retinptr)
    {
        if (!retvar)
            retvar = DtoRawAlloca((*argiter)->getContainedType(0), resulttype->alignsize(), ".rettmp");
        ++argiter;
        args.push_back(retvar);

        // add attrs for hidden ptr
        addToAttributes(attrs, 1, irFty.arg_sret->attrs);

        // verify that sret and/or inreg attributes are set
#if LDC_LLVM_VER >= 303
        llvm::AttrBuilder sretAttrs = irFty.arg_sret->attrs;
        assert((sretAttrs.contains(llvm::Attribute::StructRet) || sretAttrs.contains(llvm::Attribute::InReg))
            && "Sret arg not sret or inreg?");
#elif LDC_LLVM_VER == 302
        llvm::Attributes sretAttrs = irFty.arg_sret->attrs;
        assert((sretAttrs.hasAttribute(llvm::Attributes::StructRet) || sretAttrs.hasAttribute(llvm::Attributes::InReg))
            && "Sret arg not sret or inreg?");
#else
        llvm::Attributes sretAttrs = irFty.arg_sret->attrs;
        assert((sretAttrs & (llvm::Attribute::StructRet | llvm::Attribute::InReg))
            && "Sret arg not sret or inreg?");
#endif
    }

    // then comes a context argument...
    if(thiscall || delegatecall || nestedcall)
    {
        if (dfnval && (dfnval->func->ident == Id::ensure || dfnval->func->ident == Id::require)) {
            // ... which can be the this "context" argument for a contract
            // invocation (in D2, we do not generate a full nested contexts
            // for __require/__ensure as the needed parameters are passed
            // explicitly, while in D1, the normal nested function handling
            // mechanisms are used)
            LLValue* thisarg = DtoBitCast(DtoLoad(gIR->func()->thisArg), getVoidPtrType());
            ++argiter;
            args.push_back(thisarg);
        }
        else
        if (thiscall && dfnval && dfnval->vthis)
        {
            // ... or a normal 'this' argument
            LLValue* thisarg = DtoBitCast(dfnval->vthis, *argiter);
            ++argiter;
            args.push_back(thisarg);
        }
        else if (delegatecall)
        {
            // ... or a delegate context arg
            LLValue* ctxarg;
            if (fnval->isLVal())
            {
                ctxarg = DtoLoad(DtoGEPi(fnval->getLVal(), 0, 0), ".ptr");
            }
            else
            {
                ctxarg = gIR->ir->CreateExtractValue(fnval->getRVal(), 0, ".ptr");
            }
            ctxarg = DtoBitCast(ctxarg, *argiter);
            ++argiter;
            args.push_back(ctxarg);
        }
        else if (nestedcall)
        {
            // ... or a nested function context arg
            if (dfnval) {
                LLValue* contextptr = DtoNestedContext(loc, dfnval->func);
                contextptr = DtoBitCast(contextptr, getVoidPtrType());
                args.push_back(contextptr);
            } else {
                args.push_back(llvm::UndefValue::get(getVoidPtrType()));
            }
            ++argiter;
        }
        else
        {
            error(loc, "Context argument required but none given");
            fatal();
        }

        // add attributes for context argument
        if (irFty.arg_this && HAS_ATTRIBUTES(irFty.arg_this->attrs))
        {
            addToAttributes(attrs, retinptr ? 2 : 1, irFty.arg_this->attrs);
        }
        else if (irFty.arg_nest && HAS_ATTRIBUTES(irFty.arg_nest->attrs))
        {
            addToAttributes(attrs, retinptr ? 2 : 1, irFty.arg_nest->attrs);
        }
    }

    // handle the rest of the arguments based on param passing style

    // variadic intrinsics need some custom casts
    if (va_intrinsic)
    {
        for (size_t i=0; i<n_arguments; i++)
        {
            DValue* expelem = toElem((*arguments)[i]);
            // cast to va_list*
            LLValue* val = DtoBitCast(expelem->getLVal(), getVoidPtrType());
            ++argiter;
            args.push_back(val);
        }
    }
    // normal/vararg function call
    else
    {
        // D vararg functions need an additional "TypeInfo[] _arguments" argument
        if (dvarargs)
        {
            addTypeinfoArrayArgumentForDVarArg(args, attrs, tf, irFty, arguments, argiter - argbegin + 1);
            ++argiter;
        }

        Logger::println("doing normal arguments");
        IF_LOG {
            Logger::println("Arguments so far: (%d)", static_cast<int>(args.size()));
            Logger::indent();
            for (size_t i = 0; i < args.size(); i++) {
                Logger::cout() << *args[i] << '\n';
            }
            Logger::undent();
            Logger::cout() << "Function type: " << tf->toChars() << '\n';
            //Logger::cout() << "LLVM functype: " << *callable->getType() << '\n';
        }

        size_t n = Parameter::dim(tf->parameters);
        std::vector<DValue*> argvals;
        argvals.reserve(n);
        if (dfnval && dfnval->func->isArrayOp) {
            // For array ops, the druntime implementation signatures are crafted
            // specifically such that the evaluation order is as expected with
            // the strange DMD reverse parameter passing order. Thus, we need
            // to actually build the arguments right-to-left for them.
            for (int i=n-1; i>=0; --i) {
                Parameter* fnarg = Parameter::getNth(tf->parameters, i);
                assert(fnarg);
                DValue* argval = DtoArgument(fnarg, (*arguments)[i]);
                argvals.insert(argvals.begin(), argval);
            }
        } else {
            for (size_t i=0; i<n; ++i) {
                Parameter* fnarg = Parameter::getNth(tf->parameters, i);
                assert(fnarg);
                DValue* argval = DtoArgument(fnarg, (*arguments)[i]);
                argvals.push_back(argval);
            }
        }

        // do formal params
        int beg = argiter-argbegin;
        for (size_t i=0; i<n; i++)
        {
            DValue* argval = argvals.at(i);

            int j = (irFty.reverseParams ? n - i - 1 : i);
            LLValue *arg = fixArgument(argval, irFty, callableTy->getParamType(beg + j), i);
            args.push_back(arg);

            addToAttributes(attrs, beg + 1 + j, irFty.args[i]->attrs);
            ++argiter;
        }

        // reverse the relevant params
        if (irFty.reverseParams)
        {
            std::reverse(args.begin() + beg, args.end());
        }

        // do C varargs
        if (n_arguments > n)
        {
            for (size_t i = n; i < n_arguments; i++)
            {
                DValue* argval = DtoArgument(0, (*arguments)[i]);

#if LDC_LLVM_VER >= 303
                llvm::AttrBuilder argAttrs;
#elif LDC_LLVM_VER == 302
                llvm::Attributes argAttrs;
#else
                llvm::Attributes argAttrs = llvm::Attribute::None;
#endif
                LLValue* arg = fixOptionalArgument(argval, argAttrs);
                args.push_back(arg);
                addToAttributes(attrs, beg + 1 + i, argAttrs);

                ++argiter;
            }
        }
    }

#if 0
    IF_LOG {
        Logger::println("%lu params passed", args.size());
        for (int i=0; i<args.size(); ++i) {
            assert(args[i]);
            Logger::cout() << "arg["<<i<<"] = " << *args[i] << '\n';
        }
    }
#endif

    // void returns cannot not be named
    const char* varname = "";
    if (callableTy->getReturnType() != LLType::getVoidTy(gIR->context()))
        varname = "tmp";

#if 0
    IF_LOG Logger::cout() << "Calling: " << *callable << '\n';
#endif

    // call the function
    LLCallSite call = gIR->CreateCallOrInvoke(callable, args, varname);

    // get return value
    LLValue* retllval = (retinptr) ? args[0] : call.getInstruction();

    // Ignore ABI for intrinsics
    if (!intrinsic && !retinptr)
    {
        // do abi specific return value fixups
        DImValue dretval(tf->next, retllval);
        retllval = irFty.getRet(tf->next, &dretval);
    }

    // Hack around LDC assuming structs and static arrays are in memory:
    // If the function returns a struct or a static array, and the return
    // value is not a pointer to a struct or a static array, store it to
    // a stack slot before continuing.
    int ty = tf->next->toBasetype()->ty;
    if ((ty == Tstruct && !isaPointer(retllval))
        || (ty == Tsarray && isaArray(retllval))
        )
    {
        Logger::println("Storing return value to stack slot");
        LLValue* mem = DtoRawAlloca(retllval->getType(), 0);
        DtoStore(retllval, mem);
        retllval = mem;
    }

    // repaint the type if necessary
    if (resulttype)
    {
        Type* rbase = stripModifiers(resulttype->toBasetype());
        Type* nextbase = stripModifiers(tf->nextOf()->toBasetype());
        if (!rbase->equals(nextbase))
        {
            IF_LOG Logger::println("repainting return value from '%s' to '%s'", tf->nextOf()->toChars(), rbase->toChars());
            switch(rbase->ty)
            {
            case Tarray:
                if (tf->isref)
                    retllval = DtoBitCast(retllval, DtoType(rbase->pointerTo()));
                else
                retllval = DtoAggrPaint(retllval, DtoType(rbase));
                break;

            case Tsarray:
                // nothing ?
                break;

            case Tclass:
            case Taarray:
            case Tpointer:
                if (tf->isref)
                    retllval = DtoBitCast(retllval, DtoType(rbase->pointerTo()));
                else
                retllval = DtoBitCast(retllval, DtoType(rbase));
                break;

            case Tstruct:
                if (nextbase->ty == Taarray && !tf->isref)
                {
                    // In the D2 frontend, the associative array type and its
                    // object.AssociativeArray representation are used
                    // interchangably in some places. However, AAs are returned
                    // by value and not in an sret argument, so if the struct
                    // type will be used, give the return value storage here
                    // so that we get the right amount of indirections.
                    LLValue* tmp = DtoAlloca(rbase, ".aalvauetmp");
                    LLValue* val = DtoInsertValue(
                        llvm::UndefValue::get(DtoType(rbase)), retllval, 0);
                    DtoStore(val, tmp);
                    retllval = tmp;
                    retinptr = true;
                    break;
                }
                // Fall through.

            default:
                // Unfortunately, DMD has quirks resp. bugs with regard to name
                // mangling: For voldemort-type functions which return a nested
                // struct, the mangled name of the return type changes during
                // semantic analysis.
                //
                // (When the function deco is first computed as part of
                // determining the return type deco, its return type part is
                // left off to avoid cycles. If mangle/toDecoBuffer is then
                // called again for the type, it will pick up the previous
                // result and return the full deco string for the nested struct
                // type, consisting of both the full mangled function name, and
                // the struct identifier.)
                //
                // Thus, the type merging in stripModifiers does not work
                // reliably, and the equality check above can fail even if the
                // types only differ in a qualifier.
                //
                // Because a proper fix for this in the frontend is hard, we
                // just carry on and hope that the frontend didn't mess up,
                // i.e. that the LLVM types really match up.
                //
                // An example situation where this case occurs is:
                // ---
                // auto iota() {
                //     static struct Result {
                //         this(int) {}
                //         inout(Result) test() inout { return cast(inout)Result(0); }
                //     }
                //     return Result.init;
                // }
                // void main() { auto r = iota(); }
                // ---
                Logger::println("Unknown return mismatch type, ignoring.");
                break;
            }
            IF_LOG Logger::cout() << "final return value: " << *retllval << '\n';
        }
    }

    // set calling convention and parameter attributes
#if LDC_LLVM_VER >= 303
    llvm::AttributeSet attrlist = attrs;
#else
    std::vector<llvm::AttributeWithIndex> attrsWithIndex;
    attrsWithIndex.reserve(attrs.size());
    for (size_t i = 0; i < attrs.size(); ++i)
        attrsWithIndex.push_back(llvm::AttributeWithIndex::get(i, attrs[i]));
#if LDC_LLVM_VER == 302
    llvm::AttrListPtr attrlist = llvm::AttrListPtr::get(gIR->context(),
        llvm::ArrayRef<llvm::AttributeWithIndex>(attrsWithIndex));
#else
    llvm::AttrListPtr attrlist = llvm::AttrListPtr::get(attrsWithIndex.begin(), attrsWithIndex.end());
#endif
#endif
    if (dfnval && dfnval->func)
    {
        LLFunction* llfunc = llvm::dyn_cast<LLFunction>(dfnval->val);
        if (llfunc && llfunc->isIntrinsic()) // override intrinsic attrs
#if LDC_LLVM_VER >= 302
            attrlist = llvm::Intrinsic::getAttributes(gIR->context(), static_cast<llvm::Intrinsic::ID>(llfunc->getIntrinsicID()));
#else
            attrlist = llvm::Intrinsic::getAttributes(static_cast<llvm::Intrinsic::ID>(llfunc->getIntrinsicID()));
#endif
        else
            call.setCallingConv(callconv);
    }
    else
        call.setCallingConv(callconv);
    call.setAttributes(attrlist);

    // if we are returning through a pointer arg
    // or if we are returning a reference
    // make sure we provide a lvalue back!
    if (retinptr
        || tf->isref
        )
        return new DVarValue(resulttype, retllval);

    return new DImValue(resulttype, retllval);
}
