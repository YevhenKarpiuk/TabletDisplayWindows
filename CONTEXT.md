# TabletDisplayWindows

Windows-часть системы, превращающей Android-планшет в дополнительный монитор Windows 11 по USB.

- Парный проект: `C:\Users\ka\Documents\kotlin\TabletDisplayAndroid`.
- Назначение: создать виртуальный дисплей, получить его кадры, закодировать их в H.264 и передать Android-приложению; принять touch и преобразовать его во ввод Windows.
- MVP: Windows 11 x64, один дисплей, 1280×800 и 1920×1200, 30/60 Гц, H.264, одно USB-подключение через ADB.
- Стек: C++20, UMDF 2, IddCx, D3D11, Media Foundation, Windows SDK/WDK, MSBuild, VS Code, ADB.
- WDK: в VS 2022 установлены Windows Driver Kit component и MSVC Spectre x64 libraries; portable NuGet WDK/SDK 10.0.26100.6584 находится в `C:\tools\wdk\10.0.26100.6584`. Microsoft IddSample `Debug|x64` успешно собран через x64 MSBuild; INF/CAT проверены, тестовая подпись ещё не настроена.
- Компоненты: `TabletDisplayDriver.dll` (виртуальный монитор и swap chain) и `TabletDisplayHost.exe` (кодирование, транспорт, управление и ввод).
- Транспорт MVP: Android слушает TCP `27183`; Windows выполняет `adb forward tcp:27183 tcp:27183` и подключается к `127.0.0.1:27183`.
- Контракт с Android: версионированный бинарный протокол; эталонная спецификация должна храниться в `doc/PROTOCOL.md` и синхронно отражаться в обоих проектах.
- Главный риск: надёжная и быстрая передача кадров из UMDF/IddCx в отдельный host-процесс; сначала нужен изолированный прототип IPC.
- План: [doc/IMPLEMENTATION_PLAN.md](doc/IMPLEMENTATION_PLAN.md).

Статус: каркас проверен в Debug/Release x64: IddCx-драйвер на базе Microsoft IddSample, C++20 host и portable-WDK build scripts собираются без ошибок. Потоковое видео, IPC и ввод ещё не реализованы.
