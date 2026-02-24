// Copyright cocotb contributors
// Copyright (c) 2015/16 Potential Ventures Ltd
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

#include <cstring>

#include "../logging.hpp"
#include "./FliImpl.hpp"
#include "_vendor/fli/mti.h"

// FLI processes can't be deleted once created.
// They also are associated with a gpi_callback object
// that can't be changed after creation.
// This means that we have to keep the gpi_callback
// objects in these FLI-specific free lists instead of
// releasing them back to the global pool.
static gpi_callback *free_timed_callback = nullptr;
static gpi_callback *free_read_write_callback = nullptr;
static gpi_callback *free_read_only_callback = nullptr;
static gpi_callback *free_next_phase_callback = nullptr;
static gpi_callback *free_value_change_callback = nullptr;

static void fli_proc_callback_release(gpi_callback *cb) {
    cb->meta &= ~META_VALID;
    cb->meta += GEN_INCR;

    switch (cb->kind & CB_BASIC_KIND) {
        case CB_READWRITE:
            cb->next = free_read_write_callback;
            free_read_write_callback = cb;
            break;
        case CB_READONLY:
            cb->next = free_read_only_callback;
            free_read_only_callback = cb;
            break;
        case CB_NEXT_TIMESTEP:
            cb->next = free_next_phase_callback;
            free_next_phase_callback = cb;
            break;
        case CB_TIMED:
            cb->next = free_timed_callback;
            free_timed_callback = cb;
            break;
        case CB_VALUE_CHANGE:
            cb->next = free_value_change_callback;
            free_value_change_callback = cb;
            break;

        case CB_STARTUP:
            LOG_CRITICAL("FLI: Attempting to release startup callback!");
            break;
        case CB_SHUTDOWN:
            LOG_CRITICAL("FLI: Attempting to release shutdown callback!");
            break;
    }

    LOG_TRACE("FLI: callback release -> %s callback index %d to free list",
              cb_kind_to_string(cb->kind), cb->index);
}

// Main entry point for callbacks from simulator
void handle_fli_callback(void *data) {
    SIM_TO_GPI("FLI", "callback");

    gpi_callback *cb = (gpi_callback *)data;

    int32_t error = (!cb);
    // LCOV_EXCL_START
    if (error) {
        LOG_CRITICAL("FLI: Callback data corrupted: ABORTING");
    }
    // LCOV_EXCL_STOP

    bool valid_cb = false;
    if (!error) {
        valid_cb = cb->meta & META_VALID;

        // LCOV_EXCL_START
        if (!valid_cb) {
            LOG_CRITICAL("FLI: Callback index %d not marked valid: ABORTING",
                         cb->index);
            error = 1;
        }
        // LCOV_EXCL_STOP
    }

    if (valid_cb) {
        if (!cb->removed) {
            if ((cb->kind & CB_BASIC_KIND) == CB_VALUE_CHANGE) {
                error = handle_value_change_callback(cb);
            } else {
                if (cb->user_cb_func) {
                    GPI_TO_USER_CB("FLI");
                    error = cb->user_cb_func(cb->user_cb_data);
                    USER_CB_TO_GPI("FLI");
                } else {
                    LOG_WARN("FLI: %s callback has no user callback function",
                             cb_kind_to_string(cb->kind));
                }
            }
        } else {
            LOG_TRACE("FLI: %s callback index %d is removed",
                      cb_kind_to_string(cb->kind), cb->index);
        }

        switch (cb->kind & CB_BASIC_KIND) {
            case CB_TIMED:
            case CB_READWRITE:
            case CB_READONLY:
            case CB_NEXT_TIMESTEP:
                fli_proc_callback_release(cb);
                break;
            case CB_STARTUP:
            case CB_SHUTDOWN:
                gpi_callback_release(cb);
                break;
            case CB_VALUE_CHANGE:
                // Do nothing here.
                // If there are no more user callbacks, the parent callback
                // will be desensitized and released in the cleanup below.
                // Otherwise we leave it to fire again.
                break;
        }
    }

    gpi_cleanup_valuechange_cbs();

    if (error) {
        gpi_end_of_sim_time();
    }

    GPI_TO_SIM("FLI");
}

