
/*

  KLayout Layout Viewer
  Copyright (C) 2006-2017 Matthias Koefferlein

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

*/


#include "pyaObject.h"
#include "pyaMarshal.h"
#include "pyaUtils.h"
#include "pyaConvert.h"
#include "pya.h"

#include "tlLog.h"

namespace pya
{

// --------------------------------------------------------------------------
//  Implementation of CallbackFunction

CallbackFunction::CallbackFunction (PythonRef pym, const gsi::MethodBase *m)
  : mp_method (m)
{
  //  We have a problem here with cyclic references. Bound instances methods can 
  //  create reference cycles if their target objects somehow points back to us
  //  (or worse, to some parent of us, i.e. inside a QWidget hierarchy).
  //  A solution is to take a bound instance method apart and store a weak
  //  reference to self plus a real reference to the function.

  if (pym && PyMethod_Check (pym.get ()) && PyMethod_Self (pym.get ()) != NULL) {

    m_weak_self = PythonRef (PyWeakref_NewRef (PyMethod_Self (pym.get ()), NULL));
    m_callable = PythonRef (PyMethod_Function (pym.get ()), false /* borrowed ref */);
#if PY_MAJOR_VERSION < 3
    m_class = PythonRef (PyMethod_Class (pym.get ()), false /* borrowed ref */);
#endif

  } else {
    m_callable = pym;
  }
}

const gsi::MethodBase *CallbackFunction::method () const
{
  return mp_method;
}

PythonRef CallbackFunction::callable () const
{
  if (m_callable && m_weak_self) {

    PyObject *self = PyWeakref_GetObject (m_weak_self.get ());
    if (self == Py_None) {
      //  object expired - no callback possible
      return PythonRef ();
    }

#if PY_MAJOR_VERSION < 3
    return PythonRef (PyMethod_New (m_callable.get (), self, m_class.get ()));
#else
    return PythonRef (PyMethod_New (m_callable.get (), self));
#endif

  } else {
    return m_callable;
  }
}

bool CallbackFunction::is_instance_method () const
{
  return m_callable && m_weak_self;
}

PyObject *CallbackFunction::self_ref () const
{
  return PyWeakref_GetObject (m_weak_self.get ());
}

PyObject *CallbackFunction::callable_ref () const
{
  return m_callable.get ();
}

bool CallbackFunction::operator== (const CallbackFunction &other) const
{
  if (is_instance_method () != other.is_instance_method ()) {
    return false;
  }
  if (m_weak_self) {
    if (self_ref () != other.self_ref ()) {
      return false;
    }
  }
  return callable_ref () == other.callable_ref ();
}

// --------------------------------------------------------------------------
//  Implementation of Callee

Callee::Callee (PYAObjectBase *obj)
  : mp_obj (obj)
{
  //  .. nothing yet ..
}

Callee::~Callee ()
{
  //  .. nothing yet ..
}

int 
Callee::add_callback (const CallbackFunction &vf)
{
  m_cbfuncs.push_back (vf);
  return int (m_cbfuncs.size () - 1);
}

void 
Callee::clear_callbacks ()
{
  m_cbfuncs.clear ();
}

void 
Callee::call (int id, gsi::SerialArgs &args, gsi::SerialArgs &ret) const
{
  const gsi::MethodBase *meth = m_cbfuncs [id].method ();

  try {

    PythonRef callable (m_cbfuncs [id].callable ());

    tl::Heap heap;

    if (callable) {

      PYTHON_BEGIN_EXEC

        size_t arg4self = 1;

        //  One argument for "self"
        PythonRef argv (PyTuple_New (arg4self + std::distance (meth->begin_arguments (), meth->end_arguments ())));

        //  Put self into first argument
        PyTuple_SetItem (argv.get (), 0, mp_obj);
        Py_INCREF (mp_obj);

        //  TODO: callbacks with default arguments?
        for (gsi::MethodBase::argument_iterator a = meth->begin_arguments (); args && a != meth->end_arguments (); ++a) {
          PyTuple_SetItem (argv.get (), arg4self + (a - meth->begin_arguments ()), pop_arg (*a, args, NULL, heap).release ());
        }

        PythonRef result (PyObject_CallObject (callable.get (), argv.get ()));
        if (! result) {
          check_error ();
        }

        tl::Heap heap;
        push_arg (meth->ret_type (), ret, result.get (), heap);

        
        //  a Python callback must not leave temporary objects
        tl_assert (heap.empty ());

      PYTHON_END_EXEC

    }

  } catch (PythonError &err) {
    PythonError err_with_context (err);
    err_with_context.set_context (mp_obj->cls_decl ()->name () + "." + meth->names ());
    throw err_with_context;
  } catch (tl::ExitException &) {
    throw;
  } catch (tl::Exception &ex) {
    throw tl::Exception (tl::to_string (QObject::tr ("Error calling method")) + " '" + mp_obj->cls_decl ()->name () + "." + meth->names () + "': " + ex.msg ());
  } catch (...) {
    throw;
  }
}

// --------------------------------------------------------------------------
//  Implementation of SignalHandler

SignalHandler::SignalHandler ()
{
  //  .. nothing yet ..
}

SignalHandler::~SignalHandler ()
{
  clear ();
}

void SignalHandler::call (const gsi::MethodBase *meth, gsi::SerialArgs &args, gsi::SerialArgs &ret) const
{
  PYTHON_BEGIN_EXEC

    tl::Heap heap;

    int args_avail = int (std::distance (meth->begin_arguments (), meth->end_arguments ()));
    PythonRef argv (PyTuple_New (args_avail));
    for (gsi::MethodBase::argument_iterator a = meth->begin_arguments (); args && a != meth->end_arguments (); ++a) {
      PyTuple_SetItem (argv.get (), int (a - meth->begin_arguments ()), pop_arg (*a, args, NULL, heap).release ());
    }

    PythonRef result;
    for (std::vector<CallbackFunction>::const_iterator c = m_cbfuncs.begin (); c != m_cbfuncs.end (); ++c) {

      //  determine the number of arguments required
      int arg_count = args_avail;
      if (args_avail > 0) {

        PythonRef fc (PyObject_GetAttrString (c->callable ().get (), "__code__"));
        if (fc) {
          PythonRef ac (PyObject_GetAttrString (fc.get (), "co_argcount"));
          if (ac) {
            arg_count = python2c<int> (ac.get ());
            if (PyObject_HasAttrString (c->callable ().get (), "__self__")) {
              arg_count -= 1;
            }
          }
        }

      }

      //  use less arguments if applicable
      if (arg_count == 0) {
        result = PythonRef (PyObject_CallObject (c->callable ().get (), NULL));
      } else if (arg_count < args_avail) {
        PythonRef argv_less (PyTuple_GetSlice (argv.get (), 0, arg_count));
        result = PythonRef (PyObject_CallObject (c->callable ().get (), argv_less.get ()));
      } else {
        result = PythonRef (PyObject_CallObject (c->callable ().get (), argv.get ()));
      }

      if (! result) {
        check_error ();
      }

    }

    push_arg (meth->ret_type (), ret, result.get (), heap);

    //  a Python callback must not leave temporary objects
    tl_assert (heap.empty ());

  PYTHON_END_EXEC
}

void SignalHandler::add (PyObject *callable)
{
  remove (callable);
  m_cbfuncs.push_back (CallbackFunction (PythonPtr (callable), 0));
}

void SignalHandler::remove (PyObject *callable)
{
  //  To avoid cyclic references, the CallbackFunction holder is employed. However, the
  //  "true" callable no longer is the original one. Hence, we need to do a strict compare
  //  against the effective one.
  CallbackFunction cbref (PythonPtr (callable), 0);
  for (std::vector<CallbackFunction>::iterator c = m_cbfuncs.begin (); c != m_cbfuncs.end (); ++c) {
    if (*c == cbref) {
      m_cbfuncs.erase (c);
      break;
    }
  }
}

void SignalHandler::clear ()
{
  m_cbfuncs.clear ();
}

void SignalHandler::assign (const SignalHandler *other)
{
  m_cbfuncs = other->m_cbfuncs;
}

// --------------------------------------------------------------------------
//  Implementation of StatusChangedListener

StatusChangedListener::StatusChangedListener (PYAObjectBase *pya_object)
  : mp_pya_object (pya_object)
{
  //  .. nothing yet ..
}

void
StatusChangedListener::object_status_changed (gsi::ObjectBase::StatusEventType type)
{
  mp_pya_object->object_status_changed (type);
}

// --------------------------------------------------------------------------
//  Implementation of PYAObjectBase

PYAObjectBase::PYAObjectBase(const gsi::ClassBase *_cls_decl)
  : m_listener (this),
    m_callee (this),
    m_cls_decl (_cls_decl),
    m_obj (0),
    m_owned (false),
    m_const_ref (false),
    m_destroyed (false),
    m_can_destroy (false)
{
}

PYAObjectBase::~PYAObjectBase ()
{
  try {

    bool prev_owned = m_owned;
    void *prev_obj = m_obj;

    detach ();

    //  Destroy the object if we are owner. We don't destroy the object if it was locked
    //  (either because we are not owner or from C++ side using keep())
    if (m_cls_decl && prev_obj && prev_owned) {
      m_cls_decl->destroy (prev_obj);
    }

  } catch (std::exception &ex) {
    tl::warn << "Caught exception in object destructor: " << ex.what ();
  } catch (tl::Exception &ex) {
    tl::warn << "Caught exception in object destructor: " << ex.msg ();
  } catch (...) {
    tl::warn << "Caught unspecified exception in object destructor";
  }

  m_destroyed = true;
}

void
PYAObjectBase::object_status_changed (gsi::ObjectBase::StatusEventType type)
{
  if (type == gsi::ObjectBase::ObjectDestroyed) {

    //  This may happen outside the Python interpreter, so we safeguard ourselves against this.
    //  In this case, we may encounter a memory leak, but there is little we can do
    //  against this and it will happen in the application teardown anyway.
    if (PythonInterpreter::instance ()) {

      bool prev_owner = m_owned;

      m_destroyed = true;  // NOTE: must be set before detach!

      detach ();

      //  NOTE: this may delete "this"!
      if (!prev_owner) {
        Py_DECREF (this);
      }

    }

  } else if (type == gsi::ObjectBase::ObjectKeep) {
    keep_internal ();
  } else if (type == gsi::ObjectBase::ObjectRelease) {
    release ();
  }
}

void 
PYAObjectBase::release ()
{
  //  If the object is managed we first reset the ownership of all other clients
  //  and then make us the owner
  const gsi::ClassBase *cls = cls_decl ();
  if (cls && cls->is_managed ()) {
    void *o = obj ();
    if (o) {
      cls->gsi_object (o)->keep ();
    }
  }

  //  NOTE: this is fairly dangerous
  if (!m_owned) {
    m_owned = true;
    //  NOTE: this may delete "this"! TODO: this should not happen. Can we assert that somehow?
    Py_DECREF (this);
  }
}

void
PYAObjectBase::keep_internal ()
{
  if (m_owned) {
    Py_INCREF (this);
    m_owned = false;
  }
}

void 
PYAObjectBase::keep ()
{
  const gsi::ClassBase *cls = cls_decl ();
  if (cls) {
    void *o = obj ();
    if (o) {
      if (cls->is_managed ()) {
        cls->gsi_object (o)->keep ();
      } else {
        keep_internal ();
      }
    }
  }
}

void 
PYAObjectBase::detach ()
{
  if (m_obj) {

    const gsi::ClassBase *cls = cls_decl ();

    if (! m_destroyed && cls && cls->is_managed ()) {
      gsi::ObjectBase *gsi_object = cls->gsi_object (m_obj, false);
      if (gsi_object) {
        gsi_object->status_changed_event ().remove (&m_listener, &StatusChangedListener::object_status_changed);
      }
    }

    detach_callbacks ();

    m_obj = 0;
    m_const_ref = false;
    m_owned = false;
    m_can_destroy = false;

  }
}

void 
PYAObjectBase::set (void *obj, bool owned, bool const_ref, bool can_destroy) 
{
  const gsi::ClassBase *cls = cls_decl ();
  if (!cls) {
    return;
  }

  tl_assert (! m_obj);
  tl_assert (obj);

  m_obj = obj;
  m_owned = owned;
  m_can_destroy = can_destroy;
  m_const_ref = const_ref;

  //  initialize the callbacks according to the methods which need some
  initialize_callbacks ();

  if (cls->is_managed ()) {
    gsi::ObjectBase *gsi_object = cls->gsi_object (m_obj);
    //  Consider the case of "keep inside constructor"
    if (gsi_object->already_kept ()) {
      keep_internal ();
    }
    gsi_object->status_changed_event ().add (&m_listener, &StatusChangedListener::object_status_changed);
  }

  if (!m_owned) {
    Py_INCREF (this);
  }
}

//  TODO: a static (singleton) instance is not thread-safe
PYAObjectBase::callbacks_cache PYAObjectBase::s_callbacks_cache;

pya::SignalHandler *
PYAObjectBase::signal_handler (const gsi::MethodBase *meth)
{
  std::map <const gsi::MethodBase *, pya::SignalHandler>::iterator st = m_signal_table.find (meth);
  if (st == m_signal_table.end ()) {
    st = m_signal_table.insert (std::make_pair (meth, pya::SignalHandler ())).first;
    meth->add_handler (obj (), &st->second);
  }
  return &st->second;
}

void
PYAObjectBase::initialize_callbacks ()
{
//  1 to enable caching, 0 to disable it.
//  TODO: caching appears to create some leaks ...
#if 1

  PythonRef type_ref ((PyObject *) Py_TYPE (this), false /*borrowed*/);

  //  Locate the callback-enabled methods set by Python tpye object (pointer)
  //  NOTE: I'm not quite sure whether the type object pointer is a good key
  //  for the cache. It may change since class objects may expire too if
  //  classes are put on the heap. Hence we have to keep a reference which is
  //  a pity, but hard to avoid.
  callbacks_cache::iterator cb = s_callbacks_cache.find (type_ref);
  if (cb == s_callbacks_cache.end ()) {

    cb = s_callbacks_cache.insert (std::make_pair (type_ref, callback_methods_type ())).first;
    
    const gsi::ClassBase *cls = cls_decl ();

    //  TODO: cache this .. this is taking too much time if done on every instance
    //  we got a new object - hence we have to attach event handlers.
    //  We don't need to install virtual function callbacks because in that case, no overload is possible
    //  (the object has been created on C++ side).
    while (cls) {

      for (gsi::ClassBase::method_iterator m = cls->begin_callbacks (); m != cls->end_callbacks (); ++m) {

        if (m_owned) {

          //  NOTE: only Python-implemented classes can reimplement methods. Since we
          //  take the attribute from the class object, only Python instances can overwrite 
          //  the methods and owned indicates that. owned == true indicates that.

          //  NOTE: a callback may not have aliases nor overloads
          const char *nstr = (*m)->primary_name ().c_str ();

          //  NOTE: we just take attributes from the class object. That implies that it's not
          //  possible to reimplement a method through instance attributes (rare case, I hope).
          //  In addition, if we'd use instance attributes we create circular references 
          //  (self/callback to method, method to self).
          //  TOOD: That may happen too often, i.e. if the Python class does not reimplement the virtual
          //  method, but the C++ class defines a method hook that the reimplementation can call. 
          //  We don't want to produce a lot of overhead for the Qt classes here.
          PythonRef py_attr = PyObject_GetAttrString ((PyObject *) Py_TYPE (this), nstr);
          if (! py_attr) {

            //  because PyObject_GetAttrString left an error
            PyErr_Clear ();

          } else {

            //  Only if a Python-level class defines that method we can link the virtual method call to the 
            //  Python method. We should not create callbacks which we refer to C class implementations because that
            //  may create issues with callbacks during destruction (i.e. QWidget-destroyed signal)
            if (! PyCFunction_Check (py_attr.get ())) {
              cb->second.push_back (*m);
            }

          }

        }

      }

      //  consider base classes as well.
      cls = cls->base ();

    }

  }

  for (callback_methods_type::const_iterator m = cb->second.begin (); m != cb->second.end (); ++m) {

    PythonRef py_attr;
    const char *nstr = (*m)->primary_name ().c_str ();
    py_attr = PyObject_GetAttrString ((PyObject *) Py_TYPE (this), nstr);

    int id = m_callee.add_callback (CallbackFunction (py_attr, *m));
    (*m)->set_callback (m_obj, gsi::Callback (id, &m_callee, (*m)->argsize (), (*m)->retsize ()));

  }

#else

  const gsi::ClassBase *cls = cls_decl ();

  //  TODO: cache this .. this is taking too much time if done on every instance
  //  we got a new object - hence we have to attach event handlers.
  //  We don't need to install virtual function callbacks because in that case, no overload is possible
  //  (the object has been created on C++ side).
  while (cls) {

    for (gsi::ClassBase::method_iterator m = cls->begin_methods (); m != cls->end_methods (); ++m) {

      if ((*m)->is_callback () && m_owned) {

        //  NOTE: only Python-implemented classes can reimplement methods. Since we
        //  take the attribute from the class object, only Python instances can overwrite 
        //  the methods and owned indicates that. owned == true indicates that.

        //  NOTE: a callback may not have aliases nor overloads
        const char *nstr = (*m)->primary_name ().c_str ();

        //  NOTE: we just take attributes from the class object. That implies that it's not
        //  possible to reimplement a method through instance attributes (rare case, I hope).
        //  In addition, if we'd use instance attributes we create circular references 
        //  (self/callback to method, method to self).
        //  TOOD: That may happen too often, i.e. if the Python class does not reimplement the virtual
        //  method, but the C++ class defines a method hook that the reimplementation can call. 
        //  We don't want to produce a lot of overhead for the Qt classes here.
        PythonRef py_attr = PyObject_GetAttrString ((PyObject *) Py_TYPE (this), nstr);
        if (! py_attr) {

          //  because PyObject_GetAttrString left an error
          PyErr_Clear ();

        } else {

          //  Only if a Python-level class defines that method we can link the virtual method call to the 
          //  Python method. We should not create callbacks which we refer to C class implementations because that
          //  may create issues with callbacks during destruction (i.e. QWidget-destroyed signal)
          if (! PyCFunction_Check (py_attr.get ())) {

            PyObject *py_attr = PyObject_GetAttrString ((PyObject *) Py_TYPE (this), nstr);
            tl_assert (py_attr != NULL);
            int id = m_callee.add_callback (CallbackFunction (py_attr, *m));
            (*m)->set_callback (m_obj, gsi::Callback (id, &m_callee, (*m)->argsize (), (*m)->retsize ()));

          }

        }

      }

    }

    //  consider base classes as well.
    cls = cls->base ();

  }

#endif
}

void 
PYAObjectBase::clear_callbacks_cache ()
{
  s_callbacks_cache.clear ();
}

void
PYAObjectBase::detach_callbacks ()
{
  PythonRef type_ref ((PyObject *) Py_TYPE (this), false /*borrowed*/);

  callbacks_cache::iterator cb = s_callbacks_cache.find (type_ref);
  if (cb != s_callbacks_cache.end ()) {
    for (callback_methods_type::const_iterator m = cb->second.begin (); m != cb->second.end (); ++m) {
      (*m)->set_callback (m_obj, gsi::Callback ());
    }
  }

  m_callee.clear_callbacks ();
}

void 
PYAObjectBase::destroy ()
{
  if (! m_cls_decl) {
    m_obj = 0;
    return;
  }

  if (!m_can_destroy && m_obj) {
    throw tl::Exception (tl::to_string (QObject::tr ("Object cannot be destroyed explicitly")));
  }

  //  first create the object if it was not created yet and check if it has not been 
  //  destroyed already (the former is to ensure that the object is created at least)
  if (! m_obj) {
    if (m_destroyed) {
      throw tl::Exception (tl::to_string (QObject::tr ("Object has been destroyed already")));
    } else {
      m_obj = m_cls_decl->create ();
      m_owned = true;
    }
  }

  void *o = 0;
  if (m_owned || m_can_destroy) {
    o = m_obj;
  }

  detach ();

  if (o) {
    m_cls_decl->destroy (o);
  }

  m_destroyed = true;
}

void *
PYAObjectBase::obj () 
{
  if (! m_obj) {
    if (m_destroyed) {
      throw tl::Exception (tl::to_string (QObject::tr ("Object has been destroyed already")));
    } else {
      //  delayed creation of a detached C++ object ..
      set(cls_decl ()->create (), true, false, true);
    }
  }

  return m_obj;
}

}

