// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres
//
// Testes da parte da GDExtension que não depende do Godot (rodam no CI):
// o adaptador GateCore (semântica do swarmstream.h sobre os mocks do sw-core)
// e os leitores dos formatos do portão.

#include "gate_core.h"
#include "gate_formats.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "OK  " : "FAIL", what);
    if (!ok) ++g_failures;
}

void put32(std::vector<uint8_t>& b, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void put64(std::vector<uint8_t>& b, uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void putf(std::vector<uint8_t>& b, float f)
{
    uint32_t v = 0;
    std::memcpy(&v, &f, 4);
    put32(b, v);
}

std::vector<uint8_t> make_tex(uint32_t w, uint32_t h, size_t data_len, uint8_t fill)
{
    std::vector<uint8_t> b = {'S', 'W', 'T', 'X'};
    put32(b, 1);
    put32(b, w);
    put32(b, h);
    put32(b, 17); // Image::FORMAT_DXT1
    put32(b, 1);
    put64(b, data_len);
    for (size_t i = 0; i < data_len; ++i)
        b.push_back(static_cast<uint8_t>(fill + i * 7));
    return b;
}

std::vector<uint8_t> make_mesh(uint32_t bad_index = 0)
{
    std::vector<uint8_t> b = {'S', 'W', 'M', 'S'};
    put32(b, 1);
    put32(b, 3); // vértices
    put32(b, 3); // índices
    for (float f : {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f})
        putf(b, f); // posição
    for (int i = 0; i < 3; ++i) {
        for (float f : {0.f, 0.f, 1.f})
            putf(b, f); // normal
    }
    for (float f : {0.f, 0.f, 1.f, 0.f, 0.f, 1.f})
        putf(b, f); // uv
    put32(b, 0);
    put32(b, 1);
    put32(b, bad_index ? bad_index : 2);
    return b;
}

void test_formats()
{
    swgate::TexBlob t;
    auto tex = make_tex(8, 8, 32, 1);
    check(swgate::parse_tex(tex, t) && t.width == 8 && t.size == 32 && t.format == 17,
          "parse_tex aceita textura válida");
    auto truncated = tex;
    truncated.pop_back();
    check(!swgate::parse_tex(truncated, t), "parse_tex recusa dados truncados");
    auto zero = make_tex(0, 8, 32, 1);
    check(!swgate::parse_tex(zero, t), "parse_tex recusa largura 0");
    std::vector<uint8_t> tiny = {'S', 'W', 'T', 'X', 1, 0};
    check(!swgate::parse_tex(tiny, t), "parse_tex recusa cabeçalho curto");

    swgate::MeshBlob m;
    auto mesh = make_mesh();
    check(swgate::parse_mesh(mesh, m) && m.vertices == 3 && m.indices == 3,
          "parse_mesh aceita malha válida");
    check(!swgate::parse_mesh(make_mesh(7), m), "parse_mesh recusa índice fora dos vértices");
    auto short_mesh = mesh;
    short_mesh.resize(short_mesh.size() - 4);
    check(!swgate::parse_mesh(short_mesh, m), "parse_mesh recusa tamanho errado");
}

void write_file(const std::filesystem::path& p, const std::vector<uint8_t>& b)
{
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

void test_gate_core()
{
    namespace fs = std::filesystem;
    // Pasta única por execução: duas execuções ao mesmo tempo não disputam os arquivos.
    const fs::path dir =
        fs::temp_directory_path() /
        ("swarmstream_gate_core_test_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    const auto big = make_tex(64, 64, (2u << 20) + 12345, 3); // 3 peças de 1 MiB
    const auto low = make_tex(8, 8, 32, 5);
    write_file(dir / "rock_4k.swtex", big);
    write_file(dir / "rock_4k.low.swtex", low);
    write_file(dir / "sphere.swmesh", make_mesh());
    write_file(dir / "sphere.low.swmesh", make_mesh());
    write_file(dir / "tone.ogg", std::vector<uint8_t>(5000, 0x4f));

    swgate::GateCore& core = swgate::GateCore::instance();
    check(core.init((dir / "nao_existe").string(), 0) == SW_E_NOT_FOUND,
          "init recusa pasta sem os arquivos");
    check(core.init(dir.string(), 2000) == SW_OK, "init com latência de 2000 ms");
    check(core.is_local("rock_4k.low.swtex") && !core.is_local("rock_4k.swtex"),
          "só o grupo core começa no cache");

    std::vector<uint8_t> buf(low.size());
    uint64_t got = 0;
    check(core.read("rock_4k.low.swtex", 0, buf.data(), buf.size(), &got) == SW_OK &&
              got == low.size() && buf == low,
          "read do core devolve os bytes exatos");

    // Aquecimento: a 1ª leitura fora do cache do processo cria a primeira thread de pedido e
    // pode levar >100 ms numa máquina carregada (medido pela revisão do PR #26). Ela não entra
    // na medição: mede-se a 2ª, de outro arquivo.
    // Folga (F1-28): o read não espera a fonte, mas cria 6 threads (uma por peça no mock e outra
    // aqui) dentro do trecho medido. Parado leva 0-1 ms; com a CPU sobrecarregada (16 processos
    // em 8 núcleos) levou 66-226 ms, e o limite antigo (150 ms contra 300 ms de latência) falhava
    // 10 de 24 vezes. Com 2000 ms de latência e limite de 1000 ms, esperar a fonte continua
    // impossível de passar, e o pior caso medido fica com mais de 4x de folga. A F1-50 (pool de
    // threads) tira a criação de threads deste caminho.
    std::vector<uint8_t> warm(5000);
    check(core.read("tone.ogg", 0, warm.data(), warm.size(), &got) == SW_E_NOT_LOCAL,
          "aquecimento: tone.ogg fora do cache");
    std::vector<uint8_t> full(big.size());
    const auto t0 = std::chrono::steady_clock::now();
    const sw_result r = core.read("rock_4k.swtex", 0, full.data(), full.size(), &got);
    const auto dt = std::chrono::steady_clock::now() - t0;
    check(r == SW_E_NOT_LOCAL && got == 0, "read fora do cache devolve SW_E_NOT_LOCAL sem copiar");
    std::printf(
        "     read fora do cache levou %lld ms\n",
        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(dt).count()));
    check(dt < std::chrono::milliseconds(1000),
          "read fora do cache volta sem esperar a latência (< 1000 ms com 2000 ms)");

    std::atomic<int> calls{0};
    std::atomic<sw_result> result{SW_E_INTERNAL};
    std::atomic<uint64_t> bytes{0}, id_seen{0};
    std::atomic<bool> inside{true};
    const uint64_t id =
        core.ensure_async("rock_4k.swtex", [&](sw_result res, uint64_t n, uint64_t eid) {
            check(!inside.load(),
                  "callback não roda na pilha de ensure_async (com 2000 ms de latência)");
            result = res;
            bytes = n;
            id_seen = eid;
            calls.fetch_add(1);
        });
    inside = false;
    check(id != 0, "ensure_async aceita o pedido");
    for (int i = 0; i < 1500 && calls.load() == 0; ++i) // até 15 s (latência de 2 s)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    check(calls.load() == 1, "callback chamado exatamente uma vez");
    check(result.load() == SW_OK && bytes.load() == big.size() && id_seen.load() == id,
          "callback com SW_OK, bytes do arquivo e o mesmo id");
    check(core.read("rock_4k.swtex", 0, full.data(), full.size(), &got) == SW_OK && full == big,
          "depois do ensure o read devolve os bytes exatos das 3 peças");
    std::vector<uint8_t> mid(1000);
    check(core.read("rock_4k.swtex", (1u << 20) - 500, mid.data(), mid.size(), &got) == SW_OK &&
              std::memcmp(mid.data(), big.data() + (1u << 20) - 500, 1000) == 0,
          "leitura que cruza duas peças");
    core.release(id);

    check(core.read("nao_existe", 0, buf.data(), 1, &got) == SW_E_NOT_FOUND,
          "caminho desconhecido = SW_E_NOT_FOUND");
    check(core.read("tone.ogg", 999999, buf.data(), 1, &got) == SW_E_OUT_OF_RANGE,
          "offset além do fim = SW_E_OUT_OF_RANGE");
    std::atomic<int> never{0};
    check(core.ensure_async("nao_existe", [&](sw_result, uint64_t, uint64_t) { never++; }) == 0,
          "ensure_async de caminho desconhecido devolve 0");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    check(never.load() == 0, "e nesse caso o callback nunca é chamado");
    const auto st = core.stats();
    check(st.reads_not_local == 2 && st.ensures == 1 && st.pieces_fetched == 4,
          "contadores do mock");
    core.shutdown();
    fs::remove_all(dir);
}

} // namespace

int main()
{
    test_formats();
    test_gate_core();
    std::printf("%s (%d falhas)\n", g_failures == 0 ? "gate_core: ok" : "gate_core: FALHOU",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
