/*
 * Copyright (c) 1998, 2023, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "ci/ciSymbols.hpp"
#include "compiler/compileLog.hpp"
#include "oops/objArrayKlass.hpp"
#include "opto/addnode.hpp"
#include "opto/memnode.hpp"
#include "opto/mulnode.hpp"
#include "opto/parse.hpp"
#include "opto/rootnode.hpp"
#include "opto/runtime.hpp"
#include "runtime/sharedRuntime.hpp"

#ifndef PRODUCT
unsigned peaNumAllocsTracked = 0;
unsigned peaNumMaterializations = 0;

void printPeaStatistics() {
  tty->print("PEA: ");
  tty->print("num allocations tracked = %u, ", peaNumAllocsTracked);
  tty->print_cr("num materializations = %u", peaNumMaterializations);
}
#endif

//------------------------------make_dtrace_method_entry_exit ----------------
// Dtrace -- record entry or exit of a method if compiled with dtrace support
void GraphKit::make_dtrace_method_entry_exit(ciMethod* method, bool is_entry) {
  const TypeFunc *call_type    = OptoRuntime::dtrace_method_entry_exit_Type();
  address         call_address = is_entry ? CAST_FROM_FN_PTR(address, SharedRuntime::dtrace_method_entry) :
                                            CAST_FROM_FN_PTR(address, SharedRuntime::dtrace_method_exit);
  const char     *call_name    = is_entry ? "dtrace_method_entry" : "dtrace_method_exit";

  // Get base of thread-local storage area
  Node* thread = _gvn.transform( new ThreadLocalNode() );

  // Get method
  const TypePtr* method_type = TypeMetadataPtr::make(method);
  Node *method_node = _gvn.transform(ConNode::make(method_type));

  kill_dead_locals();

  // For some reason, this call reads only raw memory.
  const TypePtr* raw_adr_type = TypeRawPtr::BOTTOM;
  make_runtime_call(RC_LEAF | RC_NARROW_MEM,
                    call_type, call_address,
                    call_name, raw_adr_type,
                    thread, method_node);
}


//=============================================================================
//------------------------------do_checkcast-----------------------------------
void Parse::do_checkcast() {
  bool will_link;
  ciKlass* klass = iter().get_klass(will_link);

  Node *obj = peek();

  // Throw uncommon trap if class is not loaded or the value we are casting
  // _from_ is not loaded, and value is not null.  If the value _is_ null,
  // then the checkcast does nothing.
  const TypeOopPtr *tp = _gvn.type(obj)->isa_oopptr();
  if (!will_link || (tp && !tp->is_loaded())) {
    if (C->log() != nullptr) {
      if (!will_link) {
        C->log()->elem("assert_null reason='checkcast' klass='%d'",
                       C->log()->identify(klass));
      }
      if (tp && !tp->is_loaded()) {
        // %%% Cannot happen?
        ciKlass* klass = tp->unloaded_klass();
        C->log()->elem("assert_null reason='checkcast source' klass='%d'",
                       C->log()->identify(klass));
      }
    }
    null_assert(obj);
    assert( stopped() || _gvn.type(peek())->higher_equal(TypePtr::NULL_PTR), "what's left behind is null" );
    return;
  }

  Node* res = gen_checkcast(obj, makecon(TypeKlassPtr::make(klass, Type::trust_interfaces)));
  if (stopped()) {
    return;
  }

  // Pop from stack AFTER gen_checkcast because it can uncommon trap and
  // the debug info has to be correct.
  pop();
  push(res);
}


//------------------------------do_instanceof----------------------------------
void Parse::do_instanceof() {
  if (stopped())  return;
  // We would like to return false if class is not loaded, emitting a
  // dependency, but Java requires instanceof to load its operand.

  // Throw uncommon trap if class is not loaded
  bool will_link;
  ciKlass* klass = iter().get_klass(will_link);

  if (!will_link) {
    if (C->log() != nullptr) {
      C->log()->elem("assert_null reason='instanceof' klass='%d'",
                     C->log()->identify(klass));
    }
    null_assert(peek());
    assert( stopped() || _gvn.type(peek())->higher_equal(TypePtr::NULL_PTR), "what's left behind is null" );
    if (!stopped()) {
      // The object is now known to be null.
      // Shortcut the effect of gen_instanceof and return "false" directly.
      pop();                   // pop the null
      push(_gvn.intcon(0));    // push false answer
    }
    return;
  }

  // Push the bool result back on stack
  Node* res = gen_instanceof(peek(), makecon(TypeKlassPtr::make(klass, Type::trust_interfaces)), true);

  // Pop from stack AFTER gen_instanceof because it can uncommon trap.
  pop();
  push(res);
}

//------------------------------array_store_check------------------------------
// pull array from stack and check that the store is valid
void Parse::array_store_check() {

  // Shorthand access to array store elements without popping them.
  Node *obj = peek(0);
  Node *idx = peek(1);
  Node *ary = peek(2);

  if (_gvn.type(obj) == TypePtr::NULL_PTR) {
    // There's never a type check on null values.
    // This cutout lets us avoid the uncommon_trap(Reason_array_check)
    // below, which turns into a performance liability if the
    // gen_checkcast folds up completely.
    return;
  }

  // Extract the array klass type
  int klass_offset = oopDesc::klass_offset_in_bytes();
  Node* p = basic_plus_adr( ary, ary, klass_offset );
  // p's type is array-of-OOPS plus klass_offset
  Node* array_klass = _gvn.transform(LoadKlassNode::make(_gvn, nullptr, immutable_memory(), p, TypeInstPtr::KLASS));
  // Get the array klass
  const TypeKlassPtr *tak = _gvn.type(array_klass)->is_klassptr();

  // The type of array_klass is usually INexact array-of-oop.  Heroically
  // cast array_klass to EXACT array and uncommon-trap if the cast fails.
  // Make constant out of the inexact array klass, but use it only if the cast
  // succeeds.
  bool always_see_exact_class = false;
  if (MonomorphicArrayCheck
      && !too_many_traps(Deoptimization::Reason_array_check)
      && !tak->klass_is_exact()
      && tak != TypeInstKlassPtr::OBJECT) {
      // Regarding the fourth condition in the if-statement from above:
      //
      // If the compiler has determined that the type of array 'ary' (represented
      // by 'array_klass') is java/lang/Object, the compiler must not assume that
      // the array 'ary' is monomorphic.
      //
      // If 'ary' were of type java/lang/Object, this arraystore would have to fail,
      // because it is not possible to perform a arraystore into an object that is not
      // a "proper" array.
      //
      // Therefore, let's obtain at runtime the type of 'ary' and check if we can still
      // successfully perform the store.
      //
      // The implementation reasons for the condition are the following:
      //
      // java/lang/Object is the superclass of all arrays, but it is represented by the VM
      // as an InstanceKlass. The checks generated by gen_checkcast() (see below) expect
      // 'array_klass' to be ObjArrayKlass, which can result in invalid memory accesses.
      //
      // See issue JDK-8057622 for details.

    always_see_exact_class = true;
    // (If no MDO at all, hope for the best, until a trap actually occurs.)

    // Make a constant out of the inexact array klass
    const TypeKlassPtr *extak = tak->cast_to_exactness(true);

    if (extak->exact_klass(true) != nullptr) {
      Node* con = makecon(extak);
      Node* cmp = _gvn.transform(new CmpPNode( array_klass, con ));
      Node* bol = _gvn.transform(new BoolNode( cmp, BoolTest::eq ));
      Node* ctrl= control();
      { BuildCutout unless(this, bol, PROB_MAX);
        uncommon_trap(Deoptimization::Reason_array_check,
                      Deoptimization::Action_maybe_recompile,
                      extak->exact_klass());
      }
      if (stopped()) {          // MUST uncommon-trap?
        set_control(ctrl);      // Then Don't Do It, just fall into the normal checking
      } else {                  // Cast array klass to exactness:
        // Use the exact constant value we know it is.
        replace_in_map(array_klass,con);
        CompileLog* log = C->log();
        if (log != nullptr) {
          log->elem("cast_up reason='monomorphic_array' from='%d' to='(exact)'",
                    log->identify(extak->exact_klass()));
        }
        array_klass = con;      // Use cast value moving forward
      }
    }
  }

  // Come here for polymorphic array klasses

  // Extract the array element class
  int element_klass_offset = in_bytes(ObjArrayKlass::element_klass_offset());
  Node *p2 = basic_plus_adr(array_klass, array_klass, element_klass_offset);
  // We are allowed to use the constant type only if cast succeeded. If always_see_exact_class is true,
  // we must set a control edge from the IfTrue node created by the uncommon_trap above to the
  // LoadKlassNode.
  Node* a_e_klass = _gvn.transform(LoadKlassNode::make(_gvn, always_see_exact_class ? control() : nullptr,
                                                       immutable_memory(), p2, tak));

  // Check (the hard way) and throw if not a subklass.
  // Result is ignored, we just need the CFG effects.
  gen_checkcast(obj, a_e_klass);
}


//------------------------------do_new-----------------------------------------
void Parse::do_new() {
  kill_dead_locals();

  bool will_link;
  ciInstanceKlass* klass = iter().get_klass(will_link)->as_instance_klass();
  assert(will_link, "_new: typeflow responsibility");

  // Should throw an InstantiationError?
  if (klass->is_abstract() || klass->is_interface() ||
      klass->name() == ciSymbols::java_lang_Class() ||
      iter().is_unresolved_klass()) {
    uncommon_trap(Deoptimization::Reason_unhandled,
                  Deoptimization::Action_none,
                  klass);
    return;
  }

  if (C->needs_clinit_barrier(klass, method())) {
    clinit_barrier(klass, method());
    if (stopped())  return;
  }

  Node* kls = makecon(TypeKlassPtr::make(klass));
  Node* obj = new_instance(kls);

  // Push resultant oop onto stack
  push(obj);

  // Keep track of whether opportunities exist for StringBuilder
  // optimizations.
  if (OptimizeStringConcat &&
      (klass == C->env()->StringBuilder_klass() ||
       klass == C->env()->StringBuffer_klass())) {
    C->set_has_stringbuilder(true);
  }

  // Keep track of boxed values for EliminateAutoBox optimizations.
  if (C->eliminate_boxing() && klass->is_box_klass()) {
    C->set_has_boxed_value(true);
  }

  if (DoPartialEscapeAnalysis) {
    // obj is a CheckCastPP Node, aka. cooked oop.
    jvms()->alloc_state().add_new_allocation(this, obj);
  }
}

#ifndef PRODUCT
//------------------------------dump_map_adr_mem-------------------------------
// Debug dump of the mapping from address types to MergeMemNode indices.
void Parse::dump_map_adr_mem() const {
  tty->print_cr("--- Mapping from address types to memory Nodes ---");
  MergeMemNode *mem = map() == nullptr ? nullptr : (map()->memory()->is_MergeMem() ?
                                      map()->memory()->as_MergeMem() : nullptr);
  for (uint i = 0; i < (uint)C->num_alias_types(); i++) {
    C->alias_type(i)->print_on(tty);
    tty->print("\t");
    // Node mapping, if any
    if (mem && i < mem->req() && mem->in(i) && mem->in(i) != mem->empty_memory()) {
      mem->in(i)->dump();
    } else {
      tty->cr();
    }
  }
}

#endif

#include "ci/ciUtilities.inline.hpp"
#include "compiler/methodMatcher.hpp"

class PEAContext {
private:
  BasicMatcher* _matcher;

  PEAContext() {
    if (PEAMethodOnly != nullptr) {
      const char* error_msg = nullptr;
      _matcher = BasicMatcher::parse_method_pattern((char*)PEAMethodOnly, error_msg, false);
      if (error_msg != nullptr) {
        tty->print_cr("Invalid PEAMethodOnly: %s", error_msg);
      }
    }
  }

  NONCOPYABLE(PEAContext);
public:
  bool match(ciMethod* method) const;
  // mayer's singleton.
  static PEAContext& instance() {
    static PEAContext s;
    return s;
  }
};

//
// Partial Escape Analysis
// Stadler, Lukas, Thomas Würthinger, and Hanspeter Mössenböck. "Partial escape analysis and scalar replacement for Java."
//
// Our adaption to C2.
// https://gist.github.com/navyxliu/62a510a5c6b0245164569745d758935b
//

VirtualState::VirtualState(const TypeOopPtr* oop_type): _oop_type(oop_type), _lockcnt(0) {
  Compile* C = Compile::current();
  int nof = nfields();
  _entries = NEW_ARENA_ARRAY(C->parser_arena(), Node*, nof);
  // only track explicit stores.
  // see IntializeNode semantics in memnode.cpp
  for (int i = 0; i < nof; ++i) {
    _entries[i] = nullptr;
  }
}

// do NOT call base's copy constructor. we would like to reset refcnt!
VirtualState::VirtualState(const VirtualState& other) : _oop_type(other._oop_type), _lockcnt(other._lockcnt) {
  int nof = nfields();
  _entries = NEW_ARENA_ARRAY(Compile::current()->parser_arena(), Node*, nof);

  // Using arraycopy stub is more efficient?
  Node** dst = _entries;
  Node** src = other._entries;
  while (nof-- > 0) {
    *dst++ = *src++;
  }
}

int VirtualState::nfields() const {
  ciInstanceKlass* holder = _oop_type->is_instptr()->instance_klass();
  return holder->nof_nonstatic_fields();
}

void VirtualState::set_field(ciField* field, Node* val) {
  // We can't trust field->holder() here. It may reference to the super class.
  // field layouter may flip order in jdk15+, refer to:
  // https://shipilev.net/jvm/objects-inside-out/#_superhierarchy_gaps_in_java_15
  //
  // _oop_type is the exact type when we registered ObjID in allocation state.
  //
  ciInstanceKlass* holder = _oop_type->is_instptr()->instance_klass();

  for (int i = 0; i < holder->nof_nonstatic_fields(); ++i) {
    if (field->offset_in_bytes() == holder->nonstatic_field_at(i)->offset_in_bytes()) {
      _entries[i] = val;
      return;
    }
  }

  ShouldNotReachHere();
}

Node* VirtualState::get_field(ciField* field) const {
  ciInstanceKlass* holder = _oop_type->is_instptr()->instance_klass();

  for (int i = 0; i < holder->nof_nonstatic_fields(); ++i) {
    if (field->offset_in_bytes() == holder->nonstatic_field_at(i)->offset_in_bytes()) {
      return _entries[i];
    }
  }

  ShouldNotReachHere();
  return nullptr;
}

static void ensure_phi(PhiNode* phi, uint pnum) {
    while (phi->req() <= pnum) {
      phi->add_req(nullptr);
    }
}

static const Type* initialize_null_field(GraphKit* kit, ciField* field, Node*& val) {
  assert(val == nullptr, "must been a null field");
  const Type* type;
  BasicType bt = field->layout_type();

  if (bt == T_OBJECT && field->type()->is_instance_klass()) {
    val = kit->gvn().makecon(TypePtr::NULL_PTR);
    type = TypeInstPtr::make(TypePtr::BotPTR, field->type()->as_instance_klass());
  } else {
    val = kit->zerocon(bt);
    type = Type::get_const_basic_type(bt);
  }

  return type;
}

ObjectState& VirtualState::merge(ObjectState* newin, GraphKit* kit, RegionNode* r, int pnum) {
  assert(newin->is_virtual(), "only support VirtualState");

  if (this != newin) {
    VirtualState* vs = static_cast<VirtualState*>(newin);
    ciInstanceKlass* ik = _oop_type->is_instptr()->instance_klass();
    assert(nfields() == ik->nof_nonstatic_fields(), "_nfields should be consistent with instanceKlass");

    for (int i = 0; i < nfields(); ++i) {
      Node* m = _entries[i];

      if (m != vs->_entries[i]) {
        ciField* field = ik->nonstatic_field_at(i);
        Node* n = vs->_entries[i];
        const Type* tn;
        if (n == nullptr) {
          tn = initialize_null_field(kit, field, n);
        } else {
          tn = kit->gvn().type(n);
        }

        if (m == nullptr || !m->is_Phi() || m->in(0) != r) {
          const Type* type;

          if (m == nullptr) {
            type = initialize_null_field(kit, field, m);
          } else {
            type = kit->gvn().type(m);
          }
          type = type->meet(tn);

          m = PhiNode::make(r, m, type);
          kit->gvn().set_type(m, type);
          _entries[i] = m;
        }

        ensure_phi(m->as_Phi(), pnum);
        m->set_req(pnum, n);
      }
    }

    // Because we may have identical fields, value number phi nodes in the 2nd pass.
    if (pnum == 1) {
      for (int i = 0; i < nfields(); ++i)  {
        Node* m = _entries[i];

        if (m != nullptr && m->is_Phi() && m->in(0) == r) {
          _entries[i] = kit->gvn().transform(m);
        }
      }
    }
  }

  return *this;
}

#ifndef PRODUCT
void VirtualState::print_on(outputStream* os) const {
  os->print_cr("Virt = %p", this);

  for (int i = 0; i < nfields(); ++i) {
    Node* val = _entries[i];
    os->print("#%d: ", i);
    if (val != nullptr) {
      val->dump();
    } else {
      os->print_cr("_");
    }
  }
}

void EscapedState::print_on(outputStream* os) const {
  os->print_cr("Escaped = %p %d", this, _materialized);
  if (_merged_value == nullptr) {
    os->print_cr(" null");
  } else {
    _merged_value->dump();
  }
}

#endif

void PEAState::add_new_allocation(GraphKit* kit, Node* obj) {
  PartialEscapeAnalysis* pea = kit->PEA();
  int nfields;
  const TypeOopPtr* oop_type = obj->as_Type()->type()->is_oopptr();

  if (oop_type->isa_aryptr()) {
    const TypeAryPtr* ary_type = oop_type->is_aryptr();
    const TypeInt* size = ary_type->size();
    if (size->is_con() && size->get_con() <= EliminateAllocationArraySizeLimit) {
      nfields = size->get_con();
    } else {
      // length of array is too long or unknown
      return;
    }
  } else {
    const TypeInstPtr* inst_type = oop_type->is_instptr();
    nfields = inst_type->instance_klass()->nof_nonstatic_fields();
  }

  if (nfields >= 0) {
    AllocateNode* alloc = obj->in(1)->in(0)->as_Allocate();
    int idx = pea->add_object(alloc);
#ifndef PRODUCT
    // node_idx_t is unsigned. Use static_cast<> here to avoid comparison between signed and unsigned.
    if (PEA_debug_idx > 0 && alloc->_idx != static_cast<node_idx_t>(PEA_debug_idx)) {         // only allow PEA_debug_idx
      return;
    } else if (PEA_debug_idx < 0 && alloc->_idx == static_cast<node_idx_t>(-PEA_debug_idx)) { // block PEA_debug_idx
      return;
    }
    Atomic::inc(&peaNumAllocsTracked);
#endif
    // Opt out all subclasses of Throwable because C2 will not inline all methods of them including <init>.
    // PEA needs to materialize it at <init>.
    ciInstanceKlass* ik = oop_type->is_instptr()->instance_klass();
    ciEnv* env = ciEnv::current();
    if (ik->is_subclass_of(env->Throwable_klass())) {
      return;
    }
    // Opt out of all subclasses that non-partial escape analysis opts out of. Opt out of StringBuffer/Builder and
    // defer those objects to StringOpts.
    if (ik->is_subclass_of(env->Thread_klass()) ||
        ik->is_subclass_of(env->Reference_klass()) ||
        ik->is_subclass_of(env->StringBuffer_klass()) ||
        ik->is_subclass_of(env->StringBuilder_klass()) ||
        !ik->can_be_instantiated() || ik->has_finalizer()) {
      return;
    }
    if (idx < PEA_debug_start || idx >= PEA_debug_stop) {
      return;
    }

    ciMethod* method = kit->jvms()->method();
    if (PEAContext::instance().match(method)) {
#ifndef PRODUCT
      if (PEAVerbose) {
        if (method != nullptr) {
          method->dump_name_as_ascii(tty);
        }
        tty->print_cr(" start tracking %d | obj#%d", idx, alloc->_idx);
        alloc->dump();
      }
#endif
      bool result = _state.put(alloc, new VirtualState(oop_type));
      assert(result, "the key existed in _state");
      pea->add_alias(alloc, obj);
    }
  }
}

PEAState& PEAState::operator=(const PEAState& init) {
  if (this != &init) {
    clear();

    init._state.iterate([&](ObjID key, ObjectState* value) {
      _state.put(key, value->clone());
      return true;
    });
  }

  return *this;
}

void PEAState::put_field(GraphKit* kit, ciField* field, Node* objx, Node* val) {
  Compile* C = kit->C;
  PartialEscapeAnalysis* pea = C->PEA();
  int offset = field->offset_in_bytes();
  Node* adr = kit->basic_plus_adr(objx, objx, offset);
  const TypePtr* adr_type = C->alias_type(field)->adr_type();
  DecoratorSet decorators = IN_HEAP;

  BasicType bt = field->layout_type();
  const Type* type = Type::get_const_basic_type(bt);
  bool is_obj = is_reference_type(bt);

  if (is_obj && pea->is_alias(val)) {
    // recurse if val is virtual
    if (as_virtual(pea, val)) {
      materialize(kit, val);
    }
    EscapedState* es = as_escaped(pea, val);
    assert(es != nullptr, "the object of val is not Escaped");
    val = es->merged_value();
  }
  // Store the value.
  const Type* field_type;
  if (!field->type()->is_loaded()) {
    field_type = TypeInstPtr::BOTTOM;
  } else {
    if (is_obj) {
      field_type = TypeOopPtr::make_from_klass(field->type()->as_klass());
    } else {
      field_type = Type::BOTTOM;
    }
  }
  decorators |= field->is_volatile() ? MO_SEQ_CST : MO_UNORDERED;

#ifndef PRODUCT
  if (PEAVerbose) {
    val->dump();
  }
#endif
  kit->access_store_at(objx, adr, adr_type, val, field_type, bt, decorators);
}

// clone AllocateNode
Node* PEAState::clone_object(GraphKit* kit, ObjID alloc) {
  auto pea = kit->PEA();
  assert(alloc != nullptr && get_object_state(alloc)->is_virtual(), "sanity check");
  VirtualState* virt = static_cast<VirtualState*>(get_object_state(alloc));

#ifndef PRODUCT
  if (PEAVerbose) {
    tty->print_cr("PEA materializes a virtual %d obj%d ", pea->object_idx(alloc), alloc->_idx);
  }
  Atomic::inc(&peaNumMaterializations);
#endif

  Node* objx = kit->materialize_object(alloc, virt->type());
  pea->add_alias(alloc, objx);

#ifndef PRODUCT
  if (PEAVerbose) {
    tty->print("new object: ");
    objx->dump();
  }
#endif
  return objx;
}

// Because relevant objects may form a directed cyclic graph, materialization is a DFS process.
// PEA clones the object and marks escaped in allocation state. PEA then iterates all fields
// and recursively materializes the references which are still aliasing with virtual objects in
// allocation state.
Node* PEAState::materialize(GraphKit* kit, Node* var) {
  PartialEscapeAnalysis* pea = kit->PEA();
  ObjID alloc = pea->is_alias(var);
  VirtualState* virt = static_cast<VirtualState*>(get_object_state(alloc));
  Node* objx = clone_object(kit, alloc);

  // we save VirtualState beforehand.
  escape(alloc, objx, true);

  if (virt->lockcnt() > 0 && GenerateSynchronizationCode) {
    if (PEAVerbose) {
      tty->print_cr("materializing an object with unbalanced monitor");
    }

    int mon_id = 0;
    JVMState* jvms = kit->jvms();
    int cnt = 0;
    // It's possible that the locked moninitor is not in the youngest JVMState,
    // so we have to walk the stacktrace to discover them all.
    //
    // PEA Materialization steals those monitors from the original object. Here is the scheme:
    // 1. unlock the original object.
    // 2. lock the materialized object.
    // 3. backfill the obj for Monitor 'obj|box' pair when Parse translates 'monitor-exit'.
    // 4. split Phi-Unlock in the upcoming monitor_exit (Parse::do_monitor_exit).
    //
    while (jvms != nullptr) {
      for (mon_id = 0;  mon_id < jvms->nof_monitors() && jvms->map()->monitor_obj(jvms, mon_id) != var; ++mon_id);

      if (mon_id < jvms->nof_monitors()) {
        cnt++;
        Node* box = jvms->map()->monitor_box(jvms, mon_id);
        kit->shared_unlock(box, var, true/*preserve_monitor*/); // PEA pops the monitor in Parse::monitor_exit().
        kit->clone_shared_lock(box, objx);
      }
      jvms = jvms->caller();
    }
    assert(cnt == virt->lockcnt(), "steal all locks from var");
  }

  kit->replace_in_map(var, objx);

  auto oop_type = virt->type();
  if (oop_type->isa_instptr()) {
    // virt->_oop_type is an exact non-null pointer. oop_type may not be exact, or BOT
    // We check that they both refer to the same java type.
    assert(virt->_oop_type->is_instptr()->is_same_java_type_as(oop_type), "type of oopptr is inconsistent!");
#ifndef PRODUCT
    if (PEAVerbose) {
      ciInstanceKlass* ik = oop_type->is_instptr()->instance_klass();
      tty->print("ciInstanceKlass: ");
      ik->print_name_on(tty);
      tty->cr();
    }
#endif

    for (auto&& it = virt->field_iterator(); it.has_next(); ++it) {
      ciField* field = it.field();
      Node* val = it.value();

#ifndef PRODUCT
      if (PEAVerbose) {
        tty->print("field: ");
        field->print_name_on(tty);
        tty->cr();
      }
#endif
      // no initial value or is captured by InitializeNode
      if (val == nullptr) continue;

      put_field(kit, field, objx, val);
    }

    // back from DFS, we still need to check again for all virtual states.
    // they may have a field 'var' which has committed to memory via prior putfield. We emit a store with updated objx.
    // Hopefully, two consecutive stores coalesce.
    _state.iterate([&](ObjID obj, ObjectState* os) {
      if (os->is_virtual()) {
        VirtualState* vs = static_cast<VirtualState*>(os);

        for (auto&& i = vs->field_iterator(); i.has_next(); ++i) {
          if (i.value() == var) {
            vs->set_field(i.field(), objx);
            put_field(kit, i.field(), get_java_oop(obj), objx);
          }
        }
      }
      return true;
    });

    // if var is associated with MemBarRelease, copy it for objx
    for (DUIterator_Fast kmax, k = var->fast_outs(kmax); k < kmax; k++) {
      Node* use = var->fast_out(k);

      if (use->Opcode() == Op_MemBarRelease) {
        kit->insert_mem_bar(Op_MemBarRelease, objx);
        break;
      }
    }
  } else {
    assert(false, "array not support yet!");
  }

  return objx;
}

