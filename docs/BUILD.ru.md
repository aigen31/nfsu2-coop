# Сборка

## Требования

* Windows 10/11 x64 (сам мод — 32-битный: `speed2.exe` это i386).
* MinGW-w64 **i686** (ставится `toolchain\install.ps1`).
* PowerShell 5.1+.
* Для тестов игры — установленная NFSU2.

## Быстро

```powershell
.\toolchain\install.ps1     # компилятор + vendor + python-зависимости, SHA-256 проверяется
.\build.ps1                 # всё
.\build.ps1 -Targets core   # только статическая библиотека (быстрая проверка)
.\build.ps1 -Clean          # снести build\
```

Результат в `build\`:

```
libnfc.a            ядро (сеть, мир, гонки, карьера)
coopd.exe           выделенный хост
coop-bot.exe        безголовый клиент для тестов
coop-probe.exe      анализ speed2.exe
core-tests.exe      юнит-тесты
sync-tests.exe      сессионные тесты в одном процессе
net-tests.exe       тесты через chaos-прокси
NFSU2Coop.asi       плагин (2.2 МБ, статическая линковка libstdc++/libgcc)
```

## Почему `C:\nfsu2-toolchain`

GCC не поддерживает установку в путь с пробелами: при линковке он разворачивает
`%D` без кавычек и падает на `cannot find C:/Games/NFS: No such file...`. Junction
не помогает (GCC резолвит реальный путь), поэтому компилятор живёт в
`C:\nfsu2-toolchain`, а `toolchain\mingw32` — junction для удобства. Путь можно
изменить: `$env:NFSU2_TOOLCHAIN = "D:\mingw32"`.

## Флаги

```
-std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter
-D_WIN32_WINNT=0x0601 -DNOMINMAX -DWIN32_LEAN_AND_MEAN
-static -static-libgcc -static-libstdc++ -Wl,-s
```

Статическая линковка обязательна для `.asi`: иначе нужны `libstdc++-6.dll` и
`libgcc_s_dw2-1.dll` рядом с игрой, а это лишние файлы в чужой установке.

## CMake / MSVC

`CMakeLists.txt` описывает те же цели для тех, кто хочет MSVC. Проверенный путь —
`build.ps1` с MinGW; CMake-файл держится в актуальном состоянии, но в этом окружении
MSVC нет, поэтому он не прогонялся. С MSVC:

```powershell
cmake -B build-msvc -A Win32
cmake --build build-msvc --config Release
```

## Если не собирается

| Симптом | Причина / решение |
|---|---|
| `cannot find C:/Games/NFS...` | компилятор в пути с пробелами → `C:\nfsu2-toolchain` |
| `undefined reference to timeGetTime` | нет `-lwinmm` (в `build.ps1` есть) |
| `MH_Initialize failed` в логе | другой мод уже инициализировал MinHook; мод это терпит |
| `cannot bind udp/27970` | порт занят: смените `Port` в ini |
| `.asi` не грузится | нет ASI-загрузчика (`dinput8.dll`) или файл лежит не в `scripts\` |

