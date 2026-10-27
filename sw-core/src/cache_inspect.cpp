// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

#include "cache_inspect.hpp"

#include "cache_journal.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <string>
#include <system_error>
#include <unordered_map>

namespace swarmstream::detail {

namespace fs = std::filesystem;

namespace {

// Cópia temporária do journal, apagada ao sair. file() vazio = a cópia falhou (IoError).
class JournalCopy
{
public:
    explicit JournalCopy(const fs::path& src)
    {
        static std::atomic<uint64_t> counter{0};
        std::error_code ec;
        const fs::path base = fs::temp_directory_path(ec);
        if (ec) return;
        dir_ = base / ("sw-cache-inspect-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                       "-" + std::to_string(counter.fetch_add(1)));
        if (!fs::create_directories(dir_, ec) || ec) {
            dir_.clear();
            return;
        }
        const fs::path dst = dir_ / "journal.bin";
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (!ec) file_ = dst;
    }
    ~JournalCopy()
    {
        if (dir_.empty()) return;
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    JournalCopy(const JournalCopy&) = delete;
    JournalCopy& operator=(const JournalCopy&) = delete;
    [[nodiscard]] const fs::path& file() const noexcept { return file_; }

private:
    fs::path dir_;
    fs::path file_;
};

// "store-NNN.bin" -> NNN
bool parse_store_name(const std::string& name, uint32_t& id)
{
    if (name.size() != 13 || name.compare(0, 6, "store-") != 0 || name.compare(9, 4, ".bin") != 0)
        return false;
    id = 0;
    for (size_t i = 6; i < 9; ++i) {
        if (name[i] < '0' || name[i] > '9') return false;
        id = id * 10 + static_cast<uint32_t>(name[i] - '0');
    }
    return true;
}

} // namespace

Result<CacheInspection, CacheErrorCode> inspect_cache(const fs::path& dir,
                                                      const StoreLayout& layout)
{
    std::error_code ec;
    if (!fs::is_directory(dir, ec) || ec) return Unexpected(CacheErrorCode::IoError);

    CacheInspection out;
    // Mesma regra da recuperação: a peça mais nova vale; um slot guarda uma peça só.
    std::unordered_map<PieceHash, InspectedPiece> by_piece;
    std::unordered_map<uint64_t, PieceHash> by_slot;
    auto slot_of = [&](const InspectedPiece& p) {
        return uint64_t{p.store_file_id} * layout.slots_per_file + p.slot_in_file;
    };
    auto erase_piece = [&](const PieceHash& h) {
        auto it = by_piece.find(h);
        if (it == by_piece.end()) return;
        by_slot.erase(slot_of(it->second));
        by_piece.erase(it);
    };
    auto put = [&](const InspectedPiece& p) {
        if (auto s = by_slot.find(slot_of(p)); s != by_slot.end()) {
            const PieceHash old = s->second; // erase_piece apaga a entrada que `s` aponta
            erase_piece(old);
        }
        erase_piece(p.piece);
        by_slot[slot_of(p)] = p.piece;
        by_piece[p.piece] = p;
    };

    // index.bin
    LoadedIndex loaded;
    switch (read_index_file(dir / "index.bin", layout, loaded)) {
    case IndexLoad::Missing: out.index = InspectFileState::Missing; break;
    case IndexLoad::Invalid: out.index = InspectFileState::Invalid; break;
    case IndexLoad::Ok:
        out.index = InspectFileState::Ok;
        out.index_lsn = loaded.checkpoint_lsn;
        out.index_entries = loaded.entries.size();
        for (const IndexEntry& e : loaded.entries) {
            put(InspectedPiece{.piece = e.piece,
                               .store_file_id = e.slot / layout.slots_per_file,
                               .slot_in_file = e.slot % layout.slots_per_file,
                               .size = e.size,
                               .block_count = static_cast<uint32_t>(e.blocks.size())});
        }
        break;
    }

    // journal.bin, lido numa cópia
    const fs::path journal_path = dir / "journal.bin";
    const bool journal_exists = fs::exists(journal_path, ec);
    if (ec) return Unexpected(CacheErrorCode::IoError);
    if (journal_exists) {
        JournalCopy copy(journal_path);
        if (copy.file().empty()) return Unexpected(CacheErrorCode::IoError);
        auto apply = [&](const JournalRecord& r) {
            const uint64_t slot =
                uint64_t{r.store_file_id} * layout.slots_per_file + r.slot_in_file;
            if (r.slot_in_file >= layout.slots_per_file || slot >= kInvalidSlot) return;
            if (r.type == JournalType::CommitPiece) {
                put(InspectedPiece{.piece = r.piece,
                                   .store_file_id = r.store_file_id,
                                   .slot_in_file = r.slot_in_file,
                                   .size = r.piece_size,
                                   .block_count = static_cast<uint32_t>(r.blocks.size())});
            } else if (r.type == JournalType::FreeSlot) {
                if (auto s = by_slot.find(slot); s != by_slot.end() && s->second == r.piece)
                    erase_piece(r.piece);
            }
        };
        JournalReplay replay;
        auto opened = Journal::open(copy.file(), out.index_lsn, apply, replay);
        if (!opened) return Unexpected(opened.error());
        if (auto rc = opened->close(); rc != CacheErrorCode::Success) return Unexpected(rc);
        out.journal = replay.header_ok ? InspectFileState::Ok : InspectFileState::Invalid;
        out.journal_checkpoint_lsn = replay.checkpoint_lsn;
        out.journal_last_lsn = replay.last_lsn;
        out.journal_records = replay.records;
        out.journal_applied = replay.applied;
        out.journal_truncated_bytes = replay.truncated_bytes;
    }

    // Arquivos de store: presentes na pasta ou citados por alguma peça.
    std::map<uint32_t, InspectedStoreFile> files;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        uint32_t id = 0;
        if (!parse_store_name(it->path().filename().string(), id)) continue;
        std::error_code sec;
        const auto sz = fs::file_size(it->path(), sec);
        if (sec) return Unexpected(CacheErrorCode::IoError);
        InspectedStoreFile& f = files[id];
        f.id = id;
        f.present = true;
        f.size_bytes = sz;
    }
    if (ec) return Unexpected(CacheErrorCode::IoError);

    out.pieces.reserve(by_piece.size());
    for (const auto& kv : by_piece)
        out.pieces.push_back(kv.second);
    std::sort(out.pieces.begin(), out.pieces.end(),
              [](const InspectedPiece& a, const InspectedPiece& b) {
                  return a.store_file_id != b.store_file_id ? a.store_file_id < b.store_file_id
                                                            : a.slot_in_file < b.slot_in_file;
              });
    for (InspectedPiece& p : out.pieces) {
        InspectedStoreFile& f = files[p.store_file_id];
        f.id = p.store_file_id;
        p.store_file_present = f.present;
        ++f.pieces;
        f.piece_bytes += p.size;
        out.total_piece_bytes += p.size;
        if (!p.store_file_present) ++out.pieces_in_missing_files;
    }
    for (const auto& kv : files)
        out.store_files.push_back(kv.second);
    return out;
}

} // namespace swarmstream::detail
