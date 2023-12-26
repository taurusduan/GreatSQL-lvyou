/* Copyright (c) 2023, GreatDB Software Co., Ltd. All rights reserved.

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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/
#include "sql/iterators/gdb_connect_by_iterator.h"
#include <assert.h>
#include "sql/item_sum.h"
#include "sql/iterators/basic_row_iterators.h"
#include "sql/iterators/composite_iterators.h"
#include "sql/iterators/ref_row_iterators.h"
#include "sql/iterators/timing_iterator.h"
#include "sql/log.h"
#include "sql/sql_executor.h"
#include "sql/sql_optimizer.h"
#if 0
#define DUMP_TB_VAL(tb)                                          \
  DBUG_EXECUTE_IF("connect_by", {                                \
    StringBuffer<MAX_FIELD_WIDTH> str_buffer;                    \
    String str_value;                                            \
    sql_print_information("" #tb "start:%p", tb);                \
    for (Field **fi = tb->visible_field_ptr(); *fi; fi++) {      \
      String *str_v = (*fi)->val_str(&str_buffer, &str_value);   \
      sql_print_information(" field: %s  %s", (*fi)->field_name, \
                            str_v->c_ptr());                     \
    }                                                            \
    sql_print_information("record end" #tb);                     \
  };);
#else
#define DUMP_TB_VAL(tb)
#endif

bool init_tmp_table(THD *thd, TABLE *table) {
  assert(table);
  if (!table->is_created()) {
    if (instantiate_tmp_table(thd, table)) {
      return true;
    }
    empty_record(table);
  } else {
    if (table->file->inited) {
      // If we're being called several times (in particular, as part of a
      // LATERAL join), the table iterator may have started a scan, so end it
      // before we start our own.
      table->file->ha_index_or_rnd_end();
    }
    // tmp table is the same trunc
    table->file->ha_delete_all_rows();
  }
  return false;
}

/**
 * @brief save table record[0]
 *
 *
 * @param thd[in]
 * @param table[in]
 * @param rows[out]
 * @return true
 * @return false
 */
bool write_to_tb(THD *thd, TABLE *table, ha_rows &rows) {
  DUMP_TB_VAL(table);
  auto error = table->file->ha_write_row(table->record[0]);
  if (error != 0) {
    if (!table->file->is_ignorable_error(error)) {
      bool is_duplicate;
      if (create_ondisk_from_heap(thd, table, error, true, true,
                                  &is_duplicate)) {
        return true; /* purecov: inspected */
      }
      if (!is_duplicate) {
        rows++;
      }
    }
  } else {
    rows++;
  }
  return false;
}

/**
 * @brief truncate table
 *
 * @param table[in]
 */
bool clear_data(TABLE *table) {
  if (table) {
    auto err = table->file->delete_all_rows();
    if (err != 0) {
      table->file->print_error(err, MYF(0));
      return true;
    }
    if (table->blob_storage) table->blob_storage->reset();
  }
  return false;
}

bool TempTableParam::Destory() {
  if (tab) {
    if (tab->file->inited) {
      tab->file->ha_index_or_rnd_end();
    }
    if (tab->blob_storage) destroy(tab->blob_storage);

    close_tmp_table(tab);
    free_tmp_table(tab);
  }
  param->cleanup();
  return false;
}

bool TempStore::Destory() {
  fields.clear();
  return false;
}

bool param_Init(THD *thd, Connect_by_param *param) {
  assert(param);
  auto init = [](THD *thd_, TempTableParam *t) -> bool {
    if (t) {
      if (init_tmp_table(thd_, t->tab)) return true;
    }
    return false;
  };

  if (param->cache && init(thd, param->cache.get())) return true;
  if (param->stack && init(thd, param->stack.get())) return true;
  if (param->history && init(thd, param->history.get())) return true;
  if (param->start_rownum_it) {
    param->start_rownum_it->reset_value();
  }
  return false;
}

TempStore *CreateTempTab(THD *thd, const char *alias, Query_block *query_block,
                         uint ref_item_slice_size,
                         mem_root_deque<Item *> *fields,
                         Temp_table_param &tmp_table_param,
                         longlong opt_connect_by) {
  auto temp = new (thd->mem_root) TempStore(thd, ref_item_slice_size);
  if (temp == nullptr) return nullptr;

  temp->param =
      new (thd->mem_root) Temp_table_param(thd->mem_root, tmp_table_param);
  if (temp->param == nullptr) return nullptr;

  temp->param->allow_connect_by_tmp_table = opt_connect_by;

  temp->param->skip_create_table = true;
  temp->tab =
      create_tmp_table(thd, temp->param, *fields, nullptr, false, false,
                       query_block->active_options(), HA_POS_ERROR, alias);
  if (temp->tab == nullptr) return nullptr;
  if (change_to_use_tmp_fields_except_sums_or_connect_by(
          fields, thd, query_block, temp->ref_items, &temp->fields,
          query_block->m_added_non_hidden_fields, opt_connect_by))
    return nullptr;

  if (temp->tab->s->blob_fields) {
    temp->tab->blob_storage = new (thd->mem_root) Blob_mem_storage();
    if (temp->tab->blob_storage == nullptr) return nullptr;
  }
  return temp;
}

