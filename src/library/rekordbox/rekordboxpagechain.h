#pragma once

#include <QSet>
#include <cstdint>
#include <stdexcept>

namespace mixxx::rekordbox {

// A table's page references are 32-bit. Validate before the parser seeks,
// and bound traversal even when a corrupt chain never reaches its last page.
class PageChainGuard {
  public:
    PageChainGuard(uint32_t pageSize, uint64_t fileSize)
            : m_pageSize(pageSize), m_fileSize(fileSize) {
        if (pageSize == 0 || pageSize > fileSize) {
            throw std::runtime_error("Invalid Rekordbox database page size");
        }
    }

    void visit(uint32_t index) {
        if ((uint64_t(index) + 1) * m_pageSize > m_fileSize) {
            throw std::runtime_error("Rekordbox page lies outside the database");
        }
        if (m_visited.contains(index)) {
            throw std::runtime_error("Rekordbox database contains a cyclic page chain");
        }
        m_visited.insert(index);
    }

  private:
    uint32_t m_pageSize;
    uint64_t m_fileSize;
    QSet<uint32_t> m_visited;
};

} // namespace mixxx::rekordbox
