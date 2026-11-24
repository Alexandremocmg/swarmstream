// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Alexandre Aparecido Rodrigues Peres

// Registro de pedidos assíncronos, ids e threads de conclusão da API C (F1-67 parte B; F1-60
// §1.5-§1.7). Interno do sw-core: nada aqui é exportado. A API pública (partes C e D; leitura na
// etapa 2, F1-68) é quem usa; nesta parte o registro é exercitado por pedidos SINTÉTICOS
// (`RequestKind::Synthetic`), que nada leem: o teste faz o papel da fonte e os conclui por id.
//
// POOL FIXO. `capacity` pedidos (max_pending_requests, padrão 4096 no sw_init), criados de uma vez
// com o alocador do contexto (nível 1), junto com a tabela de ids (2 x capacity posições) e o
// próprio objeto. Pool cheio => QueueFull, SEM alocar. Estados: LIVRE -> ENFILEIRADO -> EM_VOO ->
// CONCLUINDO -> LIVRE. Cada pedido guarda o dono (o Context), o arquivo, `dst`, trecho, tipo,
// `cb`/`user` e prazo.
//
// IDS ÚNICOS NO PROCESSO. Um único contador global de 64 bits emite os sw_request_id de TODOS os
// registros (contextos). Avança por compare-and-swap (não fetch_add, que daria a volta): emite
// 1 .. UINT64_MAX-1 e, quando o próximo seria UINT64_MAX, NÃO avança mais. Nunca 0, nunca repete,
// nunca dá a volta. Pedido novo nesse estado => IdsExhausted na hora, `*out_id = 0`, sem callback;
// `first_exhaustion` diz ao chamador para registrar o erro no log UMA vez (no processo). Os pedidos
// já pendentes seguem normalmente. Cada registro acha os seus pedidos numa tabela fixa (sem
// alocar) comparando o id INTEIRO, e `cancel(owner, id)` confere também que o pedido achado é do
// `owner`: duas barreiras independentes (unicidade e dono). O mesmo contador serve ao request_id
// da telemetria (F1-59).
//
// CALLBACKS. Threads de conclusão dedicadas do registro (nunca a dona do cache, nunca a thread que
// criou o pedido; pilhas e estado são do sistema, nível 2). A conclusão passa por uma fila
// intrusiva (o nó é o próprio pedido), sem alocar. Uma marca por thread diz "dentro de callback"
// (`in_callback()`): nesse estado sw_read, sw_close e sw_shutdown devolvem SW_E_BUSY (partes C/D);
// aqui, `close_file` e `stop` devolvem Busy. Cada pedido aceito tem EXATAMENTE UM callback; erro em
// `submit` => nenhum.
//
// CANCELAMENTO E FECHAMENTO. `cancel` e `close_file` levam o pedido pendente (ENFILEIRADO ou
// EM_VOO) direto a CONCLUINDO com SW_E_CANCELLED e o tiram da tabela de ids: uma conclusão
// posterior da "fonte" (`complete(id, ...)`) não o acha mais e é descartada (NotFound). Por isso o
// cancelamento NÃO ESPERA A REDE: um pedido "preso" em EM_VOO não segura o fechamento. (A espera de
// "no máximo uma escrita de disco em curso" do header é da etapa 2, quando houver escrita; aqui não
// há.) `close_file`: a partir da chamada, todo pedido novo no arquivo (inclusive vindo de callback)
// devolve Cancelled NA HORA, sem callback; cancela os pendentes; e só volta depois que todos os
// callbacks do arquivo (os cancelados e os que já estavam concluindo) retornaram. Depois do retorno
// nenhum callback deste arquivo ocorre.
//
// LEASES POR ARQUIVO (`FileLeases`): a lista dos leases que um arquivo segura (de sw_ensure_async,
// etapa 2); os nós vêm do alocador do contexto; `release_all` solta todos (a peça deixa de estar
// presa) e devolve os nós. A lista não é thread-safe: o dono do arquivo a protege.
//
// MSVC Debug (Regra R): nenhum contêiner da STL nos objetos daqui; nada de `unordered_*`. O pool, a
// tabela e o objeto são blocos crus do alocador do contexto; as threads são um vetor inline de
// `std::thread` construídas por padrão (não aloca) e só depois atribuídas.
#pragma once

#include "api_allocator.hpp"
#include "swarmstream.h"
#include "swarmstream/lease.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

