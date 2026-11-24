// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Registro global de handles da API C, contagem de uso (pin) e quarentena de cascas (F1-67 parte
// B; desenho do F1-60 §1.3 e §1.4, aprovado pelo dono em 2026-11-02 como MITIGAÇÃO LIMITADA).
// Interno do sw-core: nada aqui é exportado; a API pública (partes C e D) é quem usa.
//
// COMO A API ACHA O CONTEXTO SEM LER UM ENDEREÇO NÃO VALIDADO. `sw_file*`, `sw_dir*` e `sw_ctx*`
// NÃO são lidos antes de serem achados neste registro do processo: uma tabela de endereços
// emitidos, em 64 fatias (a fatia sai SÓ do valor do ponteiro), cada fatia com um mutex curto e
// endereçamento aberto. A entrada guarda {endereço, tipo, estado (VIVO/FECHANDO), dono, PinCore*}.
// O dono vem DA ENTRADA, nunca de um campo do objeto apontado. Endereço ausente (NULL, nunca
// emitido, interior de um handle, fechado e removido) => `Unknown`, sem ler um byte dele.
//
// PIN (contagem de uso). Tomado SOMENTE sob o mutex da fatia e SOMENTE se a entrada está VIVA;
// solto sem mutex de fatia. Depois de a entrada virar FECHANDO a contagem só pode DIMINUIR. A
// contagem e o bit FECHANDO ficam numa palavra atômica do `PinCore`, que vive no CORPO do handle
// (não na casca nem no registro): o fechador espera nela, numa variável de condição do próprio
// objeto. Protocolo (sem perda de despertar e sem uso depois da liberação):
//   - unpin sem FECHANDO: um CAS que decrementa; depois dele a thread não toca mais no objeto;
//   - unpin com FECHANDO: decrementa SOB o mutex do PinCore e acorda o fechador se chegou a 0;
//   - o fechador liga FECHANDO sob o mutex da fatia (nenhum pin novo depois disso), solta a fatia
//     e espera, sob o mutex do PinCore, a contagem chegar a 0. Só vê 0 depois de o último
//     decremento lento soltar o mutex (ou de um decremento rápido ANTES de FECHANDO, que não toca
//     mais em nada). Destruir o mutex logo depois do unlock de outra thread é o padrão de objeto
//     com contagem de referência que o POSIX e o SRWLOCK aceitam (registrado como premissa).
// Regras de ordem: nenhum código espera (pins, callbacks, disco, rede) com mutex de fatia na mão;
// o mutex de fatia nunca é tomado dentro do mutex de um PinCore; o fechador não toma pin, então
// não espera o próprio uso.
//
// ALOCAÇÃO. Busca e pin não alocam. A inserção pode crescer a tabela da fatia (alocador do
// sistema, `new (std::nothrow)`, exceção declarada do F1-60 §7: o registro é do processo e é
// compartilhado por contextos com alocadores diferentes); falha => `OutOfMemory`, nada muda. A
// tabela de uma fatia é DEVOLVIDA quando a última entrada dela sai.
//
// QUARENTENA (F1-60 §1.4): o endereço de um handle FECHADO não é reemitido enquanto a casca dele
// estiver retida: as 256 últimas cascas de `sw_file`/`sw_dir` fechadas de cada contexto
// (`ShellQuarantine<kFileQuarantine>`, no contexto) e as 64 últimas cascas de `sw_ctx` desligados
// do processo (`ProcessCtxQuarantine`, vetor estático: memória alcançável no fim do processo, não
// vazamento para o LSan). Nesse intervalo o ponteiro antigo é rejeitado (não está no registro e o
// alocador não pode devolver o mesmo endereço). Fora dele o endereço volta ao alocador e PODE SER
// REEMITIDO; a partir daí o ponteiro antigo vale como o handle NOVO e a API não distingue. ISSO NÃO
// É PROMESSA DE DETECÇÃO: é mitigação de diagnóstico, não de segurança. Garantido só: NULL,
// endereço nunca emitido (inclusive interior) e tipo errado são rejeitados. Não prometido: handle
// fechado fora da janela; uso concorrente durante o fechamento (proibido pelo header; pin e
// FECHANDO só evitam corrupção de memória).
//
// CASCAS: o objeto cujo endereço o chamador conhece tem tamanho fixo e pequeno (no máximo 64
// bytes, `static_assert` abaixo): tipo, número de série interno e o ponteiro para o corpo, que é
// zerado no fechamento. Todo o resto (pedidos, leases, estado de iteração, cópias, PinCore) vive no
// corpo e é devolvido ao alocador no fechamento. Só a casca fica em quarentena.
//
// MSVC Debug (Regra R do F1-69/F1-72): nada aqui usa contêiner da STL; nenhum `unordered_*` (no
// MSVC com _ITERATOR_DEBUG_LEVEL != 0 qualquer construtor deles pode virar std::terminate sem
// memória). A tabela é um vetor cru de `Entry` trivial.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace swarmstream::detail {

