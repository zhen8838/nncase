/* Copyright 2020 Canaan Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <nncase/ir/op_utils.h>
#include <nncase/ir/ops/bitcast.h>
#include <nncase/ir/ops/concat.h>
#include <nncase/ir/ops/constant.h>
#include <nncase/ir/visitor.h>
#include <nncase/schedule/scheduler.h>
#include <nncase/targets/target.h>
#include <unordered_map>

#include <xtensor/xarray.hpp>

using namespace nncase;
using namespace nncase::ir;
using namespace nncase::schedule;

namespace
{
memory_location_t decide_memory_location(ir::output_connector &conn) noexcept
{
    auto &opcode = conn.owner().runtime_opcode();
    if (opcode == op_input_node)
        return mem_input;
    else if (opcode == op_constant)
        return mem_rdata;

    auto connections = conn.connections();
    if (std::any_of(connections.begin(), connections.end(), [](input_connector *conn) { return conn->owner().runtime_opcode() == op_output_node; }))
        return mem_output;
    return mem_data;
}

shape_t to_strides(const shape_t &shape)
{
    shape_t strides(shape.size());
    xt::compute_strides(shape, xt::layout_type::row_major, strides);
    return strides;
}

class lifetime_recorder
{
public:
    lifetime_recorder(std::unordered_map<const ir::output_connector *, logical_buffer> &buffers)
        : buffers_(buffers)
    {
    }

    void allocate(ir::output_connector &conn)
    {
        auto it = buffers_.find(&conn);
        if (it == buffers_.end())
        {
            logical_buffer buffer(next_buffer_id_++, conn, decide_memory_location(conn));
            buffer.lifetime().birth = cnt_age_;
            buffer.lifetime().used_count = conn.connections().size();
            buffers_.emplace(&conn, buffer);
        }
    }

    void release(ir::output_connector &conn)
    {
        auto node = buffers_.find(&conn);
        if (node != buffers_.end())
        {
            auto &lifetime = node->second.lifetime();
            if (!lifetime.is_alive())
                throw std::runtime_error("Trying to free a released buffer");
            else
                lifetime.used_count--;
        }
    }

    void grow_age()
    {
        cnt_age_++;
        for (auto &b : buffers_)
        {
            auto &lifetime = b.second.lifetime();
            if (lifetime.is_alive())
                lifetime.age++;
        }
    }

private:
    size_t next_buffer_id_ = 0;
    size_t cnt_age_ = 0;
    std::unordered_map<const ir::output_connector *, logical_buffer> &buffers_;
};
}

void scheduler::generate_compute_sequence(schedule_result &result)
{
    auto alloc_visitor = make_relay_ir_visitor([&](node &node) {
        if (node.attributes() & node_attr_action)
            result.compute_sequence.emplace_back(&node);
    });

    alloc_visitor.visit(outputs_);
}

void scheduler::make_logical_buffers()
{
    lifetime_recorder lr(logical_buffers_);
    auto alloc_visitor = make_relay_ir_visitor([&](node &node) {
        for (auto out : node.outputs())
            lr.allocate(*out);

        lr.grow_age();

        for (auto in : node.inputs())
        {
            auto out = in->connection();
            assert(out);
            lr.release(*out);
        }
    });
    alloc_visitor.visit(outputs_);
}

void scheduler::analyze_buffer_alias()
{
    auto alias_visitor = make_relay_ir_visitor([&](node &node) {
        // 1. bitcast
        if (auto b = node_cast<bitcast>(node))
        {
            auto &input = *b->input().connection();
            auto &in_buf = logical_buffers_.at(&input);
            auto &out_buf = logical_buffers_.at(&b->output());

            if (out_buf.memory_location() == mem_output && in_buf.memory_location() == mem_data)
                in_buf.memory_location() = mem_output;

            // input & rdata should be copied to output
            if (out_buf.memory_location() != mem_output
                || (in_buf.memory_location() != mem_input && in_buf.memory_location() != mem_rdata))
            {
                shape_t begin(input.shape().size(), 0);
                out_buf.parent() = { &logical_buffers_.at(&input), begin };
                b->attributes(b->attributes() & ~node_attr_action);
            }
        }

        // 2. concat
        else if (auto c = node_cast<concat>(node))
        {
            auto inputs = c->inputs();
            auto outputs = c->output().connections();

            // simple & exlusive concat
            // input & rdata should be copied to output
            if ((c->axis() == 0 || std::all_of(inputs[0]->shape().begin(), inputs[0]->shape().begin() + c->axis(), [](size_t dim) { return dim == 1; }))
                && std::all_of(inputs.begin(), inputs.end(), [this](input_connector *in) {
                       auto &in_buf = logical_buffers_.at(in->connection());
                       return (in_buf.memory_location() != mem_input && in_buf.memory_location() != mem_rdata)
                           && in->connection()->owner().runtime_opcode() != op_slice;
                   })
                && std::count_if(outputs.begin(), outputs.end(), [](input_connector *in) {
                       return in->owner().runtime_opcode() == op_concat;
                   })
                    < 2)
            {
                // Fix parent later
                c->attributes(c->attributes() & ~node_attr_action);
            }
        }
    });
    alias_visitor.visit(outputs_);
}

void scheduler::fix_concat_indices()
{
    auto fix_concat_visitor = make_relay_ir_visitor([&](node &node) {
        if (auto c = node_cast<concat>(node))
        {
            if (c->attributes() & node_attr_action)
                return;

            // 1. Init indices
            {
                auto axis = c->axis();
                auto &out_buf = logical_buffers_.at(&c->output());
                shape_t cnt_begin(c->input_at(0).shape().size(), 0);
                for (auto in : c->inputs())
                {
                    auto &in_buf = logical_buffers_.at(in->connection());
                    in_buf.parent() = { &out_buf, cnt_begin };
                    cnt_begin[axis] += in->shape()[axis];
                }
            }

            // 2. Iterate parent
            auto child = c;
            while (true)
            {
                auto parent = try_get_direct_child<concat>(*child);
                if (!parent || parent->attributes() & node_attr_action)
                    break;
                auto index = get_input_index(*parent, child->output());
                auto axis = parent->axis();
                shape_t child_begin(child->output().shape().size(), 0);
                child_begin[axis] += std::accumulate(parent->concat_dims().begin(), parent->concat_dims().begin() + index, 0);

                auto &in_buf = logical_buffers_.at(&child->output());
                auto &out_buf = logical_buffers_.at(&parent->output());
                in_buf.parent() = { &out_buf, child_begin };
                for (auto &in : c->inputs())
                {
                    auto &in_buf = logical_buffers_.at(in->connection());
                    auto &desc = *in_buf.parent();
                    desc.parent = &out_buf;
                    desc.begin += child_begin;
                }

                child = parent;
            }
        }
    });
    fix_concat_visitor.visit(outputs_);
}

void scheduler::fix_lifetime()
{
    // Assign parent
    for (auto &bp : logical_buffers_)
    {
        auto &p = bp.second.parent();
        if (p)
        {
            while (p->parent->parent())
                p = p->parent->parent();
        }
    }

    // Extend lifetime
    for (auto &bp : logical_buffers_)
    {
        auto &lifetime = bp.second.lifetime();
        if (bp.second.parent())
        {
            auto &p_liftime = bp.second.parent()->parent->lifetime();
            auto birth = std::min(lifetime.birth, p_liftime.birth);
            auto end = std::max(lifetime.end(), p_liftime.end());
            p_liftime.birth = birth;
            p_liftime.age = end - birth;
        }
    }
}

void scheduler::make_physical_buffers()
{
    std::unordered_map<logical_buffer *, size_t> physical_ids;
    for (auto &bp : logical_buffers_)
    {
        if (!bp.second.parent())
        {
            auto id = physical_ids.size();
            physical_ids.emplace(&bp.second, id);
            physical_buffers_.emplace_back(id, bp.second);
        }
    }

    // Assign parents
    for (auto &bp : logical_buffers_)
    {
        auto parent = bp.second.parent() ? bp.second.parent()->parent : &bp.second;
        bp.second.physical() = &physical_buffers_.at(physical_ids.at(parent));
    }
}

void scheduler::allocate_physical_buffers(schedule_result &result)
{
    allocator_map_t allocators;
    std::vector<std::unique_ptr<buffer_allocator>> allocator_holder;
    target_.register_allocators(allocators, allocator_holder);

    std::vector<physical_buffer *> orders;
    orders.reserve(physical_buffers_.size());
    for (auto &b : physical_buffers_)
        orders.emplace_back(&b);
    std::sort(orders.begin(), orders.end(), [](const physical_buffer *lhs, const physical_buffer *rhs) {
        return lhs->lifetime().birth < rhs->lifetime().birth;
    });

    for (auto &b : orders)
        allocators.at(b->owner().memory_location())->mark(*b);

    for (auto &alloc : allocators)
    {
        alloc.second->finish();
        result.max_usages.emplace(alloc.first, alloc.second->max_usage());
    }

    for (auto &b : physical_buffers_)
        b.allocation() = allocators.at(b.owner().memory_location())->allocations().at(&b);
}

void scheduler::assign_allocations(schedule_result &result)
{
    auto alloc_visitor = make_relay_ir_visitor([&](node &node) {
        for (auto out : node.outputs())
        {
            auto &lbuf = logical_buffers_.at(out);
            auto &owner = lbuf.physical()->owner();
            auto &memory = lbuf.physical()->allocation();

            // TODO: take account of subbuffer
            buffer_allocation alloc;
            alloc.memory_location = owner.memory_location();
            alloc.type = lbuf.type();
            alloc.size = ir::get_bytes(lbuf.type(), lbuf.shape());
            alloc.shape = lbuf.shape();
            if (lbuf.parent() && node.runtime_opcode() != op_bitcast)
                alloc.parent_shape = owner.shape();
            else
                alloc.parent_shape = lbuf.shape();
            alloc.strides = to_strides(alloc.parent_shape);
            alloc.start = memory.start;
            if (lbuf.parent())
            {
                auto &begin = lbuf.parent()->begin;
                alloc.start += ir::get_bytes(lbuf.type()) * xt::element_offset<size_t>(alloc.strides, begin.begin(), begin.end());
            }
            result.allocations.emplace(out, alloc);
        }
    });
    alloc_visitor.visit(outputs_);
}

schedule_result scheduler::schedule()
{
    schedule_result result;

    make_logical_buffers();
    analyze_buffer_alias();
    fix_concat_indices();
    fix_lifetime();
    generate_compute_sequence(result);
    make_physical_buffers();
    allocate_physical_buffers(result);
    assign_allocations(result);

    return result;
}