/**
 * create btree idx 0 on tab
 */
bool create_index_for_table(Field *f, TABLE *tab) {
  // index always
  assert(f && tab);
  f->part_of_key.set_bit(0);
  f->set_flag(PART_KEY_FLAG);
  Field_map used_fields;
  used_fields.clear_all();
  used_fields.set_bit(f->field_index());

  if (tab->alloc_tmp_keys(1, 1, true) ||
      !tab->add_tmp_key(&used_fields, false, true)) {
    return true;
  }
  return false;
}
/**
 * @brief Create a index for cond object
 *
 * @param thd
 * @param idx_list
 * @param val_list
 * @param tab
 * @param const_table_map
 * @param ref
 *
 * @return true  create index failure
 * @return false
 */
bool create_index_for_cond(THD *thd, mem_root_deque<Item *> *idx_list,
                           mem_root_deque<Item *> *val_list, TABLE *tab,
                           table_map const_table_map, Index_lookup *ref) {
  assert(!idx_list->empty());
  Field_map used_fields;
  used_fields.clear_all();
  uint keyno = 1;
  mem_root_deque<uint16> sort_index(thd->mem_root);
  mem_root_deque<uint16> indexes(thd->mem_root);
  for (uint i = 0; i < idx_list->size(); i++) {
    auto it = idx_list->at(i);
    auto it_field = it->get_tmp_table_field();
    // not allow blob use index
    if (it_field->is_flag_set(BLOB_FLAG)) return true;

    it_field->part_of_key.set_bit(keyno);
    it_field->set_flag(PART_KEY_FLAG);
    used_fields.set_bit(it_field->field_index());
    // map order
    sort_index.push_back(it_field->field_index());
    indexes.push_back(i);
  }
  std::sort(indexes.begin(), indexes.end(), [&](const int &a, const int &b) {
    return sort_index[a] < sort_index[b];
  });

  // level_idx , and connect by field idx
  if (tab->alloc_tmp_keys(/*1 + 1*/ 2, 1 + idx_list->size(), true) ||
      !tab->add_tmp_key(&used_fields, false, true)) {
    return true;
  }

  auto key = &tab->key_info[1];
  assert(key);
  auto parts = actual_key_parts(key);
  if (init_ref(thd, parts, key->key_length, 1, ref)) {
    return true;
  }
  assert(parts == idx_list->size());
  uchar *key_buff = ref->key_buff;
  for (uint i = 0; i < parts; i++) {
    // key parts is order by table field_index()

    auto val = val_list->at(indexes[i]);
    /*
    left.field = right.keypart OR right.keypart IS NULL.

    const bool null_rejecting = cond->functype() != Item_func::EQUAL_FUNC &&
                                cond->functype() != Item_func::ISNULL_FUNC &&
                                cond->functype() != Item_func::ISNOTNULL_FUNC;
    */
    const KEY_PART_INFO *keyinfo = &key->key_part[i];
    if (init_ref_part(thd, i, val, nullptr, true, const_table_map,
                      val->used_tables(), keyinfo->null_bit, keyinfo, key_buff,
                      ref))
      return true;
    key_buff += keyinfo->store_length;
  }
  return false;
}

