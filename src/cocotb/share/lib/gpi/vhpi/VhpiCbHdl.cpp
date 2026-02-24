// Copyright cocotb contributors
// Copyright (c) 2013 Potential Ventures Ltd
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

#include <cstring>

#include "../logging.hpp"
#include "./VhpiImpl.hpp"
#include "_vendor/vhpi/vhpi_user.h"
#include "gpi.h"

static inline int32_t reason_from_cb_kind(cb_kind kind) {
    switch (kind & CB_BASIC_KIND) {
        case CB_READWRITE:
            return vhpiCbRepLastKnownDeltaCycle;
        case CB_READONLY:
            return vhpiCbRepEndOfTimeStep;
        case CB_NEXT_TIMESTEP:
            return vhpiCbRepNextTimeStep;
        case CB_TIMED:
            return vhpiCbAfterDelay;
        case CB_VALUE_CHANGE:
            return vhpiCbValueChange;
        case CB_STARTUP:
            return vhpiCbStartOfSimulation;
        case CB_SHUTDOWN:
            return vhpiCbEndOfSimulation;
    }
    return vhpiCbPLIError;
}

// Main entry point for callbacks from simulator
void handle_vhpi_callback(const vhpiCbDataT *sim_cb_data) {
    SIM_TO_GPI("VHPI", VhpiImpl::reason_to_string(sim_cb_data->reason));

    gpi_callback *cb = (gpi_callback *)sim_cb_data->user_data;

    int32_t error = (!cb);
    // LCOV_EXCL_START
    if (error) {
        LOG_CRITICAL("VHPI: Callback data corrupted: ABORTING");
    }
    // LCOV_EXCL_STOP

    bool valid_cb = false;
    if (!error) {
        valid_cb = cb->meta & META_VALID;

        // LCOV_EXCL_START
        if (!valid_cb) {
            LOG_CRITICAL("VHPI: Callback index %d not marked valid: ABORTING",
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
                    GPI_TO_USER_CB("VHPI");
                    error = cb->user_cb_func(cb->user_cb_data);
                    USER_CB_TO_GPI("VHPI");
                } else {
                    LOG_WARN("VHPI: %s callback has no user callback function",
                             cb_kind_to_string(cb->kind));
                }
            }
        } else {
            LOG_TRACE("VHPI: %s callback index %d is removed",
                      cb_kind_to_string(cb->kind), cb->index);
        }

        switch (cb->kind & CB_BASIC_KIND) {
            case CB_READWRITE:
            case CB_READONLY:
            case CB_NEXT_TIMESTEP: {
                if (!cb->removed) {
                    // For recurring VHPI callbacks, we try to remove them after
                    // they fire.
                    vhpiHandleT sim_cb_hdl =
                        static_cast<vhpiHandleT>(cb->sim_cb_hdl);
                    int remove_error = vhpi_remove_cb(sim_cb_hdl);

                    // LCOV_EXCL_START
                    if (remove_error) {
                        LOG_DEBUG(
                            "VHPI: Unable to remove %s callback index %d!",
                            cb_kind_to_string(cb->kind), cb->index);
                        check_vhpi_error();

                        // If we fail to remove the callback,
                        // mark as removed so we can ignore next time it fires.
                        cb->removed = true;
                    }
                    // LCOV_EXCL_STOP
                    else {
                        gpi_callback_release(cb);
                    }
                } else {
                    // If it has already been removed,
                    // just release the callback.
                    gpi_callback_release(cb);
                }
                break;
            }
            case CB_TIMED:
            case CB_STARTUP:
            case CB_SHUTDOWN:
                // These one-shot callbacks have fired and can be released.
                gpi_callback_release(cb);
                break;
            case CB_VALUE_CHANGE:
                // If the parent callback object was marked removed,
                // it should be ignored as if it didn't happen.
                // The callback has occurred so we can release the handle.
                if (cb->removed) {
                    gpi_callback_release(cb);
                }
                break;
        }
    }

    gpi_cleanup_valuechange_cbs();

    if (error) {
        gpi_end_of_sim_time();
    }

    GPI_TO_SIM("VHPI");
}

gpi_hdl VhpiSignalObjHdl::register_value_change_callback(gpi_edge edge,
                                                         int (*cb_func)(void *),
                                                         void *cb_data) {
    gpi_hdl ret_hdl = gpi_nil_hdl;

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
            "VHPI: new valuechange child callback index %d added to existing "
            "parent callback index %d",
            user_cb->index, m_valuechange_cb->index);
    } else {
        // If there is no sim callback,
        // register one with a parent callback object and start the user
        // callback list.

        gpi_callback *parent_cb = gpi_callback_acquire();

        vhpiCbDataT vhpi_cb_data = {};
        vhpi_cb_data.reason = reason_from_cb_kind(CB_VALUE_CHANGE);
        vhpi_cb_data.cb_rtn = handle_vhpi_callback;
        vhpi_cb_data.obj = get_handle<vhpiHandleT>();
        vhpi_cb_data.user_data = (char *)parent_cb;

        vhpiHandleT sim_cb_hdl = vhpi_register_cb(&vhpi_cb_data, vhpiReturnCb);

        bool valid_sim_cb = true;
        // LCOV_EXCL_START
        if (!sim_cb_hdl) {
            check_vhpi_error();
            LOG_ERROR(
                "VHPI: Unable to register a callback handle for VHPI type "
                "%s(%d)",
                VhpiImpl::reason_to_string(vhpi_cb_data.reason),
                vhpi_cb_data.reason);
            valid_sim_cb = false;
        }
        // LCOV_EXCL_STOP

        if (valid_sim_cb) {
            parent_cb->impl = m_impl;
            parent_cb->sim_cb_hdl = sim_cb_hdl;
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
                "VPI: from sim callback %p -> new valuechange parent callback "
                "index %d and child callback index %d",
                sim_cb_hdl, parent_cb->index, user_cb->index);
        }
        // LCOV_EXCL_START
        else {
            gpi_callback_release(parent_cb);
            valid_cb = false;
        }
        // LCOV_EXCL_STOP
    }

    if (valid_cb) {
        ret_hdl.index = user_cb->index;
        ret_hdl.meta = user_cb->meta;
    }
    // LCOV_EXCL_START
    else {
        gpi_callback_release(user_cb);
    }
    // LCOV_EXCL_STOP

    return ret_hdl;
}

