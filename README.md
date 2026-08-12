# TabletDisplayWindows

Windows-часть TabletDisplay: IddCx/UMDF-драйвер виртуального дисплея и пользовательский host-процесс.

Текущий каркас содержит собираемый драйвер на базе официального Microsoft IddSample, консольный `TabletDisplayHost.exe`, локальное восстановление portable WDK и задачи VS Code.

Сборка Debug x64:

```cmd
build.cmd
```

Подробности: [doc/BUILD.md](doc/BUILD.md) и [doc/IMPLEMENTATION_PLAN.md](doc/IMPLEMENTATION_PLAN.md).
