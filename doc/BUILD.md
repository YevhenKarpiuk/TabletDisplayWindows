# Сборка TabletDisplayWindows

## Зафиксированное окружение

- Windows 11 x64.
- Visual Studio 2022 с MSVC v143, Windows Driver Kit и Spectre libraries (x64).
- Windows SDK/WDK `10.0.26100.6584` в `C:\tools\wdk\10.0.26100.6584\packages`.
- Целевая SDK: `10.0.26100.0`.

## Команды

Из корня проекта:

```cmd
build.cmd Debug
build.cmd Release
test.cmd
connect.cmd
```

`build.cmd`:

1. распаковывает три portable NuGet-пакета в игнорируемый каталог `packages`;
2. находит установленный x64 MSBuild через `vswhere`;
3. добавляет WDK/SDK tools только в окружение дочернего процесса;
4. собирает `TabletDisplayWindows.sln` для x64 без подписи драйвера.

`test.cmd` собирает Debug x64 и запускает проверку C++ parser по общим golden-векторам из `protocol\golden`.

`connect.cmd [--serial SERIAL]` находит `adb`, требует ровно одно готовое устройство (либо указанный serial), создаёт недостающий port forward и выполняет `HELLO/HELLO_ACK/DISPLAY_INFO/PING/PONG`. Чужой ранее существовавший forward не удаляется; созданный host-процессом удаляется при выходе.

Артефакты находятся в `artifacts\x64\<Configuration>`.

## Ограничения каркаса

- `TabletDisplayDriver` — рабочая IddCx-база из официального Microsoft IddSample, ограниченная одним монитором; production IPC пока не реализован.
- `TabletDisplayHost` выполняет выбор ADB-устройства, port forward, TCP connect и handshake; автоматический reconnect, Media Foundation и IPC появятся на следующих этапах.
- Строгий incremental parser wire-протокола подключён к тестовому transport host.
- INF и CAT создаются, но пакет не подписывается. Для установки нужна отдельно настроенная тестовая подпись и системный режим test-signing.
