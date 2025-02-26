/* The implementation of the StringDType class */
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "structmember.h"

#define NPY_NO_DEPRECATED_API NPY_API_VERSION
#define _MULTIARRAYMODULE

#include "numpy/arrayobject.h"
#include "numpy/ndarraytypes.h"
#include "numpy/npy_math.h"

#include "static_string.h"
#include "dtypemeta.h"
#include "dtype.h"
#include "casts.h"
#include "gil_utils.h"
#include "conversion_utils.h"
#include "npy_import.h"

/*
 * Internal helper to create new instances
 */
PyObject *
new_stringdtype_instance(PyObject *na_object, int coerce, npy_string_allocator *allocator)
{
    PyObject *new =
            PyArrayDescr_Type.tp_new((PyTypeObject *)&PyArray_StringDType, NULL, NULL);

    if (new == NULL) {
        return NULL;
    }

    char *default_string_buf = NULL;
    char *na_name_buf = NULL;
    char array_owned = 0;

    if (allocator == NULL) {
        allocator = NpyString_new_allocator(PyMem_RawMalloc, PyMem_RawFree,
                                            PyMem_RawRealloc);
        if (allocator == NULL) {
            PyErr_SetString(PyExc_MemoryError,
                            "Failed to create string allocator");
            goto fail;
        }
    }
    else {
        // indicating that this is a view
        array_owned = 2;
    }

    npy_static_string default_string = {0, NULL};
    npy_static_string na_name = {0, NULL};

    Py_XINCREF(na_object);
    ((PyArray_StringDTypeObject *)new)->na_object = na_object;
    int has_null = na_object != NULL;
    int has_nan_na = 0;
    int has_string_na = 0;
    if (has_null) {
        // first check for a string
        if (PyUnicode_Check(na_object)) {
            has_string_na = 1;
            Py_ssize_t size = 0;
            const char *buf = PyUnicode_AsUTF8AndSize(na_object, &size);
            if (buf == NULL) {
                goto fail;
            }
            default_string.buf = PyMem_RawMalloc(size);
            if (default_string.buf == NULL) {
                PyErr_NoMemory();
                goto fail;
            }
            memcpy((char *)default_string.buf, buf, size);
            default_string.size = size;
        }
        else {
            // treat as nan-like if != comparison returns a object whose truth
            // value raises an error (pd.NA) or a truthy value (e.g. a
            // NaN-like object)
            PyObject *ne_result = PyObject_RichCompare(na_object, na_object, Py_NE);
            if (ne_result == NULL) {
                goto fail;
            }
            int is_truthy = PyObject_IsTrue(ne_result);
            if (is_truthy == -1) {
                PyErr_Clear();
                has_nan_na = 1;
            }
            else if (is_truthy == 1) {
                has_nan_na = 1;
            }
            Py_DECREF(ne_result);
        }
        PyObject *na_pystr = PyObject_Str(na_object);
        if (na_pystr == NULL) {
            goto fail;
        }

        Py_ssize_t size = 0;
        const char *utf8_ptr = PyUnicode_AsUTF8AndSize(na_pystr, &size);
        if (utf8_ptr == NULL) {
            Py_DECREF(na_pystr);
            goto fail;
        }
        na_name.buf = PyMem_RawMalloc(size);
        if (na_name.buf == NULL) {
            Py_DECREF(na_pystr);
            goto fail;
        }
        memcpy((char *)na_name.buf, utf8_ptr, size);
        na_name.size = size;
        Py_DECREF(na_pystr);
    }

    PyArray_StringDTypeObject *snew = (PyArray_StringDTypeObject *)new;

    snew->has_nan_na = has_nan_na;
    snew->has_string_na = has_string_na;
    snew->coerce = coerce;
    snew->allocator = allocator;
    snew->array_owned = 2;
    snew->na_name = na_name;
    snew->default_string = default_string;

    PyArray_Descr *base = (PyArray_Descr *)new;
    base->elsize = SIZEOF_NPY_PACKED_STATIC_STRING;
    base->alignment = ALIGNOF_NPY_PACKED_STATIC_STRING;
    base->flags |= NPY_NEEDS_INIT;
    base->flags |= NPY_LIST_PICKLE;
    base->flags |= NPY_ITEM_REFCOUNT;
    base->type_num = NPY_VSTRING;
    base->kind = NPY_VSTRINGLTR;
    base->type = NPY_VSTRINGLTR;

    return new;

fail:
    // this only makes sense if the allocator isn't attached to new yet
    Py_DECREF(new);
    if (default_string_buf != NULL) {
        PyMem_RawFree(default_string_buf);
    }
    if (na_name_buf != NULL) {
        PyMem_RawFree(na_name_buf);
    }
    if (allocator != NULL && array_owned != 2) {
        NpyString_free_allocator(allocator);
    }
    return NULL;
}

