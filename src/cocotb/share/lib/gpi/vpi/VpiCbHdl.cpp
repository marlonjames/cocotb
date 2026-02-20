// Copyright cocotb contributors
// Copyright (c) 2013, 2018 Potential Ventures Ltd
// Copyright (c) 2013 SolarFlare Communications Inc
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

#include "../gpi_priv.hpp"
#include "../logging.hpp"
#include "./VpiImpl.hpp"

#ifndef VPI_NO_QUEUE_SETIMMEDIATE_CALLBACKS
#include <algorithm>
#include <deque>

static std::deque<gpi_callback *> cb_queue;
#endif

static inline PLI_INT32 reason_from_cb_kind(cb_kind kind) {
    switch (kind & CB_BASIC_KIND) {
        case CB_READWRITE:
            return cbReadWriteSynch;
        case CB_READONLY:
            return cbReadOnlySynch;
        case CB_NEXT_TIMESTEP:
            return cbNextSimTime;
        case CB_TIMED:
            return cbAfterDelay;
        case CB_VALUE_CHANGE:
            return cbValueChange;

        case CB_STARTUP:
#ifdef IUS
            return cbAfterDelay;
#else
            return cbStartOfSimulation;
#endif
        case CB_SHUTDOWN:
            return cbEndOfSimulation;
    }
    return cbPLIError;
}

