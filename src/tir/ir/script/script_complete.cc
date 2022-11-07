/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 * \file tir/ir/script/script_complete.cc
 * \brief Used by TVM Script parser to expand incomplete TIR input
 */

#include "./script_complete.h"

#include <tvm/arith/int_set.h>
#include <tvm/tir/analysis.h>

#include <utility>

namespace tvm {
namespace tir {

/*! \brief Generate surrounding loops automatically */
class ScriptCompleter : public StmtMutator {
 public:
  explicit ScriptCompleter(Map<Var, Buffer>* buffer_var_map) : buffer_var_map_(buffer_var_map) {}
  /*! \brief Whether the stmt contains at least one block. */
  bool contains_block = false;

 private:
  Map<Var, Buffer>* buffer_var_map_;
  Stmt VisitStmt_(const BlockRealizeNode* op) override {
    contains_block = true;
    for (const PrimExpr& value : op->iter_values) {
      CHECK(value.dtype().is_int())
          << "BlockRealize iter_value expected a IntImm, but got " << value.dtype();
    }
    return StmtMutator::VisitStmt_(op);
  }

  Stmt VisitStmt_(const BlockNode* op) override {
    // Buffers allocated in the block can be accessed by its body.
    for (const auto& alloc_buffer : op->alloc_buffers) {
      buffer_var_map_->Set(alloc_buffer->data, alloc_buffer);
    }
    for (const auto& match_buffer : op->match_buffers) {
      const Buffer& target_buffer = match_buffer->buffer;
      buffer_var_map_->Set(target_buffer->data, target_buffer);
    }
    Block block = Downcast<Block>(StmtMutator::VisitStmt_(op));
    // Remove buffers allocated inside block to detect its access region
    for (const auto& alloc_buffer : op->alloc_buffers) {
      buffer_var_map_->erase(alloc_buffer->data);
    }
    for (const auto& match_buffer : op->match_buffers) {
      const Buffer& target_buffer = match_buffer->buffer;
      buffer_var_map_->erase(target_buffer->data);
    }
    // Get access detection mask
    // 0 for provided region, 1 and 3 for need detect read, 2 and 3 for need detect write
    int mask = 0;
    auto it = op->annotations.find(attr::script_parsing_detect_access);
    if (it != op->annotations.end()) {
      mask = Downcast<IntImm>((*it).second)->value;
    }
    // ignore root block or blocks which already has reads/writes regions
    if (mask != 0) {
      auto access_region = GetBlockAccessRegion(block, *buffer_var_map_);
      const Array<BufferRegion>& reads = access_region[0];
      const Array<BufferRegion>& writes = access_region[1];
      const Array<BufferRegion>& opaque = access_region[2];
      CHECK(opaque.empty())
          << "ValueError: Can not auto detect buffer access region from tir.Load, tir.Store or "
             "direct access by buffer data. Please annotation the access region manually";
      auto n = CopyOnWrite(block.operator->());
      if (mask & 1) n->reads = reads;
      if (mask & 2) n->writes = writes;
      n->annotations = op->annotations;
      n->annotations.erase(attr::script_parsing_detect_access);
      return Block(n);
    } else {
      return std::move(block);
    }
  }
};

PrimFunc ScriptComplete(PrimFunc func, const Array<Buffer>& root_allocates) {
  Map<Var, Buffer> buffer_var_map;
  for (const auto& pair : func->buffer_map) {
    const Buffer& buffer = pair.second;
    buffer_var_map.Set(buffer->data, buffer);
  }
  for (const auto& alloc : root_allocates) {
    buffer_var_map.Set(alloc->data, alloc);
  }
  bool contain_root = root_allocates.empty() && func->body->IsInstance<BlockRealizeNode>() &&
                      Downcast<BlockRealize>(func->body)->block->iter_vars.empty();
  ScriptCompleter script_completer(&buffer_var_map);
  // generate surrounding loops automatically
  Stmt res = script_completer(func->body);
  // generate root block automatically
  if ((script_completer.contains_block || root_allocates.size()) && !contain_root) {
    res = Block({}, {}, {}, "root", res, NullOpt, root_allocates);
    res = BlockRealize({}, Bool(true), Downcast<Block>(res));
  }
  if (func->body.same_as(res)) {
    return func;
  } else {
    auto fptr = func.CopyOnWrite();
    fptr->body = res;
    return func;
  }
}

TVM_REGISTER_GLOBAL("script.Complete").set_body_typed(ScriptComplete);

}  // namespace tir
}  // namespace tvm