namespace swarmstream::detail {

// ---------------------------------------------------------------------------------------- ids

// O próximo id do processo (1 .. UINT64_MAX-1), ou 0 se esgotou. Em esgotamento, `*first` (se não
// for nulo) diz se esta é a primeira vez no processo (para registrar o erro uma vez). Não aloca.
[[nodiscard]] sw_request_id issue_request_id(bool* first = nullptr) noexcept;

// "Dentro de callback" na thread que chama (ver o topo).
[[nodiscard]] bool in_callback() noexcept;

// ----------------------------------------------------------------------------------- pedidos

enum class RequestKind : std::uint8_t { Read, Ensure, Synthetic };
enum class RequestState : std::uint8_t { Free, Queued, InFlight, Completing };

struct Request;

// Os pedidos de um arquivo. Vive no corpo do arquivo; é protegido pelo mutex do registro (não tem
// mutex próprio). Zerado = arquivo sem pedidos, aberto.
struct FileRequests
{
    Request* head = nullptr;   // lista dupla dos pedidos não LIVRES deste arquivo
    std::uint32_t pending = 0; // pedidos não LIVRES (inclui os com callback em curso)
    bool closing = false;      // close_file começou: pedido novo => Cancelled
};

struct Request
{
    std::atomic<RequestState> state{RequestState::Free};
    RequestKind kind = RequestKind::Synthetic;
    sw_request_id id = 0;
    const void* owner = nullptr; // o dono (Context) do registro
    FileRequests* file = nullptr;
    void* dst = nullptr;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::uint32_t deadline_ms = 0;
    sw_read_cb cb = nullptr;
    void* user = nullptr;
    sw_result result = SW_OK; // com CONCLUINDO
    std::uint64_t bytes = 0;  // com CONCLUINDO
    Request* next = nullptr;  // lista livre ou fila de conclusão
    Request* file_prev = nullptr;
    Request* file_next = nullptr;
};

struct SubmitArgs
{
    FileRequests* file = nullptr; // obrigatório
    RequestKind kind = RequestKind::Synthetic;
    void* dst = nullptr;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::uint32_t deadline_ms = 0;
    sw_read_cb cb = nullptr; // obrigatório
    void* user = nullptr;
};

enum class RequestStatus : std::uint8_t {
    Ok = 0,
    QueueFull,       // pool cheio: SW_E_QUEUE_FULL (sem alocar)
    Cancelled,       // o arquivo está fechando: SW_E_CANCELLED na hora, sem callback
    IdsExhausted,    // contador global esgotado: SW_E_INTERNAL, *out_id = 0, sem callback
    ShuttingDown,    // stop() começou: SW_E_SHUTTING_DOWN
    NotFound,        // cancel/complete/mark_in_flight: id desconhecido, de outro dono ou concluído
    Busy,            // close_file/stop de dentro de callback: SW_E_BUSY, nada muda
    InvalidArgument, // erro de uso interno (cb ou file nulos; resultado inválido)
    OutOfMemory,     // create
    Internal,        // create (thread que o sistema não criou) / stop (join que falhou)
};

[[nodiscard]] const char* to_string(RequestStatus status) noexcept;

struct SubmitResult
{
    RequestStatus status = RequestStatus::Ok;
    bool first_exhaustion = false; // só com IdsExhausted: registre o erro no log (uma vez)
};

class RequestRegistry;
namespace testing {
class RequestRegistryTestHook;
} // namespace testing

struct RequestRegistryConfig
{
    std::uint32_t capacity = 4096;        // >= 1
    std::uint32_t completion_threads = 1; // 1 .. kMaxCompletionThreads
    const void* owner = nullptr;          // o Context dono (para a barreira de dono)
};

struct RequestRegistryCreate
{
    RequestStatus status = RequestStatus::Ok; // Ok, OutOfMemory, InvalidArgument, Internal
    AllocStatus alloc = AllocStatus::Ok;      // detalhe do alocador quando OutOfMemory
    RequestRegistry* registry = nullptr;
};

class RequestRegistry
{
public:
    static constexpr std::uint32_t kMaxCompletionThreads = 16;

    // Cria o registro (objeto, pool e tabela pelo alocador do contexto; threads pelo sistema). Em
    // qualquer falha, tudo o que a chamada construiu é desfeito (threads paradas e juntadas,
    // memória devolvida) e `registry` é nulo. `alloc` tem de sobreviver ao registro.
    [[nodiscard]] static RequestRegistryCreate create(const ContextAllocator& alloc,
                                                      const RequestRegistryConfig& config) noexcept;

    // Faz o stop() (se ainda não) e devolve a memória. Normalmente não há pedido pendente (os
    // arquivos fecharam antes); se houver, o stop() os cancela e cada um recebe SW_E_CANCELLED
    // (exatamente um callback) antes de as threads pararem. Se o stop falhar (join), as threads não
    // podem ser destruídas: devolve Internal SEM liberar nada (o chamador registra e vaza, em vez
    // de terminar o processo). De dentro de callback: Busy, nada muda.
    [[nodiscard]] static RequestStatus destroy(RequestRegistry* registry) noexcept;

    RequestRegistry(const RequestRegistry&) = delete;
    RequestRegistry& operator=(const RequestRegistry&) = delete;

    // Cria um pedido. Ok: `*out_id` (se não nulo) recebe o id ANTES de o pedido poder concluir, e o
    // callback virá exatamente uma vez. Qualquer outro status: `*out_id = 0` e nenhum callback.
    // Não aloca.
    [[nodiscard]] SubmitResult submit(const SubmitArgs& args, sw_request_id* out_id) noexcept;

    // A fonte (etapa 2; aqui o teste) pegou o pedido: ENFILEIRADO -> EM_VOO. Não aloca.
    [[nodiscard]] RequestStatus mark_in_flight(sw_request_id id) noexcept;