static int32_t handle_vpi_callback_(gpi_callback *cb) {
    int32_t error = (!cb);
    // LCOV_EXCL_START
    if (error) {
        LOG_CRITICAL("VPI: Callback data corrupted: ABORTING");
    }
    // LCOV_EXCL_STOP

    bool valid_cb = false;
    if (!error) {
        valid_cb = cb->meta & META_VALID;

        // LCOV_EXCL_START
        if (!valid_cb) {
            LOG_CRITICAL("VPI: Callback index %d not marked valid: ABORTING",
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
                    GPI_TO_USER_CB("VPI");
                    error = cb->user_cb_func(cb->user_cb_data);
                    USER_CB_TO_GPI("VPI");
                } else {
                    LOG_WARN("VPI: %s callback has no user callback function",
                             cb_kind_to_string(cb->kind));
                }
            }
        } else {
            LOG_TRACE("VPI: %s callback index %d is removed",
                      cb_kind_to_string(cb->kind), cb->index);
        }

        switch (cb->kind & CB_BASIC_KIND) {
            case CB_READWRITE:
            case CB_READONLY:
            case CB_NEXT_TIMESTEP:
            case CB_TIMED:
            case CB_STARTUP:
            case CB_SHUTDOWN:
#ifdef VERILATOR
            {
                if (!cb->removed) {
                    // Verilator seems to think some callbacks are recurring
                    // that Icarus and other sims do not. So we remove all
                    // callbacks here after firing because Verilator doesn't
                    // seem to mind (other sims do). Remove recurring callback
                    // once fired
                    vpiHandle sim_cb_hdl =
                        static_cast<vpiHandle>(cb->sim_cb_hdl);
                    int remove_success = vpi_remove_cb(sim_cb_hdl);

                    // LCOV_EXCL_START
                    if (!remove_success) {
                        LOG_DEBUG("VPI: Unable to remove %s callback index %d!",
                                  cb_kind_to_string(cb->kind), cb->index);
                        check_vpi_error();

                        // If we fail to remove the callback,
                        // mark as removed so we can ignore next time it fires.
                        cb->removed = true;
                    }
                    // LCOV_EXCL_STOP
                    else {
                        // If we removed it successfully,
                        // release the callback.
                        gpi_callback_release(cb);
                    }
                } else {
                    // If it has already been removed,
                    // just release the callback.
                    gpi_callback_release(cb);
                }
                break;
            }
#else
                // These one-shot callbacks have fired and can be released.
                gpi_callback_release(cb);
                break;
#endif
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

    return error ? -1 : 0;
}

// Main re-entry point for callbacks from simulator
int32_t handle_vpi_callback(p_cb_data sim_cb_data) {
    SIM_TO_GPI("VPI", VpiImpl::reason_to_string(sim_cb_data->reason));

    int handle_error = 0;
    gpi_callback *cb = (gpi_callback *)sim_cb_data->user_data;
#ifdef VPI_NO_QUEUE_SETIMMEDIATE_CALLBACKS
    handle_error = handle_vpi_callback_(cb);
#else
    // must push things into a queue because Icarus (gh-4067), Xcelium
    // (gh-4013), and Questa (gh-4105) react to value changes on signals that
    // are set with vpiNoDelay immediately, and not after the current callback
    // has ended, causing re-entrancy.
    static bool reacting = false;
    if (reacting) {
        cb_queue.push_back(cb);
    } else {
        reacting = true;
        handle_error = handle_vpi_callback_(cb);
        while (!cb_queue.empty()) {
            gpi_callback *next_cb = cb_queue.front();

            if (handle_error) {
                LOG_WARN(
                    "VPI: There was an error handling previous callback. "
                    "Skipping handling of %s callback index %d",
                    cb_kind_to_string(cb->kind), cb->index);
            } else {
                handle_error = handle_vpi_callback_(next_cb);
            }

            // The processed callback may have already been removed while it was
            // executing, so search for it and don't just pop the front.
            auto it = std::find(cb_queue.begin(), cb_queue.end(), next_cb);
            if (it != cb_queue.end()) {
                cb_queue.erase(it);
            }
        }
        reacting = false;
    }
#endif
    GPI_TO_SIM("VPI");
    return handle_error;
}

gpi_hdl VpiImpl::register_non_valuechange_callback(cb_kind kind, uint64_t time,
                                                   int (*cb_func)(void *),
                                                   void *cb_data) {
    gpi_hdl ret_hdl = gpi_nil_hdl;

    gpi_callback *cb = gpi_callback_acquire();

    s_vpi_time vpi_time = {};
    vpi_time.high = (uint32_t)(time >> 32);
    vpi_time.low = (uint32_t)(time);
    vpi_time.type = vpiSimTime;

    s_cb_data vpi_cb_data = {};
    vpi_cb_data.reason = reason_from_cb_kind(kind);
    vpi_cb_data.cb_rtn = handle_vpi_callback;
    vpi_cb_data.time = &vpi_time;
    vpi_cb_data.user_data = (char *)cb;

    vpiHandle sim_cb_hdl = vpi_register_cb(&vpi_cb_data);

    bool valid_cb = true;
    // LCOV_EXCL_START
    if (!sim_cb_hdl) {
        check_vpi_error();
        LOG_ERROR(
            "VPI: Unable to register a callback handle for VPI type %s(%d)",
            reason_to_string(vpi_cb_data.reason), vpi_cb_data.reason);
        valid_cb = false;
    }
    // LCOV_EXCL_STOP

    if (valid_cb) {
        cb->impl = this;
        cb->sim_cb_hdl = sim_cb_hdl;
        cb->user_cb_func = cb_func;
        cb->user_cb_data = cb_data;
        cb->kind = CB_TIMED;

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
gpi_hdl VpiImpl::register_timed_callback(uint64_t time, int (*cb_func)(void *),
                                         void *cb_data) {
    return register_non_valuechange_callback(CB_TIMED, time, cb_func, cb_data);
}

gpi_hdl VpiImpl::register_readonly_callback(int (*cb_func)(void *),
                                            void *cb_data) {
    return register_non_valuechange_callback(CB_READONLY, 0, cb_func, cb_data);
}

gpi_hdl VpiImpl::register_readwrite_callback(int (*cb_func)(void *),
                                             void *cb_data) {
    return register_non_valuechange_callback(CB_READWRITE, 0, cb_func, cb_data);
}

gpi_hdl VpiImpl::register_nexttime_callback(int (*cb_func)(void *),
                                            void *cb_data) {
    return register_non_valuechange_callback(CB_NEXT_TIMESTEP, 0, cb_func,
                                             cb_data);
}

int VpiImpl::remove_callback(gpi_callback *cb) {
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
            vpiHandle sim_cb_hdl = static_cast<vpiHandle>(cb->sim_cb_hdl);
            int remove_success = vpi_remove_cb(sim_cb_hdl);

            // LCOV_EXCL_START
            if (!remove_success) {
                LOG_DEBUG("VPI: Unable to remove %s callback index %d!",
                          cb_kind_to_string(cb->kind), cb->index);
                check_vpi_error();

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
            // just don't try.
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
#ifndef VPI_NO_QUEUE_SETIMMEDIATE_CALLBACKS
                // check if it's already fired and is in callback queue
                auto it = std::find(cb_queue.begin(), cb_queue.end(), cb);
                if (it != cb_queue.end()) {
                    cb_queue.erase(it);
                }
#endif

                vpiHandle sim_cb_hdl = static_cast<vpiHandle>(cb->sim_cb_hdl);
                int remove_success = vpi_remove_cb(sim_cb_hdl);

                // LCOV_EXCL_START
                if (!remove_success) {
                    LOG_DEBUG(
                        "VPI: Unable to remove valuechange callback index %d!",
                        cb->index);
                    check_vpi_error();

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

gpi_hdl VpiSignalObjHdl::register_value_change_callback(gpi_edge edge,
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
            "VPI: new valuechange child callback index %d added to existing "
            "parent callback index %d",
            user_cb->index, m_valuechange_cb->index);
    } else {
        // If there is no sim callback,
        // register one with a parent callback object and start the user
        // callback list.

        gpi_callback *parent_cb = gpi_callback_acquire();

        s_vpi_time vpi_time = {};
        vpi_time.type = vpiSuppressTime;
        s_vpi_value vpi_value = {};

        // The value is not needed in the callback data.
        // Any getting of values will be done separately.
        vpi_value.format = vpiSuppressVal;

        s_cb_data vpi_cb_data = {};
        vpi_cb_data.reason = reason_from_cb_kind(CB_VALUE_CHANGE);
        vpi_cb_data.cb_rtn = handle_vpi_callback;
        vpi_cb_data.obj = get_handle<vpiHandle>();
        vpi_cb_data.value = &vpi_value;
        vpi_cb_data.time = &vpi_time;
        vpi_cb_data.user_data = (char *)parent_cb;

        vpiHandle sim_cb_hdl = vpi_register_cb(&vpi_cb_data);

        bool valid_sim_cb = true;
        // LCOV_EXCL_START
        if (!sim_cb_hdl) {
            check_vpi_error();
            LOG_ERROR(
                "VPI: Unable to register a callback handle for VPI type "
                "%s(%d)",
                VpiImpl::reason_to_string(vpi_cb_data.reason),
                vpi_cb_data.reason);
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
