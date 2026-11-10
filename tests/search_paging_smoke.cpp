#include "../src/SearchResultCache.h"
#include <cassert>
#include <string>

struct Row
{
    int id;
    std::string description;
};

int main()
{
    SearchResultCache<Row> cache(3);
    cache.put(0, Row{1, "first"});
    cache.put(999999, Row{1000000, "last"});
    assert(cache.size() == 2);
    assert(cache.loadedExtent() == 1000000);
    assert(!cache.contains(500000));
    assert(cache[999999].id == 1000000);
    cache.put(42, Row{43, "middle"});
    (void)cache[0]; // A real hit keeps this row more recently used.
    cache.put(43, Row{44, "next"});
    assert(cache.size() == 3);
    assert(cache.contains(0));
    assert(!cache.contains(999999));
    assert(!cache.contains(1));
    cache.put(42, Row{43, "edited"});
    assert(cache[42].description == "edited");
    assert(cache.size() == 3);
    for (int row = 100000; row < 110000; ++row)
        cache.put(row, Row{row + 1, "page"});
    assert(cache.size() == 3);
    assert(cache.begin()->first == 109997);
    cache.erase(cache.find(109998));
    assert(!cache.contains(109998));
    assert(cache.size() == 2);
    cache.clear();
    assert(cache.empty());
    assert(cache.loadedExtent() == 0);
    cache.put(-1, Row{0, "invalid"});
    assert(cache.empty());
    SearchResultCache<Row> disabled(0);
    disabled.put(0, Row{1, "disabled"});
    assert(disabled.empty());
    SearchResultCache<Row> selectedRows(3);
    selectedRows.put(0, Row{10, "first"});
    selectedRows.put(3, Row{40, "last"});
    const auto idOf = [](const Row& row) { return row.id; };
    const std::vector<int> original{10, 20, 30, 40};
    assert(selectedRows.matchesIds(original, idOf));
    // Background insertion/reorder must never combine new positions with old
    // cached identities when a bulk operation resolves previously unseen rows.
    assert(!selectedRows.matchesIds(std::vector<int>{50, 10, 20, 30, 40}, idOf));
    assert(!selectedRows.matchesIds(std::vector<int>{40, 20, 30, 10}, idOf));
    assert(!selectedRows.matchesIds(std::vector<int>{10, 20, 30}, idOf));
    const std::vector<int> changedGap{10, 30, 20, 40};
    assert(selectedRows.matchesIds(changedGap, idOf));
    assert(changedGap != original); // An existing ID snapshot catches this too.
    return 0;
}
