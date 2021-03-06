

/* SWIG interface for rapi.h */

/******* Language-independent exceptions *******/
//%include "exception.i"
//SWIG_exception(SWIG_MemoryError, "Not enough memory");
// LP: I can't get SWIG_exception to work. It generates a "goto fail;"
// statement without defining the "fail" label, thus resulting in a
// compiler error.  I've tried this with SWIG 2.0.11 and 3.0.2.
// This leaves me writing one interface per target language.

/******* swig -builtin option *******
PyErr_SetString wouldn't raise a Python exception until I
started calling swig with the "-builtin" option.

*************************************/


%module rapi

%{
/* Includes the header in the wrapper code */
#include "rapi.h"
%}

%header %{

#include <stddef.h>
#include <stdio.h>

#define PDEBUG(...) { fprintf(stderr, "%s(%d): ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); }

/* Helper to convert RAPI errors to Python errors */
PyObject* rapi_py_error_type(int rapi_code) {
  PyObject* type = 0;
  switch(rapi_code) {
  case RAPI_GENERIC_ERROR:
    type = PyExc_RuntimeError;
    break;
  case RAPI_OP_NOT_SUPPORTED_ERROR:
    type = PyExc_NotImplementedError;
    break;
  case RAPI_MEMORY_ERROR:
    type = PyExc_MemoryError;
    break;
  case RAPI_PARAM_ERROR:
    type = PyExc_ValueError;
    break;
  case RAPI_TYPE_ERROR:
    type = PyExc_TypeError;
    break;
  default:
    type = PyExc_RuntimeError;
    break;
  };
  return type;
}

void* rapi_malloc(size_t nbytes) {
  void* result = malloc(nbytes);
  if (!result)
    PyErr_SetString(PyExc_MemoryError, "Failed to allocate memory");

  return result;
}

%}

/*
Remove rapi_ prefix from function names.
Make sure this general rename comes before the ignore clauses below
or they won't have effect.
*/
%rename("%(strip:[rapi_])s") ""; // e.g., rapi_load_ref -> load_ref

// ignore functions from klib
%rename("$ignore", regextarget=1) "^k";
%rename("$ignore", regextarget=1) "^K";

// also ignore anything that starts with an underscore
%rename("$ignore", regextarget=1) "^_";

%include "stdint.i"; // needed to tell swig about types such as uint32_t, uint8_t, etc.
// This %includes are needed since we have some structure elements that are kvec_t
%include "kvec.h";

%inline %{
  /* rewrite rapi_init to launch an exception instead of returning an error */
  void init(const rapi_opts* opts) {
    int error = rapi_init(opts);
    if (error != RAPI_NO_ERROR)
      PyErr_SetString(rapi_py_error_type(error), "Error initializing library");
  }
%};

#define QENC_SANGER   33
#define QENC_ILLUMINA 64

/*
These are wrapped automatically by SWIG -- the wrapper doesn't try to free the
strings since they are "const".
*/
const char* rapi_aligner_name();
const char* rapi_aligner_version();

/**** rapi_param ****/
typedef struct {
  uint8_t type;
  union {
    char character;
    char* text; // if set, type should be set to TEXT and the str will be freed by rapi_param_clear
    long integer;
    double real;
  } value;
} rapi_param;


%extend rapi_param {
  const char* name; // kstring_t accessed as a const char*

  rapi_param() {
    rapi_param* param = (rapi_param*) rapi_malloc(sizeof(rapi_param));
    if (!param) return NULL;
    rapi_param_init(param);
    return param;
  }

  ~rapi_param() {
    rapi_param_free($self);
  }
};

%{
const char* rapi_param_name_get(const rapi_param *p) {
  return rapi_param_get_name(p);
}

void rapi_param_name_set(rapi_param *p, const char *val) {
  rapi_param_set_name(p, val);
}
%}

/********* rapi_opts *******/
typedef struct {
  int ignore_unsupported;
  /* Standard Ones - Differently implemented by aligners*/
  int mapq_min;
  int isize_min;
  int isize_max;
  /* Mismatch / Gap_Opens / Quality Trims --> Generalize ? */

  // TODO: how to wrap this thing?
  kvec_t(rapi_param) parameters;
} rapi_opts;


%extend rapi_opts {
  rapi_opts() {
    rapi_opts* opts = (rapi_opts*) rapi_malloc(sizeof(rapi_opts));
    if (!opts) return NULL;

    int error = rapi_opts_init(opts);
    if (error == RAPI_NO_ERROR)
      return opts;
    else {
      free(opts);
      PyErr_SetString(rapi_py_error_type(error), "Not enough memory to initialize rapi_opts");
      return NULL;
    }
  }

  ~rapi_opts() {
    int error = rapi_opts_free($self);
    if (error != RAPI_NO_ERROR) {
      PDEBUG("Problem destroying opts (error code %d)\n", error);
      // TODO: should we raise exceptions in case of errors when freeing/destroying?
    }
  }
};

