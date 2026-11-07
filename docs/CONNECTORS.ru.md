# Коннекторы

[README](../README_rus.md) · [Архитектура](ARCHITECTURE.ru.md) · [Интерфейс](INTERFACE.ru.md) · [English](CONNECTORS.md)

Коннектор — это папка с `manifest.json` и реализацией: изолированный скрипт `service.js` или нативный провайдер, встроенный в приложение. Руководство описывает полную схему манифеста, API JavaScript-песочницы, нативные провайдеры и законченный пример.

## Структура

```text
services/<тип>/<id>/           поставляемые коннекторы (в репозитории: data/services/, копируются рядом с exe)
  manifest.json                обязательный
  service.js                   обязателен для engine=javascript
  icon.png                     иконка в интерфейсе (по умолчанию icon.png)
  prompts.json                 необязательный, шаблоны промптов LLM
  dictionaries/                необязательная папка данных орфографии/OCR
Data/services/<id>/            пользовательская копия: settings.json и переопределения
```

Пользовательский коннектор с тем же `id`, что и поставляемый, полностью его переопределяет. Настройки пользователя пишутся в `Data/services/<id>/settings.json` и не трогают исходную папку.

## manifest.json

| Поле | Тип | Описание |
|---|---|---|
| `id` | string | Уникальный id коннектора; по умолчанию — имя папки. Используется в настройках, порядке и проверках CLI. |
| `name` | string | Отображаемое имя. |
| `version` | string | Версия коннектора на карточке. |
| `type` | string | Группа: `translate`, `dictionaries`, `ocr`, `spelling`, `tts`, `llm`. |
| `engine` | string | `javascript` или `native`. |
| `capabilities` | string[] | Возможности: `translate`, `dictionary`, `ocr`, `spelling`, `tts`, `llm`, `summarize`, `paraphrase`, `ocr_vision`. |
| `order` | int | Позиция в панели сервисов (по умолчанию 100). |
| `icon` | string | Файл иконки в папке коннектора (по умолчанию `icon.png`). |
| `description` | string | Описание на карточке коннектора. |
| `link` | string | Необязательная ссылка на сайт. |
| `service_script` | string | Имя скрипта (по умолчанию `service.js`). |
| `settings_schema` | array | Редактируемые поля, см. ниже. |
| `native_provider` | string | Для `engine=native`: выбор реализации, см. ниже. |
| `native_executable` | string | Для `engine=native`: путь к внешнему исполняемому файлу. |
| `tessdata_path` | string | Папка данных Tesseract (коннектор Tesseract). |
| `enabled_by_default` | bool | Должно согласовываться с `DisabledServices` из `settings-default.json`. |

### Поля settings_schema

Каждая запись описывает одно редактируемое поле на карточке коннектора:

| Поле | Описание |
|---|---|
| `key` | Ключ настройки, доступный скрипту в глобальном объекте `settings`. |
| `label` | Подпись поля. |
| `type` | `string`, `password`, `number`, `boolean`, `select`, `models`, `xdxf_folders`, `tessdata_languages`. |
| `default` | Начальное значение. |
| `required` | Помечает поле как обязательное (например, API-ключ). |
| `options` | Пары значение/подпись для `type=select`. |
| `endpoint_key` | Для `type=models`: ключ настройки с endpoint для загрузки списка моделей. |

`type=models` загружает список моделей из endpoint коннектора; `type=xdxf_folders` выводит подпапки папки словарей (Local XDXF); `type=tessdata_languages` показывает установленные языки Tesseract.

## JavaScript-песочница

Каждый вызов выполняется в **новом `QJSEngine`**: между запусками ничего не сохраняется, кроме явного кэша (`cacheGet`/`cacheSet`). Скрипт вычисляется, затем вызывается `execute(capability, params)`.

Глобальные объекты в `service.js`:

