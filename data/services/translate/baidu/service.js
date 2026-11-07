/*
 * Baidu Translate connector using the official key-based API.
 *
 * The old web connector depended on fanyi.baidu.com cookies, gtk and a token
 * that the site no longer exposes.  The official endpoint stays stable and
 * only needs an AppID/key pair.
 */

var ENDPOINT = "https://fanyi-api.baidu.com/api/trans/vip/translate";

function baiduLanguage(value) {
    var code = String(value == null ? "" : value).trim();
    if (!code || code === "auto")
        return "auto";
    if (code === "zh-CN" || code === "zh-TW" || code === "zh")
        return "zh";
    return code.split("-")[0];
}

function baiduError(code) {
    var messages = {
        "52001": "таймаут запроса",
        "52002": "ошибка на стороне сервиса",
        "52003": "неверный AppID",
        "54000": "не указан обязательный параметр",
        "54001": "неверная подпись (проверьте ключ)",
        "54003": "превышен лимит запросов",
        "54004": "недостаточно средств на аккаунте",
        "54005": "слишком длинный текст",
        "58000": "IP не разрешён в настройках приложения",
        "58001": "неподдерживаемая языковая пара",
        "58002": "сервис временно выключен"
    };
    return messages[String(code)] || ("ошибка API " + code);
}

function execute(capability, params) {
    if (capability !== "translate")
        return { error: "Baidu: поддерживается только перевод." };

    var appId = String(settings.app_id || "").trim();
    var appKey = String(settings.app_key || "").trim();
    if (!appId || !appKey)
        return { error: "Baidu: укажите AppID и ключ в настройках коннектора." };

    var text = String(params.text || params.word || "").trim();
    if (!text)
        return { error: "Baidu: не задан текст для перевода." };

    var salt = String(new Date().getTime());
    var body = "q=" + encodeURIComponent(text)
        + "&from=" + encodeURIComponent(baiduLanguage(params.from))
        + "&to=" + encodeURIComponent(baiduLanguage(params.to) || "ru")
        + "&appid=" + encodeURIComponent(appId)
        + "&salt=" + encodeURIComponent(salt)
        + "&sign=" + encodeURIComponent(qtranslate.md5(appId + text + salt + appKey));

    var response = http.post(ENDPOINT, body, {
        "Content-Type": "application/x-www-form-urlencoded",
        "Accept": "application/json"
    });
    if (response.error)
        return { error: "Baidu: " + response.error };

    var data;
    try {
        data = json.parse(response.body);
    } catch (error) {
        return { error: "Baidu: некорректный ответ сервиса." };
    }

    if (!data || data.error_code)
        return { error: "Baidu: " + baiduError(data && data.error_code) };

    var rows = data.trans_result || [];
    var translated = [];
    for (var i = 0; i < rows.length; ++i) {
        if (rows[i] && rows[i].dst)
            translated.push(rows[i].dst);
    }
    if (!translated.length)
        return { error: "Baidu: пустой ответ." };

    var answer = { text: translated.join("\n") };
    if (data.from)
        answer.source_language = baiduLanguage(data.from);
    return answer;
}
