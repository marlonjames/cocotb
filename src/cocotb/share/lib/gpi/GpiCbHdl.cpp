// Copyright cocotb contributors
// Copyright (c) 2013 Potential Ventures Ltd
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

#include <gpi.h>

#include <cstddef>
#include <vector>

#include "./gpi_priv.hpp"
#include "./logging.hpp"

// We store pointers to value change callbacks that have been
// removed during user callbacks, then clean everything up
// before returning to the simulator.
static std::vector<gpi_callback *> removed_valuechange_cbs;

const char *GpiObjHdl::get_name_str() { return m_name.c_str(); }

const char *GpiObjHdl::get_fullname_str() { return m_fullname.c_str(); }

const std::string &GpiObjHdl::get_fullname() { return m_fullname; }

const char *GpiObjHdl::get_type_str() {
#define CASE_OPTION(_X) \
    case _X:            \
        ret = #_X;      \
        break

    const char *ret;

    switch (m_type) {
        CASE_OPTION(GPI_UNKNOWN);
        CASE_OPTION(GPI_MEMORY);
        CASE_OPTION(GPI_MODULE);
        CASE_OPTION(GPI_ARRAY);
        CASE_OPTION(GPI_ENUM);
        CASE_OPTION(GPI_STRUCTURE);
        CASE_OPTION(GPI_REAL);
        CASE_OPTION(GPI_INTEGER);
        CASE_OPTION(GPI_STRING);
        CASE_OPTION(GPI_GENARRAY);
        CASE_OPTION(GPI_PACKAGE);
        CASE_OPTION(GPI_LOGIC);
        CASE_OPTION(GPI_LOGIC_ARRAY);
        CASE_OPTION(GPI_PACKED_OBJECT);
        default:
            ret = "unknown";
    }

    return ret;
}

const std::string &GpiObjHdl::get_name() { return m_name; }

/* Genertic base clss implementations */
bool GpiHdl::is_this_impl(GpiImplInterface *impl) {
    return impl == this->m_impl;
}

int GpiObjHdl::initialise(const std::string &name, const std::string &fq_name) {
    m_name = name;
    m_fullname = fq_name;
    return 0;
}

// Xar (Exponential Array) support
// Made of separately allocated arrays (chunks) to keep pointer stability
// Each chunk of the array doubles the total size of the array
// [4kB][4kB][8kB][16kB]...
// Details about Xar can be seen in Andrew Reece's BSC 2025 talk
// Assuming as Much as Possible at https://youtu.be/i-h95QIGchY?t=3191

// Define helper function/macro in terms of compiler intrinsics
#if defined(_MSC_VER) || (defined(_WIN32) && defined(__clang__))

static inline uint8_t MSB32(uint32_t x) {
    unsigned long result = 0;
    _BitScanReverse(&result, x);
    return (uint8_t)result;
}

#elif defined(__clang__) || defined(__GNUC__) || defined(__GNUG__)

#define MSB32(x) ((uint8_t)(31 - __builtin_clz(x)))

#else
#error "clz intrinsics not defined for this compiler."
#endif

// Shift defines how many callbacks in the first chunk (1 << CB_SHIFT)
#define CB_SHIFT 12  // First chunk is 4096
#define CB_CHUNK_COUNT 16
static gpi_callback *callback_chunks[CB_CHUNK_COUNT] = {};
// With these parameters, the Xar can hold ~134 million callbacks if it were
// fully allocated

static gpi_callback *free_callback = nullptr;
static int32_t callback_count = 0;

struct xar_elem_info {
    int32_t chunk_index;
    uint32_t chunk_capacity;
    uint32_t elem_index;
};

static inline xar_elem_info xar_get_elem_info(uint32_t shift, int32_t index) {
    // Initialize data for lookup in xar
    uint32_t elem_index = static_cast<uint32_t>(index);
    uint32_t chunk_capacity = 1 << shift;
    uint8_t chunk_index = 0;

    // Check if index is outside first chunk
    uint32_t i_shift = elem_index >> shift;
    if (i_shift > 0) {
        // Calculate target chunk and the index within it
        chunk_index = MSB32(i_shift);
        chunk_capacity = 1 << (chunk_index + shift);
        elem_index -= chunk_capacity;
        chunk_index++;
    }

    // Return calculated info for element at index
    return xar_elem_info{chunk_index, chunk_capacity, elem_index};
}

