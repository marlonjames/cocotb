// Copyright cocotb contributors
// Copyright (c) 2014 Potential Ventures Ltd
// Licensed under the Revised BSD License, see LICENSE for details.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef COCOTB_FLI_IMPL_H_
#define COCOTB_FLI_IMPL_H_

#include <exports.h>
#include <gpi.h>

#include <map>
#include <vector>

#include "../gpi_priv.hpp"
#include "_vendor/fli/mti.h"

#ifdef COCOTBFLI_EXPORTS
#define COCOTBFLI_EXPORT COCOTB_EXPORT
#else
#define COCOTBFLI_EXPORT COCOTB_IMPORT
#endif

class FliImpl;
class FliSignalObjHdl;

// Main entry point for callbacks from simulator
void handle_fli_callback(void *data);

// Object Handles
class FliObj {
  public:
    FliObj(int acc_type, int acc_full_type)
        : m_acc_type(acc_type), m_acc_full_type(acc_full_type) {}

    virtual ~FliObj() = default;

    int get_acc_type() { return m_acc_type; }
    int get_acc_full_type() { return m_acc_full_type; }

  protected:
    int m_acc_type;
    int m_acc_full_type;
};

class FliObjHdl : public GpiObjHdl, public FliObj {
  public:
    FliObjHdl(GpiImplInterface *impl, void *hdl, gpi_objtype objtype,
              int acc_type, int acc_full_type, bool is_const = false)
        : GpiObjHdl(impl, hdl, objtype, is_const),
          FliObj(acc_type, acc_full_type) {}

    int initialise(const std::string &name,
                   const std::string &fq_name) override;
};

class FliSignalObjHdl : public GpiSignalObjHdl, public FliObj {
  public:
    FliSignalObjHdl(GpiImplInterface *impl, void *hdl, gpi_objtype objtype,
                    bool is_const, int acc_type, int acc_full_type, bool is_var)
        : GpiSignalObjHdl(impl, hdl, objtype, is_const),
          FliObj(acc_type, acc_full_type),
          m_is_var(is_var) {}

    int initialise(const std::string &name,
                   const std::string &fq_name) override;
    gpi_hdl register_value_change_callback(gpi_edge edge,
                                           int (*function)(void *),
                                           void *cb_data) override;

    bool is_variable() { return m_is_var; }

  protected:
    bool m_is_var;
};

class FliValueObjHdl : public FliSignalObjHdl {
  public:
    FliValueObjHdl(GpiImplInterface *impl, void *hdl, gpi_objtype objtype,
                   bool is_const, int acc_type, int acc_full_type, bool is_var,
                   mtiTypeIdT valType, mtiTypeKindT typeKind)
        : FliSignalObjHdl(impl, hdl, objtype, is_const, acc_type, acc_full_type,
                          is_var),
          m_fli_type(typeKind),
          m_val_type(valType) {}

    ~FliValueObjHdl() override {
        if (m_val_buff != NULL) delete[] m_val_buff;
        if (m_sub_hdls != NULL) mti_VsimFree(m_sub_hdls);
    }

    const char *get_signal_value_binstr() override;
    const char *get_signal_value_str() override;
    double get_signal_value_real() override;
    long get_signal_value_long() override;

    int set_signal_value(int32_t value, gpi_set_action action) override;
    int set_signal_value(double value, gpi_set_action action) override;
    int set_signal_value_str(std::string &value,
                             gpi_set_action action) override;
    int set_signal_value_binstr(std::string &value,
                                gpi_set_action action) override;

    void *get_sub_hdl(int index);

    int initialise(const std::string &name,
                   const std::string &fq_name) override;

    mtiTypeKindT get_fli_typekind() { return m_fli_type; }
    mtiTypeIdT get_fli_typeid() { return m_val_type; }

  protected:
    mtiTypeKindT m_fli_type;
    mtiTypeIdT m_val_type;
    char *m_val_buff = nullptr;
    void **m_sub_hdls = nullptr;
};