bool SetupConnectByTmp(THD *thd, JOIN *join, uint qep_tab_n,
                       mem_root_deque<Item *> *curr_fields,
                       Temp_table_param &tmp_table_param) {
  mem_root_deque<Item *> cond_by_idx_list(thd->mem_root);
  mem_root_deque<Item *> connect_func_cond(thd->mem_root);
  List<Item> filter_cond;
  List<Item> filter_cond2;
  List<Item> rownum_filter_cond;

  mem_root_deque<Item_func_prior *> cond_cits(thd->mem_root);

  Item_func_rownum *start_rownum_it{nullptr};

  if (join->start_with_cond)
    if (join->start_with_cond->has_rownum_expr()) {
      Item *new_item = join->start_with_cond->transform(
          &Item::replace_rownum_func, pointer_cast<uchar *>(&start_rownum_it));
      if (!new_item) return true;
      if (new_item != join->start_with_cond) {
        join->start_with_cond = new_item;
      }
    }

  // only get by conect by cond use for check uniq history
  join->connect_by_cond->walk(&Item::collect_connect_by_item_processor,
                              enum_walk::POSTFIX, (uchar *)(&cond_cits));

  if (connect_by_cond_lookup_ref(thd, join->connect_by_cond, &cond_by_idx_list,
                                 &connect_func_cond, &filter_cond,
                                 &rownum_filter_cond, &filter_cond2))
    return true;

  QEP_TAB *tab = &join->qep_tab[qep_tab_n];
  auto connect_by_param = new (thd->mem_root)
      Connect_by_param(thd, join->start_with_cond, join->connect_by_cond,
                       join->query_block->connect_by_nocycle);
  if (connect_by_param == nullptr) return true;
  if (start_rownum_it) {
    connect_by_param->start_rownum_it = start_rownum_it;
  }

  Item_connect_by_func_level *level_it = nullptr;
  for (Item *it : *curr_fields) {
    if (it->has_connect_by_func()) {
      if (it->type() == Item::CONNECT_BY_FUNC_ITEM) {
        auto cit = dynamic_cast<Item_connect_by_func *>(it);
        if (!cit) return true;
        if (!level_it &&
            cit->ConnectBy_func() == Item_connect_by_func::LEVEL_FUNC) {
          level_it = dynamic_cast<Item_connect_by_func_level *>(cit);
        }
        switch (cit->ConnectBy_func()) {
          case Item_connect_by_func::ISLEAF_FUNC:
            connect_by_param->func_leaf_list.push_back(cit);
            break;
          case Item_connect_by_func::ISCYCLE_FUNC:
            /* code */
            connect_by_param->func_cycle_list.push_back(cit);
            break;
          default:
            connect_by_param->connect_by_func_list.push_back(cit);
            break;
        }
      } else {
        it->walk(&Item::collect_connect_by_leaf_cycle_or_ref_processor,
                 enum_walk::POSTFIX, (uchar *)connect_by_param);
      }
    }
  }
  if (!level_it) {
    assert(level_it);
    return true;
  }
  connect_by_param->level_it = level_it;
  auto query_block = join->query_block;
  auto ref_item_size = join->ref_items[0].size();
  // copy all result into start with if start with is empty

  auto result_table = CreateTempTab(
      thd, "<connect_by_cache>", query_block, ref_item_size, curr_fields,
      tmp_table_param, (EXCEPT_CONNECT_BY_FUNC | EXCEPT_RAND_FUNC));
  if (!result_table) return true;
  if (result_table->tab->connect_by_field)
    if (create_index_for_table(result_table->tab->connect_by_field,
                               result_table->tab))
      return true;

  // create index
  if (!cond_by_idx_list.empty()) {
    Index_lookup *ref = new (thd->mem_root) Index_lookup;

    if (!create_index_for_cond(thd, &cond_by_idx_list, &connect_func_cond,
                               result_table->tab, join->const_table_map, ref)) {
      if (thd->is_error()) return true;
      connect_by_param->ref = ref;
      connect_by_param->connect_by_cond = CreateConjunction(&filter_cond);
    } else {
      filter_cond.concat(&filter_cond2);
      connect_by_param->connect_by_cond = CreateConjunction(&filter_cond);
    }
  }
  //
  if (!rownum_filter_cond.is_empty()) {
    connect_by_param->connect_by_rownum_it =
        CreateConjunction(&rownum_filter_cond);
  }

  connect_by_param->cache.reset(result_table);

  join->copy_ref_item_slice(join->ref_items[REF_SLICE_ACTIVE],
                            result_table->ref_items);

  auto stack =
      CreateTempTab(thd, "<connect_by_stack>", query_block, ref_item_size,
                    &result_table->fields, tmp_table_param,
                    (EXCEPT_CONNECT_BY_VALUE | EXCEPT_RAND_FUNC));
  if (!stack) return true;

  auto level_it_field = level_it->get_result_field();
  assert(level_it_field);
  if (!level_it_field) {
    my_error(ER_INDEX_CORRUPT, MYF(0), "<connect_by_stack>");
    return true;
  }

  if (create_index_for_table(level_it_field, stack->tab)) return true;

  connect_by_param->stack.reset(stack);

  join->copy_ref_item_slice(join->ref_items[REF_SLICE_ACTIVE],
                            stack->ref_items);
  // setup join to stack temp table
  for (auto it : connect_by_param->connect_by_func_list) {
    if (it->setup(thd)) return true;
  }

  if (!cond_cits.empty()) {
    mem_root_deque<Item *> history_fields(thd->mem_root);
    for (auto it : cond_cits) {
      history_fields.push_back(it->Ref_item());
    }
    if (!history_fields.empty()) {
      connect_by_param->history =
          make_unique_destroy_only<TempTableParam>(thd->mem_root);
      if (!connect_by_param->history) return true;
      connect_by_param->history->param = new (thd->mem_root) Temp_table_param;
      if (connect_by_param->history->param == nullptr) return true;
      count_field_types(query_block, connect_by_param->history->param,
                        history_fields, false, false);
      connect_by_param->history->param->skip_create_table = true;
      connect_by_param->history->tab = create_tmp_table(
          thd, connect_by_param->history->param, history_fields, nullptr, true,
          false, query_block->active_options(), HA_POS_ERROR,
          "<conect_by_history>");
      if (connect_by_param->history->tab == nullptr) return true;
    }
  }

  tab->connect_by = connect_by_param;

  curr_fields = &stack->fields;
  // Need to set them now for correct group_fields setup, reset at the end.
  {
    tab->tmp_table_param =
        new (thd->mem_root) Temp_table_param(thd->mem_root, tmp_table_param);
    if (tab->tmp_table_param == nullptr) {
      return true;
    }
    // tab->tmp_table_param ->precomputed_group_by = false;
    tab->tmp_table_param->skip_create_table = true;
    TABLE *res_table =
        create_tmp_table(thd, tab->tmp_table_param, *curr_fields, nullptr,
                         false, false, join->query_block->active_options(),
                         HA_POS_ERROR, "<connect_by_result>");
    if (res_table == nullptr) {
      tab->set_table(nullptr);
      return true;
    }
    join->tmp_table_param->using_outer_summary_function =
        tab->tmp_table_param->using_outer_summary_function;
    assert(tab->idx() > 0);
    tab->set_table(res_table);
  }
  if (join->alloc_ref_item_slice(thd, REF_SLICE_CONNECT_BY_RES)) return true;
  if (change_to_use_tmp_fields_except_sums(
          curr_fields, thd, join->query_block,
          join->ref_items[REF_SLICE_CONNECT_BY_RES],
          &(join->tmp_fields[REF_SLICE_CONNECT_BY_RES]),
          join->query_block->m_added_non_hidden_fields))
    return true;
  // repalce rownum
  if (join->after_connect_by_cond && query_block->has_rownum_in_cond) {
    Item *new_cond = join->after_connect_by_cond->transform(
        &Item::replace_rownum_use_tmp_field, nullptr);
    if (!new_cond) return true;
    if (new_cond != join->after_connect_by_cond) {
      join->after_connect_by_cond = new_cond;
    }
  }
  return false;
}

