# Mapa de teste Godot

Projeto fixado em Godot 4.7.2-stable (`.godot-version`) para benchmark em quatro regiões, com câmera em modo robô e 16 conjuntos de textura CC0 8K (aprox. 2,74 GiB de fontes compatíveis). O script `tools/download_assets.py` salva arquivos fora do Git, valida cada MD5, confirma o hash de catálogo em `assets.lock.json` e produz `assets.csv`. O inventário `assets.xlsx` lista cada ativo e licença. São usados Diffuse/Normal/Roughness JPEG e Displacement PNG; os EXRs da origem não são importados pelo Godot 4.7.2. Powered by Poly Haven: https://polyhaven.com.

A integração `FileAccess` do Godot segue em aberto em `docs/06-integracao-engine-sdk.md`. A cena carrega apenas diffuse com `Image.load_from_file` e fica independente da decisão de integração; alinhar com o sênior antes de integrar GDExtension.

## Preparar assets e abrir

```powershell
python tools/download_assets.py --resolution 8k
godot --editor --path project
```

### Texturas GPU e baseline (F0-12)

A cena carrega, por padrão, texturas BC1 pré-comprimidas (`--tex gpu`). Gere-as uma vez com `godot --headless --path project --script res://scripts/bake_gpu_textures.gd` (saem em `project/assets/gpu/`, fora do Git). `--tex jpg` repete o método antigo (decodificação de JPEG 8K na CPU). Método, limites e carga a frio: [docs/medicoes/baseline.md](../docs/medicoes/baseline.md).

Os arquivos de arte ficam fora do Git em `project/assets/regions/`. `assets.csv` detalha cada arquivo e checksum após o download. A rota automatizada inicia com `godot --path project -- --robot --route-seconds 8`; ela percorre waypoints no passo fixo de física, grava coordenadas e tempos simulados em `user://route.log` e sai ao concluir. `--route-seconds` reduz o tempo sem alterar os waypoints.