static int
na_eq_cmp(PyObject *a, PyObject *b) {
    if (a == b) {
        // catches None and other singletons like Pandas.NA
        return 1;
    }
    if (a == NULL || b == NULL) {
        return 0;
    }
    if (PyFloat_Check(a) && PyFloat_Check(b)) {
        // nan check catches np.nan and float('nan')
        double a_float = PyFloat_AsDouble(a);
        if (a_float == -1.0 && PyErr_Occurred()) {
            return -1;
        }
        double b_float = PyFloat_AsDouble(b);
        if (b_float == -1.0 && PyErr_Occurred()) {
            return -1;
        }
        if (npy_isnan(a_float) && npy_isnan(b_float)) {
            return 1;
        }
    }
    int ret = PyObject_RichCompareBool(a, b, Py_EQ);
    if (ret == -1) {
        PyErr_Clear();
        return 0;
    }
    return ret;
}

// sets the logical rules for determining equality between dtype instances
int
_eq_comparison(int scoerce, int ocoerce, PyObject *sna, PyObject *ona)
{
    if (scoerce != ocoerce) {
        return 0;
    }
    return na_eq_cmp(sna, ona);
}

/*
 * This is used to determine the correct dtype to return when dealing
 * with a mix of different dtypes (for example when creating an array
 * from a list of scalars).
 */
static PyArray_StringDTypeObject *
common_instance(PyArray_StringDTypeObject *dtype1, PyArray_StringDTypeObject *dtype2)
{
    int eq = _eq_comparison(dtype1->coerce, dtype2->coerce, dtype1->na_object,
                            dtype2->na_object);

    if (eq <= 0) {
        PyErr_SetString(
                PyExc_ValueError,
                "Cannot find common instance for unequal dtype instances");
        return NULL;
    }

    return (PyArray_StringDTypeObject *)new_stringdtype_instance(
            dtype1->na_object, dtype1->coerce, NULL);
}

/*
 *  Used to determine the correct "common" dtype for promotion.
 *  cls is always PyArray_StringDType, other is an arbitrary other DType
 */
static PyArray_DTypeMeta *
common_dtype(PyArray_DTypeMeta *cls, PyArray_DTypeMeta *other)
{
    if (other->type_num == NPY_UNICODE) {
        /*
         *  We have a cast from unicode, so allow unicode to promote
         *  to PyArray_StringDType
         */
        Py_INCREF(cls);
	return cls;
    }
    Py_INCREF(Py_NotImplemented);
    return (PyArray_DTypeMeta *)Py_NotImplemented;
}


