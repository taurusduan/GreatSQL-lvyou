/* -*- C++ -*- */
/* Copyright (c) 2002, 2022, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2023, GreatDB Software Co., Ltd.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef _SP_PCONTEXT_H_
#define _SP_PCONTEXT_H_

#include <assert.h>
#include <string.h>
#include <sys/types.h>

#include "field_types.h"  // enum_field_types
#include "lex_string.h"

#include "mysql_com.h"
#include "sql/create_field.h"    // Create_field
#include "sql/mem_root_array.h"  // Mem_root_array
#include "sql/sql_error.h"
#include "sql/sql_list.h"

class Item;
class String;
class THD;
class sp_pcontext;
class sp_instr;

/// This class represents a stored program variable or a parameter
/// (also referenced as 'SP-variable').

class sp_variable {
 public:
  enum enum_mode { MODE_IN, MODE_OUT, MODE_INOUT };

  /// Name of the SP-variable.
  LEX_STRING name;

  /// Field-type of the SP-variable.
  enum enum_field_types type;

  /// Mode of the SP-variable.
  enum_mode mode;

  /// The index to the variable's value in the runtime frame.
  ///
  /// It is calculated during parsing and used when creating sp_instr_set
  /// instructions and Item_splocal items. I.e. values are set/referred by
  /// array indexing in runtime.
  uint offset;

  /// Default value of the SP-variable (if any).
  Item *default_value;

  /// Full type information (field meta-data) of the SP-variable.
  Create_field field_def;

  Item *ref_actual_parameter;

 public:
  void reset_ref_actual_param() { ref_actual_parameter = nullptr; }

 public:
  sp_variable(LEX_STRING _name, enum_field_types _type, enum_mode _mode,
              uint _offset)
      : name(_name),
        type(_type),
        mode(_mode),
        offset(_offset),
        default_value(nullptr),
        ref_actual_parameter(nullptr) {}
};

///////////////////////////////////////////////////////////////////////////

/// This class represents an SQL/PSM label. Can refer to the identifier
/// used with the "label_name:" construct which may precede some SQL/PSM
/// statements, or to an implicit implementation-dependent identifier which
/// the parser inserts before a high-level flow control statement such as
/// IF/WHILE/REPEAT/LOOP, when such statement is rewritten into a
/// combination of low-level jump/jump_if instructions and labels.

class sp_label {
 public:
  enum enum_type {
    /// Implicit label generated by parser.
    IMPLICIT,

    /// Label at BEGIN.
    BEGIN,

    /// Label at iteration control
    ITERATION,

    /// Label for jump
    GOTO
  };

  /// Name of the label.
  LEX_CSTRING name;

  /// Instruction pointer of the label.
  uint ip;

  /// Type of the label.
  enum_type type;

  /// Scope of the label.
  class sp_pcontext *ctx;

  /// ip of begin block label.GOTO stmt can't goto
  /// IF,CASE,LOOP stmt,use this to control it.
  uint beginblock_label_ip;

 public:
  sp_label(LEX_CSTRING _name, uint _ip, enum_type _type, sp_pcontext *_ctx)
      : name(_name), ip(_ip), type(_type), ctx(_ctx), beginblock_label_ip(0) {}
};

///////////////////////////////////////////////////////////////////////////

/// This class represents condition-value term in DECLARE CONDITION or
/// DECLARE HANDLER statements. sp_condition_value has little to do with
/// SQL-conditions.
///
/// In some sense, this class is a union -- a set of filled attributes
/// depends on the sp_condition_value::type value.

class sp_condition_value {
 public:
  enum enum_type { ERROR_CODE, SQLSTATE, WARNING, NOT_FOUND, EXCEPTION };

  /// Type of the condition value.
  enum_type type;

  /// SQLSTATE of the condition value.
  char sql_state[SQLSTATE_LENGTH + 1];

  /// MySQL error code of the condition value.
  /// number of user defined exception in pcontext if m_is_user_defined = true .
  uint mysqlerr;

  bool m_is_user_defined;

 public:
  sp_condition_value(uint _mysqlerr)
      : type(ERROR_CODE), mysqlerr(_mysqlerr), m_is_user_defined(false) {}

  sp_condition_value(const char *_sql_state)
      : type(SQLSTATE), m_is_user_defined(false) {
    memcpy(sql_state, _sql_state, SQLSTATE_LENGTH);
    sql_state[SQLSTATE_LENGTH] = 0;
  }

  sp_condition_value(enum_type _type) : type(_type), m_is_user_defined(false) {
    assert(type != ERROR_CODE && type != SQLSTATE);
  }

  /// Print a condition_value in human-readable form.
  ///
  /// @param str The variable to print to.
  void print(String *str) const;

  /// Check if two instances of sp_condition_value are equal or not.
  ///
  /// @param cv another instance of sp_condition_value to check.
  ///
  /// @return true if the instances are equal, false otherwise.
  bool equals(const sp_condition_value *cv) const;
};

/// this class for predefine oracle exception error
class sp_oracle_condition_value : public sp_condition_value {
 public:
  /*
    SQLSTATE in  SQL-2011
    ref: https://en.wikipedia.org/wiki/SQLSTATE

    ER_SIGNAL_EXCEPTION HY000
  */

  static const char *UER_DEFINED_EXCEPTION_SQLSTATE;

  sp_oracle_condition_value(uint _mysqlerr) : sp_condition_value(_mysqlerr) {
    sql_state[0] = '\0';
  }
  // set predefine oracle exception name
  sp_oracle_condition_value(uint _mysqlerr, const char *_sql_state)
      : sp_condition_value(_mysqlerr) {
    memcpy(sql_state, _sql_state, SQLSTATE_LENGTH);
    sql_state[SQLSTATE_LENGTH] = 0;
  }

  sp_oracle_condition_value()
      : sp_condition_value(UER_DEFINED_EXCEPTION_SQLSTATE) {
    m_is_user_defined = true;
    mysqlerr = ER_SIGNAL_EXCEPTION;
  }

  /// set sp_condition value
  void set_redirect_err_to_user_defined(uint _mysqlerr);
};

