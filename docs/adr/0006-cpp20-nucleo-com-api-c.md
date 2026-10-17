# ADR-0006: Núcleo em C++20 com API pública em C

**Status:** Aceita (2026-10-17, pelo dono do produto) · **Data:** 2026-10-17

## Contexto
O núcleo precisa ser embutido em engines C++ (Godot, Unreal) e C# (Unity), e usar o libtorrent (C++). Rust seria mais seguro em memória e tem boas bibliotecas QUIC (iroh), mas a integração com libtorrent e com as engines exigiria mais camadas.

## Decisão
- `sw-core` e `sw-pack` em **C++20**, build com **CMake**, dependências via **vcpkg**.
- API pública **em C** (`swarmstream.h`) para ABI estável e bindings fáceis (C#, GDExtension).
- Sanitizers (ASan/UBSan) e fuzzing no CI para compensar o risco de memória.
- Serviços de backend: linguagem decidida em ADR próprio (Go ou Rust).

## Consequências
- (+) Integração direta com libtorrent e engines.
- (−) Exige disciplina de revisão em código de parsing de rede; a equipe precisa de experiência em C++ moderno.
- Reavaliar Rust para o transporte se o libtorrent for substituído.
