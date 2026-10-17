# Glossário

| Termo | Significado no projeto |
| --- | --- |
| **Bloco** | Unidade de compressão e leitura (64–256 KB). Descomprimível sozinho. |
| **Peça** | Unidade de troca entre peers e de download da CDN (~1 MB). Agrupa blocos. |
| **Folha Merkle** | Parte de 16 KB de uma peça, verificável sozinha. |
| **Árvore de Merkle** | Estrutura de hashes que permite validar partes de uma peça sem ter a peça inteira. |
| **Manifesto** | Lista assinada de todos os arquivos, blocos, peças e hashes de uma versão do jogo. |
| **Endereçamento por conteúdo** | Identificar um dado pelo hash dele, não pelo nome ou versão. |
| **FastCDC** | Algoritmo de corte de blocos definido pelo conteúdo; mudanças pequenas mudam poucos blocos. |
| **Core** | Grupo de conteúdo sempre instalado: lógica, colisão, UI, mips/LODs baixos. |
| **Grupo de conteúdo** | Conjunto de peças baixado junto (região do mapa, capítulo, idioma). |
| **Modo compacto** | Instalação com core + cache limitado; o resto vem sob demanda. |
| **Modo completo** | Jogo inteiro no disco; joga offline e serve outros como super-seeder. |
| **Super-seeder** | Jogador em modo completo que serve peças para os outros. |
| **Seed infinito / web seed** | A CDN vista pelo protocolo P2P como uma fonte que sempre tem tudo. |
| **Falta (cache miss)** | O jogo pediu um dado que não está no cache local. |
| **Política de falta** | O que o jogo faz numa falta (LOD menor, esperar, tela de loading). |
| **Prazo (deadline)** | Momento em que o jogo vai precisar de uma peça; o agendador usa para escolher a fonte. |
| **Hedging** | Pedir a mesma peça à CDN quando o peer provavelmente vai atrasar; vale o que chegar primeiro. |
| **Prefetch** | Baixar antes do jogo pedir, a partir de dicas, traces e movimento. |
| **Dica (hint)** | Aviso do jogo ao núcleo sobre o que vai precisar em breve. |
| **Trace** | Registro de quais peças foram lidas, onde e quando, gravado em sessões de teste. |
| **Lease** | "Reserva" de uma peça enquanto alguém lê; impede que ela seja removida. |
| **Slot** | Espaço fixo em arquivo pré-alocado onde uma peça é guardada. |
| **Journal** | Registro só de append das mudanças no índice do cache, usado para recuperar após queda. |
| **CLOCK / W-TinyLFU** | Algoritmos de remoção de cache mais resistentes que LRU puro. |
| **Marca d'água** | Limites de uso (95%/85%) que disparam e param a remoção. |
| **NAT / CGNAT** | Tradução de endereço do roteador / da operadora; dificulta conexão direta entre jogadores. |
| **STUN** | Serviço que diz ao cliente qual é o seu endereço público. |
| **TURN** | Relay que repassa dados quando não há conexão direta (não usamos para assets). |
| **Hole punching** | Técnica para dois clientes atrás de NAT se conectarem diretamente. |
| **QoS** | Controle para o upload P2P não prejudicar a conexão do jogador. |
| **Cold-start** | Período de lançamento em que ainda há poucos peers com o conteúdo. |
| **Portão (gate)** | Ponto de decisão ao fim de uma fase: seguir, ajustar ou parar. |
| **Trilha (T1–T8)** | Área de trabalho do projeto; ver [09-roadmap-fases.md](09-roadmap-fases.md#trilhas-de-trabalho). |
| **Bancada (`sw-netlab`)** | Ambiente de teste com rede simulada, peers sintéticos e jogador robô. |