/****** rapi_contig and rapi_ref *******/

%immutable; /** don't let the user modify the reference through the wrapper */

typedef struct {
  char * name;
  uint32_t len;
  char * assembly_identifier;
  char * species;
  char * uri;
  char * md5;
} rapi_contig;

%inline %{
typedef struct {
  rapi_contig* next_item;
  size_t n_left;
} ref_contig_iter;
%}

/* Without these %feature statements the __len__ and __getitem__ methods
   will be defined but they won't work (at least when calling swig with -builtin).
   You'll get errors like:

----> len(r)
TypeError: object of type 'ref' has no len()
*/
%feature("python:slot", "tp_iter", functype="getiterfunc") ref_contig_iter::rapi___iter__;
%feature("python:slot", "tp_iternext", functype="iternextfunc") ref_contig_iter::next;
%extend ref_contig_iter {
  ref_contig_iter(rapi_contig* array, size_t len) {
    ref_contig_iter* iter = (ref_contig_iter*) rapi_malloc(sizeof(ref_contig_iter));
    if (!iter) return NULL;

    iter->next_item = array;
    iter->n_left = len;
    return iter;
  }

  ~ref_contig_iter() {
    free($self);
  }

  ref_contig_iter* rapi___iter__() { return $self; }

  rapi_contig* next() {
    if ($self->n_left > 0) {
      $self->n_left -= 1;
      return $self->next_item++;
    }
    else {
      PyErr_SetString(PyExc_StopIteration, "");
      return NULL;
    }
  }
};

/*
We're wrapping rapi_ref as a container of contigs.  In other words, the rapi_contig
elements are accessed by indexing into the rapi_ref.  Though I'd prefer it, I haven't
figured out how to expose the contigs attribute as an array of structures, supporting
usage like:

    > ref = rapi.ref('some path')
    > c1 = ref.contigs[0]
*/
%feature("python:slot", "sq_length", functype="lenfunc") rapi_ref::rapi___len__;
%feature("python:slot", "mp_subscript", functype="binaryfunc") rapi_ref::rapi___getitem__;
typedef struct {
  char * path;
  int n_contigs;
} rapi_ref;

%feature("python:slot", "tp_iter", functype="getiterfunc") rapi_ref::rapi___iter__;
%extend rapi_ref {
  rapi_ref(const char* reference_path) {
    rapi_ref* ref = (rapi_ref*) rapi_malloc(sizeof(rapi_ref));
    if (!ref) return NULL;

    int error = rapi_ref_load(reference_path, ref);
    if (error == RAPI_NO_ERROR)
      return ref;
    else {
      free(ref);
      const char* msg;
      if (error == RAPI_MEMORY_ERROR)
        msg = "Insufficient memory available to load the reference.";
      else if (error == RAPI_GENERIC_ERROR)
        msg = "Library failed to load reference. Check paths and reference format.";
      else
        msg = "";

      PyErr_SetString(rapi_py_error_type(error), msg);
      return NULL;
    }
  }

  void unload() {
    int error = rapi_ref_free($self);
    if (error != RAPI_NO_ERROR) {
      PDEBUG("Problem destroying reference (error code %d)\n", error);
    }
  }

  ~rapi_ref() {
    rapi_ref_unload($self);
  }

  size_t rapi___len__() { return $self->n_contigs; }

  /* XXX: I worry about memory management here.  We're returning a pointer to the rapi_ref's
    chunk of memory.  I don't think there's anything preventing the interpreter from deciding
    to free the underlying memory and making everything blow up.
  */
  rapi_contig* rapi_get_contig(size_t i) {
    if (i >= $self->n_contigs) {
      PyErr_SetString(PyExc_IndexError, "index out of bounds");
      return NULL;
    }
    else
      return $self->contigs + i;
  }

  rapi_contig* rapi___getitem__(size_t i) { return rapi_ref_rapi_get_contig($self, i); }

  ref_contig_iter* rapi___iter__() { return new_ref_contig_iter($self->contigs, $self->n_contigs); }
};

/****** rapi_batch and rapi_reads *******/

%feature("python:slot", "sq_length", functype="lenfunc") rapi_read::rapi___len__;
typedef struct {
  char * id;
  char * seq;
  char * qual;
  unsigned int length;
  rapi_alignment* alignments;
  uint8_t n_alignments;
} rapi_read;

%extend rapi_read {
  size_t rapi___len__() { return $self->length; }
};


//%rename("batch") "rapi_batch_wrap"; // how do I make this rename work properly?

%feature("python:slot", "sq_length", functype="lenfunc") rapi_batch_wrap::rapi___len__;
%inline %{
  typedef struct {
    rapi_batch* batch;
    size_t len; // number of reads inserted in batch (as opposed to the space reserved)
  } rapi_batch_wrap;
%}