enum class HandleKind : std::uint32_t { None = 0, Context = 1, File = 2, Dir = 3 };

[[nodiscard]] const char* to_string(HandleKind kind) noexcept;

} // namespace swarmstream::detail

// Cascas dos handles opacos do swarmstream.h (definidas aqui, no namespace global, porque o header
// declara `typedef struct sw_file sw_file;`). Só a casca tem o endereço que o chamador conhece.
struct sw_ctx
{
    swarmstream::detail::HandleKind kind = swarmstream::detail::HandleKind::Context;
    std::uint32_t reserved = 0;
    std::uint64_t serial = 0; // número de série interno (diagnóstico)
    void* body = nullptr;     // o Context; nulo depois do desligamento
};

struct sw_file
{
    swarmstream::detail::HandleKind kind = swarmstream::detail::HandleKind::File;
    std::uint32_t reserved = 0;
    std::uint64_t serial = 0;
    void* body = nullptr; // o corpo do arquivo aberto; nulo depois do fechamento
};

struct sw_dir
{
    swarmstream::detail::HandleKind kind = swarmstream::detail::HandleKind::Dir;
    std::uint32_t reserved = 0;
    std::uint64_t serial = 0;
    void* body = nullptr;
};

namespace swarmstream::detail {

inline constexpr std::size_t kMaxShellBytes = 64;
static_assert(sizeof(sw_ctx) <= kMaxShellBytes, "a casca do sw_ctx passa de 64 bytes (F1-60 §1.3)");
static_assert(sizeof(sw_file) <= kMaxShellBytes, "a casca do sw_file passa de 64 bytes");
static_assert(sizeof(sw_dir) <= kMaxShellBytes, "a casca do sw_dir passa de 64 bytes");

namespace testing {
class PinCoreTestHook;
} // namespace testing

// Contagem de uso de um handle e o bit FECHANDO (ver o topo). Vive no corpo do handle.
class PinCore
{
public:
    PinCore() noexcept = default;
    PinCore(const PinCore&) = delete;
    PinCore& operator=(const PinCore&) = delete;

    // Solta um pin tomado por HandleRegistry::acquire. Sem mutex de fatia.
    void unpin() noexcept;

    // Usado pelo fechador DEPOIS de HandleRegistry::begin_close: espera os pins de OUTROS usuários
    // chegarem a 0. O fechador não tem pin. Não chame com mutex de fatia na mão (nenhum código
    // daqui o faz).
    void wait_unpinned() noexcept;

    [[nodiscard]] std::uint32_t pins() const noexcept
    {
        return word_.load(std::memory_order_acquire) & ~kClosing;
    }
    [[nodiscard]] bool closing() const noexcept
    {
        return (word_.load(std::memory_order_acquire) & kClosing) != 0;
    }

private:
    friend class HandleRegistry;
    friend class testing::PinCoreTestHook;
    static constexpr std::uint32_t kClosing = std::uint32_t{1} << 31;

    // O decremento do unpin lento (sob m_), com o ponto de interposição dos testes logo depois.
    [[nodiscard]] std::uint32_t drop_one_slow() noexcept;

    // Só sob o mutex da fatia (HandleRegistry).
    [[nodiscard]] bool pin_locked() noexcept;
    void set_closing_locked() noexcept;
    void clear_closing_locked() noexcept;