#ifndef PRODUCT
void PEAState::print_on(outputStream* os) const {
  if (size() > 0) {
    os->print_cr("PEAState:");
  }

  _state.iterate([&](ObjID obj, ObjectState* state) {
    bool is_virt = state->is_virtual();
    os->print("Obj#%d(%s) ref = %d\n", obj->_idx, is_virt ? "Virt" : "Mat", state->ref_cnt());

    if (is_virt) {
      VirtualState* vs = static_cast<VirtualState*>(state);
      vs->print_on(os);
    } else {
      EscapedState* es = static_cast<EscapedState*>(state);
      es->print_on(tty);
    }
    return true;
  });
}

#endif

bool safepointContains(SafePointNode* sfpt, Node *oop) {
  for (uint i = TypeFunc::Parms; i < sfpt->req(); ++i) {
    if (oop == sfpt->in(i)) {
      return true;
    }
  }
  return false;
}

void PEAState::mark_all_escaped(PartialEscapeAnalysis* pea, ObjID id, Node* obj) {
  VirtualState* virt = as_virtual(pea, obj);
  escape(id, obj, false);

  for (auto&& it = virt->field_iterator(); it.has_next(); ++it) {
    ciField* field = it.field();
    Node* val = it.value();

    BasicType bt = field->layout_type();
    bool is_obj = is_reference_type(bt);

    ObjID alias = pea->is_alias(val);
    if (is_obj && alias != nullptr) {
      // recurse if val is a virtual object.
      if (get_object_state(alias)->is_virtual()) {
        mark_all_escaped(pea, alias, val);
      }
      assert(as_escaped(pea, val) != nullptr, "the object of val is not Escaped");
    }
  }
}