// returns a new reference to the string repr of
// `scalar`. If scalar is not already a string and
// coerce is nonzero, __str__ is called to convert it
// to a string. If coerce is zero, raises an error for
// non-string or non-NA input.
static PyObject *
as_pystring(PyObject *scalar, int coerce)
{
    PyTypeObject *scalar_type = Py_TYPE(scalar);
    if (scalar_type == &PyUnicode_Type) {
        Py_INCREF(scalar);
        return scalar;
    }
    if (coerce == 0) {
        PyErr_SetString(PyExc_ValueError,
                        "StringDType only allows string data when "
                        "string coercion is disabled.");
        return NULL;
    }
    else {
        // attempt to coerce to str
        scalar = PyObject_Str(scalar);
        if (scalar == NULL) {
            // __str__ raised an exception
            return NULL;
        }
    }
    return scalar;
}

static PyArray_Descr *
string_discover_descriptor_from_pyobject(PyTypeObject *NPY_UNUSED(cls),
                                         PyObject *obj)
{
    PyObject *val = as_pystring(obj, 1);
    if (val == NULL) {
        return NULL;
    }

    Py_DECREF(val);

    PyArray_Descr *ret = (PyArray_Descr *)new_stringdtype_instance(NULL, 1, NULL);

    return ret;
}

// Take a python object `obj` and insert it into the array of dtype `descr` at
// the position given by dataptr.
int
stringdtype_setitem(PyArray_StringDTypeObject *descr, PyObject *obj, char **dataptr)
{
    npy_packed_static_string *sdata = (npy_packed_static_string *)dataptr;

    int is_cmp = 0;

    // borrow reference
    PyObject *na_object = descr->na_object;

    // Note there are two different na_object != NULL checks here.
    //
    // Do not refactor this!
    //
    // We need the result of the comparison after acquiring the allocator, but
    // cannot use functions requiring the GIL when the allocator is acquired,
    // so we do the comparison before acquiring the allocator.

    if (na_object != NULL) {
        is_cmp = na_eq_cmp(obj, na_object);
        if (is_cmp == -1) {
            return -1;
        }
    }

    npy_string_allocator *allocator = NpyString_acquire_allocator(descr);

    if (na_object != NULL) {
        if (is_cmp) {
            if (NpyString_pack_null(allocator, sdata) < 0) {
                PyErr_SetString(PyExc_MemoryError,
                                "Failed to pack null string during StringDType "
                                "setitem");
                goto fail;
            }
            goto success;
        }
    }
    PyObject *val_obj = as_pystring(obj, descr->coerce);

    if (val_obj == NULL) {
        goto fail;
    }

    Py_ssize_t length = 0;
    const char *val = PyUnicode_AsUTF8AndSize(val_obj, &length);
    if (val == NULL) {
        Py_DECREF(val_obj);
        goto fail;
    }

    if (NpyString_pack(allocator, sdata, val, length) < 0) {
        PyErr_SetString(PyExc_MemoryError,
                        "Failed to pack string during StringDType "
                        "setitem");
        Py_DECREF(val_obj);
        goto fail;
    }
    Py_DECREF(val_obj);

success:
    NpyString_release_allocator(allocator);

    return 0;

fail:
    NpyString_release_allocator(allocator);

    return -1;
}

static PyObject *
stringdtype_getitem(PyArray_StringDTypeObject *descr, char **dataptr)
{
    PyObject *val_obj = NULL;
    npy_packed_static_string *psdata = (npy_packed_static_string *)dataptr;
    npy_static_string sdata = {0, NULL};
    int has_null = descr->na_object != NULL;
    npy_string_allocator *allocator = NpyString_acquire_allocator(descr);
    int is_null = NpyString_load(allocator, psdata, &sdata);

    if (is_null < 0) {
        PyErr_SetString(PyExc_MemoryError,
                        "Failed to load string in StringDType getitem");
        goto fail;
    }
    else if (is_null == 1) {
        if (has_null) {
            PyObject *na_object = descr->na_object;
            Py_INCREF(na_object);
            val_obj = na_object;
        }
        else {
            // cannot fail
            val_obj = PyUnicode_FromStringAndSize("", 0);
        }
    }
    else {
#ifndef PYPY_VERSION
        val_obj = PyUnicode_FromStringAndSize(sdata.buf, sdata.size);
#else
        // work around pypy issue #4046, can delete this when the fix is in
        // a released version of pypy
        val_obj = PyUnicode_FromStringAndSize(
                sdata.buf == NULL ? "" : sdata.buf, sdata.size);
#endif
        if (val_obj == NULL) {
            goto fail;
        }
    }

    NpyString_release_allocator(allocator);

    return val_obj;

fail:

    NpyString_release_allocator(allocator);

    return NULL;
}

