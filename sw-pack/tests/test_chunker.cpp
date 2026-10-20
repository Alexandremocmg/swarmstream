// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Testes do chunker "swarmstream-fastcdc-v0" (F1-01).
//   sw-pack-tests <vectors-v0.json>            testes rápidos (CTest padrão)
//   sw-pack-tests <vectors-v0.json> --heavy    estabilidade em 1 GiB (rótulo "heavy")
//
// A referência são os vetores do JSON (docs/03 §11.1), não outra biblioteca. O leitor de JSON
// abaixo é mínimo de propósito: o projeto não tem dependências de terceiros.

#include "check.hpp"
#include "swarmstream/chunker.hpp"
#include "swarmstream/sha256.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace sp = swarmstream::pack;

namespace {

// ---------------------------------------------------------------- JSON mínimo

struct Json
{
    enum class Kind { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0;
    std::string text;
    std::vector<Json> items;
    std::map<std::string, Json> members;

    [[nodiscard]] const Json& at(const std::string& key) const
    {
        const auto found = members.find(key);
        SW_CHECK(kind == Kind::Object && found != members.end());
        return found->second;
    }
    [[nodiscard]] bool has(const std::string& key) const
    {
        return kind == Kind::Object && members.count(key) != 0;
    }
    [[nodiscard]] std::size_t as_size() const
    {
        SW_CHECK(kind == Kind::Number && number >= 0 &&
                 number == static_cast<double>(static_cast<std::uint64_t>(number)));
        return static_cast<std::size_t>(number);
    }
    [[nodiscard]] const std::string& as_string() const
    {
        SW_CHECK(kind == Kind::String);
        return text;
    }
};

class JsonParser
{
public:
    explicit JsonParser(std::string_view source) : source_(source) {}

    Json parse()
    {
        Json value = parse_value();
        skip_space();
        SW_CHECK(position_ == source_.size());
        return value;
    }

private:
    void skip_space()
    {
        while (position_ < source_.size() &&
               std::string_view(" \t\r\n").find(source_[position_]) != std::string_view::npos) {
            ++position_;
        }
    }
    char peek()
    {
        skip_space();
        SW_CHECK(position_ < source_.size());
        return source_[position_];
    }
    void expect(char c)
    {
        SW_CHECK(peek() == c);
        ++position_;
    }
    bool take_literal(std::string_view literal)
    {
        if (source_.substr(position_, literal.size()) == literal) {
            position_ += literal.size();
            return true;
        }
        return false;
    }

    std::string parse_string()
    {
        expect('"');
        std::string out;
        while (true) {
            SW_CHECK(position_ < source_.size());
            const char c = source_[position_++];
            if (c == '"') {
                return out;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            SW_CHECK(position_ < source_.size());
            const char escaped = source_[position_++];
            switch (escaped) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'u': append_utf8(out, parse_hex4()); break;
            default: SW_CHECK(false && "escape de JSON invalido");
            }
        }
    }

    // Escape unicode de 4 digitos hexa: so o plano basico (sem pares substitutos), o bastante
    // para o texto dos vetores.
    std::uint32_t parse_hex4()
    {
        static constexpr std::string_view kDigits = "0123456789abcdef";
        SW_CHECK(position_ + 4 <= source_.size());
        std::uint32_t value = 0;
        for (int k = 0; k < 4; ++k) {
            const char digit =
                static_cast<char>(std::tolower(static_cast<unsigned char>(source_[position_++])));
            const std::size_t found = kDigits.find(digit);
            SW_CHECK(found != std::string_view::npos);
            value = value * 16 + static_cast<std::uint32_t>(found);
        }
        SW_CHECK(value < 0xD800 || value > 0xDFFF);
        return value;
    }