void PEAState::mark_all_live_objects_escaped(PartialEscapeAnalysis *pea, SafePointNode* sfpt) {
  Unique_Node_List objs;
  int sz = objects(objs);

  for (int i = 0; i < sz; ++i) {
    ObjID id = static_cast<ObjID>(objs.at(i));
    ObjectState* os = get_object_state(id);

    if (os->is_virtual()) {
      Node *oop = get_java_oop(id);
      // We only need to mark objects that are live as escaped.
      if (safepointContains(sfpt, oop)) {
        mark_all_escaped(pea, id, oop);
      }
    }
  }
}

// get the key set from _state. we stop maintaining aliases for the materialized objects.
int PEAState::objects(Unique_Node_List& nodes) const {
  _state.iterate([&](ObjID obj, ObjectState* state) {
                   nodes.push(obj); return true;
                 });
  return nodes.size();
}

// We track '_merged_value' along with control-flow but only return it if _materialized = true;
// GraphKit::backfill_materialized() replaces the original CheckCastPP with it at do_exits() or at safepoints.
// If materialization doesn't take place, replacement shouldn't happen either.
//
// @return: nullptr if id has not been materialized, or the SSA java_oop that denotes the original object.
Node* PEAState::get_materialized_value(ObjID id) const {
  assert(contains(id), "must exists in allocation");
  ObjectState* os = get_object_state(id);

  if (os->is_virtual()) {
    return nullptr;
  } else {
    return static_cast<EscapedState*>(os)->materialized_value();
  }
}