template <typename Profiler>
class ConnectbyIterator final : public TableRowIterator {
  Temp_table_param *m_result_table_param;
  unique_ptr_destroy_only<RowIterator> m_src_iter;
  unique_ptr_destroy_only<RowIterator> m_table_iterator;
  Connect_by_param *m_param;
  JOIN *m_join;
  int m_in_ref_slice;
  int m_out_ref_slice;

 public:
  ConnectbyIterator(THD *thd, JOIN *join, TABLE *table,
                    Temp_table_param *temp_table_param,
                    unique_ptr_destroy_only<RowIterator> source,
                    unique_ptr_destroy_only<RowIterator> result,
                    Connect_by_param *connect_by_param, int ref_slice)
      : TableRowIterator(thd, table),
        m_result_table_param(temp_table_param),
        m_src_iter(move(source)),
        m_table_iterator(move(result)),
        m_param(connect_by_param),
        m_join(join),
        m_in_ref_slice(0),
        m_out_ref_slice(ref_slice) {}

  bool Init() override;

  int Read() override;

  void SetNullRowFlag(bool is_null_row) override {
    m_table_iterator->SetNullRowFlag(is_null_row);
  }
  void EndPSIBatchModeIfStarted() override {
    m_table_iterator->EndPSIBatchModeIfStarted();
    m_src_iter->EndPSIBatchModeIfStarted();
  }

  const IteratorProfiler *GetProfiler() const override {
    assert(thd()->lex->is_explain_analyze);
    return &m_profiler;
  }

  const Profiler *GetTableIterProfiler() const {
    return &m_table_iter_profiler;
  }

 private:
  bool isleaf{true};
  bool iscycle{false};
  IteratorProfilerImpl::TimeStamp r_time;
  IteratorProfilerImpl::TimeStamp read_stack_time;
  IteratorProfilerImpl::TimeStamp cond_time;
  ha_rows loop;