    static void append_utf8(std::string& out, std::uint32_t code)
    {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    Json parse_value()
    {
        Json value;
        const char c = peek();
        if (c == '{') {
            ++position_;
            value.kind = Json::Kind::Object;
            if (peek() == '}') {
                ++position_;
                return value;
            }
            while (true) {
                std::string key = parse_string();
                expect(':');
                value.members.emplace(std::move(key), parse_value());
                if (peek() == ',') {
                    ++position_;
                    continue;
                }
                expect('}');
                return value;
            }
        }
        if (c == '[') {
            ++position_;
            value.kind = Json::Kind::Array;
            if (peek() == ']') {
                ++position_;
                return value;
            }
            while (true) {
                value.items.push_back(parse_value());
                if (peek() == ',') {
                    ++position_;
                    continue;
                }
                expect(']');
                return value;
            }
        }
        if (c == '"') {
            value.kind = Json::Kind::String;
            value.text = parse_string();
            return value;
        }
        const bool is_true = take_literal("true");
        if (is_true || take_literal("false")) {
            value.kind = Json::Kind::Bool;
            value.boolean = is_true;
            return value;
        }
        if (take_literal("null")) {
            return value;
        }
        const std::size_t start = position_;
        while (position_ < source_.size() &&
               std::string_view("+-.eE0123456789").find(source_[position_]) !=
                   std::string_view::npos) {
            ++position_;
        }
        SW_CHECK(position_ > start);
        value.kind = Json::Kind::Number;
        value.number =
            std::strtod(std::string(source_.substr(start, position_ - start)).c_str(), nullptr);
        return value;
    }

    std::string_view source_;
    std::size_t position_ = 0;
};

// ---------------------------------------------------------------- utilidades

using Bytes = std::vector<std::byte>;

std::span<const std::byte> as_bytes(std::string_view text)
{
    return std::as_bytes(std::span<const char>(text.data(), text.size()));
}

std::string hex_of(std::span<const std::byte> data) { return sp::to_hex(sp::sha256(data)); }

// stream(label, n) = SHA-256("swarmstream-vec-v0|" ‖ label ‖ u32le(contador)) concatenado a
// partir do contador 0 e cortado em n bytes (sw-pack/testdata/README.md).
Bytes stream(std::string_view label, std::size_t n)
{
    Bytes out;
    out.reserve(n + 32);
    for (std::uint32_t counter = 0; out.size() < n; ++counter) {
        sp::Sha256 hasher;
        hasher.update(std::string_view("swarmstream-vec-v0|"));
        hasher.update(label);
        const std::uint8_t le[4] = {
            static_cast<std::uint8_t>(counter), static_cast<std::uint8_t>(counter >> 8),
            static_cast<std::uint8_t>(counter >> 16), static_cast<std::uint8_t>(counter >> 24)};
        hasher.update(std::as_bytes(std::span<const std::uint8_t>(le)));
        const sp::Sha256Digest digest = hasher.finish();
        for (const std::uint8_t b : digest) {
            out.push_back(static_cast<std::byte>(b));
        }
    }
    out.resize(n);
    return out;
}

std::vector<std::uint8_t> from_hex(const std::string& hex)
{
    SW_CHECK(hex.size() % 2 == 0);
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

std::size_t total(const std::vector<std::size_t>& lengths)
{
    std::size_t sum = 0;
    for (const std::size_t length : lengths) {
        sum += length;
    }
    return sum;
}

// ---------------------------------------------------------------- testes

void test_sha256_known_answers()
{
    std::puts("[Test] SHA-256: respostas conhecidas (FIPS 180-4)");
    const auto digest_of = [](std::string_view text) {
        return sp::to_hex(sp::sha256(as_bytes(text)));
    };
    SW_CHECK(digest_of("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    SW_CHECK(digest_of("abc") ==
             "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    SW_CHECK(digest_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
             "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    // um milhão de 'a', em pedaços de tamanhos que não alinham com o bloco de 64 bytes
    sp::Sha256 hasher;
    const std::string chunk(997, 'a');
    std::size_t sent = 0;
    while (sent + chunk.size() <= 1'000'000) {
        hasher.update(std::string_view(chunk));
        sent += chunk.size();
    }
    hasher.update(std::string_view(chunk.data(), 1'000'000 - sent));
    SW_CHECK(sp::to_hex(hasher.finish()) ==
             "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

void test_gear_table(const Json& chunker)
{
    std::puts("[Test] Chunker: tabela gear");
    const sp::GearTable& table = sp::Chunker::gear_table();

    // Os 4 primeiros valores: o JSON os escreve como o NUMERO de 64 bits em hexa (digito mais
    // significativo primeiro), nao como os bytes do SHA-256 na ordem little-endian.
    const Json& first4 = chunker.at("gear_first4_hex");
    SW_CHECK(first4.items.size() == 4);
    for (std::size_t i = 0; i < 4; ++i) {
        const std::vector<std::uint8_t> digits = from_hex(first4.items[i].as_string());
        SW_CHECK(digits.size() == 8);
        std::uint64_t value = 0;
        for (const std::uint8_t byte : digits) {
            value = (value << 8U) | byte;
        }
        SW_CHECK(table[i] == value);
    }

    // SHA-256 dos 256 valores, cada um em little-endian (2048 bytes).
    Bytes serialized;
    for (const std::uint64_t value : table) {
        for (std::size_t k = 0; k < 8; ++k) {
            serialized.push_back(static_cast<std::byte>(value >> (8U * k)));
        }
    }
    SW_CHECK(hex_of(serialized) == chunker.at("gear_table_sha256").as_string());
}

void test_vector_cases(const Json& chunker)
{
    std::puts("[Test] Chunker: vetores do vectors-v0.json");
    SW_CHECK(chunker.at("algorithm").as_string() == sp::Chunker::kAlgorithm);
    const auto& cases = chunker.at("cases").items;
    std::size_t reproduced = 0;
    for (const Json& test_case : cases) {
        const std::string& label = test_case.at("label").as_string();
        const std::size_t size = test_case.at("size").as_size();
        const sp::ChunkerParams params{static_cast<std::uint32_t>(test_case.at("min").as_size()),
                                       static_cast<std::uint32_t>(test_case.at("avg").as_size()),
                                       static_cast<std::uint32_t>(test_case.at("max").as_size())};

        Bytes data;
        if (test_case.has("zeros") && test_case.at("zeros").boolean) {
            data.assign(size, std::byte{0});
        } else {
            data = stream(test_case.at("stream").as_string(), size);
        }
        if (hex_of(data) != test_case.at("data_sha256").as_string()) {
            std::fprintf(stderr,
                         "caso '%s': os dados de entrada gerados nao batem com data_sha256\n",
                         label.c_str());
            std::exit(1);
        }

        const auto chunker_instance = sp::Chunker::make(params);
        SW_CHECK(chunker_instance.has_value());
        const std::vector<std::size_t> lengths = chunker_instance->split(data);

        const auto& expected = test_case.at("chunks").items;
        if (lengths.size() != expected.size()) {
            std::fprintf(stderr, "caso '%s': %zu blocos, esperados %zu\n", label.c_str(),
                         lengths.size(), expected.size());
            std::exit(1);
        }
        std::size_t offset = 0;
        for (std::size_t i = 0; i < lengths.size(); ++i) {
            if (lengths[i] != expected[i].at("len").as_size()) {
                std::fprintf(stderr, "caso '%s': bloco %zu tem %zu bytes, esperados %zu\n",
                             label.c_str(), i, lengths[i], expected[i].at("len").as_size());
                std::exit(1);
            }
            const auto block = std::span<const std::byte>(data).subspan(offset, lengths[i]);
            if (hex_of(block) != expected[i].at("sha256").as_string()) {
                std::fprintf(stderr, "caso '%s': bloco %zu com SHA-256 diferente\n", label.c_str(),
                             i);
                std::exit(1);
            }
            offset += lengths[i];
        }
        SW_CHECK(offset == size);
        ++reproduced;
        std::printf("  ok  %-20s %9zu bytes  %3zu blocos\n", label.c_str(), size, lengths.size());
    }
    std::printf("VETORES DO CHUNKER REPRODUZIDOS: %zu de %zu\n", reproduced, cases.size());
    SW_CHECK(reproduced == cases.size() && !cases.empty());
}

void test_invariants()
{
    std::puts("[Test] Chunker: invariantes em dados pseudoaleatorios e repetidos");
    const auto chunker = sp::Chunker::make();
    SW_CHECK(chunker.has_value());
    const sp::ChunkerParams params = chunker->params();

    for (const std::size_t size :
         {std::size_t{0}, std::size_t{1}, std::size_t{params.min_size - 1},
          std::size_t{params.min_size}, std::size_t{params.min_size + 1},
          std::size_t{params.max_size}, std::size_t{params.max_size + 1}, std::size_t{3'000'000}}) {
        for (const bool zeros : {false, true}) {
            Bytes data = zeros ? Bytes(size, std::byte{0}) : stream("invariants", size);
            const std::vector<std::size_t> lengths = chunker->split(data);
            SW_CHECK(total(lengths) == size);
            SW_CHECK(lengths.empty() == (size == 0));
            for (std::size_t i = 0; i < lengths.size(); ++i) {
                SW_CHECK(lengths[i] >= 1 && lengths[i] <= params.max_size);
                if (i + 1 < lengths.size()) {
                    SW_CHECK(lengths[i] > params.min_size); // só o último bloco pode ser <= min
                }
            }
            // cut() com uma janela de pelo menos max_size bytes dá o mesmo primeiro corte
            if (size > 0) {
                const std::size_t window = std::min<std::size_t>(size, params.max_size);
                SW_CHECK(chunker->cut(std::span<const std::byte>(data).first(window)) ==
                         lengths.front());
            }
        }
    }
    SW_CHECK(chunker->cut({}) == 0);
}

void test_invalid_params()
{
    std::puts("[Test] Chunker: parametros invalidos sao recusados");
    SW_CHECK(sp::Chunker::make({0, 1024, 4096}) == std::nullopt);    // min = 0
    SW_CHECK(sp::Chunker::make({2048, 1024, 4096}) == std::nullopt); // min > avg
    SW_CHECK(sp::Chunker::make({256, 8192, 4096}) == std::nullopt);  // avg > max
    SW_CHECK(sp::Chunker::make({256, 1000, 4096}) == std::nullopt);  // avg nao e potencia de 2
    SW_CHECK(sp::Chunker::make({1, 4, 16}) == std::nullopt); // avg < 8: log2(avg) - 2 < 1 bit
    SW_CHECK(sp::Chunker::make({256, 1024, 4096}).has_value());
    SW_CHECK(sp::Chunker::make({1, 8, 8}).has_value());
}

// Transcrição literal do pseudocódigo de sw-pack/testdata/README.md (um laço só, máscara escolhida
// a cada posição). Lenta e sem otimização: serve de referência para a versão com dois laços.
std::size_t reference_cut(std::span<const std::byte> data, const sp::ChunkerParams& params)
{
    const std::size_t n = data.size();
    if (n <= params.min_size) {
        return n;
    }
    const auto high_bits = [](unsigned bits) { return ~std::uint64_t{0} << (64U - bits); };
    const auto log2_avg = static_cast<unsigned>(std::bit_width(params.avg_size) - 1);
    const std::uint64_t mask_s = high_bits(log2_avg + 2);
    const std::uint64_t mask_l = high_bits(log2_avg - 2);
    const std::size_t limit = std::min<std::size_t>(n, params.max_size);
    const std::size_t normal = std::min<std::size_t>(params.avg_size, limit);
    const sp::GearTable& gear = sp::Chunker::gear_table();
    std::uint64_t fp = 0;
    for (std::size_t i = params.min_size; i < limit; ++i) {
        fp = (fp << 1) + gear[std::to_integer<std::size_t>(data[i])];
        if ((fp & (i < normal ? mask_s : mask_l)) == 0) {
            return i + 1;
        }
    }
    return limit;
}

// Os vetores do JSON quase não exercitam as bordas (primeiras posições depois de `min`, a posição
// `avg` em que a máscara troca): com os parâmetros padrão um corte ali tem chance de ~1e-4 por
// bloco. Com parâmetros minúsculos esses cortes são comuns, então comparamos com a referência.
void test_against_literal_reference()
{
    std::puts(
        "[Test] Chunker: bordas (min, avg, max) contra a transcricao literal do pseudocodigo");
    const sp::ChunkerParams cases[] = {{1, 8, 8},     {1, 8, 64},    {4, 16, 64},
                                       {16, 64, 256}, {64, 64, 128}, {256, 1024, 4096}};
    std::size_t compared = 0;
    for (const sp::ChunkerParams& params : cases) {
        const auto chunker = sp::Chunker::make(params);
        SW_CHECK(chunker.has_value());
        for (const std::string_view label : {"edges-a", "edges-b"}) {
            const Bytes data = stream(label, 150'000);
            std::size_t offset = 0;
            while (offset < data.size()) {
                const auto rest = std::span<const std::byte>(data).subspan(offset);
                const std::size_t expected = reference_cut(rest, params);
                if (chunker->cut(rest) != expected) {
                    std::fprintf(
                        stderr, "min=%u avg=%u max=%u: corte em %zu difere da referencia (%zu)\n",
                        params.min_size, params.avg_size, params.max_size, offset, expected);
                    std::exit(1);
                }
                offset += expected;
                ++compared;
            }
        }
    }
    std::printf("  %zu cortes comparados\n", compared);
    SW_CHECK(compared > 5'000);
}

// Quantos blocos de `after` não existem em `before`, comparando por conteúdo (SHA-256).
std::size_t changed_blocks(const sp::Chunker& chunker, std::span<const std::byte> before,
                           std::span<const std::byte> after)
{
    const auto hashes = [&](std::span<const std::byte> data) {
        std::vector<std::string> out;
        std::size_t offset = 0;
        for (const std::size_t length : chunker.split(data)) {
            out.push_back(hex_of(data.subspan(offset, length)));
            offset += length;
        }
        return out;
    };
    const std::vector<std::string> old_blocks = hashes(before);
    const std::set<std::string> known(old_blocks.begin(), old_blocks.end());
    std::size_t changed = 0;
    for (const std::string& block : hashes(after)) {
        changed += known.count(block) == 0 ? 1 : 0;
    }
    return changed;
}

void test_stability(std::size_t size, bool heavy)
{
    std::printf("[Test] Chunker: estabilidade (%zu MiB%s)\n", size >> 20, heavy ? ", pesado" : "");
    const auto chunker = sp::Chunker::make();
    SW_CHECK(chunker.has_value());

    Bytes data = stream("stability", size);
    data.reserve(size + 1); // a inserção abaixo não realoca (1 GiB não pode ser copiado duas vezes)
    const std::size_t middle = size / 2 + 123;

    // Hashes dos blocos originais, guardados uma vez (sem copiar o arquivo todo, que no teste
    // pesado tem 1 GiB). Um bloco "novo" é um que não existe no arquivo original.
    // 1) mudar 1 byte no meio
    const std::vector<std::size_t> before_lengths = chunker->split(data);
    std::vector<std::string> before_hashes;
    {
        std::size_t offset = 0;
        for (const std::size_t length : before_lengths) {
            before_hashes.push_back(
                hex_of(std::span<const std::byte>(data).subspan(offset, length)));
            offset += length;
        }
    }
    const std::set<std::string> known(before_hashes.begin(), before_hashes.end());
    const auto count_new = [&](const Bytes& changed) {
        std::size_t offset = 0;
        std::size_t novel = 0;
        for (const std::size_t length : chunker->split(changed)) {
            novel += known.count(
                         hex_of(std::span<const std::byte>(changed).subspan(offset, length))) == 0
                         ? 1
                         : 0;
            offset += length;
        }
        return novel;
    };

    data[middle] ^= std::byte{0xFF};
    const std::size_t replaced = count_new(data);
    data[middle] ^= std::byte{0xFF};
    std::printf("  mudar 1 byte no meio: %zu bloco(s) novo(s) de %zu\n", replaced,
                before_lengths.size());
    SW_CHECK(replaced >= 1 && replaced <= 3);

    // 2) inserir 1 byte no meio (desloca todo o resto)
    data.insert(data.begin() + static_cast<std::ptrdiff_t>(middle), std::byte{0x5A});
    const std::size_t inserted = count_new(data);
    std::printf("  inserir 1 byte no meio: %zu bloco(s) novo(s)\n", inserted);
    SW_CHECK(inserted >= 1 && inserted <= 3);
    data.erase(data.begin() + static_cast<std::ptrdiff_t>(middle));

    // 3) remover 1 byte do meio
    data.erase(data.begin() + static_cast<std::ptrdiff_t>(middle));
    const std::size_t removed = count_new(data);
    std::printf("  remover 1 byte do meio: %zu bloco(s) novo(s)\n", removed);
    SW_CHECK(removed >= 1 && removed <= 3);
}

void test_stability_small_helper()
{
    std::puts("[Test] Chunker: estabilidade (helper de comparacao, 4 MiB)");
    const auto chunker = sp::Chunker::make();
    SW_CHECK(chunker.has_value());
    Bytes data = stream("stability-small", 4U << 20);
    Bytes edited = data;
    edited[2'000'000] ^= std::byte{0x01};
    const std::size_t changed = changed_blocks(*chunker, data, edited);
    std::printf("  mudar 1 byte em 4 MiB: %zu bloco(s) novo(s)\n", changed);
    SW_CHECK(changed >= 1 && changed <= 3);
    SW_CHECK(changed_blocks(*chunker, data, data) == 0);
}

void test_throughput()
{
    const auto chunker = sp::Chunker::make();
    SW_CHECK(chunker.has_value());
    const Bytes data = stream("throughput", 16U << 20);
    const auto start = std::chrono::steady_clock::now();
    const std::size_t blocks = chunker->split(data).size();
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("[Info] vazao do corte: %.0f MiB/s (16 MiB, %zu blocos; informativo, sem limite)\n",
                16.0 / seconds, blocks);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "uso: %s <vectors-v0.json> [--heavy]\n", argv[0]);
        return 2;
    }
    const bool heavy = argc > 2 && std::strcmp(argv[2], "--heavy") == 0;

    std::ifstream file(argv[1], std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "nao consegui abrir %s\n", argv[1]);
        return 2;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    const Json document = JsonParser(buffer.str()).parse();
    const Json& chunker = document.at("chunker");

    if (heavy) {
        test_stability(1ULL << 30, true);
        std::puts("TESTE PESADO OK");
        return 0;
    }
    test_sha256_known_answers();
    test_gear_table(chunker);
    test_vector_cases(chunker);
    test_invariants();
    test_invalid_params();
    test_against_literal_reference();
    test_stability_small_helper();
    test_stability(8U << 20, false);
    test_throughput();
    std::puts("TODOS OS TESTES DO CHUNKER PASSARAM");
    return 0;
}