class FliEnumObjHdl : public FliValueObjHdl {
  public:
    FliEnumObjHdl(GpiImplInterface *impl, void *hdl, gpi_objtype objtype,
                  bool is_const, int acc_type, int acc_full_type, bool is_var,
                  mtiTypeIdT valType, mtiTypeKindT typeKind)
        : FliValueObjHdl(impl, hdl, objtype, is_const, acc_type, acc_full_type,
                         is_var, valType, typeKind) {}

    const char *get_signal_value_str() override;
    long get_signal_value_long() override;

    using FliValueObjHdl::set_signal_value;
    int set_signal_value(int32_t value, gpi_set_action action) override;

    int initialise(const std::string &name,
                   const std::string &fq_name) override;

  private:
    char **m_value_enum = nullptr;  // Do Not Free
    mtiInt32T m_num_enum = 0;
};

class FliLogicObjHdl : public FliValueObjHdl {
  public:
    FliLogicObjHdl(GpiImplInterface *impl, void *hdl, gpi_objtype objtype,
                   bool is_const, int acc_type, int acc_full_type, bool is_var,
                   mtiTypeIdT valType, mtiTypeKindT typeKind)
        : FliValueObjHdl(impl, hdl, objtype, is_const, acc_type, acc_full_type,
                         is_var, valType, typeKind) {}

    ~FliLogicObjHdl() override {
        if (m_mti_buff != NULL) delete[] m_mti_buff;
    }

    const char *get_signal_value_binstr() override;

    using FliValueObjHdl::set_signal_value;
    int set_signal_value(int32_t value, gpi_set_action action) override;
    int set_signal_value_binstr(std::string &value,
                                gpi_set_action action) override;

    int initialise(const std::string &name,
                   const std::string &fq_name) override;

  private:
    char *m_mti_buff = nullptr;
    char **m_value_enum = nullptr;  // Do Not Free
    mtiInt32T m_num_enum = 0;
    std::map<char, mtiInt32T> m_enum_map;
};

class FliIntObjHdl : public FliValueObjHdl {
  public:
    FliIntObjHdl(GpiImplInterface *impl, void *hdl, gpi_objtype objtype,
                 bool is_const, int acc_type, int acc_full_type, bool is_var,
                 mtiTypeIdT valType, mtiTypeKindT typeKind)
        : FliValueObjHdl(impl, hdl, objtype, is_const, acc_type, acc_full_type,
                         is_var, valType, typeKind) {}

    const char *get_signal_value_binstr() override;
    long get_signal_value_long() override;
    int get_signed() override {
        // We don't know if the object is a Verilog object or VHDL object, but
        // we assume that FLI is for VHDL accesses primarily and VHDL ints are
        // always signed.
        return 1;
    }

    using FliValueObjHdl::set_signal_value;
    int set_signal_value(int32_t value, gpi_set_action action) override;

    int initialise(const std::string &name,
                   const std::string &fq_name) override;
};

class FliRealObjHdl : public FliValueObjHdl {
  public:
    FliRealObjHdl(GpiImplInterface *impl, void *hdl, gpi_objtype objtype,
                  bool is_const, int acc_type, int acc_full_type, bool is_var,
                  mtiTypeIdT valType, mtiTypeKindT typeKind)
        : FliValueObjHdl(impl, hdl, objtype, is_const, acc_type, acc_full_type,
                         is_var, valType, typeKind) {}

    ~FliRealObjHdl() override {
        if (m_mti_buff != NULL) delete m_mti_buff;
    }

    double get_signal_value_real() override;

    using FliValueObjHdl::set_signal_value;
    int set_signal_value(double value, gpi_set_action action) override;

    int initialise(const std::string &name,
                   const std::string &fq_name) override;

  private:
    double *m_mti_buff = nullptr;
};

class FliStringObjHdl : public FliValueObjHdl {
  public:
    FliStringObjHdl(GpiImplInterface *impl, void *hdl, gpi_objtype objtype,
                    bool is_const, int acc_type, int acc_full_type, bool is_var,
                    mtiTypeIdT valType, mtiTypeKindT typeKind)
        : FliValueObjHdl(impl, hdl, objtype, is_const, acc_type, acc_full_type,
                         is_var, valType, typeKind) {}