Node* PEAState::get_java_oop(ObjID id) const {
  if (!contains(id)) return nullptr;

  Node* obj = get_materialized_value(id);
  if (obj != nullptr) {
    return obj;
  }

  ProjNode* resproj = id->proj_out_or_null(TypeFunc::Parms);
  if (resproj != nullptr) {
    for (DUIterator_Fast imax, i = resproj->fast_outs(imax); i < imax; i++) {
      Node* p = resproj->fast_out(i);
      if (p->is_CheckCastPP()) {
        assert(obj == nullptr, "multiple CheckCastPP?");
        obj = p;
      }
    }
  }
  assert(obj == nullptr || AllocateNode::Ideal_allocation(obj) == id, "sanity check");
  return obj;
}

static SafePointNode* attached_sfpt(RegionNode* shim) {
  SafePointNode* sfpt = nullptr;
  for (DUIterator_Fast imax, i = shim->fast_outs(imax); i < imax; i++) {
    Node* node = shim->fast_out(i);
    if (node->is_SafePoint() && node->outcnt() == 0) {
        assert(sfpt == nullptr || sfpt == node, "sanity");
        sfpt = node->as_SafePoint();
    }
  }
  return sfpt;
}

static bool matched(SafePointNode* sfpt, ObjID id, VirtualState* vs) {
  PEAState& as = sfpt->jvms()->alloc_state();
  return as.contains(id) && as.equals(id, vs);
}