///////////////////////////////////////////////////////////////////////////

/// This class represents 'DECLARE CONDITION' statement.
/// sp_condition has little to do with SQL-conditions.

class sp_condition {
 public:
  /// Name of the condition.
  LEX_STRING name;

  /// Value of the condition.
  sp_condition_value *value;

 public:
  sp_condition(LEX_STRING _name, sp_condition_value *_value)
      : name(_name), value(_value) {}
};

///////////////////////////////////////////////////////////////////////////

/**
  class sp_pcursor.
  Stores information about a cursor:
  - Cursor's name in LEX_STRING.
  - Cursor's formal parameter descriptions.

    Formal parameter descriptions reside in a separate context block,
    pointed by the "m_param_context" member.

    m_param_context can be NULL. This means a cursor with no parameters.
    Otherwise, the number of variables in m_param_context means
    the number of cursor's formal parameters.

    Note, m_param_context can be not NULL, but have no variables.
    This is also means a cursor with no parameters (similar to NULL).
*/
class sp_pcursor : public LEX_CSTRING {
  class sp_pcontext *m_param_context;  // Formal parameters
  int m_offset;
  sp_variable *m_cursor_spv;
  /* Type c is ref cursor return t1%rowtype/record
  or cursor c return t1%rowtype/record is select_stmt
  */
  Table_ident *m_table_ref;
  Row_definition_list *m_rdl;
  int m_cursor_rowtype_offset;