    ~FliStringObjHdl() override {
        if (m_mti_buff != NULL) delete[] m_mti_buff;
    }

    const char *get_signal_value_str() override;

    using FliValueObjHdl::set_signal_value;
    int set_signal_value_str(std::string &value,
                             gpi_set_action action) override;

    int initialise(const std::string &name,
                   const std::string &fq_name) override;

  private:
    char *m_mti_buff = nullptr;
};

class FliIterator : public GpiIterator {
  public:
    enum OneToMany {
        OTM_CONSTANTS,  // include Generics
        OTM_SIGNALS,
        OTM_REGIONS,
        OTM_SIGNAL_SUB_ELEMENTS,
        OTM_VARIABLE_SUB_ELEMENTS
    };

    FliIterator(GpiImplInterface *impl, GpiObjHdl *hdl);

    Status next_handle(std::string &name, GpiObjHdl **hdl,
                       void **raw_hdl) override;

  private:
    void populate_handle_list(OneToMany childType);

  private:
    static std::map<int, std::vector<OneToMany>>
        iterate_over;                 /* Possible mappings */
    std::vector<OneToMany> *selected; /* Mapping currently in use */
    std::vector<OneToMany>::iterator one2many;

    std::vector<void *> m_vars;
    std::vector<void *> m_sigs;
    std::vector<void *> m_regs;
    std::vector<void *> *m_currentHandles;
    std::vector<void *>::iterator m_iterator;
};

class FliImpl : public GpiImplInterface {
  public:
    FliImpl(const std::string &name) : GpiImplInterface(name) {}

    /* Sim related */
    void sim_end() override;
    void get_sim_time(uint32_t *high, uint32_t *low) override;
    void get_sim_precision(int32_t *precision) override;
    const char *get_simulator_product() override;
    const char *get_simulator_version() override;
    int get_simulator_args(int *argc, const char *const **argv) override;

    /* Hierarchy related */
    GpiObjHdl *get_child_by_name(const std::string &name,
                                 GpiObjHdl *parent) override;
    GpiObjHdl *get_child_by_index(int32_t index, GpiObjHdl *parent) override;
    GpiObjHdl *get_child_from_handle(void *raw_hdl, GpiObjHdl *parent) override;
    GpiObjHdl *get_root_handle(const char *name) override;
    GpiIterator *iterate_handle(GpiObjHdl *obj_hdl,
                                gpi_iterator_sel type) override;

    /* Callback related, these may (will) return the same handle*/
    gpi_hdl register_timed_callback(uint64_t time, int (*function)(void *),
                                    void *cb_data) override;
    gpi_hdl register_readonly_callback(int (*function)(void *),
                                       void *cb_data) override;
    gpi_hdl register_nexttime_callback(int (*function)(void *),
                                       void *cb_data) override;
    gpi_hdl register_readwrite_callback(int (*function)(void *),
                                        void *cb_data) override;
    int remove_callback(gpi_callback *cb) override;

    /* Method to provide strings from operation types */
    GpiObjHdl *create_gpi_obj_from_handle(void *hdl, const std::string &name,
                                          const std::string &fq_name,
                                          int accType, int accFullType);

    static bool compare_generate_labels(const std::string &a,
                                        const std::string &b);

    void main() noexcept;

  private:
    bool isValueConst(int kind);
    bool isValueLogic(mtiTypeIdT type);
    bool isValueChar(mtiTypeIdT type);
    bool isValueBoolean(mtiTypeIdT type);
    bool isTypeValue(int type);
    bool isTypeSignal(int type, int full_type);

  private:
    // We store the shutdown callback handle here so that if sim_end() is
    // called, it can be removed.
    gpi_callback *m_sim_finish_cb;

    // Cache simulator info
    std::string m_product;
    std::string m_version;
    int m_argc = 0;
    std::vector<std::string> m_argv_storage;
    char const **m_argv = nullptr;

    friend FliSignalObjHdl;
};

#endif /*COCOTB_FLI_IMPL_H_ */
