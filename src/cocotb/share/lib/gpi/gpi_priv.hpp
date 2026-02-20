// Copyright cocotb contributors
// Copyright (c) 2013, 2018 Potential Ventures Ltd
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef COCOTB_GPI_PRIV_H_
#define COCOTB_GPI_PRIV_H_

#include <exports.h>
#include <gpi.h>

#include <string>

#include "../utils.hpp"  // xstr

#ifdef GPI_EXPORTS
#define GPI_EXPORT COCOTB_EXPORT
#else
#define GPI_EXPORT COCOTB_IMPORT
#endif

// Callback kind
// Bottom byte is basic kind
// Upper bits are for flags
typedef uint16_t cb_kind;
enum cb_kind_ : uint16_t {
    CB_TIMED,
    CB_VALUE_CHANGE,
    CB_READONLY,
    CB_NEXT_TIMESTEP,
    CB_READWRITE,
    CB_STARTUP,
    CB_SHUTDOWN,

    CB_PARENT = 1 << 8,
};

#define CB_BASIC_KIND 0xFF

GPI_EXPORT const char *cb_kind_to_string(cb_kind kind);

// handle/object metadata
// Bit 0 is valid bit for tracking if callback object is acquired or released
// The rest are generation for handle validity
typedef uint32_t meta32;
#define GEN_INCR (1 << 1)
#define META_GEN 0xFFFFFFFE
#define META_VALID 0x1U

class GpiCbHdl;
class GpiImplInterface;
class GpiIterator;
class GpiSignalObjHdl;

struct gpi_callback {
    // Callback kind
    cb_kind kind;

    // Removed (valid callback but not to be processed / called)
    bool removed;

    // Metadata (generation, validity)
    meta32 meta;

    // GPI implementation
    GpiImplInterface *impl;

    // Simulator callback object handle
    // Null when this is a child callback
    void *sim_cb_hdl;

    // Pointers to manage parent and child (user) callbacks
    gpi_callback *parent;
    gpi_callback *next;
    gpi_callback *prev;
    gpi_callback *last;

    // User callback
    int32_t (*user_cb_func)(void *);
    void *user_cb_data;

    // ValueChange callbacks
    GpiSignalObjHdl *signal;
    gpi_edge edge;

    // Index in the callback pool
    int32_t index;
};

/* Base GPI class others are derived from */
class GPI_EXPORT GpiHdl {
  public:
    GpiHdl(GpiImplInterface *impl, void *hdl = NULL)
        : m_impl(impl), m_obj_hdl(hdl) {}
    virtual ~GpiHdl() = default;

    template <typename T>
    T get_handle() const {
        return static_cast<T>(m_obj_hdl);
    }

  private:
    GpiHdl() {}  // Disable default constructor

  public:
    GpiImplInterface *m_impl;                   // VPI/VHPI/FLI routines
    bool is_this_impl(GpiImplInterface *impl);  // Is the passed interface the
                                                // one this object uses?

  protected:
    void *m_obj_hdl;
};

/* GPI object handle, maps to a simulation object */
// An object is any item in the hierarchy
// Provides methods for iterating through children or finding by name
// Initial object is returned by call to GpiImplInterface::get_root_handle()
// Subsequent operations to get children go through this handle.
// GpiObjHdl::get_handle_by_name/get_handle_by_index are really factories
// that construct an object derived from GpiSignalObjHdl or GpiObjHdl
class GPI_EXPORT GpiObjHdl : public GpiHdl {
  public:
    GpiObjHdl(GpiImplInterface *impl, void *hdl = nullptr,
              gpi_objtype objtype = GPI_UNKNOWN, bool is_const = false)
        : GpiHdl(impl, hdl), m_type(objtype), m_const(is_const) {}

    virtual ~GpiObjHdl() = default;

    // TODO why do these even exist? Just return the string by ref and call
    // c_str.
    virtual const char *get_name_str();
    virtual const char *get_fullname_str();
    virtual const char *get_type_str();
    gpi_objtype get_type() { return m_type; };
    bool get_const() { return m_const; };
    int get_num_elems() { return m_num_elems; }
    int get_range_left() { return m_range_left; }
    int get_range_right() { return m_range_right; }
    gpi_range_dir get_range_dir() { return m_range_dir; }
    int get_indexable() { return m_indexable; }
    virtual int get_signed() { return -1; }

