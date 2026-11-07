/*
 * Youdao connector.
 *
 * The web translator signs the key request with MD5, then encrypts the
 * translation response with AES-CBC.  The shared HTTP bridge keeps the
 * session cookie and the native bridge exposes the AES step.
 */

var PAGE_URL = "https://fanyi.youdao.com/";
var API_HOST = "https://dict.youdao.com";
var KEY_SEED = "yU5nT5dK3eZ1pI4j";
var KEY_ID = "webfanyi-key-getter-2025";
var TRANSLATE_KEY_ID = "webfanyi";
var CLIENT = "fanyideskweb";
var PRODUCT = "webfanyi";
var APP_VERSION = "12.0.0";
var VENDOR = "web";
var KEYFROM = "fanyi.web";
var POINT_PARAM = "client,mysticTime,product";

var USER_AGENT = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    + "(KHTML, like Gecko) Chrome/135.0.0.0 Safari/537.36";

function youdaoLanguage(value) {
    var code = String(value == null ? "" : value).trim();
    if (!code || code === "auto")
        return "auto";
    if (code === "zh-CN" || code === "zh-TW" || code === "zh")
        return "zh-CHS";
    return code.split("-")[0];
}

function nowMillis() {
    return String(new Date().getTime());
}

function signFor(secret, time) {
    return qtranslate.md5("client=" + CLIENT + "&mysticTime=" + time
                          + "&product=" + PRODUCT + "&key=" + secret);
}

function commonHeaders() {
    return {
        "Accept": "application/json, text/plain, */*",
        "Accept-Language": "ru-RU,ru;q=0.9,en-US;q=0.8,en;q=0.7",
        "Origin": "https://fanyi.youdao.com",
        "Referer": PAGE_URL,
        "User-Agent": USER_AGENT
    };
}

function keyUrl(time, sign) {
    return API_HOST + "/webtranslate/key"
        + "?keyid=" + encodeURIComponent(KEY_ID)
        + "&client=" + encodeURIComponent(CLIENT)
        + "&product=" + encodeURIComponent(PRODUCT)
        + "&appVersion=" + encodeURIComponent(APP_VERSION)
        + "&vendor=" + encodeURIComponent(VENDOR)
        + "&pointParam=" + encodeURIComponent(POINT_PARAM)
        + "&mysticTime=" + encodeURIComponent(time)
        + "&keyfrom=" + encodeURIComponent(KEYFROM)
        + "&mid=1&screen=1&model=1&network=wifi&abtest=0&yduuid=abcdefg"
        + "&sign=" + encodeURIComponent(sign);
}

function requestKey() {
    var time = nowMillis();
    var response = http.get(keyUrl(time, signFor(KEY_SEED, time)), commonHeaders());
    if (response.error)
        return { error: "Youdao: " + response.error };

    var data;
    try {
        data = json.parse(response.body);
    } catch (error) {
        return { error: "Youdao: некорректный ответ ключевого сервиса." };
    }
    if (!data || data.code !== 0 || !data.data || !data.data.secretKey)
        return { error: (data && data.msg) || "Youdao: не удалось получить ключ перевода." };
    return data.data;
}

function translate(capability, params) {
    if (capability !== "translate")
        return { error: "Youdao: поддерживается только перевод." };

    var text = String(params.text || params.word || "").trim();
    if (!text)
        return { error: "Youdao: не задан текст для перевода." };

    // Reuse the short-lived key and the process-wide session cookie.  A fresh
    // page request is only needed when the cached key is missing or expired.
    var key = qtranslate.cacheGet("youdao_key");
    if (!key || Number(key.expires) <= new Date().getTime() || !key.secretKey) {
        http.get(PAGE_URL, {
            "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
            "User-Agent": USER_AGENT
        });

        key = requestKey();
        if (key.error)
            return key;
        key.expires = new Date().getTime() + 5 * 60 * 1000;
        qtranslate.cacheSet("youdao_key", key);
    }

    var time = nowMillis();
    var from = youdaoLanguage(params.from);
    var to = youdaoLanguage(params.to) || "ru";
    var body = "i=" + encodeURIComponent(text)
        + "&from=" + encodeURIComponent(from)
        + "&to=" + encodeURIComponent(to)
        + "&useTerm=false&dictResult=true"
        + "&keyid=" + encodeURIComponent(TRANSLATE_KEY_ID)
        + "&noCheckPrivate=false"
        + "&client=" + encodeURIComponent(CLIENT)
        + "&product=" + encodeURIComponent(PRODUCT)
        + "&appVersion=" + encodeURIComponent(APP_VERSION)
        + "&vendor=" + encodeURIComponent(VENDOR)
        + "&pointParam=" + encodeURIComponent(POINT_PARAM)
        + "&mysticTime=" + encodeURIComponent(time)
        + "&keyfrom=" + encodeURIComponent(KEYFROM)
        + "&mid=1&screen=1&model=1&network=wifi&abtest=0&yduuid=abcdefg"
        + "&sign=" + encodeURIComponent(signFor(key.secretKey, time));

    var headers = commonHeaders();
    headers["Content-Type"] = "application/x-www-form-urlencoded; charset=utf-8";
    var response = http.post(API_HOST + "/webtranslate", body, headers);
    if (response.error)
        return { error: "Youdao: " + response.error };

    var decrypted = qtranslate.aesDecryptBase64(String(response.body || "").trim(),
                                                String(key.aesKey || ""),
                                                String(key.aesIv || ""));
    if (!decrypted)
        return { error: "Youdao: не удалось расшифровать ответ." };

    var result;
    try {
        result = json.parse(decrypted);
    } catch (error) {
        return { error: "Youdao: некорректный ответ перевода." };
    }

    if (!result || result.code !== 0)
        return { error: (result && result.msg) || ("Youdao: HTTP " + response.status) };

    var translated = "";
    var rows = result.translateResult || [];
    for (var i = 0; i < rows.length; ++i) {
        var row = rows[i];
        if (!row)
            continue;
        if (typeof row.length === "number") {
            for (var j = 0; j < row.length; ++j) {
                if (row[j] && row[j].tgt)
                    translated += row[j].tgt;
            }
        } else if (row.tgt) {
            translated += row.tgt;
        }
    }

    if (!translated)
        return { error: "Youdao: пустой ответ." };

    var answer = { text: translated };
    var type = String(result.type || "");
    if (type.indexOf("2") > 0)
        answer.source_language = type.split("2")[0];
    return answer;
}

function execute(capability, params) {
    return translate(capability, params);
}
