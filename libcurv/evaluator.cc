// Copyright 2016-2020 Doug Moen
// Licensed under the Apache License, version 2.0
// See accompanying file LICENSE or https://www.apache.org/licenses/LICENSE-2.0

#include <libcurv/context.h>
#include <libcurv/exception.h>
#include <libcurv/function.h>
#include <libcurv/list.h>
#include <libcurv/meaning.h>
#include <libcurv/module.h>
#include <libcurv/prim.h>
#include <libcurv/record.h>
#include <libcurv/sc_compiler.h>
#include <libcurv/string.h>
#include <cmath>

namespace curv {

Value
tail_eval_frame(std::unique_ptr<Frame> f)
{
    while (f->next_op_ != nullptr)
        f->next_op_->tail_eval(f);
    return f->result_;
}

Value
Operation::eval(Frame& f) const
{
    throw Exception(At_Phrase(*syntax_, f), "not an expression");
}

void
Operation::tail_eval(std::unique_ptr<Frame>& f) const
{
    f->result_ = eval(*f);
    f->next_op_ = nullptr;
}

void
Operation::Action_Executor::push_value(Value, const Context& cstmt)
{
    throw Exception(cstmt, "illegal statement type: expecting an action");
}
void
Operation::Action_Executor::push_field(Symbol_Ref, Value, const Context& cstmt)
{
    throw Exception(cstmt, "illegal statement type: expecting an action");
}

void
Operation::List_Executor::push_value(Value val, const Context& cx)
{
    list_.push_back(val);
}
void
Operation::List_Executor::push_field(Symbol_Ref, Value, const Context& cstmt)
{
    throw Exception(cstmt,
        "illegal statement type: can't add record fields to a list");
}

void
Operation::Record_Executor::push_value(Value, const Context& cstmt)
{
    throw Exception(cstmt,
        "illegal statement type: can't add list elements to a record");
}
void
Operation::Record_Executor::push_field(Symbol_Ref name, Value value, const Context& cx)
{
    record_.fields_[name] = value;
}

void
Just_Expression::exec(Frame& f, Executor& ex) const
{
    ex.push_value(eval(f), At_Phrase(*syntax_,f));
}

void
Null_Action::exec(Frame&, Executor&) const
{
}

Value
Constant::eval(Frame&) const
{
    return value_;
}

Value
Symbolic_Ref::eval(Frame& f) const
{
    auto& m = *f.nonlocals_;
    auto b = m.dictionary_->find(name_);
    assert(b != m.dictionary_->end());
    return m.get(b->second);
}

Value
Module_Data_Ref::eval(Frame& f) const
{
    Module& m = (Module&)f[slot_].to_ref_unsafe();
    assert(m.subtype_ == Ref_Value::sty_module);
    return m.at(index_);
}

Value
Nonlocal_Data_Ref::eval(Frame& f) const
{
    return f.nonlocals_->at(slot_);
}

Value
Local_Data_Ref::eval(Frame& f) const
{
    return f[slot_];
}

Value record_at(Value rec, Symbol_Ref id, const Context& cx)
{
#if 0
    if (auto list = rec.maybe<const List>()) {
        Shared<List> result = List::make(list->size());
        for (unsigned i = 0; i < list->size(); ++i)
            result->at(i) = record_at(list->at(i), id, cx);
        return {result};
    }
    else
#endif
        return rec.at(id, cx);
}
Value
Dot_Expr::eval(Frame& f) const
{
    Value basev = base_->eval(f);
    Symbol_Ref id = selector_.eval(f);
    return record_at(basev, id, At_Phrase(*base_->syntax_, f));
}

#define BOOL_EXPR_EVAL(AND_EXPR, AND, NOT, FALSE) \
Value AND_EXPR::eval(Frame& f) const \
{ \
    Value av = arg1_->eval(f); \
    if (av.is_bool()) { \
        /* fast path */ \
        if (NOT av.to_bool_unsafe()) \
            return {FALSE}; \
        Value bv = arg2_->eval(f); \
        assert_bool(bv, At_Phrase(*arg2_->syntax_, f)); \
        return bv; \
    } \
    /* slow path, handle case where arg1 is a reactive Bool */ \
    assert_bool(av, At_Phrase(*arg1_->syntax_, f)); \
    /* TODO: if arg2_->eval aborts, construct Error value and continue. */ \
    Value bv = arg2_->eval(f); \
    if (bv.is_bool()) { \
        /* The 'return {false}' case is importance for correctness; */ \
        /* see new_core/Reactive "Lazy Boolean Operators" */ \
        bool b = bv.to_bool_unsafe(); \
        if (NOT b) return {FALSE}; else return av; \
    } \
    assert_bool(bv, At_Phrase(*arg2_->syntax_, f)); \
    return {make<Reactive_Expression>( \
        SC_Type::Bool(), \
        make<AND_EXPR>( \
            share(*syntax_), \
            to_expr(av, *arg1_->syntax_), \
            to_expr(bv, *arg2_->syntax_)), \
        At_Phrase(*syntax_, f))}; \
} \
void AND_EXPR::print(std::ostream& out) const \
  { out<<"("<<*arg1_<<#AND<<*arg2_<<")"; }
BOOL_EXPR_EVAL(And_Expr, &&, !, false)
BOOL_EXPR_EVAL(Or_Expr, ||, , true)

Value
If_Op::eval(Frame& f) const
{
    throw Exception{At_Phrase{*syntax_, f},
        "if: not an expression (missing else clause)"};
}
void
If_Op::exec(Frame& f, Executor& ex) const
{
    bool a = arg1_->eval(f).to_bool(At_Phrase(*arg1_->syntax_, f));
    if (a)
        arg2_->exec(f, ex);
}

Value
If_Else_Op::eval(Frame& f) const
{
    Value cond = arg1_->eval(f);
    At_Phrase cx(*arg1_->syntax_, f);
    if (cond.is_bool()) {
        if (cond.to_bool(cx))
            return arg2_->eval(f);
        else
            return arg3_->eval(f);
    }
    auto re = cond.maybe<Reactive_Value>();
    if (re && re->sctype_ == SC_Type::Bool()) {
        Value a2 = arg2_->eval(f);
        Value a3 = arg3_->eval(f);
        SC_Type t2 = sc_type_of(a2);
        SC_Type t3 = sc_type_of(a3);
        if (t2 == t3) {
            return {make<Reactive_Expression>(
                t2,
                make<If_Else_Op>(
                    share(*syntax_),
                    to_expr(cond, *arg1_->syntax_),
                    to_expr(a2, *arg2_->syntax_),
                    to_expr(a3, *arg3_->syntax_)),
                At_Phrase(*syntax_, f))};
        }
        throw Exception(At_Phrase(*syntax_, f),
            stringify("then and else expressions have mismatched types: ",
                t2," and ",t3));
    }
    throw Exception(cx, stringify(cond, " is not a boolean"));
}
void
If_Else_Op::tail_eval(std::unique_ptr<Frame>& f) const
{
    Value cond = arg1_->eval(*f);
    At_Phrase cx(*arg1_->syntax_, *f);
    if (cond.is_bool()) {
        if (cond.to_bool(cx))
            f->next_op_ = &*arg2_;
        else
            f->next_op_ = &*arg3_;
        return;
    }
    auto re = cond.maybe<Reactive_Value>();
    if (re && re->sctype_ == SC_Type::Bool()) {
        Value a2 = arg2_->eval(*f);
        Value a3 = arg3_->eval(*f);
        SC_Type t2 = sc_type_of(a2);
        SC_Type t3 = sc_type_of(a3);
        if (t2 == t3) {
            f->result_ = Value{make<Reactive_Expression>(
                t2,
                make<If_Else_Op>(
                    share(*syntax_),
                    to_expr(cond, *arg1_->syntax_),
                    to_expr(a2, *arg2_->syntax_),
                    to_expr(a3, *arg3_->syntax_)),
                At_Phrase(*syntax_, *f))};
            f->next_op_ = nullptr;
            return;
        }
        throw Exception(At_Phrase(*syntax_, *f),
            stringify("then and else expressions have mismatched types: ",
                t2," and ",t3));
    }
    throw Exception(cx, stringify(cond, " is not a boolean"));
}
void
If_Else_Op::exec(Frame& f, Executor& ex) const
{
    bool a = arg1_->eval(f).to_bool(At_Phrase(*arg1_->syntax_, f));
    if (a)
        arg2_->exec(f, ex);
    else
        arg3_->exec(f, ex);
}

Value
Equal_Expr::eval(Frame& f) const
{
    Value a = arg1_->eval(f);
    Value b = arg2_->eval(f);
    At_Phrase cx(*syntax_, f);
    return eqval<Equal_Expr>(a.equal(b, cx), a, b, cx);
}
void Equal_Expr::print(std::ostream& out) const
  { out<<"("<<*arg1_<<"=="<<*arg2_<<")"; }
Value
Not_Equal_Expr::eval(Frame& f) const
{
    Value a = arg1_->eval(f);
    Value b = arg2_->eval(f);
    At_Phrase cx(*syntax_, f);
    return eqval<Not_Equal_Expr>(!a.equal(b, cx), a, b, cx);
}
void Not_Equal_Expr::print(std::ostream& out) const
  { out<<"("<<*arg1_<<"!="<<*arg2_<<")"; }

struct Index_State
{
    Shared<const Phrase> callph_;
    At_Phrase cx;
    At_Phrase icx;

    Index_State(
        Shared<const Phrase> callph,
        Frame& f)
    :
        callph_(callph),
        cx(*callph, f),
        icx(*arg_part(callph), f)
    {}

    Shared<const Phrase> ph() { return callph_; }
    Shared<const Phrase> iph() { return arg_part(callph_); }
    Shared<const Phrase> lph() { return func_part(callph_); }

    Shared<const String> err(Value list, Value index,
        const Value* path, const Value* endpath)
    {
        String_Builder msg;
        msg << "indexing error\n";
        msg << "left side: " << list;
        if (auto rx = list.maybe<Reactive_Value>())
            msg << " (type " << rx->sctype_ << ")";
        msg << "\nright side: [" << index;
        while (path < endpath) {
            msg << "," << *path;
            ++path;
        }
        msg << "]";
        return msg.get_string();
    }
};
Value value_at_path(Value, const Value*, const Value*, Index_State&);
Value list_at_path(const List& list, Value index,
    const Value* path, const Value* endpath, Index_State& state)
{
    if (index.is_num()) {
        int i = index.to_int(0, int(list.size()-1), state.icx);
        return value_at_path(list.at(i), path, endpath, state);
    }
    else if (auto indices = index.maybe<List>()) {
        Shared<List> result = List::make(indices->size());
        int j = 0;
        for (auto ival : *indices)
            (*result)[j++] = list_at_path(list, ival, path, endpath, state);
        return {result};
    }
    else if (auto ri = index.maybe<Reactive_Value>()) {
        if (ri->sctype_.is_num()) {
            Value val = {share(list)};
            auto type = sc_type_of(val);
            if (type.is_list()) {
                Shared<List_Expr> index =
                    List_Expr::make({ri->expr()}, state.iph());
                index->init();
                Value rx = {make<Reactive_Expression>(
                    type.elem_type(),
                    make<Call_Expr>(
                        state.ph(),
                        make<Constant>(state.lph(), val),
                        index),
                    state.cx)};
                return value_at_path(rx, path+1, endpath, state);
            }
        }
        /* TODO: add general support for A[[i,j,k]] to SubCurv
        else if (ri->sc_type_.is_num_vec()) {
            ...
        }
        */
    }
    throw Exception(state.cx, state.err({share(list)}, index, path, endpath));
}
Value value_at_path(Value val, const Value* path, const Value* endpath,
    Index_State& state)
{
    if (path == endpath) return val;
    Value index = path[0];
    if (auto list = val.maybe<List>()) {
        return list_at_path(*list, index, path+1, endpath, state);
    }
    else if (auto string = val.maybe<String>()) {
        if (path+1 == endpath) {
            // TODO: this code only works for ASCII strings.
            if (index.is_num()) {
                int i = index.to_int(0, int(string->size()-1), state.icx);
                return {make_string(string->data()+i, 1)};
            }
            else if (auto indices = index.maybe<List>()) {
                String_Builder sb;
                for (auto ival : *indices) {
                    int i = ival.to_int(0, int(string->size()-1), state.icx);
                    sb << string->at(i);
                }
                return {sb.get_string()};
            }
            // reactive index not supported because String is not in SubCurv
        }
    }
    else if (auto rx = val.maybe<Reactive_Value>()) {
        // TODO: what to do for pathsize > 1? punt for now.
        if (path+1 == endpath && is_num(index)) {
            auto iph = state.iph();
            auto lph = state.lph();
            Shared<List_Expr> ix = List_Expr::make({to_expr(index,*iph)}, iph);
            ix->init();
            return {make<Reactive_Expression>(
                rx->sctype_.elem_type(),
                make<Call_Expr>(state.ph(), to_expr(val,*lph), ix),
                state.cx)};
        }
    }
    throw Exception(state.cx, state.err(val, index, path+1, endpath));
}
Value value_at(Value list, Value index, Shared<const Phrase> callph, Frame& f)
{
    Index_State state(callph, f);
    // TODO: support reactive index
    auto path = index.to<List>(state.icx);
    return value_at_path(list, path->begin(), path->end(), state);
}

Value
call_func(Value func, Value arg, Shared<const Phrase> call_phrase, Frame& f)
{
    static Symbol_Ref callkey = make_symbol("call");
    static Symbol_Ref conskey = make_symbol("constructor");
    Value funv = func;
    for (;;) {
        if (!funv.is_ref())
            throw Exception(At_Phrase(*func_part(call_phrase), f),
                stringify(funv,": not a function"));
        Ref_Value& funp( funv.to_ref_unsafe() );
        switch (funp.type_) {
        case Ref_Value::ty_function:
          {
            Function* fun = (Function*)&funp;
            std::unique_ptr<Frame> f2 {
                Frame::make(fun->nslots_, f.system_, &f, call_phrase, nullptr)
            };
            f2->func_ = share(*fun);
            fun->tail_call(arg, f2);
            return tail_eval_frame(std::move(f2));
          }
        case Ref_Value::ty_record:
          {
            Record* s = (Record*)&funp;
            if (s->hasfield(callkey)) {
                funv = s->getfield(callkey, At_Phrase(*call_phrase, f));
                continue;
            }
            if (s->hasfield(conskey)) {
                funv = s->getfield(conskey, At_Phrase(*call_phrase, f));
                continue;
            }
            break;
          }
        case Ref_Value::ty_string:
        case Ref_Value::ty_list:
        case Ref_Value::ty_reactive:
          {
            return value_at(funv, arg, call_phrase, f);
          }
        }
        throw Exception(At_Phrase(*func_part(call_phrase), f),
            stringify(func,": not a function"));
    }
}
void
tail_call_func(
    Value func, Value arg,
    Shared<const Phrase> call_phrase, std::unique_ptr<Frame>& f)
{
    static Symbol_Ref callkey = make_symbol("call");
    static Symbol_Ref conskey = make_symbol("constructor");
    Value funv = func;
    for (;;) {
        if (!funv.is_ref())
            throw Exception(At_Phrase(*func_part(call_phrase), *f),
                stringify(funv,": not a function"));
        Ref_Value& funp( funv.to_ref_unsafe() );
        switch (funp.type_) {
        case Ref_Value::ty_function:
          {
            Function* fun = (Function*)&funp;
            f = Frame::make(
                fun->nslots_, f->system_, f->parent_frame_,
                call_phrase, nullptr);
            f->func_ = share(*fun);
            fun->tail_call(arg, f);
            return;
          }
        case Ref_Value::ty_record:
          {
            Record* s = (Record*)&funp;
            if (s->hasfield(callkey)) {
                funv = s->getfield(callkey, At_Phrase(*call_phrase, *f));
                continue;
            }
            if (s->hasfield(conskey)) {
                funv = s->getfield(conskey, At_Phrase(*call_phrase, *f));
                continue;
            }
            break;
          }
        case Ref_Value::ty_string:
        case Ref_Value::ty_list:
        case Ref_Value::ty_reactive:
          {
            f->result_ = value_at(funv, arg, call_phrase, *f);
            f->next_op_ = nullptr;
            return;
          }
        }
        throw Exception(At_Phrase(*func_part(call_phrase), *f),
            stringify(func,": not a function"));
    }
}
Value
Call_Expr::eval(Frame& f) const
{
    return call_func(func_->eval(f), arg_->eval(f), syntax_, f);
}
void
Call_Expr::tail_eval(std::unique_ptr<Frame>& f) const
{
    tail_call_func(func_->eval(*f), arg_->eval(*f), syntax_, f);
}

Shared<List>
List_Expr_Base::eval_list(Frame& f) const
{
    // TODO: if the # of elements generated is known at compile time,
    // then the List could be constructed directly without using a std::vector.
    List_Builder lb;
    List_Executor lex(lb);
    for (size_t i = 0; i < this->size(); ++i)
        (*this)[i]->exec(f, lex);
    return lb.get_list();
}

Value
List_Expr_Base::eval(Frame& f) const
{
    return {eval_list(f)};
}

void
Spread_Op::exec(Frame& f, Executor& ex) const
{
    At_Phrase cstmt(*syntax_, f);
    At_Phrase carg(*arg_->syntax_, f);
    auto arg = arg_->eval(f);
    if (auto list = arg.maybe<const List>()) {
        for (size_t i = 0; i < list->size(); ++i)
            ex.push_value(list->at(i), cstmt);
        return;
    }
    if (auto rec = arg.maybe<const Record>()) {
        for (auto i = rec->iter(); !i->empty(); i->next())
            ex.push_field(i->key(), i->value(carg), cstmt);
        return;
    }
    throw Exception(carg, stringify(arg, " is not a list or record"));
}

void
Assoc::exec(Frame& f, Executor& ex) const
{
    ex.push_field(name_.eval(f), definiens_->eval(f), At_Phrase(*syntax_, f));
}

Value
Record_Expr::eval(Frame& f) const
{
    auto record = make<DRecord>();
    Record_Executor rex(*record);
    for (auto op : fields_)
        op->exec(f, rex);
    return {record};
}

Shared<Module>
Scope_Executable::eval_module(Frame& f) const
{
    assert(module_slot_ != (slot_t)(-1));
    assert(module_dictionary_ != nullptr);

    Shared<Module> module =
        Module::make(module_dictionary_->size(), module_dictionary_);
    f[module_slot_] = {module};
    Operation::Action_Executor aex;
    for (auto action : actions_)
        action->exec(f, aex);
    return module;
}
void
Scope_Executable::exec(Frame& f) const
{
    if (module_slot_ != (slot_t)(-1)) {
        (void) eval_module(f);
    } else {
        Operation::Action_Executor aex;
        for (auto action : actions_) {
            action->exec(f, aex);
        }
    }
}

void
Boxed_Locative::store(Frame& f, const Operation& expr) const
{
    *reference(f,false) = expr.eval(f);
}

Shared<Locative>
Boxed_Locative::get_field(
    Environ& env,
    Shared<const Phrase> syntax,
    Symbol_Expr selector)
{
    return make<Dot_Locative>(syntax, share(*this), selector);
}

Shared<Locative>
Boxed_Locative::get_element(
    Environ& env,
    Shared<const Phrase> syntax,
    Shared<Operation> index)
{
    return make<Indexed_Locative>(syntax, share(*this), index);
}

Value*
Local_Locative::reference(Frame& f,bool) const
{
    return &f[slot_];
}

Value*
Dot_Locative::reference(Frame& f, bool need_value) const
{
    Value* base = base_->reference(f,true);
    Shared<Record> base_rec = base->to<Record>(At_Phrase(*base_->syntax_, f));
    if (base_rec->use_count > 1) {
        base_rec = base_rec->clone();
        *base = {base_rec};
    }
    Symbol_Ref id = selector_.eval(f);
    return base_rec->ref_field(id, need_value, At_Phrase(*syntax_, f));
}

Value*
Indexed_Locative::reference(Frame& f, bool need_value) const
{
    Value* base = base_->reference(f,true);
    Shared<List> base_list = base->to<List>(At_Phrase(*base_->syntax_, f));
    if (base_list->use_count > 1) {
        base_list = base_list->clone();
        *base = {base_list};
    }
    auto ix = index_->eval(f);
    return base_list->ref_element(ix, need_value, At_Phrase(*syntax_, f));
}

void
Assignment_Action::exec(Frame& f, Executor&) const
{
    locative_->store(f, *expr_);
}

Value
Module_Expr::eval(Frame& f) const
{
    auto module = eval_module(f);
    return {module};
}

Shared<Module>
Enum_Module_Expr::eval_module(Frame& f) const
{
    Shared<Module> module = Module::make(exprs_.size(), dictionary_);
    for (size_t i = 0; i < exprs_.size(); ++i)
        module->at(i) = exprs_[i]->eval(f);
    return module;
}

Shared<Module>
Scoped_Module_Expr::eval_module(Frame& f) const
{
    return executable_.eval_module(f);
}

Value
Block_Op::eval(Frame& f) const
{
    statements_.exec(f);
    return body_->eval(f);
}
void
Block_Op::tail_eval(std::unique_ptr<Frame>& f) const
{
    statements_.exec(*f);
    body_->tail_eval(f);
}
void
Block_Op::exec(Frame& f, Executor& ex) const
{
    statements_.exec(f);
    body_->exec(f, ex);
}

Value
Do_Expr::eval(Frame& f) const
{
    Action_Executor aex;
    actions_->exec(f, aex);
    return body_->eval(f);
}
void
Do_Expr::tail_eval(std::unique_ptr<Frame>& f) const
{
    Action_Executor aex;
    actions_->exec(*f, aex);
    body_->tail_eval(f);
}

void
Compound_Op_Base::exec(Frame& f, Executor& ex) const
{
    for (auto s : *this)
        s->exec(f, ex);
}

void
While_Op::exec(Frame& f, Executor& ex) const
{
    for (;;) {
        Value c = cond_->eval(f);
        bool b = c.to_bool(At_Phrase{*cond_->syntax_, f});
        if (!b) return;
        body_->exec(f, ex);
    }
}

void
For_Op::exec(Frame& f, Executor& ex) const
{
    At_Phrase cx{*list_->syntax_, f};
    At_Index icx{0, cx};
    auto list = list_->eval(f).to<List>(cx);
    for (size_t i = 0; i < list->size(); ++i) {
        icx.index_ = i;
        // TODO: For_Op::exec: can't use icx in pattern_->exec(), not At_Syntax
        pattern_->exec(f.array_, list->at(i), cx, f);
        if (cond_ && !cond_->eval(f).to_bool(At_Phrase{*cond_->syntax_,f}))
            break;
        body_->exec(f, ex);
    }
}

Value
Range_Expr::eval(Frame& f) const
{
    Value firstv = arg1_->eval(f);
    double first = firstv.to_num_or_nan();

    Value lastv = arg2_->eval(f);
    double last = lastv.to_num_or_nan();

    Value stepv;
    double step = 1.0;
    if (arg3_) {
        stepv = arg3_->eval(f);
        step = stepv.to_num_or_nan();
    }

    double delta = round((last - first)/step);
    double countd = delta < 0.0 ? 0.0 : delta + (half_open_ ? 0.0 : 1.0);
    // Note: countd could be infinity. It could be too large to fit in an
    // integer. It could be a float integer too large to increment (for large
    // float i, i==i+1). So we impose a limit on the count.
    if (countd < 1'000'000'000.0) {
        List_Builder lb;
        unsigned count = (unsigned) countd;
        for (unsigned i = 0; i < count; ++i)
            lb.push_back(Value{first + step*i});
        return {lb.get_list()};
    }

    // Fast path failed (assuming Num arguments).
    // Next check for the reactive case.
    if (first==first && last==last && step==step) {
        // all arguments are Num, therefore not a reactive expression
    }
    else if (is_num(firstv) && is_num(lastv)
             && (stepv.is_missing() || is_num(stepv)))
    {
        // For now, this is almost useless: it's triggered when
        // a reactive range value is a parameter to a shape constructor.
        return {make<Reactive_Expression>(
            SC_Type::Error(), // TODO: should be 'Array 1 Num'
            make<Range_Expr>(
                share(*syntax_),
                to_expr(firstv, *arg1_->syntax_),
                to_expr(lastv, *arg2_->syntax_),
                !arg3_ ? nullptr : to_expr(stepv, *arg3_->syntax_),
                half_open_),
            At_Phrase(*syntax_, f))};
    }
    
    // Report error.
    const char* err =
        (countd == countd ? "too many elements in range" : "domain error");
    const char* dots = (half_open_ ? " ..< " : " .. ");
    throw Exception(At_Phrase(*syntax_, f),
        arg3_
            ? stringify(firstv,dots,lastv," by ",stepv,": ", err)
            : stringify(firstv,dots,lastv,": ", err));
}

Value
Lambda_Expr::eval(Frame& f) const
{
    auto c = make<Closure>(
        pattern_,
        body_,
        nonlocals_->eval_module(f),
        nslots_);
    c->name_ = name_;
    c->argpos_ = argpos_;
    return Value{c};
}

void
Literal_Segment::generate(Frame&, String_Builder& sb) const
{
    sb << *data_;
}
void
Ident_Segment::generate(Frame& f, String_Builder& sb) const
{
    Value val = expr_->eval(f);
    val.print_string(sb);
}
void
Paren_Segment::generate(Frame& f, String_Builder& sb) const
{
    sb << expr_->eval(f);
}
void
Bracket_Segment::generate(Frame& f, String_Builder& sb) const
{
    At_Phrase cx(*expr_->syntax_, f);
    auto list = expr_->eval(f).to<List>(cx);
    for (size_t i = 0; i < list->size(); ++i)
        sb << (char)(*list)[i].to_int(1, 127, At_Index(i,cx));
}
void
Brace_Segment::generate(Frame& f, String_Builder& sb) const
{
    At_Phrase cx(*expr_->syntax_, f);
    auto list = expr_->eval(f).to<List>(cx);
    for (auto val : *list)
        val.print_string(sb);
}
Value
String_Expr_Base::eval(Frame& f) const
{
    String_Builder sb;
    for (auto seg : *this)
        seg->generate(f, sb);
    return {sb.get_string()};
}
Symbol_Ref
String_Expr_Base::eval_symbol(Frame& f) const
{
    String_Builder sb;
    for (auto seg : *this)
        seg->generate(f, sb);
    return make_symbol(sb.str());
}

void
Data_Setter::exec(Frame& f, Executor&) const
{
    Value* slots;
    if (module_slot_ == (slot_t)(-1))
        slots = &f[0];
    else {
        auto mval = f[module_slot_];
        auto m = (Module*)&mval.to_ref_unsafe();
        assert(m->subtype_ == Ref_Value::sty_module);
        slots = &m->at(0);
    }
    Value value = definiens_->eval(f);
    pattern_->exec(slots, value, At_Phrase(*definiens_->syntax_, f), f);
}

void
Function_Setter_Base::exec(Frame& f, Executor&) const
{
    Value* slots;
    if (module_slot_ == (slot_t)(-1))
        slots = &f[0];
    else {
        auto mval = f[module_slot_];
        auto m = (Module*)&mval.to_ref_unsafe();
        assert(m->subtype_ == Ref_Value::sty_module);
        slots = &m->at(0);
    }
    Shared<Module> nonlocals = nonlocals_->eval_module(f);
    for (auto& e : *this)
        slots[e.slot_] = {make<Closure>(*e.lambda_, *nonlocals)};
}

void
Include_Setter_Base::exec(Frame& f, Executor&) const
{
    Value* slots;
    if (module_slot_ == (slot_t)(-1))
        slots = &f[0];
    else {
        auto mval = f[module_slot_];
        auto m = (Module*)&mval.to_ref_unsafe();
        assert(m->subtype_ == Ref_Value::sty_module);
        slots = &m->at(0);
    }
    for (auto& e : *this)
        slots[e.slot_] = e.value_;
}

// `val :: pred` is a predicate assertion.
// It aborts if `pred val` is false, returns val if `pred val` is true.
Value
Predicate_Assertion_Expr::eval(Frame& f) const
{
    Value val = arg1_->eval(f);
    Value pred = arg2_->eval(f);
    bool r =
        call_func(pred, val, syntax_, f).to_bool(At_Phrase(*syntax_, f));
    if (r) return val;
    throw Exception(At_Phrase(*syntax_, f), "predicate assertion failed");
}

Value
Parametric_Expr::eval(Frame& f) const
{
    At_Phrase cx(*syntax_, f);
    Value func = ctor_->eval(f);
    auto closure = func.maybe<Closure>();
    if (closure == nullptr)
        throw Exception(cx, "internal error in Parametric_Expr");
    Shared<const Phrase> call_phrase = syntax_; // TODO?
    std::unique_ptr<Frame> f2 {
        Frame::make(closure->nslots_, f.system_, &f, call_phrase, nullptr)
    };
    auto default_arg = record_pattern_default_value(*closure->pattern_,*f2);
    Value res = closure->call({default_arg}, *f2);
    auto rec = res.to<Record>(cx);
    auto drec = make<DRecord>();
    rec->each_field(cx, [&](Symbol_Ref id, Value val) -> void {
        drec->fields_[id] = val;
    });
    // TODO: The `constructor` function should return another parametric record.
    drec->fields_[make_symbol("constructor")] = func;
    drec->fields_[make_symbol("argument")] = {default_arg};
    return {drec};
}

} // namespace curv