// PyArray_NonzeroFunc
// Unicode strings are nonzero if their length is nonzero.
npy_bool
nonzero(void *data, void *NPY_UNUSED(arr))
{
    return NpyString_size((npy_packed_static_string *)data) != 0;
}

// Implementation of PyArray_CompareFunc.
// Compares unicode strings by their code points.
int
compare(void *a, void *b, void *arr)
{
    PyArray_StringDTypeObject *descr = (PyArray_StringDTypeObject *)PyArray_DESCR(arr);
    // acquire the allocator here but let _compare get its own reference via
    // descr so we can assume in _compare that the mutex is already acquired
    NpyString_acquire_allocator(descr);
    int ret = _compare(a, b, descr, descr);
    NpyString_release_allocator(descr->allocator);
    return ret;
}

int
_compare(void *a, void *b, PyArray_StringDTypeObject *descr_a,
         PyArray_StringDTypeObject *descr_b)
{
    npy_string_allocator *allocator_a = descr_a->allocator;
    npy_string_allocator *allocator_b = descr_b->allocator;
    // descr_a and descr_b are either the same object or objects
    // that are equal, so we can safely refer only to descr_a.
    // This is enforced in the resolve_descriptors for comparisons
    //
    // Note that even though the default_string isn't checked in comparisons,
    // it will still be the same for both descrs because the value of
    // default_string is always the empty string unless na_object is a string.
    int has_null = descr_a->na_object != NULL;
    int has_string_na = descr_a->has_string_na;
    int has_nan_na = descr_a->has_nan_na;
    npy_static_string *default_string = &descr_a->default_string;
    const npy_packed_static_string *ps_a = (npy_packed_static_string *)a;
    npy_static_string s_a = {0, NULL};
    int a_is_null = NpyString_load(allocator_a, ps_a, &s_a);
    const npy_packed_static_string *ps_b = (npy_packed_static_string *)b;
    npy_static_string s_b = {0, NULL};
    int b_is_null = NpyString_load(allocator_b, ps_b, &s_b);
    if (NPY_UNLIKELY(a_is_null == -1 || b_is_null == -1)) {
        char *msg = "Failed to load string in string comparison";
        npy_gil_error(PyExc_MemoryError, msg);
        return 0;
    }
    else if (NPY_UNLIKELY(a_is_null || b_is_null)) {
        if (has_null && !has_string_na) {
            if (has_nan_na) {
                if (a_is_null) {
                    return 1;
                }
                else if (b_is_null) {
                    return -1;
                }
            }
            else {
                npy_gil_error(
                        PyExc_ValueError,
                        "Cannot compare null that is not a nan-like value");
                return 0;
            }
        }
        else {
            if (a_is_null) {
                s_a = *default_string;
            }
            if (b_is_null) {
                s_b = *default_string;
            }
        }
    }
    return NpyString_cmp(&s_a, &s_b);
}

// PyArray_ArgFunc
// The max element is the one with the highest unicode code point.
int
argmax(char *data, npy_intp n, npy_intp *max_ind, void *arr)
{
    PyArray_Descr *descr = PyArray_DESCR(arr);
    npy_intp elsize = descr->elsize;
    *max_ind = 0;
    for (int i = 1; i < n; i++) {
        if (compare(data + i * elsize, data + (*max_ind) * elsize, arr) > 0) {
            *max_ind = i;
        }
    }
    return 0;
}