    const std::string &get_name();
    const std::string &get_fullname();

    virtual const char *get_definition_name() {
        return m_definition_name.c_str();
    };
    virtual const char *get_definition_file() {
        return m_definition_file.c_str();
    };

    bool is_native_impl(GpiImplInterface *impl);
    virtual int initialise(const std::string &name,
                           const std::string &full_name);

  protected:
    int m_num_elems = 0;
    bool m_indexable = false;
    int m_range_left = -1;
    int m_range_right = -1;
    gpi_range_dir m_range_dir = GPI_RANGE_NO_DIR;
    std::string m_name = "unknown";
    std::string m_fullname = "unknown";

    std::string m_definition_name;
    std::string m_definition_file;

    gpi_objtype m_type;
    bool m_const;
};

/* GPI Signal object handle, maps to a simulation object */
//
// Identical to an object but adds additional methods for getting/setting the
// value of the signal (which doesn't apply to non signal items in the hierarchy
class GPI_EXPORT GpiSignalObjHdl : public GpiObjHdl {
  public:
    using GpiObjHdl::GpiObjHdl;

    virtual ~GpiSignalObjHdl() = default;
    // Provide public access to the implementation (composition vs inheritance)
    virtual const char *get_signal_value_binstr() = 0;
    virtual const char *get_signal_value_str() = 0;
    virtual double get_signal_value_real() = 0;
    virtual long get_signal_value_long() = 0;

    int m_length = 0;

    virtual int set_signal_value(const int32_t value,
                                 gpi_set_action action) = 0;
    virtual int set_signal_value(const double value, gpi_set_action action) = 0;
    virtual int set_signal_value_str(std::string &value,
                                     gpi_set_action action) = 0;
    virtual int set_signal_value_binstr(std::string &value,
                                        gpi_set_action action) = 0;

    virtual gpi_hdl register_value_change_callback(gpi_edge edge,
                                                   int (*gpi_function)(void *),
                                                   void *gpi_cb_data) = 0;

    // Value change callback is associated with sim callback object.
    gpi_callback *m_valuechange_cb = nullptr;
};

// Obsolete GPI callback handle
//
// This is kept around for now, as PyGPI uses a shared Python object type
// that is templated on a GPI pointer type,
// one of which is a pointer to this type.
class GPI_EXPORT GpiCbHdl {};

class GPI_EXPORT GpiIterator : public GpiHdl {
  public:
    enum Status {
        NATIVE,          // Fully resolved object was created
        NATIVE_NO_NAME,  // Native object was found but unable to fully create
        NOT_NATIVE,      // Non-native object was found but we did get a name
        NOT_NATIVE_NO_NAME,  // Non-native object was found without a name
        END
    };

    GpiIterator(GpiImplInterface *impl, GpiObjHdl *hdl)
        : GpiHdl(impl), m_parent(hdl) {}
    virtual ~GpiIterator() = default;

    virtual Status next_handle(std::string &name, GpiObjHdl **hdl, void **) {
        name = "";
        *hdl = NULL;
        return GpiIterator::END;
    }

    GpiObjHdl *get_parent() { return m_parent; }

  protected:
    GpiObjHdl *m_parent;
};

class GPI_EXPORT GpiImplInterface {
  public:
    GpiImplInterface(const std::string &name) : m_name(name) {}
    const char *get_name_c();
    const std::string &get_name_s();
    virtual ~GpiImplInterface() = default;

    /* Sim related */
    virtual void sim_end() = 0;
    virtual void get_sim_time(uint32_t *high, uint32_t *low) = 0;
    virtual void get_sim_precision(int32_t *precision) = 0;
    virtual const char *get_simulator_product() = 0;
    virtual const char *get_simulator_version() = 0;
    virtual int get_simulator_args(int *argc, char const *const **argv) = 0;

    /* Hierarchy related */
    virtual GpiObjHdl *get_child_by_name(const std::string &name,
                                         GpiObjHdl *parent) = 0;
    virtual GpiObjHdl *get_child_by_index(int32_t index, GpiObjHdl *parent) = 0;
    virtual GpiObjHdl *get_child_from_handle(void *raw_hdl,
                                             GpiObjHdl *parent) = 0;
    virtual GpiObjHdl *get_root_handle(const char *name) = 0;
    virtual GpiIterator *iterate_handle(GpiObjHdl *obj_hdl,
                                        gpi_iterator_sel type) = 0;

