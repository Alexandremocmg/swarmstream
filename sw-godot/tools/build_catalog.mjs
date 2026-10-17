import fs from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { SpreadsheetFile, Workbook } from "@oai/artifact-tool";

const assets = [
  ["Floresta", "forest_leaves_04", "Forest Leaves 04", "CC0 1.0", "https://creativecommons.org/publicdomain/zero/1.0/", "https://polyhaven.com/a/forest_leaves_04", "8K texture maps; downloaded by tools/download_assets.py"],
  ["Floresta", "tree_bark_03", "Tree Bark 03", "CC0 1.0", "https://creativecommons.org/publicdomain/zero/1.0/", "https://polyhaven.com/a/tree_bark_03", "8K texture maps; downloaded by tools/download_assets.py"],
  ["Floresta", "grass_ground", "Grass Ground", "CC0 1.0", "https://creativecommons.org/publicdomain/zero/1.0/", "https://polyhaven.com/a/grass_ground", "8K texture maps; downloaded by tools/download_assets.py"],
  ["Floresta", "mossy_brick", "Mossy Brick", "CC0 1.0", "https://creativecommons.org/publicdomain/zero/1.0/", "https://polyhaven.com/a/mossy_brick", "8K texture maps; downloaded by tools/download_assets.py"],
  ["Pedreira", "rock_path", "Rock Path", "CC0 1.0", "https://creativecommons.org/publicdomain/zero/1.0/", "https://polyhaven.com/a/rock_path", "8K texture maps; downloaded by tools/download_assets.py"],
  ["Pedreira", "rock_01", "Rock 01", "CC0 1.0", "https://creativecommons.org/publicdomain/zero/1.0/", "https://polyhaven.com/a/rock_01", "8K texture maps; downloaded by tools/download_assets.py"],
  ["Pedreira", "cobblestone_03", "Cobblestone 03", "CC0 1.0", "https://creativecommons.org/publicdomain/zero/1.0/", "https://polyhaven.com/a/cobblestone_03", "8K texture maps; downloaded by tools/download_assets.py"],
  ["Pedreira", "gravel_road", "Gravel Road", "CC0 1.0", "https://creativecommons.org/publicdomain/zero/1.0/", "https://polyhaven.com/a/gravel_road", "8K texture maps; downloaded by tools/download_assets.py"],
  ["Costa", "aerial_beach_01", "Aerial Beach 01", "CC0 1.0", "https://creativecommons.org/publicdomain/zero/1.0/", "https://polyhaven.com/a/aerial_beach_01", "8K texture maps; downloaded by tools/download_assets.py"],
  ["Costa", "coast_sand_01", "Coast Sand 01", "CC0 1.0", "https://creativecommons.org/publicdomain/zero/1.0/", "https://polyhaven.com/a/coast_sand_01", "8K texture maps; downloaded by tools/download_assets.py"],
  ["Costa", "aerial_rocks_01", "Aerial Rocks 01", "CC0 1.0", "https://creativecommons.org/publicdomain/zero/1.0/", "https://polyhaven.com/a/aerial_rocks_01", "8K texture maps; downloaded by tools/download_assets.py"],
  ["Costa", "rock_wall_08", "Rock Wall 08", "CC0 1.0", "https://creativecommons.org/publicdomain/zero/1.0/", "https://polyhaven.com/a/rock_wall_08", "8K texture maps; downloaded by tools/download_assets.py"],
  ["Distrito", "aerial_asphalt_01", "Aerial Asphalt 01", "CC0 1.0", "https://creativecommons.org/publicdomain/zero/1.0/", "https://polyhaven.com/a/aerial_asphalt_01", "8K texture maps; downloaded by tools/download_assets.py"],
  ["Distrito", "asphalt_01", "Asphalt 01", "CC0 1.0", "https://creativecommons.org/publicdomain/zero/1.0/", "https://polyhaven.com/a/asphalt_01", "8K texture maps; downloaded by tools/download_assets.py"],
  ["Distrito", "brick_floor", "Brick Floor", "CC0 1.0", "https://creativecommons.org/publicdomain/zero/1.0/", "https://polyhaven.com/a/brick_floor", "8K texture maps; downloaded by tools/download_assets.py"],
  ["Distrito", "concrete_moss", "Concrete Moss", "CC0 1.0", "https://creativecommons.org/publicdomain/zero/1.0/", "https://polyhaven.com/a/concrete_moss", "8K texture maps; downloaded by tools/download_assets.py"],
];
const headers = ["Região", "ID Poly Haven", "Ativo", "Licença", "Texto da licença", "Página do ativo", "Formato / observações"];
const wb = Workbook.create();
const sheet = wb.worksheets.add("Assets CC0");
sheet.showGridLines = false;
sheet.getRange("A1:G1").merge();
sheet.getRange("A1").values = [["Swarmstream — Inventário de assets CC0"]];
sheet.getRange("A1:G1").format.font = { bold: true, size: 15, color: "#17365D" };
sheet.getRange("A2:G2").merge();
sheet.getRange("A2").values = [["Poly Haven publica estes ativos sob CC0; 4 regiões, 16 conjuntos de textura. O script baixa e confere hashes MD5 da API."]];
sheet.getRange("A2:G2").format.font = { italic: true, size: 10, color: "#595959" };
sheet.getRange("A4:G4").values = [headers];
sheet.getRange(`A5:G${assets.length + 4}`).values = assets;
sheet.getRange("A4:G4").format.fill = "#1F4E78";
sheet.getRange("A4:G4").format.font = { bold: true, color: "#FFFFFF" };
sheet.getRange(`A4:G${assets.length + 4}`).format.verticalAlignment = "center";
sheet.getRange(`A4:G${assets.length + 4}`).format.wrapText = false;
[16, 24, 24, 14, 44, 46, 52].forEach((width, i) => { sheet.getRangeByIndexes(0, i, assets.length + 4, 1).format.columnWidth = width; });
sheet.freezePanes.freezeRows(4);
wb.recalculate();
const inspection = await wb.inspect({kind: "table", range: `Assets CC0!A4:G${assets.length + 4}`, include: "values", tableMaxRows: 22, tableMaxCols: 8});
console.log(inspection.ndjson);
const output = await SpreadsheetFile.exportXlsx(wb);
const outputPath = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../assets.xlsx");
await output.save(outputPath);
console.log(`Saved ${outputPath}`);
const preview = await wb.render({sheetName: "Assets CC0", range: "A1:G20", scale: 1, format: "png"});
const previewBytes = new Uint8Array(await preview.arrayBuffer());
await fs.writeFile(path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../assets-preview.png"), previewBytes);