gpi_hdl VhpiImpl::register_non_valuechange_callback(cb_kind kind, uint64_t time,
                                                    int (*cb_func)(void *),
                                                    void *cb_data) {
    gpi_hdl ret_hdl = gpi_nil_hdl;

    gpi_callback *cb = gpi_callback_acquire();

    vhpiTimeT vhpi_time = {};
    vhpi_time.high = (uint32_t)(time >> 32);
    vhpi_time.low = (uint32_t)(time);

    vhpiCbDataT vhpi_cb_data = {};
    vhpi_cb_data.reason = reason_from_cb_kind(kind);
    vhpi_cb_data.cb_rtn = handle_vhpi_callback;
    // For non vhpiCbAfterDelay callbacks,
    // providing a non-NULL time member will result in
    // the callback data having a time value of when event occurred,
    // so it's fine.
    // (IEEE Std 1076-2008 21.3.2.1)
    vhpi_cb_data.time = &vhpi_time;
    vhpi_cb_data.user_data = (char *)cb;

    vhpiHandleT sim_cb_hdl = vhpi_register_cb(&vhpi_cb_data, vhpiReturnCb);

    bool valid_cb = true;
    // LCOV_EXCL_START
    if (!sim_cb_hdl) {
        check_vhpi_error();
        LOG_ERROR(
            "VHPI: Unable to register a callback handle for VHPI type "
            "%s(%d)",
            reason_to_string(vhpi_cb_data.reason), vhpi_cb_data.reason);
        valid_cb = false;
    }
    // LCOV_EXCL_STOP

    if (valid_cb) {
        cb->impl = this;
        cb->sim_cb_hdl = sim_cb_hdl;
        cb->user_cb_func = cb_func;
        cb->user_cb_data = cb_data;
        cb->kind = kind;

        ret_hdl.index = cb->index;
        ret_hdl.meta = cb->meta;
    }
    // LCOV_EXCL_START
    else {
        gpi_callback_release(cb);
    }
    // LCOV_EXCL_STOP

    return ret_hdl;
}

gpi_hdl VhpiImpl::register_timed_callback(uint64_t time, int (*cb_func)(void *),
                                          void *cb_data) {
    return register_non_valuechange_callback(CB_TIMED, time, cb_func, cb_data);
}

gpi_hdl VhpiImpl::register_readonly_callback(int (*cb_func)(void *),
                                             void *cb_data) {
    return register_non_valuechange_callback(CB_READONLY, 0, cb_func, cb_data);
}

gpi_hdl VhpiImpl::register_readwrite_callback(int (*cb_func)(void *),
                                              void *cb_data) {
    return register_non_valuechange_callback(CB_READWRITE, 0, cb_func, cb_data);
}

gpi_hdl VhpiImpl::register_nexttime_callback(int (*cb_func)(void *),
                                             void *cb_data) {
    return register_non_valuechange_callback(CB_NEXT_TIMESTEP, 0, cb_func,
                                             cb_data);
}

int VhpiImpl::remove_callback(gpi_callback *cb) {
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
            vhpiHandleT sim_cb_hdl = static_cast<vhpiHandleT>(cb->sim_cb_hdl);
            int remove_error = vhpi_remove_cb(sim_cb_hdl);

            // LCOV_EXCL_START
            if (remove_error) {
                LOG_DEBUG("VHPI: Unable to remove %s callback index %d!",
                          cb_kind_to_string(cb->kind), cb->index);
                check_vhpi_error();

                // It is already marked removed but we don't release it until
                // the callback fires
            }
            // LCOV_EXCL_STOP
            else {
                gpi_callback_release(cb);
            }
            break;
        }
        case CB_STARTUP:
        case CB_SHUTDOWN: {
            // Too many sims get upset trying to remove startup callbacks so we
            // just don't try
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
            // In that case, we try to remove the sim callback
            // and clear it from the associated signal object.

            if (cb->kind & CB_PARENT) {
                vhpiHandleT sim_cb_hdl =
                    static_cast<vhpiHandleT>(cb->sim_cb_hdl);
                int remove_error = vhpi_remove_cb(sim_cb_hdl);

                // LCOV_EXCL_START
                if (remove_error) {
                    LOG_DEBUG(
                        "VHPI: Unable to remove valuechange callback index %d!",
                        cb->index);
                    check_vhpi_error();

                    // It is already marked removed but we don't release it
                    // until the callback fires
                }
                // LCOV_EXCL_STOP
                else {
                    gpi_callback_release(cb);

                    cb->signal->m_valuechange_cb = nullptr;
                }
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
