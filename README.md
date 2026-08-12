# TabletDisplayWindows

Windows-часть TabletDisplay: IddCx/UMDF-драйвер виртуального дисплея и пользовательский host-процесс.

Доступны IddCx-каркас, ADB/TCP session, synthetic video, проверка настоящего Media Foundation encoder и захват основного Windows-дисплея через DXGI Desktop Duplication:

```cmd
build.cmd
test.cmd
install-driver.cmd Release
check-driver.cmd
demo.cmd
encoder-demo.cmd --seconds 5
stream.cmd --seconds 10
virtual-stream.cmd --seconds 10
```

`stream.cmd` передаёт основной физический дисплей через Desktop Duplication. `virtual-stream.cmd` включает режим «Расширить», получает последний BGRA-кадр виртуального монитора через read-only IOCTL драйвера и передаёт его существующим H.264-конвейером. Обе команды запускайте из обычного интерактивного пользовательского сеанса Windows.

`install-driver.cmd Release` собирает, test-sign-подписывает и устанавливает IddCx-пакет с запросом UAC. Для отката используйте `uninstall-driver.cmd`; детали Secure Boot/Test Mode описаны в [doc/BUILD.md](doc/BUILD.md).

Подробности: [doc/BUILD.md](doc/BUILD.md) и [doc/IMPLEMENTATION_PLAN.md](doc/IMPLEMENTATION_PLAN.md).