 public:
  sp_pcursor(const LEX_CSTRING name, class sp_pcontext *param_ctx, int offset,
             sp_variable *cursor_spv)
      : LEX_CSTRING(name),
        m_param_context(param_ctx),
        m_offset(offset),
        m_cursor_spv(cursor_spv),
        m_table_ref(nullptr),
        m_rdl(nullptr),
        m_cursor_rowtype_offset(-1) {}
  class sp_pcontext *param_context() const {
    return m_param_context;
  }
  bool check_param_count_with_error(uint param_count) const;
  uint get_offset() const { return m_offset; }
  sp_variable *get_cursor_spv() { return m_cursor_spv; }
  void set_table_ref(Table_ident *table_ref) { m_table_ref = table_ref; }
  Table_ident *get_table_ref() { return m_table_ref; }
  void set_row_definition_list(Row_definition_list *rdl) { m_rdl = rdl; }
  Row_definition_list *get_row_definition_list() { return m_rdl; }
  void set_cursor_rowtype_offset(int offset) {
    m_cursor_rowtype_offset = offset;
  }
  int get_cursor_rowtype_offset() { return m_cursor_rowtype_offset; }
  bool has_return_type() {
    return m_table_ref || m_rdl || m_cursor_rowtype_offset != -1;
  }
};

///////////////////////////////////////////////////////////////////////////

/// This class represents 'DECLARE HANDLER' statement.

class sp_handler {
 public:
  /// Enumeration of possible handler types.
  /// Note: UNDO handlers are not (and have never been) supported.
  enum enum_type { EXIT, CONTINUE };

  /// Handler type.
  enum_type type;

  /// BEGIN..END block of the handler.
  sp_pcontext *scope;

  /// Conditions caught by this handler.
  List<const sp_condition_value> condition_values;

 public:
  /// The constructor.
  ///
  /// @param _type    SQL-handler type.
  /// @param _scope   Handler scope.
  sp_handler(enum_type _type, sp_pcontext *_scope)
      : type(_type), scope(_scope) {}

  /// Print all conditions of a handler in human-readable form.
  ///
  /// @param str The variable to print to.
  void print_conditions(String *str) const;

  /// Print type and conditions (but not body) of a handler.
  ///
  /// @param str The variable to print to.
  void print(String *str) const;
};

///////////////////////////////////////////////////////////////////////////

/// The class represents parse-time context, which keeps track of declared
/// variables/parameters, conditions, handlers, cursors and labels.
///
/// sp_context objects are organized in a tree according to the following
/// rules:
///   - one sp_pcontext object corresponds for for each BEGIN..END block;
///   - one sp_pcontext object corresponds for each exception handler;
///   - one additional sp_pcontext object is created to contain
///     Stored Program parameters.
///
/// sp_pcontext objects are used both at parse-time and at runtime.
///
/// During the parsing stage sp_pcontext objects are used:
///   - to look up defined names (e.g. declared variables and visible
///     labels);
///   - to check for duplicates;
///   - for error checking;
///   - to calculate offsets to be used at runtime.
///
/// During the runtime phase, a tree of sp_pcontext objects is used:
///   - for error checking (e.g. to check correct number of parameters);
///   - to resolve SQL-handlers.

class sp_pcontext {
 public:
  enum enum_scope {
    /// REGULAR_SCOPE designates regular BEGIN ... END blocks.
    REGULAR_SCOPE,

    /// HANDLER_SCOPE designates SQL-handler blocks.
    HANDLER_SCOPE
  };

 public:
  sp_pcontext(THD *thd);
  ~sp_pcontext();

  /// Create and push a new context in the tree.

  /// @param thd   thread context.
  /// @param scope scope of the new parsing context.
  /// @return the node created.
  sp_pcontext *push_context(THD *thd, enum_scope scope);

  /// Pop a node from the parsing context tree.
  /// @return the parent node.
  sp_pcontext *pop_context();

  sp_pcontext *parent_context() const { return m_parent; }

  sp_pcontext *child_context(uint i) const {
    return i < m_children.size() ? m_children.at(i) : NULL;
  }

  int get_level() const { return m_level; }

