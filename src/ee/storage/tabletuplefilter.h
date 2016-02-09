/* This file is part of VoltDB.
 * Copyright (C) 2008-2015 VoltDB Inc.
 *
 * This file contains original code and/or modifications of original code.
 * Any modifications made by VoltDB Inc. are licensed under the following
 * terms and conditions:
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with VoltDB.  If not, see <http://www.gnu.org/licenses/>.
 */
/* Copyright (C) 2008 by H-Store Project
 * Brown University
 * Massachusetts Institute of Technology
 * Yale University
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef VOLTDB_TABLETUPLEFILTER_H
#define VOLTDB_TABLETUPLEFILTER_H

#include "common/tabletuple.h"

#include <boost/unordered_map.hpp>
#include <boost/iterator/iterator_facade.hpp>

#include <vector>
#include <limits>

namespace voltdb {

class Table;

// Iterator forward declaration
template<int8_t MARKER, typename Value>
class TableTupleFilter_iter;

/**
 * A lightweight representation of a table - a contiguous array where each tuple
 * (active and non-active) is represented as a byte with a certain value. The physical tuple address
 * in the real table and the corresponding tuple index in the TableTupleFilter are related by the following
 * equation:
 *
 * Tuple Index = (Tuple Address - Tuple Block Address) / Tuple Size + Block Offset
 *
 * where Block Offset is the index of the first tuple in the block into the array:
 *
 * Block Offset = Block Number * Tuples Per Block
 *
 */
class TableTupleFilter {

    public:

    const static int8_t INACTIVE_TUPLE  = -1;
    const static int8_t ACTIVE_TUPLE    =  0;

    template<int8_t MARKER, typename Value>
    friend class TableTupleFilter_iter;

    /**
     * Default constructor
     */
    TableTupleFilter();

    /**
     * Initialize TableTupleFilter from a table by setting the value for all active tuples
     * to ACTIVE_TUPLE(0) and advancing the last active tuple index
     */
    void init(Table* table);

    /**
     * Update an active tuple and return the tuple index
     */
    uint64_t updateTuple(const TableTuple& tuple, char marker)
    {
        uint64_t tupleIdx = getTupleIndex(tuple);
        assert(tupleIdx <= m_lastActiveTupleIndex && m_lastActiveTupleIndex != INVALID_INDEX);
        assert(m_tuples[tupleIdx] != INACTIVE_TUPLE);
        m_tuples[tupleIdx] = marker;
        return tupleIdx;
    }

    /**
     * Returns the tuple value
     */
    char getTupleValue(size_t tupleIdx) const
    {
        assert(tupleIdx < m_tuples.size());
        return m_tuples[tupleIdx];
    }

    /**
     * Returns the tuple address
     */
    uint64_t getTupleAddress(size_t tupleIdx) const
    {
        assert(tupleIdx < m_tuples.size());
        size_t blockIdx = tupleIdx / m_tuplesPerBlock;
        assert(blockIdx < m_blocks.size());
        return m_blocks[blockIdx] + (tupleIdx - blockIdx) * m_tupleLength;
    }

    bool empty() const
    {
        return m_lastActiveTupleIndex == INVALID_INDEX;
    }

    // Non-const Iterators
    template<int8_t MARKER>
    TableTupleFilter_iter<MARKER, uint64_t> begin();

    template<int8_t MARKER>
    TableTupleFilter_iter<MARKER, uint64_t> end();

    // Const Iterators
    template<int8_t MARKER>
    TableTupleFilter_iter<MARKER, uint64_t const> begin() const;

    template<int8_t MARKER>
    TableTupleFilter_iter<MARKER, uint64_t const> end() const;

    private:

    const static uint64_t INVALID_INDEX;

    void init(const std::vector<uint64_t>& blocks, uint32_t tuplesPerBlock, uint32_t tupleLength);

    uint64_t getTupleIndex(const TableTuple& tuple)
    {
        uint64_t tupleAddress = (uint64_t) tuple.address();
        uint64_t blockIndex = findBlockIndex(tupleAddress);
        return (tupleAddress - m_prevBlockAddress) / m_tupleLength + blockIndex;
    }

    /**
     * Initialize an active tuple by setting its value to ACTIVE_TUPLE and advance last active tuple index.
     * This method should be called only once during the initialization.
     * To update tuple value use updateTuple method
     */
    void initActiveTuple(const TableTuple& tuple)
    {
        uint64_t tupleIdx = getTupleIndex(tuple);
        assert(m_tuples[tupleIdx] == INACTIVE_TUPLE);
        m_tuples[tupleIdx] = ACTIVE_TUPLE;
        // Advance last active tuple index if necessary
        if (m_lastActiveTupleIndex == INVALID_INDEX || m_lastActiveTupleIndex < tupleIdx)
        {
            m_lastActiveTupleIndex = tupleIdx;
        }
    }