    std::atomic<std::uint32_t> word_{0};
    std::mutex m_;
    std::condition_variable cv_;
};

enum class HandleStatus : std::uint8_t {
    Ok = 0,
    Unknown,     // endereço ausente: NULL, nunca emitido, interior, fechado e já removido
    WrongKind,   // está no registro, mas é de outro tipo (arquivo no lugar de diretório...)
    Closing,     // está FECHANDO: pedido novo => SW_E_CANCELLED; outro fechador => CANCELLED
    NotClosing,  // remove/cancel_close numa entrada VIVA (erro de uso interno)
    Duplicate,   // insert de um endereço que já está no registro (erro de uso interno)
    OutOfMemory, // insert: a tabela da fatia não pôde crescer
    PinOverflow, // acquire: contagem de pins no limite (2^31 - 1 usos simultâneos)
};

[[nodiscard]] const char* to_string(HandleStatus status) noexcept;

struct HandleLookup
{
    HandleStatus status = HandleStatus::Unknown;
    void* owner = nullptr;   // o dono (Context*) registrado na entrada; nulo se não Ok
    PinCore* core = nullptr; // nulo se não Ok
};

class HandleRegistry
{
public:
    static constexpr std::size_t kShards = 64;

    // O registro do processo. Nunca é destruído (vive num armazenamento estático: as tabelas que
    // restarem no fim do processo são alcançáveis, não vazamento). Não aloca para existir.
    [[nodiscard]] static HandleRegistry& instance() noexcept;

    // Para os testes: registros próprios (o comportamento é o mesmo do global).
    HandleRegistry() noexcept = default;
    ~HandleRegistry();
    HandleRegistry(const HandleRegistry&) = delete;
    HandleRegistry& operator=(const HandleRegistry&) = delete;

    // Registra `addr` VIVO com `kind`, `owner` e `core` (que tem de viver até o remove). Zera o
    // `core` (sem pins, sem FECHANDO): PRECONDIÇÃO, o core é novo ou o fechamento anterior dele
    // terminou (ninguém mais o usa). Pode crescer a tabela da fatia (`OutOfMemory`, nada muda,
    // inclusive o core). `addr` nulo, `kind` None ou `core` nulo => Unknown (erro de uso).
    [[nodiscard]] HandleStatus insert(const void* addr, HandleKind kind, void* owner,
                                      PinCore* core) noexcept;

    // Busca e toma um pin. Ok: `owner` e `core` valem e o chamador TEM de chamar core->unpin().
    // Closing: o handle está sendo fechado; sem pin, então `owner` e `core` vêm NULOS (o fechador
    // pode liberar o corpo a qualquer instante: não há o que desreferenciar). Unknown / WrongKind /
    // PinOverflow: idem. NÃO lê `addr` e NÃO aloca.
    [[nodiscard]] HandleLookup acquire(const void* addr, HandleKind kind) noexcept;

    // Troca VIVO por FECHANDO (a troca é a posse do fechamento): Ok com `owner`/`core`; quem perde
    // a corrida, ou chama de novo, recebe Closing com `owner` e `core` NULOS (o corpo é de quem
    // venceu, que pode liberá-lo a qualquer instante). Depois de Ok nenhum pin novo é dado; o
    // chamador espera `core->wait_unpinned()` SEM mutex de fatia (este método já o soltou). Não
    // aloca.
    [[nodiscard]] HandleLookup begin_close(const void* addr, HandleKind kind) noexcept;

    // Desfaz um begin_close (FECHANDO -> VIVO), para o sw_shutdown que encontra um handle aberto
    // depois de zerar os pins (F1-60 §1.8: nada muda). Não aloca.
    [[nodiscard]] HandleStatus cancel_close(const void* addr, HandleKind kind) noexcept;

    // Tira do registro uma entrada FECHANDO (depois de o fechamento terminar). A tabela da fatia é
    // devolvida ao sistema quando fica vazia. Não aloca.
    [[nodiscard]] HandleStatus remove(const void* addr, HandleKind kind) noexcept;