  bool preorder_dfs() {
    DBUG_EXECUTE_IF("connect_by_time",
                    { r_time = IteratorProfilerImpl::Now(); });
    ha_rows rows = 0;
    ha_rows level = 1;
    Item_func_rownum *ir = m_join->query_block->rownum_func;

    if (get_result_by_scan(m_param->start_with_cond, m_param->start_rownum_it,
                           rows))
      return true;
    if (rows == 0) return false;

    for (auto it : m_param->connect_by_func_list) {
      if (it->reset()) return true;
    }
#ifndef NDEBUG
    auto old_rows = rows;
#endif
    rows = 0;
    if (reverse_order_copy(true, rows)) return true;
    assert(old_rows == rows);

    DBUG_EXECUTE_IF("connect_by_time", {
      cond_time = read_stack_time = IteratorProfilerImpl::Now();
      loop = 0;
    });

    if (m_param->ref) {
      // copy all souce iter into temp result
      if (get_result_by_scan(nullptr, nullptr, rows)) return true;
      DBUG_PRINT("info", ("JOIN %p ref slice %u -> connect_by_result", m_join,
                          m_in_ref_slice));
    }

    if (!m_param->stack->ref_items.is_null()) {
      m_join->copy_ref_item_slice(m_join->ref_items[REF_SLICE_ACTIVE],
                                  m_param->stack->ref_items);
      m_join->current_ref_item_slice = -1;
    }

    bool find_next = true;
    bool stack_slice = false;
    for (;;) {
      DBUG_EXECUTE_IF("connect_by_time", { loop++; });
      if (read_stack_last(level)) return true;
      if (level == 0) {
        break;
      }

      if ((level + 1) > thd()->variables.cte_max_recursion_depth) {
        my_error(ER_CTE_MAX_RECURSION_DEPTH, MYF(0), level);
        return true;
      }

      isleaf = true;
      iscycle = false;

      if (level == 1) {
        if (m_param->history) clear_data(m_param->history->tab);
      }
      // read from stack
      if (update_connect_by_func(level + 1)) return true;

      if (m_param->connect_by_rownum_it) {
        if (m_param->connect_by_rownum_it->val_int()) {
          find_next = true;
        } else {
          if (level != 1) {
            if (delete_last_row()) return true;
            ir->fallback_value();
            ir->reset_read_flag();
            find_next = false;
            continue;
          } else {
            find_next = false;
          }
        }
      }
      stack_slice = false;

      if (find_next) {
        if (m_param->ref) {
          if (get_result_by_index(level)) return true;
        } else {
          if (get_result_by_connect_by(level)) return true;
        }

        if (update_leaf_cycle_into_stack()) return true;

        if (!m_param->stack->ref_items.is_null()) {
          m_join->copy_ref_item_slice(m_join->ref_items[REF_SLICE_ACTIVE],
                                      m_param->stack->ref_items);
          m_join->current_ref_item_slice = -1;
          stack_slice = true;
        }
        if (!isleaf) {
          if (save_into_history()) return true;
        }
      }

      if (!stack_slice && !m_param->stack->ref_items.is_null()) {
        m_join->copy_ref_item_slice(m_join->ref_items[REF_SLICE_ACTIVE],
                                    m_param->stack->ref_items);
        m_join->current_ref_item_slice = -1;
        stack_slice = true;
      }

      rows = 0;
      if (copy_funcs(m_result_table_param, thd())) return true;
      if (write_to_tb(thd(), table(), rows)) return true;
      assert(rows);

      if (delete_last_row()) return true;
      if (ir) {
        ir->reset_read_flag();
      }
    }
    return false;
  }

  bool get_result_by_index(ha_rows &level) {
    ha_rows rows = 0;
    if (!m_param->cache->ref_items.is_null()) {
      m_join->copy_ref_item_slice(m_join->ref_items[REF_SLICE_ACTIVE],
                                  m_param->cache->ref_items);
      m_join->current_ref_item_slice = -1;
    }

    auto iter = NewIterator<RefIterator<true>>(
        thd(), thd()->mem_root, m_param->cache->tab, m_param->ref, false, 100.0,
        nullptr);

    if (iter->Init()) return true;
    bool matched = true;
    auto cond = m_param->connect_by_cond;

    int err = 0;
    for (;;) {
      err = iter->Read();
      if (err != 0) {
        if (err < 0) {
          break;
        } else
          return true;
      }

      if (cond) {
        matched = cond->val_int();
      }
      if (thd()->killed) {
        thd()->send_kill_message();
        return true;
      }
      if (thd()->is_error()) return true;
      if (!matched) {
        m_src_iter->UnlockRow();

        continue;
      }
      if (save_into_stack(false, rows)) return true;
    }

    DBUG_EXECUTE_IF("connect_by_time", {
      auto now = IteratorProfilerImpl::Now();
      DBUG_PRINT(
          "connect_by_time",
          ("get_by_idx loop: %llu,rows: %llu,dur: %lf,rdur: %lf", loop, rows,
           std::chrono::duration<double>(now - r_time).count() * 1e3,
           std::chrono::duration<double>(now - cond_time).count() * 1e3));
      cond_time = r_time = now;
    });
    DBUG_PRINT("connect_by", ("connect index with %llu", rows));

    level = rows > 0 ? level + 1 : level;
    restore_record(m_param->stack->tab, record[1]);
    return false;
  }

