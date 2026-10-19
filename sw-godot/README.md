# Mapa de teste Godot

Projeto fixado em Godot 4.7.2-stable (`.godot-version`) para benchmark em quatro regiões, com câmera em modo robô e 16 conjuntos de textura CC0 8K (aprox. 2,74 GiB de fontes compatíveis). O script `tools/download_assets.py` salva arquivos fora do Git, valida cada MD5, confirma o hash de catálogo em `assets.lock.json` e produz `assets.csv`. O inventário `assets.xlsx` lista cada ativo e licença. São usados Diffuse/Normal/Roughness JPEG e Displacement PNG; os EXRs da origem não são importados pelo Godot 4.7.2. Powered by Poly Haven: https://polyhaven.com.

A integração com o Godot segue o [ADR-0016](../docs/adr/0016-ponto-de-intercepcao-godot.md): um `ResourceFormatLoader` da GDExtension, registrado na frente, materializa os recursos a partir de bytes, sem `FileAccess`. A cena do benchmark continua carregando o diffuse com `Image.load_from_file`; o loader só reivindica caminhos `swarm://gate/**`.

## Preparar assets e abrir

```powershell
python tools/download_assets.py --resolution 8k
godot --editor --path project
```

### Texturas GPU e baseline (F0-12)

A cena carrega, por padrão, texturas BC1 pré-comprimidas (`--tex gpu`). Gere-as uma vez com `godot --headless --path project --script res://scripts/bake_gpu_textures.gd` (saem em `project/assets/gpu/`, fora do Git). `--tex jpg` repete o método antigo (decodificação de JPEG 8K na CPU). Método, limites e carga a frio: [docs/medicoes/baseline.md](../docs/medicoes/baseline.md).

Os arquivos de arte ficam fora do Git em `project/assets/regions/`. `assets.csv` detalha cada arquivo e checksum após o download. A rota automatizada inicia com `godot --path project -- --robot --route-seconds 8`; ela percorre waypoints no passo fixo de física, grava coordenadas e tempos simulados em `user://route.log` e sai ao concluir. `--route-seconds` reduz o tempo sem alterar os waypoints.

## GDExtension e portão do ADR-0016 (F1-20)

Código em `extension/`: o loader (`src/swarm_loader.*`), o adaptador do mock do `sw-core` (`src/gate_core.*`, com a semântica do `swarmstream.h` congelado) e os formatos provisórios `.swtex`/`.swmesh` (`src/gate_formats.h`). Resultado e método: [docs/medicoes/f1-20-portao.md](../docs/medicoes/f1-20-portao.md).

O `godot-cpp` fica fora do git. Use o tag `godot-4.5-stable` (o mesmo do spike F0, compatível com o 4.7.2), clonado em qualquer pasta fora do repositório:

```bash
cmake -S sw-godot/extension -B build-godot -G Ninja -DCMAKE_BUILD_TYPE=Debug -DGODOT_CPP_DIR=<godot-cpp>
cmake --build build-godot
python sw-godot/tools/gate_f1_20.py --godot <Godot_v4.7.2-stable_win64_console.exe> --out <pasta>
```

O build grava o DLL e o `swarmstream.gdextension` em `project/addons/swarmstream/` (fora do git): sem compilar, o projeto abre sem a extensão e sem erro. O executor gera o áudio de teste com o `ffmpeg`, empacota textura e malha (`scripts/gate_pack.gd`, papel do `sw-pack`), importa o projeto e roda `scenes/gate_f1_20.tscn`, que sai com 0 se o portão passar. `--headless` roda sem janela (renderizador e áudio falsos). A parte que não depende do Godot (`extension/tests/`) entra no build da raiz e roda no CI.