  /// Calculate and return the number of handlers to pop between the given
  /// context and this one.
  ///
  /// @param ctx       the other parsing context.
  /// @param exclusive specifies if the last scope should be excluded.
  ///
  /// @return the number of handlers to pop between the given context and
  /// this one.  If 'exclusive' is true, don't count the last scope we are
  /// leaving; this is used for LEAVE where we will jump to the hpop
  /// instructions.
  size_t diff_handlers(const sp_pcontext *ctx, bool exclusive) const;

  /// Calculate and return the number of cursors to pop between the given
  /// context and this one.
  ///
  /// @param ctx       the other parsing context.
  /// @param exclusive specifies if the last scope should be excluded.
  ///
  /// @return the number of cursors to pop between the given context and
  /// this one.  If 'exclusive' is true, don't count the last scope we are
  /// leaving; this is used for LEAVE where we will jump to the cpop
  /// instructions.
  size_t diff_cursors(const sp_pcontext *ctx, bool exclusive) const;

  /////////////////////////////////////////////////////////////////////////
  // SP-variables (parameters and variables).
  /////////////////////////////////////////////////////////////////////////

  /// @return the maximum number of variables used in this and all child
  /// contexts. For the root parsing context, this gives us the number of
  /// slots needed for variables during the runtime phase.
  uint max_var_index() const { return m_max_var_index; }
  uint max_udt_var_index() const { return m_max_udt_var_index; }

  /// @return the current number of variables used in the parent contexts
  /// (from the root), including this context.
  uint current_var_count() const {
    return m_var_offset + static_cast<uint>(m_vars.size());
  }

  /// @return the number of variables in this context alone.
  uint context_var_count() const { return static_cast<uint>(m_vars.size()); }

  uint context_udt_var_count() const {
    return static_cast<uint>(m_udt_vars.size());
  }

  /// COPY  vals default value to args
  ////@param THD
  ////@param  default_value list
  /// @retval true if  clone default item failed
  /// @retval false otherwise.
  bool copy_default_params(mem_root_deque<Item *> *args);

  /// @return map index in this parsing context to runtime offset.
  uint var_context2runtime(uint i) const { return m_var_offset + i; }

  /*
    Return the i-th last context variable.
    If i is 0, then return the very last variable in m_vars.
  */
  sp_variable *get_last_context_variable(uint i = 0) const {
    assert(i < m_vars.size());
    return m_vars.at(m_vars.size() - i - 1);
  }

  /// Add SP-variable to the parsing context.
  ///
  /// @param thd  Thread context.
  /// @param name Name of the SP-variable.
  /// @param type Type of the SP-variable.
  /// @param mode Mode of the SP-variable.
  ///
  /// @return instance of newly added SP-variable.
  sp_variable *add_variable(THD *thd, LEX_STRING name,
                            enum enum_field_types type,
                            sp_variable::enum_mode mode);
  Item *make_item_plsql_cursor_attr(THD *thd, LEX_CSTRING name, uint offset);
  Item *make_item_plsql_sp_for_loop_attr(THD *thd, int m_direction, Item *var,
                                         Item *value_to);
  Item *make_item_plsql_plus_one(THD *thd, POS &pos, int direction,
                                 Item_splocal *item_splocal);

  /// Retrieve full type information about SP-variables in this parsing
  /// context and its children.
  ///
  /// @param [out] field_def_lst Container to store type information.
  /// @param [out] vars_list Container to store sp_variable information.
  /// @param [out] vars_udt_list Container to store udt sp_variable information.
  /// @param [out] udt_var_count_diff udt counts after retrieve.
  void retrieve_field_definitions(List<Create_field> *field_def_lst,
                                  List<sp_variable> *vars_list,
                                  List<sp_variable> *vars_udt_list,
                                  uint *udt_var_count_diff) const;

  void retrieve_udt_field_definitions(THD *thd,
                                      List<sp_variable> *vars_list) const;

  bool resolve_udt_create_field_list(THD *thd, Create_field *def,
                                     List<sp_variable> *vars_udt_list,
                                     uint *udt_var_count_diff) const;

