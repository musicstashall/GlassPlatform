# GitHub Wiki: исходники страниц

Готовые страницы для вики репозитория. Вики **опубликована** 2026-11-27
(<https://github.com/musicstashall/GlassPlatform/wiki>, коммит вики `786fa7c`):
16 страниц, 10 блоков Mermaid, `_Sidebar` и `_Footer` подставляются на всех страницах.

Правки вносятся здесь, в `docs/wiki/`, и затем переливаются в вики-репозиторий
командами ниже — сама вики отдельным git-репозиторием, файлы в `master` не попадают.

## Состав

| Файл | Страница вики |
|---|---|
| `Home.md` | главная: сводка, оглавление 01–13, ссылки на интерактив |
| `_Sidebar.md` | навигация слева на всех страницах |
| `_Footer.md` | подпись снизу на всех страницах |
| `01-overview.md` … `13-openglass-textures.md` | разделы документации, 8 из них с Mermaid-диаграммами |

## Как опубликовать

1. Открыть <https://github.com/musicstashall/GlassPlatform/wiki> → **Create the first
   page**, имя `Home`, вставить текст из `Home.md`. Это единственный шаг, который
   нельзя сделать через API.
2. Дальше — обычным git:

```bash
git clone https://github.com/musicstashall/GlassPlatform.wiki.git
cp docs/wiki/*.md GlassPlatform.wiki/
rm GlassPlatform.wiki/README.md   # инструкция для репозитория, страницей вики быть не должна
cd GlassPlatform.wiki
git add . && git commit -m "wiki: разделы 01–13 и диаграммы Mermaid" && git push
```

3. Проверить на GitHub, что все восемь диаграмм отрисовались (`03`, `04`, `05`, `06`,
   `07`, `09`, `10`, `13`, плюс `01`).

Ссылки между страницами — относительные, без `.md`: `[05](05-window-classifier)`.
Альтернатива, которую тоже понимает вики, — `[[05-window-classifier]]`.
Файлы `_Sidebar.md` и `_Footer.md` GitHub подставляет автоматически; префикс `_`
не даёт им стать обычными страницами.

## Что ограничено в GitHub Wiki

| Возможность | Статус |
|---|---|
| Markdown (GFM): таблицы, `<details>`, `<kbd>`, `<sub>` | ✅ |
| Mermaid: `flowchart`, `stateDiagram`, `sequenceDiagram`, `classDiagram`, `erDiagram` | ✅ рендерится нативно |
| SVG/PNG как изображение (`![...](файл.svg)`) | ✅, файл кладётся в вики-репозиторий |
| `<script>`, `<style>`, `iframe`, `onclick`, `style=""`, классы, id | ❌ вырезается санитайзером |
| Своя тема/шрифты/анимация, клики по узлам, фильтры, симуляторы | ❌ |
| Ограничение размера блока Mermaid | ~50 КБ, очень крупные схемы могут не отрисоваться |

## Что перенесено, а что осталось на сайте

| Блок сайта | В вики | Страница |
|---|---|---|
| Архитектура 13 модулей (плоскости A/B/C1/C2) | ✅ 4 `subgraph` вместо тумблера | [01](01-overview) |
| `GlassCompat::DoInitialize` FLOWCHART | ✅, фильтры ветвей → цвет узла | [02](02-init) |
| IAT / PaintDispatcher PIPELINE | ✅ + таблица 4 слотов `IATRegistry` | [03](03-hooks-pipeline) |
| GVT1 / 32bpp DATA FLOW | ✅ один в один | [04](04-gdi-32bpp) |
| `WindowClassifier` CLASSIFICATION | ✅ схема + таблица шагов 0..7 | [05](05-window-classifier) |
| LAYER STACK 3D (Explorer + Taskbar) | ⚠️ вместо изометрии — `flowchart BT` + таблицы | [06](06-win11-xaml) |
| UICompat / AppContainer COREWINDOW | ✅ граница песочницы = `subgraph` | [07](07-appcontainer) |
| `InjectProcessMap` MEMORY MAP | ✅ таблица смещений 176 B | [08](08-ipc) |
| RESOLVER & SPECTRUM (`EffectType 0..8`) | ✅ каскад + таблица ET | [09](09-registry) |
| `ExecuteShutdown` SEQUENCE (S0 + 1–6) | ✅ `sequenceDiagram` + таблица фаз | [10](10-unload) |
| OPENGLASS STACK 3D | ⚠️ `flowchart LR` конвейера полотна | [13](13-openglass-textures) |
| SIMULATOR: `IATRegistry`, Taskbar corners, Texture Lab | ❌ только сайт | — |

Все блоки Mermaid прогнаны через парсер `mermaid@11` (`parse()`): синтаксис валиден.
Итоговая схема публикации — структурные диаграммы в вики, интерактив и симуляторы
только на сайте со ссылкой «интерактивная версия».