// PyArray_ArgFunc
// The min element is the one with the lowest unicode code point.
int
argmin(char *data, npy_intp n, npy_intp *min_ind, void *arr)
{
    PyArray_Descr *descr = PyArray_DESCR(arr);
    npy_intp elsize = descr->elsize;
    *min_ind = 0;
    for (int i = 1; i < n; i++) {
        if (compare(data + i * elsize, data + (*min_ind) * elsize, arr) < 0) {
            *min_ind = i;
        }
    }
    return 0;
}

static PyArray_StringDTypeObject *
stringdtype_ensure_canonical(PyArray_StringDTypeObject *self)
{
    Py_INCREF(self);
    return self;
}

static int
stringdtype_clear_loop(void *NPY_UNUSED(traverse_context),
                       PyArray_Descr *descr, char *data, npy_intp size,
                       npy_intp stride, NpyAuxData *NPY_UNUSED(auxdata))
{
    PyArray_StringDTypeObject *sdescr = (PyArray_StringDTypeObject *)descr;
    npy_string_allocator *allocator = NpyString_acquire_allocator(sdescr);
    while (size--) {
        npy_packed_static_string *sdata = (npy_packed_static_string *)data;
        if (data != NULL && NpyString_free(sdata, allocator) < 0) {
            npy_gil_error(PyExc_MemoryError,
                      "String deallocation failed in clear loop");
            goto fail;
        }
        data += stride;
    }
    NpyString_release_allocator(allocator);
    return 0;

fail:
    NpyString_release_allocator(allocator);
    return -1;
}

static int
stringdtype_get_clear_loop(void *NPY_UNUSED(traverse_context),
                           PyArray_Descr *NPY_UNUSED(descr),
                           int NPY_UNUSED(aligned),
                           npy_intp NPY_UNUSED(fixed_stride),
                           PyArrayMethod_TraverseLoop **out_loop,
                           NpyAuxData **NPY_UNUSED(out_auxdata),
                           NPY_ARRAYMETHOD_FLAGS *flags)
{
    *flags = NPY_METH_NO_FLOATINGPOINT_ERRORS;
    *out_loop = &stringdtype_clear_loop;
    return 0;
}

static int
stringdtype_is_known_scalar_type(PyArray_DTypeMeta *NPY_UNUSED(cls),
                                 PyTypeObject *pytype)
{
    if (pytype == &PyFloat_Type) {
        return 1;
    }
    if (pytype == &PyLong_Type) {
        return 1;
    }
    if (pytype == &PyBool_Type) {
        return 1;
    }
    if (pytype == &PyComplex_Type) {
        return 1;
    }
    if (pytype == &PyUnicode_Type) {
        return 1;
    }
    if (pytype == &PyBytes_Type) {
        return 1;
    }
    if (pytype == &PyBoolArrType_Type) {
        return 1;
    }
    if (pytype == &PyByteArrType_Type) {
        return 1;
    }
    if (pytype == &PyShortArrType_Type) {
        return 1;
    }
    if (pytype == &PyIntArrType_Type) {
        return 1;
    }
    if (pytype == &PyLongArrType_Type) {
        return 1;
    }
    if (pytype == &PyLongLongArrType_Type) {
        return 1;
    }
    if (pytype == &PyUByteArrType_Type) {
        return 1;
    }
    if (pytype == &PyUShortArrType_Type) {
        return 1;
    }
    if (pytype == &PyUIntArrType_Type) {
        return 1;
    }
    if (pytype == &PyULongArrType_Type) {
        return 1;
    }
    if (pytype == &PyULongLongArrType_Type) {
        return 1;
    }
    if (pytype == &PyHalfArrType_Type) {
        return 1;
    }
    if (pytype == &PyFloatArrType_Type) {
        return 1;
    }
    if (pytype == &PyDoubleArrType_Type) {
        return 1;
    }
    if (pytype == &PyLongDoubleArrType_Type) {
        return 1;
    }
    if (pytype == &PyCFloatArrType_Type) {
        return 1;
    }
    if (pytype == &PyCDoubleArrType_Type) {
        return 1;
    }
    if (pytype == &PyCLongDoubleArrType_Type) {
        return 1;
    }
    if (pytype == &PyIntpArrType_Type) {
        return 1;
    }
    if (pytype == &PyUIntpArrType_Type) {
        return 1;
    }
    if (pytype == &PyDatetimeArrType_Type) {
        return 1;
    }
    return 0;
}