  bool get_result_by_connect_by(ha_rows &level) {
    ha_rows rows = 0;
    if (m_param->connect_by_cond) {
      if (get_result_by_scan(m_param->connect_by_cond, nullptr, rows))
        return true;

      DBUG_EXECUTE_IF("connect_by_time", {
        auto now = IteratorProfilerImpl::Now();
        DBUG_PRINT(
            "connect_by_time",
            ("get_result_by loop: %llu,rows: %llu,dur: %lf,rdur: %lf", loop,
             rows, std::chrono::duration<double>(now - r_time).count() * 1e3,
             std::chrono::duration<double>(now - cond_time).count() * 1e3));
        cond_time = r_time = now;
      });

      if (rows != 0) {
        rows = 0;
        // store result into stack
        // if result is not empty stack_table->record[0] will change to last
        if (reverse_order_copy(false, rows)) return true;

        DBUG_EXECUTE_IF("connect_by_time", {
          auto now = IteratorProfilerImpl::Now();
          DBUG_PRINT(
              "connect_by_time",
              ("reverse_order loop: %llu,rows: %llu,dur: %lf", loop, rows,
               std::chrono::duration<double>(now - r_time).count() * 1e3));
          r_time = now;
        });
        if (rows != 0) {
          level++;
        }
        restore_record(m_param->stack->tab, record[1]);
      }
    } else {
      // only rownum copy first into stack
      if (get_first_result_by_scan(rows)) return true;
      if (rows != 0) {
        level++;
        restore_record(m_param->stack->tab, record[1]);
      }
    }
    return false;
  }

  bool update_connect_by_func(ha_rows level) {
    for (auto it : m_param->connect_by_func_list) {
      if (it->update_value(level)) return true;
    }
    return false;
  }

  bool update_leaf_cycle_into_stack() {
    longlong val_leaf = isleaf ? 1 : 0;
    longlong val_cycle = iscycle ? 1 : 0;
    for (Item *it : m_param->func_leaf_list) {
      if (it->type() == Item::CONNECT_BY_FUNC_ITEM) {
        auto cit = dynamic_cast<Item_connect_by_func *>(it);
        if (!cit) return true;
        cit->update_value(val_leaf);
      }
    }
    for (Item *it : m_param->func_cycle_list) {
      if (it->type() == Item::CONNECT_BY_FUNC_ITEM) {
        auto cit = dynamic_cast<Item_connect_by_func *>(it);
        if (!cit) return true;
        cit->update_value(val_cycle);
      }
    }
    return false;
  }

  bool get_first_result_by_scan(ha_rows &rows) {
    if (m_in_ref_slice != -1) {
      assert(m_join != nullptr);
      if (!m_join->ref_items[m_in_ref_slice].is_null()) {
        m_join->set_ref_item_slice(m_in_ref_slice);
      }
    }
    if (m_src_iter->Init()) return true;

    auto err = m_src_iter->Read();
    if (err != 0) {
      if (err > 0 || thd()->is_error())  // Fatal error
        return true;
      else if (err < 0)
        return false;
    }
    if (copy_funcs(m_param->cache->param, thd())) return true;

    // not need into cache

    if (copy_funcs(m_param->stack->param, thd())) return true;

    if (write_to_tb(thd(), m_param->stack->tab, rows)) return true;
    return false;
  }

  // get start with cond match rows
  bool get_result_by_scan(Item *cond, Item_func_rownum *rown_it,
                          ha_rows &rows) {
    if (m_in_ref_slice != -1) {
      assert(m_join != nullptr);
      if (!m_join->ref_items[m_in_ref_slice].is_null()) {
        m_join->set_ref_item_slice(m_in_ref_slice);
      }
    }
    // rownum
    if (m_src_iter->Init()) return true;
    int err = 0;
    bool matched = true;
    for (;;) {
      err = m_src_iter->Read();
      if (err != 0) {
        if (err > 0 || thd()->is_error())  // Fatal error
          return true;
        else if (err < 0)
          break;
      }
      if (cond) {
        matched = cond->val_int();
      }
      if (thd()->killed) {
        thd()->send_kill_message();
        return true;
      }
      if (thd()->is_error()) return true;
      if (!matched) {
        m_src_iter->UnlockRow();

        if (rown_it) {
          rown_it->fallback_value();
          rown_it->reset_read_flag();
        }
        continue;
      }

      if (copy_funcs(m_param->cache->param, thd())) return true;
      if (write_to_tb(thd(), m_param->cache->tab, rows)) return true;
      if (rown_it) {
        rown_it->reset_read_flag();
      }
    }

    DBUG_PRINT("connect_by", ("get_result_by with %llu", rows));
    DBUG_EXECUTE_IF("connect_by_time", {
      auto now = IteratorProfilerImpl::Now();
      DBUG_PRINT(
          "connect_by_time",
          ("get_result_by %lf, rows: %llu",
           std::chrono::duration<double>(now - r_time).count() * 1e3, rows));
      r_time = now;
    });
    return false;
  }

