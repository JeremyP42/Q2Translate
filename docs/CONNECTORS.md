# Connectors

[README](../README.md) · [Architecture](ARCHITECTURE.md) · [Interface](INTERFACE.md) · [Русская версия](CONNECTORS.ru.md)

A connector is a folder with a `manifest.json` and an implementation: a sandboxed `service.js` script or a native provider compiled into the application. This guide covers the full manifest schema, the JavaScript sandbox API, native providers, and a complete example.

## Layout

```text
services/<type>/<id>/          shipped connectors (repo: data/services/, copied next to the exe)
  manifest.json                required
  service.js                   required for engine=javascript
  icon.png                     shown in the UI (defaults to icon.png)
  prompts.json                 optional, LLM prompt templates
  dictionaries/                optional, spelling/OCR data
Data/services/<id>/            user copy: settings.json and overrides
```

A user connector with the same `id` as a shipped one overrides it completely. User settings are stored in `Data/services/<id>/settings.json` and never touch the shipped folder.

## manifest.json

| Field | Type | Description |
|---|---|---|
| `id` | string | Unique connector id; defaults to the folder name. Used in settings, order, and CLI probes. |
| `name` | string | Display name. |
| `version` | string | Connector version, shown on the card. |
| `type` | string | Group: `translate`, `dictionaries`, `ocr`, `spelling`, `tts`, `llm`. |
| `engine` | string | `javascript` or `native`. |
| `capabilities` | string[] | What the connector can do: `translate`, `dictionary`, `ocr`, `spelling`, `tts`, `llm`, `summarize`, `paraphrase`, `ocr_vision`. |
| `order` | int | Position in the service panel (default 100). |
| `icon` | string | Icon file inside the connector folder (default `icon.png`). |
| `description` | string | Shown on the connector card. |
| `link` | string | Optional site link on the card. |
| `service_script` | string | Script file name (default `service.js`). |
| `settings_schema` | array | Editable fields, see below. |
| `native_provider` | string | For `engine=native`: implementation selector, see below. |
| `native_executable` | string | For `engine=native`: path to an external executable. |
| `tessdata_path` | string | Tesseract data folder (Tesseract connector). |
| `enabled_by_default` | bool | Must agree with `DisabledServices` in `settings-default.json`. |

### settings_schema fields

Each entry describes one editable field rendered on the connector card:

| Field | Description |
|---|---|
| `key` | Setting key passed to the script in the global `settings` object. |
| `label` | Field caption. |
| `type` | `string`, `password`, `number`, `boolean`, `select`, `models`, `xdxf_folders`, `tessdata_languages`. |
| `default` | Initial value. |
| `required` | Marks the field as required (e.g. an API key). |
| `options` | Value/label pairs for `type=select`. |
| `endpoint_key` | For `type=models`: settings key holding the endpoint used to list models. |

`type=models` fetches the model list from the connector endpoint; `type=xdxf_folders` lists subfolders of the connector dictionary folder (Local XDXF); `type=tessdata_languages` lists installed Tesseract languages.

## The JavaScript sandbox

Every call runs in a **fresh `QJSEngine`**: no state survives between executions except the explicit cache (`cacheGet`/`cacheSet`). The script file is evaluated, then `execute(capability, params)` is called.

Globals available in `service.js`:

| Global | Description |
|---|---|
| `http` | HTTP bridge: `get`, `post`, `multipart_post` — see below. |
| `qtranslate` | Helper bridge: `loadJSON`, `md5`, `aesDecryptBase64`, `cacheGet`, `cacheSet`. |
| `settings` | Connector settings as an object (`settings.api_key`, `settings.endpoint`, …). |
| `json` / `JSON` | `JSON.parse` / `JSON.stringify`. |
| `loadJSON(name)` | Reads a JSON file from the connector folder; a user copy in `Data/services/<id>/` overrides individual keys. |
| `console` | `console.log` for debugging (goes to debug output). |

## execute(capability, params)

Return an object, or a plain string (treated as `{text: ...}`). An empty result is reported as an error.

Result fields recognized by the application:

| Field | Used by | Description |
|---|---|---|
| `text` | translate, dictionary, LLM | Plain-text result. |
| `html` | dictionary | Formatted result (definitions, examples, links). |
| `detected_lang` | translate | Source language detected by the service, shown when detection mode is "service". |
| `error` | all | Error message; a non-empty value marks the call as failed. |
| `correct`, `suggestions` | spelling (native) | Spell-check verdict and up to 7 suggestions. |
| `ok` | tts (native) | Playback success flag. |

Params by capability:

