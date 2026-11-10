#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

// Row numbers are absolute SQL result offsets. Unloaded rows consume no space.
// The caller serializes access; iterators remain valid until that row is evicted.
template<class Row>
class SearchResultCache
{
public:
    using Storage = std::map<int, Row>;
    using iterator = typename Storage::iterator;
    using const_iterator = typename Storage::const_iterator;

    explicit SearchResultCache(std::size_t capacity = 512) : m_capacity(capacity) {}
    iterator begin() { return m_rows.begin(); }
    iterator end() { return m_rows.end(); }
    const_iterator begin() const { return m_rows.begin(); }
    const_iterator end() const { return m_rows.end(); }
    std::size_t size() const { return m_rows.size(); }
    bool empty() const { return size() == 0; }
    int loadedExtent() const { return m_loadedExtent; }
    bool contains(int row) const { return m_rows.find(row) != m_rows.end(); }
    Row& operator[](int row)
    {
        m_access[row] = ++m_clock;
        return m_rows.at(row);
    }
    iterator find(int row)
    {
        auto found = m_rows.find(row);
        if (found != m_rows.end())
            m_access[row] = ++m_clock;
        return found;
    }
    void put(int row, const Row& value)
    {
        if (row < 0 || m_capacity == 0)
            return;
        m_rows[row] = value;
        m_access[row] = ++m_clock;
        if (row >= m_loadedExtent)
            m_loadedExtent = row + 1;
        while (m_rows.size() > m_capacity)
        {
            auto oldest = m_access.begin();
            for (auto item = m_access.begin(); item != m_access.end(); ++item)
                if (item->second < oldest->second)
                    oldest = item;
            m_rows.erase(oldest->first);
            m_access.erase(oldest);
        }
    }
    template<class GetId>
    bool matchesIds(const std::vector<int>& ids, GetId getId) const
    {
        for (const auto& item : m_rows)
            if (static_cast<std::size_t>(item.first) >= ids.size() ||
                ids[static_cast<std::size_t>(item.first)] != getId(item.second))
                return false;
        return true;
    }
    void erase(iterator item)
    {
        m_access.erase(item->first);
        m_rows.erase(item);
    }
    void clear()
    {
        m_rows.clear();
        m_access.clear();
        m_loadedExtent = 0;
        m_clock = 0;
    }
private:
    Storage m_rows;
    std::map<int, std::uint64_t> m_access;
    std::size_t m_capacity;
    std::uint64_t m_clock = 0;
    int m_loadedExtent = 0;
};