PyArray_Descr *
stringdtype_finalize_descr(PyArray_Descr *dtype)
{
    PyArray_StringDTypeObject *sdtype = (PyArray_StringDTypeObject *)dtype;
    if (sdtype->array_owned == 0) {
        sdtype->array_owned = 1;
        Py_INCREF(dtype);
        return dtype;
    }
    PyArray_StringDTypeObject *ret = (PyArray_StringDTypeObject *)new_stringdtype_instance(
            sdtype->na_object, sdtype->coerce, sdtype->allocator);
    if (ret->array_owned == 0) {
        ret->array_owned = 1;
    }
    return (PyArray_Descr *)ret;
}

static PyType_Slot PyArray_StringDType_Slots[] = {
        {NPY_DT_common_instance, &common_instance},
        {NPY_DT_common_dtype, &common_dtype},
        {NPY_DT_discover_descr_from_pyobject,
         &string_discover_descriptor_from_pyobject},
        {NPY_DT_setitem, &stringdtype_setitem},
        {NPY_DT_getitem, &stringdtype_getitem},
        {NPY_DT_ensure_canonical, &stringdtype_ensure_canonical},
        {NPY_DT_PyArray_ArrFuncs_nonzero, &nonzero},
        {NPY_DT_PyArray_ArrFuncs_compare, &compare},
        {NPY_DT_PyArray_ArrFuncs_argmax, &argmax},
        {NPY_DT_PyArray_ArrFuncs_argmin, &argmin},
        {NPY_DT_get_clear_loop, &stringdtype_get_clear_loop},
        {NPY_DT_finalize_descr, &stringdtype_finalize_descr},
        {_NPY_DT_is_known_scalar_type, &stringdtype_is_known_scalar_type},
        {0, NULL}};


static PyObject *
stringdtype_new(PyTypeObject *NPY_UNUSED(cls), PyObject *args, PyObject *kwds)
{
    static char *kwargs_strs[] = {"", "coerce", "na_object", NULL};

    long size = 0;
    PyObject *na_object = NULL;
    int coerce = 1;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|l$pO&:StringDType",
                                     kwargs_strs, &size, &coerce,
                                     _not_NoValue, &na_object)) {
        return NULL;
    }

    return new_stringdtype_instance(na_object, coerce, NULL);
}

static void
stringdtype_dealloc(PyArray_StringDTypeObject *self)
{
    Py_XDECREF(self->na_object);
    // this can be null if an error happens while initializing an instance
    if (self->allocator != NULL && self->array_owned != 2) {
        NpyString_free_allocator(self->allocator);
    }
    PyMem_RawFree((char *)self->na_name.buf);
    PyMem_RawFree((char *)self->default_string.buf);
    PyArrayDescr_Type.tp_dealloc((PyObject *)self);
}

static PyObject *
stringdtype_repr(PyArray_StringDTypeObject *self)
{
    PyObject *ret = NULL;
    // borrow reference
    PyObject *na_object = self->na_object;
    int coerce = self->coerce;

    if (na_object != NULL && coerce == 0) {
        ret = PyUnicode_FromFormat("StringDType(na_object=%R, coerce=False)",
                                   na_object);
    }
    else if (na_object != NULL) {
        ret = PyUnicode_FromFormat("StringDType(na_object=%R)", na_object);
    }
    else if (coerce == 0) {
        ret = PyUnicode_FromFormat("StringDType(coerce=False)", coerce);
    }
    else {
        ret = PyUnicode_FromString("StringDType()");
    }

    return ret;
}