  /* ident t1%rowtype and ident t1.udt%type,if there is Field_udt_type,
    must add it to vars_udt_list.
  */
  bool add_udt_to_sp_var_list(THD *thd, List<Create_field> field_def_lst,
                              Create_field *cdf_mas,
                              List<sp_variable> *vars_udt_list,
                              uint *udt_var_count_diff) const;

  /// resolve Row_definition_list's record table members,as table of t1%ROWTYPE
  /// may be changed before.
  ///
  /// @param list   the source Row_definition_list.
  ///
  /// @return false if success, or true in case of error.
  bool resolve_the_definitions(Row_definition_list *list) const;

  /// resolve Row_definition_table_list's member,as table of t1%ROWTYPE
  /// may be changed before.
  ///
  /// @param def   the source Create_field.
  ///
  /// @return false if success, or true in case of error.
  bool resolve_table_definitions(Create_field *def) const;

  bool resolve_create_field_definitions(Create_field *def) const;

  /// return the i-th variable on the current context
  sp_variable *get_context_variable(uint i) const {
    assert(i < m_vars.size());
    return m_vars.at(i);
  }

  sp_variable *get_context_udt_variable(uint i) const {
    assert(i < m_udt_vars.size());
    return m_udt_vars.at(i);
  }

  /// Find SP-variable by name.
  ///
  /// The function does a linear search (from newer to older variables,
  /// in case we have shadowed names).
  ///
  /// The function is called only at parsing time.
  ///
  /// @param name               Variable name.
  /// @param name_len           Variable name length.
  /// @param current_scope_only A flag if we search only in current scope.
  ///
  /// @return instance of found SP-variable, or NULL if not found.
  sp_variable *find_variable(const char *name, size_t name_len,
                             bool current_scope_only) const;

  sp_variable *find_udt_variable(const char *name, size_t name_len,
                                 const char *db_name, size_t db_name_len,
                                 bool current_scope_only) const;

  /// Make new udt sp_variable.
  ///
  /// @param thd              Thread
  /// @param rdl              Row_definition_list
  /// @param name             create_field name.
  /// @param table_type       create_field type
  /// @param varray_limit     varray limit
  /// @param nested_table_udt udt nested name
  /// @param udt_db_name      db name
  /// @param spvar            sp_variable.
  ///
  /// @return 1 if error,0 if right,-1 if need add new udt var.
  int make_udt_variable(THD *thd, Row_definition_list *rdl, LEX_STRING name,
                        uint table_type, ulonglong varray_limit,
                        LEX_CSTRING nested_table_udt, LEX_STRING udt_db_name,
                        sp_variable *spvar_udt) const;

  sp_variable *add_udt_variable(THD *thd, Row_definition_list *rdl,
                                LEX_STRING name, uint table_type,
                                enum enum_field_types type,
                                ulonglong varray_limit,
                                LEX_CSTRING nested_table_udt,
                                LEX_STRING udt_db_name);

  /// Find SP-variable by the offset in the root parsing context.
  ///
  /// The function is used for two things:
  /// - When evaluating parameters at the beginning, and setting out parameters
  ///   at the end, of invocation. (Top frame only, so no recursion then.)
  /// - For printing of sp_instr_set. (Debug mode only.)
  ///
  /// @param offset Variable offset in the root parsing context.
  ///
  /// @return instance of found SP-variable, or NULL if not found.
  sp_variable *find_variable(uint offset) const;

  /// Set the current scope boundary (for default values).
  ///
  /// @param n The number of variables to skip.
  void declare_var_boundary(uint n) { m_pboundary = n; }

  /////////////////////////////////////////////////////////////////////////
  // CASE expressions.
  /////////////////////////////////////////////////////////////////////////

  int get_num_case_exprs() const { return m_num_case_exprs; }

  int push_case_expr_id() {
    if (m_case_expr_ids.push_back(m_num_case_exprs)) return -1;

    return m_num_case_exprs++;
  }