gpi_callback *gpi_callback_acquire() {
    gpi_callback *cb = free_callback;

    if (cb) {
        free_callback = free_callback->next;
    } else {
        // Get info about where in xar the next callback object is
        xar_elem_info cb_info = xar_get_elem_info(CB_SHIFT, callback_count);

        // If the callback is in a chunk that hasn't been allocated yet, do the
        // allocation.
        // Use calloc to get zero-initialized callback objects.
        if (!callback_chunks[cb_info.chunk_index]) {
            callback_chunks[cb_info.chunk_index] = static_cast<gpi_callback *>(
                calloc(cb_info.chunk_capacity, sizeof(gpi_callback)));
            LOG_TRACE(
                "GPI: allocating callback pool chunk index %d of capacity %d",
                cb_info.chunk_index, cb_info.chunk_capacity);
        }

        cb = &callback_chunks[cb_info.chunk_index][cb_info.elem_index];
        cb->index = callback_count;
        callback_count += 1;
    }

    // Preserve persistent data and clear the rest
    meta32 meta = cb->meta;
    int32_t index = cb->index;
    *cb = {};
    cb->meta = meta;
    cb->index = index;

    cb->meta |= META_VALID;

    LOG_TRACE("GPI: callback acquire -> index %d", cb->index);

    return cb;
}

void gpi_callback_release(gpi_callback *cb) {
    cb->meta &= ~META_VALID;
    cb->meta += GEN_INCR;

    cb->next = free_callback;
    free_callback = cb;

    LOG_TRACE("GPI: callback release -> index %d", cb->index);
}

gpi_callback *gpi_callback_from_index(int32_t index) {
    // Do bounds check, this index is usually from user code
    if (index < 0 || index >= callback_count) {
        return nullptr;
    }

    xar_elem_info cb_info = xar_get_elem_info(CB_SHIFT, index);
    return &callback_chunks[cb_info.chunk_index][cb_info.elem_index];
}

void gpi_callback_audit() {
    if (!gpi_debug_enabled) {
        return;
    }

    LOG_TRACE("GPI: %d callback objects used", callback_count);

    for (int32_t cursor = 0; cursor < callback_count; cursor++) {
        gpi_callback *cb = gpi_callback_from_index(cursor);

        if (cb->index != cursor) {
            LOG_TRACE(
                "GPI: callback index %d has invalid internal index %d: "
                "kind: %s, is_parent: %d, removed: %d, impl: %s",
                cursor, cb->index, cb_kind_to_string(cb->kind),
                cb->kind & CB_PARENT, cb->removed, cb->impl->get_name_c());
        }
        if (cb->meta & META_VALID) {
            LOG_TRACE(
                "GPI: callback index %d marked valid: "
                "kind: %s, is_parent: %d, removed: %d, impl: %s",
                cursor, cb_kind_to_string(cb->kind), cb->kind & CB_PARENT,
                cb->removed, cb->impl->get_name_c());
        }
    }
}

const char *cb_kind_to_string(cb_kind kind) {
    switch (kind & CB_BASIC_KIND) {
        case CB_READWRITE:
            return "readwrite";
        case CB_READONLY:
            return "readonly";
        case CB_NEXT_TIMESTEP:
            return "nexttime";
        case CB_TIMED:
            return "timed";
        case CB_VALUE_CHANGE:
            return "valuechange";
        case CB_STARTUP:
            return "startup";
        case CB_SHUTDOWN:
            return "shutdown";
    }
    return "<unknown>";
}