| Глобал | Описание |
|---|---|
| `http` | HTTP-мост: `get`, `post`, `multipart_post` — см. ниже. |
| `qtranslate` | Вспомогательный мост: `loadJSON`, `md5`, `aesDecryptBase64`, `cacheGet`, `cacheSet`. |
| `settings` | Настройки коннектора как объект (`settings.api_key`, `settings.endpoint`, …). |
| `json` / `JSON` | `JSON.parse` / `JSON.stringify`. |
| `loadJSON(name)` | Читает JSON-файл из папки коннектора; пользовательская копия в `Data/services/<id>/` переопределяет отдельные ключи. |
| `console` | `console.log` для отладки. |

## execute(capability, params)

Верните объект или обычную строку (она превращается в `{text: ...}`). Пустой результат считается ошибкой.

Поля результата, которые понимает приложение:

| Поле | Используется | Описание |
|---|---|---|
| `text` | translate, dictionary, LLM | Результат в виде простого текста. |
| `html` | dictionary | Форматированный результат (определения, примеры, ссылки). |
| `detected_lang` | translate | Исходный язык, определённый сервисом; показывается в режиме определения «онлайн-сервисом». |
| `error` | все | Сообщение об ошибке; непустое значение помечает вызов как неудачный. |
| `correct`, `suggestions` | spelling (нативный) | Результат проверки и до 7 вариантов исправления. |
| `ok` | tts (нативный) | Флаг успешного воспроизведения. |

Параметры по возможностям:

| Возможность | Параметры |
|---|---|
| `translate` | `text`, `from`, `to`, `detected_from` (локально определённый язык), `from_name`, `to_name`, `source_language_name` |
| `dictionary` | `word`, `text`, `from`, `to` |
| `ocr` | `image_base64` (PNG, base64), `lang`, `language` |
| `summarize`, `paraphrase` | `text`, `to` |
| `ocr_vision` | `image_base64` — LLM с функцией зрения извлекают текст из изображения |
| `spelling` | `word`/`text`, `language`, `action=check` (нативные провайдеры) |
| `tts` | `text`, `lang` (нативные провайдеры) |

## HTTP-мост

```js
http.get(url, headers?)
http.post(url, body, headers?)
http.multipart_post(url, fields, headers?)
```

- `body`: объект или массив сериализуются в JSON с `Content-Type: application/json`; строка отправляется как есть.
- `fields` для `multipart_post`: поля формы; значения могут быть строками или объектами с файловыми частями.
- Каждый вызов возвращает `{status, body, body_base64, content_type, error}`:
  - `status` — HTTP-код, `0` при сетевой ошибке;
  - `body` — текст ответа (UTF-8);
  - `body_base64` — те же байты для бинарных ответов;
  - `content_type` — `Content-Type` ответа;
  - `error` — текст сетевой ошибки, пустой при успехе.

Правила сети:

- запросы идут только через этот мост — скрипты не имеют прямого доступа к файлам и сокетам;
- проверка TLS никогда не отключается;
- cookies хранятся в общем для процесса jar и переживают вызовы, сохраняя сессии сайтов;
- таймауты, повторы и прокси берутся из **Настройки → Сеть** (`Internet.*`, `Proxy.*`) и действуют для всех коннекторов.

## Мост qtranslate

| Метод | Описание |
|---|---|
| `loadJSON(name)` | Читает `<коннектор>/<name>` и добавляет переопределения из `Data/services/<id>/<name>`. Используется для `prompts.json` и конфигурации коннектора. |
| `md5(value)` | MD5-хэкс-дайджест (многие API перевода подписывают запросы им). |
| `aesDecryptBase64(payload, key, iv)` | AES-расшифровка base64-ответов (Windows CNG; ключ и IV — производные MD5). Используется несколькими онлайн-сервисами. |
| `cacheGet(key)` / `cacheSet(key, object)` | Кэш в памяти, общий для всех запусков коннектора, — храните здесь короткоживущие токены и ключи вместо глобальных переменных. |

## Нативные провайдеры

Коннекторы с `engine=native` выбирают реализацию через `native_provider`:

| Значение | Реализация | Особенности |
|---|---|---|
| `hunspell` | `HunspellProvider` | Встроенный движок `.aff`/`.dic`; словари скачиваются по запросу. |
| `nuspell` | `NuspellProvider` | Nuspell 5.1.9 + ICU 76; та же папка словарей. Только в полной сборке — в урезанной по умолчанию Hunspell. |
| `windows_spell` | `WindowsSpellProvider` | Windows Spell Checking API. |
| `windows_ocr` | `WindowsOcrProvider` | `Windows.Media.Ocr`. |
| `tesseract` | `TesseractProvider` | Встроен в полную сборку; в урезанной устанавливается как пакет движка. Языковые данные в `tessdata`. |
| `paddleocr` | `PaddleOcrProvider` | Модели PP-OCRv5 через `onnxruntime.dll` (без Python); в полной сборке встроены, в урезанной ставятся по запросу. Лучшее качество на страницах с рисунком и фото-скриншотах. |
| `tts` | `TtsProvider` | Общая HTTP-реализация TTS для `bing_tts`, `google_tts`, `yandex_tts`; `windows_sapi` использует SAPI. |
| `xdxf` | `XdxfProvider` | Локальные XDXF-словари; настройка `folder` выбирает подпапку. |

Tesseract, PaddleOCR и Nuspell встроены только в полную сборку. Урезанная сборка удаляет каталоги этих коннекторов и по умолчанию использует Windows OCR и Hunspell. Пакеты движков, установленные из карточки коннектора, кладутся в `Data/services` и имеют приоритет над встроенными.

## prompts.json (LLM)

LLM-коннекторы хранят промпты в `prompts.json`; ключи соответствуют возможностям (`translate`, `summarize`, `paraphrase`, `dictionary`, `ocr_vision`). Плейсхолдеры `{source}`, `{target}`, `{text}` подставляются перед запросом:

```json
{
  "translate": "Translate the following text from {source} to {target}. Return ONLY the translation.\n\n{text}",
  "summarize": "Summarize the following text in 3 concise bullet points.\n\n{text}"
}
```

## Законченный пример

`manifest.json`:

```json
{
  "id": "mymemory",
  "name": "MyMemory",
  "version": "1.0.0",
  "type": "translate",
  "engine": "javascript",
  "order": 20,
  "icon": "icon.png",
  "description": "Free translation via MyMemory with limits per day.",
  "capabilities": ["translate"],
  "settings_schema": [
    { "key": "email", "type": "string", "label": "Email (optional)", "default": "" },
    { "key": "endpoint", "type": "string", "label": "Endpoint",
      "default": "https://api.mymemory.translated.net/get" }
  ]
}
```

`service.js`:

```js
function execute(capability, params) {
    if (capability !== "translate")
        return { error: "MyMemory: unsupported capability " + capability };

    var text = params.text || "";
    if (!text)
        return { error: "Empty text" };

    var from = params.detected_from || params.from || "en";
    if (from === "auto") from = "en";
    var to = params.to || "ru";
    if (to === "auto") to = "ru";

    var url = (settings.endpoint || "https://api.mymemory.translated.net/get") +
        "?q=" + encodeURIComponent(text) +
        "&langpair=" + encodeURIComponent(from + "|" + to);

    var response = http.get(url, { "Accept": "application/json" });
    if (response.error)
        return { error: response.error };
    if (response.status !== 200)
        return { error: "MyMemory: HTTP " + response.status };

    var data = json.parse(response.body);
    if (!data.responseData || !data.responseData.translatedText)
        return { error: "MyMemory: unexpected response" };
    return { text: data.responseData.translatedText };
}
```

Скопируйте папку в `Data/services/mymemory/` (или в поставляемое дерево), перезапустите приложение и включите коннектор в **Настройки → Коннекторы**.

## Проверка

```bash
./work/"Q2Translate.exe" --probe-connectors mymemory
```

Проверка пишет по строке на возможность в `Data/connector_probe.log` (`<id> <capability> OK/FAIL: ...`) с таймаутом 12 секунд на коннектор. `--selftest` дополнительно сквозно проверяет перевод, OCR и XDXF; `--tts-test <id>` и `--http-test <url>` проверяют речь и доступность сети.

## Лицензия

Иконки коннекторов — фавиконки или бренды соответствующих сервисов и не входят в лицензию MIT проекта — см. `data/services/ICONS.md` перед распространением.