  void pop_case_expr_id() { m_case_expr_ids.pop_back(); }

  int get_current_case_expr_id() const { return m_case_expr_ids.back(); }

  /////////////////////////////////////////////////////////////////////////
  // Labels.
  /////////////////////////////////////////////////////////////////////////

  sp_label *push_label(THD *thd, LEX_CSTRING name, uint ip);

  sp_label *find_label(LEX_CSTRING name);

  /*for goto stmt.
          (1)for next case,it's unallowed.
          BEGIN
            WHILE 1=1 loop
            <<lab18_1>>
              a := 2;
            END LOOP;
            WHILE 1=1 loop
              goto lab18_1; ==> find_label_by_ip() == false
            END LOOP;
          END;
          (2)for next case,it's allowed.
          BEGIN
            WHILE 1=1 loop
            <<lab18_1>>
              WHILE 1=1 loop
                goto lab18_1; ==> find_label_by_ip() == true
              END LOOP;
            END LOOP;
          END;
    @param name ip of sp_label.
    @return true if found SP-label, or false if not found.
  */
  bool find_label_by_ip(uint ip);

  sp_label *last_label() {
    sp_label *label = m_labels.head();

    if (!label && m_parent) label = m_parent->last_label();

    return label;
  }

  sp_label *find_label_current_loop_start();

  sp_label *pop_label() { return m_labels.pop(); }

  sp_label *push_goto_label(THD *thd, LEX_CSTRING name, uint ip);

  sp_label *find_goto_label(LEX_CSTRING name, bool recusive);

  sp_label *last_goto_label() { return m_goto_labels.head(); }

  void push_unique_goto_label(sp_label *a);

  /////////////////////////////////////////////////////////////////////////
  // Conditions.
  /////////////////////////////////////////////////////////////////////////

  bool add_condition(THD *thd, LEX_STRING name, sp_condition_value *value);

  /// See comment for find_variable() above.
  sp_condition_value *find_condition(LEX_STRING name,
                                     bool current_scope_only) const;

  sp_condition_value *find_declared_or_predefined_condition(
      LEX_STRING name) const;

  bool declared_or_predefined_condition(LEX_STRING name, sp_instr *i);
  /////////////////////////////////////////////////////////////////////////
  // Handlers.
  /////////////////////////////////////////////////////////////////////////

  sp_handler *add_handler(THD *thd, sp_handler::enum_type type);

  /// This is an auxiliary parsing-time function to check if an SQL-handler
  /// exists in the current parsing context (current scope) for the given
  /// SQL-condition. This function is used to check for duplicates during
  /// the parsing phase.
  ///
  /// This function can not be used during the runtime phase to check
  /// SQL-handler existence because it searches for the SQL-handler in the
  /// current scope only (during runtime, current and parent scopes
  /// should be checked according to the SQL-handler resolution rules).
  ///
  /// @param cond_value      the handler condition value
  ///                        (not SQL-condition!).
  ///
  /// @retval true if such SQL-handler exists.
  /// @retval false otherwise.
  bool check_duplicate_handler(const sp_condition_value *cond_value) const;

  /// Find an SQL handler for the given SQL condition according to the
  /// SQL-handler resolution rules. This function is used at runtime.
  ///
  /// @param sql_state        The SQL condition state
  /// @param sql_errno        The error code
  /// @param severity         The SQL condition severity level
  ///
  /// @return a pointer to the found SQL-handler or NULL.
  sp_handler *find_handler(const char *sql_state, uint sql_errno,
                           Sql_condition::enum_severity_level severity,
                           sp_condition_value *user_defined = nullptr) const;

  /////////////////////////////////////////////////////////////////////////
  // Cursors.
  /////////////////////////////////////////////////////////////////////////

  bool add_cursor(LEX_STRING name);

  sp_pcursor *find_cursor_parameters(uint offset) const;