int32_t handle_value_change_callback(gpi_callback *parent_cb) {
    int32_t any_error = 0;
    const char *impl_name = parent_cb->impl->get_name_c();

    // LCOV_EXCL_START
    if (!(parent_cb->kind & CB_PARENT)) {
        LOG_CRITICAL("%s: valuechange callback index %d not marked as parent!",
                     impl_name, parent_cb->index);
    }

    if (!parent_cb->signal) {
        LOG_CRITICAL(
            "%s: valuechange callback doesn't have an associated signal!",
            impl_name);
        any_error = 1;
    }
    // LCOV_EXCL_STOP
    else {
        GpiSignalObjHdl *signal =
            static_cast<GpiSignalObjHdl *>(parent_cb->signal);

        // LCOV_EXCL_START
        if (signal->m_valuechange_cb != parent_cb) {
            LOG_CRITICAL(
                "%s: valuechange callback is not first callback on signal's "
                "list!",
                impl_name);
        }
        // LCOV_EXCL_STOP

        const char *value_binstr = signal->get_signal_value_binstr();

        // Store the current last registered user callback.
        // This is the last callback that will be called before returning to the
        // simulator. Newly registered callbacks will not be processed until the
        // next value change.
        gpi_callback *last_cb = parent_cb->last;

        gpi_callback *next_cb = parent_cb->next;

        // Process callbacks
        bool process_cbs = next_cb != nullptr;
        while (process_cbs) {
            bool call_user_func = false;
            switch (next_cb->edge) {
                case GPI_VALUE_CHANGE:
                    call_user_func = true;
                    break;
                case GPI_RISING:
                    call_user_func = !strcmp(value_binstr, "1");
                    break;
                case GPI_FALLING:
                    call_user_func = !strcmp(value_binstr, "0");
                    break;
            }

            int32_t error = 0;
            if (call_user_func) {
                if (!next_cb->removed) {
                    if (next_cb->user_cb_func) {
                        GPI_TO_USER_CB(impl_name);
                        error = next_cb->user_cb_func(next_cb->user_cb_data);
                        USER_CB_TO_GPI(impl_name);
                    } else {
                        LOG_WARN(
                            "%s: valuechange user callback index %d has no "
                            "user function",
                            impl_name, next_cb->index);
                    }
                } else {
                    LOG_TRACE(
                        "%s: valuechange user callback index %d is removed",
                        impl_name, next_cb->index);
                }

                next_cb->impl->remove_callback(next_cb);
            }

            if (error) {
                any_error = error;
            }

            // Check if the user callback we just processed is the last one.
            // We need this at the end of the loop, but before going to the next
            // user callback, so that we don't skip over last_cb.
            if (next_cb == last_cb) {
                process_cbs = false;
            }

            // We're done processing if we hit the end of the list.
            next_cb = next_cb->next;
            if (!next_cb) {
                process_cbs = false;
            }
        }
    }
    return any_error;
}

void gpi_mark_valuechange_for_cleanup(gpi_callback *cb) {
    removed_valuechange_cbs.push_back(cb);
}

void gpi_cleanup_valuechange_cbs() {
    // For each child (user) callback in the list, remove it from the list.
    // If the user callback list is now empty, remove the parent sim callback.
    // Release the removed user callback to the callback pool

    for (gpi_callback *cb : removed_valuechange_cbs) {
        LOG_TRACE("%s: valuechange callback cleanup -> index %d",
                  cb->impl->get_name_c(), cb->index);

        // Only process valid child callbacks.
        bool valid_cb = cb->meta & META_VALID;
        if (valid_cb) {
            gpi_callback *parent_cb = cb->parent;
            if (parent_cb) {
                // LCOV_EXCL_START
                if (!cb->removed) {
                    LOG_CRITICAL(
                        "%s: callback index %d in cleanup list but not marked "
                        "removed",
                        cb->impl->get_name_c(), cb->index);
                }
                // LCOV_EXCL_STOP

                // Remove the callback from the list
                if (cb->next) {
                    cb->next->prev = cb->prev;
                }
                if (cb->prev) {
                    cb->prev->next = cb->next;
                }

                // If we were the last user callback for the parent,
                // remove the parent and sim callback.
                if (!parent_cb->next) {
                    LOG_TRACE(
                        "%s: parent callback index %d -> removing sim callback "
                        "%p",
                        parent_cb->impl->get_name_c(), parent_cb->index,
                        parent_cb->sim_cb_hdl);

                    parent_cb->impl->remove_callback(parent_cb);
                } else {
                    // If the removed callback was the last user callback for
                    // the parent, replace it with the callback before it in the
                    // list.
                    if (parent_cb->last == cb) {
                        parent_cb->last = cb->prev;
                    }
                }
            }
            // LCOV_EXCL_START
            else {
                LOG_CRITICAL("%s: callback index %d -> no parent callback",
                             cb->impl->get_name_c(), cb->index);
            }
            // LCOV_EXCL_STOP

            gpi_callback_release(cb);
        }
    }

    removed_valuechange_cbs.clear();
}
