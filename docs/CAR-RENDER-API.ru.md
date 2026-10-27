# Что даёт SDK Unlimiter по машинам (проверено на нашей сборке)

Адреса из `references/NFSU2Unlimiter/InGameFunctions.h`:

| Адрес | Символ | Смысл |
|---|---|---|
| `0x638190` | `CarRenderInfo_ctor(CarRenderInfo, RideInfo, Car)` | строит объект рендера машины из **RideInfo** и машины |
| `0x634800` | `CarRenderInfo_UpdateCarParts(CarRenderInfo)` | пересобирает детали/кузов по RideInfo |
| `0x6399D0` | `RideInfo_SyncVisualPartsWithPhysics(CCM, perf, random)` | синхронизирует визуальные детали с физикой |
| `0x610270` | `RideInfo_Init(RideInfo, type, usage, has_dash, vertex_damage)` | инициализация RideInfo |
| `0x6102F0` | `RideInfo_GetPart(RideInfo, CarSlotID)` | деталь по слоту |
| `0x61BC80` | `FindPartWithLevel(RideInfo, CarSlotID, level)` | подбор детали уровня |
| `0x4A7890` | `CarViewer_GetRideInfo(which_car)` | RideInfo в гараже/просмотре |

Структура RideInfo (из `RideInfo.h`, `CarRenderInfo.h`):
- `RideInfo[0]` — CarTypeID (индекс в таблице `CarTypeInfo`; 13 = CELICA, 4 = MIATA);
- `RideInfo[356 + CARSLOTID_*]` — детали (hood, wheels, spoiler, neon, decal…).

**Что это меняет для подмены модели.** Мы вызывали `FUN_0063A5A0(car, model)` — это и есть
`CarRenderInfo_ctor(this, RideInfo, Car)`: второй аргумент не «таблица модели», а **RideInfo**.
Поэтому подстановка 0x890170/0x890AB0 давала случайные модели: мы передавали не тот объект.
Правильный путь — взять RideInfo нужного типа (или копию RideInfo и `RideInfo[0] = typeId`),
вызвать ctor и затем `CarRenderInfo_UpdateCarParts`.

Открытый вопрос: где лежит RideInfo конкретной машины. Сканы по объекту машины и объекту
рендера указателя на блок, первый dword которого равен CarTypeID, не нашли — вероятно,
RideInfo хранится в мировом списке машин или в данных машины (`carData`), а не в заголовке.
Дальше: найти её через `CarRenderInfo_ctor` (детуром поймать второй аргумент при спавне
машины — он и есть RideInfo) и сохранить как известный адрес.