  bool add_cursor_parameters(THD *thd, const LEX_STRING name,
                             sp_pcontext *param_ctx, uint *offset,
                             sp_variable *cursor_spv);

  /// See comment for find_variable() above.
  bool find_cursor(LEX_STRING name, uint *poff, bool current_scope_only) const;

  /// Find cursor by offset (for debugging only).
  const LEX_STRING *find_cursor(uint offset) const;

  uint max_cursor_index() const {
    return m_max_cursor_index + static_cast<uint>(m_cursors.size());
  }

  uint current_cursor_count() const {
    return m_cursor_offset + static_cast<uint>(m_cursors.size());
  }

 private:
  /// Constructor for a tree node.
  /// @param thd  thread context
  /// @param prev the parent parsing context
  /// @param scope scope of this parsing context
  sp_pcontext(THD *thd, sp_pcontext *prev, enum_scope scope);

  void init(uint var_offset, uint cursor_offset, uint udt_var_offset,
            int num_case_expressions);

  /* Prevent use of these */
  sp_pcontext(const sp_pcontext &);
  void operator=(sp_pcontext &);

 private:
  /// Level of the corresponding BEGIN..END block (0 means the topmost block).
  int m_level;

  /// m_max_var_index -- number of variables (including all types of arguments)
  /// in this context including all children contexts.
  ///
  /// m_max_var_index >= m_vars.size().
  ///
  /// m_max_var_index of the root parsing context contains number of all
  /// variables (including arguments) in all enclosed contexts.
  uint m_max_var_index;

  /// The maximum sub context's framesizes.
  uint m_max_cursor_index;

  /// The maximum sub context's framesizes.
  uint m_max_udt_var_index;

  /// Parent context.
  sp_pcontext *m_parent;

  /// An index of the first SP-variable in this parsing context. The index
  /// belongs to a runtime table of SP-variables.
  ///
  /// Note:
  ///   - m_var_offset is 0 for root parsing context;
  ///   - m_var_offset is different for all nested parsing contexts.
  uint m_var_offset;

  /// Cursor offset for this context.
  uint m_cursor_offset;

  /// udt offset for this context.
  uint m_udt_var_offset;

  /// Boundary for finding variables in this context. This is the number of
  /// variables currently "invisible" to default clauses. This is normally 0,
  /// but will be larger during parsing of DECLARE ... DEFAULT, to get the
  /// scope right for DEFAULT values.
  uint m_pboundary;

  int m_num_case_exprs;

  int m_user_defined_exception;

  /// SP parameters/variables.
  Mem_root_array<sp_variable *> m_vars;

  /// SP parameters/variables.
  Mem_root_array<sp_variable *> m_udt_vars;

  /// Stack of CASE expression ids.
  Mem_root_array<int> m_case_expr_ids;

  /// Stack of SQL-conditions.
  Mem_root_array<sp_condition *> m_conditions;

  /// Stack of cursors.
  Mem_root_array<LEX_STRING> m_cursors;

  /// Stack of cursor's parameters.
  Mem_root_array<sp_pcursor *> m_cursor_vars;

  /// Stack of SQL-handlers.
  Mem_root_array<sp_handler *> m_handlers;

  /*
   In the below example the label <<lab>> has two meanings:
   - GOTO lab : must go before the beginning of the loop
   - CONTINUE lab : must go to the beginning of the loop
   We solve this by storing block labels and goto labels into separate lists.

   BEGIN
     <<lab>>
     FOR i IN a..10 LOOP
       ...
       GOTO lab;
       ...
       CONTINUE lab;
       ...
     END LOOP;
   END;
  */
  /// List of labels.
  List<sp_label> m_labels;

  /// List of goto labels
  List<sp_label> m_goto_labels;

  /// Children contexts, used for destruction.
  Mem_root_array<sp_pcontext *> m_children;

  /// Scope of this parsing context.
  enum_scope m_scope;
};

#endif /* _SP_PCONTEXT_H_ */
