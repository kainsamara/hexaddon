# HEXADDON → Home Assistant → Алиса

Как завести подсветку и питание аддонов в Home Assistant по MQTT и управлять
ими голосом через Алису. Прошивка устройства должна быть **0.10.0 или новее**
(в веб-интерфейсе: Система → Информация → Версия).

Что получится в итоге:

```
Алиса → Яндекс.Облако → Home Assistant → MQTT-брокер → HEXADDON
```

В Home Assistant появится устройство **HEXADDON** с пятью сущностями:

| Сущность | Тип | Иконка | Что делает |
|---|---|---|---|
| Питание аддонов | Выключатель | `mdi:hexagon-multiple` | питание аддонов (без него подсветка мертва — потому и первый) |
| Подсветка | Свет | (стандартная) | вкл/выкл, яркость, **цветовое колесо**, выбор эффекта (названия из прошивки) |
| Случайный цвет | Выключатель | `mdi:palette-outline` | эффекты сами выбирают цвета |
| Эффект | Select (список) | (стандартная) | выбор эффекта из выпадающего списка прямо на дашборде |
| Предыдущий эффект | Кнопка | `mdi:arrow-left-bold` | предыдущий эффект по кругу |
| Следующий эффект | Кнопка | `mdi:arrow-right-bold` | следующий эффект по кругу |
| Скорость | Number (слайдер) | (стандартная) | скорость эффектов 0..100 |

