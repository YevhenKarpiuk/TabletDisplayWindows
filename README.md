# TabletDisplayWindows

Windows-часть TabletDisplay: превращает Android-планшет в дополнительный
монитор Windows 11 через USB/ADB. Парное приложение:
[TabletDisplayAndroid](https://github.com/YevhenKarpiuk/TabletDisplayAndroid).

## Возможности

- Виртуальный монитор через драйвер UMDF 2 / IddCx; создание после подключения планшета и удаление после отключения.
- Передача виртуального монитора или основного физического экрана.
- H.264 через Media Foundation, GPU-путь D3D11 с shared texture и Intel Quick Sync; CPU fallback.
- Альбомные и портретные режимы: 1280×800, 1920×1200, 800×1280, 1200×1920; 30/60 Гц.
- Касания планшета преобразуются в движение мыши, нажатие и перетаскивание.
- Tray-приложение, автозапуск текущего пользователя и восстановление подключения.

Схема: виртуальный дисплей → D3D11/захват → H.264 → TCP через ADB → Android.
Касания возвращаются по тому же TCP-сеансу.

## Готовые пакеты

В [prebuilt/](prebuilt/) сохранены существующие пакеты версии 0.1.5:

- [Драйвер](prebuilt/TabletDisplayDriver-0.1.5.zip): установка через `Install.cmd`, удаление через `Uninstall.cmd`.
- [Portable Host](prebuilt/TabletDisplayHostPortable-0.1.5.zip): tray-приложение `TabletDisplay.exe`, host и ADB.

Установите Android-приложение из парного репозитория, включите USB debugging,
подключите планшет кабелем и подтвердите доступ ADB. Установите драйвер
с правами администратора и запустите portable Host в обычном пользовательском сеансе.
Используется тестовая подпись драйвера; особенности установки описаны в
[doc/BUILD.md](doc/BUILD.md).

## Сборка и команды

Окружение: Windows 11 x64, Visual Studio 2022/MSVC v143, Spectre libraries,
Windows SDK/WDK 10.0.26100.6584, Android platform-tools.
Скрипты содержат пути к локальному toolchain в `C:\tools`; адаптируйте их
к своему окружению. `build.cmd` восстанавливает portable WDK/SDK.

```cmd
build.cmd Release
test.cmd
install-driver.cmd Release
check-driver.cmd
virtual-stream.cmd --seconds 10
```

`stream.cmd` передаёт физический экран; `virtual-stream.cmd` — виртуальный
монитор. Для обеих команд нужен интерактивный пользовательский сеанс Windows.
`demo.cmd` и `encoder-demo.cmd` передают тестовые изображения.
`release.cmd` формирует пакеты поставки, `uninstall-driver.cmd` удаляет драйвер.

## Структура

| Каталог | Назначение |
|---|---|
| `driver/` | Драйвер виртуального дисплея |
| `host/` | Захват, кодирование, ADB/TCP и ввод мыши |
| `host-tray/` | Tray-интерфейс и управление host |
| `shared/` | Протокол, IPC и обработка видео |
| `protocol/golden/`, `tests/` | Эталонные пакеты и тесты |
| `scripts/`, `packaging/` | Сборка, подпись и установка |

Протокол v1: [doc/PROTOCOL.md](doc/PROTOCOL.md). Документ и golden vectors
согласованы с Android-проектом. Архитектура и этапы:
[doc/IMPLEMENTATION_PLAN.md](doc/IMPLEMENTATION_PLAN.md), [CONTEXT.md](CONTEXT.md).

## Статус

Согласно исходной документации, проверены аппаратный поток 60 Гц,
портретный режим, tap/drag и reconnect на Lenovo TB-X606F. Длительный
soak-тест остаётся следующим этапом. При публикации сборка и проверка
на устройствах повторно не выполнялись; соответствие готовых пакетов
снимку исходников не проверено.

Уведомления о стороннем коде: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