// update the internal phi node in virtual objects.
void PEAState::update_internal_phi_nodes(GraphKit* kit, Unique_Node_List& visited, ObjID key, VirtualState* vs, RegionNode* region, RegionNode* shim, const GrowableArray<uint>& paths) {
  if (visited.member(key)) return;
  visited.push(key);

  auto pea = kit->PEA();
  for (auto&& it = vs->field_iterator(); it.has_next(); ++it) {
      ciField* field = it.field();
      Node* val = it.value();
      if (val == nullptr) continue;

      BasicType bt = field->layout_type();
      bool is_obj = is_reference_type(bt);

      if (is_obj && pea->is_alias(val)) {
        VirtualState* vs_field = as_virtual(pea, val);
        if (vs_field != nullptr) {
          update_internal_phi_nodes(kit, visited, pea->is_alias(val), vs_field, region, shim, paths);
        }
      }

      if (val->is_Phi() && val->in(0) == region) {
        const Type* type = kit->gvn().type(val);
        PhiNode* phi = PhiNode::make(shim, nullptr, type, val->adr_type());

        for (int j =0; j < paths.length(); ++j) {
          int idx = paths.at(j);
          phi->set_req(j + 1, val->in(idx));
        }
        kit->gvn().set_type(phi, type);
        vs->set_field(field, kit->gvn().transform(phi));
      }
  }
}
// Passive Materialization
// ------------------------
// Materialize an object at the phi node because at least one of its predecessors has materialized the object.
// https://gist.github.com/navyxliu/d2602d4a4df98f2fb7982cbd3e5ab6eb#solution
//
// Arguments newin@ and newin_map@ from predecessor r->in(pnum) and we need to materialize var for it.
// there are 2 possibilities. we found a shim region which has an identical virtual state of obj, then we merge it.
// or we create a new shim region.
Node* PEAState::materialize_incoming_virtual(GraphKit* kit, Node* var, PEAState& newin, SafePointNode* newin_map, PhiNode* phi, RegionNode* r, uint pnum) {
  if (!EnablePassiveMaterialization) return var;

  auto pea = kit->PEA();
  ObjID id = pea->is_alias(var);
  assert(newin.get_object_state(id)->is_virtual(), "must be virtual in newin");
  VirtualState* vs = static_cast<VirtualState*>(newin.get_object_state(id));
  // Stationary map:
  // because map (role as a local symbol table) is rolling, we need to preserve compile-time state.
  // it attaches to shim region.
  RegionNode* shim = nullptr;
  SafePointNode* map = nullptr;
  uint path;
  Node* retval;

  for (uint i = 1; i < r->req(); ++i) {
      Node* n = r->in(0);
      if (n != nullptr && n->is_Region()) {
        SafePointNode* sfpt = attached_sfpt(n->as_Region());
        if (sfpt != nullptr && matched(sfpt, id, vs)) {
          shim = n->as_Region();
          map = sfpt;
          path = i;
        }
      }
  }

  auto mat = [&](RegionNode* shim, SafePointNode* map, Node* var, RegionNode* r, uint pnum) -> Node* {
      GraphKit shim_kit(map->jvms());
      Node* prior_ctrl = shim_kit.control();
      Node* retval = newin.materialize(&shim_kit, var);

      for (uint i=1; i < r->req(); ++i) {
        if (i == pnum || r->in(i) == prior_ctrl) {
          r->set_req(i, shim_kit.control());
        }
      }
      return retval;
  };

  if (shim == nullptr) {
      shim = new RegionNode(2);
      shim->set_req(1, r->in(pnum));
      kit->gvn().set_type(shim, Type::CONTROL);
      kit->record_for_igvn(shim);

      map = newin_map->clone()->as_SafePoint();
      map->set_req(0, shim); // attach to shim!
      retval = mat(shim, map, var, r, pnum);
  } else {
    assert(map != nullptr, "must associate with a SafePoint");
    shim->add_req(r->in(pnum));
    ObjectState* os = get_object_state(id);
    if (os->is_virtual()) {
        retval = mat(shim, map, var, r, pnum);
    } else {
        retval = phi->in(path);
    }
  }

  phi->set_req(pnum, retval);
  return retval;
}