%extend rapi_batch_wrap {
  rapi_batch_wrap(int n_reads_per_frag) {
    if (n_reads_per_frag <= 0) {
      PyErr_SetString(PyExc_ValueError, "number of reads per fragment must be greater than or equal to 0");
      return NULL;
    }

    rapi_batch_wrap* wrapper = (rapi_batch_wrap*) rapi_malloc(sizeof(rapi_batch_wrap));
    if (!wrapper) return NULL;

    wrapper->batch = (rapi_batch*) rapi_malloc(sizeof(rapi_batch));
    if (!wrapper->batch) {
      free(wrapper);
      return NULL;
    }

    wrapper->len = 0;

    int error = rapi_reads_alloc(wrapper->batch, n_reads_per_frag, 0); // zero-sized allocation to initialize

    if (error != RAPI_NO_ERROR) {
      free(wrapper->batch);
      free(wrapper);
      PyErr_SetString(rapi_py_error_type(error), "Error allocating space for reads");
      return NULL;
    }
    else
      return wrapper;
  }

  ~rapi_batch_wrap() {
    int error = rapi_reads_free($self->batch);
    free($self);
    if (error != RAPI_NO_ERROR) {
      PDEBUG("Problem destroying read batch (error code %d)\n", error);
      // TODO: should we raise exceptions in case of errors when freeing/destroying?
    }
  }

  /** Number of reads inserted in batch (as opposed to the space reserved).
   *  This is actually index + 1 of the "forward-most" read to have been inserted.
   */
  int rapi___len__() { return $self->len; }

  /** Number of reads for which we have allocated memory. */
  int capacity() { return $self->batch->n_frags * $self->batch->n_reads_frag; }

  void reserve(int n_reads) {
    if (n_reads < 0)
      PyErr_SetString(rapi_py_error_type(RAPI_PARAM_ERROR), "number of reads to reserve must be >= 0");

    int n_fragments = n_reads / $self->batch->n_reads_frag;
    // If the reads don't fit completely in n_fragments, add one more
    if (n_reads % $self->batch->n_reads_frag != 0)
      n_fragments +=  1;

    int error = rapi_reads_reserve($self->batch, n_fragments);

    if (error != RAPI_NO_ERROR)
      PyErr_SetString(rapi_py_error_type(error), "Failed to reserve space");
    else {
      for (int i = 0; i < $self->len; ++i) {
        PDEBUG("i: %d; n_alignments: %d\n", i, $self->batch->reads[i].n_alignments);
      }
    }
  }

  void append(const char* id, const char* seq, const char* qual, int q_offset) {
    int fragment_num = $self->len / $self->batch->n_reads_frag;
    int read_num = $self->len % $self->batch->n_reads_frag;

    size_t read_capacity = rapi_batch_wrap_capacity($self);
    if (read_capacity < $self->len + 1) {
      // grow space
      size_t new_capacity = read_capacity > 0 ? read_capacity * 2 : 2;
      rapi_batch_wrap_reserve($self, new_capacity);
      // We let the reserve function run.  Though it doesn't have a return value,
      // it will set exceptions if it has problems.  So, we re-check whethet our
      // capacity is sufficient (meaning that it has been incremented).  If it
      // hasn't we return without appending the read and let the system raise
      // the exception set by rapi_batch_wrap_reserve()
      if (rapi_batch_wrap_capacity($self) < $self->len + 1) {
        return;
      }
    }
    PDEBUG("Setting read %d of fragment %d\n", read_num, fragment_num);
    int error = rapi_set_read($self->batch, fragment_num, read_num, id, seq, qual, q_offset);
    if (error != RAPI_NO_ERROR) {
      PyErr_SetString(rapi_py_error_type(error), "Error inserting read.");
    }
    else
      ++$self->len;
  }

  void set_read(int n_frag, int n_read, const char* id, const char* seq, const char* qual, int q_offset) {
    if (n_frag < 0 || n_read < 0) {
      PyErr_SetString(rapi_py_error_type(RAPI_PARAM_ERROR), "read and fragment indices cannot be negative");
      return;
    }

    int error = rapi_set_read($self->batch, n_frag, n_read, id, seq, qual, q_offset);
    if (error != RAPI_NO_ERROR) {
      PyErr_SetString(rapi_py_error_type(error), "Error setting read data");
    } else {
      // If the user set a read that is beyond the current batch length, reset the
      // batch length to the new limit.
      int index = n_frag * $self->batch->n_reads_frag + n_read;
      if ($self->len < index)
        $self->len = index + 1;
    }
  }

  rapi_read* get_read(int n_fragment, int n_read) {
    //return rapi_get_read($self->batch, n_fragment, n_read);
    rapi_read* r = rapi_get_read($self->batch, n_fragment, n_read);
    PDEBUG("number of alignments in this read: %d\n", r->n_alignments);
    return r;
  }
}


%mutable;


// vim: set et sw=2 ts=2
