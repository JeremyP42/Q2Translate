function execute(capability, params) {
    if (capability !== "ocr" && capability !== "ocr_vision") {
        return { error: "OCR.space: неподдерживаемая возможность " + capability };
    }
    if (!settings.api_key) {
        return { error: "OCR.space: укажите API Key в настройках сервиса" };
    }
    if (!params.image_base64) {
        return { error: "OCR.space: не передано изображение" };
    }

    var endpoint = settings.endpoint || "https://api.ocr.space/parse/image";
    var language = params.lang || settings.language || "eng";
    var fields = {
        base64Image: {
            type: "file",
            filename: "image.png",
            contentType: "image/png",
            base64: params.image_base64
        },
        language: language,
        OCREngine: settings.engine || "2",
        scale: "true",
        isOverlayRequired: "false"
    };

    var response = http.multipart_post(endpoint, fields, {
        "apikey": settings.api_key
    });
    if (response.error) {
        return { error: response.error };
    }

    var data;
    try {
        data = json.parse(response.body);
    } catch (error) {
        return { error: "OCR.space: некорректный ответ сервера" };
    }

    if (data.IsErroredOnProcessing) {
        var message = data.ErrorMessage;
        if (message instanceof Array) {
            message = message.join("; ");
        }
        return { error: message || "OCR.space: ошибка распознавания" };
    }

    if (!data.ParsedResults || !data.ParsedResults.length) {
        return { error: "OCR.space: текст не распознан" };
    }

    var text = "";
    for (var index = 0; index < data.ParsedResults.length; ++index) {
        if (data.ParsedResults[index].ParsedText) {
            text += data.ParsedResults[index].ParsedText;
        }
    }

    return { text: text, detected_lang: language };
}