void PassiveMaterializationContext::begin() {
  _shim = new RegionNode(1);
  PhaseGVN& gvn = _orig_kit->gvn();

  for (uint i=1; i<_region->req(); ++i) {
    if (i == _pnum) continue;
    Node* n = _region->in(i);
    if (n != nullptr) {
      _paths.append(i);
      _shim->add_req(n);
    }
  }

  gvn.set_type(_shim, Type::CONTROL);
  _orig_kit->record_for_igvn(_shim);

  _map = _orig_kit->map()->clone()->as_SafePoint();
  _map->set_req(0, _shim); // attach to shim!
  bool unique = _paths.length() <= 1;

  // shim only covers selective paths, so we have to rewrite inputs if they are phi nodes.
  for (uint i = 1; i < _map->req(); ++i) {
    Node* n = _map->in(i);

    if (i == TypeFunc::Memory && n->is_MergeMem()) {
      // a blank memory
      MergeMemNode* new_mem = MergeMemNode::make(MergeMemNode::make_empty_memory());
      new_mem->grow_to_match(n->as_MergeMem());
      for (MergeMemStream mms(n->as_MergeMem()); mms.next_non_empty(); ) {
        Node* p = mms.memory();

        if (p->is_Phi() && p->in(0) == _region) {
          if (unique) {
            new_mem->set_req(mms.alias_idx(), p->in(_paths.at(0)));
          } else {
            PhiNode* phi = PhiNode::make(_shim, nullptr, Type::MEMORY, p->adr_type());
            for (int j =0; j < _paths.length(); ++j) {
              int idx = _paths.at(j);
              phi->set_req(j + 1, p->in(idx));
            }
            gvn.set_type(phi, Type::MEMORY);
            new_mem->set_req(mms.alias_idx(), gvn.transform(phi));
          }
        } else {
          new_mem->set_req(mms.alias_idx(), p);
        }
      }
      _map->set_req(i, new_mem);
    } else if (n->is_Phi() && n->in(0) == _region) {
      if (unique) {
        _map->set_req(i, n->in(_paths.at(0)));
      } else {
        const Type* type = gvn.type(n);
        PhiNode* phi = PhiNode::make(_shim, nullptr, type, n->adr_type());
        for (int j =0; j < _paths.length(); ++j) {
          int idx = _paths.at(j);
          phi->set_req(j + 1, n->in(idx));
        }
        gvn.set_type(phi, type);
        _map->set_req(i, gvn.transform(phi));
      }
    }
  }
}