static gpi_callback *fli_proc_callback_acquire(cb_kind kind) {
    gpi_callback **cb_list = nullptr;
    int priority = 0;

    switch (kind & CB_BASIC_KIND) {
        case CB_READWRITE:
            cb_list = &free_read_write_callback;
            priority = MTI_PROC_SYNCH;
            break;
        case CB_READONLY:
            cb_list = &free_read_only_callback;
            priority = MTI_PROC_POSTPONED;
            break;
        case CB_NEXT_TIMESTEP:
            cb_list = &free_next_phase_callback;
            priority = MTI_PROC_IMMEDIATE;
            break;
        case CB_TIMED:
            cb_list = &free_timed_callback;
            priority = MTI_PROC_IMMEDIATE;
            break;
        case CB_VALUE_CHANGE:
            cb_list = &free_value_change_callback;
            priority = MTI_PROC_NORMAL;
            break;

        case CB_STARTUP:
            LOG_CRITICAL("FLI: Attempting to acquire startup callback!");
            return nullptr;
            break;
        case CB_SHUTDOWN:
            LOG_CRITICAL("FLI: Attempting to acquire shutdown callback!");
            return nullptr;
            break;
    }

    gpi_callback *cb = *cb_list;

    if (cb) {
        *cb_list = (*cb_list)->next;
        cb->meta |= META_VALID;

        LOG_TRACE(
            "FLI: callback acquire -> %s callback index %d from free list with "
            "sim callback %p",
            cb_kind_to_string(cb->kind), cb->index, cb->sim_cb_hdl);
    } else {
        cb = gpi_callback_acquire();
        mtiProcessIdT mti_proc = mti_CreateProcessWithPriority(
            nullptr, handle_fli_callback, cb, (mtiProcessPriorityT)priority);

        cb->sim_cb_hdl = mti_proc;

        LOG_TRACE(
            "FLI: callback acquire -> new %s callback index %d with sim "
            "callback %p",
            cb_kind_to_string(cb->kind), cb->index, cb->sim_cb_hdl);
    }

    return cb;
}

gpi_hdl FliImpl::register_timed_callback(uint64_t time, int (*cb_func)(void *),
                                         void *cb_data) {
    gpi_hdl ret_hdl = gpi_nil_hdl;

    gpi_callback *cb = fli_proc_callback_acquire(CB_TIMED);

    mti_ScheduleWakeup64(static_cast<mtiProcessIdT>(cb->sim_cb_hdl),
                         static_cast<mtiTime64T>(time));

    cb->impl = this;
    cb->user_cb_func = cb_func;
    cb->user_cb_data = cb_data;
    cb->kind = CB_TIMED;

    ret_hdl.index = cb->index;
    ret_hdl.meta = cb->meta;

    return ret_hdl;
}

gpi_hdl FliImpl::register_readonly_callback(int (*cb_func)(void *),
                                            void *cb_data) {
    gpi_hdl ret_hdl = gpi_nil_hdl;

    gpi_callback *cb = fli_proc_callback_acquire(CB_READONLY);

    mti_ScheduleWakeup(static_cast<mtiProcessIdT>(cb->sim_cb_hdl), 0);

    cb->impl = this;
    cb->user_cb_func = cb_func;
    cb->user_cb_data = cb_data;
    cb->kind = CB_READONLY;

    ret_hdl.index = cb->index;
    ret_hdl.meta = cb->meta;

    return ret_hdl;
}

gpi_hdl FliImpl::register_readwrite_callback(int (*cb_func)(void *),
                                             void *cb_data) {
    gpi_hdl ret_hdl = gpi_nil_hdl;

    gpi_callback *cb = fli_proc_callback_acquire(CB_READWRITE);

    mti_ScheduleWakeup(static_cast<mtiProcessIdT>(cb->sim_cb_hdl), 0);

    cb->impl = this;
    cb->user_cb_func = cb_func;
    cb->user_cb_data = cb_data;
    cb->kind = CB_READWRITE;

    ret_hdl.index = cb->index;
    ret_hdl.meta = cb->meta;

    return ret_hdl;
}

gpi_hdl FliImpl::register_nexttime_callback(int (*cb_func)(void *),
                                            void *cb_data) {
    gpi_hdl ret_hdl = gpi_nil_hdl;

    gpi_callback *cb = fli_proc_callback_acquire(CB_NEXT_TIMESTEP);

    mti_ScheduleWakeup(static_cast<mtiProcessIdT>(cb->sim_cb_hdl), 0);

    cb->impl = this;
    cb->user_cb_func = cb_func;
    cb->user_cb_data = cb_data;
    cb->kind = CB_NEXT_TIMESTEP;

    ret_hdl.index = cb->index;
    ret_hdl.meta = cb->meta;

    return ret_hdl;
}

