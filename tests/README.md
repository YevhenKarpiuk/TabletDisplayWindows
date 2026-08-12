# Tests

`TabletDisplayProtocolTests` проверяет все `protocol/golden` vectors, каждую возможную границу двух TCP chunks, побитовое decode→encode, последовательности обоих направлений и безопасный отказ для malformed header.

Запуск из корня:

```cmd
test.cmd
```