PassiveMaterializationContext::~PassiveMaterializationContext() {
  if (_shim == nullptr) {
    return;
  }

  SafePointNode* current_map = _orig_kit->map();
  PhaseGVN& gvn = _orig_kit->gvn();

  // merge control.
  for (int j = 0; j < _paths.length(); ++j) {
    _region->set_req(_paths.at(j), _map->control());
  }
  // if we have already created phi nodes, update the corresponding paths.
  // if we don't created phi node yet, just overwrite them.
  for (uint i = 1; i < current_map->req(); ++i) {
    Node* n = current_map->in(i);
    if (i == TypeFunc::Memory && n->is_MergeMem()) {
      for (int j = 0; j < _paths.length(); ++j) {
        int idx = _paths.at(j);
        _orig_kit->merge_memory(_map->in(i), _region, idx);
      }
    } else if (n->is_Phi() && n->in(0) == _region) {
      bool in_table = gvn.hash_delete(n);
      for (int j = 0; j < _paths.length(); ++j) {
          int idx = _paths.at(j);
          n->set_req(idx, _map->in(i));
      }
      if (in_table) {
        gvn.hash_find_insert(n);
      }
    } else if (n != _map->in(i)) {
      current_map->set_req(i, _map->in(i));
    }
  }
}

GraphKit PassiveMaterializationContext::graphKit() {
  if (_shim == nullptr) {
    begin();
  }
  return {_map->jvms()};
}


// Materialize var for all predecessors of current region.
// r->in(pnum) is newin and the object is escaped in newin.
// We need to clone kit->map() and rewrite those phi nodes for the predecessors.
// After materialization, the new code controls all covered paths.
Node* PEAState::materialize_incoming_escaped(GraphKit* kit, Node* var, PassiveMaterializationContext& ctx) {
  if (!EnablePassiveMaterialization) return var;

  auto* pea = kit->PEA();
  ObjID id = pea->is_alias(var);

  auto&& shim_kit = ctx.graphKit();

  auto paths = ctx.paths();
  if (PEAVerbose) {
    tty->print_cr("materialize_incoming_escaped for obj = %d", id->_idx);

    tty->print("paths: ");
    for (int i =0; i < paths.length(); ++i) {
        tty->print("%d", paths.at(i));
        if (i+1 < paths.length()) {
          tty->print(", ");
        } else {
          tty->cr();
        }
    }
  }

  {
    ResourceMark rm;
    Unique_Node_List visited;
    update_internal_phi_nodes(kit, visited, id, as_virtual(pea, var), ctx.region(), ctx.shim(), paths);
  }

  Node* mat = materialize(&shim_kit, var);

  // after materialization, we need to update the current map.
  return mat;
}

AllocationStateMerger::AllocationStateMerger(PEAState& target, SafePointNode* map, RegionNode* region)
                                            : _state(target), _map(map), _region(region) {}

void AllocationStateMerger::merge(GraphKit* kit, PEAState& newin, SafePointNode* newin_map, int pnum, bool update_map) {
  PartialEscapeAnalysis* pea = kit->PEA();
  Unique_Node_List set1, set2;

  _state.objects(set1);
  newin.objects(set2);

  VectorSet intersection = intersect(set1.member_set(), set2.member_set());
  set1.remove_useless_nodes(intersection);
  {
    // first pass. handle passive materialization.
    PassiveMaterializationContext ctx(kit, _region, pnum);
    for (uint i = 0; i < set1.size(); ++i) {
      ObjID obj = static_cast<ObjID>(set1.at(i));
      ObjectState* os1 = _state.get_object_state(obj);
      ObjectState* os2 = newin.get_object_state(obj);

      if (!os1->is_virtual() || !os2->is_virtual()) {
        assert(os1 != nullptr && os2 != nullptr, "sanity check");
        Node* m;
        Node* n;

        if (os1->is_virtual()) {
          // If obj is virtual in current state,  it must be escaped in newin.
          // Mark it escaped in current state.
          EscapedState* es2 = static_cast<EscapedState*>(os2);
          m = _state.get_java_oop(obj);
          if (!es2->has_materialized() ||  m == _state.materialize_incoming_escaped(kit, m, ctx)) {
            _state.escape(obj, m, es2->has_materialized());
          }
        } else {
          // If obj is virtual in newin, it must be escaped in current state.
          // Mark it escaped in newin
          n = newin.get_java_oop(obj);
          newin.escape(obj, n, false);
        }
      }
    }
  }

  // second pass: merge escaped states.
  for (uint i = 0; i < set1.size(); ++i) {
    ObjID obj = static_cast<ObjID>(set1.at(i));
    ObjectState* os1 = _state.get_object_state(obj);
    ObjectState* os2 = newin.get_object_state(obj);

    if (!os1->is_virtual()) {
      assert(os2 != nullptr && !os2->is_virtual(), "os2 must be Escaped");

      // obj is escaped in both newin and current state.
      EscapedState* es1 = static_cast<EscapedState*>(os1);
      EscapedState* es2 = static_cast<EscapedState*>(os2);
      Node* m = es1->merged_value();
      Node* n = es2->merged_value();
      bool materialized = es1->has_materialized() || es2->has_materialized();

      if (m->is_Phi() && m->in(0) == _region) {
        ensure_phi(m->as_Phi(), pnum);
        // only update the pnum if we have never seen it before.
        if (m->in(pnum) == nullptr) {
          m->set_req(pnum, n);
        }
      } else if (m != n) {
        const Type* type = obj->oop_type(kit->gvn());
        Node* phi = PhiNode::make(_region, m, type);
        phi->set_req(pnum, n);
        kit->gvn().set_type(phi, type);
        es1->update(materialized, phi);


        // when we replace m with phi, rescan local symbol table(map)
        // caveat: we don't do this for Parse::merge_common because Parse::ensure_phi()
        // conduct type check. it avoid from creating phi nodes if the type of map->in(j)
        // is valid.
        if (update_map) {
          for (uint j = TypeFunc::Parms; j < kit->map()->req(); ++j) {
            Node* input = kit->map()->in(j);
            if (input == m) {
              kit->map()->set_req(j, phi);
            }
          }
        }

        // then rescan all virtual objects and update their fields.
        for (uint j = 0; j < set1.size(); ++j) {
          if (i == j) continue;
          ObjID obj2 = static_cast<ObjID>(set1.at(j));
          ObjectState* os = _state.get_object_state(obj2);

          if (os->is_virtual()) {
            VirtualState* vs = static_cast<VirtualState*>(os);

            for (auto&& f = vs->field_iterator(); f.has_next(); ++f) {
              if (f.value() == m) {
                vs->set_field(f.field(), phi);
              }
            }
          }
        }
      }
    }
  }
  // 3rd pass: merge virtual states.
  for (uint i = 0; i < set1.size(); ++i) {
    ObjID obj = static_cast<ObjID>(set1.at(i));
    ObjectState* os1 = _state.get_object_state(obj);
    ObjectState* os2 = newin.get_object_state(obj);

    if (os1->is_virtual() ) {
      assert(os2 != nullptr && os2->is_virtual(), "sanity");
      os1->merge(os2, kit, _region, pnum);
    }
  }

  // process individual phi
  SafePointNode* map = kit->map();
  for (uint i = 0; i < map->req(); ++i) {
    Node* node = map->in(i);

    if (node != nullptr && node->is_Phi() && node->as_Phi()->region() == _region) {
      process_phi(node->as_Phi(), kit, pnum);
    }
  }
}

