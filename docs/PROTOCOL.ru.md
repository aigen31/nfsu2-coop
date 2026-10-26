# Протокол

Версия: `0.4.0` (`kProtocolVersion = 0x00040000`). Любое изменение формата требует
поднять версию: хост отвергает чужие версии с понятным сообщением, а не молча
разваливается.

## Транспорт

UDP через ENet 1.3.18, два канала:

| Канал | Режим | Что |
|---|---|---|
| 0 `Control` | reliable, ordered | Hello/Welcome, Own, RaceCmd, Progress, Digest, Chat, Kick |
| 1 `State` | unreliable, sequenced | PlayerState, Snapshot |

ENet даёт ретрансмит, дедупликацию, порядок, фрагментацию и RTT. Мы не используем его
фрагментацию: любой наш пакет < 1200 байт, а большие состояния (снапшот) режутся на
чанки по 18 сущностей.

## Заголовок пакета (12 байт)

```c
struct PacketHeader {   // little-endian
  u32 magic;    // 'NFC2' = 0x3243464E
  u16 version;  // 0x0004
  u8  type;     // MsgType
  u8  flags;    // 0
  u16 len;      // длина payload
  u16 crc;      // crc32(payload) & 0xFFFF
};
```

Проверки при приёме: magic, version, `len == остаток датаграммы`, `len <= 1200`, CRC,
`type` в диапазоне. Любая ошибка — пакет отбрасывается и считается в `badPackets`;
игра не получает мусор.

## Сообщения

| # | Имя | Канал | Payload |
|---|---|---|---|
| 1 | `Hello` | C | `u32 protocol, u32 nonce, str name, u32 flags` |
| 2 | `Welcome` | C | `u8 slot, u32 sessionId, u32 seed, u32 hostMs, u16 snapshotHz, u32 features, str host, str session, u32 yourCar` |
| 3 | `Reject` | C | `u8 reason, str text` |
| 4 | `Ping` | C | `u32 nonce, u32 clientMs` |
| 5 | `Pong` | C | `u32 nonce, u32 clientMs, u32 hostMs` |
| 6 | `Ownership` | C | `u8 kind, u32 id, u32 epoch, u8 owner, u8 prev` (резерв, см. SYNC-RULES) |
| 7 | `PlayerState` | S | `u8 slot, u32 seq, u32 carId, u32 zone, u16 cp, u16 lap, u32 timeMs, u8 finished, CarState` |
| 8 | `Snapshot` | S | `u32 tick, u32 hostMs, u8 count, count × EntityView` |
| 9 | `EntityRemove` | C | `u32 id` |
| 10–11 | `TrafficSpawn/Despawn` | — | резерв (сейчас спавн неявный, в снапшоте) |
| 12 | `RaceCmd` | C | `RaceState` целиком |
| 13 | `RaceReady` | C | `u16 eventId, u8 join, u8 ready` |
| 14 | `RaceProgress` | C | `u16 cp, u16 lap, u32 timeMs, u8 finished` |
| 15 | `ProgressEvent` | C | `u32 key` |
| 16 | `ProgressSnapshot` | C | `ProgressLedger` |
| 17 | `Digest` | C | `u32 hostMs, u32 tick, u32 progressRevision, u32 raceVersion, u64 entities, u64 race, u64 progress` |
| 18 | `RepairRequest` | C | `u8 category` (0 всё, 1 карьера, 2 гонка, 3 снапшот) |
| 19 | `RepairData` | C | `u8 category + данные` |
| 20 | `Zone` | C | `u32 zone` (резерв) |
| 21 | `PlayerList` | C | `u8 count, count × PlayerInfo` |
| 22 | `Chat` | C | `u8 slot, str text` |
| 23 | `HostInfo` | C | keep-alive хоста (резерв) |

`CarState` — 29 байт: позиция 3×i32 (1/64 м), скорость 3×i16 (1/256 м/с),
yaw u16 (1/65536 оборота), pitch/roll i16 (1e-4 рад), руль i8, передача u16,
модель u8, флаги u8. Квантование одинаковое на всех машинах, поэтому дайджесты
сравнимы.

`EntityView` — 38 байт: `id u32, kind u8, model u8, owner u8, epoch u32, CarState`.

`RaceState` — фиксированный: фаза, eventId, routeId, seed, число чекпоинтов,
времена, версия, 8 × (`flags, cp, lap, timeMs, finishMs, place`).

`ProgressLedger` — `revision u32, cash u32, rep u32, u16 count, count × u32 key`.

## Рукопожатие

```
client                         host
  | -- Hello(protocol, name) --> |
  | <-- Welcome(slot, carId) --- |   + RaceCmd, ProgressSnapshot, Snapshot, PlayerList
  | -- PlayerState (20 Гц) ----> |
  | <-- Snapshot / RaceCmd ----- |
  | -- Digest (2 с) -----------> |   (клиент сравнивает свой и хостовый)
  | <-- Digest ----------------- |
  | -- Ping/Pong --------------> |   (смещение часов хоста для отсчёта)
```

Слоты 1..7 выдаются по порядку; слот 0 — хост. Полная сессия → `Reject(2)`.

## Часы

У клиента есть `offsetMs = hostMs + rtt/2 - localNow`, обновляется по `Pong` с
наименьшим RTT. Всё, что привязано к времени (старт отсчёта, время гонки), считается
в часах хоста; так у всех один и тот же `startAt`.

## Почему нет фрагментации

Снапшот мира может быть большим, но каждому клиенту уходит только то, что в радиусе
350 м и не больше 18 сущностей на пакет. 18 × 38 + 9 = 693 байта payload, комфортно
для MTU. Большие состояния (карьера) редко превышают 1 КБ; при 1000 событиях —
около 4 КБ, и тогда `ProgressSnapshot` придётся фрагментировать: это отмечено в
`docs/KNOWN-ISSUES.ru.md`.