  bool reverse_order_copy(bool start, ha_rows &rows) {
    if (!m_param->cache->ref_items.is_null()) {
      m_join->copy_ref_item_slice(m_join->ref_items[REF_SLICE_ACTIVE],
                                  m_param->cache->ref_items);
      m_join->current_ref_item_slice = -1;
    }
    auto table = m_param->cache->tab;
    uchar connect_by_key[MAX_KEY_LENGTH] = {0};
    auto index = 0;
    int err = table->file->ha_index_init(index, false);
    if (err != 0) {
      table->file->print_error(err, MYF(0));
      return true;
    }
    auto key = table->key_info + index;
    key_copy(connect_by_key, table->record[0], key, key->key_length);
    err = table->file->ha_index_read_last_map(table->record[0], connect_by_key,
                                              HA_WHOLE_KEY);
    if (err != 0) {
      if (handleError(err, table) < 0) {
        // if empty ,will be filter before
        assert(0);
        return false;
      } else {
        return true;
      }
    }
    if (save_into_stack(start, rows)) return true;
    for (;;) {
      err = table->file->ha_index_prev(table->record[0]);
      if (err != 0) {
        if (handleError(err, table) < 0) {
          break;
        } else {
          return true;
        }
      }
      if (save_into_stack(start, rows)) return true;
    }
    err = table->file->ha_index_or_rnd_end();
    if (err != 0) {
      table->file->print_error(err, MYF(0));
      return true;
    }
    clear_data(table);
    DBUG_PRINT("connect_by", ("reverse_order rows:%llu", rows));
    DBUG_EXECUTE_IF("connect_by_time", {
      auto now = IteratorProfilerImpl::Now();
      DBUG_PRINT("connect_by_time",
                 ("reverse order copy: %lf",
                  std::chrono::duration<double>(now - r_time).count() * 1e3));
      r_time = now;
    });
    return false;
  }

  bool save_into_stack(bool start, ha_rows &rows) {
    if (copy_funcs(m_param->stack->param, thd())) return true;
    if (!start) {
      if (find_history()) {
        if (m_param->nocycle) {
          DBUG_PRINT("connect_by", ("nocycle skip %llu", rows + 1));
          iscycle = true;
          return false;
        } else {
          my_error(ER_CONNECT_BY_LOOP, MYF(0));
          return true;
        }
      }
    }
    if (write_to_tb(thd(), m_param->stack->tab, rows)) return true;
    if (!start) {
      isleaf = false;
    }
    key_copy(last_key, m_param->stack->tab->record[0],
             m_param->stack->tab->key_info,
             m_param->stack->tab->key_info->key_length);
    return false;
  }

  uchar last_key[MAX_KEY_LENGTH] = {0};
  /**
   * get last index value
   */
  bool read_stack_last(ha_rows &level) {
    // see reverse_order_copy
    auto stack_table = m_param->stack->tab;
    int err = stack_table->file->ha_index_init(0, false);
    if (err != 0) {
      stack_table->file->print_error(err, MYF(0));
      return true;
    }
    for (;;) {
      err = stack_table->file->ha_index_read_last_map(stack_table->record[0],
                                                      last_key, HA_WHOLE_KEY);
      if (err != 0) {
        if (handleError(err, stack_table) > 0) {
          return true;
        } else {
          level--;
          if (level == 0) {
            stack_table->file->ha_index_end();
            return false;
          }
          DBUG_PRINT("connect_by", ("index not find %llu", level));

          if (m_param->level_it->update_value(level)) {
            return true;
          }
          if (m_param->level_it->save_in_field(
                  m_param->level_it->get_result_field(), false) != 0) {
            my_error(ER_WRONG_ARGUMENTS, MYF(0), "level change");
            return true;
          }
          key_copy(last_key, stack_table->record[0], stack_table->key_info,
                   stack_table->key_info->key_length);
        }
      } else {
        break;
      }
    }
    stack_table->file->position(stack_table->file->ref);
    err = stack_table->file->ha_index_end();
    if (err != 0) {
      stack_table->file->print_error(err, MYF(0));
      return true;
    }
    store_record(stack_table, record[1]);
    if (m_param->history) {
      if (copy_funcs(m_param->history->param, thd())) return true;
      store_record(m_param->history->tab, record[1]);
    }

    DBUG_EXECUTE_IF("connect_by_time", {
      auto now = IteratorProfilerImpl::Now();
      DBUG_PRINT(
          "connect_by_time",
          ("read_stack_last loop: %llu,dur: %lf,rdur: %lf", loop,
           std::chrono::duration<double>(now - r_time).count() * 1e3,
           std::chrono::duration<double>(now - read_stack_time).count() * 1e3));
      read_stack_time = r_time = now;
    });
    return false;
  }

  bool delete_last_row() {
    auto stack_table = m_param->stack->tab;
    auto err = stack_table->file->ha_rnd_init(false);
    if (err != 0) {
      stack_table->file->print_error(err, MYF(0));
      return true;
    }
    err = stack_table->file->ha_rnd_pos(stack_table->record[0],
                                        stack_table->file->ref);
    if (err != 0) {
      stack_table->file->print_error(err, MYF(0));
      return true;
    }
    err = stack_table->file->ha_delete_row(stack_table->record[0]);
    if (err != 0) {
      stack_table->file->print_error(err, MYF(0));
      return true;
    }
    err = stack_table->file->ha_rnd_end();
    if (err != 0) {
      stack_table->file->print_error(err, MYF(0));
      return true;
    }
    return false;
  }