// Merge phi node incrementally.
// we check all merged inputs in _state.
// 1. all inputs refer to the same ObjID, then phi is created as alias of ObjID
// 2. otherwise, any input is alias with a 'virtual' object needs to convert to 'Escaped'. replace input with merged_value.
// 3. otherwise, if any input is aliased with an Escaped object. replace input with merged value.
void AllocationStateMerger::process_phi(PhiNode* phi, GraphKit* kit, int pnum) {
  ObjID unique = nullptr;
  bool materialized = false;
  bool same_obj = true;
  PartialEscapeAnalysis* pea = kit->PEA();

  if (pea == nullptr) return;

  for (uint i = 1; i < phi->req(); ++i) {
    if (_region->in(i) == nullptr || _region->in(i)->is_top())
      continue;

    Node* node = phi->in(i);
    ObjID obj = pea->is_alias(node);
    if (obj != nullptr) {
      if (unique == nullptr) {
        unique = obj;
      } else if (unique != obj) {
        same_obj = false;
      }
      EscapedState* es = _state.as_escaped(pea, node);
      if (es != nullptr) {
        materialized |= es->has_materialized();
      }
    } else {
      same_obj = false;
    }
  }

  if (same_obj) {
    //xliu: should I also check pnum == 1?
    // phi nodes for exception handler may have leave normal paths vacant.
    pea->add_alias(unique, phi);
  } else {
    bool printed = false;

    for (uint i = 1; i < phi->req(); ++i) {
      if (_region->in(i) == nullptr || _region->in(i)->is_top())
        continue;

      Node* node = phi->in(i);
      ObjID obj = pea->is_alias(node);
      if (obj != nullptr && _state.contains(obj)) {
        ObjectState* os = _state.get_object_state(obj);
        if (os->is_virtual()) {
          os = _state.escape(obj, node, materialized);
        }
        EscapedState* es = static_cast<EscapedState*>(os);
        Node* value = es->merged_value();
        if (value->is_Phi() && value->in(0) == _region) {
          value = value->in(i);
        }

        if (node != value) {
          assert(value != phi, "sanity");
#ifndef PRODUCT
          if (PEAVerbose) {
            if (!printed) {
              phi->dump();
              printed = true;
            }
            tty->print_cr("[PEA] replace %dth input with node %d", i, value->_idx);
          }
#endif
          phi->replace_edge(node, value);
        }
      }
    }
    ObjID obj = pea->is_alias(phi);
    if (obj != nullptr) {
      pea->remove_alias(obj, phi);
    }
  }
}

void AllocationStateMerger::merge_at_phi_creation(GraphKit* kit, PEAState& newin, SafePointNode* newin_map, int pnum, PhiNode* phi, Node* m, Node* n) {
  auto pea = kit->PEA();
  ObjID obj1 = pea->is_alias(m);
  ObjID obj2 = pea->is_alias(n);

  if (_state.contains(obj1)) { // m points to an object that as is tracking.
    ObjectState* os1 = _state.get_object_state(obj1);
    ObjectState* os2 = newin.contains(obj2) ? newin.get_object_state(obj2) : nullptr;

    // obj1 != obj2 if n points to something else. It could be the other object, null or a ConP.
    // we do nothing here because PEA doesn't create phi in this case.
    if (obj1 == obj2 && os2 != nullptr) { // n points to the same object and pred_as is tracking the object.
      if (!os1->is_virtual() || !os2->is_virtual()) {
        if (os1->is_virtual()) {
          PassiveMaterializationContext ctx(kit, _region, pnum);
          if (m == _state.materialize_incoming_escaped(kit, m, ctx)) {
            bool materialized = static_cast<EscapedState*>(os2)->has_materialized();
            _state.escape(obj1, phi, materialized);
          } else {
            assert(!_state.get_object_state(obj1)->is_virtual(), "must be escaped after passive materialization");
            static_cast<EscapedState*>(_state.get_object_state(obj1))->update(phi);
          }
        } else {
          static_cast<EscapedState*>(os1)->update(phi);
        }
      }
    }
  }
}

AllocationStateMerger::~AllocationStateMerger() {
}

bool PEAContext::match(ciMethod* method) const {
  if (_matcher != nullptr && method != nullptr) {
    VM_ENTRY_MARK;
    methodHandle mh(THREAD, method->get_Method());
    return _matcher->match(mh);
  }
  return true;
}

EscapedState* PEAState::escape(ObjID id, Node* p, bool materialized) {
  assert(p != nullptr, "the new alias must be non-null");
  EscapedState* es;

  if (contains(id)) {
    ObjectState* os = get_object_state(id);
    // if os is EscapedState and its materialized_value is not-null,
    if (!os->is_virtual()) {
      materialized |= static_cast<EscapedState*>(os)->has_materialized();
    }
    es = new EscapedState(materialized, p);
    es->ref_cnt(os->ref_cnt()); // copy the refcnt from the original ObjectState.
  } else {
    es = new EscapedState(materialized, p);
  }
  _state.put(id, es);
  if (materialized) {
    static_cast<AllocateNode*>(id)->inc_materialized();
  }
  assert(contains(id), "sanity check");
  return es;
}