    // A fonte concluiu o pedido com `result`/`bytes`: vai à fila de conclusão. NotFound se o id não
    // está pendente (cancelado, concluído, desconhecido): o resultado é descartado. Não aloca.
    [[nodiscard]] RequestStatus complete(sw_request_id id, sw_result result,
                                         std::uint64_t bytes) noexcept;

    // sw_cancel: Ok se o pedido de `owner` estava pendente (o callback ainda vem, com
    // SW_E_CANCELLED); NotFound se o id é 0, desconhecido, de outro dono ou já concluído. Não
    // aloca.
    [[nodiscard]] RequestStatus cancel(const void* owner, sw_request_id id) noexcept;

    // Protocolo do sw_close para os pedidos de `file` (ver o topo). Busy de dentro de callback
    // (nada muda). Ok depois que nenhum callback do arquivo está pendente nem em curso. Não aloca.
    [[nodiscard]] RequestStatus close_file(FileRequests& file) noexcept;

    // Pedidos não LIVRES (inclui os com callback em curso).
    [[nodiscard]] std::uint32_t pending() const noexcept;
    [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] const void* owner() const noexcept { return owner_; }

    // S2-S3 do sw_shutdown: recusa pedidos novos (ShuttingDown), cancela os que ainda estiverem
    // pendentes (SW_E_CANCELLED, um callback cada), espera todos os callbacks voltarem e junta as
    // threads de conclusão. Busy de dentro de callback (nada muda); Internal se um join falhar (as
    // threads que não juntaram ficam; destroy recusa liberar). Idempotente.
    [[nodiscard]] RequestStatus stop() noexcept;

    // Só create() constrói (a chave é privada) e só destroy() destrói: o destrutor de um registro
    // com thread juntável terminaria o processo (std::thread), e destroy() garante que não há.
    class Key
    {
        friend class RequestRegistry;
        Key() noexcept = default;
    };
    RequestRegistry(Key, const ContextAllocator& alloc, Request* pool, Request** table,
                    std::uint32_t capacity, const void* owner) noexcept;
    ~RequestRegistry() = default;

private:
    friend class testing::RequestRegistryTestHook;
    void completion_loop() noexcept;
    // Sob o mutex.
    [[nodiscard]] std::size_t slot_of(sw_request_id id) const noexcept;
    [[nodiscard]] Request* find_locked(sw_request_id id) const noexcept;
    void table_insert_locked(Request* r) noexcept;
    void table_erase_locked(sw_request_id id) noexcept;
    void to_completing_locked(Request* r, sw_result result, std::uint64_t bytes) noexcept;

    ContextAllocator alloc_;
    Request* pool_ = nullptr;
    Request** table_ = nullptr; // 2 x capacity posições; nullptr = livre
    std::uint32_t capacity_ = 0;
    std::uint32_t table_size_ = 0;
    const void* owner_ = nullptr;

    mutable std::mutex m_;
    std::condition_variable work_cv_;  // threads de conclusão: fila não vazia ou parada
    std::condition_variable files_cv_; // close_file e stop: um arquivo ou o registro esvaziou
    Request* free_ = nullptr;
    Request* queue_head_ = nullptr;
    Request* queue_tail_ = nullptr;
    std::uint32_t pending_ = 0;
    bool stopping_ = false;
    bool stopped_ = false;

    std::thread threads_[kMaxCompletionThreads];
    std::uint32_t thread_count_ = 0;
};

// Pool do registro: alinhado à linha de cache (F1-60 §7).
inline constexpr std::size_t kRequestPoolAlign = kCacheLineAlign<Request>;
static_assert(alignof(Request) <= kRequestPoolAlign);

// ----------------------------------------------------------------------------------- leases

class FileLeases
{
public:
    FileLeases() noexcept = default;
    FileLeases(const FileLeases&) = delete;
    FileLeases& operator=(const FileLeases&) = delete;
    // PRECONDIÇÃO: release_all() já foi chamado (o dono do arquivo o faz no fechamento).
    ~FileLeases() = default;

    // Guarda o lease (movido para um nó do alocador do contexto). OutOfMemory/Misaligned: o lease
    // FICA com o chamador (não foi movido) e nada muda.
    [[nodiscard]] AllocStatus add(const ContextAllocator& alloc, Lease&& lease) noexcept;
    // Solta todos os leases (a peça deixa de estar presa) e devolve os nós.
    void release_all(const ContextAllocator& alloc) noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return count_; }

private:
    struct Node
    {
        Lease lease;
        Node* next = nullptr;
        explicit Node(Lease&& l) noexcept : lease(std::move(l)) {}
    };
    Node* head_ = nullptr;
    std::size_t count_ = 0;
};

namespace testing {
// Só para os testes de esgotamento (executável próprio, um processo por caso): o último id emitido
// passa a ser `last` (o próximo é last + 1). Não exportado.
void set_last_issued_request_id(std::uint64_t last) noexcept;
} // namespace testing

} // namespace swarmstream::detail