  /**
   * check is cycle data
   */
  bool find_history() {
    if (!m_param->history) return false;

    auto history_table = m_param->history->tab;

    bool find = false;
    if (copy_funcs(m_param->history->param, thd())) return true;

    // blob or varchar will check
    if (history_table->s->blob_fields + history_table->s->varchar_fields == 0)
      if (cmp_record(history_table, record[1]) == 0) {
        return true;
      }

    auto err = history_table->file->ha_index_init(0, false);
    if (err != 0) {
      HandleError(err);
      return true;
    }
    // check is in history
    if (history_table->hash_field) {
      if (!check_unique_constraint(history_table)) {
        find = true;
        DUMP_TB_VAL(history_table);
      }
    } else {
      // use the key search
      uchar unique_key[MAX_KEY_LENGTH] = {0};
      key_copy(unique_key, history_table->record[0], history_table->key_info,
               history_table->key_info->key_length);
      if (!(history_table->file->ha_index_read_map(history_table->record[0],
                                                   unique_key, HA_WHOLE_KEY,
                                                   HA_READ_KEY_EXACT))) {
        find = true;
        DUMP_TB_VAL(history_table);
      }
    }
    err = history_table->file->ha_index_end();
    if (err != 0) {
      history_table->file->print_error(err, MYF(0));
    }

    return find;
  }

  bool save_into_history() {
    if (!m_param->history) return false;
    if (copy_funcs(m_param->history->param, thd())) return true;
    ha_rows rows = 0;
    if (write_to_tb(thd(), m_param->history->tab, rows)) return true;
    return false;
  }

  int handleError(int error, TABLE *tb) {
    if (thd()->killed) {
      thd()->send_kill_message();
      return 1;
    }

    if (error == HA_ERR_END_OF_FILE || error == HA_ERR_KEY_NOT_FOUND) {
      tb->set_no_row();
      return -1;
    } else {
      tb->file->print_error(error, MYF(0));
      return 1;
    }
  }
  /**
    Profiling data for this iterator. Used for 'EXPLAIN ANALYZE'.
    @see MaterializeIterator#m_profiler for a description of how
    this is used.
  */
  Profiler m_profiler;

  /**
      Profiling data for m_table_iterator,
      @see MaterializeIterator#m_table_iter_profiler.
  */
  Profiler m_table_iter_profiler;
};

template <typename Profiler>
bool ConnectbyIterator<Profiler>::Init() {
  const typename Profiler::TimeStamp start_time = Profiler::Now();
  if (!table()->materialized) {
    m_in_ref_slice = m_join->get_ref_item_slice();

    if (init_tmp_table(thd(), table())) return true;

    if (param_Init(thd(), m_param)) return true;

    if (preorder_dfs()) {
      return true;
    }
    if (m_in_ref_slice != -1) {
      assert(m_join != nullptr);
      if (!m_join->ref_items[m_out_ref_slice].is_null()) {
        m_join->set_ref_item_slice(m_in_ref_slice);
      }
    }
    table()->materialized = true;
  }
  m_profiler.StopInit(start_time);
  int err = m_table_iterator->Init();
  m_table_iter_profiler.StopInit(start_time);
  return err;
}

template <typename Profiler>
int ConnectbyIterator<Profiler>::Read() {
  const typename Profiler::TimeStamp start_time = Profiler::Now();

  if (m_out_ref_slice != -1) {
    assert(m_join != nullptr);
    if (!m_join->ref_items[m_out_ref_slice].is_null()) {
      m_join->set_ref_item_slice(m_out_ref_slice);
    }
  }

  int err = m_table_iterator->Read();
  m_table_iter_profiler.StopRead(start_time, err == 0);
  return err;
}

RowIterator *connect_by_iterator::CreateIterator(
    THD *thd, JOIN *join, TABLE *table, Temp_table_param *temp_table_param,
    unique_ptr_destroy_only<RowIterator> src_iter,
    unique_ptr_destroy_only<RowIterator> res_iter,
    Connect_by_param *connect_by_param, int ref_slice) {
  if (thd->lex->is_explain_analyze) {
    RowIterator *const table_iter_ptr = res_iter.get();

    auto iter = new (thd->mem_root) ConnectbyIterator<IteratorProfilerImpl>(
        thd, join, table, temp_table_param, move(src_iter), move(res_iter),
        connect_by_param, ref_slice);
    /*
      Provide timing data for the iterator that iterates over the temporary
      table. This should include the time spent both materializing the table
      and iterating over it.
    */
    table_iter_ptr->SetOverrideProfiler(iter->GetTableIterProfiler());
    return iter;
  } else {
    return new (thd->mem_root) ConnectbyIterator<DummyIteratorProfiler>(
        thd, join, table, temp_table_param, move(src_iter), move(res_iter),
        connect_by_param, ref_slice);
  }
}