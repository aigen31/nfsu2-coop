# Установка и настройка

## 1. Файлы

```powershell
.\install.ps1 -GameDir "C:\Games\NFS U2 coop mod\game"
```

Скрипт копирует в `<game>\scripts\`:

* `NFSU2Coop.asi`
* `nfsu2coop.ini` (не перезаписывает существующий без `-Force`)

Нужен ASI-загрузчик. В этой установке уже есть `dinput8.dll` (Ultimate ASI Loader из
Widescreen Fix). Если его нет — положите `dinput8.dll` из
[Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases).

Удаление: удалить `NFSU2Coop.asi`, `nfsu2coop.ini`, `NFSU2Coop.log` из `scripts\`.
Сохранения и файлы игры мод не изменяет.

## 2. Конфиг `scripts\nfsu2coop.ini`

| Секция | Ключ | По умолчанию | Что |
|---|---|---|---|
| `[Coop]` | `Host` | 0 | 1 — этот ПК хостит, 0 — подключается |
| | `Server` | 127.0.0.1 | IP хоста или релея |
| | `Port` | 27970 | UDP-порт |
| | `Name` | Player | имя в списке игроков |
| | `Session` | career | имя сессии |
| | `EventId` | 1 | событие для F6/F7 |
| `[Game]` | `PublishTraffic` | 1 | хост рассылает трафик и AI-машины |
| | `SnapshotHz` | 20 | частота снапшотов |
| | `CountdownMs` | 5000 | отсчёт перед гонкой |
| | `MaxTraffic` / `MaxBots` | 20 / 6 | лимиты зеркалирования |
| `[Overlay]` | `Enabled` | 1 | HUD |
| | `VerboseLog` | 0 | debug-лог |
| `[Bindings]` | см. `GAME-BINDINGS.ru.md` | все 0 | адреса под вашу сборку |

## 3. Сеть

* Хост открывает **UDP 27970** (или свой `Port`) в брандмауэре Windows:
  `New-NetFirewallRule -DisplayName "NFSU2 Co-op" -Direction Inbound -Protocol UDP -LocalPort 27970 -Action Allow`.
* Прямой IP работает в одной локальной сети и через проброс порта. Для интернета без
  проброса нужен релей (Steam Spacewar / VPS) — см. `KNOWN-ISSUES.ru.md`.
* NAT с «симметричным» поведением может не пропустить P2P: используйте релей.

## 4. Горячие клавиши

| Клавиша | Хост | Гость |
|---|---|---|
| F5 | принудительно начать отсчёт | готовность в лобби |
| F6 | открыть событие `EventId` | — |
| F7 | — | сообщить «событие пройдено» |
| F8 | HUD вкл/выкл | HUD вкл/выкл |
| F9 | разослать состояние | запросить состояние |

Клавиши читаются глобально (`GetAsyncKeyState`), поэтому работают и в меню, и в гонке.
Если конфликтуют с другими модами — измените в `src/plugin/main.cpp` (пока без конфига).

## 5. Проверка, что всё поднялось

`scripts\NFSU2Coop.log`:

```
NFSU2Coop 0.4.0 starting (module ...)
game: C:\...\speed2.exe base=... imageBase=0x400000
game: 10 anchors resolved
overlay: EndScene hook installed
link: listening on udp/27970 (max 8 peers)      # хост
client: welcome slot=1 car=... session=...      # гость
overlay: ImGui ready
```

## 6. Совместимость

* **Widescreen Fix** — совместим (наш хук `EndScene` ставится поверх, MinHook
  выстраивает цепочку).
* **Crashing Foreign Cars** — совместим: он хукает `CarRenderInfo::Render` и другие
  функции, мод их не трогает. Повреждения не синхронизируются.
* Другие ASI-моды, хукающие `EndScene`, обычно совместимы; при падении отключите HUD
  (`Enabled=0`) и проверьте снова.
* Выделенный сервер: `coopd.exe --port 27970 --name career` (без игры).

## 7. Версия игры

Мод рассчитан на `speed2.exe` 4 800 512 байт,
SHA-256 `bc34932b36bb19194000690ec26edf964176856c31fb0978e01e61ee814eee9d`
(PC 1.2, та же, под которую документированы адреса). На другой сборке проверьте
`coop-probe`: если якоря не сходятся, мод всё равно запустится в протокольном режиме,
но привязки надо снимать заново.