Иконки задаются прошивкой в discovery и применяются сами (ручная иконка,
поставленная в UI HA, имеет приоритет). Иконки переapply'ятся при каждом
переподключении — устройство для их смены удалять не нужно. Иконки
**устройству целиком** HA не поддерживает вовсе — только сущностям.
Иконки, зависящие от состояния (контурная для «выключено», залитая для
«включено»), через MQTT discovery пока невозможны: `icon_template` для
MQTT-сущностей — давняя фича-запрос, на момент написания существует только
открытый PR (home-assistant/core#160556). Когда его смержат — прошивке
хватит пары строк: «Случайный цвет» получит `mdi:palette-outline` /
`mdi:palette`, «Питание аддонов» — `mdi:hexagon-multiple` /
`mdi:hexagon-multiple-outline`.

Обе сущности двусторонние: изменил в HA / кнопкой на устройстве / по
расписанию — состояние синхронизируется везде в течение секунды.

---

## Шаг 1. MQTT-брокер

Home Assistant не содержит брокера сам — ему нужен Mosquitto (или совместимый).

### Вариант А: Home Assistant OS / Supervised

1. **Settings → Add-ons → Add-on Store** → найдите **Mosquitto broker** → Install → Start.
2. Откройте вкладку **Configuration** аддона и создайте учётные записи (logins):

```yaml
logins:
  - username: ha
    password: <пароль для Home Assistant>
  - username: hexaddon
    password: <пароль для устройства>
```

3. **Settings → Users** (в самом HA): создайте пользователей с теми же
   логинами/паролями — Mosquitto проверяет пароли через HA.
4. Сохраните, перезапустите.

### Вариант Б: HA в Docker (compose)

Пример `docker-compose.yml`:

```yaml
services:
  homeassistant:
    container_name: homeassistant
    image: ghcr.io/home-assistant/home-assistant:stable
    volumes:
      - ./ha:/config
    network_mode: host
    restart: unless-stopped

  mosquitto:
    container_name: mosquitto
    image: eclipse-mosquitto:2
    volumes:
      - ./mosquitto/config:/mosquitto/config
      - ./mosquitto/data:/mosquitto/data
      - ./mosquitto/log:/mosquitto/log
    ports:
      - 1883:1883
    restart: unless-stopped
```

`./mosquitto/config/mosquitto.conf`:

```
listener 1883 0.0.0.0
allow_anonymous false
password_file /mosquitto/config/passwd
persistence true
persistence_location /mosquitto/data/
log_dest file /mosquitto/log/mosquitto.log
```

Файл паролей (две учётки — `ha` для Home Assistant, `hexaddon` для устройства:

```bash
docker run --rm -v $PWD/mosquitto/config:/mosquitto/config eclipse-mosquitto:2 \
  sh -c 'mosquitto_passwd -b -c /mosquitto/config/passwd ha <пароль_HA> && \
         mosquitto_passwd -b /mosquitto/config/passwd hexaddon <пароль_устройства>'
chown -R 1883:1883 mosquitto/
docker compose up -d
```

> TLS не нужен: брокер живёт в домашней сети и наружу не торчит.

## Шаг 2. MQTT-интеграция в Home Assistant

1. **Settings → Devices & Services → Add Integration → MQTT**.
2. Host: адрес машины с брокером (например `192.168.1.60`), порт `1883`.
3. Username `ha` / пароль из шага 1.
4. Интеграция должна появиться без ошибок.

## Шаг 3. HEXADDON

1. Откройте веб-интерфейс устройства → вкладка **Сеть** → блок **Интеграции**.
2. Включите тумблер **MQTT** и заполните:

| Поле | Значение |
|---|---|
| Сервер | IP машины с брокером, например `192.168.1.60` |
| Порт | `1883` |
| Логин | `hexaddon` |
| Пароль | пароль учётки `hexaddon` |

3. Нажмите вне поля (сохранение автоматическое). Строка **Состояние** должна
   стать «подключено» через пару секунд.
4. В Home Assistant: **Settings → Devices & Services → MQTT** → устройство
   **HEXADDON** (производитель Rotten Mechanism). Или просто найдите «Подсветку»
   в общем списке устройств.

## Как это работает под капотом

Имя устройства (Система → «Имя устройства») — это база всех топиков и
идентификаторов. Переименуете устройство — HA увидит его как новое
(старое удалится после перепрошивки/переподключения).

| Назначение | Топик | Payload |
|---|---|---|
| Discovery света | `homeassistant/light/<имя>/light/config` | JSON, retained |
| Discovery питания | `homeassistant/switch/<имя>/addon/config` | JSON, retained |
| Discovery эффекта | `homeassistant/select/<имя>/effect/config` | JSON (options = список эффектов), retained |
| Discovery кнопок | `homeassistant/button/<имя>/effect_next/config` и `.../effect_prev/config` | JSON, retained |
| Discovery скорости | `homeassistant/number/<имя>/speed/config` | JSON (min 0, max 100, mode slider), retained |
| Discovery случ. цвета | `homeassistant/switch/<имя>/color_random/config` | JSON, retained |
| Состояние света | `<имя>/light/state` | `{"state":"ON","brightness":120,"effect":"Огонь","color_mode":"rgb","color":{"r":255,"g":128,"b":0}}` (retained) |
| Команды света | `<имя>/light/set` | `{"state":"ON"}` / `{"brightness":180}` / `{"effect":"Радуга"}` / `{"color":{"r":0,"g":255,"b":60}}` |
| Состояние эффекта | `<имя>/effect/state` | имя эффекта, например `Огонь` (retained) |
| Выбор эффекта | `<имя>/effect/set` | имя эффекта: `Огонь` |
| Кнопки ±эффект | `<имя>/effect/next` / `<имя>/effect/prev` | `next` / `prev` (содержимое игнорируется) |
| Состояние скорости | `<имя>/speed/state` | число `0..100` (retained) |
| Слайдер скорости | `<имя>/speed/set` | число: `60` |
| Состояние случ. цвета | `<имя>/color_random/state` | `ON` / `OFF` (retained) |
| Тумблер случ. цвета | `<имя>/color_random/set` | `ON` / `OFF` |
| Состояние питания | `<имя>/addon/state` | `ON` / `OFF` (retained) |
| Команды питания | `<имя>/addon/set` | `ON` / `OFF` |
| Доступность | `<имя>/avail` | `online` / `offline` (retained, LWT) |

Полезные детали:

- Если устройство умерло/пропала сеть — HA пометит обе сущности
  «недоступно» (Last Will), без ложного состояния.
- Изменения, сделанные кнопкой на устройстве, планировщиком или веб-морды,
  доезжают в HA сами (устройство публикует состояние при изменении,
  контрольный репаблиш — раз в 30 секунд).
- Команды из HA сохраняются в настройках устройства: после перезагрузки
  состояние то же.

### Карточка эффектов на дашборде

Кнопки, слайдер и списки появляются в устройстве HEXADDON сами (после
прошивки 0.10.3+). Сущности регистрируются в HA в порядке публикации
discovery, и устройство показывает их так: **Питание аддонов → Подсветка →
Случайный цвет → Эффект → Предыдущий эффект → Следующий эффект →
Скорость**. Чтобы держать их всегда под рукой, добавьте на дашборд карточку
**«Сущности» (Entities)** и включите в неё эти сущности (порядок в ручной
карточке можно перетаскивать как угодно). Цветовое колесо живёт внутри
карточки «Подсветка» (tap → подробнее) — в HA нет отдельной сущности
«цвет», колесо бывает только у света. Все сущности синхронизируются с
любым источником: кнопка на устройстве, веб-морда, планировщик, голос —
максимум через секунду (или сразу после 30-секундного контрольного
репаблиша).

> **Порядок «слипся» после обновления прошивки?** Порядок сущностей в HA
> запоминается при первой регистрации устройства и не меняется от
> повторных discovery. Если устройство уже было добавлено раньше и порядок
> не тот: Настройки → Устройства и службы → HEXADDON → **Удалить** (само
> устройство, не интеграцию MQTT), затем Параметры системы → Перезагрузить
> → **Перезагрузить интеграцию MQTT** (или перезапустить HA) — устройство
> вернётся в правильном порядке. Настройки и автоматизации HA, ссылающиеся
> на сущности, лучше проверить после этого.

---

## Шаг 4. Алиса

Нужен кастомный компонент **Yandex Smart Home** (репозиторий
`dext0r/yandex_smart_home`, требует HA **2025.12+**). Он бриджит устройства HA
в умный дом Яндекса.

### Установка компонента

**Через HACS** (если установлен): HACS → поиск «Yandex Smart Home» → Download,
перезагрузить HA.

**Без HACS** (как у автора — работает одинаково):

```bash
cd <каталог конфига HA>/custom_components
mkdir yandex_smart_home
curl -sfL https://github.com/dext0r/yandex_smart_home/releases/latest/download/yandex_smart_home.zip -o /tmp/ysh.zip
unzip /tmp/ysh.zip -d yandex_smart_home
# перезапустить HA
```

### Подключение к Яндексу

1. **Settings → Devices & Services → Add Integration → Yandex Smart Home**.
2. Тип подключения: **«Облачное Yaha Cloud»** — самый простой путь, не нужен
   домен, белый IP и сертификаты. (Для параноиков есть «Прямое подключение» —
   тогда HA должен быть доступен из интернета по HTTPS; см. документацию
   компонента: <https://docs.yaha-cloud.ru>.)
3. Интеграция выдаст **код привязки**.
4. В телефоне: приложение **«Дом с Алисой»** (или Яндекс → Умный дом) →
   добавить устройства / навыки → навык **Home Assistant** → привязать →
   ввести код.
5. «Подсветка» и «Питание аддонов» появятся в списке устройств Яндекса.

### Команды

- «Алиса, включи/выключи подсветку»
- «Алиса, сделай подсветку ярче / притуши / на 50 процентов»
- «Алиса, включи питание аддонов»

Эффекты («включи режим огонь» и т.п.) прокидываются в Яндекс как «режимы» —
настраивается в параметрах устройства в интеграции Yandex Smart Home
(см. «Режимы/функции» в документации компонента).

> Бонус: тот же компонент умеет умный дом VK (Маруся) — включается там же.

---

## Диагностика

**Устройство не подключается (Состояние: «нет связи»):**
- Проверьте хост/порт/логин/пароль — опечатка в логине даёт именно «нет связи».
- В мониторе порта (115200) строки `[mqtt] connected to ...` / `[mqtt] disconnected, reason=N`
  говорят всё. Классические причины отказа: `reason=4/5` — неверный логин/пароль.

**HA не показывает устройство:**
- MQTT-интеграция зелёная? В настройках интеграции MQTT есть «Listen to a topic» —
  подпишитесь на `#` и перезапустите питание устройства: должны прийти
  discovery-сообщения в `homeassistant/...`.
- Переименовали устройство в HEXADDON? Топики и сущности строятся от имени —
  в HA появится «новое» устройство.

**Ручная проверка команд без HA** (нужен `mosquitto-clients` на любой машине):

```bash
mosquitto_sub -h <брокер> -u hexaddon -P <пароль> -t '#' -v   # смотреть всё
mosquitto_pub -h <брокер> -u hexaddon -P <пароль> \
  -t 'hexaddon/light/set' -m '{"state":"ON","brightness":200}'
mosquitto_pub -h <брокер> -u hexaddon -P <пароль> \
  -t 'hexaddon/addon/set' -m 'ON'
mosquitto_pub -h <брокер> -u hexaddon -P <пароль> \
  -t 'hexaddon/effect/set' -m 'Огонь'
mosquitto_pub -h <брокер> -u hexaddon -P <пароль> \
  -t 'hexaddon/effect/next' -m 'next'
mosquitto_pub -h <брокер> -u hexaddon -P <пароль> \
  -t 'hexaddon/effect/prev' -m 'prev'
mosquitto_pub -h <брокер> -u hexaddon -P <пароль> \
  -t 'hexaddon/speed/set' -m '80'
mosquitto_pub -h <брокер> -u hexaddon -P <пароль> \
  -t 'hexaddon/color_random/set' -m 'ON'
mosquitto_pub -h <брокер> -u hexaddon -P <пароль> \
  -t 'hexaddon/light/set' -m '{"color":{"r":0,"g":255,"b":60}}'
```

(замените `hexaddon` на имя вашего устройства, если меняли). Опечатка в
имени эффекта или мусор вместо числа в скорости игнорируются: в лог
устройства падает `[mqtt] unknown effect: ...` / `[mqtt] bad speed
payload: ...`, а сущности в HA возвращаются к настоящему состоянию.
