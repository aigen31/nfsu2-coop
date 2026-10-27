# Справочные проекты

Изучать устройство NFSU2 и ускорять разработку помогают четыре внешних проекта. Они
клонируются скриптом `tools\fetch-references.ps1` в каталог `..\references` (рядом с
репозиторием, в git не попадает — там чужие исходники на десятки мегабайт).

| Проект | Что даёт |
|---|---|
| [th0mk/nfsu2-web](https://github.com/th0mk/nfsu2-web) | Браузерный порт игры (x86 → C, WebGPU) **с работающим мультиплеером**: свободная езда и Online-режим до 6 игроков, экипажи, комнаты, режим призрака, гонки. Ближайший существующий ответ на «кооп в свободной езде». Читать: `discord-bot/knowledge.md`, `docs/web/PLAYING.md`, `docs/web/DESIGN.md`. |
| [codepdbh/nfsu2-recompiled](https://github.com/codepdbh/nfsu2-recompiled) | Статическая рекомпиляция `SPEED2.EXE` в C (pcrecomp). По этому C можно искать логику создания машины и менеджер трафика вместо ручного реверса. |
| [nlgxzef/NFSU2Unlimiter](https://github.com/nlgxzef/NFSU2Unlimiter) | Моддинг-SDK: ~100 заголовков с реверс-структурами и адресами функций. Именно оттуда имена полей для наших смещений. |

| [RadekBledowski/NFSU2Unlimiter](https://github.com/RadekBledowski/NFSU2Unlimiter) | Форк SDK: те же 109 заголовков, мелкие исправления (`Game.h` — цикл стоковых гонщиков, `GetCarTypeLogoHash`; в `CarRenderInfo.h` +7 строк). |
| [chrystianfarias/FRSFastTest](https://github.com/chrystianfarias/FRSFastTest) | ASI-плагин: **грузит игру сразу в свободную езду** (без интро, «press Enter» и меню), карьера из сейва или dev-SkipFE. Собран под нашу сборку MinGW и установлен в оба экземпляра. В исходнике описан весь путь загрузки: `0x4D9580` (boot-таблица), `0x495A30` (Career > Resume), `0x5404A0` (TheRaceParameters, трек 4000 = город), dev-флаг `[0x864FAC]`. |
| [yugecin/nfsu2-re](https://github.com/yugecin/nfsu2-re) | Ранняя реимплементация + `SPEED2.CT` (таблица Cheat Engine). |

## Что уже даёт SDK (адреса проверены на нашей сборке)

| Адрес | Символ | Смысл |
|---|---|---|
| `0x610110` | `GetCarTypeInfo(int)` | запись типа машины по `CarTypeID` |
| `0x610130` | `GetCarTypeInfoFromHash(uint)` | поиск типа по хешу имени |
| `0x6108A0`, `0x6108E0` | `CarLoader_LoadSkinLayers`, `CarLoader_LoadedSkinLayers` | загрузка скинов машины |
| `0x637C10` | `CarLoader_UnloadSkinLayers` | выгрузка скинов |
| `0x63A1B0` | `CarLoader_RemoveSomethingFromCarMemoryPool` | пул памяти машин |
| `0x637B50` | `CarLoader_UnloadTexturePack` | текстурные паки |
| `0x63B160` | `CarLoader_ServiceLoading` | стриминг |

`CarTypeInfo` начинается с `CarTypeName[32]`, `BaseModelName[32]`, `GeometryFilename[32]`,
`GeometryLZCFilename[32]`, далее производитель, хеш имени (`+0xC0` на нашей сборке),
физика, камера, `Index`, `UsageType` и — для краски — `DefaultBasePaint` /
`DefaultBasePaint2`.

## Таблица типов машин (измерена на нашей сборке)

```
0 PEUGOT   1 FOCUS    2 COROLLA   3 240SX    4 MIATA     5 CIVIC
6 PEUGOT106 7 CORSA   8 HUMMER    9 NAVIGATOR 10 ESCALADE 11 TIBURON
12 SENTRA  13 CELICA  14 IS300    15 SUPRA    16 GOLF     17 A3
18 RSX     19 ECLIPSE 20 TT       21 RX8      22 350Z     23 G35
24 3000GT  25 GTO     26 MUSTANGGT 27 SKYLINE 28 LANCEREVO8 29 RX7
30 IMPREZAWRX 31 TAXI  32 4DR_SEDAN02 33 AMBULANCE 34 TAXI02 35 PANELVAN
36 COUPE   37 FIRETRUCK 38 PARCELVAN 39 PICKUP
```

Проверено в памяти: `car+0x0C` — это `CarTypeInfo*` (первое поле — имя типа);
`car+0x10`/`+0x14` — таблицы рендера (у игровых машин общие `0x890170`/`0x890160`);
`car+0x554` — объект рендера, его строит `FUN_0063A5A0(model, car)` (вызов всегда из
`0x601282`, инициализация машины).

## Куда смотреть по открытым задачам

| Задача | Источник |
|---|---|
| Спавн машины нужной модели | C-код `nfsu2-web` и `nfsu2-recompiled`: функция создания узла машины; `NFSU2Unlimiter/CarLoader.h` |
| Подмена модели существующей машине | `NFSU2Unlimiter/CarRenderInfo.h`, `CarTypeInfo.h` (`GeometryFilename`), вызывающие `FUN_0063A5A0` |
| Краска и тюнинг | `CarTypeInfo.DefaultBasePaint`, `CarCustomizerManager.h`, `ChoosePaintScreen.h` |
| Дизайн синхронизации свободной езды | `nfsu2-web/discord-bot/knowledge.md`, `docs/web/PLAYING.md` |