    /* Callback related, these may (will) return the same handle */
    virtual gpi_hdl register_timed_callback(uint64_t time,
                                            int (*gpi_function)(void *),
                                            void *gpi_cb_data) = 0;
    virtual gpi_hdl register_readonly_callback(int (*gpi_function)(void *),
                                               void *gpi_cb_data) = 0;
    virtual gpi_hdl register_nexttime_callback(int (*gpi_function)(void *),
                                               void *gpi_cb_data) = 0;
    virtual gpi_hdl register_readwrite_callback(int (*gpi_function)(void *),
                                                void *gpi_cb_data) = 0;
    virtual int remove_callback(gpi_callback *cb) = 0;

  private:
    std::string m_name;
};

/** Register a GPI implementation.
 *
 * @param new_impl  Implementation to register.
 * @return          0 on success, -1 on failure.
 */
GPI_EXPORT int gpi_register_impl(GpiImplInterface *new_impl);

// GpiImpls are currently expected to register single callbacks with the
// interface for the start and end of simulation time. These functions are
// called by the GpiImpls. The GPI layer will do the callback muxing.
GPI_EXPORT void gpi_start_of_sim_time();
GPI_EXPORT void gpi_end_of_sim_time();

/** Acquire callback from the object pool, allocating if necessary.
 *
 * @return A callback object that is marked as valid.
 */
GPI_EXPORT gpi_callback *gpi_callback_acquire();

/** Release a callback to the object pool.
 *
 * The released callback will have its metadata updated
 * to be a different generation and be marked as invalid.
 *
 * @param cb Callback object to be released.
 */
GPI_EXPORT void gpi_callback_release(gpi_callback *cb);

/** Get a stable pointer from the index in the object pool.
 *
 * @param index  Index in the object pool.
 * @return       Stable callback pointer if index is in the pool, `NULL`
 *               otherwise.
 */
GPI_EXPORT gpi_callback *gpi_callback_from_index(int32_t index);

/** Cleanup value change callbacks.
 * This should be called before returning from each simulation callback.
 */
GPI_EXPORT void gpi_cleanup_valuechange_cbs();

/** Mark value change callback for cleanup. */
GPI_EXPORT void gpi_mark_valuechange_for_cleanup(gpi_callback *cb);

/** Audit callback pool to check for acquired and not released callbacks. */
void gpi_callback_audit();

/** Common GPI entry point, called from first implementation after it registers
 * itself. */
GPI_EXPORT void gpi_entry_point();

/** Check for cleanup and finalize before returning to simulator. */
GPI_EXPORT void gpi_check_cleanup();

/** Init GPI logging and debug */
GPI_EXPORT void gpi_init_logging_and_debug();

/** Shared library support - open library */
void *utils_dyn_open(const char *lib_name);

/** Shared library support - get exported symbol */
void *utils_dyn_sym(void *handle, const char *sym_name);

/** Common handler for value change callbacks */
GPI_EXPORT int32_t handle_value_change_callback(gpi_callback *parent_cb);

/* Trace log helpers */

#define GPI_TO_USER_CB(impl_str) \
    LOG_TRACE("[ %s ] => User Callback", (const char *)impl_str)

#define USER_CB_TO_GPI(impl_str) \
    LOG_TRACE("User Callback => [ %s ]", (const char *)impl_str)

#define SIM_TO_GPI(impl_str, cb_reason) \
    LOG_TRACE("Sim => [ %s for %s ]", (const char *)impl_str, cb_reason)

#define GPI_TO_SIM(impl_str)                                \
    do {                                                    \
        gpi_check_cleanup();                                \
        LOG_TRACE("[ %s ] => Sim", (const char *)impl_str); \
    } while (0)

/* Implementation entry points for use with GPI_EXTRA */
typedef void (*layer_entry_func)();

/* Use this macro in an implementation layer to define an entry point */
#define GPI_ENTRY_POINT(NAME, func)                     \
    extern "C" {                                        \
    COCOTB_EXPORT void NAME##_entry_point() { func(); } \
    }

#endif /* COCOTB_GPI_PRIV_H_ */
