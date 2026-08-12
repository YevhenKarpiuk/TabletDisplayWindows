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
```

`build.cmd`:

1. распаковывает три portable NuGet-пакета в игнорируемый каталог `packages`;
2. находит установленный x64 MSBuild через `vswhere`;
3. добавляет WDK/SDK tools только в окружение дочернего процесса;
4. собирает `TabletDisplayWindows.sln` для x64 без подписи драйвера.

Артефакты находятся в `artifacts\x64\<Configuration>`.

## Ограничения каркаса

- `TabletDisplayDriver` — рабочая IddCx-база из официального Microsoft IddSample, ограниченная одним монитором; production IPC пока не реализован.
- `TabletDisplayHost` проверяет запуск и зафиксированные параметры подключения; ADB, Media Foundation и IPC появятся на следующих этапах.
- INF и CAT создаются, но пакет не подписывается. Для установки нужна отдельно настроенная тестовая подпись и системный режим test-signing.
