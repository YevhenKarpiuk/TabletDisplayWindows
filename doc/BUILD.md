# Сборка TabletDisplayWindows

## Зафиксированное окружение

- Windows 11 x64.
- Visual Studio 2022: MSVC v143, Windows Driver Kit, Spectre libraries (x64).
- Windows SDK/WDK `10.0.26100.6584`, целевая SDK `10.0.26100.0`.
- ADB: `C:\tools\android\sdk\platform-tools\adb.exe`.
- Portable FFmpeg 9.0.1: `C:\tools\ffmpeg`; только для проверок, не runtime-зависимость.

## Команды

```cmd
build.cmd Debug
build.cmd Release
test.cmd
sign-driver.cmd Release
install-driver.cmd Release
check-driver.cmd
uninstall-driver.cmd
connect.cmd [--serial SERIAL]
demo.cmd [--serial SERIAL]
encoder-demo.cmd [--serial SERIAL] [--seconds N]
stream.cmd [--serial SERIAL] [--seconds N]
virtual-stream.cmd [--serial SERIAL] [--seconds N]
```

- `build.cmd` восстанавливает portable WDK/SDK и собирает solution без подписи.
- `test.cmd` запускает parser/golden, H.264 и BGRA→NV12/Annex B tests.
- `sign-driver.cmd` создаёт/переиспользует локальный test certificate, экспортирует публичную часть в `artifacts\cert` и подписывает CAT.
- `install-driver.cmd` повторно собирает и подписывает пакет, запрашивает UAC, доверяет сертификату на этом компьютере и создаёт root-enumerated IddCx-устройство.
- `check-driver.cmd` находит root-enumerated display по hardware ID и показывает состояние PnP-устройства и problem code.
- `uninstall-driver.cmd` запрашивает UAC и удаляет устройство, пакет и созданный test certificate.
- `encoder-demo.cmd` кодирует динамический 1280×800/30 паттерн штатным software H264 MFT и передаёт его Android.
- `stream.cmd` захватывает основной дисплей через DXGI Desktop Duplication, преобразует BGRA→NV12 и передаёт H.264. Нужен интерактивный desktop.
- `virtual-stream.cmd` включает расширенный desktop, получает кадры IddCx через device-interface IOCTL и передаёт виртуальный монитор на Android.

Артефакты: `artifacts\x64\<Configuration>`.

## Test-signing

Сначала запускается обычный `install-driver.cmd Release`. На проверенной Windows 11 test-signed UMDF/IddCx-пакет загрузился с доверенным локальным сертификатом при включённом Secure Boot. Если на другой системе устройство установлено с problem code 52, нужен Windows Test Mode:

1. Сохранить BitLocker recovery key, если шифрование включено.
2. Отключить Secure Boot в UEFI.
3. Запустить `enable-test-signing.cmd` и подтвердить UAC.
4. Перезагрузить Windows и повторить `install-driver.cmd Release`.

`enable-test-signing.cmd` не отключает Secure Boot, BitLocker и не перезагружает компьютер автоматически. Официальная справка: [Loading Test Signed Code](https://learn.microsoft.com/windows-hardware/drivers/install/the-testsigning-boot-configuration-option).

## Текущее состояние

- Driver — IddCx-база Microsoft IddSample; swap-chain копируется в latest-frame BGRA store и выдаётся Host через read-only IOCTL.
- Host 0.1.2 — ADB/session, DXGI/IddCx capture, GPU/CPU NV12 conversion, Media Foundation H.264, protocol sender и tray-настройка автозапуска.
- Software H264 MFT проверен FFmpeg (Constrained Baseline 1280×800) и планшетом: 150/150 кадров, аппаратный MTK decoder.
- Intel Quick Sync MFT обнаружен; hardware output будет активирован с прямым GPU frame path от IddCx.
- Песочница/служба не имеет права `IDXGIOutput1::DuplicateOutput`; `stream.cmd` запускается как обычное пользовательское приложение.
- INF/CAT проверены Release-сборкой; `ROOT\DISPLAY\0000` и дочерний монитор запущены без ошибок, перезагрузка и Test Mode не потребовались.