static PyObject *_convert_to_stringdtype_kwargs = NULL;

// implementation of __reduce__ magic method to reconstruct a StringDType
// object from the serialized data in the pickle. Uses the python
// _convert_to_stringdtype_kwargs for convenience because this isn't
// performance-critical
static PyObject *
stringdtype__reduce__(PyArray_StringDTypeObject *self, PyObject *NPY_UNUSED(args))
{
    npy_cache_import("numpy._core._internal", "_convert_to_stringdtype_kwargs",
                     &_convert_to_stringdtype_kwargs);

    if (_convert_to_stringdtype_kwargs == NULL) {
        return NULL;
    }

    if (self->na_object != NULL) {
        return Py_BuildValue("O(iO)", _convert_to_stringdtype_kwargs,
                             self->coerce, self->na_object);
    }

    return Py_BuildValue("O(i)", _convert_to_stringdtype_kwargs, self->coerce);
}

static PyMethodDef PyArray_StringDType_methods[] = {
        {
                "__reduce__",
                (PyCFunction)stringdtype__reduce__,
                METH_NOARGS,
                "Reduction method for a StringDType object",
        },
        {NULL, NULL, 0, NULL},
};

static PyMemberDef PyArray_StringDType_members[] = {
        {"na_object", T_OBJECT_EX, offsetof(PyArray_StringDTypeObject, na_object),
         READONLY,
         "The missing value object associated with the dtype instance"},
        {"coerce", T_BOOL, offsetof(PyArray_StringDTypeObject, coerce), READONLY,
         "Controls hether non-string values should be coerced to string"},
        {NULL, 0, 0, 0, NULL},
};

static PyObject *
PyArray_StringDType_richcompare(PyObject *self, PyObject *other, int op)
{
    if (((op != Py_EQ) && (op != Py_NE)) ||
        (Py_TYPE(other) != Py_TYPE(self))) {
        Py_INCREF(Py_NotImplemented);
        return Py_NotImplemented;
    }

    // we know both are instances of PyArray_StringDType so this is safe
    PyArray_StringDTypeObject *sself = (PyArray_StringDTypeObject *)self;
    PyArray_StringDTypeObject *sother = (PyArray_StringDTypeObject *)other;

    int eq = _eq_comparison(sself->coerce, sother->coerce, sself->na_object,
                            sother->na_object);

    if (eq == -1) {
        return NULL;
    }

    if ((op == Py_EQ && eq) || (op == Py_NE && !eq)) {
        Py_INCREF(Py_True);
        return Py_True;
    }
    Py_INCREF(Py_False);
    return Py_False;
}

static Py_hash_t
PyArray_StringDType_hash(PyObject *self)
{
    PyArray_StringDTypeObject *sself = (PyArray_StringDTypeObject *)self;
    PyObject *hash_tup = NULL;
    if (sself->na_object != NULL) {
        hash_tup = Py_BuildValue("(iO)", sself->coerce, sself->na_object);
    }
    else {
        hash_tup = Py_BuildValue("(i)", sself->coerce);
    }

    Py_hash_t ret = PyObject_Hash(hash_tup);
    Py_DECREF(hash_tup);
    return ret;
}

/*
 * This is the basic things that you need to create a Python Type/Class in C.
 * However, there is a slight difference here because we create a
 * PyArray_DTypeMeta, which is a larger struct than a typical type.
 * (This should get a bit nicer eventually with Python >3.11.)
 */