int FliImpl::remove_callback(gpi_callback *cb) {
    if (cb->removed) {
        return 0;
    }

    // Mark as removed
    cb->removed = true;

    switch (cb->kind & CB_BASIC_KIND) {
        case CB_READWRITE:
        case CB_READONLY:
        case CB_NEXT_TIMESTEP:
        case CB_TIMED: {
            // mti_ScheduleWakeup callbacks can't be cancelled,
            // so we mark it removed and let it fire.
            // When it fires, this flag prevents the user callback from
            // being called and then the gpi_callback is released to the
            // appropriate free list to be reused.
            break;
        }
        case CB_STARTUP: {
            mti_RemoveLoadDoneCB(handle_fli_callback, cb);
            gpi_callback_release(cb);
            break;
        }
        case CB_SHUTDOWN: {
            mti_RemoveQuitCB(handle_fli_callback, cb);
            gpi_callback_release(cb);
            break;
        }
        case CB_VALUE_CHANGE: {
            // There are two types of value change callbacks to remove.
            // 1. User callbacks that are children of a parent callback
            // 2. Parent callbacks that are associated with the simulator
            // callback
            //
            // For user callbacks, we mark them for cleanup later,
            // before the sim callback returns.
            // In the later cleanup, if all child callbacks have been removed
            // and marked for cleanup,
            // this function will get called on the parent callback.
            // In that case, we desensitize and release the FLI object,
            // and clear the parent callback from the associated signal object.

            if (cb->kind & CB_PARENT) {
                mti_Desensitize(static_cast<mtiProcessIdT>(cb->sim_cb_hdl));
                fli_proc_callback_release(cb);

                cb->signal->m_valuechange_cb = nullptr;
            } else {
                // Don't release it here, just mark it to be cleaned up before
                // the callback returns
                gpi_mark_valuechange_for_cleanup(cb);
            }
            break;
        }

        default:
            break;
    }

    return 0;
}

gpi_hdl FliSignalObjHdl::register_value_change_callback(gpi_edge edge,
                                                        int (*cb_func)(void *),
                                                        void *cb_data) {
    gpi_hdl ret_hdl = gpi_nil_hdl;

    if (!m_is_var) {
        gpi_callback *user_cb = gpi_callback_acquire();

        bool valid_cb = true;
        if (m_valuechange_cb) {
            // If there is already a sim callback,
            // add a user callback to the end of the list.
            user_cb->impl = m_impl;
            user_cb->parent = m_valuechange_cb;
            user_cb->user_cb_func = cb_func;
            user_cb->user_cb_data = cb_data;
            user_cb->edge = edge;
            user_cb->kind = CB_VALUE_CHANGE;

            // Update parent
            m_valuechange_cb->last->next = user_cb;
            user_cb->prev = m_valuechange_cb->last;
            m_valuechange_cb->last = user_cb;

            LOG_TRACE(
                "FLI: new valuechange child callback index %d added to "
                "existing parent callback index %d",
                user_cb->index, m_valuechange_cb->index);
        } else {
            // If there is no sim callback,
            // register one with a parent callback object and start the user
            // callback list.

            gpi_callback *parent_cb =
                fli_proc_callback_acquire(CB_VALUE_CHANGE);

            mti_Sensitize(static_cast<mtiProcessIdT>(parent_cb->sim_cb_hdl),
                          get_handle<mtiSignalIdT>(), MTI_EVENT);

            parent_cb->impl = m_impl;
            parent_cb->next = user_cb;
            parent_cb->last = user_cb;
            parent_cb->signal = this;
            parent_cb->kind = CB_VALUE_CHANGE | CB_PARENT;

            user_cb->impl = m_impl;
            user_cb->parent = parent_cb;
            user_cb->prev = parent_cb;
            user_cb->user_cb_func = cb_func;
            user_cb->user_cb_data = cb_data;
            user_cb->edge = edge;
            user_cb->kind = CB_VALUE_CHANGE;

            m_valuechange_cb = parent_cb;

            LOG_TRACE(
                "FLI: sensitizing sim callback %p for valuechange parent "
                "callback index %d and adding new child callback index %d",
                parent_cb->sim_cb_hdl, parent_cb->index, user_cb->index);
        }

        if (valid_cb) {
            ret_hdl.index = user_cb->index;
            ret_hdl.meta = user_cb->meta;
        }
    }

    return ret_hdl;
}
