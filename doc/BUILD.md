# Сборка TabletDisplayWindows

## Зафиксированное окружение

- Windows 11 x64.
- Visual Studio 2022: MSVC v143, Windows Driver Kit, Spectre libraries (x64).
- Windows SDK/WDK `10.0.26100.6584`: `C:\tools\wdk\10.0.26100.6584\packages`.
- Целевая SDK: `10.0.26100.0`.

## Команды

Из корня проекта:

```cmd
build.cmd Debug
build.cmd Release
test.cmd
connect.cmd [--serial SERIAL]
demo.cmd [--serial SERIAL]
```

- `build.cmd` восстанавливает portable WDK/SDK в игнорируемый `packages`, находит x64 MSBuild и собирает solution без подписи драйвера.
- `test.cmd` собирает Debug x64 и запускает parser/golden tests и проверку встроенного H.264 fixture.
- `connect.cmd` выбирает ADB-устройство, создаёт только недостающий forward и выполняет handshake + PING/PONG.
- `demo.cmd` дополнительно отправляет `VIDEO_CONFIG` и один 1280×800 AVC Baseline IDR с SMPTE-полосами, затем держит heartbeat 6 секунд.

Артефакты: `artifacts\x64\<Configuration>`.

## Текущее состояние

- `TabletDisplayDriver` — собираемая IddCx-база Microsoft IddSample для одного монитора; production IPC ещё не реализован.
- `TabletDisplayHost` — ADB/TCP/session controller и сквозной synthetic H.264 demo; Media Foundation encoder и automatic reconnect ещё не реализованы.
- Synthetic fixture сгенерирован/проверен portable FFmpeg 9.0.1 из `C:\tools\ffmpeg`, но FFmpeg не является runtime-зависимостью.
- INF/CAT создаются без ошибок; для установки драйвера нужна test-signing конфигурация.