| Capability | Params |
|---|---|
| `translate` | `text`, `from`, `to`, `detected_from` (locally detected source), `from_name`, `to_name`, `source_language_name` |
| `dictionary` | `word`, `text`, `from`, `to` |
| `ocr` | `image_base64` (PNG, base64), `lang`, `language` |
| `summarize`, `paraphrase` | `text`, `to` |
| `ocr_vision` | `image_base64` — LLM vision models extract text from an image |
| `spelling` | `word`/`text`, `language`, `action=check` (native providers) |
| `tts` | `text`, `lang` (native providers) |

## http bridge

```js
http.get(url, headers?)
http.post(url, body, headers?)
http.multipart_post(url, fields, headers?)
```

- `body`: an object or array is serialized as JSON with `Content-Type: application/json`; a string is sent as-is.
- `fields` for `multipart_post`: form fields; values may be strings or objects describing file parts.
- Every call returns `{status, body, body_base64, content_type, error}`:
  - `status` — HTTP status code, `0` on network failure;
  - `body` — response text (UTF-8);
  - `body_base64` — the same bytes for binary payloads;
  - `content_type` — response `Content-Type`;
  - `error` — network error text, empty on success.

Networking rules:

- requests go only through this bridge — scripts have no direct filesystem or socket access;
- TLS verification is never disabled;
- cookies are kept in a process-wide jar and survive across calls, which keeps site sessions alive;
- timeouts, retries, and the proxy come from **Settings → Network** (`Internet.*`, `Proxy.*`) and apply to every connector.

## qtranslate bridge

| Method | Description |
|---|---|
| `loadJSON(name)` | Reads `<connector>/<name>` and merges overrides from `Data/services/<id>/<name>`. Used for `prompts.json` and connector config. |
| `md5(value)` | MD5 hex digest (many translate APIs sign requests with it). |
| `aesDecryptBase64(payload, key, iv)` | AES decryption for base64 payloads (Windows CNG; key and IV are MD5-derived). Used by several online services. |
| `cacheGet(key)` / `cacheSet(key, object)` | In-memory cache shared by all executions of this connector — store short-lived tokens and keys here instead of globals. |

## Native providers

`engine=native` connectors select an implementation with `native_provider`:

| Value | Implementation | Notes |
|---|---|---|
| `hunspell` | `HunspellProvider` | Built-in `.aff`/`.dic` engine; downloads dictionaries on demand. |
| `nuspell` | `NuspellProvider` | Nuspell 5.1.9 + ICU 76; same dictionary folder. Full build only — the small build defaults to Hunspell. |
| `windows_spell` | `WindowsSpellProvider` | Windows Spell Checking API. |
| `windows_ocr` | `WindowsOcrProvider` | `Windows.Media.Ocr`. |
| `tesseract` | `TesseractProvider` | Bundled in the full build; the small build installs it as an engine package. Language data in `tessdata`. |
| `paddleocr` | `PaddleOcrProvider` | PP-OCRv5 models run through `onnxruntime.dll` (no Python); bundled in the full build, installed on demand in the small build. Best quality on pages with artwork and photo screenshots. |
| `tts` | `TtsProvider` | Shared HTTP-based TTS implementation for `bing_tts`, `google_tts`, `yandex_tts`; `windows_sapi` uses SAPI. |
| `xdxf` | `XdxfProvider` | Local XDXF dictionaries; `folder` setting selects a subfolder. |

Tesseract, PaddleOCR and Nuspell are bundled in the full build only. The small build removes those connector directories and defaults to Windows OCR and Hunspell. Engine packages installed from the connector card are placed in `Data/services` and override the bundled copies.

## prompts.json (LLM)

LLM connectors keep prompts in `prompts.json`; keys match capabilities (`translate`, `summarize`, `paraphrase`, `dictionary`, `ocr_vision`). Placeholders `{source}`, `{target}`, `{text}` are substituted before the request:

```json
{
  "translate": "Translate the following text from {source} to {target}. Return ONLY the translation.\n\n{text}",
  "summarize": "Summarize the following text in 3 concise bullet points.\n\n{text}"
}
```

## Complete example

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

Drop the folder into `Data/services/mymemory/` (or the shipped tree), restart the application, and enable the connector in **Settings → Connectors**.

## Testing

```bash
./work/"Q2Translate.exe" --probe-connectors mymemory
```

The probe writes one line per capability to `Data/connector_probe.log` (`<id> <capability> OK/FAIL: ...`) with a 12-second per-connector timeout. `--selftest` additionally checks translation, OCR, and XDXF end to end; `--tts-test <id>` and `--http-test <url>` verify speech and connectivity.

## Licensing note

Connector icons are the favicons or brand marks of their services and are not covered by the project's MIT license — see `data/services/ICONS.md` before redistributing.