    // Diagnóstico (testes): fatia do endereço; entradas no total; bytes de tabela alocados.
    [[nodiscard]] static std::size_t shard_of(const void* addr) noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t table_bytes() const noexcept;

private:
    struct Entry
    {
        std::uintptr_t addr = 0; // 0 = livre, 1 = apagada (lápide)
        void* owner = nullptr;
        PinCore* core = nullptr;
        HandleKind kind = HandleKind::None;
        std::uint32_t closing = 0; // 0 VIVO, 1 FECHANDO
    };

    struct alignas(64) Shard
    {
        mutable std::mutex m;
        Entry* slots = nullptr;
        std::size_t cap = 0;   // potência de 2, ou 0 sem tabela
        std::size_t count = 0; // entradas vivas ou fechando
        std::size_t tombs = 0; // lápides
    };

    [[nodiscard]] static std::uint64_t hash(std::uintptr_t a) noexcept;
    // Índice da entrada de `addr` na fatia, ou cap se não está. Sob o mutex.
    [[nodiscard]] static std::size_t find_locked(const Shard& s, std::uintptr_t addr) noexcept;
    [[nodiscard]] static bool grow_locked(Shard& s) noexcept;

    Shard shards_[kShards];
};

// Quarentena de cascas, de tamanho fixo, sem alocar (o armazenamento é inline). NÃO é thread-safe:
// quem a guarda (o contexto) a protege.
inline constexpr std::size_t kFileQuarantine = 256; // cascas de sw_file/sw_dir por contexto
inline constexpr std::size_t kCtxQuarantine = 64;   // cascas de sw_ctx do processo

struct QuarantinedShell
{
    void* ptr = nullptr; // nulo: nada
    std::size_t size = 0;
};

template <std::size_t N>
class ShellQuarantine
{
public:
    static_assert(N > 0);
    constexpr ShellQuarantine() noexcept = default;

    // Retém `ptr`. Se a quarentena estava cheia, devolve a casca MAIS ANTIGA, que sai e o chamador
    // libera; senão {nullptr, 0}. Não aloca.
    [[nodiscard]] QuarantinedShell push(void* ptr, std::size_t size) noexcept
    {
        QuarantinedShell out{};
        if (count_ == N) {
            out = items_[head_];
            items_[head_] = QuarantinedShell{ptr, size};
            head_ = (head_ + 1) % N;
            return out;
        }
        items_[(head_ + count_) % N] = QuarantinedShell{ptr, size};
        ++count_;
        return out;
    }

    // Tira tudo, da mais antiga para a mais nova, entregando cada uma a
    // `release(QuarantinedShell)`.
    template <class F>
    void drain(F&& release) noexcept
    {
        while (count_ > 0) {
            const QuarantinedShell s = items_[head_];
            items_[head_] = QuarantinedShell{};
            head_ = (head_ + 1) % N;
            --count_;
            release(s);
        }
        head_ = 0;
    }

    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    [[nodiscard]] bool contains(const void* p) const noexcept
    {
        for (std::size_t i = 0; i < count_; ++i) {
            if (items_[(head_ + i) % N].ptr == p) return true;
        }
        return false;
    }

private:
    QuarantinedShell items_[N] = {};
    std::size_t head_ = 0;
    std::size_t count_ = 0;
};

// Quarentena das cascas de sw_ctx do PROCESSO (64 últimas), com mutex próprio. As cascas de sw_ctx
// usam o alocador do sistema (são do processo, F1-60 §1.3). Estática, nunca destruída.
class ProcessCtxQuarantine
{
public:
    [[nodiscard]] static ProcessCtxQuarantine& instance() noexcept;

    // Retém a casca; devolve a que saiu (o chamador a libera com o alocador do sistema) ou
    // {nullptr, 0}. Não aloca.
    [[nodiscard]] QuarantinedShell push(void* ptr, std::size_t size) noexcept;
    [[nodiscard]] bool contains(const void* p) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

    ProcessCtxQuarantine() noexcept = default;
    ProcessCtxQuarantine(const ProcessCtxQuarantine&) = delete;
    ProcessCtxQuarantine& operator=(const ProcessCtxQuarantine&) = delete;

private:
    mutable std::mutex m_;
    ShellQuarantine<kCtxQuarantine> q_;
};

} // namespace swarmstream::detail