PyArray_DTypeMeta PyArray_StringDType = {
        {{
                PyVarObject_HEAD_INIT(NULL, 0).tp_name =
                        "numpy.dtypes.StringDType",
                .tp_basicsize = sizeof(PyArray_StringDTypeObject),
                .tp_new = stringdtype_new,
                .tp_dealloc = (destructor)stringdtype_dealloc,
                .tp_repr = (reprfunc)stringdtype_repr,
                .tp_str = (reprfunc)stringdtype_repr,
                .tp_methods = PyArray_StringDType_methods,
                .tp_members = PyArray_StringDType_members,
                .tp_richcompare = PyArray_StringDType_richcompare,
                .tp_hash = PyArray_StringDType_hash,
        }},
        /* rest, filled in during DTypeMeta initialization */
};

NPY_NO_EXPORT int
init_string_dtype(void)
{
    PyArrayMethod_Spec **PyArray_StringDType_casts = get_casts();

    PyArrayDTypeMeta_Spec PyArray_StringDType_DTypeSpec = {
            .flags = NPY_DT_PARAMETRIC,
            .typeobj = &PyUnicode_Type,
            .slots = PyArray_StringDType_Slots,
            .casts = PyArray_StringDType_casts,
    };

    /* Loaded dynamically, so needs to be set here: */
    ((PyObject *)&PyArray_StringDType)->ob_type = &PyArrayDTypeMeta_Type;
    ((PyTypeObject *)&PyArray_StringDType)->tp_base = &PyArrayDescr_Type;
    if (PyType_Ready((PyTypeObject *)&PyArray_StringDType) < 0) {
        return -1;
    }

    if (dtypemeta_initialize_struct_from_spec(
                &PyArray_StringDType, &PyArray_StringDType_DTypeSpec, 1) < 0) {
        return -1;
    }

    PyArray_Descr *singleton =
            NPY_DT_CALL_default_descr(&PyArray_StringDType);

    if (singleton == NULL) {
        return -1;
    }

    PyArray_StringDType.singleton = singleton;
    PyArray_StringDType.type_num = NPY_VSTRING;

    for (int i = 0; PyArray_StringDType_casts[i] != NULL; i++) {
        PyMem_Free(PyArray_StringDType_casts[i]->dtypes);
        PyMem_Free(PyArray_StringDType_casts[i]);
    }

    PyMem_Free(PyArray_StringDType_casts);

    return 0;
}

int
free_and_copy(npy_string_allocator *in_allocator,
              npy_string_allocator *out_allocator,
              const npy_packed_static_string *in,
              npy_packed_static_string *out, const char *location)
{
    if (NpyString_free(out, out_allocator) < 0) {
        npy_gil_error(PyExc_MemoryError, "Failed to deallocate string in %s", location);
        return -1;
    }
    if (NpyString_dup(in, out, in_allocator, out_allocator) < 0) {
        npy_gil_error(PyExc_MemoryError, "Failed to allocate string in %s", location);
        return -1;
    }
    return 0;
}
/*
 * A useful pattern is to define a stack-allocated npy_static_string instance
 * initialized to {0, NULL} and pass a pointer to the stack-allocated unpacked
 * string to this function to fill out with the contents of the newly allocated
 * string.
 */
NPY_NO_EXPORT int
load_new_string(npy_packed_static_string *out, npy_static_string *out_ss,
                size_t num_bytes, npy_string_allocator *allocator,
                const char *err_context)
{
    npy_packed_static_string *out_pss = (npy_packed_static_string *)out;
    if (NpyString_free(out_pss, allocator) < 0) {
        npy_gil_error(PyExc_MemoryError,
                      "Failed to deallocate string in %s", err_context);
        return -1;
    }
    if (NpyString_newemptysize(num_bytes, out_pss, allocator) < 0) {
        npy_gil_error(PyExc_MemoryError,
                      "Failed to allocate string in %s", err_context);
        return -1;
    }
    int is_null = NpyString_load(allocator, out_pss, out_ss);
    if (is_null == -1) {
        npy_gil_error(PyExc_MemoryError,
                      "Failed to load string in %s", err_context);
        return -1;
    }
    return 0;
}