    size_t getLastActiveTupleIndex() const
    {
        return m_lastActiveTupleIndex;
    }

    uint64_t findBlockIndex(uint64_t tupleAddress);

    // Tuples (active and not active)
    std::vector<char> m_tuples;
    // Collection of table blocks addresses
    std::vector<uint64_t> m_blocks;
    // (Block Address/ Block offset into the tuples array) map
    boost::unordered_map<uint64_t, uint64_t> m_blockIndexes;

    // Block/Tuple size
    uint32_t m_tuplesPerBlock;
    uint32_t m_tupleLength;

    // Previously accessed block address
    uint64_t m_prevBlockAddress;
    // Previously accessed block index
    uint64_t m_prevBlockIndex;

    // Index of the last ACTIVE tuple in the underlying table
    uint64_t m_lastActiveTupleIndex;
};

/**
 * TableTupleFilter Iterator. Implements FORWARD ITERATOR Concept.
 * Iterates over the tuples that have a certain value set in
 * underline TableTupleFilter.
 *
 * Parameter MARKER specifies the value to look for. Only tuples that have
 * their value set to MARKER will be iterated over
 * Value is expected to be one of "uint64_t" or "const uint64_t"
 * for Non-Const and Const iterator versions
 */
template<int8_t MARKER, typename Value>
class TableTupleFilter_iter
  : public boost::iterator_facade<
        TableTupleFilter_iter<MARKER, Value>
      , Value
      , boost::forward_traversal_tag>
{
    public:
    /**
     * Default constructor
     */
    TableTupleFilter_iter()
      : m_tableFilter(0), m_tupleIdx()
    {}

    private:

    friend class TableTupleFilter;
    friend class boost::iterator_core_access;

    /**
     * Constructor. Sets the iterator position pointing to the first tuple that
     * have the TableTupleFilter value set to the MARKER
     */
    explicit TableTupleFilter_iter(TableTupleFilter* m_tableFilter)
      : m_tableFilter(m_tableFilter), m_tupleIdx(TableTupleFilter::INVALID_INDEX)
    {
        if (!m_tableFilter->empty())
        {
            increment();
        }
    }

    /**
     * Constructor. Sets the iterator position pointing to the specified position - usually the end
     */
    explicit TableTupleFilter_iter(const TableTupleFilter* m_tableFilter, size_t tupleIdx)
        :  m_tableFilter(m_tableFilter), m_tupleIdx(tupleIdx)
    {}

    private:

    bool equal(TableTupleFilter_iter const& other) const
    {
        // Shouldn't compare iterators from different tables
        assert(m_tableFilter == other.m_tableFilter);
        return m_tupleIdx == other.m_tupleIdx;
    }

    Value& dereference() const
    {
        return (Value&)m_tupleIdx;
    }

    // Forward Iteration Support
    void increment()
    {
        uint64_t lastActiveTupleIndex = m_tableFilter->getLastActiveTupleIndex();
        do
        {
            ++m_tupleIdx;
        } while(m_tupleIdx <= lastActiveTupleIndex && m_tableFilter->getTupleValue(m_tupleIdx) != MARKER);
    }

    const TableTupleFilter* m_tableFilter;
    Value m_tupleIdx;
};

template <int8_t MARKER>
inline
TableTupleFilter_iter<MARKER, uint64_t> TableTupleFilter::begin()
{
    return TableTupleFilter_iter<MARKER, uint64_t>(this);
}

template <int8_t MARKER>
inline
TableTupleFilter_iter<MARKER, uint64_t> TableTupleFilter::end()
{
    uint64_t lastActiveTupleIndex = (getLastActiveTupleIndex() != TableTupleFilter::INVALID_INDEX) ?
        getLastActiveTupleIndex() + 1 : TableTupleFilter::INVALID_INDEX;
    return TableTupleFilter_iter<MARKER, uint64_t>(this, lastActiveTupleIndex);
}

template <int8_t MARKER>
inline
TableTupleFilter_iter<MARKER, uint64_t const> TableTupleFilter::begin() const
{
    return TableTupleFilter_iter<MARKER, uint64_t const>(this);
}

template <int8_t MARKER>
inline
TableTupleFilter_iter<MARKER, uint64_t const> TableTupleFilter::end() const
{
    uint64_t lastActiveTupleIndex = (getLastActiveTupleIndex() != TableTupleFilter::INVALID_INDEX) ?
        getLastActiveTupleIndex() + 1 : TableTupleFilter::INVALID_INDEX;
    return TableTupleFilter_iter<MARKER, uint64_t const>(this, lastActiveTupleIndex);
}

}
#endif // VOLTDB_TABLETUPLEFILTER_H
